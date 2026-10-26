// Stand-alone test of the Supreme Commander ops of the field engine (EdFieldSc.cpp: pad_flatten, symmetrize, slope_limit, reach, regions; no engine needed).
//   build + run: python scripts/ed_sc_test.py
//   EdScTest.exe <cases.lua> [--update]     golden cases in editor/runtime/lua/editor/selftest_sc_cases.lua (shared with the Lua self test selftest_sctools.lua);
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
  gFix["G"] = Make("affine", "mul=3,add=20", {{"a", gFix["A"]}});
  gFix["L"] = Make("const", "width=65,height=65,value=20", {});
  gFix["M"] = Make("voronoi", "width=65,height=65,scale=14,seed=5", {});
  gFix["N"] = Make("perlin", "width=65,height=65,seed=9,scale=16,octaves=2", {});
  gFix["P"] = Make("threshold", "value=0.45", {{"a", gFix["N"]}});
  gFix["C"] = Make("affine", "mul=3,add=1", {{"a", gFix["M"]}});
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

static void TestPads()
{
  BuildFixtures();
  const GridPtr H = gFix["H"];
  // a pad with its own level: the core (snapped outward to whole cells) is one value, the mean of the input core, quantised to 1/128
  const GridPtr o = Run("pad_flatten", "pads=20.3:20.4:3:5:0:0", {{"a", H}});
  // core: x in [floor(17.3), ceil(23.3)] = [17, 24], z in [floor(17.4), ceil(23.4)] = [17, 24]
  double sum = 0;
  int n = 0;
  for (int z = 17; z <= 24; ++z) {
    for (int x = 17; x <= 24; ++x) {
      sum += H->At(x, z);
      ++n;
    }
  }
  const double want = std::floor(sum / n * 128.0 + 0.5) / 128.0;
  for (int z = 17; z <= 24; ++z) {
    for (int x = 17; x <= 24; ++x) {
      CHECK(o->At(x, z) == static_cast<float>(want), "core sample %d,%d = %g, want %g", x, z, o->At(x, z), want);
    }
  }
  // one sample outside the core on the skirt side is between input and target, far outside nothing changes
  CHECK(o->At(26, 20) != H->At(26, 20) && std::fabs(o->At(26, 20) - want) < std::fabs(H->At(26, 20) - want) + 1e-4, "skirt moves towards the target");
  CHECK(o->At(40, 40) == H->At(40, 40) && o->At(20, 40) == H->At(20, 40), "outside the skirt the field stays");
  CHECK(o->At(29, 20) == H->At(29, 20), "the skirt ends at 5 units beyond the core");
  // strength 0 changes nothing, weights are in 0..1
  const GridPtr z0 = Run("pad_flatten", "pads=20:20:3:5:0:0,strength=0", {{"a", H}});
  CHECK(Checksum(*z0) == Checksum(*H), "strength 0 must leave the field alone");
  const GridPtr wgt = Run("pad_flatten", "pads=20:20:3:5:0:0;44:40:2:8:0:0,output=weight", {{"a", H}});
  for (std::size_t i = 0; i < wgt->Count(); ++i) CHECK(wgt->v[i] >= 0.0f && wgt->v[i] <= 1.0f, "weight range");
  CHECK(wgt->At(20, 20) == 1.0f && wgt->At(60, 5) == 0.0f, "weight inside and far away");
  // a group shares one height; an absolute pad takes y (quantised to words)
  const GridPtr g = Run("pad_flatten", "pads=12:12:2:3:0:1;50:14:2:3:0:1;30:48:2:3:33.3333:-1", {{"a", H}});
  CHECK(g->At(12, 12) == g->At(50, 14) && g->At(11, 10) == g->At(51, 16), "pads of one group differ");
  CHECK(g->At(30, 48) == static_cast<float>(std::floor(33.3333 * 128.0 + 0.5) / 128.0), "absolute pad height %g", g->At(30, 48));
  // build grid alignment: with grid=4 the core edges lie on multiples of 4
  const GridPtr al = Run("pad_flatten", "pads=21:21:2:0:0:0,grid=4", {{"a", H}});
  int flat = 0;
  for (int z = 0; z < 65; ++z) for (int x = 0; x < 65; ++x) if (al->At(x, z) != H->At(x, z)) ++flat;
  CHECK(al->At(16, 16) == al->At(24, 24) && al->At(15, 21) == H->At(15, 21) && al->At(25, 21) == H->At(25, 21), "grid 4 core is [16, 24]");
  CHECK(flat <= 81, "grid aligned core touches %d samples", flat);
  // level: min <= mean <= max
  const GridPtr lmin = Run("pad_flatten", "pads=20:20:3:0:0:0,level=1", {{"a", H}});
  const GridPtr lmax = Run("pad_flatten", "pads=20:20:3:0:0:0,level=2", {{"a", H}});
  const GridPtr lmean = Run("pad_flatten", "pads=20:20:3:0:0:0,level=0", {{"a", H}});
  CHECK(lmin->At(20, 20) <= lmean->At(20, 20) && lmean->At(20, 20) <= lmax->At(20, 20), "level order");
  // later pads win where they overlap
  const GridPtr ov = Run("pad_flatten", "pads=20:20:3:0:10:-1;23:20:3:0:50:-1", {{"a", H}});
  CHECK(ov->At(21, 20) == 50.0f && ov->At(18, 20) == 10.0f, "overlap order: %g %g", ov->At(21, 20), ov->At(18, 20));
  std::string err;
  CHECK(!Make("pad_flatten", "pads=1:2:3", {{"a", H}}, &err), "bad pad list must fail");
  CHECK(!Make("pad_flatten", "pads=1:2:3:4:5:6,output=x", {{"a", H}}, &err), "bad output must fail");
}

static void TestSymmetry()
{
  BuildFixtures();
  const GridPtr H = gFix["H"];
  const auto Sym = [&](const char* mode, const char* extra = "") { return Run("symmetrize", std::string("mode=") + mode + extra, {{"a", H}}); };
  const GridPtr mx = Sym("mirror_x");
  const GridPtr mz = Sym("mirror_z");
  const GridPtr mxz = Sym("mirror_xz");
  const GridPtr r2 = Sym("rot2");
  const GridPtr r4 = Sym("rot4");
  const GridPtr dg = Sym("diag");
  const GridPtr ad = Sym("adiag");
  const GridPtr db = Sym("diag_both");
  const int N = 64;
  for (int z = 0; z <= N; ++z) {
    for (int x = 0; x <= N; ++x) {
      CHECK(mx->At(x, z) == mx->At(N - x, z), "mirror_x at %d,%d", x, z);
      CHECK(mz->At(x, z) == mz->At(x, N - z), "mirror_z at %d,%d", x, z);
      CHECK(mxz->At(x, z) == mxz->At(N - x, z) && mxz->At(x, z) == mxz->At(x, N - z), "mirror_xz at %d,%d", x, z);
      CHECK(r2->At(x, z) == r2->At(N - x, N - z), "rot2 at %d,%d", x, z);
      CHECK(r4->At(x, z) == r4->At(N - z, x) && r4->At(x, z) == r4->At(z, N - x), "rot4 at %d,%d", x, z);
      CHECK(dg->At(x, z) == dg->At(z, x), "diag at %d,%d", x, z);
      CHECK(ad->At(x, z) == ad->At(N - z, N - x), "adiag at %d,%d", x, z);
      CHECK(db->At(x, z) == db->At(z, x) && db->At(x, z) == db->At(N - z, N - x), "diag_both at %d,%d", x, z);
    }
  }
  // the master wedge is untouched, and the other side takes it over with side=1
  for (int z = 0; z <= N; ++z) {
    for (int x = 0; x <= 32; ++x) CHECK(mx->At(x, z) == H->At(x, z), "mirror_x master half at %d,%d", x, z);
  }
  const GridPtr mx1 = Sym("mirror_x", ",side=1");
  for (int z = 0; z <= N; ++z) {
    for (int x = 32; x <= N; ++x) CHECK(mx1->At(x, z) == H->At(x, z), "mirror_x side 1 master half at %d,%d", x, z);
    CHECK(mx1->At(3, z) == H->At(N - 3, z), "side 1 copies the right half");
  }
  for (int z = 0; z <= 32; ++z) {
    for (int x = 0; x <= 32; ++x) CHECK(mxz->At(x, z) == H->At(x, z), "mirror_xz master quadrant at %d,%d", x, z);
  }
  // it agrees with the mirror op of the core (exact sample copies) for the same master half
  const GridPtr coreX = Run("mirror", "mode=x,side=0", {{"a", H}});
  const GridPtr coreXZ = Run("mirror", "mode=xz,side=0", {{"a", H}});
  CHECK(Checksum(*coreX) == Checksum(*mx), "mirror_x differs from the core mirror op");
  CHECK(Checksum(*coreXZ) == Checksum(*mxz), "mirror_xz differs from the core mirror op");
  // combine
  const GridPtr avg = Sym("mirror_x", ",combine=avg");
  const GridPtr mxm = Sym("mirror_x", ",combine=max");
  const GridPtr mnm = Sym("mirror_x", ",combine=min");
  for (int x = 0; x <= N; x += 3) {
    const float a = H->At(x, 20), b = H->At(N - x, 20);
    CHECK(std::fabs(avg->At(x, 20) - 0.5f * (a + b)) < 1e-4f, "avg at %d", x);
    CHECK(mxm->At(x, 20) == std::max(a, b) && mnm->At(x, 20) == std::min(a, b), "max / min at %d", x);
  }
  // N-fold turns: master wedge kept, values between the extremes of the input, rot3 about a custom centre
  const GridPtr r3 = Sym("rotn", ",n=3,cx=30.5,cz=34.25");
  const Stats st = Measure(*H);
  for (std::size_t i = 0; i < r3->Count(); ++i) {
    CHECK(std::isfinite(r3->v[i]) && r3->v[i] >= st.min - 1e-3f && r3->v[i] <= st.max + 1e-3f, "rotn value out of range");
  }
  int same = 0, total = 0;
  for (int z = 0; z <= N; ++z) for (int x = 0; x <= N; ++x) { ++total; if (r3->At(x, z) == H->At(x, z)) ++same; }
  CHECK(same > total / 4 && same < total, "rot3 keeps about a third (%d of %d)", same, total);
  // rotn with n=4 and angle 0 about the middle is a plain quarter turn symmetry too
  const GridPtr r4n = Sym("rotn", ",n=4");
  for (int z = 0; z <= N; z += 2) for (int x = 0; x <= N; x += 2) CHECK(r4n->At(x, z) == r4n->At(N - z, x), "rotn 4 at %d,%d", x, z);
  // a seam changes values only near the wedge borders
  const GridPtr seam = Sym("rotn", ",n=3,cx=32,cz=32,seam=6");
  const GridPtr noseam = Sym("rotn", ",n=3,cx=32,cz=32");
  int diff = 0;
  for (std::size_t i = 0; i < seam->Count(); ++i) if (seam->v[i] != noseam->v[i]) ++diff;
  CHECK(diff > 50 && diff < 2200, "seam changed %d samples", diff);
  std::string err;
  CHECK(!Make("symmetrize", "mode=spin", {{"a", H}}, &err), "bad mode must fail");
  CHECK(!Make("symmetrize", "mode=rotn,n=40", {{"a", H}}, &err), "n out of range must fail");
  CHECK(!Make("symmetrize", "mode=mirror_x,combine=sum", {{"a", H}}, &err), "bad combine must fail");
}

static void TestSlopeLimit()
{
  BuildFixtures();
  const GridPtr H = gFix["H"];
  const auto MaxDiff = [](const Grid& g) {
    double m = 0;
    for (int z = 0; z < g.h; ++z) for (int x = 0; x < g.w; ++x) {
      if (x + 1 < g.w) m = std::max(m, static_cast<double>(std::fabs(g.At(x + 1, z) - g.At(x, z))));
      if (z + 1 < g.h) m = std::max(m, static_cast<double>(std::fabs(g.At(x, z + 1) - g.At(x, z))));
    }
    return m / g.cell;
  };
  CHECK(MaxDiff(*H) > 1.5, "the fixture must be steep (%g)", MaxDiff(*H));
  for (const char* mode : {"lower", "raise", "both"}) {
    const GridPtr o = Run("slope_limit", std::string("limit=0.75,mode=") + mode, {{"a", H}});
    CHECK(MaxDiff(*o) <= 0.72 + 1e-4, "%s: steepest pair %g after the limit", mode, MaxDiff(*o));
    const Grid slopeRating = *Run("slope", "mode=rating,window=3", {{"a", o}});
    const Stats s = Measure(slopeRating);
    CHECK(s.max <= 0.72f + 1e-4f, "%s: engine rating %g (window 3)", mode, s.max);
    for (std::size_t i = 0; i < o->Count(); ++i) {
      if (std::string(mode) == "lower") CHECK(o->v[i] <= H->v[i] + 1e-4f, "lower must only cut");
      if (std::string(mode) == "raise") CHECK(o->v[i] >= H->v[i] - 1e-4f, "raise must only fill");
    }
    // a second run changes nothing (the result is already inside the limit)
    const GridPtr again = Run("slope_limit", std::string("limit=0.75,mode=") + mode, {{"a", o}});
    double md = 0;
    for (std::size_t i = 0; i < o->Count(); ++i) md = std::max(md, static_cast<double>(std::fabs(again->v[i] - o->v[i])));
    CHECK(md < 1e-3, "%s is not idempotent (%g)", mode, md);
  }
  // brute force: lower envelope min over q of h(q) + L * |p - q|_1 on a small field
  const GridPtr small = Run("perlin", "width=17,height=17,seed=2,scale=5,octaves=2");
  const GridPtr big = Run("affine", "mul=20,add=0", {{"a", small}});
  const GridPtr lo = Run("slope_limit", "limit=0.5,margin=0,mode=lower", {{"a", big}});
  const GridPtr hi = Run("slope_limit", "limit=0.5,margin=0,mode=raise", {{"a", big}});
  for (int z = 0; z < 17; ++z) {
    for (int x = 0; x < 17; ++x) {
      double bl = 1e30, bh = -1e30;
      for (int qz = 0; qz < 17; ++qz) for (int qx = 0; qx < 17; ++qx) {
        const double d = 0.5 * (std::abs(x - qx) + std::abs(z - qz));
        bl = std::min(bl, big->At(qx, qz) + d);
        bh = std::max(bh, big->At(qx, qz) - d);
      }
      CHECK(std::fabs(lo->At(x, z) - bl) < 1e-4, "lower envelope at %d,%d: %g vs %g", x, z, lo->At(x, z), bl);
      CHECK(std::fabs(hi->At(x, z) - bh) < 1e-4, "raise envelope at %d,%d: %g vs %g", x, z, hi->At(x, z), bh);
    }
  }
  // already gentle ground stays; strength 0 and an empty mask change nothing
  const GridPtr gentle = gFix["G"];
  const GridPtr keep = Run("slope_limit", "limit=0.75,mode=both", {{"a", gentle}});
  CHECK(MaxDiff(*gentle) < 0.7 && Checksum(*keep) == Checksum(*gentle), "gentle ground must stay (%g)", MaxDiff(*gentle));
  const GridPtr s0 = Run("slope_limit", "limit=0.3,strength=0", {{"a", H}});
  CHECK(Checksum(*s0) == Checksum(*H), "strength 0");
  const GridPtr zero = Run("const", "width=65,height=65,value=0");
  const GridPtr m0 = Run("slope_limit", "limit=0.3", {{"a", H}, {"mask", zero}});
  CHECK(Checksum(*m0) == Checksum(*H), "empty mask");
  std::string err;
  CHECK(!Make("slope_limit", "mode=melt", {{"a", H}}, &err), "bad mode must fail");
}

static void TestReach()
{
  BuildFixtures();
  const GridPtr open = Run("const", "width=65,height=65,value=1");
  const GridPtr d = Run("reach", "src=0:0", {{"a", open}});
  for (int z = 0; z < 65; z += 4) {
    for (int x = 0; x < 65; x += 4) {
      const int lo = std::min(x, z), hi = std::max(x, z);
      const double want = (hi - lo) + lo * 1.4142135623730951;
      CHECK(std::fabs(d->At(x, z) - want) < 1e-3, "open field distance at %d,%d: %g vs %g", x, z, d->At(x, z), want);
    }
  }
  // a wall with a gap: the way round is longer, a closed wall cuts the field in two
  OpArgs wallArgs;
  auto wall = std::make_shared<Grid>(65, 65, 1.0f, 0.0f, 0.0f, 1.0f);
  for (int z = 0; z < 65; ++z) wall->At(32, z) = 0.0f;
  const GridPtr closed = Run("reach", "src=5:32,unreach=-7", {{"a", wall}});
  CHECK(closed->At(60, 32) == -7.0f && closed->At(10, 32) > 0.0f && closed->At(32, 32) == -7.0f, "closed wall");
  wall->At(32, 10) = 1.0f;
  const GridPtr gap = Run("reach", "src=5:32", {{"a", wall}});
  CHECK(gap->At(60, 32) > 65.0f, "the way through the gap is long (%g)", gap->At(60, 32));
  CHECK(gap->At(60, 32) < 130.0f, "but finite (%g)", gap->At(60, 32));
  // no corner cutting: a diagonal wall of single samples is not a gap
  auto diag = std::make_shared<Grid>(33, 33, 1.0f, 0.0f, 0.0f, 1.0f);
  for (int i = 0; i < 33; ++i) diag->At(i, 32 - i) = 0.0f;
  const GridPtr dd = Run("reach", "src=2:2", {{"a", diag}});
  CHECK(dd->At(28, 28) == -1.0f, "a diagonal chain of blocked samples must stop the search (%g)", dd->At(28, 28));
  // cost multiplier doubles the distance
  const GridPtr two = Run("const", "width=65,height=65,value=2");
  const GridPtr costly = Run("reach", "src=0:0", {{"a", open}, {"b", two}});
  CHECK(std::fabs(costly->At(40, 0) - 80.0f) < 1e-3, "cost 2 doubles the way (%g)", costly->At(40, 0));
  // a source on a blocked sample snaps to the nearest open one
  auto blocked = std::make_shared<Grid>(33, 33, 1.0f, 0.0f, 0.0f, 0.0f);
  blocked->At(10, 10) = 1.0f;
  blocked->At(11, 10) = 1.0f;
  const GridPtr snapped = Run("reach", "src=8:10", {{"a", blocked}});
  CHECK(snapped->At(10, 10) == 0.0f && snapped->At(11, 10) == 1.0f, "snapped source");
  // regions
  auto two2 = std::make_shared<Grid>(20, 10, 2.0f, 0.0f, 0.0f, 0.0f);
  for (int z = 1; z < 5; ++z) for (int x = 1; x < 5; ++x) two2->At(x, z) = 1.0f;     // 16 samples
  for (int z = 6; z < 9; ++z) for (int x = 10; x < 19; ++x) two2->At(x, z) = 1.0f;   // 27 samples
  two2->At(5, 5) = 1.0f;                                                                // touches region 1 only diagonally
  const GridPtr lab = Run("regions", "", {{"a", two2}});
  CHECK(lab->At(1, 1) == 1.0f && lab->At(10, 6) == 3.0f && lab->At(0, 0) == 0.0f, "labels %g %g (the diagonal sample is its own region)", lab->At(1, 1), lab->At(10, 6));
  CHECK(lab->At(5, 5) == 2.0f, "corner-only contact is not a connection (label %g)", lab->At(5, 5));
  const GridPtr size = Run("regions", "mode=size", {{"a", two2}});
  CHECK(size->At(2, 2) == 16.0f * 4.0f && size->At(12, 7) == 27.0f * 4.0f && size->At(5, 5) == 4.0f, "region areas %g %g", size->At(2, 2), size->At(12, 7));
  const GridPtr lab4 = Run("regions", "connectivity=4", {{"a", two2}});
  CHECK(lab4->At(5, 5) == 2.0f, "4-connectivity");
  std::string err;
  CHECK(!Make("reach", "", {{"a", open}}, &err), "no sources must fail");
  CHECK(!Make("regions", "mode=x", {{"a", open}}, &err), "bad regions mode must fail");
}

static void TestThreadsLarge()
{
  const int orig = Threads();
  std::uint64_t first = 0;
  for (const int tc : {1, 2, 4, 7, 12}) {
    SetThreads(tc);
    const GridPtr hh = Run("perlin", "width=513,height=513,seed=3,scale=90,octaves=5");
    const GridPtr H = Run("affine", "mul=60,add=10", {{"a", hh}});
    const GridPtr pads = Run("pad_flatten", "pads=100:100:6:10:0:1;380:120:6:10:0:1;250:400:12:20:0:0;60:300:3:5:44:-1", {{"a", H}});
    const GridPtr sym = Run("symmetrize", "mode=rotn,n=5,seam=8,combine=copy", {{"a", pads}});
    const GridPtr sym2 = Run("symmetrize", "mode=diag_both,combine=avg", {{"a", sym}});
    const GridPtr lim = Run("slope_limit", "limit=0.75,mode=both", {{"a", sym2}});
    const GridPtr rating = Run("slope", "mode=rating,window=2", {{"a", lim}});
    const GridPtr ok = Run("threshold", "value=0.6", {{"a", Run("invert", "lo=0,hi=1", {{"a", Run("clamp", "lo=0,hi=1", {{"a", rating}})}})}});
    const GridPtr reach = Run("reach", "src=10:10;500:500", {{"a", ok}});
    const GridPtr reg = Run("regions", "", {{"a", ok}});
    const std::uint64_t sum = Checksum(*sym2) * 31 + Checksum(*lim) * 17 + Checksum(*reach) * 7 + Checksum(*reg);
    if (tc == 1) first = sum;
    else CHECK(sum == first, "result differs with %d threads", tc);
  }
  SetThreads(orig);
}

int main(int argc, char** argv)
{
  std::printf("EdScTest: %d threads, %d ops\n", Threads(), int(OpNames().size()));
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
  TestPads();
  TestSymmetry();
  TestSlopeLimit();
  TestReach();
  TestThreadsLarge();
  std::printf(gFailures == 0 ? "EdScTest PASS\n" : "EdScTest FAIL (%d)\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
