// EDITOR: access to the terrain-type grid for other editor natives (the field engine, EdField.cpp). Implemented in EdTerrainType.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace moho::editor
{
  /** The whole terrain-type grid (one TypeCode byte per cell, row major). false when the map has none. */
  bool TerrainTypeRead(std::vector<std::uint8_t>& out, int& w, int& h);

  /** Replaces the whole terrain-type grid in the session's map and in the simulation's copy; w x h must be the grid size. */
  bool TerrainTypeWrite(const std::uint8_t* data, int w, int h);
} // namespace moho::editor
