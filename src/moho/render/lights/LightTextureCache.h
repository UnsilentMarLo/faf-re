#pragma once

#include <string>

#include "moho/render/lights/LightTexture.h"

// The colour of effect and unit textures read from the game's files, cached by path (docs/DYNAMIC_LIGHTS.md, "Colour"). Render thread only.
namespace moho::lights
{
  struct TexEntry
  {
    bool ok = false;
    TextureColour colour;
    Rgba8Image image; // the decoded top mip that fits 256 x 256: kept for ramps and for the unit glow analysis
  };

  // Reads the DDS file of a virtual path ("/textures/particles/glow_03.dds") and summarises it. nullptr when the file cannot be read or decoded
  // (the failure is remembered, the file is not tried again). The pointer stays valid until the next TrimTextureCache.
  const TexEntry* LoadTexture(const std::string& path);

  // Clears the cache when it holds 512 textures. Called once a frame, before any feed holds a pointer (clearing inside LoadTexture left the
  // albedo of a unit dangling while its specular map loaded: a crash after a long game).
  void TrimTextureCache();
} // namespace moho::lights
