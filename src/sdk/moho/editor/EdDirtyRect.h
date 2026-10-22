// EDITOR: the rectangle a height edit hands to the terrain's derived data (normal map, tier error grids), engine independent so it can be tested on its own
// (moho/editor/tests/EdDirtyRectTest.cpp).
//
// What the engine recomputes from a changed rectangle, and from how much of the neighbourhood (recovered code, src/sdk/moho/sim):
//   * Normal map (`CWldTerrainRes::UpdateNormalMap(rect)`, CWldMap.cpp): one texel per cell; texel (i, j) is `CHeightField::GetNormal(i + 0.5, j + 0.5)`, i.e. four bilinear
//     reads at (i, j + 0.5), (i + 1, j + 0.5), (i + 0.5, j), (i + 0.5, j + 1): it depends on the samples i .. i + 1 and j .. j + 1. A changed sample x therefore changes the
//     texels x - 1 and x. The function treats the rectangle's x1 / z1 as an EXCLUSIVE end (clippedX1 = min(tileEnd, rect.x1), loop `< clippedX1`; the 4 x 4 DXT blocks are
//     rounded out by the function itself, per tile), the samples' rectangle is INCLUSIVE: a tight rectangle [x0..x1] handed over as it is updates the texels x0 .. x1 - 1 and
//     leaves x0 - 1 (left / top) and x1 (right / bottom) with their old normal: a one texel crease on every border of the rectangle.
//   * Error grids (`CHeightField::UpdateError(rect)` -> UpdateErrorKernel, STIMap.cpp): tier t cell g spans the samples g << t .. (g + 1) << t INCLUSIVE, but the kernel
//     visits the cells [x0 >> t, ceil(x1 / t)) (exclusive): a sample on a cell corner also belongs to the cell next to it, which is skipped, on both sides. One extra sample on
//     each side reaches those cells at every tier.
// So every consumer needs the changed rectangle grown by at least 1 sample on every side and clamped to the field; 2 is used (a margin against the exclusive / inclusive
// reading of the engine's end coordinate). Neither consumer needs the rectangle rounded out to a tile grid (the engine clips per normal map tile and aligns to its blocks
// on its own), `tile` > 1 is for callers whose own update works in chunks.
#pragma once

#include <algorithm>

namespace moho::editor
{
  /** Inclusive sample rectangle (the shape of gpg::Rect2i / CellRect, without their headers). */
  struct DirtyRect
  {
    int x0 = 0;
    int z0 = 0;
    int x1 = -1;
    int z1 = -1;

    [[nodiscard]] constexpr bool Empty() const noexcept { return x1 < x0 || z1 < z0; }
  };

  /** Margin around a height edit for the derived data (see above: 1 is needed, 2 is used). */
  constexpr int kDerivedDataPad = 2;

  /**
   * `rect` grown by `pad` samples on every side, optionally rounded out to a grid of `tile` samples (tile <= 1: no rounding; x0 / z0 down to a multiple of `tile`, x1 / z1 up
   * to the last sample of its tile), clamped to the field 0 .. w - 1, 0 .. h - 1. An empty input, a negative pad or a field without samples gives an empty rectangle.
   */
  [[nodiscard]] constexpr DirtyRect PadDirtyRect(const DirtyRect rect, const int pad, const int w, const int h, const int tile) noexcept
  {
    if (rect.Empty() || pad < 0 || w <= 0 || h <= 0) {
      return DirtyRect{};
    }
    long x0 = static_cast<long>(rect.x0) - pad;
    long z0 = static_cast<long>(rect.z0) - pad;
    long x1 = static_cast<long>(rect.x1) + pad;
    long z1 = static_cast<long>(rect.z1) + pad;
    x0 = std::max(0L, x0);
    z0 = std::max(0L, z0);
    x1 = std::min(static_cast<long>(w) - 1, x1);
    z1 = std::min(static_cast<long>(h) - 1, z1);
    if (tile > 1) {
      const long t = tile;
      x0 = (x0 / t) * t;
      z0 = (z0 / t) * t;
      x1 = std::min(static_cast<long>(w) - 1, (x1 / t) * t + t - 1);
      z1 = std::min(static_cast<long>(h) - 1, (z1 / t) * t + t - 1);
    }
    if (x1 < x0 || z1 < z0) {
      return DirtyRect{};
    }
    return DirtyRect{static_cast<int>(x0), static_cast<int>(z0), static_cast<int>(x1), static_cast<int>(z1)};
  }
} // namespace moho::editor
