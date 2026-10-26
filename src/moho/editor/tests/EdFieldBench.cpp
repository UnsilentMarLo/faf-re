// CPU cost of the field ops that were candidates for a GPU version (docs/projects/editor/GPU_COMPUTE.md, "More field ops (GP4)"): runs each op once on a fractal terrain and prints the milliseconds.
// Not part of scripts/ed_native_tests.py (it measures, it does not check). Build by hand, with every file that registers field ops:
//   g++ -std=c++20 -O2 -ffp-contract=off -I src/sdk src/sdk/moho/editor/tests/EdFieldBench.cpp src/sdk/moho/editor/EdField{Core,Filt,Land,Gen,Sc,Path,Ero2,Interop}.cpp src/sdk/moho/editor/EdErosion.cpp -pthread -o EdFieldBench
//   EdFieldBench [size [op]]        size = samples per side (default 1025), op = only that op's cases
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "moho/editor/EdFieldCore.h"

using namespace moho::editor::field;

static std::vector<std::string> Split(const std::string& s, const char sep)
{
  std::vector<std::string> out;
  std::string cur;
  for (const char ch : s) {
    if (ch == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  out.push_back(cur);
  return out;
}

/** "k=v,k=v" -> params; a value "1:2:3" is a number array. */
static OpArgs Make(const std::string& text)
{
  OpArgs a;
  if (text.empty()) return a;
  for (const std::string& kv : Split(text, ',')) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (v.find(':') != std::string::npos) {
      std::vector<double> arr;
      for (const std::string& q : Split(v, ':')) arr.push_back(std::atof(q.c_str()));
      a.p.arr[k] = arr;
      continue;
    }
    char* end = nullptr;
    const double n = std::strtod(v.c_str(), &end);
    if (!v.empty() && *end == 0) a.p.num[k] = n;
    else a.p.str[k] = v;
  }
  return a;
}

int main(int argc, char** argv)
{
  const int n = argc > 1 ? std::atoi(argv[1]) : 1025;
  const std::string only = argc > 2 ? argv[2] : "";
  std::string err;
  Grid terrain;
  if (!RunOpCpu("fractal", Make("width=" + std::to_string(n) + ",height=" + std::to_string(n) + ",cell=2,scale=500,octaves=9,style=erosive,erosion=1,seed=3"), terrain, err)) {
    std::printf("fractal: %s\n", err.c_str());
    return 1;
  }
  for (float& v : terrain.v) v *= 250.0f;
  const auto ter = std::make_shared<Grid>(terrain);
  OpArgs sa = Make("mode=gradient");
  sa.in["a"] = ter;
  Grid slope;
  RunOpCpu("slope", sa, slope, err);
  Grid pass(terrain.w, terrain.h, terrain.cell, terrain.ox, terrain.oz);   // a passable mask: where the slope is below 0.8
  for (std::size_t i = 0; i < slope.v.size(); ++i) pass.v[i] = slope.v[i] < 0.8f ? 1.0f : 0.0f;
  const auto mask = std::make_shared<Grid>(pass);
  const std::string k5 = "1:4:6:4:1:4:16:24:16:4:6:24:36:24:6:4:16:24:16:4:1:4:6:4:1";
  std::string k15;
  for (int i = 0; i < 225; ++i) k15 += (i ? ":" : "") + std::to_string(1 + (i % 7));
  struct Case
  {
    const char* op;
    std::string params;
    bool maskInput;
  };
  const std::vector<Case> cases = {
    {"morph", "op=dilate,radius=8,shape=square", false}, {"morph", "op=dilate,radius=16,shape=disc", false}, {"morph", "op=open,radius=16,shape=disc", false}, {"morph", "op=tophat,radius=24,shape=square", false},
    {"convolve", "kernel=" + k5, false}, {"convolve", "kernel=" + k15, false}, {"unsharp", "radius=4", false}, {"unsharp", "radius=20,threshold=1", false},
    {"edge", "mode=sobel", false}, {"edge", "mode=laplace,smooth=4", false}, {"edge", "mode=dog,radius=4", false}, {"blur_slope", "radius=8", false},
    {"slope_limit", "mode=both", false}, {"slope_limit", "mode=lower", false}, {"rockiness", "breakup=0.15", false}, {"rockiness", "breakup=0", false},
    {"stratify", "thickness=3,warp=4", false}, {"stratify", "thickness=3", false}, {"settle", "iterations=20", false}, {"thermal_map", "iterations=30", false}, {"thermal", "iterations=20", false},
    {"erode_rocksoil", "iterations=60", false}, {"erode_snow", "", false}, {"erode_wind", "iterations=30", false}, {"erode_glacier", "iterations=60", false},
    {"wetness", "mode=wet,smooth=4", false}, {"flow", "mode=cells", false}, {"distance", "threshold=0.5", false}, {"symmetrize", "mode=rot4", false},
    {"reach", "src=0:0:0:0", true}, {"regions", "", true}, {"slope_area", "", false}, {"basin_depth", "", false}, {"erode_stream", "iterations=10", false},
    {"amplitude", "gain=2", false}, {"levels", "lo=0.1,hi=0.9", false}, {"blur", "radius=4", false},
  };
  std::printf("size %d x %d, %d threads\n", n, n, Threads());
  for (const Case& c : cases) {
    if (!only.empty() && only != c.op) continue;
    OpArgs a = Make(c.params);
    a.in["a"] = c.maskInput ? mask : ter;
    if (std::string(c.op) == "reach") {
      const double lo = 0.05 * n * terrain.cell, hi = 0.95 * n * terrain.cell;   // two sources near opposite corners (world positions)
      a.p.arr["src"] = {lo, lo, hi, hi};
    }
    std::map<std::string, Grid> extra;
    a.extra = &extra;
    Grid out;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = RunOpCpu(c.op, a, out, err);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-15s %-48.48s %9.0f ms %s\n", c.op, c.params.c_str(), ms, ok ? "" : err.c_str());
    std::fflush(stdout);
  }
  return 0;
}
