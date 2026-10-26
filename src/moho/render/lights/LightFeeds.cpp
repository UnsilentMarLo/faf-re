#include <algorithm>
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
#include "moho/render/lights/LightCensus.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"

namespace moho::lights
{
  int ren_DynLights = model::kOff;
  static int ren_DynLightsLog = 0;
  int ren_DynLightsTest = 0;

  static TConVar<int> gTConVar_ren_DynLights(
    "ren_DynLights",
    "Dynamic lights from explosion flashes, glows, beams, trails and Lua lights: 0 off (the default), 1 only the biggest, 2 large, 3 most, 4 all. "
    "Render only; the simulation never sees them. Needs /dynlights on the command line (docs/DYNAMIC_LIGHTS.md).",
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
    LightCollector gCollector;
    std::vector<Light> gFrameLights;
    LightGridImage gFrameImage;
    int gLogCounter = 0;
    int gLastTick = -1; // tick of the last BeginFrame

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

  void SyncQuality()
  {
    if (gCollector.Quality() != ren_DynLights) {
      gCollector.SetQuality(ren_DynLights);
    }
  }

  void BeginFrame(const int tick, const float frameAlpha)
  {
    // A clock that jumped back is a new game: the lights of the old one must not wake up when the new one reaches their birth tick.
    if (gLastTick >= 0 && tick + 20 < gLastTick) {
      gCollector.Clear();
    }
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

    FeedBeamsForFrame(tick, frameAlpha);
    gCollector.Update(static_cast<float>(tick) + frameAlpha, kMaxFrameLights, gFrameLights);

    float mapX = 0.0f, mapZ = 0.0f;
    if (ActiveMapSize(mapX, mapZ)) {
      BuildLightGrid(gFrameLights, GridLayout::ForMap(mapX, mapZ), gFrameImage);
      UpdateBinding(gFrameImage);
    }
    gpg::gal::gDynLightPassActive.store(gFrameImage.lightCount > 0, std::memory_order_relaxed);
    CensusFrame();

    if (ren_DynLightsLog > 0 && ++gLogCounter >= ren_DynLightsLog) {
      gLogCounter = 0;
      for (std::size_t i = 0; i < gFrameLights.size() && i < 4; ++i) {
        const Light& l = gFrameLights[i];
        gpg::Logf(
          "DynLights:   light %u kind=%d pos=(%.1f %.1f %.1f) radius=%.1f rgb=(%.3f %.3f %.3f) power=%.2f",
          static_cast<unsigned>(i),
          static_cast<int>(l.kind),
          l.pos[0],
          l.pos[1],
          l.pos[2],
          l.radius,
          l.rgb[0],
          l.rgb[1],
          l.rgb[2],
          l.power
        );
      }
      {
        // strength by kind: how many lights, their colour x intensity at the centre (mean / strongest) and radius, and the flux (sum of the
        // brightest channel x radius squared), to see which kind dominates a scene
        static const char* const kKindNames[] = {"flash", "glow", "beam", "trail", "mesh", "spot"};
        unsigned count[6] = {};
        double sumPeak[6] = {}, sumRadius[6] = {}, flux[6] = {};
        float maxPeak[6] = {};
        for (const Light& l : gFrameLights) {
          const int k = static_cast<int>(l.kind);
          if (k < 0 || k >= 6) {
            continue;
          }
          const float peak = std::max(l.rgb[0], std::max(l.rgb[1], l.rgb[2]));
          ++count[k];
          sumPeak[k] += peak;
          sumRadius[k] += l.radius;
          flux[k] += static_cast<double>(peak) * l.radius * l.radius;
          maxPeak[k] = std::max(maxPeak[k], peak);
        }
        for (int k = 0; k < 6; ++k) {
          if (count[k] > 0) {
            gpg::Logf(
              "DynLights: kind %s n=%u peak mean %.3f max %.3f radius mean %.1f flux %.0f",
              kKindNames[k],
              count[k],
              sumPeak[k] / count[k],
              maxPeak[k],
              sumRadius[k] / count[k],
              flux[k]
            );
          }
        }
      }
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
