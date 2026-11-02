// EDITOR: the screen quads of the overlay's lines (EdOverlay.cpp: marker chains, paths, rings, grid lines), engine independent so they can be tested on their own
// (moho/editor/tests/EdOverlayLinesTest.cpp).
//
// A line is a polyline in world space with a world width, drawn as a ribbon between projected ground corners, clamped to a pixel range, with a dark halo under it. The
// halo and the line are the same ribbon (the halo is a pixel wider on each side): the projection of the vertices (`ProjectLine`) is done once and both passes
// (`EmitLine`) work from it. The quads are appended to a caller's list (four vertices per quad, in CD3DPrimBatcher::DrawQuad's argument order) so they can be kept and
// replayed while the camera and the lines do not change. Everything is a template over the vertex type (aggregate {x, y, z, color, u, v}) and a projector:
//
//   struct Projector {
//     void  Screen(float x, float y, float z, float& sx, float& sy) const;   // screen position (GeomCamera3::Project)
//     float W(float x, float y, float z) const;                              // clip space w: > 0 in front of the camera (overlaydrape::InFront)
//   };
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "moho/editor/EdOverlayDrape.h"

namespace moho::editor::overlaylines
{
  struct Polyline
  {
    std::vector<float> pts;     // x, y, z triples
    std::uint32_t argb = 0xFFFFFFFFu;
    float worldWidth = 0.6f;    // ribbon width on the ground in world units
    float minPx = 2.0f;         // never thinner than this on screen
    float maxPx = 14.0f;        // never thicker than this on screen
    bool closed = false;
    bool halo = true;           // dark underlay for contrast on any terrain
    bool caps = true;           // squares at the vertices that join the ribbons (off for thin grid lines)
  };

  struct Pt
  {
    float x, y;
    float depth;
  };

  // What the projection of one vertex gives, independent of the halo: screen position and depth of the vertex, the width of a horizontal ribbon across it on the screen
  // (`px`) and the projected middle / half span of that ribbon (`mx`, `my`, `ax`, `ay`). `side` is false for a vertex whose neighbours coincide (no direction).
  struct LineVertex
  {
    Pt sp;
    float px;
    float ax, ay, mx, my;
    bool side;
  };

  // Buffers kept between lines and frames (no allocation per line once they have grown).
  struct LineScratch
  {
    std::vector<LineVertex> v;
    std::vector<float> half;
    std::vector<Pt> cl, cr;     // the projected ground corners left / right of every vertex, already clamped to the pixel limits and grown for the halo
    std::vector<char> cornersOk;
  };

  template <class V>
  inline void PushQuad(std::vector<V>& q, const V& a, const V& b, const V& c, const V& d)
  {
    q.push_back(a);
    q.push_back(b);
    q.push_back(c);
    q.push_back(d);
  }

  template <class V>
  inline void Emit(std::vector<V>& q, const float x0, const float y0, const float h0, const float x1, const float y1, const float h1, const std::uint32_t color)
  {
    float dx = x1 - x0;
    float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) {
      return;
    }
    dx /= len;
    dy /= len;
    const float nx = -dy;
    const float ny = dx;
    const V a{x0 + nx * h0, y0 + ny * h0, 0.0f, color, 0.0f, 0.0f};
    const V c{x0 - nx * h0, y0 - ny * h0, 0.0f, color, 0.0f, 1.0f};
    const V d{x1 - nx * h1, y1 - ny * h1, 0.0f, color, 1.0f, 1.0f};
    const V e{x1 + nx * h1, y1 + ny * h1, 0.0f, color, 1.0f, 0.0f};
    PushQuad(q, a, c, d, e);
  }

  // A ribbon segment from the four screen positions of its ground corners (left / right of each end): a ribbon wider than a line is a rectangle on the GROUND, which on the
  // screen is a parallelogram, not a rectangle across the screen direction of the spine (that one stood up like a plane when the camera looked along a diagonal).
  template <class V>
  inline void EmitCorners(std::vector<V>& q, const Pt& l0, const Pt& r0, const Pt& r1, const Pt& l1, const std::uint32_t color)
  {
    PushQuad(q, V{l0.x, l0.y, 0.0f, color, 0.0f, 0.0f}, V{r0.x, r0.y, 0.0f, color, 0.0f, 1.0f}, V{r1.x, r1.y, 0.0f, color, 1.0f, 1.0f}, V{l1.x, l1.y, 0.0f, color, 1.0f, 0.0f});
  }

  template <class V>
  inline void Cap(std::vector<V>& q, const float x, const float y, const float h, const std::uint32_t color)
  {
    PushQuad(q, V{x - h, y - h, 0.0f, color, 0.0f, 0.0f}, V{x + h, y - h, 0.0f, color, 1.0f, 0.0f},
             V{x + h, y + h, 0.0f, color, 1.0f, 1.0f}, V{x - h, y + h, 0.0f, color, 0.0f, 1.0f});
  }

  template <class Projector>
  inline void ProjectLine(const Projector& proj, const Polyline& pl, std::vector<LineVertex>& out)
  {
    const std::size_t n = pl.pts.size() / 3u;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      LineVertex& lv = out[i];
      const float px0 = pl.pts[i * 3], py0 = pl.pts[i * 3 + 1], pz0 = pl.pts[i * 3 + 2];
      float sx = 0.0f, sy = 0.0f;
      proj.Screen(px0, py0, pz0, sx, sy);
      lv.sp = {sx, sy, proj.W(px0, py0, pz0)};
      lv.px = 0.0f;
      lv.ax = lv.ay = lv.mx = lv.my = 0.0f;
      lv.side = false;

      // width of a horizontal ribbon of `worldWidth` across this vertex, measured on screen
      const std::size_t a = (i == 0u) ? (pl.closed ? n - 1u : 0u) : i - 1u;
      const std::size_t c = (i + 1u < n) ? i + 1u : (pl.closed ? 0u : n - 1u);
      const float dx = pl.pts[c * 3] - pl.pts[a * 3];
      const float dz = pl.pts[c * 3 + 2] - pl.pts[a * 3 + 2];
      const float l = std::sqrt(dx * dx + dz * dz);
      if (l > 0.0001f) {
        const float hw = pl.worldWidth * 0.5f;
        const float ox = dz / l * hw;
        const float oz = -dx / l * hw;
        float s1x = 0.0f, s1y = 0.0f, s2x = 0.0f, s2y = 0.0f;
        proj.Screen(px0 + ox, py0, pz0 + oz, s1x, s1y);
        proj.Screen(px0 - ox, py0, pz0 - oz, s2x, s2y);
        lv.px = 0.5f * std::sqrt((s1x - s2x) * (s1x - s2x) + (s1y - s2y) * (s1y - s2y));
        lv.ax = 0.5f * (s1x - s2x);        // half of the way from the right to the left corner
        lv.ay = 0.5f * (s1y - s2y);
        lv.mx = 0.5f * (s1x + s2x);        // the middle of the ribbon across this vertex on the screen
        lv.my = 0.5f * (s1y + s2y);
        lv.side = true;
      }
    }
  }

  // The quads of one pass (halo or line) of an already projected line (`sc.v`). `dbg` (line pass only) receives ed_overlay_stats' numbers for this line: first vertex
  // (screen x, y, depth), its half width, viewport w, h, segments drawn, segments behind the camera.
  template <class V>
  inline void EmitLine(const Polyline& pl, LineScratch& sc, const float vw, const float vh, const bool halo, std::vector<V>& q, float* const dbg)
  {
    const std::vector<LineVertex>& v = sc.v;
    const std::size_t n = v.size();
    const std::size_t segs = pl.closed ? n : n - 1u;
    sc.half.resize(n);
    sc.cl.resize(n);
    sc.cr.resize(n);
    sc.cornersOk.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
      const LineVertex& lv = v[i];
      // the corners: from the middle of the vertex to each side, stretched or shrunk to the pixel limits (the halo is a pixel wider on each side)
      const float want = std::min(pl.maxPx * 0.5f, std::max(pl.minPx * 0.5f, lv.px)) + (halo ? 1.0f : 0.0f);
      if (lv.side && lv.px >= pl.minPx * 0.75f && lv.px <= pl.maxPx * 0.5f) {          // a ribbon the pixel limits do not touch is drawn as the ground rectangle it is; a thin line keeps its pixel width across
        const float k = want / lv.px;
        sc.cl[i] = {lv.mx + lv.ax * k, lv.my + lv.ay * k, 0.0f};
        sc.cr[i] = {lv.mx - lv.ax * k, lv.my - lv.ay * k, 0.0f};
        sc.cornersOk[i] = 1;
      }
      sc.half[i] = std::min(pl.maxPx * 0.5f, std::max(pl.minPx * 0.5f, lv.px)) + (halo ? 1.0f : 0.0f);
    }

    if (dbg != nullptr) {
      dbg[0] = v[0].sp.x; dbg[1] = v[0].sp.y; dbg[2] = v[0].sp.depth; dbg[3] = sc.half[0]; dbg[4] = vw; dbg[5] = vh;
      dbg[6] = 0.0f; dbg[7] = 0.0f;
    }
    const std::uint32_t color = halo ? 0xB0000000u : pl.argb;
    for (std::size_t s = 0; s < segs; ++s) {
      const std::size_t i0 = s;
      const std::size_t i1 = (s + 1u) % n;
      const Pt& a = v[i0].sp;
      const Pt& c = v[i1].sp;
      if (!overlaydrape::InFront(a.depth) || !overlaydrape::InFront(c.depth)) {
        if (dbg != nullptr) dbg[7] += 1.0f;
        continue;
      }
      if ((a.x < -64.0f && c.x < -64.0f) || (a.x > vw + 64.0f && c.x > vw + 64.0f) ||
          (a.y < -64.0f && c.y < -64.0f) || (a.y > vh + 64.0f && c.y > vh + 64.0f)) {
        continue;
      }
      if (dbg != nullptr) dbg[6] += 1.0f;
      if (sc.cornersOk[i0] && sc.cornersOk[i1]) {
        EmitCorners(q, sc.cl[i0], sc.cr[i0], sc.cr[i1], sc.cl[i1], color);
      } else {
        Emit(q, a.x, a.y, sc.half[i0], c.x, c.y, sc.half[i1], color);
      }
    }
    if (pl.caps) {
      for (std::size_t i = 0; i < n; ++i) {
        if (overlaydrape::InFront(v[i].sp.depth)) {
          Cap(q, v[i].sp.x, v[i].sp.y, sc.half[i], color);   // joins the ribbons at the vertices
        }
      }
    }
  }
} // namespace moho::editor::overlaylines
