#include "moho/render/PostProcessAntiAliasing.h"

#include <cctype>
#include <string>

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/backends/common/PostProcessAA.h"
#include "moho/console/CConCommand.h"
#include "moho/misc/StartupHelpers.h"

namespace moho
{
  int ren_AntiAliasing = gpg::gal::kPostProcessAAOff;

  namespace
  {
    // The console variable. Registration is kept in this one object so it can move into the render feature registry unchanged.
    TConVar<int> gTConVar_ren_AntiAliasing(
      "ren_AntiAliasing",
      "Post-process anti-aliasing of the 3D world on Direct3D 11 and 12: 0 off (the default), 1 FXAA, 2 SMAA. Direct3D 9 and 10 use their "
      "own multisampling (Options > Video). /aa off|fxaa|smaa on the command line sets it at start (docs/RENDERER_BACKENDS.md).",
      &ren_AntiAliasing
    );

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
    const bool applied = device->ApplyPostProcessAA(head, static_cast<std::uint32_t>(mode));
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
