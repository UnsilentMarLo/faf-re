// EDITOR: the geometry of an overlay decal laid on the terrain (EdOverlay.cpp, ed_overlay_decal): a square of `size` world units that follows the ground
// instead of floating flat at one height, cut into a grid of cells so that
//   * it lies on the slope (a flat quad cut through the ground on one side and hovered above it on the other),
//   * its picture is not distorted: the batcher interpolates the texture coordinates linearly over the screen, which is only right for a quad that is small
//     on screen; one big quad seen from close by (strong perspective) showed a skewed, malformed picture.
// A header-only pure function (no engine types) so it can be tested on its own (moho/editor/tests/EdOverlayGeomTest.cpp).
//
// Heights are given by a callback `float h(int ix, int iz)` for the integer sample (ix, iz) in world units, as the height field has one sample per world unit;
// the ground between the samples is the bilinear blend of the four around it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace moho::editor::overlaygeom
{
  /** Height of the ground at a world position: bilinear between the four samples around it; positions outside the field use its edge samples. */
  template <class Height>
  [[nodiscard]] inline float GroundAt(const Height& height, const int width, const int depth, const float x, const float z)
  {
    const float cx = std::min(std::max(x, 0.0f), static_cast<float>(width - 1));
    const float cz = std::min(std::max(z, 0.0f), static_cast<float>(depth - 1));
    const int ix = std::min(static_cast<int>(cx), std::max(0, width - 2));
    const int iz = std::min(static_cast<int>(cz), std::max(0, depth - 2));
    const float fx = cx - static_cast<float>(ix);
    const float fz = cz - static_cast<float>(iz);
    const int jx = std::min(ix + 1, width - 1);
    const int jz = std::min(iz + 1, depth - 1);
    const float h00 = height(ix, iz);
    const float h10 = height(jx, iz);
    const float h01 = height(ix, jz);
    const float h11 = height(jx, jz);
    return (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz) + (h01 * (1.0f - fx) + h11 * fx) * fz;
  }

  /**
   * Cells per side for a decal of `size` world units: fine enough to follow the terrain (a cell is at most half a unit across, the ground has a sample per
   * unit) and, when `screenPx` (the decal's widest extent on screen, <= 0 when unknown or partly behind the camera) is known, no finer than a cell of
   * about 16 pixels needs: a decal far away costs one or two quads, one up close up to 48 x 48.
   */
  [[nodiscard]] inline int DecalCells(const float size, const float screenPx)
  {
    int byWorld = static_cast<int>(std::ceil(size / 0.5f));
    byWorld = std::min(48, std::max(2, byWorld));
    if (screenPx > 0.0f) {
      const int byScreen = static_cast<int>(std::ceil(screenPx / 16.0f));
      return std::min(byWorld, std::max(1, byScreen));
    }
    return std::min(byWorld, 16);
  }

  constexpr float kOnGround = 1.0f;     // a decal given this close to the ground (either way) stands on the ground
  constexpr float kGroundLift = 0.05f;  // ... a little above it

  struct DecalPoint
  {
    float x, y, z;   // world position
    float u, v;      // texture coordinate
  };

  /**
   * The (cells + 1)^2 points of a decal centred on (cx, cz), `size` across, turned by `yaw` radians, row by row (v = 0 .. 1, u = 0 .. 1 inside a row), laid on the
   * ground: y = ground(x, z) + lift. The caller's height for the centre decides the lift: within kOnGround of the ground there (a splat standing on the terrain, whose
   * height was read from the nearest sample or lifted by a few hundredths) the lift is kGroundLift, so the decal lies on the slope however the height was rounded;
   * further off (a mesh surface above the field) the difference is kept, so the decal follows the field's slope at that height.
   * The corner order and the texture coordinates are those of the flat quad this replaces: (-h, -h) is u = 0, v = 0; (h, -h) is u = 1, v = 0.
   */
  template <class Height>
  inline void BuildDecal(const Height& height, const int width, const int depth, const float cx, const float cy, const float cz, const float size, const float yaw,
                         const int cells, std::vector<DecalPoint>& out)
  {
    const int n = std::max(1, cells);
    out.clear();
    out.reserve(static_cast<std::size_t>(n + 1) * static_cast<std::size_t>(n + 1));
    const float half = size * 0.5f;
    const float cs = std::cos(yaw);
    const float sn = std::sin(yaw);
    float lift = cy - GroundAt(height, width, depth, cx, cz);
    if (std::fabs(lift) <= kOnGround) {
      lift = kGroundLift;
    }
    for (int j = 0; j <= n; ++j) {
      const float v = static_cast<float>(j) / static_cast<float>(n);
      const float lz = -half + size * v;
      for (int i = 0; i <= n; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(n);
        const float lx = -half + size * u;
        const float wx = cx + lx * cs - lz * sn;
        const float wz = cz + lx * sn + lz * cs;
        out.push_back(DecalPoint{wx, GroundAt(height, width, depth, wx, wz) + lift, wz, u, v});
      }
    }
  }
} // namespace moho::editor::overlaygeom
