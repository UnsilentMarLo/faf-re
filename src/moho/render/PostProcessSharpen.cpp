#include "moho/render/PostProcessSharpen.h"

#include <cctype>
#include <string>

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/PostProcessPass.h"
#include "gpg/gal/backends/common/PostProcessSmaa.h"
#include "moho/console/CConCommand.h"
#include "moho/misc/StartupHelpers.h"
#include "moho/render/RenderFeatures.h"

namespace moho
{
  int ren_SharpenQuality = gpg::gal::kPostProcessSharpenOff;

  namespace
  {
    TConVar<int> gTConVar_ren_SharpenQuality(
      "ren_SharpenQuality",
      "Contrast adaptive sharpening of the 3D world on Direct3D 11, 12 and Vulkan, after the post-process anti-aliasing: 0 off (the default), "
      "1 Low, 2 High. Normally set by the Sharpening option (Options > Video) or /sharpen off|low|high on the command line "
      "(docs/RENDERER_BACKENDS.md).",
      &ren_SharpenQuality
    );

    // The Options > Video entry (render features registry): Off / Low / High are ren_SharpenQuality 0..2, applied live. It needs the GPU programs
    // (GpuDriven.h): Direct3D 11, 12 and Vulkan. Direct3D 9 draws with the shipped shaders and has no post-process stage to add one to
    // without a render-target copy the renderer does not otherwise make; Direct3D 10 does not start.
    const bool gRegisteredFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "sharpen_quality";
      feature.title = "Sharpening";
      feature.tip = "Brings back the fine detail that anti-aliasing and the scaling of textures soften: units, buildings and terrain look crisper "
                    "while flat areas and the interface stay as they are. A single pass (about 0.03 ms per frame at 1600x900). Render only. Takes "
                    "effect at once.";
      feature.console = "ren_SharpenQuality";
      feature.states = {{"Off", "off"}, {"Low", "low"}, {"High", "high"}};
      feature.defaultLevel = 0;
      feature.needsRestart = false;
      feature.apis[static_cast<int>(Api::D3D9)].reason = "Direct3D 9 draws with the shipped shaders and has no post-process stage to run the pass in";
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) {
        if (level > 0) {
          ren_SharpenQuality = level;
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
      if (!CFG_GetArgOption("/sharpen", 1U, &args) || args.empty()) {
        return;
      }
      std::string value = args[0].c_str();
      for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (value == "off" || value == "0") {
        ren_SharpenQuality = gpg::gal::kPostProcessSharpenOff;
      } else if (value == "low" || value == "1") {
        ren_SharpenQuality = gpg::gal::kPostProcessSharpenLow;
      } else if (value == "high" || value == "2") {
        ren_SharpenQuality = gpg::gal::kPostProcessSharpenHigh;
      } else {
        gpg::Warnf("/sharpen %s is not off, low or high; sharpening stays at %d", value.c_str(), ren_SharpenQuality);
      }
    }
  } // namespace

  int REN_GetSharpenLevel()
  {
    ApplyCommandLine();
    return ren_SharpenQuality;
  }

  void REN_ApplyPostProcessSharpen(gpg::gal::Device* const device, const std::uint32_t head)
  {
    const int level = REN_GetSharpenLevel();
    if (level <= 0 || device == nullptr) {
      return;
    }
    static int sLoggedLevel = -1;
    const bool applied = gpg::gal::PostProcessPass::Instance().ApplySharpen(*device, head, static_cast<std::uint32_t>(level > 2 ? 2 : level));
    if (sLoggedLevel != level) {
      sLoggedLevel = level;
      if (applied) {
        gpg::Logf("Sharpening level %d active", level);
      } else {
        gpg::Warnf("Sharpening level %d is not available on this render API; nothing is drawn", level);
      }
    }
  }
}
