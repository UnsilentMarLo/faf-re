// EDITOR: draping a screen quad over bumpy ground (EdOverlay.cpp: slope view blocks, node grids with a coarse step), engine independent so it can be tested on its
// own (moho/editor/tests/EdOverlayDrapeTest.cpp).
//
// The overlays are drawn as screen space quads between projected ground nodes. One quad between four far apart nodes is a flat patch: it only follows the terrain
// when the ground inside is flat too. Over rough ground the terrain pokes through the colour (dark slivers) or the colour floats above it. A patch is therefore
// tested against the REAL ground at its edge midpoints and its centre (`NeedsSplit`) and cut into 2 x 2 (or 2 x 1 / 1 x 2 for a strip) patches that are tested the
// same way, down to single cells, within a depth and a quad budget (`QuadBudget`).
//
// The test compares a sample's true screen position with where a FLAT patch through the four corners would put it, perspective correct: a planar world
// parallelogram has clip space coordinates that are affine in the patch parameters (u, v), so the screen position of (u, v) is
//     S(u, v) = sum(b_i * w_i * S_i) / sum(b_i * w_i)      b = bilinear weights, w_i = clip w of corner i, S_i = its screen position.
// A plain screen space bilinear interpolation would be off by several pixels on perfectly flat ground seen at a low angle and would split without reason.
//
// Also here: the "is this point in front of the camera" test. `viewport.r[1]` is NOT that (see InFront).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace moho::editor::overlaydrape
{
  /**
   * A point is in front of the camera when its clip space w (the divisor of the projection: the distance along the view direction for a perspective camera, the
   * `viewProjection` column 3 dotted with (x, y, z, 1)) is positive. `kNearW` keeps points that are almost in the camera plane out: their screen position runs off to
   * infinity and a quad between such a node and a visible one covers the whole screen.
   *
   * What it replaces: `camera.viewport.ProjectViewportDepthRow1(p) >= 1.0f`. Row 1 of the viewport matrix is the LOD plane: (view space depth * widthSlope + widthIntercept)
   * * lodScale, i.e. the world width of the view at that depth times the camera's LOD scale (GeomCamera3::Init, SetLODScale). `>= 1` therefore means "at least
   * 1 / (lodScale * 2 * tan(fov / 2) * aspect) in front", a threshold that moves with the LOD scale (cam_SetLOD, cam_HighLOD ...): lodScale 0 or a tiny scale rejects
   * every point (the overlay disappears), a scale of 0.1 rejects everything nearer than ~5 units (a camera low over the ground loses the near half of the picture).
   */
  constexpr float kNearW = 0.05f;

  [[nodiscard]] inline bool InFront(const float clipW) noexcept
  {
    return clipW > kNearW;
  }

  /** A projected ground node: screen position, the clip space w it was divided by, and the vertex z the overlay is drawn with (the depth, EdOverlayLod.h VertexZ). */
  struct Node
  {
    float x = 0.0f, y = 0.0f, w = 1.0f, z = 0.0f;
  };

  /** Corner order everywhere: c[0] = (u 0, v 0), c[1] = (1, 0), c[2] = (1, 1), c[3] = (0, 1). */
  [[nodiscard]] inline Node FlatPatchAt(const Node* const c, const float u, const float v) noexcept
  {
    const float b[4] = {(1.0f - u) * (1.0f - v), u * (1.0f - v), u * v, (1.0f - u) * v};
    float sw = 0.0f, sx = 0.0f, sy = 0.0f;
    for (int i = 0; i < 4; ++i) {
      const float k = b[i] * c[i].w;
      sw += k;
      sx += k * c[i].x;
      sy += k * c[i].y;
    }
    if (sw <= 1.0e-12f) {
      return Node{c[0].x, c[0].y, c[0].w};
    }
    return Node{sx / sw, sy / sw, sw};
  }

  struct Sample
  {
    float u, v;   // where in the patch (0..1) the real ground point was taken
    Node n;       // its true projection
  };

  /** Pixels between the true projection of a sample and where the flat patch puts it. */
  [[nodiscard]] inline float Deviation(const Node* const c, const Sample& s) noexcept
  {
    const Node f = FlatPatchAt(c, s.u, s.v);
    return std::sqrt((f.x - s.n.x) * (f.x - s.n.x) + (f.y - s.n.y) * (f.y - s.n.y));
  }

  /** True when any sample is more than `tolPx` away from the flat patch through the corners. */
  [[nodiscard]] inline bool NeedsSplit(const Node* const c, const Sample* const s, const int count, const float tolPx) noexcept
  {
    for (int i = 0; i < count; ++i) {
      if (Deviation(c, s[i]) > tolPx) {
        return true;
      }
    }
    return false;
  }

  /**
   * How many quads a frame may emit. `committed` counts the quads that are drawn or still waiting to be drawn (every top level patch counts one from the start, a
   * split adds the difference), so a patch that wants to split is refused when the rest of the frame could no longer be drawn at all.
   */
  struct QuadBudget
  {
    int cap = 60000;
    int committed = 0;

    [[nodiscard]] bool TrySplit(const int depth, const int maxDepth, const int extraQuads) noexcept
    {
      if (depth >= maxDepth || committed + extraQuads > cap) {
        return false;
      }
      committed += extraQuads;
      return true;
    }
  };

  /** ARGB of a patch corner set at (u, v): every channel bilinear. */
  [[nodiscard]] inline std::uint32_t LerpArgb(const std::uint32_t* const col, const float u, const float v) noexcept
  {
    if (col[0] == col[1] && col[1] == col[2] && col[2] == col[3]) {
      return col[0];
    }
    const float b[4] = {(1.0f - u) * (1.0f - v), u * (1.0f - v), u * v, (1.0f - u) * v};
    std::uint32_t out = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
      float acc = 0.0f;
      for (int i = 0; i < 4; ++i) {
        acc += b[i] * static_cast<float>((col[i] >> shift) & 0xFFu);
      }
      out |= static_cast<std::uint32_t>(std::min(255.0f, std::max(0.0f, acc + 0.5f))) << shift;
    }
    return out;
  }

  constexpr int kMaxDepth = 4;
  constexpr float kTolerancePx = 0.75f;

  /**
   * Draws the patch [x0, x1] x [z0, z1] (integer ground coordinates, x1 > x0 or z1 > z0) whose corner projections are `c` and colours `col` (corner order above).
   *   node(x, z, Node&) -> bool   projects the real ground point at integer (x, z); false when it is behind the camera
   *   visible(minX, minY, maxX, maxY) -> bool   false when the screen box is off screen (the patch is dropped before it is cut up)
   *   emit(const Node* quad4, const std::uint32_t* col4)   one finished quad
   * Cuts the patch in two along every side that is at least 2 cells long when a sample (edge midpoints and centre) is off the flat patch by more than `tolPx`, and
   * recurses; stops at `maxDepth`, at single cells, when the budget is spent, and when a sample is behind the camera (the corners are in front: the plain quad is drawn).
   */
  template <class NodeFn, class VisibleFn, class EmitFn>
  void DrapePatch(NodeFn& node, VisibleFn& visible, EmitFn& emit, QuadBudget& budget, const int x0, const int z0, const int x1, const int z1, const Node* const c,
                  const std::uint32_t* const col, const int depth, const int maxDepth = kMaxDepth, const float tolPx = kTolerancePx)
  {
    const int w = x1 - x0, h = z1 - z0;
    const bool splitX = w >= 2, splitZ = h >= 2;
    if ((splitX || splitZ) && depth < maxDepth && budget.committed + (splitX ? 1 : 0) + (splitZ ? 1 : 0) + ((splitX && splitZ) ? 1 : 0) <= budget.cap) {
      const int xm = x0 + w / 2, zm = z0 + h / 2;
      const float um = splitX ? static_cast<float>(xm - x0) / static_cast<float>(w) : 0.5f;
      const float vm = splitZ ? static_cast<float>(zm - z0) / static_cast<float>(h) : 0.5f;
      // nodes: [row][column] over the split lines (3 x 3 at most); the corners are known
      Node g[3][3];
      bool ok = true;
      const int nxs = splitX ? 3 : 2, nzs = splitZ ? 3 : 2;
      const int xs[3] = {x0, splitX ? xm : x1, x1};
      const int zs[3] = {z0, splitZ ? zm : z1, z1};
      Sample samples[5];
      int ns = 0;
      for (int jz = 0; jz < nzs && ok; ++jz) {
        for (int ix = 0; ix < nxs && ok; ++ix) {
          const bool left = ix == 0, right = ix == nxs - 1, top = jz == 0, bottom = jz == nzs - 1;
          if ((left || right) && (top || bottom)) {
            g[jz][ix] = c[top ? (left ? 0 : 1) : (left ? 3 : 2)];
            continue;
          }
          if (!node(xs[ix], zs[jz], g[jz][ix]) || !InFront(g[jz][ix].w)) {
            ok = false;
            break;
          }
          const float u = (nxs == 3) ? (ix == 1 ? um : (ix == 0 ? 0.0f : 1.0f)) : (ix == 0 ? 0.0f : 1.0f);
          const float v = (nzs == 3) ? (jz == 1 ? vm : (jz == 0 ? 0.0f : 1.0f)) : (jz == 0 ? 0.0f : 1.0f);
          samples[ns++] = Sample{u, v, g[jz][ix]};
        }
      }
      if (ok) {
        float minX = c[0].x, maxX = c[0].x, minY = c[0].y, maxY = c[0].y;
        for (int i = 0; i < 4; ++i) {
          minX = std::min(minX, c[i].x); maxX = std::max(maxX, c[i].x);
          minY = std::min(minY, c[i].y); maxY = std::max(maxY, c[i].y);
        }
        for (int i = 0; i < ns; ++i) {
          minX = std::min(minX, samples[i].n.x); maxX = std::max(maxX, samples[i].n.x);
          minY = std::min(minY, samples[i].n.y); maxY = std::max(maxY, samples[i].n.y);
        }
        if (!visible(minX, minY, maxX, maxY)) {
          return;
        }
        if (NeedsSplit(c, samples, ns, tolPx)) {
          const int extra = (splitX ? 1 : 0) + (splitZ ? 1 : 0) + ((splitX && splitZ) ? 1 : 0);
          if (budget.TrySplit(depth, maxDepth, extra)) {
            for (int jz = 0; jz + 1 < nzs; ++jz) {
              for (int ix = 0; ix + 1 < nxs; ++ix) {
                const Node cc[4] = {g[jz][ix], g[jz][ix + 1], g[jz + 1][ix + 1], g[jz + 1][ix]};
                // child colours: the parent's, interpolated at the child's corners
                const float u0 = ix == 0 ? 0.0f : um, u1 = (ix + 1 == nxs - 1) ? 1.0f : um;
                const float v0 = jz == 0 ? 0.0f : vm, v1 = (jz + 1 == nzs - 1) ? 1.0f : vm;
                const std::uint32_t cc4[4] = {LerpArgb(col, u0, v0), LerpArgb(col, u1, v0), LerpArgb(col, u1, v1), LerpArgb(col, u0, v1)};
                DrapePatch(node, visible, emit, budget, xs[ix], zs[jz], xs[ix + 1], zs[jz + 1], cc, cc4, depth + 1, maxDepth, tolPx);
              }
            }
            return;
          }
        }
      }
    }
    emit(c, col);
  }

  /**
   * Draws the straight edge from (ax, az) to (bx, bz) (integer ground positions, one of the two coordinates equal) as a chain of screen segments that follow the real ground:
   * the middle node is projected, and when it is more than `tolPx` away from the perspective correct middle of the chord the edge is cut in two there and both halves are tried
   * the same way (down to single cells, `maxDepth`, the budget). `a` and `b` are the projected ends.
   *   node(x, z, Node&) -> bool   projects the ground point (false: behind the camera)
   *   visible(minX, minY, maxX, maxY) -> bool   false when the screen box of the edge is off screen
   *   emit(const Node& from, const Node& to)   one finished segment
   */
  constexpr int kMaxEdgeDepth = 6;
  constexpr float kEdgeTolerancePx = 0.5f;

  template <class NodeFn, class VisibleFn, class EmitFn>
  void DrapeEdge(NodeFn& node, VisibleFn& visible, EmitFn& emit, QuadBudget& budget, const int ax, const int az, const int bx, const int bz, const Node& a, const Node& b,
                 const int depth, const int maxDepth = kMaxEdgeDepth, const float tolPx = kEdgeTolerancePx)
  {
    const int len = std::max(std::abs(bx - ax), std::abs(bz - az));
    if (len >= 2 && depth < maxDepth) {
      const int mx = ax + (bx - ax) / 2, mz = az + (bz - az) / 2;
      Node m;
      if (node(mx, mz, m) && InFront(m.w)) {
        // where the chord puts the middle (perspective correct: the ends weigh by their clip w), against where the ground does
        const float t = static_cast<float>(std::max(std::abs(mx - ax), std::abs(mz - az))) / static_cast<float>(len);
        const float ka = (1.0f - t) * a.w, kb = t * b.w;
        const float cx = (ka + kb) > 1.0e-12f ? (ka * a.x + kb * b.x) / (ka + kb) : a.x;
        const float cy = (ka + kb) > 1.0e-12f ? (ka * a.y + kb * b.y) / (ka + kb) : a.y;
        const float minX = std::min(std::min(a.x, b.x), m.x), maxX = std::max(std::max(a.x, b.x), m.x);
        const float minY = std::min(std::min(a.y, b.y), m.y), maxY = std::max(std::max(a.y, b.y), m.y);
        if (!visible(minX, minY, maxX, maxY)) {
          return;
        }
        const float dev = std::sqrt((cx - m.x) * (cx - m.x) + (cy - m.y) * (cy - m.y));
        if (dev > tolPx && budget.TrySplit(depth, maxDepth, 1)) {
          DrapeEdge(node, visible, emit, budget, ax, az, mx, mz, a, m, depth + 1, maxDepth, tolPx);
          DrapeEdge(node, visible, emit, budget, mx, mz, bx, bz, m, b, depth + 1, maxDepth, tolPx);
          return;
        }
      }
    }
    emit(a, b);
  }
} // namespace moho::editor::overlaydrape
