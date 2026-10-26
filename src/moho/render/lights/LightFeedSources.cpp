#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "gpg/core/utils/Logging.h"
#include "moho/misc/FileWaitHandleSet.h"
#include "moho/particles/SParticleBuffer.h"
#include "moho/particles/SWorldBeam.h"
#include "moho/particles/SWorldLight.h"
#include "moho/particles/SWorldParticle.h"
#include "moho/particles/SWorldTrail.h"
#include "moho/render/lights/LightFeeds.h"
#include "moho/render/lights/LightTexture.h"
#include "moho/render/lights/LightTextureCache.h"
#include "moho/resource/CParticleTexture.h"

// The feeds of the light pool that read the effect records of the sim (docs/DYNAMIC_LIGHTS.md, "Sources of light"): what each effect
// draws is measured, not declared, so a modded effect lights up the same way a stock one does. Colour comes from the effect textures
// (the DDS files are read here, see LightTexture.h): the hue of the texture times the ramp at the particle age. Render side only.
namespace moho::lights
{
  namespace
  {
    constexpr float kHoldTicks = 3.0f;       // a tracked light fades out this long after its last refresh
    constexpr float kMergeCell = 4.0f;       // glow sprites inside one cell of this size are one light
    constexpr float kGlowPeakMin = 0.30f;    // a texture whose brightest alpha x luminance is below this does not glow (smoke, dust)
    constexpr float kGlowEnergyCap = 0.9f;

    constexpr std::uint64_t kKeyLua = 1ull << 40;
    constexpr std::uint64_t kKeyGlow = 2ull << 40;
    constexpr std::uint64_t kKeyBeam = 3ull << 40;
    constexpr std::uint64_t kKeyTrail = 4ull << 40;

    std::string PathOf(const CountedPtr_CParticleTexture& handle)
    {
      return handle.tex != nullptr ? std::string(handle.tex->mTexturePath.c_str()) : std::string();
    }

    // Warm white of a light whose texture cannot be read: the colour the first patch used (255, 215, 160).
    constexpr float kFallbackRgb[3] = {1.0f, 0.843f, 0.627f};

    // Fills the colour of `e` from the texture hue and, when there is a ramp, its curve over the life at row `rampV`.
    void FillColour(Emission& e, const TexEntry* texture, const TexEntry* ramp, const float rampV)
    {
      const float* hue = texture != nullptr ? texture->colour.hue : kFallbackRgb;
      e.rgb[0] = hue[0];
      e.rgb[1] = hue[1];
      e.rgb[2] = hue[2];
      if (ramp != nullptr) {
        for (int i = 0; i < 4; ++i) {
          float rgba[4];
          SampleRamp(ramp->image, static_cast<float>(i) / 3.0f, rampV, rgba);
          for (int c = 0; c < 3; ++c) {
            e.curve[i][c] = hue[c] * rgba[c] * rgba[3];
          }
        }
        e.hasCurve = true;
      }
    }

    // Sub-tick emission offset of a particle: the fractional part of mInterop (emission cursor minus tick), as the particle shader uses it.
    float BirthFraction(const float interop)
    {
      if (!std::isfinite(interop) || std::fabs(interop) >= 1.0e7f) {
        return 0.0f;
      }
      const float frac = interop - std::floor(interop);
      return frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    }

    std::uint64_t CellKey(const float x, const float y, const float z, const float cell)
    {
      const auto q = [cell](const float v) { return static_cast<std::int64_t>(std::floor(v / cell)) & 0xFFFFF; };
      return static_cast<std::uint64_t>(q(x)) | (static_cast<std::uint64_t>(q(y)) << 20);
    }

    std::uint64_t Mix(std::uint64_t h, const std::uint64_t v)
    {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      return h;
    }

    // One cell of merged glow sprites.
    struct GlowCell
    {
      float pos[3] = {0.0f, 0.0f, 0.0f};
      float rgb[3] = {0.0f, 0.0f, 0.0f};
      float weight = 0.0f;
      float energy2 = 0.0f;
      float radius = 0.0f;
    };
  } // namespace

  void FeedParticleBuffer(const SParticleBuffer& batch, const int beatTick)
  {
    SyncQuality();
    LightCollector& collector = Collector();
    if (collector.Quality() == model::kOff) {
      return;
    }
    const float now = static_cast<float>(beatTick);

    // ---- particles: explosion light particles ("TLight"), and glow sprites merged per cell
    std::unordered_map<std::uint64_t, GlowCell> glow;
    for (const SWorldParticle& particle : batch.mParticles) {
      const bool isLight = particle.mTypeTag.view() == std::string_view("TLight");
      const int blend = static_cast<int>(particle.mBlendMode);
      if (!isLight && blend != 3 && blend != 4) {
        continue; // alpha, modulate and refraction sprites do not glow
      }
      const float peakSize = std::max(particle.mBeginSize, particle.mEndSize);
      if (!(peakSize > 0.0f) || !std::isfinite(peakSize)) {
        continue;
      }

      const std::string texturePath = PathOf(particle.mTexture);
      const std::string rampPath = PathOf(particle.mRampTexture);
      const TexEntry* texture = LoadTexture(texturePath);
      const TexEntry* ramp = LoadTexture(rampPath);

      if (isLight) {
        Emission e;
        e.kind = Kind::Flash;
        e.pos[0] = particle.mPos.x;
        e.pos[1] = particle.mPos.y;
        e.pos[2] = particle.mPos.z;
        e.dir[0] = particle.mDir.x;
        e.dir[1] = particle.mDir.y;
        e.dir[2] = particle.mDir.z;
        e.accel[0] = particle.mAccel.x;
        e.accel[1] = particle.mAccel.y;
        e.accel[2] = particle.mAccel.z;
        e.birth = now + BirthFraction(particle.mInterop);
        e.life = particle.mLifetime;
        e.size0 = particle.mBeginSize;
        e.size1 = particle.mEndSize;
        FillColour(e, texture, ramp, particle.mRampSelection);
        (void)collector.Add(e);
        continue;
      }

      // a glow sprite: only textures that glow, merged with the others in the same cell
      if (texture == nullptr || texture->colour.peak < kGlowPeakMin) {
        continue;
      }
      const float gain = (blend == 4 ? 0.7f : 1.0f) * std::min(1.0f, texture->colour.peak);
      const float energy = model::GlowEnergy(peakSize) * gain;
      const float radius = model::GlowRadius(peakSize);
      float rgb[3] = {texture->colour.hue[0], texture->colour.hue[1], texture->colour.hue[2]};
      if (ramp != nullptr) {
        float rgba[4];
        SampleRamp(ramp->image, 0.25f, particle.mRampSelection, rgba);
        for (int c = 0; c < 3; ++c) {
          rgb[c] *= rgba[c];
        }
        const float ramped = std::max(rgb[0], std::max(rgb[1], rgb[2]));
        if (ramped < 0.05f) {
          continue; // the ramp is dark where the sprite is born
        }
      }
      GlowCell& cell = glow[CellKey(particle.mPos.x, particle.mPos.y, particle.mPos.z, kMergeCell)];
      const float w = energy;
      cell.pos[0] += particle.mPos.x * w;
      cell.pos[1] += particle.mPos.y * w;
      cell.pos[2] += particle.mPos.z * w;
      cell.rgb[0] += rgb[0] * w;
      cell.rgb[1] += rgb[1] * w;
      cell.rgb[2] += rgb[2] * w;
      cell.weight += w;
      cell.energy2 += energy * energy;
      cell.radius = std::max(cell.radius, radius);
    }
    for (const auto& item : glow) {
      const GlowCell& c = item.second;
      if (c.weight <= 0.0f) {
        continue;
      }
      Emission e;
      e.kind = Kind::Glow;
      e.pos[0] = c.pos[0] / c.weight;
      e.pos[1] = c.pos[1] / c.weight;
      e.pos[2] = c.pos[2] / c.weight;
      const float inv = 1.0f / c.weight;
      e.rgb[0] = std::min(1.0f, c.rgb[0] * inv);
      e.rgb[1] = std::min(1.0f, c.rgb[1] * inv);
      e.rgb[2] = std::min(1.0f, c.rgb[2] * inv);
      e.radius = c.radius;
      e.intensity = std::min(kGlowEnergyCap, std::sqrt(c.energy2)); // sprites of a cell add as energy, not as a sum
      (void)collector.Track(kKeyGlow | (item.first & 0xFFFFFFFFFFull), e, now, kHoldTicks);
    }

    // ---- trails: a segment per tick; one light per cell of the head, refreshed while the trail grows there
    for (const SWorldTrail& trail : batch.mTrails) {
      if (!(trail.mSize > 0.0f) || !std::isfinite(trail.mSize)) {
        continue;
      }
      const std::string texturePath = PathOf(trail.mTexture);
      const TexEntry* texture = LoadTexture(texturePath);
      if (texture == nullptr || texture->colour.peak < 0.2f) {
        continue; // a trail whose texture does not glow (a smoke trail) gives no light
      }
      const TexEntry* ramp = LoadTexture(PathOf(trail.mRampTexture));
      Emission e;
      e.kind = Kind::Trail;
      e.pos[0] = trail.mStartPos.x;
      e.pos[1] = trail.mStartPos.y;
      e.pos[2] = trail.mStartPos.z;
      e.end[0] = trail.mEndPos.x;
      e.end[1] = trail.mEndPos.y;
      e.end[2] = trail.mEndPos.z;
      e.size0 = e.size1 = trail.mSize;
      FillColour(e, texture, nullptr, 0.0f);
      if (ramp != nullptr) {
        float rgba[4];
        SampleRamp(ramp->image, 0.1f, 0.0f, rgba); // the head of the trail
        for (int c = 0; c < 3; ++c) {
          e.rgb[c] = std::min(1.0f, texture->colour.hue[c] * rgba[c]);
        }
      }
      std::uint64_t key = Mix(0x7ull, CellKey(e.end[0], e.end[1], e.end[2], 6.0f));
      key = Mix(key, std::hash<std::string>{}(texturePath));
      (void)collector.Track(kKeyTrail | (key & 0xFFFFFFFFFFull), e, now, kHoldTicks);
    }

    // ---- beams: one capsule per beam, refreshed every tick it exists
    for (const SWorldBeam& beam : batch.mBeams) {
      if (!(beam.mWidth > 0.0f) || !std::isfinite(beam.mWidth)) {
        continue;
      }
      const std::string texturePath = PathOf(beam.mTexture1);
      const TexEntry* texture = LoadTexture(texturePath);
      Emission e;
      e.kind = Kind::Beam;
      e.pos[0] = beam.mStart.x;
      e.pos[1] = beam.mStart.y;
      e.pos[2] = beam.mStart.z;
      e.end[0] = beam.mEnd.x;
      e.end[1] = beam.mEnd.y;
      e.end[2] = beam.mEnd.z;
      e.size0 = e.size1 = beam.mWidth;
      const float* hue = texture != nullptr ? texture->colour.hue : kFallbackRgb;
      // the beam is drawn as texture x vertex colour (start colour at one end, end colour at the other): the light has the mean
      for (int c = 0; c < 3; ++c) {
        const float start = c == 0 ? beam.mStartColor.x : (c == 1 ? beam.mStartColor.y : beam.mStartColor.z);
        const float end = c == 0 ? beam.mEndColor.x : (c == 1 ? beam.mEndColor.y : beam.mEndColor.z);
        e.rgb[c] = std::min(1.0f, std::max(0.0f, hue[c] * 0.5f * (start + end)));
      }
      std::uint64_t key = Mix(0x9ull, CellKey(e.pos[0], e.pos[1], e.pos[2], 8.0f));
      key = Mix(key, CellKey(e.end[0], e.end[1], e.end[2], 8.0f));
      key = Mix(key, std::hash<std::string>{}(texturePath));
      (void)collector.Track(kKeyBeam | (key & 0xFFFFFFFFFFull), e, now, kHoldTicks);
    }

    // ---- Lua lights: tracked by their handle
    for (const SWorldLight& light : batch.mLights) {
      if (light.id == 0) {
        continue;
      }
      Emission e;
      e.kind = light.spot != 0 ? Kind::Spot : Kind::Glow;
      e.pos[0] = light.pos[0];
      e.pos[1] = light.pos[1];
      e.pos[2] = light.pos[2];
      e.end[0] = light.dir[0];
      e.end[1] = light.dir[1];
      e.end[2] = light.dir[2];
      e.cone = light.cone;
      e.rgb[0] = light.rgb[0];
      e.rgb[1] = light.rgb[1];
      e.rgb[2] = light.rgb[2];
      e.radius = light.radius;
      e.intensity = light.intensity;
      (void)collector.Track(kKeyLua | light.id, e, now, kHoldTicks);
    }
  }
} // namespace moho::lights
