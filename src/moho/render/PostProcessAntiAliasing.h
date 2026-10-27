#pragma once

#include <cstdint>

namespace gpg::gal
{
  class Device;
}

// FAF addition, not in the shipped binary: engine side of the post-process anti-aliasing (docs/RENDERER_BACKENDS.md). The Direct3D 11 and
// 12 flip-model swap chains cannot be multisampled, so the finished 3D world in the back buffer is filtered (FXAA, SMAA 1x) before the
// UI is drawn into it. Render only; nothing here touches the simulation.
namespace moho
{
  // ren_AntiAliasing: 0 off (the default), 1 FXAA, 2 SMAA (gpg::gal::PostProcessAAMode). /aa off|fxaa|smaa on the command line sets it
  // at start. The Options > Video item is to come from the render feature registry; the console variable is its backing value.
  extern int ren_AntiAliasing;

  // The registered ren_AntiAliasing value after the command line was applied (once).
  [[nodiscard]] int REN_GetPostProcessAAMode();

  // Runs the pass on head `head` of `device` when a mode is selected; the device draws nothing (and the first refusal is logged once)
  // when its backend has no such pass.
  void REN_ApplyPostProcessAA(gpg::gal::Device* device, std::uint32_t head);
}
