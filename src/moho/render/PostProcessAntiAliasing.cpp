#include "moho/render/PostProcessAntiAliasing.h"

#include <cctype>
#include <string>

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/PostProcessPass.h"
#include "gpg/gal/backends/common/PostProcessAA.h"
#include "moho/console/CConCommand.h"
#include "moho/misc/StartupHelpers.h"
#include "moho/render/RenderFeatures.h"

namespace moho
{
  int ren_AntiAliasing = gpg::gal::kPostProcessAAOff;

  namespace
  {
    // The console variable. Registration is kept in this one object so it can move into the render feature registry unchanged.
    TConVar<int> gTConVar_ren_AntiAliasing(
      "ren_AntiAliasing",
      "Post-process anti-aliasing of the 3D world on Direct3D 11, 12 and Vulkan: 0 off (the default), 1 FXAA, 2 SMAA 1x. Direct3D 9 uses "
      "its own multisampling (Options > Video). /aa off|fxaa|smaa on the command line sets it at start (docs/RENDERER_BACKENDS.md).",
      &ren_AntiAliasing
    );

    // The Options > Video entry (render features registry): Off / FXAA / SMAA are ren_AntiAliasing 0..2. Applies live (a console variable the
    // pass reads every frame). Direct3D 11 and 12 cannot multisample a flip-model swap chain, so they filter the finished world instead;
    // Direct3D 9 keeps its real multisampling dropdown (Options, Video, Anti-Aliasing) and Direct3D 10 does not start.
    const bool gRegisteredFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "post_aa";
      feature.title = "Post-process Anti-Aliasing";
      feature.tip = "Smooths the jagged edges of units, buildings and terrain in the 3D world; the interface stays sharp. FXAA is a single cheap pass "
                    "(about 0.05 ms per frame at 1600x900); SMAA 1x takes three passes (about 0.1 ms) and keeps fine detail sharper. Render only. Takes effect at once.";
      feature.console = "ren_AntiAliasing";
      feature.states = {{"Off", "off"}, {"FXAA", "fxaa"}, {"SMAA", "smaa"}};
      feature.defaultLevel = 0;
      feature.needsRestart = false;
      feature.apis[static_cast<int>(Api::D3D9)].reason = "Direct3D 9 has real multisampling (Options, Video, Anti-Aliasing)";
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) {
        if (level > 0) {
          ren_AntiAliasing = level;
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
      if (!CFG_GetArgOption("/aa", 1U, &args) || args.empty()) {
        return;
      }
      std::string value = args[0].c_str();
      for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (value == "off" || value == "0") {
        ren_AntiAliasing = gpg::gal::kPostProcessAAOff;
      } else if (value == "fxaa" || value == "1") {
        ren_AntiAliasing = gpg::gal::kPostProcessAAFxaa;
      } else if (value == "smaa" || value == "2") {
        ren_AntiAliasing = gpg::gal::kPostProcessAASmaa;
      } else {
        gpg::Warnf("/aa %s is not off, fxaa or smaa; anti-aliasing stays at %d", value.c_str(), ren_AntiAliasing);
      }
    }
  } // namespace

  int REN_GetPostProcessAAMode()
  {
    ApplyCommandLine();
    return ren_AntiAliasing;
  }

  void REN_ApplyPostProcessAA(gpg::gal::Device* const device, const std::uint32_t head)
  {
    const int mode = REN_GetPostProcessAAMode();
    if (mode <= 0 || device == nullptr) {
      return;
    }
    static int sLoggedMode = -1;
    // FXAA is one pass per backend (Device::ApplyPostProcessAA); SMAA is three and runs the same on every backend with GPU programs.
    const bool applied = mode == gpg::gal::kPostProcessAASmaa ? gpg::gal::PostProcessPass::Instance().ApplySmaa(*device, head)
                                                              : device->ApplyPostProcessAA(head, static_cast<std::uint32_t>(mode));
    if (sLoggedMode != mode) {
      sLoggedMode = mode;
      if (applied) {
        gpg::Logf("Post-process anti-aliasing mode %d active", mode);
      } else {
        gpg::Warnf("Post-process anti-aliasing mode %d is not available on this render API; nothing is filtered", mode);
      }
    }
  }
}
