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
      const double weight = LuaWeight(Sqrt(Add(Mul(dx, dx), Mul(dz, dz))), r, b.falloff);
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
          value = Add(value, Mul(Mul(Mul(Mul(dir, LuaNoise(x, z, b.strokeId)), strength), weight), 128));
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
      const double wt = DoubleWeight(std::sqrt(dx * dx + dz * dz), b.radius, b.falloff);
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
    const double tol = std::string(lb.mode) == "smooth" ? 3.0 : 1.0;
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

int main(int argc, char** /*argv*/)
{
  TestSweep();
  TestNoise();
  TestProperties();
  TestDocument();
  if (argc > 1) {
    Bench();
  }
  std::printf("%s: %d checks, %d failures\n", gFailures == 0 ? "PASS" : "FAIL", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
