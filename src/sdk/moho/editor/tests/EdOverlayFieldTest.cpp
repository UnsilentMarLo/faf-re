// Stand-alone test of the field overlay's pure parts (EdOverlayField.h; no engine needed): value -> alpha mapping (scale / bias / invert / cutoff), the mip chain of
// alpha levels, the per-corner reduction of a leaf (average for masks, maximum for thin features) and the block emptiness test.
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdOverlayFieldTest.cpp -o EdOverlayFieldTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdOverlayFieldTest.cpp
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "moho/editor/EdOverlayField.h"
#include "moho/editor/EdOverlayLod.h"

namespace of = moho::editor::overlayfield;
namespace lod = moho::editor::overlaylod;

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    ++gChecks;                                         \
    if (!(cond)) {                                     \
      ++gFailures;                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static std::uint32_t gSeed = 12345u;
static float Rand01()
{
  gSeed = gSeed * 1664525u + 1013904223u;
  return static_cast<float>((gSeed >> 8) & 0xFFFFu) / 65535.0f;
}

static void TestMapping()
{
  const of::MapOpts def;
  CHECK(of::MapValue(0.0f, def) == 0, "0 draws nothing");
  CHECK(of::MapValue(0.02f, def) == 0, "the cutoff itself draws nothing");
  CHECK(of::MapValue(0.03f, def) == 8, "0.03 -> level 8 (%d)", of::MapValue(0.03f, def));
  CHECK(of::MapValue(1.0f, def) == 255, "1 -> 255");
  CHECK(of::MapValue(7.0f, def) == 255, "clamped above");
  CHECK(of::MapValue(-3.0f, def) == 0, "clamped below");
  CHECK(of::MapValue(std::numeric_limits<float>::quiet_NaN(), def) == 0, "NaN draws nothing");
  of::MapOpts o;
  o.scale = 0.5f;
  CHECK(of::MapValue(1.0f, o) == 128, "scale 0.5: 1 -> 128 (%d)", of::MapValue(1.0f, o));
  o = of::MapOpts{};
  o.scale = 2.0f;
  o.bias = -1.0f;   // a field of 0.5 .. 1 stretched to 0 .. 1
  CHECK(of::MapValue(0.5f, o) == 0, "bias: 0.5 -> 0");
  CHECK(of::MapValue(0.75f, o) == 128, "bias: 0.75 -> 128 (%d)", of::MapValue(0.75f, o));
  CHECK(of::MapValue(1.0f, o) == 255, "bias: 1 -> 255");
  o = of::MapOpts{};
  o.invert = true;
  CHECK(of::MapValue(0.0f, o) == 255, "invert: 0 -> 255");
  CHECK(of::MapValue(1.0f, o) == 0, "invert: 1 -> 0");
  CHECK(of::MapValue(0.25f, o) == 191, "invert: 0.25 -> 191 (%d)", of::MapValue(0.25f, o));
  o = of::MapOpts{};
  o.cutoff = 0.0f;
  CHECK(of::MapValue(0.01f, o) == 3, "cutoff 0: 0.01 -> 3 (%d)", of::MapValue(0.01f, o));
  CHECK(of::MapValue(0.0f, o) == 0, "cutoff 0: zero still draws nothing");
  CHECK(of::MapOpts{} == of::MapOpts{}, "equal options");
  o = of::MapOpts{};
  o.bias = 0.1f;
  CHECK(o != of::MapOpts{}, "different options");

  CHECK(of::AlphaOf(0.0f, 170u) == 0u, "alpha of 0");
  CHECK(of::AlphaOf(255.0f, 170u) == 170u, "alpha of 255 is the maximum");
  CHECK(of::AlphaOf(127.5f, 170u) == 85u, "alpha of 127.5 -> 85 (%u)", of::AlphaOf(127.5f, 170u));
  CHECK(of::AlphaOf(300.0f, 170u) == 170u, "never above the maximum");
  CHECK(of::AlphaOf(-5.0f, 170u) == 0u, "never below zero");
}

static void TestMinLevel()
{
  CHECK(of::MinLevel(1.0f) == 0, "cell 1 -> level 0");
  CHECK(of::MinLevel(1.9f) == 0, "cell 1.9 -> level 0");
  CHECK(of::MinLevel(2.0f) == 1, "cell 2 -> level 1");
  CHECK(of::MinLevel(3.0f) == 1, "cell 3 -> level 1");
  CHECK(of::MinLevel(4.0f) == 2, "cell 4 -> level 2");
  CHECK(of::MinLevel(8.0f) == 3, "cell 8 -> level 3");
  CHECK(of::MinLevel(5000.0f) == of::kRootLevel, "capped at the root level");
  CHECK(of::MinLevel(0.5f) == 0, "a finer field than the map is level 0");
}

static void TestPyramidStructure()
{
  // 5 x 3 field of known levels (cutoff 0, scale 255 maps the values straight to levels)
  of::MapOpts o;
  o.scale = 1.0f / 255.0f;
  o.cutoff = 0.0f;
  const float f[15] = {10, 20, 30, 40, 50,  //
                       60, 70, 80, 90, 100,  //
                       110, 120, 130, 140, 150};
  of::Pyramid py;
  py.Build(f, 5, 3, o);
  CHECK(py.Width() == 5 && py.Height() == 3, "base size");
  CHECK(py.Levels() == 4, "5 x 3 -> 3 x 2 -> 2 x 1 -> 1 x 1: 4 levels (%d)", py.Levels());
  CHECK(py.Sample(3, 1) == 90, "base sample (%d)", py.Sample(3, 1));
  CHECK(py.Texel(1, 0, 0, false) == 40, "level 1 avg (10 20 60 70) = 40 (%d)", py.Texel(1, 0, 0, false));
  CHECK(py.Texel(1, 0, 0, true) == 70, "level 1 max = 70 (%d)", py.Texel(1, 0, 0, true));
  CHECK(py.Texel(1, 2, 0, false) == 75, "level 1 right edge column (50, 100) avg = 75 (%d)", py.Texel(1, 2, 0, false));
  CHECK(py.Texel(1, 1, 1, true) == 140, "level 1 bottom row (130 140) max = 140 (%d)", py.Texel(1, 1, 1, true));
  CHECK(py.Texel(py.Levels() - 1, 0, 0, true) == 150, "top level max is the field max");
  // the top average is the weighted mean of all samples: 80
  CHECK(std::abs(static_cast<int>(py.Texel(py.Levels() - 1, 0, 0, false)) - 80) <= 1, "top average ~ the mean (%d)", py.Texel(py.Levels() - 1, 0, 0, false));

  of::Pyramid empty;
  CHECK(empty.Empty() && empty.Reduce(0, 0, 1, 1, false) == 0.0f && empty.Point(0, 0) == 0.0f, "an empty pyramid answers 0");
  empty.Build(nullptr, 4, 4, o);
  CHECK(empty.Empty(), "no data -> empty");
}

static void TestReduce()
{
  of::MapOpts o;
  o.scale = 1.0f / 255.0f;
  o.cutoff = 0.0f;
  const int W = 37, H = 23;   // not powers of two: partial edge texels
  std::vector<float> f(static_cast<std::size_t>(W) * H);
  for (float& v : f) {
    v = std::floor(Rand01() * 255.0f);
  }
  of::Pyramid py;
  py.Build(f.data(), W, H, o);

  // the whole field: the average is the mean of all samples
  double mean = 0.0;
  float mx = 0.0f;
  for (const float v : f) {
    mean += v;
    mx = std::max(mx, v);
  }
  mean /= static_cast<double>(W * H);
  const float all = py.Reduce(-10, -10, 100, 100, false);
  CHECK(std::abs(all - mean) < 2.0, "average of everything ~ the mean: %.2f vs %.2f", all, mean);
  CHECK(py.Reduce(-10, -10, 100, 100, true) == mx, "maximum of everything");

  // a box exactly around one sample is that sample, at any position
  bool single = true;
  for (int t = 0; t < 40; ++t) {
    const int i = static_cast<int>(Rand01() * (W - 1)), j = static_cast<int>(Rand01() * (H - 1));
    if (py.Reduce(i - 0.5, j - 0.5, i + 0.5, j + 0.5, false) != f[static_cast<std::size_t>(j) * W + i]) single = false;
    if (py.Reduce(i - 0.5, j - 0.5, i + 0.5, j + 0.5, true) != f[static_cast<std::size_t>(j) * W + i]) single = false;
  }
  CHECK(single, "a one sample box returns that sample");

  // aligned power-of-two blocks: the average is the brute force mean (within the rounding of the chain), the maximum is exact
  for (int size : {2, 4, 8}) {
    for (int t = 0; t < 12; ++t) {
      const int bx = static_cast<int>(Rand01() * (W / size - 1)) * size, bz = static_cast<int>(Rand01() * (H / size - 1)) * size;
      double sum = 0.0;
      float m = 0.0f;
      for (int j = bz; j < bz + size; ++j) {
        for (int i = bx; i < bx + size; ++i) {
          sum += f[static_cast<std::size_t>(j) * W + i];
          m = std::max(m, f[static_cast<std::size_t>(j) * W + i]);
        }
      }
      sum /= size * size;
      const float a = py.Reduce(bx - 0.5, bz - 0.5, bx + size - 0.5, bz + size - 0.5, false);
      const float b = py.Reduce(bx - 0.5, bz - 0.5, bx + size - 0.5, bz + size - 0.5, true);
      CHECK(std::abs(a - sum) < 2.0, "aligned %d block average %.2f vs %.2f", size, a, sum);
      CHECK(b == m, "aligned %d block max %.0f vs %.0f", size, b, m);
    }
  }

  // an arbitrary box: the maximum never misses a sample inside it, the average stays between the smallest and the largest sample it touches
  bool conservative = true, bounded = true;
  for (int t = 0; t < 300; ++t) {
    const double x0 = Rand01() * (W - 6) - 2.0, z0 = Rand01() * (H - 6) - 2.0;
    const double x1 = x0 + 0.3 + Rand01() * 14.0, z1 = z0 + 0.3 + Rand01() * 10.0;
    float bm = -1.0f, bmin = 1000.0f;
    for (int j = 0; j < H; ++j) {
      for (int i = 0; i < W; ++i) {
        if (i + 0.5 > x0 && i - 0.5 < x1 && j + 0.5 > z0 && j - 0.5 < z1) {   // the sample's square touches the box
          bm = std::max(bm, f[static_cast<std::size_t>(j) * W + i]);
          bmin = std::min(bmin, f[static_cast<std::size_t>(j) * W + i]);
        }
      }
    }
    if (bm < 0.0f) {
      continue;
    }
    float inside = -1.0f;
    for (int j = 0; j < H; ++j) {
      for (int i = 0; i < W; ++i) {
        if (i >= x0 && i <= x1 && j >= z0 && j <= z1) inside = std::max(inside, f[static_cast<std::size_t>(j) * W + i]);
      }
    }
    if (py.Reduce(x0, z0, x1, z1, true) + 0.01f < inside) conservative = false;
    const float a = py.Reduce(x0, z0, x1, z1, false);
    if (a < bmin - 1.0f || a > bm + 1.0f) bounded = false;
  }
  CHECK(conservative, "the maximum is never below a sample inside the box");
  CHECK(bounded, "the average stays within the samples the box touches");

  // outside the field nothing is drawn; a box that hangs over the edge counts only the part inside (a field of 200s is 200 at its edge, not diluted)
  std::vector<float> flat(16 * 16, 200.0f);
  of::Pyramid fp;
  fp.Build(flat.data(), 16, 16, o);
  CHECK(fp.Reduce(40, 40, 50, 50, false) == 0.0f, "far outside -> 0");
  CHECK(fp.Reduce(-30, -30, -5, -5, true) == 0.0f, "outside (max) -> 0");
  CHECK(std::abs(fp.Reduce(-8, -8, 4, 4, false) - 200.0f) < 0.6f, "hanging over the corner is not diluted: %.2f", fp.Reduce(-8, -8, 4, 4, false));
  CHECK(fp.Point(-0.8, 3) == 0.0f && fp.Point(3, 15.9) == 0.0f, "Point is 0 beyond the extent");
  CHECK(fp.Point(-0.4, 3) == 200.0f && fp.Point(7.3, 7.7) == 200.0f, "Point inside the extent clamps to the edge samples");
  // Point is bilinear between samples
  const float two[2] = {0.0f, 100.0f};
  of::Pyramid tp;
  tp.Build(two, 2, 1, o);
  CHECK(std::abs(tp.Point(0.5, 0.0) - 50.0f) < 0.01f, "bilinear midpoint %.2f", tp.Point(0.5, 0.0));
}

static of::Placement Place(const float ox, const float oz, const float cell, const of::Pyramid& py)
{
  return of::Placement{ox, oz, cell, py.Width(), py.Height()};
}

static void TestLeafs()
{
  of::MapOpts o;   // default cutoff 0.02
  // a 64 x 64 mask: everything 1 in the left half, a single bright sample in the right half
  const int N = 64;
  std::vector<float> f(static_cast<std::size_t>(N) * N, 0.0f);
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < 32; ++i) f[static_cast<std::size_t>(j) * N + i] = 1.0f;
  }
  f[static_cast<std::size_t>(40) * N + 50] = 1.0f;
  of::Pyramid py;
  py.Build(f.data(), N, N, o);
  const of::Placement pl = Place(0, 0, 1, py);
  std::uint32_t a[4];

  // a leaf inside the solid half
  CHECK(of::LeafAlphas(py, pl, 8, 8, 8, 170u, false, a), "solid leaf draws");
  CHECK(a[0] == 170u && a[1] == 170u && a[2] == 170u && a[3] == 170u, "solid leaf: full alpha at all corners (%u %u %u %u)", a[0], a[1], a[2], a[3]);
  // a leaf over empty ground
  CHECK(!of::LeafAlphas(py, pl, 40, 8, 8, 170u, false, a), "empty leaf draws nothing");
  CHECK(a[0] == 0u && a[1] == 0u && a[2] == 0u && a[3] == 0u, "empty leaf: zero alpha");
  // a leaf on the edge of the solid half: soft ramp (corner at x = 32 sees half solid, half empty), corner at x = 24 fully solid, x = 40 empty
  CHECK(of::LeafAlphas(py, pl, 24, 8, 16, 255u, false, a), "edge leaf draws");
  CHECK(a[0] >= 245u && a[3] >= 245u, "edge leaf: the solid side keeps (nearly) full alpha, its box reaches half a sample past the edge (%u)", a[0]);
  CHECK(a[1] == 0u && a[2] == 0u, "edge leaf: the empty side (a box 16 wide centred at x = 40 starts at 32) has none (%u)", a[1]);
  // a corner centred on the edge: half
  CHECK(of::LeafAlphas(py, pl, 16, 8, 16, 255u, false, a), "leaf centred on the edge draws");
  CHECK(a[1] > 100u && a[1] < 155u, "corner centred on the edge is about half (%u)", a[1]);

  // the single bright sample: average at a coarse level is faint, maximum keeps it
  std::uint32_t avgA[4], maxA[4];
  of::LeafAlphas(py, pl, 48, 32, 16, 255u, false, avgA);
  of::LeafAlphas(py, pl, 48, 32, 16, 255u, true, maxA);
  CHECK(maxA[0] == 255u || maxA[1] == 255u || maxA[2] == 255u || maxA[3] == 255u, "max keeps the bright sample on some corner (%u %u %u %u)", maxA[0], maxA[1], maxA[2], maxA[3]);
  CHECK(avgA[0] < 40u && avgA[1] < 40u && avgA[2] < 40u && avgA[3] < 40u, "average dilutes it (%u %u %u %u)", avgA[0], avgA[1], avgA[2], avgA[3]);
  // at level 0 the node is the sample itself
  of::LeafAlphas(py, pl, 49, 39, 1, 255u, false, avgA);
  CHECK(avgA[2] == 255u, "level 0 corner (50, 40) is the sample: %u %u %u %u", avgA[0], avgA[1], avgA[2], avgA[3]);
  CHECK(avgA[0] == 0u && avgA[1] == 0u && avgA[3] == 0u, "its neighbours are empty");

  // neighbouring leaves of one level agree on their shared corners (no seams)
  std::uint32_t l[4], r[4];
  of::LeafAlphas(py, pl, 8, 16, 8, 200u, false, l);
  of::LeafAlphas(py, pl, 16, 16, 8, 200u, false, r);
  CHECK(l[1] == r[0] && l[2] == r[3], "shared corners equal (%u/%u, %u/%u)", l[1], r[0], l[2], r[3]);
  of::LeafAlphas(py, pl, 8, 24, 8, 200u, false, r);
  CHECK(l[3] == r[0] && l[2] == r[1], "shared corners equal below");
}

static void TestPlacementAndEmptiness()
{
  of::MapOpts o;
  // a coarse field: cell 4, origin (8, 8), 8 x 8 samples = covers world 6 .. 38; one bright sample at index (3, 3) = world (20, 20)
  std::vector<float> f(64, 0.0f);
  f[3 * 8 + 3] = 1.0f;
  of::Pyramid py;
  py.Build(f.data(), 8, 8, o);
  const of::Placement pl = Place(8, 8, 4, py);
  CHECK(of::ExtentMinX(pl) == 6.0 && of::ExtentMaxX(pl) == 38.0, "extent %.1f .. %.1f", of::ExtentMinX(pl), of::ExtentMaxX(pl));
  CHECK(of::NodeLevel(py, pl, 20, 20, 1, false) == 255.0f, "a node on the sample");
  CHECK(of::NodeLevel(py, pl, 22, 20, 1, false) == 127.5f, "half way to the next sample: bilinear %.2f", of::NodeLevel(py, pl, 22, 20, 1, false));
  CHECK(of::NodeLevel(py, pl, 100, 100, 1, false) == 0.0f, "far outside the field");
  // a big box around the sample sees it
  CHECK(of::NodeLevel(py, pl, 20, 20, 16, true) == 255.0f, "max over a big box");
  const float avg = of::NodeLevel(py, pl, 20, 20, 16, false);
  CHECK(avg > 5.0f && avg < 40.0f, "average over a 16 x 16 box around one sample of 16 (%.2f)", avg);

  // emptiness: blocks away from the sample (and its half-block margin) are empty
  CHECK(of::BlockMaxLevel(py, pl, 16, 16, 8) > 0.0f, "block around the sample is not empty");
  CHECK(of::BlockMaxLevel(py, pl, 0, 0, 8) == 0.0f, "block far from it is empty");
  CHECK(of::BlockMaxLevel(py, pl, 64, 64, 8) == 0.0f, "block outside the field is empty");
  // the margin: a block just outside the sample's cell still reaches it through its corner boxes (half a block of margin)
  CHECK(of::BlockMaxLevel(py, pl, 24, 16, 8) > 0.0f, "the margin keeps a neighbouring block");
  CHECK(of::BlockMaxLevel(py, pl, 40, 16, 8) == 0.0f, "but not a distant one");
}

static void TestFullMapPipeline()
{
  // scale / bias / invert reach the corner alpha: a height-like field 0.4 .. 0.6 stretched to 0 .. 1
  const int N = 16;
  std::vector<float> f(static_cast<std::size_t>(N) * N);
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) f[static_cast<std::size_t>(j) * N + i] = 0.4f + 0.2f * static_cast<float>(i) / (N - 1);
  }
  of::MapOpts o;
  o.scale = 5.0f;
  o.bias = -2.0f;
  of::Pyramid py;
  py.Build(f.data(), N, N, o);
  const of::Placement pl = Place(0, 0, 1, py);
  std::uint32_t a[4];
  of::LeafAlphas(py, pl, 0, 4, 1, 255u, false, a);
  CHECK(a[0] == 0u, "bias: the lowest column is below the range -> 0 (%u)", a[0]);
  of::LeafAlphas(py, pl, 14, 4, 1, 255u, false, a);
  CHECK(a[1] >= 250u, "the highest column is at full alpha (%u)", a[1]);
  of::MapOpts inv = o;
  inv.invert = true;
  of::Pyramid pi;
  pi.Build(f.data(), N, N, inv);
  std::uint32_t b[4];
  of::LeafAlphas(pi, pl, 0, 4, 1, 255u, false, b);
  CHECK(b[0] >= 250u, "invert: the lowest column is full (%u)", b[0]);
  of::LeafAlphas(pi, pl, 14, 4, 1, 255u, false, b);
  CHECK(b[1] <= 5u, "invert: the highest column is empty (%u)", b[1]);
}


// ---- the quadtree over a field (the same pinhole camera and block probe as EdOverlayLodTest.cpp, plus the emptiness test of EdOverlay.cpp) -----------------------------------
struct Cam
{
  float ex, ey, ez, fx, fy, fz, rx, ry, rz, ux, uy, uz, focal;
  Cam(const float ex_, const float ey_, const float ez_, const float tx, const float ty, const float tz) : ex(ex_), ey(ey_), ez(ez_)
  {
    float dx = tx - ex, dy = ty - ey, dz = tz - ez;
    const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
    fx = dx / l; fy = dy / l; fz = dz / l;
    rx = -fz; ry = 0.0f; rz = fx;
    const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    rx /= rl; rz /= rl;
    ux = ry * fz - rz * fy; uy = rz * fx - rx * fz; uz = rx * fy - ry * fx;
    focal = 360.0f / std::tan(30.0f * 3.14159265f / 180.0f);
  }
  lod::GroundNode Project(const float x, const float y, const float z) const
  {
    const float dx = x - ex, dy = y - ey, dz = z - ez;
    const float w = dx * fx + dy * fy + dz * fz;
    lod::GroundNode n;
    n.w = w;
    if (w > 0.05f) {
      const float sx = dx * rx + dy * ry + dz * rz, sy = dx * ux + dy * uy + dz * uz;
      n.x = 640.0f + focal * sx / w;
      n.y = 360.0f - focal * sy / w;
      n.front = true;
    }
    return n;
  }
};

static lod::NodeInfo ProbeFlat(const Cam& cam, const lod::Key& k, const int mapSize)
{
  const int size = 1 << k.level;
  const int x0 = k.bx << k.level, z0 = k.bz << k.level;
  if (x0 >= mapSize || z0 >= mapSize) return {};
  const int x1 = std::min(x0 + size, mapSize), z1 = std::min(z0 + size, mapSize);
  const lod::GroundNode n[4] = {cam.Project(static_cast<float>(x0), 0, static_cast<float>(z0)), cam.Project(static_cast<float>(x1), 0, static_cast<float>(z0)),
                                cam.Project(static_cast<float>(x1), 0, static_cast<float>(z1)), cam.Project(static_cast<float>(x0), 0, static_cast<float>(z1))};
  int front = 0;
  float wSum = 0;
  for (const auto& g : n) { front += g.front ? 1 : 0; wSum += std::max(0.0f, g.w); }
  if (front == 0) return {};
  if (front < 4) return lod::NodeInfo{true, 1.0e9f, 0.0f};
  const float sx[4] = {n[0].x, n[1].x, n[2].x, n[3].x}, sy[4] = {n[0].y, n[1].y, n[2].y, n[3].y};
  const float minX = std::min(std::min(sx[0], sx[1]), std::min(sx[2], sx[3])), maxX = std::max(std::max(sx[0], sx[1]), std::max(sx[2], sx[3]));
  const float minY = std::min(std::min(sy[0], sy[1]), std::min(sy[2], sy[3])), maxY = std::max(std::max(sy[0], sy[1]), std::max(sy[2], sy[3]));
  const float margin = 0.5f * std::max(maxX - minX, maxY - minY) + 16.0f;
  if (maxX < -margin || maxY < -margin || minX > 1280 + margin || minY > 720 + margin) return {};
  return lod::NodeInfo{true, lod::ProjectedSize(sx, sy), 0.25f * wSum};
}

/** The leaves the engine would build for a field over a flat map (EdOverlay.cpp, the field layers' block). */
static void BuildFieldTree(const Cam& cam, const int mapSize, const of::Pyramid& py, const of::Placement& pl, lod::Tree& tree, const float refinePx = of::kRefinePx)
{
  lod::RefineState st;
  const auto probe = [&](const lod::Key& k) {
    lod::NodeInfo info = ProbeFlat(cam, k, mapSize);
    if (info.visible && of::BlockMaxLevel(py, pl, k.bx << k.level, k.bz << k.level, 1 << k.level) <= 0.0f) info.visible = false;
    return info;
  };
  std::vector<lod::Candidate> roots;
  const int per = (mapSize + 63) / 64;
  for (int bz = 0; bz < per; ++bz) {
    for (int bx = 0; bx < per; ++bx) {
      const lod::Key k{lod::kRootLevel, bx, bz};
      const lod::NodeInfo info = probe(k);
      if (info.visible) roots.push_back(lod::Candidate{k, info});
    }
  }
  lod::TreeParams p;
  p.th = lod::Thresholds{refinePx, refinePx * (of::kMergePx / of::kRefinePx)};
  p.maxLeaves = of::kMaxLeaves;
  p.minLevel = of::MinLevel(pl.cell);
  lod::BuildTree(roots, probe, st, 1, p, tree);
}

static void TestTree()
{
  const int map = 1024;
  of::MapOpts o;
  of::Pyramid full;
  std::vector<float> ones(static_cast<std::size_t>(513) * 513, 1.0f);
  full.Build(ones.data(), 513, 513, o);
  const of::Placement pf = Place(0, 0, 2, full);   // 513 samples, 2 units apart: a 1024 map

  // close to the ground, tilted: near leaves are single field samples, far ones coarse; the budget holds
  {
    const Cam cam(512, 120, 300, 512, 0, 520);
    lod::Tree tree;
    BuildFieldTree(cam, map, full, pf, tree);
    int minL = 99, maxL = -1;
    for (const auto& l : tree.leaves) { minL = std::min(minL, l.key.level); maxL = std::max(maxL, l.key.level); }
    std::printf("  full mask, close oblique camera: %zu leaves, levels %d..%d\n", tree.leaves.size(), minL, maxL);
    CHECK(minL == of::MinLevel(2.0f), "no leaf finer than a field sample (cell 2 -> level %d, got %d)", of::MinLevel(2.0f), minL);
    CHECK(maxL >= 3, "the far ground is coarse (%d)", maxL);
    CHECK(static_cast<int>(tree.leaves.size()) <= of::kMaxLeaves, "within the leaf budget (%zu)", tree.leaves.size());
  }
  // the whole map from high above: about one leaf per 8 pixels, well inside the budget
  {
    const Cam cam(512, 1000, 511, 512, 0, 512);
    lod::Tree tree;
    BuildFieldTree(cam, map, full, pf, tree);
    std::printf("  full mask, whole map from above: %zu leaves\n", tree.leaves.size());
    CHECK(tree.leaves.size() > 500 && static_cast<int>(tree.leaves.size()) <= of::kMaxLeaves, "a plausible leaf count (%zu)", tree.leaves.size());
  }
  // a small blob on a big map: the empty ground is not cut up
  {
    std::vector<float> blob(static_cast<std::size_t>(513) * 513, 0.0f);
    for (int j = 100; j < 120; ++j) for (int i = 100; i < 120; ++i) blob[static_cast<std::size_t>(j) * 513 + i] = 1.0f;
    of::Pyramid bp;
    bp.Build(blob.data(), 513, 513, o);
    const Cam cam(512, 1000, 511, 512, 0, 512);
    lod::Tree tree;
    BuildFieldTree(cam, map, bp, pf, tree);
    std::printf("  one 40 x 40 blob, whole map from above: %zu leaves\n", tree.leaves.size());
    CHECK(tree.leaves.size() < 400, "only the blob is cut up (%zu leaves)", tree.leaves.size());
    bool covers = false;
    for (const auto& l : tree.leaves) {
      const int s = 1 << l.key.level, x0 = l.key.bx << l.key.level, z0 = l.key.bz << l.key.level;
      if (x0 <= 210 && x0 + s >= 210 && z0 <= 210 && z0 + s >= 210) covers = true;
    }
    CHECK(covers, "a leaf covers the blob");
    // the leaf over the blob draws (leaves around it may be too faint to round to a level: they draw nothing, which is correct)
    bool drawn = false;
    for (const auto& l : tree.leaves) {
      const int sz = 1 << l.key.level, x0 = l.key.bx << l.key.level, z0 = l.key.bz << l.key.level;
      std::uint32_t a[4];
      if (x0 <= 210 && x0 + sz >= 210 && z0 <= 210 && z0 + sz >= 210 && of::LeafAlphas(bp, pf, x0, z0, sz, 170u, false, a)) drawn = true;
    }
    CHECK(drawn, "the leaf over the blob has alpha");
  }
}

int main()
{
  TestTree();
  TestMapping();
  TestMinLevel();
  TestPyramidStructure();
  TestReduce();
  TestLeafs();
  TestPlacementAndEmptiness();
  TestFullMapPipeline();
  std::printf("EdOverlayFieldTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
