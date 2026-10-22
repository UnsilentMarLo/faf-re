// EDITOR: the grid math of the terrain-type natives (EdTerrainType.cpp): a hard disc of one type code, and a sample of the grid at the nodes of an overlay window. Header-only and free
// of engine types so it can be tested on its own (moho/editor/tests/EdTerrainTypeTest.cpp). It does the same arithmetic, in 32-bit floats and one operation per statement, as the Lua
// code it replaces (terraintype.lua Dab and NodeCodes), so the Lua fallback and the native agree cell for cell.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace moho::editor::ttype
{
  struct DiscResult
  {
    int changed = 0;   // cells that held another code
    int x0 = 0;        // inclusive cell rectangle of the cells that changed (empty when changed == 0)
    int z0 = 0;
    int x1 = -1;
    int z1 = -1;
  };

  /** Sets every cell of the w x h grid whose centre (x + 0.5, z + 0.5) lies within `radius` of (cx, cz) (all in cells) to `code`; reports the cells that changed. */
  inline DiscResult PaintDisc(std::uint8_t* const data, const int w, const int h, const float cx, const float cz, const float radius, const std::uint8_t code)
  {
    DiscResult out;
    const float lowX = cx - radius;
    const float lowZ = cz - radius;
    const float highX = cx + radius;
    const float highZ = cz + radius;
    const int x0 = std::max(0, static_cast<int>(std::floor(lowX)));
    const int z0 = std::max(0, static_cast<int>(std::floor(lowZ)));
    const int x1 = std::min(w - 1, static_cast<int>(std::ceil(highX)));
    const int z1 = std::min(h - 1, static_cast<int>(std::ceil(highZ)));
    if (x1 < x0 || z1 < z0) {
      return out;
    }
    const float r2 = radius * radius;
    for (int z = z0; z <= z1; ++z) {
      const float cellZ = static_cast<float>(z) + 0.5f;
      const float dz = cellZ - cz;
      const float dz2 = dz * dz;
      std::uint8_t* const row = data + static_cast<std::size_t>(z) * static_cast<std::size_t>(w);
      for (int x = x0; x <= x1; ++x) {
        const float cellX = static_cast<float>(x) + 0.5f;
        const float dx = cellX - cx;
        const float dx2 = dx * dx;
        const float d2 = dx2 + dz2;
        if (d2 <= r2 && row[x] != code) {
          row[x] = code;
          if (out.changed == 0) {
            out.x0 = x;
            out.z0 = z;
            out.x1 = x;
            out.z1 = z;
          } else {
            out.x0 = std::min(out.x0, x);
            out.x1 = std::max(out.x1, x);
            out.z0 = std::min(out.z0, z);
            out.z1 = std::max(out.z1, z);
          }
          ++out.changed;
        }
      }
    }
    return out;
  }

  /** The cell a world position falls in on one axis: floor(pos / cellSize), clamped to 0 .. n - 1 (terraintype.lua NodeCodes). */
  [[nodiscard]] inline int NodeCell(const float pos, const float cellSize, const int n)
  {
    const float q = pos / cellSize;
    const int c = static_cast<int>(std::floor(q));
    return std::max(0, std::min(n - 1, c));
  }

  /**
   * The codes under a grid of nw x nh nodes: node (i, j) lies at world (x0 + i * step, z0 + j * step) and takes the code of the cell it is in (cells are csx x csz world units).
   * `out` receives nw * nh codes, row major.
   */
  inline void SampleNodes(
    const std::uint8_t* const data, const int w, const int h, const int x0, const int z0, const int nw, const int nh, const int step, const float csx, const float csz,
    std::uint8_t* const out
  )
  {
    for (int j = 0; j < nh; ++j) {
      const float wz = static_cast<float>(z0 + j * step);
      const int cz = NodeCell(wz, csz, h);
      const std::uint8_t* const row = data + static_cast<std::size_t>(cz) * static_cast<std::size_t>(w);
      std::uint8_t* const dst = out + static_cast<std::size_t>(j) * static_cast<std::size_t>(nw);
      for (int i = 0; i < nw; ++i) {
        const float wx = static_cast<float>(x0 + i * step);
        dst[i] = row[NodeCell(wx, csx, w)];
      }
    }
  }
} // namespace moho::editor::ttype
