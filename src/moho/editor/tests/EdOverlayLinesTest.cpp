// Stand-alone test of the overlay's line quads (EdOverlayLines.h; no engine, no GPU needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdOverlayLinesTest.cpp -o EdOverlayLinesTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src /fp:precise EdOverlayLinesTest.cpp
//
// The reference below is the previous implementation of EdOverlay.cpp (DrawLine, called once for the halo and once for the line, five vector allocations per call, drawing
// straight into the batcher). The new code projects once, emits both passes from that projection and appends to a list: the quads must be bit for bit the same, in the
// same order, for any line and camera. Also checked: reusing the scratch buffers across lines, the ed_overlay_stats numbers.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "moho/editor/EdOverlayLines.h"

namespace ol = moho::editor::overlaylines;
namespace od = moho::editor::overlaydrape;

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    ++gChecks;                                         \
    if (!(cond)) {                                     \
      if (++gFailures < 30) {                          \
        std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
      }                                                \
    }                                                  \
  } while (0)

struct Vertex
{
  float mX, mY, mZ;
  std::uint32_t mColor;
  float mU, mV;
};

// A pinhole camera looking at a target; w = distance along the view direction. Screen = pixels of a vw x vh viewport.
struct Cam
{
  float ex, ey, ez, fx, fy, fz, rx, ry, rz, ux, uy, uz, focal, vw, vh;
  Cam(const float ex_, const float ey_, const float ez_, const float tx, const float ty, const float tz, const float vw_, const float vh_, const float fovDeg)
    : ex(ex_), ey(ey_), ez(ez_), vw(vw_), vh(vh_)
  {
    float dx = tx - ex, dy = ty - ey, dz = tz - ez;
    const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
    fx = dx / l; fy = dy / l; fz = dz / l;
    rx = fy * 0.0f - fz * 1.0f; ry = fz * 0.0f - fx * 0.0f; rz = fx * 1.0f - fy * 0.0f;
    const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    rx /= rl; ry /= rl; rz /= rl;
    ux = ry * fz - rz * fy; uy = rz * fx - rx * fz; uz = rx * fy - ry * fx;
    focal = (vh * 0.5f) / std::tan(fovDeg * 0.5f * 3.14159265f / 180.0f);
  }
  float W(const float x, const float y, const float z) const { return (x - ex) * fx + (y - ey) * fy + (z - ez) * fz; }
  void Screen(const float x, const float y, const float z, float& sx, float& sy) const
  {
    const float dx = x - ex, dy = y - ey, dz = z - ez;
    const float w = dx * fx + dy * fy + dz * fz;
    sx = vw * 0.5f + focal * (dx * rx + dy * ry + dz * rz) / w;
    sy = vh * 0.5f - focal * (dx * ux + dy * uy + dz * uz) / w;
  }
};

// ---- the previous implementation ---------------------------------------------------------------------------------------------------------
namespace ref
{
  struct Pt { float x, y, depth; };
  struct Batcher
  {
    std::vector<Vertex> q;
    void DrawQuad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) { q.push_back(a); q.push_back(b); q.push_back(c); q.push_back(d); }
  };

  void Emit(Batcher& b, float x0, float y0, float h0, float x1, float y1, float h1, std::uint32_t color)
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
    using V = Vertex;
    const V a{x0 + nx * h0, y0 + ny * h0, 0.0f, color, 0.0f, 0.0f};
    const V c{x0 - nx * h0, y0 - ny * h0, 0.0f, color, 0.0f, 1.0f};
    const V d{x1 - nx * h1, y1 - ny * h1, 0.0f, color, 1.0f, 1.0f};
    const V e{x1 + nx * h1, y1 + ny * h1, 0.0f, color, 1.0f, 0.0f};
    b.DrawQuad(a, c, d, e);
  }

  void EmitCorners(Batcher& b, const Pt& l0, const Pt& r0, const Pt& r1, const Pt& l1, std::uint32_t color)
  {
    using V = Vertex;
    b.DrawQuad(V{l0.x, l0.y, 0.0f, color, 0.0f, 0.0f}, V{r0.x, r0.y, 0.0f, color, 0.0f, 1.0f}, V{r1.x, r1.y, 0.0f, color, 1.0f, 1.0f}, V{l1.x, l1.y, 0.0f, color, 1.0f, 0.0f});
  }

  void Cap(Batcher& b, float x, float y, float h, std::uint32_t color)
  {
    using V = Vertex;
    b.DrawQuad(V{x - h, y - h, 0.0f, color, 0.0f, 0.0f}, V{x + h, y - h, 0.0f, color, 1.0f, 0.0f},
               V{x + h, y + h, 0.0f, color, 1.0f, 1.0f}, V{x - h, y + h, 0.0f, color, 0.0f, 1.0f});
  }

  float gDbg[8] = {};

  void DrawLine(const Cam& cam, Batcher& b, const ol::Polyline& pl, float vw, float vh, bool halo)
  {
    const std::size_t n = pl.pts.size() / 3u;
    if (n < 2u) {
      return;
    }
    const std::size_t segs = pl.closed ? n : n - 1u;

    std::vector<Pt> sp(n);
    std::vector<float> half(n);
    std::vector<Pt> cl(n), cr(n);
    std::vector<char> cornersOk(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
      float sx, sy;
      cam.Screen(pl.pts[i * 3], pl.pts[i * 3 + 1], pl.pts[i * 3 + 2], sx, sy);
      sp[i] = {sx, sy, cam.W(pl.pts[i * 3], pl.pts[i * 3 + 1], pl.pts[i * 3 + 2])};

      const std::size_t a = (i == 0u) ? (pl.closed ? n - 1u : 0u) : i - 1u;
      const std::size_t c = (i + 1u < n) ? i + 1u : (pl.closed ? 0u : n - 1u);
      const float dx = pl.pts[c * 3] - pl.pts[a * 3];
      const float dz = pl.pts[c * 3 + 2] - pl.pts[a * 3 + 2];
      const float l = std::sqrt(dx * dx + dz * dz);
      float px = 0.0f;
      if (l > 0.0001f) {
        const float hw = pl.worldWidth * 0.5f;
        const float ox = dz / l * hw;
        const float oz = -dx / l * hw;
        float s1x, s1y, s2x, s2y;
        cam.Screen(pl.pts[i * 3] + ox, pl.pts[i * 3 + 1], pl.pts[i * 3 + 2] + oz, s1x, s1y);
        cam.Screen(pl.pts[i * 3] - ox, pl.pts[i * 3 + 1], pl.pts[i * 3 + 2] - oz, s2x, s2y);
        px = 0.5f * std::sqrt((s1x - s2x) * (s1x - s2x) + (s1y - s2y) * (s1y - s2y));
        const float want = std::min(pl.maxPx * 0.5f, std::max(pl.minPx * 0.5f, px)) + (halo ? 1.0f : 0.0f);
        const float ax = 0.5f * (s1x - s2x), ay = 0.5f * (s1y - s2y);
        const float mx = 0.5f * (s1x + s2x), my = 0.5f * (s1y + s2y);
        if (px >= pl.minPx * 0.75f && px <= pl.maxPx * 0.5f) {
          const float k = want / px;
          cl[i] = {mx + ax * k, my + ay * k, 0.0f};
          cr[i] = {mx - ax * k, my - ay * k, 0.0f};
          cornersOk[i] = 1;
        }
      }
      half[i] = std::min(pl.maxPx * 0.5f, std::max(pl.minPx * 0.5f, px)) + (halo ? 1.0f : 0.0f);
    }

    if (!halo) {
      gDbg[0] = sp[0].x; gDbg[1] = sp[0].y; gDbg[2] = sp[0].depth; gDbg[3] = half[0]; gDbg[4] = vw; gDbg[5] = vh;
      gDbg[6] = 0.0f; gDbg[7] = 0.0f;
    }
    const std::uint32_t color = halo ? 0xB0000000u : pl.argb;
    for (std::size_t s = 0; s < segs; ++s) {
      const std::size_t i0 = s;
      const std::size_t i1 = (s + 1u) % n;
      const Pt& a = sp[i0];
      const Pt& c = sp[i1];
      if (!od::InFront(a.depth) || !od::InFront(c.depth)) {
        if (!halo) gDbg[7] += 1.0f;
        continue;
      }
      if ((a.x < -64.0f && c.x < -64.0f) || (a.x > vw + 64.0f && c.x > vw + 64.0f) ||
          (a.y < -64.0f && c.y < -64.0f) || (a.y > vh + 64.0f && c.y > vh + 64.0f)) {
        continue;
      }
      if (!halo) gDbg[6] += 1.0f;
      if (cornersOk[i0] && cornersOk[i1]) {
        EmitCorners(b, cl[i0], cr[i0], cr[i1], cl[i1], color);
      } else {
        Emit(b, a.x, a.y, half[i0], c.x, c.y, half[i1], color);
      }
    }
    if (pl.caps) {
      for (std::size_t i = 0; i < n; ++i) {
        if (od::InFront(sp[i].depth)) {
          Cap(b, sp[i].x, sp[i].y, half[i], color);
        }
      }
    }
  }
} // namespace ref

// ---- test data ----------------------------------------------------------------------------------------------------------------------------
static std::uint32_t gSeed = 12345u;
static float Rnd()
{
  gSeed = gSeed * 1664525u + 1013904223u;
  return static_cast<float>((gSeed >> 8) & 0xFFFFFFu) / 16777216.0f;
}

static ol::Polyline MakeLine(const int kind)
{
  ol::Polyline pl;
  const int n = (kind % 11 == 0) ? 1 : (kind % 7 == 0 ? 2 : 2 + static_cast<int>(Rnd() * 14.0f));
  const float cx = 100.0f + Rnd() * 800.0f, cz = 100.0f + Rnd() * 800.0f;
  for (int i = 0; i < n; ++i) {
    float x, y, z;
    if (kind % 5 == 0) {              // a ring
      const float a = static_cast<float>(i) / static_cast<float>(n) * 6.2831853f;
      x = cx + std::cos(a) * 6.0f; z = cz + std::sin(a) * 6.0f;
    } else if (kind % 13 == 0 && i > 0) {   // a coincident vertex (no direction)
      x = pl.pts[(i - 1) * 3]; z = pl.pts[(i - 1) * 3 + 2];
    } else {
      x = cx + (Rnd() - 0.5f) * 60.0f; z = cz + (Rnd() - 0.5f) * 60.0f;
    }
    y = 5.0f + Rnd() * 40.0f;
    pl.pts.push_back(x); pl.pts.push_back(y); pl.pts.push_back(z);
  }
  pl.argb = 0x80000000u | static_cast<std::uint32_t>(Rnd() * 16777215.0f);
  pl.worldWidth = (kind % 3 == 0) ? 0.05f : 0.1f + Rnd() * 3.0f;
  pl.minPx = (kind % 4 == 0) ? 1.0f : 2.0f;
  pl.maxPx = 3.0f + Rnd() * 20.0f;
  pl.closed = (kind % 3 == 1);
  pl.halo = (kind % 6 != 0);
  pl.caps = (kind % 5 != 4);
  return pl;
}

static bool SameQuads(const std::vector<Vertex>& a, const std::vector<Vertex>& b)
{
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vertex)) == 0);
}

struct Proj
{
  const Cam& cam;
  void Screen(const float x, const float y, const float z, float& sx, float& sy) const { cam.Screen(x, y, z, sx, sy); }
  float W(const float x, const float y, const float z) const { return cam.W(x, y, z); }
};

static void TestAgainstReference()
{
  const Cam cams[] = {
    Cam(500, 200, 300, 520, 0, 520, 1280, 720, 55),      // high, looking down
    Cam(300, 30, 300, 700, 10, 700, 1920, 1080, 60),     // low, looking along the ground (lines behind and beside the camera)
    Cam(520, 60, 520, 520, 0, 521, 800, 600, 40),        // straight down
    Cam(900, 120, 100, 100, 5, 900, 2560, 1440, 70),
  };
  ol::LineScratch scratch;      // one scratch for everything: leftovers of a longer line must not leak into a shorter one
  int lines = 0, quadsTotal = 0;
  for (const Cam& cam : cams) {
    const Proj proj{cam};
    for (int kind = 0; kind < 400; ++kind) {
      const ol::Polyline pl = MakeLine(kind);
      ++lines;
      ref::Batcher refHalo, refLine;
      ref::DrawLine(cam, refHalo, pl, cam.vw, cam.vh, true);
      const float refDbgBefore[8] = {ref::gDbg[0], ref::gDbg[1], ref::gDbg[2], ref::gDbg[3], ref::gDbg[4], ref::gDbg[5], ref::gDbg[6], ref::gDbg[7]};
      (void)refDbgBefore;
      ref::DrawLine(cam, refLine, pl, cam.vw, cam.vh, false);

      std::vector<Vertex> newHalo, newLine;
      float dbg[8] = {};
      if (pl.pts.size() / 3u >= 2u) {
        ol::ProjectLine(proj, pl, scratch.v);
        if (pl.halo) {
          ol::EmitLine(pl, scratch, cam.vw, cam.vh, true, newHalo, nullptr);
        }
        ol::EmitLine(pl, scratch, cam.vw, cam.vh, false, newLine, dbg);
        CHECK(std::memcmp(dbg, ref::gDbg, sizeof(dbg)) == 0, "stats of line %d differ", kind);
      }
      if (!pl.halo) {
        refHalo.q.clear();      // the previous code drew the halo pass only for lines that have a halo
      }
      CHECK(SameQuads(refHalo.q, newHalo), "halo quads of line %d (%zu points) differ: %zu vs %zu vertices", kind, pl.pts.size() / 3u, refHalo.q.size(), newHalo.size());
      CHECK(SameQuads(refLine.q, newLine), "line quads of line %d (%zu points) differ: %zu vs %zu vertices", kind, pl.pts.size() / 3u, refLine.q.size(), newLine.size());
      quadsTotal += static_cast<int>(newHalo.size() + newLine.size()) / 4;
    }
  }
  std::printf("  %d lines, %d quads compared bit for bit\n", lines, quadsTotal);
  CHECK(quadsTotal > 20000, "the test scene drew only %d quads: the cameras do not see the lines", quadsTotal);
}

static void TestEdgeCases()
{
  const Cam cam(500, 200, 300, 520, 0, 520, 1280, 720, 55);
  const Proj proj{cam};
  ol::LineScratch scratch;
  // fewer than two points: nothing
  ol::Polyline one;
  one.pts = {520, 10, 520};
  std::vector<Vertex> q;
  if (one.pts.size() / 3u >= 2u) { ol::ProjectLine(proj, one, scratch.v); }
  CHECK(q.empty(), "a single point draws nothing");
  // two coincident points: no direction, no ribbon (Emit drops a zero length segment), caps only
  ol::Polyline same;
  same.pts = {520, 10, 520, 520, 10, 520};
  ol::ProjectLine(proj, same, scratch.v);
  ol::EmitLine(same, scratch, cam.vw, cam.vh, false, q, nullptr);
  CHECK(q.size() == 8u, "two coincident points make two caps, got %zu vertices", q.size());
  // a line wholly behind the camera: nothing
  ol::Polyline behind;
  behind.pts = {500, 300, 100, 510, 300, 90};
  q.clear();
  ol::ProjectLine(proj, behind, scratch.v);
  float dbg[8] = {};
  ol::EmitLine(behind, scratch, cam.vw, cam.vh, false, q, dbg);
  CHECK(q.empty() && dbg[7] == 1.0f && dbg[6] == 0.0f, "a line behind the camera draws nothing and counts one culled segment (%zu, %g, %g)", q.size(), dbg[6], dbg[7]);
}

int main()
{
  std::printf("EdOverlayLinesTest\n");
  TestAgainstReference();
  TestEdgeCases();
  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
