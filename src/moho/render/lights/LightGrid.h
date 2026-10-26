#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "moho/render/lights/LightCollector.h"

// The CPU image of the two light textures a lit shader reads (docs/DYNAMIC_LIGHTS.md, "Textures"). Pure data and arithmetic, no engine
// headers and no GPU: the host test checks it, LightBinding uploads it.
//
//   Light data   RGBA32F, kDataWidth texels wide. Light i owns texels 3i .. 3i+2 (row-major, rows of kDataWidth):
//                  A = (pos.x, pos.y, pos.z, radius)
//                  B = (r, g, b, kind)            colour x intensity, linear; kind = Kind as a float
//                  C = (x, y, z, 0)               the other end of a capsule (a beam); equal to pos for a point light
//   Light grid   RGBA32F, 2 * cellsX texels wide, cellsZ rows. Cell (cx, cz) owns texels (2cx, cz) and (2cx+1, cz): up to 8 entries,
//                each "light index + 1" (0 = empty), strongest light first, filled left to right. Exact in a float up to 2^24.
//
// The grid covers the whole map in the XZ plane, so it does not depend on any camera: every view (main, reflections, split screen) reads
// the same textures. A light is entered in every cell its radius reaches (the 2D footprint, ignoring height: conservative).
//
// Shader lookup, for the translator step (stage 3):  cell = floor((p.xz - origin) * invCell); texel = (2*cell.x [+1], cell.z).
namespace moho::lights
{
  constexpr std::uint32_t kDataWidth = 1024;       // texels
  constexpr std::uint32_t kTexelsPerLight = 3;
  constexpr std::uint32_t kMaxGridCells = 64;      // per side
  constexpr std::uint32_t kLightsPerCell = 8;
  constexpr float kMinCellSize = 16.0f;            // world units
  constexpr std::size_t kMaxGridLights = 256;      // the data texture holds this many (768 texels) in one row

  // Where the grid sits in the world. Built from the map's extent.
  struct GridLayout
  {
    float originX = 0.0f; // world position of the corner of cell (0, 0)
    float originZ = 0.0f;
    float cellSize = kMinCellSize;
    std::uint32_t cellsX = 1;
    std::uint32_t cellsZ = 1;

    // A map of `sizeX` x `sizeZ` world units, origin at 0: the cell size is the smallest that fits kMaxGridCells cells on the longer
    // side, but not below kMinCellSize.
    static GridLayout ForMap(float sizeX, float sizeZ);
  };

  struct LightGridImage
  {
    GridLayout layout;
    std::uint32_t lightCount = 0;
    std::vector<float> data; // RGBA texels, kDataWidth x DataRows()
    std::vector<float> grid; // RGBA texels, GridWidth() x layout.cellsZ

    // Statistics of the last Build
    std::uint32_t cellsLit = 0;    // cells with at least one light
    std::uint32_t overflow = 0;    // (light, cell) pairs that did not fit the 8 entries
    std::uint32_t outsideMap = 0;  // lights whose footprint misses the map entirely
    std::uint32_t skipped = 0;     // invalid lights (non-finite)

    [[nodiscard]] std::uint32_t GridWidth() const { return layout.cellsX * 2u; }
    [[nodiscard]] static std::uint32_t DataRows(std::uint32_t lightCount);
    [[nodiscard]] std::uint32_t DataRows() const { return DataRows(lightCount); }

    // The up to 8 light indices of the cell containing the world point (what the shader does); returns how many. 0 outside the map.
    std::uint32_t CellLights(float x, float z, std::uint32_t (&indices)[kLightsPerCell]) const;
    // Number of lights in cell (cx, cz).
    [[nodiscard]] std::uint32_t CellCount(std::uint32_t cx, std::uint32_t cz) const;
  };

  // Fills `image` for `lights` (already ranked strongest first, as LightCollector::Update returns them; at most kMaxGridLights are used).
  // Capsule end points are optional: lights of kinds other than Beam/Trail get C = pos.
  void BuildLightGrid(const std::vector<Light>& lights, const GridLayout& layout, LightGridImage& image);
} // namespace moho::lights
