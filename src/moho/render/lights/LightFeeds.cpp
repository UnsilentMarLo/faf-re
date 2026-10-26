#include "moho/render/lights/LightFeeds.h"

#include <cmath>
#include <string_view>

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/DynLightPass.h"
#include "moho/console/CConCommand.h"
#include "moho/particles/SParticleBuffer.h"
#include "moho/particles/SWorldParticle.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/lights/LightBinding.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"

namespace moho::lights
{
  int ren_DynLights = model::kOff;
  static int ren_DynLightsLog = 0;
  int ren_DynLightsTest = 0;

  static TConVar<int> gTConVar_ren_DynLights(
    "ren_DynLights",
    "Dynamic lights from explosion flashes, glows, beams and trails: 0 off (the default), 1 only the biggest, 2 large, 3 most, 4 all. "
    "Render only; the simulation never sees them. Work in progress: nothing draws them yet (see docs/DYNAMIC_LIGHTS.md).",
    &ren_DynLights
  );

  static TConVar<int> gTConVar_ren_DynLightsTest(
    "ren_DynLightsTest",
    "Test light for the dynamic lights: every N ticks a flash (size 12, 2 seconds) appears on the ground in the middle of the view; "
    "0 (the default) adds none. Needs ren_DynLights.",
    &ren_DynLightsTest
  );

  static TConVar<int> gTConVar_ren_DynLightsLog(
    "ren_DynLightsLog",
    "Log the dynamic light pool (offered, stored, gated, active lights) every N rendered frames; 0 (the default) logs nothing.",
    &ren_DynLightsLog
  );

  namespace
  {
    // Colour of a CreateLightParticle flash until the ramp texture can be read on the CPU (stage 5): the warm white the patch used for a
    // light without a ramp (255, 215, 160).
    constexpr float kDefaultFlashRgb[3] = {1.0f, 0.843f, 0.627f};

    LightCollector gCollector;
    std::vector<Light> gFrameLights;
    LightGridImage gFrameImage;
    int gLogCounter = 0;
    int gLastTick = -1; // tick of the last BeginFrame

    void SyncQuality()
    {
      if (gCollector.Quality() != ren_DynLights) {
        gCollector.SetQuality(ren_DynLights);
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
    // Width and depth of the active map in world units; false when there is no map (front end, loading).
    bool ActiveMapSize(float& sizeX, float& sizeZ)
    {
      CWldSession* const session = WLD_GetActiveSession();
      if (session == nullptr) {
        return false;
      }
      const STIMap* const map = session->GetSTIMap();
      const CHeightField* const field = map != nullptr ? map->GetHeightField() : nullptr;
      if (field == nullptr || field->Width() < 2 || field->Height() < 2) {
        return false;
      }
      sizeX = static_cast<float>(field->Width() - 1);
      sizeZ = static_cast<float>(field->Height() - 1);
      return true;
    }
  } // namespace

  LightCollector& Collector()
  {
    return gCollector;
  }

  void FeedParticleBuffer(const SParticleBuffer& batch, const int beatTick)
  {
    SyncQuality();
    if (gCollector.Quality() == model::kOff) {
      return;
    }

    for (const SWorldParticle& particle : batch.mParticles) {
      if (particle.mTypeTag.view() != std::string_view("TLight")) {
        continue;
      }

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
      e.birth = static_cast<float>(beatTick) + BirthFraction(particle.mInterop);
      e.life = particle.mLifetime;
      e.size0 = particle.mBeginSize;
      e.size1 = particle.mEndSize;
      e.rgb[0] = kDefaultFlashRgb[0];
      e.rgb[1] = kDefaultFlashRgb[1];
      e.rgb[2] = kDefaultFlashRgb[2];
      (void)gCollector.Add(e);
    }
  }

  void BeginFrame(const int tick, const float frameAlpha)
  {
    gLastTick = tick;
    SyncQuality();
    if (gCollector.Quality() == model::kOff) {
      gpg::gal::gDynLightPassActive.store(false, std::memory_order_relaxed);
      if (!gFrameLights.empty() || gFrameImage.lightCount != 0) {
        gFrameLights.clear();
        gFrameImage.lightCount = 0; // the binding clears the textures once
        LightGridImage cleared;
        float sx = 0.0f, sz = 0.0f;
        if (ActiveMapSize(sx, sz)) {
          BuildLightGrid(gFrameLights, GridLayout::ForMap(sx, sz), cleared);
          gFrameImage = cleared;
          UpdateBinding(gFrameImage);
        }
      }
      return;
    }

    gCollector.Update(static_cast<float>(tick) + frameAlpha, kMaxFrameLights, gFrameLights);

    float mapX = 0.0f, mapZ = 0.0f;
    if (ActiveMapSize(mapX, mapZ)) {
      BuildLightGrid(gFrameLights, GridLayout::ForMap(mapX, mapZ), gFrameImage);
      UpdateBinding(gFrameImage);
    }
    gpg::gal::gDynLightPassActive.store(gFrameImage.lightCount > 0, std::memory_order_relaxed);

    if (ren_DynLightsLog > 0 && ++gLogCounter >= ren_DynLightsLog) {
      gLogCounter = 0;
      const Stats& s = gCollector.GetStats();
      gpg::Logf(
        "DynLights: quality=%d stored=%u offered=%u gated=%u invalid=%u full=%u expired=%u | pool=%u active=%u lights=%u cut=%u | "
        "grid cells=%u overflow=%u | tex uploads=%u fail=%u vars=%u",
        gCollector.Quality(),
        s.stored,
        s.offered,
        s.gated,
        s.invalid,
        s.poolFull,
        s.expired,
        static_cast<unsigned>(gCollector.Stored()),
        s.active,
        static_cast<unsigned>(gFrameLights.size()),
        s.culledByCap,
        gFrameImage.cellsLit,
        gFrameImage.overflow,
        GetBindingStats().uploads,
        GetBindingStats().failures,
        GetBindingStats().boundVariables
      );
    }
  }

  void SetView(GeomCamera3& camera)
  {
    if (gCollector.Quality() == model::kOff) {
      return;
    }
    const Wm3::Vec3f& eye = camera.tranform.pos_;
    SetCameraPosition(eye.x, eye.y, eye.z);

    // test light: the point where the ray through the middle of the view meets the ground
    static int sLastTestTick = -1;
    if (ren_DynLightsTest > 0 && gLastTick >= 0 && gLastTick != sLastTestTick && (gLastTick % ren_DynLightsTest) == 0) {
      sLastTestTick = gLastTick;
      CWldSession* const session = WLD_GetActiveSession();
      const STIMap* const map = session != nullptr ? session->GetSTIMap() : nullptr;
      const CHeightField* const field = map != nullptr ? map->GetHeightField() : nullptr;
      if (field != nullptr) {
        const float w = camera.viewport.r[3].z;
        const float h = camera.viewport.r[3].w;
        const GeomLine3 ray = camera.Unproject(Wm3::Vector2f(w * 0.5f, h * 0.5f));
        for (float t = 1.0f; t < 6000.0f; t += 3.0f) {
          const float x = ray.pos.x + ray.dir.x * t;
          const float y = ray.pos.y + ray.dir.y * t;
          const float z = ray.pos.z + ray.dir.z * t;
          const float ground = field->GetElevation(x, z);
          if (y <= ground) {
            Emission e;
            e.kind = Kind::Flash;
            e.pos[0] = x;
            e.pos[1] = ground + 6.0f;
            e.pos[2] = z;
            e.birth = static_cast<float>(gLastTick);
            e.life = 20.0f;
            e.size0 = 18.0f;
            e.size1 = 18.0f;
            e.rgb[0] = 1.0f;
            e.rgb[1] = 0.6f;
            e.rgb[2] = 0.25f;
            (void)gCollector.Add(e);
            break;
          }
        }
      }
    }
  }

  const std::vector<Light>& FrameLights()
  {
    return gFrameLights;
  }

  const LightGridImage& FrameImage()
  {
    return gFrameImage;
  }
} // namespace moho::lights
