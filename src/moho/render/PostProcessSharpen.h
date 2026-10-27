#pragma once

#include <cstdint>

namespace gpg::gal
{
  class Device;
}

// FAF addition, not in the shipped binary: engine side of the sharpening pass (docs/RENDERER_BACKENDS.md, "Sharpening"). A contrast adaptive
// sharpening of the finished 3D world, run right after the post-process anti-aliasing and before the UI. Render only; nothing here touches the
// simulation.
namespace moho
{
  // ren_SharpenQuality: 0 off (the default), 1 Low, 2 High (gpg::gal::PostProcessSharpenLevel). /sharpen off|low|high on the command line sets it
  // at start. The Options > Video item comes from the render feature registry (key "sharpen_quality"); the console variable is its backing value.
  extern int ren_SharpenQuality;

  // The level after the command line was applied (once).
  [[nodiscard]] int REN_GetSharpenLevel();

  // Sharpens head `head`'s back buffer when a level is selected; nothing is drawn (and the first refusal is logged once) when the backend has no
  // GPU programs.
  void REN_ApplyPostProcessSharpen(gpg::gal::Device* device, std::uint32_t head);
}
