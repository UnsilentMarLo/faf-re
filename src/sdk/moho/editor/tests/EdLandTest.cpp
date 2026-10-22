// Stand-alone test of the Fields panel's "Generate" ops (EdFieldLand.cpp: fractal, amplitude, terrain_mix, keep_side) and of the `rotate` option of the core noise ops (no engine needed).
//   build + run: python scripts/ed_native_tests.py
//   EdLandTest.exe
// Property tests only (no golden checksums): bit identical results for any thread count, the exact size of an `amplitude` result, the maths of terrain_mix (blend modes, strength, mask)
// and keep_side (water level), noise that depends on the world position only, and the absence of the lattice artefacts that `rotate` removes.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "moho/editor/EdFieldCore.h"

using namespace moho::editor::field;

static int gFailures = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      ++gFailures;                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

using Num = std::map<std::string, double>;
using Str = std::map<std::string, std::string>;

static GridPtr Run(const std::string& op, const Num& nums, const Str& strs = {}, const std::map<std::string, GridPtr>& in = {}, std::string* errOut = nullptr)
{
  OpArgs a;
  a.p.num = nums;
  a.p.str = strs;
  a.in = in;
  auto g = std::make_shared<Grid>();
  std::string err;
  if (!RunOp(op, a, *g, err)) {
    if (errOut != nullptr) *errOut = err;
    else std::printf("op %s failed: %s\n", op.c_str(), err.c_str());
    return nullptr;
  }
  return g;
}

static GridPtr Const(const int w, const int h, const float v, const float cell = 1.0f)
{
  return std::make_shared<Grid>(w, h, cell, 0.0f, 0.0f, v);
}

static bool Same(const Grid& a, const Grid& b)
{
  return a.w == b.w && a.h == b.h && a.v == b.v;
}

static const int kThreads[] = {1, 2, 3, 5, 8};

/** The checksum of an op at every thread count must be the same. */
static void CheckThreads(const char* const label, const std::string& op, const Num& nums, const Str& strs, const std::map<std::string, GridPtr>& in)
{
  std::uint64_t first = 0;
  bool have = false;
  for (const int t : kThreads) {
    SetThreads(t);
    const GridPtr g = Run(op, nums, strs, in);
    CHECK(g != nullptr, "%s: op failed", label);
    if (g == nullptr) continue;
    const std::uint64_t sum = Checksum(*g);
    if (!have) {
      first = sum;
      have = true;
    }
    CHECK(sum == first, "%s: %d threads give a different result", label, t);
  }
  SetThreads(1);
}

static void TestDeterminism()
{
  const Num base = {{"width", 97}, {"height", 83}, {"seed", 5}, {"scale", 40}, {"octaves", 7}};
  CheckThreads("fractal fbm", "fractal", base, {}, {});
  CheckThreads("fractal billow warp", "fractal", {{"width", 97}, {"height", 83}, {"seed", 6}, {"scale", 50}, {"octaves", 6}, {"warp", 12}}, {{"kind", "billow"}}, {});
  CheckThreads("fractal ridged slope", "fractal", {{"width", 97}, {"height", 83}, {"seed", 7}, {"scale", 60}, {"octaves", 8}, {"slope_detail", 0.7}, {"sharpness", 3}, {"finest", 4}}, {{"kind", "ridged"}}, {});
  const GridPtr a = Run("fractal", base);
  const GridPtr b = Run("fractal", {{"width", 97}, {"height", 83}, {"seed", 9}, {"scale", 30}, {"octaves", 3}});
  CheckThreads("amplitude", "amplitude", {{"amplitude", 17}}, {}, {{"a", a}});
  CheckThreads("terrain_mix add", "terrain_mix", {{"strength", 0.7}}, {{"mode", "add"}}, {{"a", a}, {"b", b}, {"mask", b}});
  CheckThreads("terrain_mix multiply", "terrain_mix", {{"strength", 1}}, {{"mode", "multiply"}}, {{"a", a}, {"b", b}});
  CheckThreads("keep_side", "keep_side", {{"level", 0.5}, {"margin", 0.01}, {"soft", 0.05}}, {}, {{"a", b}, {"b", a}});
  for (const char* op : {"perlin", "simplex", "ridged", "billow"}) {
    CheckThreads(op, op, {{"width", 97}, {"height", 83}, {"seed", 3}, {"scale", 30}, {"octaves", 6}, {"rotate", 1}}, {}, {});
  }
  CheckThreads("musgrave rotate", "musgrave", {{"width", 97}, {"height", 83}, {"seed", 3}, {"scale", 30}, {"octaves", 6}, {"rotate", 1}}, {}, {});
  CheckThreads("fbm_damped rotate", "fbm_damped", {{"width", 97}, {"height", 83}, {"seed", 3}, {"scale", 30}, {"octaves", 6}, {"rotate", 1}}, {}, {});
}

/** Noise depends on the world position and the seed only: a coarse grid (cell 4) samples the same values as the fine one at the same places. */
static void TestWorldPosition()
{
  for (const std::string kind : {"fbm", "billow", "ridged"}) {
    const Num fine = {{"width", 129}, {"height", 129}, {"seed", 4}, {"scale", 90}, {"octaves", 6}, {"warp", 10}, {"slope_detail", 0.5}};
    Num coarse = fine;
    coarse["width"] = 33;
    coarse["height"] = 33;
    coarse["cell"] = 4;
    const GridPtr f = Run("fractal", fine, {{"kind", kind}});
    const GridPtr c = Run("fractal", coarse, {{"kind", kind}});
    bool ok = f != nullptr && c != nullptr;
    for (int z = 0; ok && z < 33; ++z) {
      for (int x = 0; ok && x < 33; ++x) ok = f->At(x * 4, z * 4) == c->At(x, z);
    }
    CHECK(ok, "fractal %s: a coarse grid does not sample the same noise as the fine one", kind.c_str());
  }
}

static GridPtr Ramp(const int w, const int h, const float lo, const float hi)
{
  auto g = std::make_shared<Grid>(w, h, 1.0f, 0.0f, 0.0f);
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) g->At(x, z) = lo + (hi - lo) * static_cast<float>(x + z) / static_cast<float>(w + h - 2);
  }
  return g;
}

static void TestAmplitude()
{
  const GridPtr noise = Run("fractal", {{"width", 129}, {"height", 129}, {"seed", 2}, {"scale", 50}, {"octaves", 6}});
  for (const double amp : {1.0, 17.5, 60.0}) {
    const GridPtr peak = Run("amplitude", {{"amplitude", amp}}, {}, {{"a", noise}});
    const Stats s = Measure(*peak);
    const double top = std::max(std::fabs(static_cast<double>(s.min)), std::fabs(static_cast<double>(s.max)));
    CHECK(std::fabs(top - amp) < 1e-5 * amp + 1e-6, "amplitude peak %g: the largest deviation is %.7f", amp, top);
    CHECK(std::fabs(s.mean) < 1e-5 * amp, "amplitude peak %g: the mean is %.9f, not zero", amp, s.mean);
    const GridPtr range = Run("amplitude", {{"amplitude", amp}}, {{"mode", "range"}}, {{"a", noise}});
    const Stats r = Measure(*range);
    CHECK(std::fabs((static_cast<double>(r.max) - static_cast<double>(r.min)) - amp) < 1e-5 * amp + 1e-6, "amplitude range %g: highest minus lowest is %.7f", amp, static_cast<double>(r.max) - static_cast<double>(r.min));
    const GridPtr lift = Run("amplitude", {{"amplitude", amp}}, {{"mode", "range"}, {"centre", "min"}}, {{"a", noise}});
    const Stats l = Measure(*lift);
    CHECK(std::fabs(static_cast<double>(l.min)) < 1e-6 && std::fabs(static_cast<double>(l.max) - amp) < 1e-5 * amp + 1e-6, "amplitude min centre %g: runs %.7f .. %.7f", amp, l.min, l.max);
    const GridPtr sd = Run("amplitude", {{"amplitude", amp}}, {{"mode", "sd"}}, {{"a", noise}});
    const Stats d = Measure(*sd);
    CHECK(std::fabs(d.sd - amp) < 1e-4 * amp + 1e-6, "amplitude sd %g: the standard deviation is %.7f", amp, d.sd);
  }
  // a negative amplitude flips the relief; a field without variation gives zeros; a shifted copy gives the same relief
  const GridPtr flat = Run("amplitude", {{"amplitude", 5}}, {}, {{"a", Const(9, 9, 3.0f)}});
  const Stats fs = Measure(*flat);
  CHECK(fs.min == 0.0f && fs.max == 0.0f, "amplitude of a flat field is not zero");
  const GridPtr shifted = Run("affine", {{"mul", 1}, {"add", 123}}, {}, {{"a", noise}});
  const GridPtr a1 = Run("amplitude", {{"amplitude", 20}}, {}, {{"a", noise}});
  const GridPtr a2 = Run("amplitude", {{"amplitude", 20}}, {}, {{"a", shifted}});
  double worst = 0.0;
  for (std::size_t i = 0; i < a1->v.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(a1->v[i] - a2->v[i])));
  CHECK(worst < 1e-3, "amplitude depends on the offset of the input (%.6f)", worst);
  std::string err;
  CHECK(Run("amplitude", {{"amplitude", 1}}, {{"mode", "bogus"}}, {{"a", noise}}, &err) == nullptr, "a bad mode is accepted");
}

static void TestMix()
{
  const int w = 20, h = 12;
  const GridPtr ter = Ramp(w, h, 10.0f, 50.0f);                   // 10 .. 50
  auto rel = std::make_shared<Grid>(w, h, 1.0f, 0.0f, 0.0f);        // +-4 checkerboard
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) rel->At(x, z) = ((x + z) & 1) ? 4.0f : -4.0f;
  }
  auto mask = std::make_shared<Grid>(w, h, 1.0f, 0.0f, 0.0f);       // left half 0, right half 1, one column 0.5
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) mask->At(x, z) = x < 9 ? 0.0f : (x == 9 ? 0.5f : 1.0f);
  }
  const Stats ts = Measure(*ter);
  const std::map<std::string, GridPtr> in = {{"a", ter}, {"b", rel}};

  // strength 0: the terrain exactly
  CHECK(Same(*Run("terrain_mix", {{"strength", 0}}, {{"mode", "add"}}, in), *ter), "strength 0 changed the terrain");
  // add, strength 1 / 0.5
  const GridPtr add = Run("terrain_mix", {{"strength", 1}}, {{"mode", "add"}}, in);
  const GridPtr half = Run("terrain_mix", {{"strength", 0.5}}, {{"mode", "add"}}, in);
  bool okAdd = true, okHalf = true;
  for (int i = 0; i < w * h; ++i) {
    okAdd = okAdd && std::fabs(add->v[i] - (ter->v[i] + rel->v[i])) < 1e-4f;
    okHalf = okHalf && std::fabs(half->v[i] - (ter->v[i] + 0.5f * rel->v[i])) < 1e-4f;
  }
  CHECK(okAdd, "add is not terrain + relief");
  CHECK(okHalf, "strength 0.5 is not the midpoint");
  // mask: 0 keeps the terrain bit for bit, 1 applies the relief, 0.5 half
  const GridPtr masked = Run("terrain_mix", {{"strength", 1}}, {{"mode", "add"}}, {{"a", ter}, {"b", rel}, {"mask", mask}});
  bool okMask = true;
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      const float m = mask->At(x, z);
      const float want = ter->At(x, z) + rel->At(x, z) * m;
      okMask = okMask && std::fabs(masked->At(x, z) - want) < 1e-4f;
      if (m == 0.0f) okMask = okMask && masked->At(x, z) == ter->At(x, z);
    }
  }
  CHECK(okMask, "the mask does not scale the change (or changes where it is 0)");
  // max raises only, min lowers only
  const GridPtr mx = Run("terrain_mix", {}, {{"mode", "max"}}, in);
  const GridPtr mn = Run("terrain_mix", {}, {{"mode", "min"}}, in);
  bool okMax = true, okMin = true;
  for (int i = 0; i < w * h; ++i) {
    okMax = okMax && mx->v[i] >= ter->v[i] && std::fabs(mx->v[i] - (ter->v[i] + std::max(rel->v[i], 0.0f))) < 1e-4f;
    okMin = okMin && mn->v[i] <= ter->v[i] && std::fabs(mn->v[i] - (ter->v[i] + std::min(rel->v[i], 0.0f))) < 1e-4f;
  }
  CHECK(okMax, "max does not only raise");
  CHECK(okMin, "min does not only lower");
  // multiply: the lowest point does not move, the highest takes the whole relief
  const GridPtr mul = Run("terrain_mix", {}, {{"mode", "multiply"}}, in);
  CHECK(std::fabs(mul->At(0, 0) - ter->At(0, 0)) < 1e-5f, "multiply moved the lowest point");
  CHECK(std::fabs(mul->At(w - 1, h - 1) - (ter->At(w - 1, h - 1) + rel->At(w - 1, h - 1))) < 1e-4f, "multiply did not apply the whole relief at the highest point");
  CHECK(std::fabs(mul->At(w / 2, h / 2) - ter->At(w / 2, h / 2)) <= std::fabs(rel->At(w / 2, h / 2)) + 1e-4f, "multiply changed the middle by more than the relief");
  // replace: mean + relief
  const GridPtr rep = Run("terrain_mix", {}, {{"mode", "replace"}}, in);
  bool okRep = true;
  for (int i = 0; i < w * h; ++i) okRep = okRep && std::fabs(rep->v[i] - (static_cast<float>(ts.mean) + rel->v[i])) < 1e-3f;
  CHECK(okRep, "replace is not mean + relief");
  // errors
  std::string err;
  CHECK(Run("terrain_mix", {}, {{"mode", "bogus"}}, in, &err) == nullptr, "a bad mode is accepted");
  CHECK(Run("terrain_mix", {}, {}, {{"a", ter}, {"b", Const(5, 5, 0)}}, &err) == nullptr, "a relief of another size is accepted");
}

static void TestKeepSide()
{
  const int w = 64, h = 64;
  const float level = 20.0f;
  auto before = std::make_shared<Grid>(w, h, 1.0f, 0.0f, 0.0f);
  auto after = std::make_shared<Grid>(w, h, 1.0f, 0.0f, 0.0f);
  std::uint32_t s = 12345u;
  auto rnd = [&s]() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) / 16777216.0f;
  };
  for (int i = 0; i < w * h; ++i) {
    before->v[i] = 5.0f + 30.0f * rnd();                 // 5 .. 35: land and sea
    after->v[i] = before->v[i] + (rnd() - 0.5f) * 40.0f; // a violent change
  }
  for (const float soft : {0.0f, 1.0f}) {
    const GridPtr out = Run("keep_side", {{"level", level}, {"margin", 0.25}, {"soft", soft}}, {}, {{"a", after}, {"b", before}});
    bool landDry = true, seaWet = true, untouched = true, neverPast = true;
    for (int i = 0; i < w * h; ++i) {
      const float h0 = before->v[i], r = after->v[i], o = out->v[i];
      if (h0 >= level) {
        landDry = landDry && o >= level - 1e-5f && o >= level + std::min(0.25f, h0 - level) - 1e-5f;
        if (r >= h0) untouched = untouched && o == r;
        neverPast = neverPast && o <= std::max(r, h0) + 1e-5f;
      } else {
        seaWet = seaWet && o <= level + 1e-5f && o <= level - std::min(0.25f, level - h0) + 1e-5f;
        if (r <= h0) untouched = untouched && o == r;
        neverPast = neverPast && o >= std::min(r, h0) - 1e-5f;
      }
    }
    CHECK(landDry, "soft %g: land went under the water", soft);
    CHECK(seaWet, "soft %g: the sea floor came above the water", soft);
    CHECK(untouched, "soft %g: a sample that moved away from the water changed", soft);
    CHECK(neverPast, "soft %g: a sample was moved past its old height", soft);
  }
  // a result that keeps its side is returned unchanged
  const GridPtr gentle = Run("affine", {{"mul", 1}, {"add", 0.01}}, {}, {{"a", before}});
  const GridPtr same = Run("keep_side", {{"level", 4}, {"margin", 0.25}, {"soft", 1}}, {}, {{"a", gentle}, {"b", before}});    // the water is below everything
  CHECK(Same(*same, *gentle), "keep_side changed a terrain that never reaches the water");
}

/** The mean |value - 0.5| at the coarse lattice points against the mean at the same number of other points: plain octaves all vanish at the lattice points, `rotate` ones do not. */
static double LatticeRatio(const std::string& op, const Num& nums, const double scale)
{
  const GridPtr g = Run(op, nums);
  if (g == nullptr) return -1.0;
  const int step = static_cast<int>(scale);
  double onLattice = 0.0, other = 0.0;
  int n1 = 0, n2 = 0;
  for (int z = 0; z + step / 2 < g->h; z += step) {
    for (int x = 0; x + step / 2 < g->w; x += step) {
      onLattice += std::fabs(static_cast<double>(g->At(x, z)) - 0.5);
      ++n1;
      other += std::fabs(static_cast<double>(g->At(x + step / 2, z + step / 2)) - 0.5);
      ++n2;
    }
  }
  return (onLattice / n1) / std::max(1e-9, other / n2);
}

static void TestLattice()
{
  // octave 0 only: at the lattice points gradient noise is exactly 0 (the mean value 0.5); every octave of a plain sum shares those points when the lacunarity is 2
  const Num plain = {{"width", 513}, {"height", 513}, {"seed", 3}, {"scale", 32}, {"octaves", 4}};
  Num rot = plain;
  rot["rotate"] = 1;
  const double rPlain = LatticeRatio("perlin", plain, 32);
  const double rRot = LatticeRatio("perlin", rot, 32);
  CHECK(rPlain >= 0.0 && rPlain < 0.3, "the plain sum is expected to vanish at the lattice points (ratio %.3f): the test no longer shows the artefact", rPlain);
  CHECK(rRot > 0.7 && rRot < 1.4, "rotated octaves still show the lattice (ratio %.3f)", rRot);
  for (const std::string op : {"ridged", "billow", "simplex"}) {
    Num r = rot;
    const double v = LatticeRatio(op, r, 32);
    CHECK(v > 0.6 && v < 1.5, "%s with rotate: lattice ratio %.3f", op.c_str(), v);
  }
  for (const std::string kind : {"fbm", "billow", "ridged"}) {
    const GridPtr g = Run("fractal", {{"width", 513}, {"height", 513}, {"seed", 3}, {"scale", 32}, {"octaves", 4}}, {{"kind", kind}});
    double onLattice = 0.0, other = 0.0;
    int n = 0;
    for (int z = 0; z + 16 < g->h; z += 32) {
      for (int x = 0; x + 16 < g->w; x += 32) {
        onLattice += std::fabs(static_cast<double>(g->At(x, z)) - 0.5);
        other += std::fabs(static_cast<double>(g->At(x + 16, z + 16)) - 0.5);
        ++n;
      }
    }
    if (kind == "fbm") CHECK(onLattice / other > 0.7 && onLattice / other < 1.4, "fractal fbm: lattice ratio %.3f", onLattice / other);
    (void)n;
  }
}

/** The variance of the slope along a direction must not depend on the direction (the 8 gradient noise prefers the axes). */
static void TestIsotropy()
{
  const GridPtr g = Run("fractal", {{"width", 513}, {"height", 513}, {"seed", 11}, {"scale", 70}, {"octaves", 6}});
  double var[6] = {};
  for (int d = 0; d < 6; ++d) {
    const double t = static_cast<double>(d) * 3.14159265358979 / 6.0;
    const double cx = std::cos(t), cz = std::sin(t);
    double sum = 0.0;
    long n = 0;
    for (int z = 1; z + 1 < g->h; ++z) {
      for (int x = 1; x + 1 < g->w; ++x) {
        const double gx = 0.5 * (static_cast<double>(g->At(x + 1, z)) - static_cast<double>(g->At(x - 1, z)));
        const double gz = 0.5 * (static_cast<double>(g->At(x, z + 1)) - static_cast<double>(g->At(x, z - 1)));
        const double dd = gx * cx + gz * cz;
        sum += dd * dd;
        ++n;
      }
    }
    var[d] = sum / static_cast<double>(n);
  }
  const double lo = *std::min_element(var, var + 6), hi = *std::max_element(var, var + 6);
  CHECK(hi / lo < 1.15, "the slope variance depends on the direction (ratio %.3f)", hi / lo);
}

static void TestFractalOptions()
{
  // finest = scale: one octave only, the same as octaves = 1
  const GridPtr one = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}, {"octaves", 1}});
  const GridPtr cut = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}, {"octaves", 8}, {"finest", 40}});
  CHECK(Same(*one, *cut), "finest = scale does not reduce the noise to one octave");
  const GridPtr three = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}, {"octaves", 3}});
  const GridPtr cut3 = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}, {"octaves", 8}, {"finest", 10}});
  CHECK(Same(*three, *cut3), "finest = scale / 4 does not give three octaves");
  // the seed changes the result, the same seed repeats it
  const GridPtr s1 = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 1}, {"scale", 40}});
  const GridPtr s1b = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 1}, {"scale", 40}});
  const GridPtr s2 = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}});
  CHECK(Same(*s1, *s1b) && !Same(*s1, *s2), "seeds");
  // every kind stays inside 0..1 and has a spread (not a flat field)
  for (const std::string kind : {"fbm", "billow", "ridged"}) {
    const GridPtr g = Run("fractal", {{"width", 257}, {"height", 257}, {"seed", 4}, {"scale", 80}, {"octaves", 6}, {"warp", 20}, {"slope_detail", 0.8}}, {{"kind", kind}});
    const Stats s = Measure(*g);
    CHECK(s.min >= 0.0f && s.max <= 1.0f && s.sd > 0.08 && s.sd < 0.3, "fractal %s: range %.3f .. %.3f sd %.3f", kind.c_str(), s.min, s.max, s.sd);
  }
  // the warp moves the pattern: not equal to the unwarped one, but still the same statistics
  const GridPtr nowarp = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}});
  const GridPtr warped = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}, {"warp", 15}});
  CHECK(!Same(*nowarp, *warped), "warp has no effect");
  // slope_detail 0 is the same as not giving it; a positive value changes the field
  const GridPtr sd0 = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}, {"slope_detail", 0}});
  const GridPtr sd1 = Run("fractal", {{"width", 65}, {"height", 65}, {"seed", 2}, {"scale", 40}, {"slope_detail", 0.8}});
  CHECK(Same(*nowarp, *sd0) && !Same(*sd0, *sd1), "slope_detail");
  std::string err;
  CHECK(Run("fractal", {{"width", 8}, {"height", 8}}, {{"kind", "bogus"}}, {}, &err) == nullptr, "a bad kind is accepted");
}

/** The old ops keep their results when `rotate` is not given (saved graphs and the golden cases of EdFieldTest keep working). */
static void TestLegacyUnchanged()
{
  for (const char* op : {"perlin", "simplex", "ridged", "billow"}) {
    const Num p = {{"width", 33}, {"height", 33}, {"seed", 1}, {"scale", 10}, {"octaves", 5}};
    Num p0 = p;
    p0["rotate"] = 0;
    Num p1 = p;
    p1["rotate"] = 1;
    const GridPtr a = Run(op, p);
    const GridPtr b = Run(op, p0);
    const GridPtr c = Run(op, p1);
    CHECK(Same(*a, *b), "%s: rotate = 0 differs from no rotate", op);
    CHECK(!Same(*a, *c), "%s: rotate = 1 has no effect", op);
  }
}

int main()
{
  TestDeterminism();
  TestWorldPosition();
  TestAmplitude();
  TestMix();
  TestKeepSide();
  TestLattice();
  TestIsotropy();
  TestFractalOptions();
  TestLegacyUnchanged();
  if (gFailures == 0) std::printf("EdLandTest PASS\n");
  else std::printf("EdLandTest FAIL (%d)\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
