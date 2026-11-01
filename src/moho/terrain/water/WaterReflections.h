#pragma once

#include <string>

namespace gpg::gal
{
  class Device;
}

namespace moho
{
  struct GeomCamera3;
}

// Screen-space reflections for the water (docs/LIGHTING_V2.md, "Water shader"): the water pixel shader marches the reflected view ray through a
// copy of the scene depth and, where the ray goes behind what the scene drew, takes that pixel of the scene colour (the copy the refraction
// already uses) as the reflection. Terrain, buildings, trees and lit units show in the water; the sky cube and the mirrored-mesh pass stay as the
// fallback where the ray leaves the screen or hits nothing. Render only.
namespace moho::water
{
  // 0 off, 1 (the default) 24 steps, 2 48 steps of the march.
  extern int ren_WaterSSR;

  // Whether the water effect should get the scene-depth patch: the march (ren_WaterSSR) or the real-depth thickness (ren_WaterAbsorb) is on.
  // The real-depth thickness: the water pixel shader replaces the map's baked half-resolution depth by the distance the view ray travels through the water
  // before the sampled scene depth, so tint, refraction strength and the "in front of the water" rejection follow what is really there.
  bool SsrWanted();

  struct SsrPatchResult
  {
    bool patched = false;
    std::string error;
  };
  // Source patch of water2.fx: declares the depth sampler and the march and mixes the hit into the reflection. Text in, text out.
  SsrPatchResult PatchWaterReflections(std::string& source);

  // Once per frame right before the water is drawn, with the world's colour and depth targets bound: copies the depth, binds it and the march
  // parameters to the water effect. Binds "off" when the view cannot use it (orthographic, no sampled depth).
  void BindWaterReflections(gpg::gal::Device* device, const moho::GeomCamera3& camera);

  // Frees the GPU resources (a device reset, shutdown).
  void ReleaseWaterReflections();
} // namespace moho::water
