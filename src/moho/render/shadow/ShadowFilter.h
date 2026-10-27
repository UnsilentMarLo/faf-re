#pragma once

#include "gpg/gal/DeviceContext.hpp"

// Engine side of the filtered shadows (docs/SHADOW_FILTERING.md): the quality level, the shadow map size that goes with it, the shader
// parameters of the patched lookup (ShadowEffectPatch) and the render features registry entry (`shadow_filter`). Render only: nothing
// here reads or writes sim state.
namespace moho::shadowfilter
{
  // 0 = Off (the shipped lookup), 1..4 = Low, Medium, High, Ultra. Console variable ren_ShadowFilter; set live by the Options > Video entry.
  extern int ren_ShadowFilter;

  struct LevelParams
  {
    int mapSize;         // shadow map edge in texels
    int taps;            // filter taps
    float minRadius;     // kernel radius in texels
    float maxRadius;     // largest radius (blocker search radius) when the penumbra estimate is on
    float penumbraTan;   // tangent of the light's angular radius for the penumbra estimate; 0 = fixed radius
  };

  // The parameters of a level (1..4); level 0 returns the Low entry.
  const LevelParams& ParamsFor(int level);

  // Whether the effects are patched on this API (the patch is always there where the API supports the filter; the level only switches it).
  bool Supported(gpg::gal::DeviceApi api);

  // CD3DEffect::InitEffectFromFile reports the effects it patched, so that the engine never selects the filtered path for an effect
  // that does not have it.
  void NoteEffectPatched(bool terrain, bool patched);

  // The filter is on: the level is above Off and both effects carry the patch.
  bool Active();

  // The shadow map edge the shadow renderer should have: the larger of the level's and the user's ren_ShadowSize, or just the user's while the filter is off.
  int WantedMapSize(int userSize);

  // Called by the shadow renderer when it derives the light camera: the world size of the light volume (width of the map in world units,
  // depth range in world units).
  void SetLightVolume(float width, float depthRange);

  // Writes the shader parameters of the current level (every frame, before the shadow lookups are drawn). `mapSize` is the size of the map
  // in use.
  void UpdateShaderVars(int mapSize);
} // namespace moho::shadowfilter
