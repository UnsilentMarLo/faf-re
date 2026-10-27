#pragma once

#include <string>

// Source-level patch of FAF's mesh.fx and terrain.fx that replaces the shadow-map lookup with a filtered one
// (docs/SHADOW_FILTERING.md, "Shader side").
//
// Like the dynamic-light patch (moho/render/lights/LightEffectPatch.h) it is pure text in, text out: the effect files are user data, they
// are never copied into this repository. The two patches touch different places (this one the shadow lookup functions, the lights the
// vertex shaders, PBR helpers and technique list) and are chained by CD3DEffect::InitEffectFromFile: this one runs first, the lights patch
// continues from its output, so neither depends on the other's text.
//
// An unexpected source (a FAF update that moved something this patch relies on) leaves the text untouched and says why in `error`.
//
// Effect parameters the patched text declares: mesh.fx `shadowFilter`, `shadowFilter2`; terrain.fx `ShadowFilter`, `ShadowFilter2` (both
// float4, set by ShadowFilter.cpp every frame):
//   filter.x   tap count; 0 selects the shipped lookup (the patched shader then behaves as the original one)
//   filter.y   kernel radius in shadow texels (the smallest radius when the penumbra estimate is on)
//   filter.z   largest radius in texels, also the blocker search radius
//   filter.w   penumbra scale (shadow map uv per unit of depth difference); 0 = fixed radius, no blocker search
//   filter2.x  scale of the constant depth bias, filter2.y scale of the receiver-plane (slope) bias
namespace moho::shadowfilter
{
  struct PatchResult
  {
    bool patched = false;
    int lookups = 0; // lookup sites that now go through the filter
    std::string error;
  };

  PatchResult PatchMeshEffect(std::string& source);
  PatchResult PatchTerrainEffect(std::string& source);

  // True for the base names of the effects this patch handles ("mesh", "terrain").
  bool HandlesEffect(const std::string& baseName);
} // namespace moho::shadowfilter
