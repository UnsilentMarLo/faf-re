// EDITOR: access to the water masks (foam, flatness, depth bias) for other editor natives (the field engine, EdField.cpp). Implemented in EdEnv.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace moho::editor
{
  /** The bytes of water mask `kind` (0 foam, 1 flatness, 2 depth bias), row major, (map width / 2) x (map height / 2) samples. false when there is no terrain. */
  bool WaterMaskRead(int kind, std::vector<std::uint8_t>& out, int& w, int& h);

  /** Replaces the whole mask `kind` and rebuilds the water map texture that carries it; w x h must be the mask size. */
  bool WaterMaskWrite(int kind, const std::uint8_t* data, int w, int h);
} // namespace moho::editor
