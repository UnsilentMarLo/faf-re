#include "moho/render/AmbientOcclusion.h"

#include <cctype>
#include <cmath>
#include <string>

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/AmbientOcclusionPass.h"
#include "gpg/gal/Device.hpp"
#include "moho/console/CConCommand.h"
#include "moho/misc/StartupHelpers.h"
#include "moho/render/RenderFeatures.h"
#include "moho/render/camera/GeomCamera3.h"

namespace moho
{
  int ren_AmbientOcclusion = gpg::gal::kAmbientOcclusionOff;
  float ren_AORadius = 1.5f;
  float ren_AOIntensity = 1.0f;
  float ren_AOPower = 1.6f;
  int ren_AOHalfRes = -1;

  namespace
  {
    TConVar<int> gTConVar_ren_AmbientOcclusion(
      "ren_AmbientOcclusion",
      "Screen-space ambient occlusion of the 3D world on Direct3D 11, 12 and Vulkan: 0 off (the default), 1 Low, 2 Medium, 3 High. "
      "Normally set by the Ambient Occlusion option (Options > Video) or /ao off|low|medium|high on the command line (docs/RENDERER_BACKENDS.md).",
      &ren_AmbientOcclusion
    );
    TConVar<float> gTConVar_ren_AORadius(
      "ren_AORadius", "Ambient occlusion: how far (world units) the search for occluding geometry reaches. Default 1.5.", &ren_AORadius
    );
    TConVar<float> gTConVar_ren_AOIntensity(
      "ren_AOIntensity", "Ambient occlusion: how strongly the occlusion darkens the picture; 1 is as computed (the default), 0 nothing.", &ren_AOIntensity
    );
    TConVar<float> gTConVar_ren_AOPower(
      "ren_AOPower", "Ambient occlusion: contrast of the occlusion; more is darker in the creases and lighter on open ground. Default 1.6.", &ren_AOPower
    );
    TConVar<int> gTConVar_ren_AOHalfRes(
      "ren_AOHalfRes", "Ambient occlusion: -1 the level's choice (half resolution at Low and Medium, the default), 0 full resolution, 1 half resolution.", &ren_AOHalfRes
    );

    // The Options > Video entry (render features registry): Off / Low / Medium / High are ren_AmbientOcclusion 0..3, applied live (the pass reads
    // the console variable every frame). The pass needs the head's depth buffer as a texture and the GPU programs (GpuDriven.h): Direct3D 11,
    // 12 and Vulkan. Direct3D 9 draws with the shipped shader model 2 techniques, which write no depth the pass could read; Direct3D 10 does not start.
    const bool gRegisteredFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "ambient_occlusion";
      feature.title = "Ambient Occlusion";
      feature.tip = "Darkens the creases and the places where terrain, buildings and units meet, which gives the scene depth. Render only, never affects "
                    "the game. Low and Medium work at half resolution. Takes effect at once.";
      feature.console = "ren_AmbientOcclusion";
      feature.states = {{"Off", "off"}, {"Low", "low"}, {"Medium", "medium"}, {"High", "high"}};
      feature.defaultLevel = 0;
      feature.needsRestart = false;
      feature.apis[static_cast<int>(Api::D3D9)].reason = "Direct3D 9 draws with the shipped shaders and a depth buffer it cannot sample";
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) {
        if (level > 0) {
          ren_AmbientOcclusion = level;
        }
      };
      render::RegisterFeature(feature);
      return true;
    }();

    void ApplyCommandLine()
    {
      static bool applied = false;
      if (applied) {
        return;
      }
      applied = true;
      msvc8::vector<msvc8::string> args;
      if (!CFG_GetArgOption("/ao", 1U, &args) || args.empty()) {
        return;
      }
      std::string value = args[0].c_str();
      for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (value == "off" || value == "0") {
        ren_AmbientOcclusion = gpg::gal::kAmbientOcclusionOff;
      } else if (value == "low" || value == "1") {
        ren_AmbientOcclusion = gpg::gal::kAmbientOcclusionLow;
      } else if (value == "medium" || value == "2") {
        ren_AmbientOcclusion = gpg::gal::kAmbientOcclusionMedium;
      } else if (value == "high" || value == "3") {
        ren_AmbientOcclusion = gpg::gal::kAmbientOcclusionHigh;
      } else {
        gpg::Warnf("/ao %s is not off, low, medium or high; ambient occlusion stays at %d", value.c_str(), ren_AmbientOcclusion);
      }
    }
  } // namespace

  int REN_GetAmbientOcclusionLevel()
  {
    ApplyCommandLine();
    return ren_AmbientOcclusion;
  }

  void REN_ApplyAmbientOcclusion(
    gpg::gal::Device* const device, const std::uint32_t head, const GeomCamera3& camera, const int x, const int y, const int width, const int height
  )
  {
    const int level = REN_GetAmbientOcclusionLevel();
    if (level <= 0 || device == nullptr || width <= 0 || height <= 0) {
      return;
    }
    // A perspective projection puts the view distance in w (m23 = -1 for the engine's right-handed cameras, +1 for a left-handed one; m33 = 0);
    // the cartographic and minimap views are orthographic.
    const auto& projection = camera.projection;
    if (std::fabs(projection.r[2].w) < 0.5f || std::fabs(projection.r[3].w) > 0.5f) {
      static bool sLoggedSkip = false;
      if (!sLoggedSkip) {
        sLoggedSkip = true;
        gpg::Logf(
          "Ambient occlusion skips a view with a non-perspective projection (rows: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f)",
          projection.r[0].x, projection.r[0].y, projection.r[0].z, projection.r[0].w, projection.r[1].x, projection.r[1].y, projection.r[1].z,
          projection.r[1].w, projection.r[2].x, projection.r[2].y, projection.r[2].z, projection.r[2].w, projection.r[3].x, projection.r[3].y,
          projection.r[3].z, projection.r[3].w
        );
      }
      return;
    }
    gpg::gal::AmbientOcclusionParams params;
    params.level = level > 3 ? 3 : level;
    params.projectionXScale = projection.r[0].x;
    params.projectionYScale = projection.r[1].y;
    // depth = A + B / distance in front of the camera: A is -m22 when the camera looks down -z (m23 = -1), m22 otherwise.
    params.projectionDepthScale = projection.r[2].w < 0.0f ? -projection.r[2].z : projection.r[2].z;
    params.projectionDepthBias = projection.r[3].z;
    params.rectX = static_cast<float>(x);
    params.rectY = static_cast<float>(y);
    params.rectWidth = static_cast<float>(width);
    params.rectHeight = static_cast<float>(height);
    params.radius = ren_AORadius;
    params.intensity = ren_AOIntensity;
    params.power = ren_AOPower;
    params.halfResolution = ren_AOHalfRes;
    static int sLoggedLevel = -1;
    const bool applied = gpg::gal::AmbientOcclusionPass::Instance().Apply(*device, head, params);
    if (sLoggedLevel != level) {
      sLoggedLevel = level;
      if (applied) {
        gpg::Logf(
          "Ambient occlusion level %d active (%s resolution, view %dx%d at %d,%d, projection %.3f %.3f %.3f %.3f)", level,
          gpg::gal::AmbientOcclusionHalfRes(params) ? "half" : "full", width, height, x, y, params.projectionXScale, params.projectionYScale,
          params.projectionDepthScale, params.projectionDepthBias
        );
      }
    }
  }
}
