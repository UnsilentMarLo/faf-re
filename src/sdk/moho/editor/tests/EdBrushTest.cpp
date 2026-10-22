// Stand-alone test of the native terrain brush dab (EdBrush.h; no engine needed). It links MapDocument.cpp to check the write path too.
//   g++ -std=c++20 -O2 -ffp-contract=off -I src/sdk src/sdk/moho/editor/tests/EdBrushTest.cpp src/sdk/moho/editor/MapDocument.cpp -o EdBrushTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdBrushTest.cpp MapDocument.cpp
//
// Three references:
//   1. LuaRef: a line-by-line transliteration of terrain.lua DabOne / Weight / Noise / BoxBlur (1-based tables, the same expressions) run in
//      double with every Lua operation rounded to a float, as the game's float Lua does. The native must equal it word for word.
//   2. DoubleRef: the same formulas in plain double without the roundings; the native may differ by a word or two (rounding at the .5 edges, the
//      running sums of the blur).
//   3. The document: a dab written through MapDocument::EditHeights changes exactly the samples the math says, is one undo step per stroke and
//      undo restores the words.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "moho/editor/EdBrush.h"
#include "moho/editor/MapDocument.h"

namespace br = moho::editor::brush;
using moho::editor::CellRect;
using moho::editor::MapDocument;

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

// ---- the Lua VM's arithmetic: every operation result is a float ------------------------------------------------------------------------

static double F(const double v)
{
  return static_cast<double>(static_cast<float>(v));
}
static double Add(const double a, const double b) { return F(a + b); }
static double Sub(const double a, const double b) { return F(a - b); }
static double Mul(const double a, const double b) { return F(a * b); }
static double Div(const double a, const double b) { return F(a / b); }
static double Mod(const double a, const double b) { return F(std::fmod(a, b)); }
static double Sqrt(const double a) { return F(std::sqrt(a)); }
static double Cos(const double a) { return F(std::cos(a)); }
static double Floor(const double a) { return F(std::floor(a)); }
static double Min(const double a, const double b) { return a < b ? a : b; }
static double Max(const double a, const double b) { return a > b ? a : b; }

// ---- reference 1: terrain.lua --------------------------------------------------------------------------------------------------------

struct LuaBrush
{
  double radius, falloff, strength;
  int dir;
  const char* mode;
  double level;   // < 0: nil
  int strokeId;
  double noiseScale = 8, noiseRough = 0.5;
  int noiseOctaves = 2;
  const br::Shape* shape = nullptr;   // a shaped brush (brushes.lua Active ~= 'round'): the mask replaces the falloff profile
  double angle = 0;                   // degrees: brushes.lua Angle + the stroke's random turn
};

static double LuaWeight(const double distance, const double radius, const double falloff)
{
  if (distance >= radius) {
    return 0;
  }
  double f = falloff;
  if (f < 0) f = 0; else if (f > 1) f = 1;
  const double width = Min(radius, Max(Mul(radius, f), 1));
  const double inner = Sub(radius, width);
  if (distance <= inner) {
    return 1;
  }
  const double PI = F(3.14159265);
  return Mul(0.5, Add(1, Cos(Div(Mul(PI, Sub(distance, inner)), width))));
}

// brushes.lua Weight() for a shape grid (the mask at the offset (dx, dz) of a brush of radius r, turned by `angle` degrees), operation by operation as the Lua source:
//   local a = math.rad(angle); local c, s = math.cos(a), math.sin(a)
//   local u = (dx * c + dz * s) / r;  local v = (-dx * s + dz * c) / r
//   if u * u + v * v > 1.6 then return 0 end
//   ImageSample(m, u, v): bilinear on the n x n grid over -1..1, 0 outside it
static double LuaShapedWeight(const br::Shape& shape, const double dx, const double dz, const double r, const double angle)
{
  const double a = F(angle * (3.14159265358979323846 / 180.0));
  const double c = Cos(a), s = F(std::sin(a));
  const double u = Div(Add(Mul(dx, c), Mul(dz, s)), r);
  const double v = Div(Add(Mul(-dx, s), Mul(dz, c)), r);
  if (Add(Mul(u, u), Mul(v, v)) > F(1.6)) {
    return 0;
  }
  const int n = shape.n;
  const double fx = Mul(Mul(Add(u, 1), 0.5), n - 1), fz = Mul(Mul(Add(v, 1), 0.5), n - 1);
  if (fx < 0 || fz < 0 || fx > n - 1 || fz > n - 1) {
    return 0;
  }
  const int x0 = static_cast<int>(Min(n - 2, Floor(fx))), z0 = static_cast<int>(Min(n - 2, Floor(fz)));
  const double tx = Sub(fx, x0), tz = Sub(fz, z0);
  auto at = [&](const int x, const int z) { return static_cast<double>(shape.values[static_cast<std::size_t>(z) * n + x]); };   // m.values[z * n + x + 1]
  const double top = Add(Mul(at(x0, z0), Sub(1, tx)), Mul(at(x0 + 1, z0), tx));
  const double bottom = Add(Mul(at(x0, z0 + 1), Sub(1, tx)), Mul(at(x0 + 1, z0 + 1), tx));
  return Add(Mul(top, Sub(1, tz)), Mul(bottom, tz));
}

static double LuaNoise(const double x, const double z, const double seed)
{
  // terrain.lua: local a = math.mod(math.mod(x, 4093) * 1103 + 17, 4093) ...
  const double ax = Mod(Add(Mul(Mod(x, 4093), 1103), 17), 4093);
  const double bz = Mod(Add(Mul(Mod(z, 4091), 1999), 31), 4091);
  const double cs = Mod(Add(Mul(Mod(seed, 4079), 997), 5), 4079);
  double n = Mod(Add(Mul(ax, 4001), bz), 4093);
  n = Mod(Add(Mul(n, n), cs), 4093);
  n = Mod(Add(Mul(n, n), ax), 4093);
  n = Mod(Add(Mul(n, n), bz), 4093);
  return Sub(Mul(Div(n, 4093), 2), 1);
}

// terrain.lua ValueNoise / SmoothNoise: smooth value noise on the hash above
static double LuaValueNoise(const double x, const double z, const double cell, const double seed)
{
  const double fx = Div(x, cell);
  const double fz = Div(z, cell);
  const double ix = Floor(fx);
  const double iz = Floor(fz);
  double tx = Sub(fx, ix);
  double tz = Sub(fz, iz);
  tx = Mul(Mul(tx, tx), Sub(3, Mul(2, tx)));
  tz = Mul(Mul(tz, tz), Sub(3, Mul(2, tz)));
  const double v00 = LuaNoise(ix, iz, seed), v10 = LuaNoise(Add(ix, 1), iz, seed);
  const double v01 = LuaNoise(ix, Add(iz, 1), seed), v11 = LuaNoise(Add(ix, 1), Add(iz, 1), seed);
  const double top = Add(v00, Mul(Sub(v10, v00), tx));
  const double bottom = Add(v01, Mul(Sub(v11, v01), tx));
  return Add(top, Mul(Sub(bottom, top), tz));
}

static double LuaSmoothNoise(const double x, const double z, const double seed, double scale, double octaves, double rough)
{
  if (scale < 1) scale = 1;
  octaves = Floor(octaves);
  if (octaves < 1) octaves = 1; else if (octaves > 4) octaves = 4;
  if (rough < 0) rough = 0; else if (rough > 1) rough = 1;
  double total = 0, amp = 1, norm = 0, cell = scale;
  for (int o = 0; o < octaves; ++o) {
    if (o > 0 && cell < 1) break;
    total = Add(total, Mul(amp, LuaValueNoise(x, z, cell, Add(seed, Mul(o, 1013)))));
    norm = Add(norm, amp);
    amp = Mul(amp, rough);
    cell = Div(cell, 2);
  }
  return Div(total, norm);
}

// terrain.lua BoxBlur on 1-based arrays (index 0 unused)
static std::vector<double> LuaBoxBlur(const std::vector<double>& src, const int w, const int h, const int kr)
{
  const double n = 2 * kr + 1;
  std::vector<double> tmp(src.size()), out(src.size());
  for (int z = 0; z <= h - 1; ++z) {
    const int row = z * w;
    double sum = 0;
    for (int k = -kr; k <= kr; ++k) {
      sum = Add(sum, src[row + std::min(std::max(k, 0), w - 1) + 1]);
    }
    for (int x = 0; x <= w - 1; ++x) {
      tmp[row + x + 1] = Div(sum, n);
      sum = Sub(Add(sum, src[row + std::min(x + kr + 1, w - 1) + 1]), src[row + std::max(x - kr, 0) + 1]);
    }
  }
  for (int x = 0; x <= w - 1; ++x) {
    double sum = 0;
    for (int k = -kr; k <= kr; ++k) {
      sum = Add(sum, tmp[std::min(std::max(k, 0), h - 1) * w + x + 1]);
    }
    for (int z = 0; z <= h - 1; ++z) {
      out[z * w + x + 1] = Div(sum, n);
      sum = Sub(Add(sum, tmp[std::min(z + kr + 1, h - 1) * w + x + 1]), tmp[std::max(z - kr, 0) * w + x + 1]);
    }
  }
  return out;
}

struct RefOut
{
  int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
  std::vector<std::uint16_t> after;
  int changed = 0;
  double level = -1;
  bool usedLevel = false;
};

// DabOne from `ed_terrain_read_rect` to the last write, on a grid of gw x gh words.
static RefOut LuaDab(const std::vector<std::uint16_t>& grid, const int gw, const int gh, const double cxIn, const double czIn, const LuaBrush& bIn)
{
  RefOut o;
  // every Lua number is a float: the arguments and the constants of the source are rounded to one
  const double cx = F(cxIn), cz = F(czIn);
  LuaBrush b = bIn;
  b.radius = F(b.radius);
  b.falloff = F(b.falloff);
  b.strength = F(b.strength);
  b.level = b.level < 0 ? b.level : F(b.level);
  b.noiseScale = F(b.noiseScale);
  b.noiseRough = F(b.noiseRough);
  b.angle = F(b.angle);
  const double K02 = F(0.2);
  const double r = b.radius;
  int x0 = static_cast<int>(Floor(Sub(cx, r))) - 1, z0 = static_cast<int>(Floor(Sub(cz, r))) - 1;
  int x1 = static_cast<int>(std::ceil(Add(cx, r))) + 1, z1 = static_cast<int>(std::ceil(Add(cz, r))) + 1;
  if (x0 > x1) std::swap(x0, x1);
  if (z0 > z1) std::swap(z0, z1);
  x0 = std::max(0, x0);
  z0 = std::max(0, z0);
  x1 = std::min(gw - 1, x1);
  z1 = std::min(gh - 1, z1);
  o.x0 = x0; o.z0 = z0; o.x1 = x1; o.z1 = z1;
  if (x1 < x0 || z1 < z0) {
    return o;
  }
  const int w = x1 - x0 + 1, h = z1 - z0 + 1;
  std::vector<double> before(static_cast<std::size_t>(w) * h + 1), after;
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      before[z * w + x + 1] = grid[static_cast<std::size_t>(z0 + z) * gw + x0 + x];
    }
  }
  after = before;
  const std::string mode = b.mode;
  const double strength = b.strength;
  const double dir = b.dir;
  double strokeLevel = b.level;
  const bool haveLevel = b.level >= 0;
  if ((mode == "flatten" || mode == "plateau") && !haveLevel) {
    const int cxi = std::min(std::max(static_cast<int>(Floor(Add(cx, 0.5))), x0), x1);
    const int czi = std::min(std::max(static_cast<int>(Floor(Add(cz, 0.5))), z0), z1);
    strokeLevel = Div(before[(czi - z0) * w + (cxi - x0) + 1], 128);
  }
  if (mode == "flatten" || mode == "plateau") {
    o.level = strokeLevel;
    o.usedLevel = true;
  }
  std::vector<double> blurred;
  if (mode == "smooth") {
    const int kr = 1 + static_cast<int>(Floor(Add(Mul(Min(1, Div(std::fabs(strength), 10)), Max(0, Sub(Min(Mul(r, 0.25), 6), 1))), 0.5)));
    blurred = LuaBoxBlur(LuaBoxBlur(before, w, h, kr), w, h, kr);
  }
  int changed = 0;
  for (int z = z0; z <= z1; ++z) {
    for (int x = x0; x <= x1; ++x) {
      const double dx = Sub(x, cx);
      const double dz = Sub(z, cz);
      const double distance = Sqrt(Add(Mul(dx, dx), Mul(dz, dz)));
      // terrain.lua Weight(): 0 at and beyond the radius, then the shape mask or the round profile
      const double weight = b.shape ? (distance >= r ? 0.0 : LuaShapedWeight(*b.shape, dx, dz, r, b.angle)) : LuaWeight(distance, r, b.falloff);
      if (weight > 0) {
        const int i = (z - z0) * w + (x - x0) + 1;
        double value = before[i];
        if (mode == "raise") {
          value = Add(value, Mul(Mul(Mul(dir, strength), weight), 128));
        } else if (mode == "lower") {
          value = Sub(value, Mul(Mul(Mul(dir, strength), weight), 128));
        } else if (mode == "plateau") {
          const double k = Mul(Min(1, Mul(Mul(strength, 0.5), weight)), dir);
          value = Add(value, Mul(Sub(Mul(strokeLevel, 128), value), k));
        } else if (mode == "flatten") {
          const double k = Mul(Min(1, Mul(Mul(strength, K02), weight)), dir);
          value = Add(value, Mul(Sub(Mul(strokeLevel, 128), value), k));
        } else if (mode == "smooth") {
          const double k = Mul(Min(1, Mul(Mul(strength, K02), weight)), dir);
          value = Add(value, Mul(Sub(blurred[i], value), k));
        } else if (mode == "noise") {
          value = Add(value, Mul(Mul(Mul(Mul(dir, LuaSmoothNoise(x, z, b.strokeId, b.noiseScale, b.noiseOctaves, b.noiseRough)), strength), weight), 128));
        }
        value = Floor(Add(value, 0.5));
        if (value < 0) value = 0;
        if (value > 65535) value = 65535;
        if (value != before[i]) {
          after[i] = value;
          ++changed;
        }
      }
    }
  }
  o.changed = changed;
  o.after.resize(static_cast<std::size_t>(w) * h);
  for (std::size_t i = 0; i < o.after.size(); ++i) {
    o.after[i] = static_cast<std::uint16_t>(after[i + 1]);
  }
  return o;
}

// ---- reference 2: plain double, rows and columns spelled out ------------------------------------------------------------------------

static std::vector<double> DoubleBlurPass(const std::vector<double>& src, const int w, const int h, const int kr)
{
  // a direct box average with clamped edges (no running sums)
  std::vector<double> tmp(src.size()), out(src.size());
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      double s = 0;
      for (int k = -kr; k <= kr; ++k) s += src[z * w + std::min(std::max(x + k, 0), w - 1)];
      tmp[z * w + x] = s / (2 * kr + 1);
    }
  }
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      double s = 0;
      for (int k = -kr; k <= kr; ++k) s += tmp[std::min(std::max(z + k, 0), h - 1) * w + x];
      out[z * w + x] = s / (2 * kr + 1);
    }
  }
  return out;
}

static double DoubleWeight(const double d, const double r, double falloff)
{
  if (d >= r) return 0;
  falloff = std::min(1.0, std::max(0.0, falloff));
  const double width = std::min(r, std::max(r * falloff, 1.0));
  const double inner = r - width;
  if (d <= inner) return 1;
  return 0.5 * (1 + std::cos(3.14159265358979 * (d - inner) / width));
}

// the same shaped weight in plain double (a straightforward bilinear read of the grid, the rotation by cos / sin)
static double DoubleShapedWeight(const br::Shape& shape, const double dx, const double dz, const double r, const double angle)
{
  const double a = angle * 3.14159265358979323846 / 180.0;
  const double c = std::cos(a), s = std::sin(a);
  const double u = (dx * c + dz * s) / r, v = (-dx * s + dz * c) / r;
  if (u * u + v * v > 1.6) return 0;
  const int n = shape.n;
  const double fx = (u + 1) * 0.5 * (n - 1), fz = (v + 1) * 0.5 * (n - 1);
  if (fx < 0 || fz < 0 || fx > n - 1 || fz > n - 1) return 0;
  const int x0 = std::min(n - 2, static_cast<int>(std::floor(fx))), z0 = std::min(n - 2, static_cast<int>(std::floor(fz)));
  const double tx = fx - x0, tz = fz - z0;
  auto at = [&](const int x, const int z) { return static_cast<double>(shape.values[static_cast<std::size_t>(z) * n + x]); };
  return (at(x0, z0) * (1 - tx) + at(x0 + 1, z0) * tx) * (1 - tz) + (at(x0, z0 + 1) * (1 - tx) + at(x0 + 1, z0 + 1) * tx) * tz;
}

// returns the window's new words (no noise: its hash is integer arithmetic and is checked on its own)
static std::vector<double> DoubleDab(const std::vector<std::uint16_t>& grid, const int gw, const int gh, const double cx, const double cz, const LuaBrush& b, const RefOut& win)
{
  (void)gh;
  const int w = win.x1 - win.x0 + 1, h = win.z1 - win.z0 + 1;
  std::vector<double> before(static_cast<std::size_t>(w) * h), out;
  for (int z = 0; z < h; ++z) for (int x = 0; x < w; ++x) before[z * w + x] = grid[static_cast<std::size_t>(win.z0 + z) * gw + win.x0 + x];
  out = before;
  const std::string mode = b.mode;
  double level = win.level * 128;
  std::vector<double> blurred;
  if (mode == "smooth") {
    const int kr = 1 + static_cast<int>(std::floor(std::min(1.0, std::fabs(b.strength) / 10) * std::max(0.0, std::min(b.radius * 0.25, 6.0) - 1) + 0.5));
    blurred = DoubleBlurPass(DoubleBlurPass(before, w, h, kr), w, h, kr);
  }
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      const double dx = win.x0 + x - cx, dz = win.z0 + z - cz;
      const double dist = std::sqrt(dx * dx + dz * dz);
      const double wt = b.shape ? (dist >= b.radius ? 0.0 : DoubleShapedWeight(*b.shape, dx, dz, b.radius, b.angle)) : DoubleWeight(dist, b.radius, b.falloff);
      if (wt <= 0) continue;
      double v = before[z * w + x];
      if (mode == "raise") v += b.dir * b.strength * wt * 128;
      else if (mode == "lower") v -= b.dir * b.strength * wt * 128;
      else if (mode == "plateau") v += (level - v) * std::min(1.0, b.strength * 0.5 * wt) * b.dir;
      else if (mode == "flatten") v += (level - v) * std::min(1.0, b.strength * 0.2 * wt) * b.dir;
      else if (mode == "smooth") v += (blurred[z * w + x] - v) * std::min(1.0, b.strength * 0.2 * wt) * b.dir;
      v = std::min(65535.0, std::max(0.0, std::floor(v + 0.5)));
      out[z * w + x] = v;
    }
  }
  return out;
}

// ---- test data --------------------------------------------------------------------------------------------------------------------------

static std::uint32_t gSeed = 12345u;
static std::uint32_t Rand()
{
  gSeed = gSeed * 1664525u + 1013904223u;
  return gSeed >> 8;
}

static std::vector<std::uint16_t> MakeGrid(const int w, const int h, const int kind)
{
  std::vector<std::uint16_t> g(static_cast<std::size_t>(w) * h);
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      double v;
      switch (kind) {
      case 0: v = 20 * 128 + 800 * std::sin(x * 0.2) * std::cos(z * 0.13) + (Rand() % 400); break;   // hills with a little roughness
      case 1: v = 60 + (Rand() % 120); break;                                                       // near the floor: clamps at 0
      case 2: v = 65535 - (Rand() % 150); break;                                                    // near the top: clamps at 65535
      default: v = Rand() % 65536; break;                                                           // white noise
      }
      g[static_cast<std::size_t>(z) * w + x] = static_cast<std::uint16_t>(std::min(65535.0, std::max(0.0, v)));
    }
  }
  return g;
}

// ---- checks ------------------------------------------------------------------------------------------------------------------------------

static void CompareDab(const std::vector<std::uint16_t>& grid, const int gw, const int gh, const double cx, const double cz, const LuaBrush& lb, const char* what)
{
  br::Dab dab;
  dab.cx = static_cast<float>(cx);
  dab.cz = static_cast<float>(cz);
  dab.radius = static_cast<float>(lb.radius);
  dab.strength = static_cast<float>(lb.strength);
  dab.dir = lb.dir;
  dab.falloff = static_cast<float>(lb.falloff);
  dab.level = static_cast<float>(lb.level);
  dab.seed = lb.strokeId;
  dab.noiseScale = static_cast<float>(lb.noiseScale);
  dab.noiseOctaves = lb.noiseOctaves;
  dab.noiseRoughness = static_cast<float>(lb.noiseRough);
  dab.shape = lb.shape;
  dab.shapeAngle = static_cast<float>(lb.angle);
  CHECK(br::ModeFromName(lb.mode, dab.mode), "mode %s", lb.mode);

  br::Window win;
  std::vector<std::uint16_t> after;
  const br::Result res = br::ComputeDab(grid.data(), gw, gh, dab, win, after);
  const RefOut ref = LuaDab(grid, gw, gh, cx, cz, lb);

  CHECK(win.x0 == ref.x0 && win.z0 == ref.z0 && win.x1 == ref.x1 && win.z1 == ref.z1, "%s: window %d,%d..%d,%d vs %d,%d..%d,%d", what, win.x0, win.z0, win.x1, win.z1, ref.x0, ref.z0, ref.x1, ref.z1);
  if (win.Empty() || ref.after.empty()) {
    CHECK(win.Empty() && ref.after.empty() && res.changed == 0, "%s: empty window", what);
    return;
  }
  CHECK(after.size() == ref.after.size(), "%s: size", what);
  std::size_t wrong = 0, first = 0;
  for (std::size_t i = 0; i < after.size() && i < ref.after.size(); ++i) {
    if (after[i] != ref.after[i]) {
      if (wrong == 0) first = i;
      ++wrong;
    }
  }
  CHECK(wrong == 0, "%s: %zu words differ from the Lua transliteration (first at %zu: native %u, lua %u)", what, wrong, first, wrong ? after[first] : 0u, wrong ? ref.after[first] : 0u);
  CHECK(res.changed == ref.changed, "%s: changed %d vs %d", what, res.changed, ref.changed);
  CHECK(res.usedLevel == ref.usedLevel, "%s: usedLevel", what);
  if (ref.usedLevel) {
    CHECK(std::fabs(res.level - ref.level) < 1e-6, "%s: level %f vs %f", what, res.level, ref.level);
  }
  // changed counts the words that differ from the grid
  int diff = 0;
  const int ww = win.Width();
  for (int z = 0; z < win.Height(); ++z) {
    for (int x = 0; x < ww; ++x) {
      if (after[static_cast<std::size_t>(z) * ww + x] != grid[static_cast<std::size_t>(win.z0 + z) * gw + win.x0 + x]) ++diff;
    }
  }
  CHECK(diff == res.changed, "%s: changed %d, words that differ %d", what, res.changed, diff);

  // the plain double reference (not for noise: the integer hash has no double twin)
  if (std::string(lb.mode) != "noise") {
    const std::vector<double> d = DoubleDab(grid, gw, gh, cx, cz, lb, ref);
    double worst = 0;
    for (std::size_t i = 0; i < d.size(); ++i) worst = std::max(worst, std::fabs(d[i] - after[i]));
    // (a shaped brush rotates with float cos / sin, which moves a read of a noisy picture mask by a few 1e-6 of a cell: up to two words on a full-range pull)
    const double tol = lb.shape ? (std::string(lb.mode) == "smooth" ? 4.0 : 3.0) : (std::string(lb.mode) == "smooth" ? 3.0 : 1.0);
    CHECK(worst <= tol, "%s: worst difference from the double reference %f words", what, worst);
  }
}

static void TestSweep()
{
  const char* const modes[] = {"raise", "lower", "plateau", "flatten", "smooth", "noise"};
  const int gw = 61, gh = 47;
  for (int kind = 0; kind < 4; ++kind) {
    const std::vector<std::uint16_t> grid = MakeGrid(gw, gh, kind);
    // centres: inside, on every corner and edge, fractional, off the map
    const double centres[][2] = {{30, 23}, {0, 0}, {60, 46}, {0, 46}, {60, 0}, {30.37, 12.81}, {0.4, 23.5}, {59.7, 3.2}, {-3, -3}, {70, 20}, {12.5, 40.25}};
    const double radii[] = {1, 0.5, 2, 3.7, 9, 20, 80};
    const double falloffs[] = {0, 0.3, 1};
    const double strengths[] = {0.05, 1, 6, 12};
    char what[160];
    int count = 0;
    for (const char* const mode : modes) {
      for (const auto& c : centres) {
        for (const double r : radii) {
          for (const double f : falloffs) {
            for (const double s : strengths) {
              for (const int dir : {1, -1}) {
                if ((++count % 3) != 0 && r > 3) continue;       // thin out the large ones
                LuaBrush lb{r, f, s, dir, mode, -1, 7};
                std::snprintf(what, sizeof(what), "kind %d %s c=(%g,%g) r=%g f=%g s=%g dir=%d", kind, mode, c[0], c[1], r, f, s, dir);
                CompareDab(grid, gw, gh, c[0], c[1], lb, what);
                if (std::string(mode) == "flatten" || std::string(mode) == "plateau") {
                  LuaBrush lv = lb;
                  lv.level = 37.3;
                  std::snprintf(what, sizeof(what), "kind %d %s level 37.3 c=(%g,%g) r=%g f=%g s=%g dir=%d", kind, mode, c[0], c[1], r, f, s, dir);
                  CompareDab(grid, gw, gh, c[0], c[1], lv, what);
                }
              }
            }
          }
        }
      }
    }
  }
}

static void TestNoiseDabs()
{
  // whole noise dabs with every setting against the Lua transliteration
  const int gw = 61, gh = 47;
  const std::vector<std::uint16_t> grid = MakeGrid(gw, gh, 0);
  const double scales[] = {1, 3, 8, 12.5, 30};
  const int octs[] = {1, 2, 3, 4};
  const double roughs[] = {0.3, 0.5, 0.7};
  char what[160];
  for (const double sc : scales) {
    for (const int oc : octs) {
      for (const double ro : roughs) {
        LuaBrush lb{9, 0.6, 4, 1, "noise", -1, 21};
        lb.noiseScale = sc; lb.noiseOctaves = oc; lb.noiseRough = ro;
        std::snprintf(what, sizeof(what), "noise scale %g oct %d rough %g", sc, oc, ro);
        CompareDab(grid, gw, gh, 31.4, 22.7, lb, what);
        lb.dir = -1;
        CompareDab(grid, gw, gh, 3.2, 40.1, lb, what);
      }
    }
  }
}

static void TestNoise()
{
  // the integer hash in long arithmetic, and the float transliteration of the Lua source
  auto py = [](const int x, const int z, const int seed) {
    const long a = ((x % 4093) * 1103L + 17) % 4093, b = ((z % 4091) * 1999L + 31) % 4091, c = ((seed % 4079) * 997L + 5) % 4079;
    long n = (a * 4001 + b) % 4093;
    n = (n * n + c) % 4093;
    n = (n * n + a) % 4093;
    n = (n * n + b) % 4093;
    return static_cast<double>(n) / 4093.0 * 2 - 1;
  };
  for (int i = 0; i < 20000; ++i) {
    const int x = static_cast<int>(Rand() % 8192), z = static_cast<int>(Rand() % 8192), seed = static_cast<int>(Rand() % 100000);
    const float n = br::detail::Noise(x, z, seed);
    CHECK(std::fabs(n - py(x, z, seed)) < 1e-6, "noise(%d,%d,%d) = %f vs %f", x, z, seed, n, py(x, z, seed));
    CHECK(std::fabs(n - LuaNoise(x, z, seed)) == 0, "noise vs the float transliteration at %d,%d,%d", x, z, seed);
    CHECK(n >= -1.0f && n <= 1.0f, "noise range");
  }
  // it is real noise: spread over -1..1, no neighbour correlation
  double sum = 0, sum2 = 0, cross = 0;
  const int n = 128;
  std::vector<float> v(static_cast<std::size_t>(n) * n);
  for (int z = 0; z < n; ++z) for (int x = 0; x < n; ++x) v[static_cast<std::size_t>(z) * n + x] = br::detail::Noise(x + 1500, z + 2200, 9);
  for (const float f : v) { sum += f; sum2 += static_cast<double>(f) * f; }
  for (int z = 0; z < n; ++z) for (int x = 0; x + 1 < n; ++x) cross += static_cast<double>(v[static_cast<std::size_t>(z) * n + x]) * v[static_cast<std::size_t>(z) * n + x + 1];
  const double count = static_cast<double>(n) * n;
  CHECK(std::fabs(sum / count) < 0.03, "noise mean %f", sum / count);
  CHECK(std::fabs(sum2 / count - 1.0 / 3.0) < 0.02, "noise variance %f", sum2 / count);
  CHECK(std::fabs(cross / (static_cast<double>(n) * (n - 1))) < 0.03, "noise neighbour correlation %f", cross / (static_cast<double>(n) * (n - 1)));
}

static void TestSmoothNoise()
{
  // the native smooth noise equals the float transliteration of terrain.lua for random points and settings
  const double scales[] = {1, 2, 3.5, 8, 8.25, 16, 40, 0.4};
  const double roughs[] = {0.3, 0.5, 0.7, 1.0, 0.0};
  for (int i = 0; i < 20000; ++i) {
    const int x = static_cast<int>(Rand() % 2100), z = static_cast<int>(Rand() % 2100), seed = static_cast<int>(Rand() % 100000);
    const double scale = F(scales[Rand() % 8]), rough = F(roughs[Rand() % 5]);
    const int oct = 1 + static_cast<int>(Rand() % 4);
    const float n = br::detail::SmoothNoise(x, z, seed, static_cast<float>(scale), oct, static_cast<float>(rough));
    const double ref = LuaSmoothNoise(x, z, seed, scale, oct, rough);
    CHECK(n == static_cast<float>(ref), "smooth noise (%d,%d) seed %d scale %g oct %d rough %g: %.9g vs lua %.9g", x, z, seed, scale, oct, rough, static_cast<double>(n), ref);
    CHECK(n >= -1.0001f && n <= 1.0001f, "smooth noise range %f", static_cast<double>(n));
  }
  // one octave on the lattice points is the hash itself
  for (int i = 0; i < 400; ++i) {
    const int ix = static_cast<int>(Rand() % 200), iz = static_cast<int>(Rand() % 200);
    const float n = br::detail::SmoothNoise(ix * 8, iz * 8, 5, 8.0f, 1, 0.5f);
    CHECK(n == br::detail::Noise(ix, iz, 5), "lattice point (%d,%d): %f vs %f", ix, iz, static_cast<double>(n), static_cast<double>(br::detail::Noise(ix, iz, 5)));
  }
  // smooth: neighbouring samples differ by at most the steepest smoothstep slope (3 / 2 of the range over one cell) while the white hash jumps
  {
    double worst = 0, sum = 0, whiteSum = 0;
    int count = 0;
    for (int z = 0; z < 120; ++z) {
      for (int x = 0; x < 119; ++x) {
        const double a = br::detail::SmoothNoise(x, z, 9, 8.0f, 1, 0.5f), b = br::detail::SmoothNoise(x + 1, z, 9, 8.0f, 1, 0.5f);
        worst = std::max(worst, std::fabs(a - b));
        sum += std::fabs(a - b);
        whiteSum += std::fabs(static_cast<double>(br::detail::Noise(x, z, 9)) - br::detail::Noise(x + 1, z, 9));
        ++count;
      }
    }
    CHECK(worst <= 2.0 * 1.5 / 8.0 + 1e-4, "one octave of scale 8: neighbour step %f", worst);
    CHECK(sum / count < 0.25 * whiteSum / count, "smooth noise steps %f vs white %f", sum / count, whiteSum / count);
  }
  // it still has spread (not flat): the standard deviation over a big window
  {
    double sum = 0, sum2 = 0;
    const int n = 200;
    for (int z = 0; z < n; ++z) for (int x = 0; x < n; ++x) {
      const double v = br::detail::SmoothNoise(x + 300, z + 700, 4, 8.0f, 2, 0.5f);
      sum += v;
      sum2 += v * v;
    }
    const double mean = sum / (n * n), sd = std::sqrt(sum2 / (n * n) - mean * mean);
    CHECK(std::fabs(mean) < 0.1 && sd > 0.2 && sd < 0.7, "smooth noise mean %f sd %f", mean, sd);
  }
  // the scale sets the size of the bumps: the number of sign changes along a line falls when the cell grows
  {
    auto crossings = [](const float scale) {
      int c = 0;
      float last = br::detail::SmoothNoise(0, 50, 3, scale, 1, 0.5f);
      for (int x = 1; x < 400; ++x) {
        const float v = br::detail::SmoothNoise(x, 50, 3, scale, 1, 0.5f);
        if ((v < 0) != (last < 0)) ++c;
        last = v;
      }
      return c;
    };
    CHECK(crossings(4.0f) > crossings(16.0f) && crossings(16.0f) > crossings(64.0f), "bigger scale, fewer zero crossings (%d %d %d)", crossings(4.0f), crossings(16.0f), crossings(64.0f));
  }
  // coherent across dabs: two overlapping flat dabs of one stroke (same seed) add the same amount to a sample, whatever their centres
  {
    const int gw = 80, gh = 80;
    const std::vector<std::uint16_t> grid(static_cast<std::size_t>(gw) * gh, 30000);
    br::Dab a;
    a.mode = br::Mode::Noise; a.radius = 16; a.falloff = 0; a.strength = 6; a.seed = 11; a.noiseScale = 6; a.noiseOctaves = 3; a.noiseRoughness = 0.5f;
    br::Dab b = a;
    a.cx = 30; a.cz = 38;
    b.cx = 44; b.cz = 40;
    br::Window wa, wb;
    std::vector<std::uint16_t> ra, rb;
    br::ComputeDab(grid.data(), gw, gh, a, wa, ra);
    br::ComputeDab(grid.data(), gw, gh, b, wb, rb);
    int overlap = 0, bad = 0;
    for (int z = 30; z <= 48; ++z) {
      for (int x = 36; x <= 38; ++x) {
        const int ia = (z - wa.z0) * wa.Width() + (x - wa.x0), ib = (z - wb.z0) * wb.Width() + (x - wb.x0);
        ++overlap;
        if (ra[static_cast<std::size_t>(ia)] != rb[static_cast<std::size_t>(ib)]) ++bad;
      }
    }
    CHECK(overlap > 40 && bad == 0, "the two dabs give the same noise at shared samples (%d of %d differ)", bad, overlap);
    // another stroke (seed) gives another pattern
    br::Dab c = a;
    c.seed = 12;
    br::Window wc;
    std::vector<std::uint16_t> rc;
    br::ComputeDab(grid.data(), gw, gh, c, wc, rc);
    CHECK(rc != ra, "another stroke id gives another pattern");
  }
}

static void TestProperties()
{
  const int gw = 40, gh = 40;
  const std::vector<std::uint16_t> grid = MakeGrid(gw, gh, 0);
  br::Window win;
  std::vector<std::uint16_t> after;

  // nothing beyond the radius changes; the window is the radius plus one sample, clamped
  br::Dab dab;
  dab.cx = 20; dab.cz = 20; dab.radius = 5; dab.strength = 4; dab.mode = br::Mode::Raise;
  br::Result r = br::ComputeDab(grid.data(), gw, gh, dab, win, after);
  CHECK(win.x0 == 14 && win.x1 == 26 && win.z0 == 14 && win.z1 == 26, "window of r=5 at (20,20): %d..%d", win.x0, win.x1);
  CHECK(r.changed > 0, "raise changes samples");
  bool outside = true;
  for (int z = win.z0; z <= win.z1; ++z) {
    for (int x = win.x0; x <= win.x1; ++x) {
      const double d = std::sqrt((x - 20.0) * (x - 20.0) + (z - 20.0) * (z - 20.0));
      const std::uint16_t a = after[static_cast<std::size_t>(z - win.z0) * win.Width() + (x - win.x0)];
      if (d >= 5 && a != grid[static_cast<std::size_t>(z) * gw + x]) outside = false;
    }
  }
  CHECK(outside, "samples at or beyond the radius are untouched");
  CHECK(after[static_cast<std::size_t>(20 - win.z0) * win.Width() + (20 - win.x0)] == grid[20 * gw + 20] + 4 * 128, "raise at the centre adds strength * 128 words");

  // radius 1: only the centre sample has a weight (distance 0), neighbours at distance 1 are outside
  dab.radius = 1; dab.cx = 10; dab.cz = 10; dab.strength = 2; dab.falloff = 1;
  r = br::ComputeDab(grid.data(), gw, gh, dab, win, after);
  CHECK(r.changed == 1, "radius 1 changes the centre sample only (%d)", r.changed);
  CHECK(win.x0 == 8 && win.x1 == 12, "radius 1 window %d..%d", win.x0, win.x1);

  // lower is the inverse of raise on an unclamped sample, dir flips it
  dab.radius = 6; dab.strength = 3; dab.mode = br::Mode::Raise; dab.dir = 1;
  std::vector<std::uint16_t> up, down, inv;
  br::ComputeDab(grid.data(), gw, gh, dab, win, up);
  dab.mode = br::Mode::Lower; dab.dir = -1;
  br::ComputeDab(grid.data(), gw, gh, dab, win, inv);
  CHECK(up == inv, "lower with dir -1 equals raise");

  // the level: nil/negative takes the ground under the centre and returns it; a given level is used as is
  dab.mode = br::Mode::Flatten; dab.dir = 1; dab.level = -1; dab.cx = 15; dab.cz = 25; dab.radius = 4; dab.strength = 5;
  r = br::ComputeDab(grid.data(), gw, gh, dab, win, after);
  CHECK(r.usedLevel && std::fabs(r.level - grid[25 * gw + 15] / 128.0) < 1e-9, "flatten level = ground under the centre (%f)", r.level);
  dab.level = 12.5f;
  r = br::ComputeDab(grid.data(), gw, gh, dab, win, after);
  CHECK(r.usedLevel && r.level == 12.5f, "a given level is kept");
  dab.mode = br::Mode::Raise;
  r = br::ComputeDab(grid.data(), gw, gh, dab, win, after);
  CHECK(!r.usedLevel, "raise has no level");

  // flatten at strength 5 on a plateau of the level changes nothing
  {
    std::vector<std::uint16_t> flat(static_cast<std::size_t>(gw) * gh, 3200);
    dab.mode = br::Mode::Flatten; dab.level = 25.0f;     // 25 * 128 = 3200
    r = br::ComputeDab(flat.data(), gw, gh, dab, win, after);
    CHECK(r.changed == 0, "flatten of flat ground at its own level changes nothing");
    dab.mode = br::Mode::Smooth;
    r = br::ComputeDab(flat.data(), gw, gh, dab, win, after);
    CHECK(r.changed == 0, "smooth of flat ground changes nothing (%d)", r.changed);
  }

  // clamping
  {
    std::vector<std::uint16_t> top(static_cast<std::size_t>(gw) * gh, 65500);
    dab.mode = br::Mode::Raise; dab.strength = 12; dab.level = -1; dab.dir = 1;
    br::ComputeDab(top.data(), gw, gh, dab, win, after);
    std::uint16_t hi = 0;
    for (const std::uint16_t a : after) hi = std::max(hi, a);
    CHECK(hi == 65535, "raise clamps at 65535 (%u)", hi);
    std::vector<std::uint16_t> low(static_cast<std::size_t>(gw) * gh, 100);
    dab.mode = br::Mode::Lower;
    br::ComputeDab(low.data(), gw, gh, dab, win, after);
    std::uint16_t lo = 65535;
    for (const std::uint16_t a : after) lo = std::min(lo, a);
    CHECK(lo == 0, "lower clamps at 0 (%u)", lo);
  }

  // a dab entirely off the map has an empty window; radius 0 changes nothing
  dab.cx = -50; dab.cz = -50; dab.radius = 5; dab.mode = br::Mode::Raise;
  r = br::ComputeDab(grid.data(), gw, gh, dab, win, after);
  CHECK(win.Empty() && r.changed == 0, "a dab off the map is empty");
  dab.cx = 10; dab.cz = 10; dab.radius = 0;
  r = br::ComputeDab(grid.data(), gw, gh, dab, win, after);
  CHECK(r.changed == 0, "radius 0 changes nothing");

  // unsupported modes are refused by name
  br::Mode m;
  CHECK(!br::ModeFromName("erode", m) && !br::ModeFromName("stamp", m) && !br::ModeFromName(nullptr, m) && br::ModeFromName("noise", m), "mode names");
}


// ---- shaped brushes (EdBrush.h Shape / ShapeSampler): the mask replaces the falloff profile ------------------------------------------------------

static double SmoothStep(const double a, const double b, const double x)
{
  double t = (x - a) / (b - a);
  t = std::min(1.0, std::max(0.0, t));
  return t * t * (3 - 2 * t);
}

// A test shape: n x n values over -1..1 (corners on the samples), like brushes.lua Grid(): 0 ridge, 1 mountain-like (cone times bumps), 2 plateau, 3 a picture of random pixels,
// 4 a gradient that is not symmetric (shows the turn direction), 5 all ones (every sample inside the disc gets 1)
static br::Shape MakeShape(const int kind, const int n)
{
  br::Shape sh;
  sh.n = n;
  sh.values.resize(static_cast<std::size_t>(n) * n);
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      const double u = static_cast<double>(i) / (n - 1) * 2 - 1, v = static_cast<double>(j) / (n - 1) * 2 - 1;
      const double d = std::sqrt(u * u + v * v);
      double w = 0;
      switch (kind) {
      case 0: w = SmoothStep(1.0, 0.85, std::fabs(u)) * SmoothStep(0.35, 0.0, std::fabs(v)); break;
      case 1: w = std::min(1.0, std::max(0.0, 1 - d) * (0.35 + 1.3 * (0.5 + 0.5 * std::sin(u * 9.1 + 1.3) * std::cos(v * 7.7 - 0.4)))); break;
      case 2: w = SmoothStep(1.0, 0.7, d); break;
      case 3: w = static_cast<double>(Rand() % 65536) / 65535.0; break;
      case 4: w = (u + 1) * 0.5 * (1 - 0.5 * (v + 1) * 0.5); break;
      default: w = 1; break;
      }
      sh.values[static_cast<std::size_t>(j) * n + i] = static_cast<float>(w);
    }
  }
  return sh;
}

static void TestShapeRegistry()
{
  using namespace br;
  CHECK(FindShape(1) == nullptr, "no shape yet");
  CHECK(!SetShape(0, 4, std::vector<float>(16, 1.0f)), "handle 0 is refused");
  CHECK(!SetShape(1, 1, std::vector<float>(1, 1.0f)), "a 1 x 1 shape is refused");
  CHECK(!SetShape(1, 4, std::vector<float>(15, 1.0f)), "a wrong value count is refused");
  CHECK(!SetShape(1, kMaxShapeSize + 1, std::vector<float>(static_cast<std::size_t>(kMaxShapeSize + 1) * (kMaxShapeSize + 1), 1.0f)), "a huge shape is refused");
  CHECK(SetShape(1, 4, std::vector<float>(16, 0.5f)) && FindShape(1) && FindShape(1)->n == 4, "a shape is stored");
  CHECK(SetShape(1, 8, std::vector<float>(64, 0.25f)) && FindShape(1)->n == 8 && FindShape(1)->values[63] == 0.25f, "set again replaces it");
  CHECK(FindShape(2) == nullptr, "another handle is empty");
}

static void TestShaped()
{
  // native shaped dabs against the Lua transliteration (every Lua operation rounded to a float: the words must be equal) and against plain double (within a word or two)
  const int gw = 71, gh = 63;
  const char* const modes[] = {"raise", "lower", "plateau", "flatten", "smooth", "noise"};
  const double angles[] = {0, 17.5, 90, 200, 359.9, -30, 45};
  const int sizes[] = {64, 129};
  const double centres[][2] = {{35, 31}, {0, 0}, {70, 62}, {20.37, 40.81}, {-3, 10}, {66.5, 5.25}};
  const double radii[] = {1, 2.5, 7.3, 16, 30};
  std::vector<std::uint16_t> grids[4];
  for (int k = 0; k < 4; ++k) grids[k] = MakeGrid(gw, gh, k);
  char what[200];
  int count = 0;
  for (int kind = 0; kind < 6; ++kind) {
    for (const int n : sizes) {
      const br::Shape shape = MakeShape(kind, n);
      for (const char* const mode : modes) {
        for (const double angle : angles) {
          for (const auto& c : centres) {
            for (const double r : radii) {
              if ((++count % 4) != 0) continue;                         // thin out: 6 x 2 x 6 x 7 x 6 x 5 dabs are a lot
              const std::vector<std::uint16_t>& grid = grids[count % 4 == 0 ? (count / 4) % 4 : 0];
              LuaBrush lb{r, 0.5, (std::string(mode) == "smooth") ? 6 : 2.5, (count % 8 == 0) ? -1 : 1, mode, -1, 9};
              lb.shape = &shape;
              lb.angle = angle;
              std::snprintf(what, sizeof(what), "shape %d n=%d %s angle %g c=(%g,%g) r=%g dir=%d", kind, n, mode, angle, c[0], c[1], r, lb.dir);
              CompareDab(grid, gw, gh, c[0], c[1], lb, what);
              if (std::string(mode) == "flatten" || std::string(mode) == "plateau") {
                LuaBrush lv = lb;
                lv.level = 41.7;
                std::snprintf(what, sizeof(what), "shape %d n=%d %s level angle %g c=(%g,%g) r=%g", kind, n, mode, angle, c[0], c[1], r);
                CompareDab(grid, gw, gh, c[0], c[1], lv, what);
              }
            }
          }
        }
      }
    }
  }
  CHECK(count > 1000, "the shaped sweep ran %d dabs", count);
}

static void TestShapeProperties()
{
  const int gw = 60, gh = 60;
  const std::vector<std::uint16_t> flat(static_cast<std::size_t>(gw) * gh, 20000);
  br::Window win;
  std::vector<std::uint16_t> a, b;
  br::Dab dab;
  dab.cx = 30; dab.cz = 30; dab.radius = 12; dab.strength = 2; dab.mode = br::Mode::Raise; dab.falloff = 1;

  // a shape of ones: a flat disc, full strength to the rim (the falloff is not used), nothing at or beyond the radius
  const br::Shape ones = MakeShape(5, 64);
  dab.shape = &ones;
  br::ComputeDab(flat.data(), gw, gh, dab, win, a);
  int inside = 0, atFull = 0, bad = 0;
  for (int z = win.z0; z <= win.z1; ++z) {
    for (int x = win.x0; x <= win.x1; ++x) {
      const double d = std::sqrt(static_cast<double>((x - 30) * (x - 30) + (z - 30) * (z - 30)));
      const std::uint16_t v = a[static_cast<std::size_t>(z - win.z0) * win.Width() + (x - win.x0)];
      if (d < 12 - 1e-3) {
        ++inside;
        if (v == 20000 + 2 * 128) ++atFull; else ++bad;
      } else if (d >= 12 + 1e-3 && v != 20000) {
        ++bad;
      }
    }
  }
  CHECK(inside > 400 && atFull == inside && bad == 0, "a shape of ones is a flat disc (%d of %d at full strength, %d wrong)", atFull, inside, bad);

  // the shape is turned by the angle, anti-clockwise as brushes.lua turns it: the gradient shape (white at +u) is white towards +x at angle 0, towards +z at 90 degrees... (u = dx c + dz s)
  const br::Shape grad = MakeShape(4, 64);
  dab.shape = &grad;
  dab.radius = 20;
  dab.shapeAngle = 0;
  br::ComputeDab(flat.data(), gw, gh, dab, win, a);
  auto at = [&](const std::vector<std::uint16_t>& v, const int x, const int z) { return static_cast<int>(v[static_cast<std::size_t>(z - win.z0) * win.Width() + (x - win.x0)]) - 20000; };
  // the gradient shape is (u + 1) / 2 * (1 - (v + 1) / 4); strength 2 = 256 words at weight 1; at 10 of 20 units: u or v = +-0.5
  CHECK(std::abs(at(a, 40, 30) - 144) <= 2 && std::abs(at(a, 20, 30) - 48) <= 2, "angle 0: +x is the white side (%d vs %d, expected 144 and 48)", at(a, 40, 30), at(a, 20, 30));
  dab.shapeAngle = 90;
  br::ComputeDab(flat.data(), gw, gh, dab, win, b);
  // a mask turned by 90 degrees: u = dz / r, v = -dx / r
  CHECK(std::abs(at(b, 30, 40) - 144) <= 2 && std::abs(at(b, 30, 20) - 48) <= 2, "angle 90: +z is the white side (%d vs %d)", at(b, 30, 40), at(b, 30, 20));
  CHECK(std::abs(at(b, 40, 30) - 112) <= 2 && std::abs(at(b, 20, 30) - 80) <= 2, "angle 90: along x v = -dx / r (%d vs %d, expected 112 and 80)", at(b, 40, 30), at(b, 20, 30));
  dab.shapeAngle = 360;
  br::ComputeDab(flat.data(), gw, gh, dab, win, b);
  int differ = 0;
  for (std::size_t i = 0; i < a.size(); ++i) differ += (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > 1) ? 1 : 0;
  CHECK(differ == 0, "angle 360 = angle 0 (%d samples differ)", differ);

  // a null or too small shape is the round brush
  dab.shape = nullptr;
  dab.shapeAngle = 0;
  dab.radius = 9;
  br::Dab round = dab;
  br::Shape tiny;
  round.shape = &tiny;           // n = 0: not usable, the round profile is used
  std::vector<std::uint16_t> r1, r2;
  br::ComputeDab(flat.data(), gw, gh, dab, win, r1);
  br::ComputeDab(flat.data(), gw, gh, round, win, r2);
  CHECK(r1 == r2, "an empty shape is the round brush");

  // the mask replaces the profile for every mode: a smooth dab on flat ground changes nothing, flatten at the ground's own level changes nothing
  dab.shape = &grad;
  dab.mode = br::Mode::Smooth;
  CHECK(br::ComputeDab(flat.data(), gw, gh, dab, win, a).changed == 0, "smooth with a shape on flat ground changes nothing");
  dab.mode = br::Mode::Flatten;
  dab.level = 20000.0f / 128.0f;
  CHECK(br::ComputeDab(flat.data(), gw, gh, dab, win, a).changed == 0, "flatten with a shape at the ground's level changes nothing");
}

static void TestShapeWeights()
{
  // FillShapeWeights (the erosion brush's window) = the transliteration per sample, 0 at and beyond the radius, row major
  const double angles[] = {0, 33, 90, 271.5};
  const double radii[] = {1.5, 6, 25.25};
  for (int kind = 0; kind < 5; ++kind) {
    const br::Shape shape = MakeShape(kind, kind % 2 ? 64 : 128);
    for (const double angle : angles) {
      for (const double r : radii) {
        const double cx = 40.3, cz = 22.8;
        const int x0 = static_cast<int>(std::floor(cx - r)) - 4, z0 = static_cast<int>(std::floor(cz - r)) - 3, x1 = static_cast<int>(std::ceil(cx + r)) + 5, z1 = static_cast<int>(std::ceil(cz + r)) + 2;
        std::vector<float> w;
        br::FillShapeWeights(shape, static_cast<float>(angle), static_cast<float>(cx), static_cast<float>(cz), static_cast<float>(r), x0, z0, x1, z1, w);
        CHECK(w.size() == static_cast<std::size_t>(x1 - x0 + 1) * (z1 - z0 + 1), "weights count");
        int wrong = 0, nonzero = 0;
        std::size_t i = 0;
        for (int z = z0; z <= z1; ++z) {
          for (int x = x0; x <= x1; ++x, ++i) {
            const double dx = Sub(x, F(cx)), dz = Sub(z, F(cz));
            const double dist = Sqrt(Add(Mul(dx, dx), Mul(dz, dz)));
            const double ref = dist >= F(r) ? 0.0 : LuaShapedWeight(shape, dx, dz, F(r), F(angle));
            if (static_cast<double>(w[i]) != ref) ++wrong;
            if (w[i] > 0) ++nonzero;
          }
        }
        CHECK(wrong == 0, "shape %d angle %g r=%g: %d weights differ from the Lua transliteration", kind, angle, r, wrong);
        CHECK(nonzero > 0 || r < 2, "shape %d r=%g has weights", kind, r);
      }
    }
  }
}

// ---- the document: the write path of ed_terrain_brush ---------------------------------------------------------------------------------------

static void TestDocument()
{
  const int w = 70, h = 60;
  std::vector<std::uint16_t> start = MakeGrid(w, h, 0);
  MapDocument doc;
  std::vector<std::uint16_t> mirror = start;
  int sinks = 0;
  doc.Reset(w, h, start, [&](const CellRect& rect, const std::uint16_t* words) {
    ++sinks;
    for (int z = rect.z0; z <= rect.z1; ++z) {
      for (int x = rect.x0; x <= rect.x1; ++x) mirror[static_cast<std::size_t>(z) * w + x] = words[static_cast<std::size_t>(z - rect.z0) * rect.Width() + (x - rect.x0)];
    }
  });

  // the same steps ed_terrain_brush does, as one stroke of 12 dabs
  doc.BeginStroke("Terrain test");
  std::vector<std::uint16_t> model = start;
  std::size_t totalChanged = 0;
  for (int i = 0; i < 12; ++i) {
    br::Dab dab;
    dab.cx = 5.0f + i * 4.5f;
    dab.cz = 20.0f + 3.0f * std::sin(i * 0.7f);
    dab.radius = 9.0f;
    dab.strength = 3.0f;
    dab.mode = (i % 2) ? br::Mode::Smooth : br::Mode::Raise;
    dab.falloff = 0.8f;
    const br::Window win = br::WindowOf(dab, doc.Width(), doc.Height());
    CellRect rect;
    rect.x0 = win.x0; rect.z0 = win.z0; rect.x1 = win.x1; rect.z1 = win.z1;
    std::vector<std::uint16_t> before;
    doc.ReadRect(rect, before);
    CHECK(before.size() == win.Count(), "ReadRect of the window");
    std::vector<std::uint16_t> after(before.size());
    const br::Result res = br::ApplyWindow(dab, win, before.data(), after.data());
    const std::size_t changed = res.changed > 0 ? doc.EditHeights(rect, after.data()) : 0;
    CHECK(static_cast<int>(changed) == res.changed, "EditHeights count %zu vs %d", changed, res.changed);
    totalChanged += changed;
    for (int z = 0; z < win.Height(); ++z) for (int x = 0; x < win.Width(); ++x) model[static_cast<std::size_t>(win.z0 + z) * w + win.x0 + x] = after[static_cast<std::size_t>(z) * win.Width() + x];
  }
  doc.EndStroke();
  CHECK(totalChanged > 100, "the stroke changed samples (%zu)", totalChanged);
  CHECK(doc.UndoDepth() == 1, "one undo step for the whole stroke (%zu)", doc.UndoDepth());
  CHECK(mirror == model, "the sink saw exactly the words of the model");
  CellRect all;
  all.x0 = 0; all.z0 = 0; all.x1 = w - 1; all.z1 = h - 1;
  std::vector<std::uint16_t> read;
  doc.ReadRect(all, read);
  CHECK(read == model, "the document holds the model");
  const moho::editor::HistoryResult u = doc.Undo();
  CHECK(u.ok && u.verified, "undo ok");
  doc.ReadRect(all, read);
  CHECK(read == start, "undo restores the start");
  CHECK(mirror == start, "the sink saw the undo");
  doc.Redo();
  doc.ReadRect(all, read);
  CHECK(read == model, "redo restores the stroke");
}

// ---- timing (printed, not checked) -----------------------------------------------------------------------------------------------------------

static void Bench()
{
  const int w = 4096, h = 4096;
  std::vector<std::uint16_t> grid(static_cast<std::size_t>(w) * h);
  for (std::size_t i = 0; i < grid.size(); ++i) grid[i] = static_cast<std::uint16_t>(2000 + (i * 2654435761u >> 22));
  const char* const modes[] = {"raise", "plateau", "smooth", "noise"};
  for (const char* const mode : modes) {
    br::Dab dab;
    if (!br::ModeFromName(mode, dab.mode)) continue;
    dab.radius = 50; dab.strength = 3; dab.falloff = 1; dab.seed = 3;
    const auto t0 = std::chrono::steady_clock::now();
    const int reps = 200;
    long total = 0;
    for (int i = 0; i < reps; ++i) {
      dab.cx = 1000.0f + i * 7.3f;
      dab.cz = 2000.0f + i * 3.1f;
      br::Window win;
      std::vector<std::uint16_t> after;
      total += br::ComputeDab(grid.data(), w, h, dab, win, after).changed;
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("bench r=50 %-8s %.3f ms per dab (%ld samples changed per dab)\n", mode, std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, total / reps);
  }
}

static void BenchShaped()
{
  // a 64-radius dab on a 4096 map, round against shaped (the Lua loop it replaces costs ~25 Lua operations per sample plus a procedural shape function or a bilinear mask read)
  const int w = 4096, h = 4096;
  std::vector<std::uint16_t> grid(static_cast<std::size_t>(w) * h);
  for (std::size_t i = 0; i < grid.size(); ++i) grid[i] = static_cast<std::uint16_t>(2000 + (i * 2654435761u >> 22));
  const br::Shape mountain = MakeShape(1, 129), image = MakeShape(3, 64);
  const struct { const char* name; const br::Shape* shape; } cases[] = {{"round", nullptr}, {"mountain 129", &mountain}, {"image 64", &image}};
  for (const auto& cs : cases) {
    for (const char* const mode : {"raise", "smooth", "noise"}) {
      br::Dab dab;
      if (!br::ModeFromName(mode, dab.mode)) continue;
      dab.radius = 64; dab.strength = 3; dab.falloff = 1; dab.seed = 3;
      dab.shape = cs.shape;
      dab.shapeAngle = 33.0f;
      const auto t0 = std::chrono::steady_clock::now();
      const int reps = 200;
      long total = 0;
      for (int i = 0; i < reps; ++i) {
        dab.cx = 1000.0f + i * 7.3f;
        dab.cz = 2000.0f + i * 3.1f;
        br::Window win;
        std::vector<std::uint16_t> after;
        total += br::ComputeDab(grid.data(), w, h, dab, win, after).changed;
      }
      const auto t1 = std::chrono::steady_clock::now();
      std::printf("bench r=64 %-13s %-6s %.3f ms per dab (%ld samples changed per dab)\n", cs.name, mode, std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, total / reps);
    }
  }
}

int main(int argc, char** /*argv*/)
{
  TestSweep();
  TestNoise();
  TestNoiseDabs();
  TestSmoothNoise();
  TestProperties();
  TestShapeRegistry();
  TestShapeProperties();
  TestShapeWeights();
  TestShaped();
  TestDocument();
  if (argc > 1) {
    Bench();
    BenchShaped();
  }
  std::printf("%s: %d checks, %d failures\n", gFailures == 0 ? "PASS" : "FAIL", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
