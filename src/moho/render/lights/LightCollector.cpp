#include "moho/render/lights/LightCollector.h"

#include <algorithm>
#include <cmath>

namespace moho::lights
{
  namespace
  {
    constexpr float kMaxExplicitRadius = 5000.0f;
    constexpr float kMaxExplicitIntensity = 100.0f;

    bool Finite3(const float (&v)[3])
    {
      return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    }

    // Radius and intensity of an emission at a given sprite size: the author's own numbers when stated, else the model's for its kind.
    void Strength(const Emission& e, const float size, float& radius, float& intensity)
    {
      if (e.radius > 0.0f) {
        radius = e.radius;
        intensity = e.intensity;
        return;
      }
      switch (e.kind) {
      case Kind::Glow:
        radius = model::GlowRadius(size);
        intensity = model::GlowEnergy(size);
        return;
      case Kind::Beam:
      case Kind::Trail:
        radius = model::TrailRadius(size);
        intensity = model::TrailIntensity(size);
        return;
      default:
        radius = model::FlashRadius(size);
        intensity = model::FlashIntensity(size);
        return;
      }
    }

    // Which importance an emission has at its peak.
    float PeakPower(const Emission& e)
    {
      float radius = 0.0f;
      float intensity = 0.0f;
      Strength(e, std::max(e.size0, e.size1), radius, intensity);
      return model::Power(intensity, radius);
    }

    // 0..1 envelope: linear fade-in over `inTicks` after birth, full during the life, linear fade-out over `outTicks` after it.
    float Envelope(const float age, const float life, const float inTicks, const float outTicks)
    {
      if (age < 0.0f) {
        return 0.0f;
      }
      float env = 1.0f;
      if (inTicks > 0.0f && age < inTicks) {
        env = age / inTicks;
      }
      if (age > life) {
        if (outTicks <= 0.0f) {
          return 0.0f;
        }
        const float out = 1.0f - (age - life) / outTicks;
        env = std::min(env, out);
      }
      return env < 0.0f ? 0.0f : env;
    }
  } // namespace

  float LightCollector::FadeInTicks() const
  {
    return model::kFadeInSeconds / secondsPerTick_;
  }

  float LightCollector::FadeOutTicks() const
  {
    return model::kFadeOutSeconds / secondsPerTick_;
  }

  void LightCollector::RebuildIndex()
  {
    trackedIndex_.clear();
    stats_.tracked = 0;
    for (std::size_t i = 0; i < pool_.size(); ++i) {
      if (pool_[i].key != 0) {
        trackedIndex_[pool_[i].key] = i;
        ++stats_.tracked;
      }
    }
  }

  void LightCollector::SetQuality(const int quality)
  {
    const int q = quality < model::kOff ? model::kOff : (quality > model::kUltra ? model::kUltra : quality);
    quality_ = q;
    // A lower level no longer admits some of what is stored (the class of an entry never changes, so one check is enough).
    const std::size_t before = pool_.size();
    pool_.erase(
      std::remove_if(pool_.begin(), pool_.end(), [q](const Entry& entry) { return !model::Admits(q, entry.peakPower); }),
      pool_.end()
    );
    if (pool_.size() != before) {
      RebuildIndex();
    }
  }

  void LightCollector::SetSecondsPerTick(const float secondsPerTick)
  {
    // Outside this range the clock is paused or garbage; keep the last good value.
    if (secondsPerTick > 0.005f && secondsPerTick < 2.0f) {
      secondsPerTick_ = secondsPerTick;
    }
  }

  void LightCollector::PruneExpired(const float nowTick)
  {
    const float outTicks = FadeOutTicks();
    const std::size_t before = pool_.size();
    pool_.erase(
      std::remove_if(
        pool_.begin(), pool_.end(), [&](const Entry& entry) { return nowTick - entry.e.birth > entry.e.life + outTicks; }
      ),
      pool_.end()
    );
    if (pool_.size() != before) {
      stats_.expired += static_cast<std::uint32_t>(before - pool_.size());
      RebuildIndex();
    }
  }

  bool LightCollector::Validate(const Emission& emission)
  {
    const float peak = std::max(emission.size0, emission.size1);
    const bool explicitStrength = emission.radius > 0.0f;
    const bool ok = Finite3(emission.pos) && Finite3(emission.dir) && Finite3(emission.accel) && Finite3(emission.end) &&
                    std::isfinite(emission.cone) && std::isfinite(emission.birth) && std::isfinite(emission.life) &&
                    std::isfinite(emission.size0) && std::isfinite(emission.size1) && std::isfinite(emission.radius) &&
                    std::isfinite(emission.intensity) && emission.size0 >= 0.0f && emission.size1 >= 0.0f &&
                    emission.life > 0.0f && emission.life < model::kMaxLifeTicks &&
                    (explicitStrength ? (emission.radius < kMaxExplicitRadius && emission.intensity >= 0.0f &&
                                         emission.intensity <= kMaxExplicitIntensity)
                                      : (peak > 0.0f && peak < model::kMaxSpriteSize));
    if (!ok) {
      ++stats_.invalid;
    }
    return ok;
  }

  bool LightCollector::Add(const Emission& emission)
  {
    ++stats_.offered;
    if (quality_ == model::kOff) {
      return false; // off is free: no validation, no counters beyond `offered`
    }
    if (!Validate(emission)) {
      return false;
    }

    Entry entry{};
    entry.e = emission;
    for (float& c : entry.e.rgb) {
      c = model::Clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
    }
    for (auto& point : entry.e.curve) {
      for (float& c : point) {
        c = model::Clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
      }
    }
    entry.peakPower = PeakPower(entry.e);
    if (!model::Admits(quality_, entry.peakPower)) {
      ++stats_.gated;
      return false;
    }

    if (pool_.size() >= kPoolCapacity) {
      PruneExpired(lastNow_);
      if (pool_.size() >= kPoolCapacity) {
        ++stats_.poolFull;
        return false;
      }
    }

    entry.serial = nextSerial_++;
    entry.key = 0;
    pool_.push_back(entry);
    ++stats_.stored;
    return true;
  }

  bool LightCollector::Track(const std::uint64_t key, const Emission& emission, const float nowTick, const float holdTicks)
  {
    ++stats_.offered;
    if (quality_ == model::kOff || key == 0) {
      return false;
    }
    if (!std::isfinite(nowTick) || !std::isfinite(holdTicks) || holdTicks <= 0.0f) {
      ++stats_.invalid;
      return false;
    }

    const auto found = trackedIndex_.find(key);
    Emission e = emission;
    e.dir[0] = e.dir[1] = e.dir[2] = 0.0f; // the owner moves the light by refreshing it
    e.accel[0] = e.accel[1] = e.accel[2] = 0.0f;
    if (found != trackedIndex_.end() && found->second < pool_.size() && pool_[found->second].key == key) {
      Entry& entry = pool_[found->second];
      e.birth = entry.e.birth; // fades in once
      e.life = (nowTick - e.birth) + holdTicks;
      if (!Validate(e)) {
        return false;
      }
      for (float& c : e.rgb) {
        c = model::Clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
      }
      entry.e = e;
      entry.peakPower = std::max(entry.peakPower, PeakPower(e)); // the class never drops while the light lives
      return true;
    }

    e.birth = nowTick;
    e.life = holdTicks;
    if (!Validate(e)) {
      return false;
    }
    for (float& c : e.rgb) {
      c = model::Clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
    }
    Entry entry{};
    entry.e = e;
    entry.peakPower = PeakPower(e);
    if (!model::Admits(quality_, entry.peakPower)) {
      ++stats_.gated;
      return false;
    }
    if (pool_.size() >= kPoolCapacity) {
      PruneExpired(lastNow_);
      if (pool_.size() >= kPoolCapacity) {
        ++stats_.poolFull;
        return false;
      }
    }
    entry.serial = nextSerial_++;
    entry.key = key;
    pool_.push_back(entry);
    trackedIndex_[key] = pool_.size() - 1;
    ++stats_.tracked;
    ++stats_.stored;
    return true;
  }

  void LightCollector::Untrack(const std::uint64_t key, const float nowTick)
  {
    const auto found = trackedIndex_.find(key);
    if (found == trackedIndex_.end() || found->second >= pool_.size() || pool_[found->second].key != key) {
      return;
    }
    Entry& entry = pool_[found->second];
    entry.e.life = std::max(0.001f, std::isfinite(nowTick) ? nowTick - entry.e.birth : 0.001f); // the life ends now; the fade-out runs
    entry.key = 0;
    trackedIndex_.erase(found);
    if (stats_.tracked > 0) {
      --stats_.tracked;
    }
  }

  void LightCollector::Update(const float nowTick, const std::size_t maxLights, std::vector<Light>& out)
  {
    out.clear();
    stats_.active = 0;
    stats_.culledByCap = 0;
    if (quality_ == model::kOff) {
      pool_.clear();
      trackedIndex_.clear();
      stats_.tracked = 0;
      return;
    }
    if (!std::isfinite(nowTick)) {
      return; // a broken clock changes nothing
    }
    lastNow_ = nowTick;
    PruneExpired(nowTick);

    const float inTicks = FadeInTicks();
    const float outTicks = FadeOutTicks();

    struct Ranked
    {
      Light light;
      float birth;
      std::uint32_t serial;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(pool_.size());

    for (const Entry& entry : pool_) {
      const Emission& e = entry.e;
      const float age = nowTick - e.birth;
      const float env = Envelope(age, e.life, inTicks, outTicks);
      if (env <= 0.0f) {
        continue;
      }

      // Size follows the particle: linear from size0 to size1 over the life, held after it.
      const float t01 = model::Clamp(age / e.life, 0.0f, 1.0f);
      const float size = e.size0 + (e.size1 - e.size0) * t01;
      float radius = 0.0f;
      float baseIntensity = 0.0f;
      Strength(e, size, radius, baseIntensity);
      const float intensity = baseIntensity * model::WeakenFactor(radius) * env;

      // The colour now: constant, or along the ramp curve.
      float rgbNow[3] = {e.rgb[0], e.rgb[1], e.rgb[2]};
      if (e.hasCurve) {
        const float x = t01 * 3.0f;
        const int i0 = x >= 3.0f ? 2 : static_cast<int>(x);
        const float f = x - static_cast<float>(i0);
        for (int k = 0; k < 3; ++k) {
          rgbNow[k] = model::Clamp(e.curve[i0][k] + (e.curve[i0 + 1][k] - e.curve[i0][k]) * f, 0.0f, 1.0f);
        }
      }

      // Brightness below the visible threshold adds nothing: drop it before it takes a slot.
      const float maxChannel = std::max(rgbNow[0], std::max(rgbNow[1], rgbNow[2]));
      if (maxChannel * intensity < model::kMinVisible) {
        continue;
      }

      // The particle shader's motion rule, t in ticks (never before birth).
      const float t = age < 0.0f ? 0.0f : age;
      Ranked r{};
      r.light.pos[0] = e.pos[0] + e.dir[0] * t + 0.5f * e.accel[0] * t * t;
      r.light.pos[1] = e.pos[1] + e.dir[1] * t + 0.5f * e.accel[1] * t * t;
      r.light.pos[2] = e.pos[2] + e.dir[2] * t + 0.5f * e.accel[2] * t * t;
      if (e.kind == Kind::Beam || e.kind == Kind::Trail) {
        r.light.end[0] = e.end[0] + (r.light.pos[0] - e.pos[0]);
        r.light.end[1] = e.end[1] + (r.light.pos[1] - e.pos[1]);
        r.light.end[2] = e.end[2] + (r.light.pos[2] - e.pos[2]);
      } else if (e.kind == Kind::Spot) {
        r.light.end[0] = e.end[0];
        r.light.end[1] = e.end[1];
        r.light.end[2] = e.end[2];
      } else {
        r.light.end[0] = r.light.pos[0];
        r.light.end[1] = r.light.pos[1];
        r.light.end[2] = r.light.pos[2];
      }
      r.light.cone = e.cone;
      r.light.radius = radius;
      r.light.rgb[0] = rgbNow[0] * intensity;
      r.light.rgb[1] = rgbNow[1] * intensity;
      r.light.rgb[2] = rgbNow[2] * intensity;
      r.light.kind = e.kind;
      r.light.power = model::Power(intensity, radius);
      r.birth = e.birth;
      r.serial = entry.serial;
      ranked.push_back(r);
    }

    stats_.active = static_cast<std::uint32_t>(ranked.size());
    const auto stronger = [](const Ranked& a, const Ranked& b) {
      if (a.light.power != b.light.power) {
        return a.light.power > b.light.power;
      }
      if (a.birth != b.birth) {
        return a.birth < b.birth;
      }
      return a.serial < b.serial;
    };
    std::sort(ranked.begin(), ranked.end(), stronger);

    const std::size_t take = std::min(maxLights, ranked.size());
    stats_.culledByCap = static_cast<std::uint32_t>(ranked.size() - take);
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
      out.push_back(ranked[i].light);
    }
  }

  void LightCollector::Clear()
  {
    pool_.clear();
    trackedIndex_.clear();
    stats_.tracked = 0;
  }
} // namespace moho::lights
