// Stand-alone test of the path (spline) ops of the field engine (EdFieldPath.cpp; no engine needed).
//   build + run: python scripts/ed_path_test.py
//   EdPathTest.exe <cases.lua> [--update]     golden cases in editor/runtime/lua/editor/selftest_paths_cases.lua (shared with the Lua self test selftest_paths.lua);
//                                             --update rewrites the golden column from this run (it first proves the results do not depend on the thread count)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

static std::string Hex(const std::uint64_t v)
{
  char b[32];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

static std::vector<std::string> Split(const std::string& s, const char sep)
{
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

static bool ToNumber(const std::string& s, double& v)
{
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end != nullptr && *end == '\0';
}

static void ParseParams(const std::string& text, Params& p)
{
  if (text.empty()) return;
  for (const std::string& kv : Split(text, ',')) {
    const std::size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = kv.substr(0, eq);
    const std::string val = kv.substr(eq + 1);
    double n = 0.0;
    if (val.find(':') != std::string::npos || val.find(';') != std::string::npos) {
      std::vector<double> flat;
      for (const std::string& pt : Split(val, ';')) {
        for (const std::string& c : Split(pt, ':')) {
          if (ToNumber(c, n)) flat.push_back(n);
        }
      }
      p.arr[key] = flat;
    } else if (ToNumber(val, n)) {
      p.num[key] = n;
    } else {
      p.str[key] = val;
    }
  }
}

static std::map<std::string, GridPtr> gFix;

static GridPtr Make(const std::string& op, const std::string& params, const std::map<std::string, GridPtr>& in, std::string* err = nullptr)
{
  OpArgs a;
  ParseParams(params, a.p);
  a.in = in;
  auto g = std::make_shared<Grid>();
  std::string e;
  if (!RunOp(op, a, *g, e)) {
    if (err) *err = e;
    else std::printf("op %s(%s) failed: %s\n", op.c_str(), params.c_str(), e.c_str());
    return nullptr;
  }
  return g;
}

static void BuildFixtures()
{
  gFix.clear();
  gFix["A"] = Make("perlin", "width=65,height=65,seed=7,scale=20,octaves=3", {});
  gFix["H"] = Make("affine", "mul=30,add=20", {{"a", gFix["A"]}});
  gFix["L"] = Make("const", "width=65,height=65,value=20", {});
  gFix["M"] = Make("voronoi", "width=65,height=65,scale=14,seed=5", {});
  gFix["B"] = Make("perlin", "width=9,height=7,seed=3,scale=5,ox=4,oz=6", {});
}

struct Case
{
  std::string label, op, params, inputs, golden;
};

static std::vector<std::string> gLines;
static std::vector<Case> gCases;
static std::size_t gBegin = 0, gEnd = 0;

static bool LoadCases(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    gLines.push_back(line);
  }
  bool inside = false;
  for (std::size_t i = 0; i < gLines.size(); ++i) {
    const std::string& l = gLines[i];
    if (!inside) {
      if (l.find("Cases = [[") == 0) {
        inside = true;
        gBegin = i + 1;
      }
      continue;
    }
    if (l.find("]]") == 0) {
      gEnd = i;
      break;
    }
    if (l.empty() || l[0] == '#') continue;
    const std::vector<std::string> f = Split(l, '|');
    if (f.size() < 5) continue;
    gCases.push_back({f[0], f[1], f[2], f[3], f[4]});
  }
  return !gCases.empty();
}

static std::map<std::string, GridPtr> InputsOf(const std::string& spec)
{
  std::map<std::string, GridPtr> in;
  for (const std::string& kv : Split(spec, ',')) {
    const std::size_t eq = kv.find('=');
    if (eq != std::string::npos) in[kv.substr(0, eq)] = gFix[kv.substr(eq + 1)];
  }
  return in;
}

static void TestGoldens(const bool update)
{
  const int orig = Threads();
  const int counts[] = {1, 2, 3, 5, 8};
  std::vector<std::string> now;
  for (const int tc : counts) {
    SetThreads(tc);
    BuildFixtures();
    for (std::size_t i = 0; i < gCases.size(); ++i) {
      const Case& c = gCases[i];
      std::string err;
      const GridPtr g = Make(c.op, c.params, InputsOf(c.inputs), &err);
      if (!g) {
        CHECK(false, "case %s: %s", c.label.c_str(), err.c_str());
        if (tc == counts[0]) now.push_back("0");
        continue;
      }
      const std::string hex = Hex(Checksum(*g));
      if (tc == counts[0]) {
        now.push_back(hex);
      } else {
        CHECK(hex == now[i], "case %s differs with %d threads (%s vs %s)", c.label.c_str(), tc, hex.c_str(), now[i].c_str());
      }
    }
  }
  SetThreads(orig);
  int wrong = 0;
  for (std::size_t i = 0; i < gCases.size(); ++i) {
    if (gCases[i].golden != now[i]) {
      ++wrong;
      if (!update) std::printf("golden %s: file %s, now %s\n", gCases[i].label.c_str(), gCases[i].golden.c_str(), now[i].c_str());
    }
  }
  if (update) {
    std::size_t ci = 0;
    for (std::size_t i = gBegin; i < gEnd; ++i) {
      std::string& l = gLines[i];
      if (l.empty() || l[0] == '#') continue;
      std::vector<std::string> f = Split(l, '|');
      if (f.size() < 5) continue;
      f[4] = now[ci++];
      l = f[0] + "|" + f[1] + "|" + f[2] + "|" + f[3] + "|" + f[4];
    }
    std::printf("updated %d goldens\n", wrong);
  } else {
    CHECK(wrong == 0, "%d goldens differ (run with --update after checking the change is intended)", wrong);
  }
}

// ---- properties -------------------------------------------------------------------------------------------------------------------------------------

static GridPtr Run(const char* op, const std::string& params, std::map<std::string, GridPtr> in = {})
{
  GridPtr g = Make(op, params, in);
  CHECK(g != nullptr, "op %s failed", op);
  return g ? g : std::make_shared<Grid>(1, 1, 1.0f, 0.0f, 0.0f);
}

/** 8 numbers per point from a list of (x, z, y, w, f, bank, off). */
static std::string Pts(const std::vector<std::vector<double>>& pts)
{
  std::string out = "pts=";
  double s = 0.0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    if (i > 0) {
      s += std::hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
      out += ";";
    }
    const std::vector<double>& p = pts[i];
    char b[200];
    std::snprintf(b, sizeof(b), "%.9g:%.9g:%.9g:%.9g:%.9g:%.9g:%.9g:%.9g", p[0], p[1], p[2], p[3], p[4], p[5], p[6], s);
    out += b;
  }
  return out;
}

static double DistToSegment(const double px, const double pz, const double ax, const double az, const double bx, const double bz)
{
  const double dx = bx - ax, dz = bz - az;
  const double len2 = dx * dx + dz * dz;
  double t = len2 > 0 ? ((px - ax) * dx + (pz - az) * dz) / len2 : 0.0;
  t = std::max(0.0, std::min(1.0, t));
  return std::hypot(px - (ax + dx * t), pz - (az + dz * t));
}

static void TestDistance()
{
  BuildFixtures();
  const std::vector<std::vector<double>> pts = {{6, 8, 30, 5, 3, 0, 0}, {20, 14, 34, 6, 3, 0, 0}, {34, 26, 38, 6, 3, 0, 0}, {46, 44, 40, 5, 3, 0, 0}, {58, 56, 44, 5, 3, 0, 0}};
  const GridPtr d = Run("path_dist", Pts(pts) + ",far=-1", {{"like", gFix["L"]}});
  int inside = 0;
  for (int z = 0; z < 65; ++z) {
    for (int x = 0; x < 65; ++x) {
      double best = 1e9;
      for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        best = std::min(best, DistToSegment(x, z, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]));
      }
      const float v = d->At(x, z);
      if (v >= 0.0f) {
        ++inside;
        CHECK(best < 7.99 ? std::fabs(v - best) < 1e-3 : v >= best - 1e-3, "distance at %d,%d: %g, brute force %g", x, z, v, best);
      } else {
        CHECK(best > 7.99, "(%d,%d) was left out of reach but is %g from the path", x, z, best);
      }
    }
  }
  CHECK(inside > 400, "too few samples in reach (%d)", inside);
  // the samples ON the polyline are 0
  CHECK(d->At(6, 8) == 0.0f, "the first point is not at distance 0");
}

static void TestSigned()
{
  BuildFixtures();
  const GridPtr d = Run("path_dist", Pts({{4, 32, 0, 6, 0, 0, 0}, {60, 32, 0, 6, 0, 0, 0}}) + ",signed=1,far=99", {{"like", gFix["L"]}});
  CHECK(d->At(30, 32) == 0.0f, "on the path");
  CHECK(d->At(30, 35) > 2.9f && d->At(30, 29) < -2.9f, "signs: %g and %g (opposite sides must differ)", d->At(30, 35), d->At(30, 29));
  CHECK(std::fabs(d->At(30, 35) + d->At(30, 29)) < 1e-4, "the two sides are not symmetric");
  CHECK(d->At(30, 50) == 99.0f, "out of reach is `far`");
  const GridPtr u = Run("path_u", Pts({{4, 32, 0, 4, 2, 0, 0}, {60, 32, 0, 4, 2, 0, 0}}), {{"like", gFix["L"]}});
  CHECK(std::fabs(u->At(30, 35) - 0.5f) < 1e-4, "u at 3 units of w+f=6: %g", u->At(30, 35));
  CHECK(u->At(30, 45) == 2.0f, "u out of reach");
  const GridPtr s = Run("path_along", Pts({{4, 32, 0, 6, 0, 0, 0}, {60, 32, 0, 6, 0, 0, 0}}) + ",normalize=1", {{"like", gFix["L"]}});
  CHECK(std::fabs(s->At(32, 32) - 0.5f) < 1e-4, "along the middle: %g", s->At(32, 32));
  CHECK(s->At(32, 50) == -1.0f, "along out of reach");
}

static void TestShape()
{
  BuildFixtures();
  const std::string road = Pts({{4, 32, 50, 4, 0, 0, 0}, {60, 32, 50, 4, 0, 0, 0}});
  GridPtr t = Run("path_shape", road + ",mode=target", {{"a", gFix["H"]}});
  for (int x = 6; x < 58; ++x) {
    CHECK(t->At(x, 32) == 50.0f, "target height on the centre line at %d: %g", x, t->At(x, 32));
    CHECK(t->At(x, 40) == gFix["H"]->At(x, 40), "outside the half width the terrain must stay");
  }
  // clip min: only lowers; clip max: only raises
  const GridPtr lo = Run("path_shape", road + ",mode=target,clip=min", {{"a", gFix["H"]}});
  const GridPtr hi = Run("path_shape", road + ",mode=target,clip=max", {{"a", gFix["H"]}});
  for (int x = 6; x < 58; ++x) {
    CHECK(lo->At(x, 32) <= gFix["H"]->At(x, 32) && hi->At(x, 32) >= gFix["H"]->At(x, 32), "clip at %d", x);
    CHECK(lo->At(x, 32) == std::min(50.0f, gFix["H"]->At(x, 32)) && hi->At(x, 32) == std::max(50.0f, gFix["H"]->At(x, 32)), "clip value at %d", x);
  }
  // offset mode with a V profile: the full offset on the line, half of it at half the width
  const GridPtr v = Run("path_shape", Pts({{4, 32, 0, 8, 0, 0, -10}, {60, 32, 0, 8, 0, 0, -10}}) + ",profile=0:1;1:0,smooth=0", {{"a", gFix["L"]}});
  CHECK(std::fabs(v->At(30, 32) - 10.0f) < 1e-4, "V valley bottom: %g", v->At(30, 32));
  CHECK(std::fabs(v->At(30, 36) - 15.0f) < 1e-4, "V valley half way: %g", v->At(30, 36));
  CHECK(std::fabs(v->At(30, 40) - 20.0f) < 1e-4, "V valley rim: %g", v->At(30, 40));
  // bank: tilts the target section
  const GridPtr b = Run("path_shape", Pts({{4, 32, 50, 6, 0, 10, 0}, {60, 32, 50, 6, 0, 10, 0}}) + ",mode=target", {{"a", gFix["L"]}});
  CHECK(std::fabs(b->At(30, 36) - b->At(30, 28) - 8.0f * static_cast<float>(std::tan(10.0 * 3.14159265358979 / 180.0))) < 1e-3, "bank tilt: %g vs %g", b->At(30, 36), b->At(30, 28));
  // strength 0 changes nothing
  const GridPtr z = Run("path_shape", road + ",mode=target,strength=0", {{"a", gFix["H"]}});
  CHECK(Checksum(*z) == Checksum(*gFix["H"]), "strength 0 must leave the field alone");
  // falloff: the weight fades out beyond the half width
  const GridPtr fo = Run("path_shape", Pts({{4, 32, 50, 4, 6, 0, 0}, {60, 32, 50, 4, 6, 0, 0}}) + ",mode=target", {{"a", gFix["L"]}});
  CHECK(fo->At(30, 32 + 4) == 50.0f, "full weight up to the half width");
  CHECK(fo->At(30, 32 + 7) > 20.0f && fo->At(30, 32 + 7) < 50.0f, "falloff value %g", fo->At(30, 32 + 7));
  CHECK(fo->At(30, 32 + 10) == 20.0f, "no effect beyond w + f");
}

static void TestMask()
{
  BuildFixtures();
  const std::string p = Pts({{4, 32, 0, 6, 4, 0, 0}, {60, 32, 0, 6, 4, 0, 0}});
  const GridPtr core = Run("path_mask", p + ",kind=core", {{"like", gFix["L"]}});
  const GridPtr reach = Run("path_mask", p + ",kind=reach,soft=1", {{"like", gFix["L"]}});
  const GridPtr bank = Run("path_mask", p + ",kind=bank,soft=1", {{"like", gFix["L"]}});
  CHECK(core->At(30, 32) == 1.0f && core->At(30, 38) == 1.0f && core->At(30, 42) == 0.0f, "core mask");
  CHECK(reach->At(30, 41) > 0.99f && reach->At(30, 44) == 0.0f, "reach mask %g %g", reach->At(30, 41), reach->At(30, 44));
  CHECK(bank->At(30, 32) == 0.0f && bank->At(30, 41) > 0.1f, "bank mask");
  for (int i = 0; i < 65 * 65; ++i) {
    CHECK(core->v[static_cast<std::size_t>(i)] >= 0.0f && core->v[static_cast<std::size_t>(i)] <= 1.0f, "mask range");
  }
  // an asymmetric profile reads the two sides differently
  const GridPtr as = Run("path_mask", Pts({{4, 32, 0, 6, 0, 0, 0}, {60, 32, 0, 6, 0, 0, 0}}) + ",symmetric=0,profile=0:0;0.5:0.5;1:1,smooth=0", {{"like", gFix["L"]}});
  CHECK(as->At(30, 38) == 1.0f && as->At(30, 26) == 0.0f && std::fabs(as->At(30, 32) - 0.5f) < 1e-5, "asymmetric profile: %g %g %g", as->At(30, 38), as->At(30, 26), as->At(30, 32));
}

static void TestPaste()
{
  BuildFixtures();
  const GridPtr a = gFix["H"];
  const GridPtr b = gFix["B"];
  const GridPtr added = Run("path_paste", "mode=add", {{"a", a}, {"b", b}});
  const GridPtr back = Run("path_paste", "mode=sub", {{"a", added}, {"b", b}});
  for (int z = 0; z < 65; ++z) {
    for (int x = 0; x < 65; ++x) {
      const bool inB = x >= 4 && x < 13 && z >= 6 && z < 13;
      const float want = inB ? a->At(x, z) + b->At(x - 4, z - 6) : a->At(x, z);
      CHECK(added->At(x, z) == want, "paste add at %d,%d", x, z);
      CHECK(std::fabs(back->At(x, z) - a->At(x, z)) < 1e-4, "add then sub is not the identity at %d,%d", x, z);
    }
  }
  const GridPtr set = Run("path_paste", "mode=set,scale=2", {{"a", a}, {"b", b}});
  CHECK(set->At(4, 6) == b->At(0, 0) * 2.0f, "paste set");
  std::string err;
  const GridPtr shifted = Make("path_paste", "mode=add", {{"a", a}, {"b", Run("const", "width=3,height=3,value=1,ox=0.5,oz=0")}}, &err);
  CHECK(!shifted && !err.empty(), "a half sample offset must be refused");
}

static void TestErrors()
{
  BuildFixtures();
  std::string err;
  CHECK(!Make("path_dist", "", {{"like", gFix["L"]}}, &err), "no pts must fail");
  CHECK(!Make("path_dist", "pts=1:2:3", {{"like", gFix["L"]}}, &err), "a bad pts length must fail");
  CHECK(!Make("path_shape", Pts({{1, 1, 0, 2, 0, 0, 0}}) + ",mode=fly", {{"a", gFix["L"]}}, &err), "a bad mode must fail");
  CHECK(!Make("path_shape", Pts({{1, 1, 0, 2, 0, 0, 0}}), {{"like", gFix["L"]}}, &err), "path_shape needs a");
  // a single point is a disc
  const GridPtr disc = Run("path_dist", Pts({{32, 32, 0, 5, 0, 0, 0}}) + ",far=-1", {{"like", gFix["L"]}});
  CHECK(std::fabs(disc->At(35, 36) - 5.0f) < 1e-4, "disc distance %g", disc->At(35, 36));
  CHECK(disc->At(40, 40) == -1.0f, "disc reach");
}

static void TestThreadsLarge()
{
  // a long wiggly path on a 513 x 513 grid: the result must not depend on the number of threads
  const int orig = Threads();
  std::vector<std::vector<double>> pts;
  for (int i = 0; i < 80; ++i) {
    const double t = i / 79.0;
    pts.push_back({20 + t * 470, 256 + 150 * std::sin(t * 9.0), 30 + 10 * std::sin(t * 4.0), 6 + 3 * std::sin(t * 7.0), 8, 4 * std::sin(t * 5.0), -5, 0});
  }
  std::uint64_t first = 0;
  for (const int tc : {1, 2, 4, 7, 12}) {
    SetThreads(tc);
    const GridPtr h = Run("perlin", "width=513,height=513,seed=3,scale=90,octaves=5");
    const GridPtr s = Run("path_shape", Pts(pts) + ",mode=target,profile=0:1;0.7:1;1:0.2", {{"a", h}});
    const GridPtr m = Run("path_mask", Pts(pts) + ",kind=bank", {{"like", h}});
    const std::uint64_t sum = Checksum(*s) * 31 + Checksum(*m);
    if (tc == 1) first = sum;
    else CHECK(sum == first, "result differs with %d threads", tc);
  }
  SetThreads(orig);
}

int main(int argc, char** argv)
{
  std::printf("EdPathTest: %d threads, %d ops\n", Threads(), int(OpNames().size()));
  const std::string cases = argc > 1 ? argv[1] : "";
  const bool update = argc > 2 && std::string(argv[2]) == "--update";
  if (!cases.empty() && cases.find("--") != 0) {
    if (LoadCases(cases)) {
      TestGoldens(update);
      if (update) {
        std::ofstream out(cases, std::ios::binary | std::ios::trunc);
        for (const std::string& l : gLines) out << l << "\n";
      }
    } else {
      CHECK(false, "cannot read the cases file %s", cases.c_str());
    }
  } else {
    std::printf("(no cases file: goldens skipped)\n");
  }
  TestDistance();
  TestSigned();
  TestShape();
  TestMask();
  TestPaste();
  TestErrors();
  TestThreadsLarge();
  std::printf(gFailures == 0 ? "EdPathTest PASS\n" : "EdPathTest FAIL (%d)\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
