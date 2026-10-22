// Stand-alone test of the symmetrize op (EdFieldSc.cpp, "symmetrize"): invariance under every group, the master wedge, the soft seam, the sector / start angle choice and maps that are not square.
//   build + run: python scripts/ed_native_tests.py            (EdSymTest links EdFieldCore.cpp + EdFieldSc.cpp)
// The group is built here a second time from the angles alone (not from the code under test): a rotation by 2 pi k / n, a mirror at axis + k pi / n.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "moho/editor/EdFieldCore.h"

using namespace moho::editor::field;

static int gFailures = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      if (++gFailures < 60) {                          \
        std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
      }                                                \
    }                                                  \
  } while (0)

static const double kPi = 3.14159265358979323846;

struct M2 { double a, b, c, d; };

struct Spec
{
  std::string mode;
  int n = 3;               // rotn
  double angle = 0.0;      // degrees
  int side = 0;
  std::string combine = "copy";
  double seam = 0.0;
  bool nearest = false;
  bool hasCentre = false;
  double cx = 0.0, cz = 0.0;
};

struct Ref
{
  std::vector<M2> m;
  double a0 = 0.0, width = 0.0;
  bool mirrored = false;
};

static Ref MakeRef(const Spec& s)
{
  Ref r;
  int k = 1;
  double axis = 0.0, a0 = 0.0;
  if (s.mode == "mirror_x") { k = 1; r.mirrored = true; axis = kPi / 2; }
  else if (s.mode == "mirror_z") { k = 1; r.mirrored = true; axis = kPi; }
  else if (s.mode == "mirror_xz") { k = 2; r.mirrored = true; axis = kPi; }
  else if (s.mode == "diag") { k = 1; r.mirrored = true; axis = kPi / 4; }
  else if (s.mode == "adiag") { k = 1; r.mirrored = true; axis = 3 * kPi / 4; }
  else if (s.mode == "diag_both") { k = 2; r.mirrored = true; axis = kPi / 4; }
  else if (s.mode == "rot2") { k = 2; a0 = kPi + s.angle * kPi / 180; }
  else if (s.mode == "rot4") { k = 4; a0 = kPi + s.angle * kPi / 180; }
  else { k = s.n; a0 = s.angle * kPi / 180; }
  for (int i = 0; i < k; ++i) {
    const double t = 2 * kPi * i / k;
    r.m.push_back({std::cos(t), -std::sin(t), std::sin(t), std::cos(t)});
  }
  if (r.mirrored) {
    for (int i = 0; i < k; ++i) {
      const double t = axis + kPi * i / k;
      r.m.push_back({std::cos(2 * t), std::sin(2 * t), std::sin(2 * t), -std::cos(2 * t)});
    }
    r.a0 = axis;
    r.width = kPi / k;
  } else {
    r.a0 = a0;
    r.width = 2 * kPi / k;
  }
  return r;
}

static GridPtr Make(const int w, const int h, const int kind, const double cell = 1.0, const double ox = 0.0, const double oz = 0.0)
{
  auto g = std::make_shared<Grid>(w, h, static_cast<float>(cell), static_cast<float>(ox), static_cast<float>(oz));
  std::uint32_t s = 12345u + static_cast<std::uint32_t>(kind) * 7919u;
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      if (kind < 100) {   // white noise: no two samples equal, every copy is traceable
        s = s * 1664525u + 1013904223u;
        g->At(x, z) = static_cast<float>((s >> 8) & 0xFFFF) / 65536.0f;
      } else {            // smooth, asymmetric
        const double fx = x / static_cast<double>(std::max(w, h)), fz = z / static_cast<double>(std::max(w, h));
        g->At(x, z) = static_cast<float>(0.5 + 0.25 * std::sin(6.0 * fx + 2.0 * fz + 0.3 * kind) + 0.2 * std::sin(-3.0 * fx + 7.0 * fz + 1.0) + 0.1 * std::cos(11.0 * fx - 5.0 * fz));
      }
    }
  }
  return g;
}

static GridPtr Sym(const GridPtr& src, const Spec& s, std::string* errOut = nullptr)
{
  OpArgs a;
  a.in["a"] = src;
  a.p.str["mode"] = s.mode;
  a.p.num["n"] = s.n;
  a.p.num["angle"] = s.angle;
  a.p.num["side"] = s.side;
  a.p.str["combine"] = s.combine;
  a.p.num["seam"] = s.seam;
  a.p.num["nearest"] = s.nearest ? 1.0 : 0.0;
  if (s.hasCentre) { a.p.num["cx"] = s.cx; a.p.num["cz"] = s.cz; }
  auto out = std::make_shared<Grid>();
  std::string err;
  if (!RunOp("symmetrize", a, *out, err)) {
    if (errOut != nullptr) *errOut = err;
    return nullptr;
  }
  return out;
}

static void Centre(const Grid& g, const Spec& s, double& cx, double& cz)
{
  cx = s.hasCentre ? s.cx : g.ox + 0.5 * (g.w - 1) * g.cell;
  cz = s.hasCentre ? s.cz : g.oz + 0.5 * (g.h - 1) * g.cell;
}

/** Orbit check. For every sample p and group element m with m(p) on a sample inside the grid: |out(m p) - out(p)| <= tol. Returns the number of pairs checked; `bad` counts violations. */
static long Invariance(const Grid& out, const Spec& s, const double tol, long& bad, double& worst)
{
  const Ref ref = MakeRef(s);
  double cx, cz;
  Centre(out, s, cx, cz);
  long checked = 0;
  for (int z = 0; z < out.h; ++z) {
    for (int x = 0; x < out.w; ++x) {
      const double u = out.ox + x * static_cast<double>(out.cell) - cx;
      const double v = out.oz + z * static_cast<double>(out.cell) - cz;
      for (const M2& m : ref.m) {
        const double sx = (cx + m.a * u + m.b * v - out.ox) / out.cell;
        const double sz = (cz + m.c * u + m.d * v - out.oz) / out.cell;
        const double ix = std::floor(sx + 0.5), iz = std::floor(sz + 0.5);
        if (std::fabs(sx - ix) > 1e-6 || std::fabs(sz - iz) > 1e-6 || ix < 0 || iz < 0 || ix > out.w - 1 || iz > out.h - 1) continue;
        ++checked;
        const double d = std::fabs(static_cast<double>(out.At(static_cast<int>(ix), static_cast<int>(iz))) - static_cast<double>(out.At(x, z)));
        worst = std::max(worst, d);
        if (d > tol) ++bad;
      }
    }
  }
  return checked;
}

/** Number of samples inside the half open master wedge [start, start + width) that the op changed. */
static long MasterChanged(const Grid& src, const Grid& out, const Spec& s)
{
  const Ref ref = MakeRef(s);
  double cx, cz;
  Centre(src, s, cx, cz);
  const int count = static_cast<int>(ref.m.size());
  const double start = ref.a0 + (((s.side % count) + count) % count) * ref.width;
  long changed = 0;
  for (int z = 0; z < src.h; ++z) {
    for (int x = 0; x < src.w; ++x) {
      const double u = src.ox + x * static_cast<double>(src.cell) - cx;
      const double v = src.oz + z * static_cast<double>(src.cell) - cz;
      if (u == 0.0 && v == 0.0) continue;
      double phi = std::fmod(std::atan2(v, u) - start, 2 * kPi);
      if (phi < 0) phi += 2 * kPi;
      if (phi > 2 * kPi - 1e-9) phi = 0;
      if (phi < ref.width - 1e-9 && out.At(x, z) != src.At(x, z)) ++changed;
    }
  }
  return changed;
}

/** Every output sample must be the value of some sample of its own orbit that lies inside the grid (no edge clamped values). Lattice groups only. */
static long NotFromOrbit(const Grid& src, const Grid& out, const Spec& s)
{
  const Ref ref = MakeRef(s);
  double cx, cz;
  Centre(src, s, cx, cz);
  long bad = 0;
  for (int z = 0; z < src.h; ++z) {
    for (int x = 0; x < src.w; ++x) {
      const double u = src.ox + x * static_cast<double>(src.cell) - cx;
      const double v = src.oz + z * static_cast<double>(src.cell) - cz;
      bool found = false;
      for (const M2& m : ref.m) {
        const double sx = (cx + m.a * u + m.b * v - src.ox) / src.cell;
        const double sz = (cz + m.c * u + m.d * v - src.oz) / src.cell;
        const double ix = std::floor(sx + 0.5), iz = std::floor(sz + 0.5);
        if (std::fabs(sx - ix) > 1e-6 || std::fabs(sz - iz) > 1e-6 || ix < 0 || iz < 0 || ix > src.w - 1 || iz > src.h - 1) continue;
        if (src.At(static_cast<int>(ix), static_cast<int>(iz)) == out.At(x, z)) { found = true; break; }
      }
      if (!found) ++bad;
    }
  }
  return bad;
}

static bool IsLattice(const std::string& mode) { return mode != "rotn"; }

static void TestInvariance()
{
  const std::vector<std::string> modes = {"mirror_x", "mirror_z", "mirror_xz", "diag", "adiag", "diag_both", "rot2", "rot4", "rotn"};
  const int sizes[][2] = {{65, 65}, {64, 64}, {65, 64}, {97, 65}, {33, 81}};
  long pairs = 0, cases = 0;
  for (const std::string& mode : modes) {
    for (const int n : {3, 4, 5, 6, 8}) {
      if (mode != "rotn" && n != 3) continue;
      for (const auto& sz : sizes) {
        const GridPtr src = Make(sz[0], sz[1], 1);
        for (const char* combine : {"copy", "avg", "max", "min"}) {
          Spec s;
          s.mode = mode;
          s.n = n;
          s.combine = combine;
          const int count = static_cast<int>(MakeRef(s).m.size());
          for (int side = 0; side < count + 2; ++side) {
            if (s.combine != "copy" && side > 0) break;
            for (const double angle : {0.0, 20.0, 90.0}) {
              if (!(mode == "rotn" || mode == "rot2" || mode == "rot4") && angle != 0.0) continue;
              s.side = side;
              s.angle = angle;
              const GridPtr out = Sym(src, s);
              CHECK(out != nullptr, "op failed for %s", mode.c_str());
              if (!out) continue;
              long bad = 0;
              double worst = 0;
              pairs += Invariance(*out, s, 1e-5, bad, worst);
              ++cases;
              CHECK(bad == 0, "%s n=%d %dx%d %s side %d angle %g: %ld invariance violations (worst %g)", mode.c_str(), n, sz[0], sz[1], combine, side, angle, bad, worst);
              if (s.combine == "copy") {
                CHECK(MasterChanged(*src, *out, s) == 0, "%s n=%d %dx%d side %d angle %g: the master wedge was changed", mode.c_str(), n, sz[0], sz[1], side, angle);
                if (IsLattice(mode) && angle == 0.0 && (((sz[0] - sz[1]) % 2) == 0 || mode == "mirror_x" || mode == "mirror_z" || mode == "mirror_xz" || mode == "rot2")) {   // a turn / diagonal of a grid whose sides differ by an odd number maps samples between samples
                  CHECK(NotFromOrbit(*src, *out, s) == 0, "%s %dx%d side %d: %ld samples are not a value of their orbit (edge clamped)", mode.c_str(), sz[0], sz[1], side, NotFromOrbit(*src, *out, s));
                }
              }
            }
          }
        }
      }
    }
  }
  std::printf("invariance: %ld cases, %ld image pairs checked\n", cases, pairs);
}

static void TestCentresAndCells()
{
  // offset centre on a sample, cell != 1 and a grid that does not start at the origin
  const GridPtr src = Make(65, 65, 2, 2.0, 10.0, -6.0);
  for (const char* mode : {"rot2", "rot4", "mirror_xz", "diag_both"}) {
    for (int side = 0; side < 4; ++side) {
      Spec s;
      s.mode = mode;
      s.side = side;
      s.hasCentre = true;
      s.cx = 10.0 + 2.0 * 30;   // sample (30, 30) ... the turn about a sample that is not the middle: only the images inside the grid count
      s.cz = -6.0 + 2.0 * 30;
      GridPtr out = Sym(src, s);
      CHECK(out != nullptr, "centre case failed");
      long bad = 0;
      double worst = 0;
      const long n = Invariance(*out, s, 0.0, bad, worst);
      CHECK(bad == 0 && n > 1000, "%s side %d about (30,30), cell 2: %ld violations of %ld", mode, side, bad, n);
      CHECK(NotFromOrbit(*src, *out, s) == 0, "%s side %d about (30,30): values from outside the orbit", mode, side);
      // the default centre of a grid with cell 2 and an offset is the middle of it
      Spec d;
      d.mode = mode;
      d.side = side;
      out = Sym(src, d);
      bad = 0;
      worst = 0;
      Invariance(*out, d, 0.0, bad, worst);
      CHECK(bad == 0, "%s side %d default centre, cell 2, offset: %ld violations", mode, side, bad);
      CHECK(MasterChanged(*src, *out, d) == 0, "%s side %d: master changed with cell 2", mode, side);
    }
  }
  // a seam is measured in world units: the same seam in units on a cell 2 grid and a cell 1 grid of the same area blends the same share of the map
  const GridPtr a1 = Make(129, 129, 101, 1.0);
  const GridPtr a2 = Make(65, 65, 101, 2.0);
  Spec s;
  s.mode = "rot4";
  s.seam = 16;
  const GridPtr o1 = Sym(a1, s), o2 = Sym(a2, s);
  long diff1 = 0, diff2 = 0;
  const GridPtr n1 = Sym(a1, Spec{"rot4"}), n2 = Sym(a2, Spec{"rot4"});
  for (std::size_t i = 0; i < o1->Count(); ++i) if (o1->v[i] != n1->v[i]) ++diff1;
  for (std::size_t i = 0; i < o2->Count(); ++i) if (o2->v[i] != n2->v[i]) ++diff2;
  const double share1 = static_cast<double>(diff1) / o1->Count(), share2 = static_cast<double>(diff2) / o2->Count();
  CHECK(std::fabs(share1 - share2) < 0.03 && share1 > 0.05, "seam width in units: shares %g (cell 1) vs %g (cell 2)", share1, share2);
}

static void TestSectorsAndAngle()
{
  const GridPtr src = Make(65, 65, 3);
  const auto Same = [&](const Spec& a, const Spec& b) {
    const GridPtr x = Sym(src, a), y = Sym(src, b);
    return x && y && Checksum(*x) == Checksum(*y);
  };
  Spec a, b;
  a.mode = "rot4"; b.mode = "rot4";
  a.side = 5; b.side = 1;
  CHECK(Same(a, b), "side 5 of 4 wraps to 1");
  a.side = -1; b.side = 3;
  CHECK(Same(a, b), "side -1 wraps to 3");
  // the start angle turns the layout: with the start 90 degrees later, wedge 0 is the old wedge 1
  a.side = 0; a.angle = 90; b.side = 1; b.angle = 0;
  CHECK(Same(a, b), "rot4: start angle 90 + part 0 = start angle 0 + part 1");
  a.mode = "rot2"; b.mode = "rot2"; a.side = 0; a.angle = 180; b.side = 1; b.angle = 0;
  CHECK(Same(a, b), "rot2: start angle 180 + part 0 = start angle 0 + part 1");
  a.mode = "rotn"; b.mode = "rotn"; a.n = 5; b.n = 5; a.side = 0; a.angle = 72; b.side = 1; b.angle = 0;
  CHECK(Same(a, b), "rotn 5: start angle 72 + part 0 = start angle 0 + part 1");
  a.angle = 0; b.angle = 360; a.side = 2; b.side = 2;
  CHECK(Same(a, b), "a full turn of the start angle changes nothing");
  // wedge layout: part k of Turn by 90 is the k-th quarter clockwise from the upper left (z points down); the same layout the Symmetry panel's parts list uses
  const char* names[4] = {"UL", "UR", "LR", "LL"};
  for (int k = 0; k < 4; ++k) {
    Spec s;
    s.mode = "rot4";
    s.side = k;
    const GridPtr out = Sym(src, s);
    const int q = 16;
    const int x = k == 0 || k == 3 ? 32 - q : 32 + q, z = k == 0 || k == 1 ? 32 - q : 32 + q;
    CHECK(out->At(x, z) == src->At(x, z), "rot4 part %d (%s) keeps the sample at (%d, %d)", k, names[k], x, z);
  }
  std::string err;
  Spec bad;
  bad.mode = "spin";
  CHECK(Sym(src, bad, &err) == nullptr && !err.empty(), "unknown mode must fail");
  bad.mode = "rotn"; bad.n = 40;
  CHECK(Sym(src, bad, &err) == nullptr, "n out of range must fail");
  bad.mode = "mirror_x"; bad.n = 3; bad.combine = "sum";
  CHECK(Sym(src, bad, &err) == nullptr, "bad combine must fail");
}

static double MaxStep(const Grid& g, const std::vector<char>* mask = nullptr)
{
  double m = 0;
  for (int z = 0; z < g.h; ++z) {
    for (int x = 0; x < g.w; ++x) {
      if (x + 1 < g.w && (!mask || (*mask)[z * g.w + x] || (*mask)[z * g.w + x + 1])) m = std::max(m, std::fabs(static_cast<double>(g.At(x + 1, z)) - g.At(x, z)));
      if (z + 1 < g.h && (!mask || (*mask)[z * g.w + x] || (*mask)[(z + 1) * g.w + x])) m = std::max(m, std::fabs(static_cast<double>(g.At(x, z + 1)) - g.At(x, z)));
    }
  }
  return m;
}

static void TestSeam()
{
  const int N = 129;
  const GridPtr src = Make(N, N, 104);   // smooth
  const double srcStep = MaxStep(*src);
  struct Case { const char* mode; int n; double angle; bool hardCut; };
  const Case cases[] = {{"rot2", 3, 0, true}, {"rot4", 3, 0, true}, {"rotn", 3, 0, true}, {"rotn", 5, 20, true}, {"rotn", 6, 0, true}, {"mirror_x", 3, 0, false}, {"mirror_xz", 3, 0, false}, {"diag_both", 3, 0, false}};
  for (const Case& c : cases) {
    Spec s;
    s.mode = c.mode; s.n = c.n; s.angle = c.angle;
    const Ref ref = MakeRef(s);
    // samples within 1.5 units of a border ray, at radius 20 .. 60
    std::vector<char> near(static_cast<std::size_t>(N) * N, 0);
    const int borders = static_cast<int>(std::lround(2 * kPi / ref.width));
    for (int z = 0; z < N; ++z) {
      for (int x = 0; x < N; ++x) {
        const double u = x - 64.0, v = z - 64.0, r = std::sqrt(u * u + v * v);
        if (r < 20 || r > 60) continue;
        for (int i = 0; i < borders; ++i) {
          const double t = ref.a0 + i * ref.width;
          const double dot = u * std::cos(t) + v * std::sin(t), cr = std::fabs(-u * std::sin(t) + v * std::cos(t));
          if (dot > 0 && cr < 1.5) near[z * N + x] = 1;
        }
      }
    }
    s.seam = 0;
    const GridPtr hard = Sym(src, s);
    s.seam = 16;
    const GridPtr soft = Sym(src, s);
    const double jumpHard = MaxStep(*hard, &near), jumpSoft = MaxStep(*soft, &near);
    std::printf("seam %-9s n=%d: step at the borders %.4f hard, %.4f soft (source %.4f)\n", c.mode, c.n, jumpHard, jumpSoft, srcStep);
    CHECK(jumpSoft <= 1.0 * srcStep, "%s n=%d: soft seam leaves a jump of %g across a border (source step %g)", c.mode, c.n, jumpSoft, srcStep);
    if (c.hardCut && std::string(c.mode) != "rot2") CHECK(jumpHard > 2.0 * srcStep, "%s n=%d: control: the hard cut should jump (%g vs %g)", c.mode, c.n, jumpHard, srcStep);
    // the result stays symmetric
    long bad = 0;
    double worst = 0;
    Invariance(*soft, s, 1e-5, bad, worst);
    CHECK(bad == 0, "%s n=%d with seam: %ld invariance violations (worst %g)", c.mode, c.n, bad, worst);
    // farther than the seam from every border (and from the centre) the soft result is the hard result
    long moved = 0;
    for (int z = 0; z < N; ++z) {
      for (int x = 0; x < N; ++x) {
        const double u = x - 64.0, v = z - 64.0, r = std::sqrt(u * u + v * v);
        double best = r;
        for (int i = 0; i < borders; ++i) {
          const double t = ref.a0 + i * ref.width;
          const double dot = u * std::cos(t) + v * std::sin(t), cr = std::fabs(-u * std::sin(t) + v * std::cos(t));
          best = std::min(best, dot > 0 ? cr : r);
        }
        if (best > 16.0 + 1.0 && soft->At(x, z) != hard->At(x, z)) ++moved;
      }
    }
    CHECK(moved == 0, "%s n=%d: %ld samples farther than the seam from every border changed", c.mode, c.n, moved);
  }
  // nearest (codes) never blends
  Spec s;
  s.mode = "rot4"; s.seam = 20; s.nearest = true;
  const GridPtr codes = Make(65, 65, 5);
  const GridPtr nn = Sym(codes, s);
  s.seam = 0;
  const GridPtr nn0 = Sym(codes, s);
  CHECK(Checksum(*nn) == Checksum(*nn0), "nearest ignores the seam");
  // seam only acts in copy mode
  s.nearest = false; s.combine = "avg"; s.seam = 20;
  const GridPtr av = Sym(codes, s);
  s.seam = 0;
  CHECK(Checksum(*av) == Checksum(*Sym(codes, s)), "seam does not change avg");
}

static void TestCombineAndThreads()
{
  const GridPtr src = Make(65, 65, 6);
  Spec s;
  s.mode = "rot4";
  s.combine = "avg";
  const GridPtr avg = Sym(src, s);
  s.combine = "max";
  const GridPtr mx = Sym(src, s);
  s.combine = "min";
  const GridPtr mn = Sym(src, s);
  for (int z = 0; z < 65; z += 3) {
    for (int x = 0; x < 65; x += 3) {
      const float v[4] = {src->At(x, z), src->At(64 - z, x), src->At(64 - x, 64 - z), src->At(z, 64 - x)};
      const float hi = std::max(std::max(v[0], v[1]), std::max(v[2], v[3])), lo = std::min(std::min(v[0], v[1]), std::min(v[2], v[3]));
      CHECK(mx->At(x, z) == hi && mn->At(x, z) == lo, "rot4 max / min at %d,%d", x, z);
      CHECK(std::fabs(avg->At(x, z) - 0.25f * (v[0] + v[1] + v[2] + v[3])) < 1e-6f, "rot4 avg at %d,%d", x, z);
    }
  }
  // exactly symmetric under n = 5 as well (averaging every image), on a sample centre
  s.mode = "rotn"; s.n = 5; s.combine = "avg";
  const GridPtr a5 = Sym(src, s);
  long bad = 0;
  double worst = 0;
  Invariance(*a5, s, 1e-5, bad, worst);
  CHECK(bad == 0, "rotn 5 avg: %ld violations", bad);
  // the result does not depend on the thread count
  Spec t;
  t.mode = "rotn"; t.n = 5; t.seam = 12;
  SetThreads(1);
  const std::uint64_t one = Checksum(*Sym(src, t));
  SetThreads(4);
  const std::uint64_t four = Checksum(*Sym(src, t));
  CHECK(one == four, "thread count changes the result");
}

int main()
{
  TestInvariance();
  TestCentresAndCells();
  TestSectorsAndAngle();
  TestSeam();
  TestCombineAndThreads();
  if (gFailures != 0) {
    std::printf("EdSymTest FAIL (%d)\n", gFailures);
    return 1;
  }
  std::printf("EdSymTest PASS\n");
  return 0;
}
