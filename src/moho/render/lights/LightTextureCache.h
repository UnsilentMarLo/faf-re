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
  // (the failure is remembered, the file is not tried again). The cache is cleared when it holds 512 textures.
  const TexEntry* LoadTexture(const std::string& path);
} // namespace moho::lights
