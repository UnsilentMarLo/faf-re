#pragma once

#include <cstdint>

namespace gpg::gal
{
  class Device;
}

namespace moho
{
  struct GeomCamera3;
}

// FAF addition, not in the shipped binary: engine side of the screen-space ambient occlusion (docs/RENDERER_BACKENDS.md, "Ambient occlusion").
// The finished 3D world of a view (opaque, water and effects, before the world-space overlays and the UI) is darkened where the geometry
// closes in; the pass itself is gpg::gal::AmbientOcclusionPass. Render only; nothing here touches the simulation.
namespace moho
{
  // ren_AmbientOcclusion: 0 off (the default), 1 Low, 2 Medium, 3 High. /ao off|low|medium|high on the command line sets it at start. The
  // Options > Video item comes from the render feature registry (key "ambient_occlusion"); the console variable is its backing value.
  extern int ren_AmbientOcclusion;

  // The level after the command line was applied (once).
  [[nodiscard]] int REN_GetAmbientOcclusionLevel();

  // Darkens head `head`'s back buffer inside the pixel rectangle (x, y, width, height) from the depth buffer, seen through `camera`. Nothing is
  // drawn for an orthographic camera or when the backend cannot (the first refusal is logged once).
  void REN_ApplyAmbientOcclusion(gpg::gal::Device* device, std::uint32_t head, const GeomCamera3& camera, int x, int y, int width, int height);
}
