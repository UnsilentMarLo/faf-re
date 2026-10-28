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
//                  C = (x, y, z, cone)            Beam / Trail: the other end of the capsule; Spot: the direction (unit) and the cosine of the
//                                                 outer half angle; every other light: = pos
//   Light grid   RGBA32F, 4 * cellsX texels wide, cellsZ rows. Cell (cx, cz) owns texels (4cx .. 4cx+3, cz): up to 16 entries,
//                each "light index + 1" (0 = empty), strongest light first, filled left to right. Exact in a float up to 2^24.
//
// The grid covers the whole map in the XZ plane, so it does not depend on any camera: every view (main, reflections, split screen) reads
// the same textures. A light is entered in every cell its radius reaches (the 2D footprint, ignoring height: conservative).
//
// Shader lookup (LightEffectPatch.cpp):  cell = floor((p.xz - origin) * invCell); texels = (4*cell.x + 0..3, cell.z).
namespace moho::lights
{
  constexpr std::uint32_t kDataWidth = 1024;       // texels
  constexpr std::uint32_t kTexelsPerLight = 3;
  constexpr std::uint32_t kMaxGridCells = 128;     // per side
  // The grid texture is one linear store of RGBA32F texels, kGridTexelWidth wide, kGridRows high (a texel k sits at column k mod width, row
  // k div width): first one table texel per cell (x = first list texel, y = list texels, z = entries), then the lists. A list holds the
  // lights that reach the cell, "light index + 1", four to a texel, 0 = empty, strongest at the middle of the cell first. There is no
  // fixed number per cell (the first versions had 8, then 16 entries, and a cell that overflowed listed other lights than its neighbour:
  // lit squares); only a safety limit, and the whole texture's room.
  constexpr std::uint32_t kGridTexelWidth = 1024;
  constexpr std::uint32_t kGridRows = 128;
  constexpr std::uint32_t kMaxLightsPerCell = 256;
  // World units. A cell is a few times the size of a typical light; on a small map (10 km: 128 cells of 4) a cell sees few lights.
  constexpr float kMinCellSize = 4.0f;
  constexpr std::size_t kMaxGridLights = 4096;     // the data texture holds this many (12288 texels = 12 rows); the first patch had 16 / 28

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
    std::vector<float> grid; // RGBA texels, kGridTexelWidth wide, GridRows() rows in use (the texture has kGridRows)
    std::uint32_t gridTexels = 0; // texels in use: the table and the lists

    // Statistics of the last Build
    std::uint32_t cellsLit = 0;    // cells with at least one light
    std::uint32_t overflow = 0;    // (light, cell) pairs dropped: a cell over kMaxLightsPerCell, or no room left in the texture
    std::uint32_t entries = 0;     // (light, cell) pairs listed
    std::uint32_t outsideMap = 0;  // lights whose footprint misses the map entirely
    std::uint32_t skipped = 0;     // invalid lights (non-finite)

    [[nodiscard]] std::uint32_t GridRows() const { return (gridTexels + kGridTexelWidth - 1) / kGridTexelWidth; }
    [[nodiscard]] static std::uint32_t DataRows(std::uint32_t lightCount);
    [[nodiscard]] std::uint32_t DataRows() const { return DataRows(lightCount); }

    // The light indices of the cell containing the world point (what the shader does), strongest first; returns how many. 0 outside the map.
    std::uint32_t CellLights(float x, float z, std::uint32_t (&indices)[kMaxLightsPerCell]) const;
    // Number of lights in cell (cx, cz).
    [[nodiscard]] std::uint32_t CellCount(std::uint32_t cx, std::uint32_t cz) const;
  };

  // Fills `image` for `lights` (already ranked strongest first, as LightCollector::Update returns them; at most kMaxGridLights are used).
  // Capsule lights (Beam, Trail) are entered in every cell their segment reaches.
  void BuildLightGrid(const std::vector<Light>& lights, const GridLayout& layout, LightGridImage& image);
} // namespace moho::lights
