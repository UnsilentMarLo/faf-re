// Stand-alone test of the stage T7 ops of the field engine (EdFieldGen.cpp, EdFieldFilt.cpp, EdFieldEro2.cpp; no engine needed).
//   build + run: python scripts/ed_t7_test.py [--update] [--bench]
//   EdT7Test.exe <cases.lua> [--update]     golden cases in editor/runtime/lua/editor/selftest_t7_cases.lua (shared with the Lua self test selftest_t7.lua); --update rewrites the golden
//                                             column from this run (it first proves the results do not depend on the thread count)
//   A case line is label | op[@extra] | params | inputs | checksum; op@name checks the extra output `name` of an op with several results.
//   EdT7Test.exe <cases.lua> --bench        timings of every op on a 1025 x 1025 grid at 1 and N threads
#include <algorithm>
#include <chrono>
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

/** Runs an op; `extras` (when given) receives the additional results. */
static GridPtr Make(const std::string& op, const std::string& params, const std::map<std::string, GridPtr>& in, std::string* err = nullptr, std::map<std::string, Grid>* extras = nullptr)
{
  OpArgs a;
  ParseParams(params, a.p);
  a.in = in;
  std::map<std::string, Grid> local;
  a.extra = extras != nullptr ? extras : &local;
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
  gFix["H"] = Make("affine", "mul=30,add=20", {{"a", gFix["A"]}});                    // gentle heights 20..50
  gFix["G"] = Make("affine", "mul=3,add=20", {{"a", gFix["A"]}});                     // very gentle heights
  gFix["R"] = Make("ridged", "width=65,height=65,seed=4,scale=24,octaves=4", {});
  gFix["T"] = Make("affine", "mul=70,add=0", {{"a", gFix["R"]}});                     // mountains 0..70
  gFix["L"] = Make("const", "width=65,height=65,value=20", {});
  gFix["M"] = Make("voronoi", "width=65,height=65,scale=14,seed=5", {});
  gFix["N"] = Make("perlin", "width=65,height=65,seed=9,scale=16,octaves=2", {});
  gFix["P"] = Make("threshold", "value=0.45", {{"a", gFix["N"]}});
  gFix["S"] = Make("affine", "mul=4,add=0", {{"a", gFix["N"]}});                      // a soil thickness field 0..4
  gFix["W"] = Make("affine", "mul=0.6,add=0.3", {{"a", gFix["N"]}});                  // a talus map
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

/** Runs a case; returns the checksum of the main result, or of the extra named after `@`. false when the op failed. */
static bool RunCase(const Case& c, std::string& hex, std::string& err)
{
  std::string op = c.op, extra;
  if (const std::size_t at = op.find('@'); at != std::string::npos) {
    extra = op.substr(at + 1);
    op = op.substr(0, at);
  }
  std::map<std::string, Grid> extras;
  const GridPtr g = Make(op, c.params, InputsOf(c.inputs), &err, &extras);
  if (!g) return false;
  if (extra.empty()) {
    hex = Hex(Checksum(*g));
  } else {
    const auto it = extras.find(extra);
    if (it == extras.end()) {
      err = "no extra output '" + extra + "'";
      return false;
    }
    hex = Hex(Checksum(it->second));
  }
  return true;
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
      std::string err, hex;
      if (!RunCase(c, hex, err)) {
        CHECK(false, "case %s: %s", c.label.c_str(), err.c_str());
        if (tc == counts[0]) now.push_back("0");
        continue;
      }
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

static GridPtr Run(const char* op, const std::string& params, std::map<std::string, GridPtr> in = {}, std::map<std::string, Grid>* extras = nullptr)
{
  GridPtr g = Make(op, params, in, nullptr, extras);
  CHECK(g != nullptr, "op %s failed", op);
  return g ? g : std::make_shared<Grid>(1, 1, 1.0f, 0.0f, 0.0f);
}

static bool InRange(const Grid& g, const float lo, const float hi)
{
  for (const float v : g.v) {
    if (!(v >= lo && v <= hi)) return false;   // also false for NaN
  }
  return true;
}

static double Sum(const Grid& g)
{
  double s = 0.0;
  for (const float v : g.v) s += v;
  return s;
}

#include "EdT7Properties.inc"
#include "EdT7Lab.inc"

// ---- bench ------------------------------------------------------------------------------------------------------------------------------------------

static double TimeMs(const std::string& op, const std::string& params, const std::map<std::string, GridPtr>& in, const int reps = 1)
{
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < reps; ++i) {
    if (!Make(op, params, in, &err)) {
      std::printf("bench: %s failed: %s\n", op.c_str(), err.c_str());
      return -1.0;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
}

static void Bench()
{
  const int orig = Threads();
  const int counts[] = {1, orig};
  std::printf("bench 1025 x 1025: ms per run (1 thread / %d threads)\n", orig);
  std::map<std::string, std::pair<double, double>> res;
  std::vector<std::string> order;
  for (const int tc : counts) {
    SetThreads(tc);
    std::map<std::string, GridPtr> in;
    in["a"] = Make("affine", "mul=60,add=10", {{"a", Make("ridged", "width=1025,height=1025,seed=3,scale=180,octaves=6", {})}});
    in["b"] = Make("perlin", "width=1025,height=1025,seed=9,scale=100,octaves=3", {});
    in["mask"] = in["b"];
    const std::map<std::string, GridPtr> a = {{"a", in["a"]}};
    const std::map<std::string, GridPtr> ab = {{"a", in["a"]}, {"b", in["b"]}};
    const std::map<std::string, GridPtr> like = {{"like", in["a"]}};
    const auto B = [&](const char* name, const std::string& op, const std::string& params, const std::map<std::string, GridPtr>& inp, const int reps = 1) {
      const double ms = TimeMs(op, params, inp, reps);
      auto& r = res[name];
      if (tc == counts[0]) {
        r.first = ms;
        order.push_back(name);
      } else {
        r.second = ms;
      }
    };
#include "EdT7Bench.inc"
  }
  SetThreads(orig);
  for (const std::string& k : order) {
    const auto& r = res[k];
    std::printf("  %-34s %9.1f  %9.1f   x%.1f\n", k.c_str(), r.first, r.second, r.second > 0 ? r.first / r.second : 0.0);
  }
}

int main(int argc, char** argv)
{
  std::printf("EdT7Test: %d threads, %d ops\n", Threads(), int(OpNames().size()));
  const std::string cases = argc > 1 ? argv[1] : "";
  const bool update = argc > 2 && std::string(argv[2]) == "--update";
  const bool bench = argc > 2 && std::string(argv[2]) == "--bench";
  if (argc > 9 && std::string(argv[2]) == "--img") {   // --img <outdir> <op> <params> <kind 0 relief|1 grey> <lo> <hi> <size> [inputs a=H,...]  (the size is added to the params unless an input is given)
    BuildFixtures();
    const std::string size = argv[9];
    const std::map<std::string, GridPtr> in = argc > 10 ? InputsOf(argv[10]) : std::map<std::string, GridPtr>{};
    std::string params = argv[5];
    if (in.empty()) params = "width=" + size + ",height=" + size + (params.empty() ? "" : "," + params);
    const GridPtr g = Make(argv[4], params, in);
    if (!g) return 1;
    const Stats st = Measure(*g);
    std::printf("min %g max %g mean %g sd %g\n", st.min, st.max, st.mean, st.sd);
    Sheet sh(std::atoi(size.c_str()) > 400 ? 400 : std::atoi(size.c_str()), 1, 1);
    sh.Add("one", *g, std::atoi(argv[6]), static_cast<float>(std::atof(argv[7])), static_cast<float>(std::atof(argv[8])));
    sh.Save(argv[3], "one");
    return 0;
  }
  if (argc > 3 && std::string(argv[2]) == "--lab") {
    BuildFixtures();
    Lab(argv[3], argc > 4 ? std::atoi(argv[4]) : 257);
    return 0;
  }
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
  BuildFixtures();
  RunProperties();
  if (bench) Bench();
  std::printf(gFailures == 0 ? "EdT7Test PASS\n" : "EdT7Test FAIL (%d)\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
