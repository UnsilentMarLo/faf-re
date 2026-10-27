#include "moho/render/lights/LightGrid.h"

#include <algorithm>
#include <cmath>

namespace moho::lights
{
  GridLayout GridLayout::ForMap(const float sizeX, const float sizeZ)
  {
    GridLayout layout;
    const float sx = std::isfinite(sizeX) && sizeX > 1.0f ? sizeX : 1.0f;
    const float sz = std::isfinite(sizeZ) && sizeZ > 1.0f ? sizeZ : 1.0f;
    const float longest = std::max(sx, sz);
    layout.cellSize = std::max(kMinCellSize, longest / static_cast<float>(kMaxGridCells));
    layout.cellsX = std::min<std::uint32_t>(kMaxGridCells, static_cast<std::uint32_t>(std::ceil(sx / layout.cellSize)));
    layout.cellsZ = std::min<std::uint32_t>(kMaxGridCells, static_cast<std::uint32_t>(std::ceil(sz / layout.cellSize)));
    layout.cellsX = std::max<std::uint32_t>(1, layout.cellsX);
    layout.cellsZ = std::max<std::uint32_t>(1, layout.cellsZ);
    return layout;
  }

  std::uint32_t LightGridImage::DataRows(const std::uint32_t lightCount)
  {
    const std::uint32_t texels = std::max<std::uint32_t>(1, lightCount) * kTexelsPerLight;
    return (texels + kDataWidth - 1) / kDataWidth;
  }

  std::uint32_t LightGridImage::CellCount(const std::uint32_t cx, const std::uint32_t cz) const
  {
    if (cx >= layout.cellsX || cz >= layout.cellsZ || grid.empty()) {
      return 0;
    }
    const std::size_t base = (static_cast<std::size_t>(cz) * GridWidth() + kCellTexels * cx) * 4u;
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < kLightsPerCell; ++i) {
      if (grid[base + i] != 0.0f) {
        ++n;
      }
    }
    return n;
  }

  std::uint32_t LightGridImage::CellLights(const float x, const float z, std::uint32_t (&indices)[kLightsPerCell]) const
  {
    const float fx = std::floor((x - layout.originX) / layout.cellSize);
    const float fz = std::floor((z - layout.originZ) / layout.cellSize);
    if (!(fx >= 0.0f && fz >= 0.0f && fx < static_cast<float>(layout.cellsX) && fz < static_cast<float>(layout.cellsZ)) ||
        grid.empty()) {
      return 0;
    }
    const std::size_t base = (static_cast<std::size_t>(fz) * GridWidth() + kCellTexels * static_cast<std::size_t>(fx)) * 4u;
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < kLightsPerCell; ++i) {
      const float entry = grid[base + i];
      if (entry != 0.0f) {
        indices[n++] = static_cast<std::uint32_t>(entry) - 1u;
      }
    }
    return n;
  }

  void BuildLightGrid(const std::vector<Light>& lights, const GridLayout& layout, LightGridImage& image)
  {
    image.layout = layout;
    image.cellsLit = 0;
    image.overflow = 0;
    image.outsideMap = 0;
    image.skipped = 0;

    const std::size_t requested = std::min(lights.size(), kMaxGridLights);
    image.lightCount = 0;
    image.data.assign(static_cast<std::size_t>(LightGridImage::DataRows(static_cast<std::uint32_t>(requested))) * kDataWidth * 4u, 0.0f);
    image.grid.assign(static_cast<std::size_t>(image.GridWidth()) * layout.cellsZ * 4u, 0.0f);

    // Every (cell, light) pair the lights' footprints produce, with what the light adds at the middle of the cell.
    struct CellLight
    {
      std::uint32_t cell;
      float contribution;
      std::uint32_t light;
    };
    std::vector<CellLight> pairs;
    pairs.reserve(requested * 16u);

    const float invCell = 1.0f / layout.cellSize;
    const float spanX = static_cast<float>(layout.cellsX) * layout.cellSize;
    const float spanZ = static_cast<float>(layout.cellsZ) * layout.cellSize;

    for (std::size_t i = 0; i < requested; ++i) {
      const Light& l = lights[i];
      const bool finite = std::isfinite(l.pos[0]) && std::isfinite(l.pos[1]) && std::isfinite(l.pos[2]) && std::isfinite(l.radius) &&
                          std::isfinite(l.rgb[0]) && std::isfinite(l.rgb[1]) && std::isfinite(l.rgb[2]) && l.radius > 0.0f;
      if (!finite) {
        ++image.skipped;
        continue;
      }

      // The light keeps the next free index even if its footprint misses the map: the shader may still be handed its index by a cell
      // of another light, never by this one, so an unreferenced entry is harmless and the indices stay equal to the ranking.
      const std::uint32_t index = image.lightCount++;
      float* const a = &image.data[static_cast<std::size_t>(index) * kTexelsPerLight * 4u];
      a[0] = l.pos[0];
      a[1] = l.pos[1];
      a[2] = l.pos[2];
      a[3] = l.kind == Kind::Spot ? -l.radius : l.radius; // a spotlight has a negative radius: the shader tells it from a capsule by that
      a[4] = l.rgb[0];
      a[5] = l.rgb[1];
      a[6] = l.rgb[2];
      a[7] = static_cast<float>(l.kind);
      const bool capsule = l.kind == Kind::Beam || l.kind == Kind::Trail;
      const bool spot = l.kind == Kind::Spot;
      a[8] = capsule || spot ? l.end[0] : l.pos[0];
      a[9] = capsule || spot ? l.end[1] : l.pos[1];
      a[10] = capsule || spot ? l.end[2] : l.pos[2];
      a[11] = spot ? l.cone : 0.0f;

      // Cells whose rectangle the light's footprint reaches: a circle, or for a capsule the circle swept along the segment.
      const bool finiteEnd = !capsule || (std::isfinite(l.end[0]) && std::isfinite(l.end[1]) && std::isfinite(l.end[2]));
      if (!finiteEnd) {
        ++image.skipped;
        --image.lightCount;
        continue;
      }
      const float ex = capsule ? l.end[0] : l.pos[0];
      const float ez = capsule ? l.end[2] : l.pos[2];
      const float minX = std::min(l.pos[0], ex) - l.radius - layout.originX;
      const float maxX = std::max(l.pos[0], ex) + l.radius - layout.originX;
      const float minZ = std::min(l.pos[2], ez) - l.radius - layout.originZ;
      const float maxZ = std::max(l.pos[2], ez) + l.radius - layout.originZ;
      if (maxX < 0.0f || maxZ < 0.0f || minX >= spanX || minZ >= spanZ) {
        ++image.outsideMap;
        continue;
      }
      const std::uint32_t cx0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(minX * invCell)));
      const std::uint32_t cz0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(minZ * invCell)));
      const std::uint32_t cx1 = static_cast<std::uint32_t>(
        std::min(static_cast<float>(layout.cellsX - 1), std::floor(maxX * invCell))
      );
      const std::uint32_t cz1 = static_cast<std::uint32_t>(
        std::min(static_cast<float>(layout.cellsZ - 1), std::floor(maxZ * invCell))
      );

      const float r2 = l.radius * l.radius;
      const float segX = ex - l.pos[0];
      const float segZ = ez - l.pos[2];
      const float segLen2 = segX * segX + segZ * segZ;
      const float halfDiag = 0.7072f * layout.cellSize;
      for (std::uint32_t cz = cz0; cz <= cz1; ++cz) {
        // distance along z from the light to this cell's rectangle
        const float cellMinZ = layout.originZ + static_cast<float>(cz) * layout.cellSize;
        const float dz = std::max(std::max(cellMinZ - l.pos[2], l.pos[2] - (cellMinZ + layout.cellSize)), 0.0f);
        for (std::uint32_t cx = cx0; cx <= cx1; ++cx) {
          const float cellMinX = layout.originX + static_cast<float>(cx) * layout.cellSize;
          if (!capsule) {
            const float dx = std::max(std::max(cellMinX - l.pos[0], l.pos[0] - (cellMinX + layout.cellSize)), 0.0f);
            if (dx * dx + dz * dz > r2) {
              continue; // the corner of the square box the circle does not reach
            }
          } else {
            // the cell's centre against the segment, with the cell's half diagonal as margin (conservative)
            const float px = cellMinX + 0.5f * layout.cellSize - l.pos[0];
            const float pz = cellMinZ + 0.5f * layout.cellSize - l.pos[2];
            const float t = segLen2 > 1.0e-6f ? std::min(1.0f, std::max(0.0f, (px * segX + pz * segZ) / segLen2)) : 0.0f;
            const float qx = px - segX * t;
            const float qz = pz - segZ * t;
            const float reach = l.radius + halfDiag;
            if (qx * qx + qz * qz > reach * reach) {
              continue;
            }
          }
          // What the light adds at the middle of the cell: the shader's windowed falloff at the closest point of the light's segment.
          const float px = cellMinX + 0.5f * layout.cellSize - l.pos[0];
          const float pz = cellMinZ + 0.5f * layout.cellSize - l.pos[2];
          const float t = capsule && segLen2 > 1.0e-6f ? std::min(1.0f, std::max(0.0f, (px * segX + pz * segZ) / segLen2)) : 0.0f;
          const float qx = px - segX * t;
          const float qz = pz - segZ * t;
          const float falloff = std::max(0.0f, 1.0f - (qx * qx + qz * qz) / r2);
          const float peak = std::max(l.rgb[0], std::max(l.rgb[1], l.rgb[2]));
          pairs.push_back(CellLight{static_cast<std::uint32_t>(cz) * layout.cellsX + cx, peak * falloff, index});
        }
      }
    }

    // A cell keeps the kLightsPerCell lights that add the most at its middle. Ranking them by the power the light has overall (the order of
    // `lights`) let a strong light that only grazes the cell push out a weak one right on top of it; whatever is dropped now is the least
    // visible in that cell, so the step at the border to the next cell (where the same light may still be listed) stays small.
    std::sort(pairs.begin(), pairs.end(), [](const CellLight& a, const CellLight& b) {
      if (a.cell != b.cell) {
        return a.cell < b.cell;
      }
      if (a.contribution != b.contribution) {
        return a.contribution > b.contribution;
      }
      return a.light < b.light;
    });
    for (std::size_t first = 0; first < pairs.size();) {
      std::size_t last = first;
      while (last < pairs.size() && pairs[last].cell == pairs[first].cell) {
        ++last;
      }
      const std::size_t count = last - first;
      const std::size_t kept = std::min<std::size_t>(count, kLightsPerCell);
      image.overflow += static_cast<std::uint32_t>(count - kept);
      std::uint32_t slots[kLightsPerCell];
      for (std::size_t k = 0; k < kept; ++k) {
        slots[k] = pairs[first + k].light;
      }
      std::sort(slots, slots + kept); // the strongest light first: the ranking order of `lights`
      const std::uint32_t cz = pairs[first].cell / layout.cellsX;
      const std::uint32_t cx = pairs[first].cell % layout.cellsX;
      ++image.cellsLit;
      // entries 0..3 are the first texel, 4..7 the second, and so on
      for (std::size_t k = 0; k < kept; ++k) {
        image.grid[(static_cast<std::size_t>(cz) * image.GridWidth() + kCellTexels * cx) * 4u + k] = static_cast<float>(slots[k] + 1u);
      }
      first = last;
    }
  }
} // namespace moho::lights
