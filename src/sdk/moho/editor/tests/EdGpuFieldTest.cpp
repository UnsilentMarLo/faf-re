// Stand-alone test of the GPU versions of more field ops (EdGpuFieldIter.cpp, EdGpuFieldFilt.cpp; kernels gpu/field_*.hlsl; docs/projects/editor/GPU_COMPUTE.md, "More field ops (GP4)").
// No GPU is needed: the kernels are compiled as C++ through gpu/hlsl_cpp_shim.h and run by the CPU emulator (tests/EdGpuEmu.h) behind the field engine's interceptor, and compared with the CPU ops.
//   g++ -std=c++20 -O2 -ffp-contract=off -I src/sdk src/sdk/moho/editor/tests/EdGpuFieldTest.cpp (sources: scripts/ed_native_tests.py) -pthread -o EdGpuFieldTest
//   EdGpuFieldTest [-v]
// What is checked, per op: the GPU result against the CPU op (same arithmetic in the same order: most ops are bit equal, the tolerance is a few float steps), every mode / shape / edge option,
// masks, extra outputs, odd sizes (1 x N, N x 1, 1 x 1, not a multiple of 16), the Unsupported branches (the CPU op runs and reports its own error), that the op really ran on the (emulated) GPU,
// and that the kernels made no out of range buffer access.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "moho/editor/EdFieldCore.h"
#include "moho/editor/EdGpu.h"
#include "moho/editor/EdGpuOps.h"

#include "moho/editor/gpu/hlsl_cpp_shim.h"
#include "moho/editor/tests/EdGpuEmu.h"

// every .hlsli at global scope first
#include "moho/editor/gpu/common.hlsli"
#include "moho/editor/gpu/blur_params.hlsli"
#include "moho/editor/gpu/filt_common.hlsli"
#include "moho/editor/gpu/gen_noise.hlsli"
#include "moho/editor/gpu/field_settle_params.hlsli"
#include "moho/editor/gpu/field_glacier_params.hlsli"
#include "moho/editor/gpu/field_slide_params.hlsli"
#include "moho/editor/gpu/field_blurslope_params.hlsli"
#include "moho/editor/gpu/field_convolve_params.hlsli"
#include "moho/editor/gpu/field_edge_params.hlsli"
#include "moho/editor/gpu/field_morph_params.hlsli"
#include "moho/editor/gpu/field_rockiness_params.hlsli"
#include "moho/editor/gpu/field_slopelimit_params.hlsli"
#include "moho/editor/gpu/field_stratify_params.hlsli"
#include "moho/editor/gpu/field_snowinit_params.hlsli"
#include "moho/editor/gpu/field_slopescan_params.hlsli"
#include "moho/editor/gpu/field_sub_params.hlsli"
#include "moho/editor/gpu/field_unsharp_params.hlsli"
#include "moho/editor/gpu/field_weather_params.hlsli"
#include "moho/editor/gpu/field_wet_params.hlsli"
#include "moho/editor/gpu/field_wind_params.hlsli"

EMU_KERNEL_BEGIN(k_field_settle)
#include "moho/editor/gpu/field_settle.hlsl"
EMU_KERNEL_END(field_settle)

EMU_KERNEL_BEGIN(k_field_slide_a)
#include "moho/editor/gpu/field_slide_a.hlsl"
EMU_KERNEL_END(field_slide_a)

EMU_KERNEL_BEGIN(k_field_slide_b)
#include "moho/editor/gpu/field_slide_b.hlsl"
EMU_KERNEL_END(field_slide_b)

EMU_KERNEL_BEGIN(k_field_blurslope)
#include "moho/editor/gpu/field_blurslope.hlsl"
EMU_KERNEL_END(field_blurslope)

EMU_KERNEL_BEGIN(k_field_edge)
#include "moho/editor/gpu/field_edge.hlsl"
EMU_KERNEL_END(field_edge)

EMU_KERNEL_BEGIN(k_field_unsharp)
#include "moho/editor/gpu/field_unsharp.hlsl"
EMU_KERNEL_END(field_unsharp)

EMU_KERNEL_BEGIN(k_field_convolve)
#include "moho/editor/gpu/field_convolve.hlsl"
EMU_KERNEL_END(field_convolve)

EMU_KERNEL_BEGIN(k_field_morph)
#include "moho/editor/gpu/field_morph.hlsl"
EMU_KERNEL_END(field_morph)

EMU_KERNEL_BEGIN(k_field_rockiness)
#include "moho/editor/gpu/field_rockiness.hlsl"
EMU_KERNEL_END(field_rockiness)

EMU_KERNEL_BEGIN(k_field_stratify)
#include "moho/editor/gpu/field_stratify.hlsl"
EMU_KERNEL_END(field_stratify)

EMU_KERNEL_BEGIN(k_field_slopelimit)
#include "moho/editor/gpu/field_slopelimit.hlsl"
EMU_KERNEL_END(field_slopelimit)

EMU_KERNEL_BEGIN(k_field_slopescan)
#include "moho/editor/gpu/field_slopescan.hlsl"
EMU_KERNEL_END(field_slopescan)

EMU_KERNEL_BEGIN(k_field_weather)
#include "moho/editor/gpu/field_weather.hlsl"
EMU_KERNEL_END(field_weather)

EMU_KERNEL_BEGIN(k_field_snowinit)
#include "moho/editor/gpu/field_snowinit.hlsl"
EMU_KERNEL_END(field_snowinit)

EMU_KERNEL_BEGIN(k_field_wetfinal)
#include "moho/editor/gpu/field_wetfinal.hlsl"
EMU_KERNEL_END(field_wetfinal)

EMU_KERNEL_BEGIN(k_field_wetprep)
#include "moho/editor/gpu/field_wetprep.hlsl"
EMU_KERNEL_END(field_wetprep)

EMU_KERNEL_BEGIN(k_field_wetrank)
#include "moho/editor/gpu/field_wetrank.hlsl"
EMU_KERNEL_END(field_wetrank)

EMU_KERNEL_BEGIN(k_field_wetsweep)
#include "moho/editor/gpu/field_wetsweep.hlsl"
EMU_KERNEL_END(field_wetsweep)

EMU_KERNEL_BEGIN(k_field_wind_a)
#include "moho/editor/gpu/field_wind_a.hlsl"
EMU_KERNEL_END(field_wind_a)

EMU_KERNEL_BEGIN(k_field_wind_b)
#include "moho/editor/gpu/field_wind_b.hlsl"
EMU_KERNEL_END(field_wind_b)

EMU_KERNEL_BEGIN(k_field_glacier_a)
#include "moho/editor/gpu/field_glacier_a.hlsl"
EMU_KERNEL_END(field_glacier_a)

EMU_KERNEL_BEGIN(k_field_glacier_b)
#include "moho/editor/gpu/field_glacier_b.hlsl"
EMU_KERNEL_END(field_glacier_b)

EMU_KERNEL_BEGIN(k_field_sub)
#include "moho/editor/gpu/field_sub.hlsl"
EMU_KERNEL_END(field_sub)

// the Gaussian blur of the foundation (EdGpuOps.cpp BlurFieldDevice) and its mask blend run their kernels here too
EMU_KERNEL_BEGIN(k_blur_pass)
#include "moho/editor/gpu/blur_pass.hlsl"
EMU_KERNEL_END(blur_pass)

using namespace moho::editor;
using field::Grid;
using field::GridPtr;
using field::OpArgs;

static int gFailures = 0;
static int gChecks = 0;
static bool gVerbose = false;
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

using Num = std::map<std::string, double>;
using Str = std::map<std::string, std::string>;
using Inputs = std::map<std::string, GridPtr>;

static OpArgs Args(const Num& n, const Str& s = {}, const Inputs& in = {})
{
  OpArgs a;
  a.p.num = n;
  a.p.str = s;
  a.in = in;
  return a;
}

static std::uint64_t GpuCalls(const std::string& op)
{
  const auto st = field::OpStats(false);
  const auto it = st.find(op);
  return it == st.end() ? 0 : it->second.gpuCalls;
}

/** Runs `op` through RunOp with the emulated GPU; `ranOnGpu` tells whether the GPU version produced the result (false: it declined and the CPU op ran). */
static bool RunGpu(const std::string& op, const OpArgs& a, Grid& out, bool* ranOnGpu = nullptr, std::string* errOut = nullptr)
{
  const std::uint64_t before = GpuCalls(op);
  std::string err;
  emu::gOob = 0;
  const bool ok = field::RunOp(op, a, out, err);
  if (!ok && errOut != nullptr) *errOut = err;
  if (!ok && errOut == nullptr) std::printf("op %s failed: %s\n", op.c_str(), err.c_str());
  if (ranOnGpu != nullptr) *ranOnGpu = GpuCalls(op) == before + 1;
  CHECK(emu::gOob == 0, "%s: %d out of range buffer accesses in the kernels", op.c_str(), emu::gOob);
  return ok;
}

static bool RunCpu(const std::string& op, const OpArgs& a, Grid& out)
{
  std::string err;
  const bool ok = field::RunOpCpu(op, a, out, err);
  if (!ok) std::printf("cpu op %s failed: %s\n", op.c_str(), err.c_str());
  return ok;
}

struct Diff
{
  double maxAbs = 1e30;
  double meanAbs = 1e30;
  bool nan = false;
};

static Diff Compare(const Grid& a, const Grid& b)
{
  Diff d;
  if (!a.SameShape(b) || a.v.empty()) return d;
  double sum = 0.0, worst = 0.0;
  for (std::size_t i = 0; i < a.v.size(); ++i) {
    if (!std::isfinite(a.v[i]) || !std::isfinite(b.v[i])) d.nan = true;
    const double e = std::fabs(static_cast<double>(a.v[i]) - static_cast<double>(b.v[i]));
    sum += e;
    worst = std::max(worst, e);
  }
  d.maxAbs = worst;
  d.meanAbs = sum / static_cast<double>(a.v.size());
  return d;
}

/** The GPU result (and every extra output) must equal the CPU op's within `tol` (max abs), have the CPU result's placement, and have run on the GPU. */
static void CheckSame(const char* label, const std::string& op, const OpArgs& a, const double tol, Diff* diffOut = nullptr, const bool withExtras = false)
{
  Grid g, c;
  std::map<std::string, Grid> ge, ce;
  OpArgs ag = a, ac = a;
  if (withExtras) {
    ag.extra = &ge;
    ac.extra = &ce;
  }
  bool onGpu = false;
  const bool ok = RunGpu(op, ag, g, &onGpu) && RunCpu(op, ac, c);
  CHECK(ok && onGpu, "%s: the op did not run on the GPU", label);
  if (!ok) return;
  CHECK(g.w == c.w && g.h == c.h && g.cell == c.cell && g.ox == c.ox && g.oz == c.oz, "%s: the result has another placement than the CPU's", label);
  const Diff d = Compare(g, c);
  CHECK(!d.nan && d.maxAbs <= tol, "%s: GPU and CPU differ by %.3g (mean %.3g), allowed %.3g", label, d.maxAbs, d.meanAbs, tol);
  if (gVerbose) std::printf("  %-34s max %.3g mean %.3g\n", label, d.maxAbs, d.meanAbs);
  if (diffOut != nullptr) *diffOut = d;
  if (withExtras) {
    CHECK(ge.size() == ce.size(), "%s: %zu extra outputs, the CPU has %zu", label, ge.size(), ce.size());
    for (const auto& kv : ce) {
      const auto it = ge.find(kv.first);
      CHECK(it != ge.end(), "%s: extra output '%s' is missing", label, kv.first.c_str());
      if (it == ge.end()) continue;
      const Diff e = Compare(it->second, kv.second);
      CHECK(it->second.cell == kv.second.cell && it->second.ox == kv.second.ox && !e.nan && e.maxAbs <= tol, "%s: extra '%s' differs by %.3g, allowed %.3g", label, kv.first.c_str(), e.maxAbs, tol);
    }
  }
}

/** The GPU op declines (the CPU op runs: same result as RunOpCpu, or the same error). */
static void CheckDeclines(const char* label, const std::string& op, const OpArgs& a)
{
  Grid g, c;
  bool onGpu = true;
  std::string e1, e2;
  const bool g1 = RunGpu(op, a, g, &onGpu, &e1);
  const bool c1 = field::RunOpCpu(op, a, c, e2);
  CHECK(!onGpu, "%s: the GPU op should have declined", label);
  CHECK(g1 == c1 && (!g1 || Compare(g, c).maxAbs == 0.0) && (g1 || e1 == e2), "%s: the fall-back differs from the CPU op ('%s' / '%s')", label, e1.c_str(), e2.c_str());
}

/** A terrain-like input field: fractal noise (CPU op) in world units, `height` units tall. */
static GridPtr TestTerrain(const int w, const int h, const float cell, const std::uint32_t seed, const double height = 60.0, const double ox = 17.5, const double oz = -4.0)
{
  auto g = std::make_shared<Grid>();
  std::string err;
  const OpArgs a = Args({{"width", static_cast<double>(w)}, {"height", static_cast<double>(h)}, {"cell", static_cast<double>(cell)}, {"seed", static_cast<double>(seed)}, {"scale", 70.0 * cell}, {"octaves", 7}, {"ox", ox}, {"oz", oz}});
  if (!field::RunOpCpu("fractal", a, *g, err)) std::printf("terrain failed: %s\n", err.c_str());
  for (float& v : g->v) v *= static_cast<float>(height);
  return g;
}

/** A field of values 0..1 (a mask): smooth noise stretched so it covers the range. */
static GridPtr TestMask(const int w, const int h, const std::uint32_t seed)
{
  auto g = std::make_shared<Grid>();
  std::string err;
  const OpArgs a = Args({{"width", static_cast<double>(w)}, {"height", static_cast<double>(h)}, {"seed", static_cast<double>(seed)}, {"scale", 9.0}, {"octaves", 3}});
  if (!field::RunOpCpu("perlin", a, *g, err)) std::printf("mask failed: %s\n", err.c_str());
  for (float& v : g->v) v = std::max(0.0f, std::min(1.0f, (v - 0.5f) * 2.2f + 0.5f));
  return g;
}

static const std::pair<int, int> kOddSizes[] = {{1, 37}, {41, 1}, {1, 1}, {17, 33}, {16, 16}, {31, 5}, {2, 2}, {3, 3}};

// ---- settle ----------------------------------------------------------------------------------------------------------------------------------------------

static void TestSettle()
{
  const GridPtr t = TestTerrain(75, 49, 2.0f, 11, 40.0);
  const GridPtr mask = TestMask(75, 49, 3);
  CheckSame("settle_default", "settle", Args({}, {}, {{"a", t}}), 2e-5);
  {
    Grid g;
    RunGpu("settle", Args({}, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 1e-3, "settle changes the terrain (the comparison is not vacuous)");
  }
  CheckSame("settle_extras", "settle", Args({{"iterations", 7}}, {}, {{"a", t}}), 2e-5, nullptr, true);
  CheckSame("settle_deposit", "settle", Args({{"iterations", 12}, {"deposit_scale", 3.5}}, {{"output", "deposit"}}, {{"a", t}}), 2e-5, nullptr, true);
  CheckSame("settle_mask", "settle", Args({{"iterations", 15}}, {}, {{"a", t}, {"mask", mask}}), 2e-5, nullptr, true);
  Num n = {{"iterations", 30}, {"rate", 0.9}, {"cap", 0.05}, {"slope_lo", 0.05}, {"slope_hi", 0.3}};
  CheckSame("settle_params", "settle", Args(n, {{"output", "deposit"}}, {{"a", t}}), 2e-5, nullptr, true);
  n["slope_lo"] = 0.4;
  n["slope_hi"] = 0.4;   // an empty smoothstep range is a step
  CheckSame("settle_step", "settle", Args(n, {}, {{"a", t}}), 2e-5);
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 30.0);
    CheckSame(("settle_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "settle", Args({{"iterations", 6}}, {}, {{"a", s}}), 2e-5, nullptr, true);
  }
  // the placement of the CPU result comes from the source, a `like` of the same size changes nothing
  CheckSame("settle_like", "settle", Args({{"iterations", 3}}, {}, {{"a", t}, {"like", mask}}), 2e-5);
  // declined: nothing to do, unknown output, wrong mask / like size (the CPU op reports them)
  CheckDeclines("settle_zero_iterations", "settle", Args({{"iterations", 0}}, {}, {{"a", t}}));
  CheckDeclines("settle_bad_output", "settle", Args({}, {{"output", "bogus"}}, {{"a", t}}));
  CheckDeclines("settle_bad_mask", "settle", Args({}, {}, {{"a", t}, {"mask", TestMask(10, 10, 1)}}));
  CheckDeclines("settle_no_input", "settle", Args({}, {}, {}));
}

// ---- thermal_map -----------------------------------------------------------------------------------------------------------------------------------

static void TestThermalMap()
{
  const GridPtr t = TestTerrain(75, 49, 2.0f, 21, 90.0);
  const GridPtr tmap = TestMask(75, 49, 7);
  const GridPtr mask = TestMask(75, 49, 8);
  CheckSame("thermal_default", "thermal_map", Args({}, {}, {{"a", t}}), 2e-5, nullptr, true);
  {
    Grid g;
    RunGpu("thermal_map", Args({}, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 1e-2, "thermal_map changes the terrain (the comparison is not vacuous)");
  }
  CheckSame("thermal_scree", "thermal_map", Args({{"iterations", 9}, {"scree_scale", 2.5}}, {{"output", "scree"}}, {{"a", t}}), 2e-5, nullptr, true);
  CheckSame("thermal_moved", "thermal_map", Args({{"iterations", 9}}, {{"output", "moved"}}, {{"a", t}}), 2e-5, nullptr, true);
  CheckSame("thermal_map", "thermal_map", Args({{"iterations", 12}, {"talus_lo", 0.2}, {"talus_hi", 1.4}}, {}, {{"a", t}, {"b", tmap}}), 2e-5, nullptr, true);
  CheckSame("thermal_rate_mask", "thermal_map", Args({{"iterations", 12}, {"rate", 0.8}, {"talus", 0.3}}, {}, {{"a", t}, {"mask", mask}}), 2e-5, nullptr, true);
  CheckSame("thermal_both", "thermal_map", Args({{"iterations", 5}}, {{"output", "scree"}}, {{"a", t}, {"b", tmap}, {"mask", mask}}), 2e-5, nullptr, true);
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 80.0);
    CheckSame(("thermal_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "thermal_map", Args({{"iterations", 6}, {"talus", 0.2}}, {}, {{"a", s}}), 2e-5, nullptr, true);
  }
  CheckDeclines("thermal_zero_iterations", "thermal_map", Args({{"iterations", 0}}, {}, {{"a", t}}));
  CheckDeclines("thermal_bad_output", "thermal_map", Args({}, {{"output", "bogus"}}, {{"a", t}}));
  CheckDeclines("thermal_bad_map", "thermal_map", Args({}, {}, {{"a", t}, {"b", TestMask(10, 10, 1)}}));
  CheckDeclines("thermal_bad_mask", "thermal_map", Args({}, {}, {{"a", t}, {"mask", TestMask(10, 10, 1)}}));
}

// ---- morph -----------------------------------------------------------------------------------------------------------------------------------------

static void TestMorph()
{
  const GridPtr t = TestTerrain(61, 47, 2.0f, 31, 50.0);
  for (const char* op : {"dilate", "erode", "open", "close", "gradient", "tophat", "bothat"}) {
    for (const char* shape : {"square", "disc", "cross", "diamond"}) {
      const std::string label = std::string("morph_") + op + "_" + shape;
      CheckSame(label.c_str(), "morph", Args({{"radius", 7.0}}, {{"op", op}, {"shape", shape}}, {{"a", t}}), 0.0);
    }
  }
  {
    Grid g;
    RunGpu("morph", Args({{"radius", 8.0}}, {{"op", "dilate"}, {"shape", "disc"}}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 1.0, "morph changes the terrain (the comparison is not vacuous)");
  }
  // radii: 1 sample, the clamp (square 32 samples, others 16), a window larger than the field
  for (const double rad : {0.1, 2.0, 40.0, 500.0}) {
    for (const char* shape : {"square", "disc", "diamond", "cross"}) {
      CheckSame((std::string("morph_r") + std::to_string(static_cast<int>(rad)) + "_" + shape).c_str(), "morph", Args({{"radius", rad}}, {{"op", "open"}, {"shape", shape}}, {{"a", t}}), 0.0);
    }
  }
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 80.0);
    for (const char* shape : {"square", "disc"}) {
      CheckSame(("morph_" + std::to_string(sz.first) + "x" + std::to_string(sz.second) + "_" + shape).c_str(), "morph", Args({{"radius", 5.0}}, {{"op", "gradient"}, {"shape", shape}}, {{"a", s}}), 0.0);
    }
  }
  // placement: dilate keeps the source's, a `like` with the same placement is fine, another placement is left to the CPU
  CheckSame("morph_like", "morph", Args({{"radius", 4.0}}, {{"op", "gradient"}}, {{"a", t}, {"like", t}}), 0.0);
  auto moved = std::make_shared<Grid>(*t);
  moved->ox += 5.0f;
  CheckDeclines("morph_like_other_placement", "morph", Args({{"radius", 4.0}}, {{"op", "tophat"}}, {{"a", t}, {"like", moved}}));
  CheckDeclines("morph_bad_op", "morph", Args({}, {{"op", "bogus"}}, {{"a", t}}));
  CheckDeclines("morph_bad_shape", "morph", Args({}, {{"shape", "bogus"}}, {{"a", t}}));
  CheckDeclines("morph_no_input", "morph", Args({}, {}, {}));
}

// ---- convolve --------------------------------------------------------------------------------------------------------------------------------------

static OpArgs ConvArgs(const std::vector<double>& k, const GridPtr& a, Num n = {}, Str s = {})
{
  OpArgs args = Args(n, s, {{"a", a}});
  args.p.arr["kernel"] = k;
  return args;
}

static void TestConvolve()
{
  const GridPtr t = TestTerrain(53, 41, 2.0f, 41, 50.0);
  const std::vector<double> sharp = {0, -1, 0, -1, 5, -1, 0, -1, 0};
  const std::vector<double> edge = {-1, -1, -1, -1, 8, -1, -1, -1, -1};
  const std::vector<double> asym = {1, 2, 3, 0, 1, 0, -2, 0, 4};
  std::vector<double> big(225), mid(25);
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = 1.0 + static_cast<double>((i * 7) % 5) - 0.5 * static_cast<double>(i % 3);
  for (std::size_t i = 0; i < mid.size(); ++i) mid[i] = std::exp(-0.1 * static_cast<double>((i / 5 - 2) * (i / 5 - 2) + (i % 5 - 2) * (i % 5 - 2)));
  for (const char* e : {"clamp", "wrap", "mirror", ""}) {
    const Str es = {{"edge", e}};
    CheckSame((std::string("convolve_sharp_") + e).c_str(), "convolve", ConvArgs(sharp, t, {}, es), 0.0);
    CheckSame((std::string("convolve_asym_") + e).c_str(), "convolve", ConvArgs(asym, t, {{"scale", 1.5}, {"bias", -2.0}}, es), 0.0);
    CheckSame((std::string("convolve_edge_") + e).c_str(), "convolve", ConvArgs(edge, t, {{"normalize", 0}}, es), 0.0);
    CheckSame((std::string("convolve_5_") + e).c_str(), "convolve", ConvArgs(mid, t, {}, es), 0.0);
    CheckSame((std::string("convolve_15_") + e).c_str(), "convolve", ConvArgs(big, t, {{"scale", 0.5}}, es), 0.0);
  }
  {
    Grid g;
    RunGpu("convolve", ConvArgs(edge, t, {{"normalize", 0}}), g);
    CHECK(Compare(g, *t).maxAbs > 1.0, "convolve changes the terrain (the comparison is not vacuous)");
  }
  CheckSame("convolve_1x1_kernel", "convolve", ConvArgs({2.0}, t, {{"bias", 1.0}}), 0.0);   // a 1 x 1 kernel is below the work limit on the default policy only
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 80.0);
    for (const char* e : {"clamp", "wrap", "mirror"}) {
      CheckSame(("convolve_" + std::to_string(sz.first) + "x" + std::to_string(sz.second) + "_" + e).c_str(), "convolve", ConvArgs(mid, s, {}, {{"edge", e}}), 0.0);
    }
  }
  {
    auto like = std::make_shared<Grid>(*t);   // the result is placed like `like`
    like->ox = 100.0f;
    like->cell = 3.0f;
    OpArgs args = ConvArgs(sharp, t);
    args.in["like"] = like;
    CheckSame("convolve_like", "convolve", args, 0.0);
  }
  CheckDeclines("convolve_even_kernel", "convolve", ConvArgs({1, 2, 3, 4}, t));
  CheckDeclines("convolve_not_square", "convolve", ConvArgs({1, 2, 3, 4, 5, 6, 7, 8}, t));
  CheckDeclines("convolve_too_big", "convolve", ConvArgs(std::vector<double>(289, 1.0), t));
  CheckDeclines("convolve_no_kernel", "convolve", Args({}, {}, {{"a", t}}));
  CheckDeclines("convolve_no_input", "convolve", Args({}, {}, {}));
}

// ---- unsharp, edge, blur_slope -----------------------------------------------------------------------------------------------------------------------

static void TestBlurBased()
{
  const GridPtr t = TestTerrain(67, 45, 2.0f, 51, 50.0);
  const GridPtr mask = TestMask(67, 45, 5);
  // unsharp
  CheckSame("unsharp_default", "unsharp", Args({}, {}, {{"a", t}}), 1e-5);
  CheckSame("unsharp_params", "unsharp", Args({{"radius", 9.0}, {"amount", 2.5}, {"threshold", 1.5}}, {}, {{"a", t}}), 1e-5);
  CheckSame("unsharp_mask", "unsharp", Args({{"radius", 5.0}, {"amount", -0.7}}, {}, {{"a", t}, {"mask", mask}}), 1e-5);
  {
    Grid g;
    RunGpu("unsharp", Args({{"radius", 6.0}, {"amount", 3.0}}, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 0.5, "unsharp changes the terrain (the comparison is not vacuous)");
  }
  // edge: every mode, with and without smoothing, signed
  for (const char* mode : {"sobel", "prewitt", "laplace", "dog"}) {
    for (const double smooth : {0.0, 6.0}) {
      for (const double sgn : {0.0, 1.0}) {
        const std::string label = std::string("edge_") + mode + (smooth > 0.0 ? "_smooth" : "") + (sgn > 0.0 ? "_signed" : "");
        CheckSame(label.c_str(), "edge", Args({{"gain", 3.0}, {"smooth", smooth}, {"signed", sgn}, {"radius", 3.0}, {"ratio", 2.0}}, {{"mode", mode}}, {{"a", t}}), 1e-5);
      }
    }
  }
  {
    Grid g;
    RunGpu("edge", Args({{"gain", 8.0}}, {{"mode", "sobel"}}, {{"a", t}}), g);
    double mx = 0.0;
    for (const float v : g.v) mx = std::max(mx, static_cast<double>(v));
    CHECK(mx > 0.2, "edge finds edges (max %.3f)", mx);
  }
  // blur_slope
  for (const char* mode : {"flat", "steep"}) {
    CheckSame((std::string("blur_slope_") + mode).c_str(), "blur_slope", Args({{"radius", 8.0}, {"lo", 0.2}, {"hi", 0.6}, {"amount", 0.8}}, {{"mode", mode}}, {{"a", t}}), 1e-5);
    CheckSame((std::string("blur_slope_mask_") + mode).c_str(), "blur_slope", Args({{"radius", 4.0}}, {{"mode", mode}}, {{"a", t}, {"mask", mask}}), 1e-5);
  }
  // odd sizes (the blur is wider than the field)
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 80.0);
    const std::string tag = std::to_string(sz.first) + "x" + std::to_string(sz.second);
    CheckSame(("unsharp_" + tag).c_str(), "unsharp", Args({{"radius", 4.0}, {"amount", 1.0}}, {}, {{"a", s}}), 1e-5);
    CheckSame(("edge_dog_" + tag).c_str(), "edge", Args({{"radius", 2.0}, {"gain", 2.0}}, {{"mode", "dog"}}, {{"a", s}}), 1e-5);
    CheckSame(("edge_sobel_" + tag).c_str(), "edge", Args({{"gain", 2.0}}, {{"mode", "sobel"}}, {{"a", s}}), 1e-5);
    CheckSame(("edge_laplace_" + tag).c_str(), "edge", Args({{"gain", 2.0}, {"smooth", 3.0}}, {{"mode", "laplace"}}, {{"a", s}}), 1e-5);
    CheckSame(("blur_slope_" + tag).c_str(), "blur_slope", Args({{"radius", 3.0}}, {}, {{"a", s}}), 1e-5);
  }
  // a `like` places the result
  {
    auto like = std::make_shared<Grid>(*t);
    like->ox = -50.0f;
    CheckSame("edge_like", "edge", Args({}, {{"mode", "sobel"}}, {{"a", t}, {"like", like}}), 1e-5);
    CheckSame("unsharp_like", "unsharp", Args({}, {}, {{"a", t}, {"like", like}}), 1e-5);
  }
  CheckDeclines("unsharp_bad_mask", "unsharp", Args({}, {}, {{"a", t}, {"mask", TestMask(9, 9, 1)}}));
  CheckDeclines("unsharp_no_input", "unsharp", Args({}, {}, {}));
  CheckDeclines("edge_bad_mode", "edge", Args({}, {{"mode", "bogus"}}, {{"a", t}}));
  CheckDeclines("blur_slope_bad_mode", "blur_slope", Args({}, {{"mode", "bogus"}}, {{"a", t}}));
  CheckDeclines("blur_slope_bad_mask", "blur_slope", Args({}, {}, {{"a", t}, {"mask", TestMask(9, 9, 1)}}));
}

// ---- slope_limit -----------------------------------------------------------------------------------------------------------------------------------

/** The largest height difference between two neighbouring samples, divided by the cell size (the slope the pathing rule looks at). */
static double SteepestStep(const Grid& g)
{
  double worst = 0.0;
  for (int z = 0; z < g.h; ++z) {
    for (int x = 0; x < g.w; ++x) {
      if (x + 1 < g.w) worst = std::max(worst, static_cast<double>(std::fabs(g.At(x + 1, z) - g.At(x, z))));
      if (z + 1 < g.h) worst = std::max(worst, static_cast<double>(std::fabs(g.At(x, z + 1) - g.At(x, z))));
    }
  }
  return worst / static_cast<double>(g.cell);
}

static void TestSlopeLimit()
{
  // the GPU scan runs in float, the CPU op in double: a few 1e-4 on values of a few hundred
  const GridPtr t = TestTerrain(97, 61, 2.0f, 61, 300.0);
  const GridPtr mask = TestMask(97, 61, 9);
  for (const char* mode : {"lower", "raise", "both"}) {
    const std::string m = mode;
    CheckSame(("slope_limit_" + m).c_str(), "slope_limit", Args({{"limit", 0.4}}, {{"mode", mode}}, {{"a", t}}), 1e-3);
    CheckSame(("slope_limit_" + m + "_mask").c_str(), "slope_limit", Args({{"limit", 0.2}, {"strength", 0.7}, {"balance", 0.25}}, {{"mode", mode}}, {{"a", t}, {"mask", mask}}), 1e-3);
  }
  {
    Grid g;
    RunGpu("slope_limit", Args({{"limit", 0.3}, {"margin", 0.0}}, {{"mode", "both"}}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 1.0, "slope_limit changes the terrain (the comparison is not vacuous)");
    const double before = SteepestStep(*t), after = SteepestStep(g);
    CHECK(after <= 0.3 + 2e-3 && before > 0.4, "slope_limit: the steepest step %.3f becomes %.3f (limit 0.3)", before, after);
  }
  CheckSame("slope_limit_default", "slope_limit", Args({}, {}, {{"a", t}}), 1e-3);
  CheckSame("slope_limit_zero", "slope_limit", Args({{"limit", 0.0}, {"margin", 0.0}}, {{"mode", "lower"}}, {{"a", t}}), 1e-3);   // a flat plane at the lowest point
  // long chains: a ramp that has to be cut over hundreds of samples, a wide shallow field
  {
    auto ramp = std::make_shared<Grid>(701, 9, 1.0f, 0.0f, 0.0f);
    for (int z = 0; z < ramp->h; ++z) {
      for (int x = 0; x < ramp->w; ++x) ramp->At(x, z) = (x < 350 ? 0.0f : 300.0f) + 0.01f * static_cast<float>(z);
    }
    CheckSame("slope_limit_ramp", "slope_limit", Args({{"limit", 0.5}, {"margin", 0.0}}, {{"mode", "both"}}, {{"a", ramp}}), 1e-3);
  }
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 80.0);
    CheckSame(("slope_limit_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "slope_limit", Args({{"limit", 0.3}}, {}, {{"a", s}}), 1e-3);
  }
  auto like = std::make_shared<Grid>(5, 5, 9.0f, 1.0f, 1.0f);   // the CPU op ignores `like`
  CheckSame("slope_limit_like", "slope_limit", Args({{"limit", 0.4}}, {}, {{"a", t}, {"like", like}}), 1e-3);
  CheckDeclines("slope_limit_bad_mode", "slope_limit", Args({}, {{"mode", "bogus"}}, {{"a", t}}));
  CheckDeclines("slope_limit_bad_mask", "slope_limit", Args({}, {}, {{"a", t}, {"mask", TestMask(9, 9, 1)}}));
  CheckDeclines("slope_limit_no_input", "slope_limit", Args({}, {}, {}));
}

// ---- erode_rocksoil ----------------------------------------------------------------------------------------------------------------------------------

static void TestRockSoil()
{
  const GridPtr t = TestTerrain(71, 53, 2.0f, 71, 80.0);
  auto soil = std::make_shared<Grid>(*TestMask(71, 53, 4));
  for (float& v : soil->v) v *= 1.8f;
  CheckSame("rocksoil_default", "erode_rocksoil", Args({{"iterations", 20}}, {}, {{"a", t}}), 1e-5, nullptr, true);
  {
    Grid g;
    RunGpu("erode_rocksoil", Args({{"iterations", 20}}, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 1e-3, "erode_rocksoil changes the terrain (the comparison is not vacuous)");
  }
  CheckSame("rocksoil_soil_map", "erode_rocksoil", Args({{"iterations", 17}, {"weather", 0.05}, {"depth0", 0.6}}, {}, {{"a", t}, {"b", soil}}), 1e-5, nullptr, true);
  for (const char* o : {"soil", "rock", "moved"}) {
    CheckSame((std::string("rocksoil_") + o).c_str(), "erode_rocksoil", Args({{"iterations", 13}, {"rock_every", 3}, {"rock_talus", 0.4}, {"soil_talus", 0.15}}, {{"output", o}}, {{"a", t}, {"b", soil}}), 1e-5, nullptr, true);
  }
  CheckSame("rocksoil_no_rock_slide", "erode_rocksoil", Args({{"iterations", 9}, {"rock_talus", 0.0}, {"rate", 0.9}}, {}, {{"a", t}}), 1e-5, nullptr, true);
  CheckSame("rocksoil_every_1", "erode_rocksoil", Args({{"iterations", 8}, {"rock_every", 1}, {"soil", 2.5}, {"rock_vis", 1.2}}, {}, {{"a", t}}), 1e-5, nullptr, true);
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 60.0);
    CheckSame(("rocksoil_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "erode_rocksoil", Args({{"iterations", 8}, {"soil_talus", 0.1}, {"rock_talus", 0.3}, {"rock_every", 2}}, {}, {{"a", s}}), 1e-5, nullptr, true);
  }
  CheckDeclines("rocksoil_zero_iterations", "erode_rocksoil", Args({{"iterations", 0}}, {}, {{"a", t}}));
  CheckDeclines("rocksoil_bad_output", "erode_rocksoil", Args({}, {{"output", "bogus"}}, {{"a", t}}));
  CheckDeclines("rocksoil_bad_soil", "erode_rocksoil", Args({}, {}, {{"a", t}, {"b", TestMask(9, 9, 1)}}));
  CheckDeclines("rocksoil_no_input", "erode_rocksoil", Args({}, {}, {}));
}

// ---- erode_snow ------------------------------------------------------------------------------------------------------------------------------------

static void TestSnow()
{
  const GridPtr t = TestTerrain(69, 57, 2.0f, 81, 90.0);
  const GridPtr acc = TestMask(69, 57, 6);
  CheckSame("snow_default", "erode_snow", Args({{"iterations", 15}}, {}, {{"a", t}}), 1e-5, nullptr, true);
  {
    Grid g;
    RunGpu("erode_snow", Args({{"iterations", 15}}, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 0.5, "erode_snow puts snow on the terrain (the comparison is not vacuous)");
  }
  CheckSame("snow_mask", "erode_snow", Args({{"iterations", 10}, {"amount", 6.0}, {"snowline", 30.0}, {"band", 25.0}}, {}, {{"a", t}, {"mask", acc}}), 1e-5, nullptr, true);
  for (const char* o : {"snow", "cover", "moved"}) {
    CheckSame((std::string("snow_") + o).c_str(), "erode_snow", Args({{"iterations", 12}, {"snow_talus", 0.3}, {"steep_lo", 0.2}, {"steep_hi", 0.9}, {"cover_depth", 1.5}, {"rate", 0.8}}, {{"output", o}}, {{"a", t}}), 1e-5, nullptr, true);
  }
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 70.0);
    CheckSame(("snow_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "erode_snow", Args({{"iterations", 7}, {"snowline", 25.0}}, {}, {{"a", s}}), 1e-5, nullptr, true);
  }
  CheckDeclines("snow_zero_iterations", "erode_snow", Args({{"iterations", 0}}, {}, {{"a", t}}));
  CheckDeclines("snow_bad_output", "erode_snow", Args({}, {{"output", "bogus"}}, {{"a", t}}));
  CheckDeclines("snow_bad_mask", "erode_snow", Args({}, {}, {{"a", t}, {"mask", TestMask(9, 9, 1)}}));
  CheckDeclines("snow_no_input", "erode_snow", Args({}, {}, {}));
}

// ---- erode_wind ------------------------------------------------------------------------------------------------------------------------------------

static void TestWind()
{
  const GridPtr t = TestTerrain(73, 55, 2.0f, 91, 40.0);
  auto sand = std::make_shared<Grid>(*TestMask(73, 55, 12));
  for (float& v : sand->v) v *= 1.2f;
  CheckSame("wind_default", "erode_wind", Args({{"iterations", 15}}, {}, {{"a", t}}), 1e-5, nullptr, true);
  {
    Grid g;
    RunGpu("erode_wind", Args({{"iterations", 15}}, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 1e-3, "erode_wind changes the terrain (the comparison is not vacuous)");
  }
  for (const double az : {0.0, 30.0, 90.0, 180.0, 225.0, 300.0}) {
    CheckSame(("wind_az" + std::to_string(static_cast<int>(az))).c_str(), "erode_wind", Args({{"iterations", 9}, {"azimuth", az}, {"speed", 0.6}, {"abrade", 0.08}}, {}, {{"a", t}, {"b", sand}}), 1e-5, nullptr, true);
  }
  for (const char* o : {"sand", "abrasion"}) {
    CheckSame((std::string("wind_") + o).c_str(), "erode_wind", Args({{"iterations", 11}, {"azimuth", 45.0}, {"drag", 3.0}, {"repose", 0.3}, {"sand", 1.5}}, {{"output", o}}, {{"a", t}}), 1e-5, nullptr, true);
  }
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 50.0);
    CheckSame(("wind_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "erode_wind", Args({{"iterations", 6}, {"azimuth", 60.0}}, {}, {{"a", s}}), 1e-5, nullptr, true);
  }
  CheckDeclines("wind_zero_iterations", "erode_wind", Args({{"iterations", 0}}, {}, {{"a", t}}));
  CheckDeclines("wind_bad_output", "erode_wind", Args({}, {{"output", "bogus"}}, {{"a", t}}));
  CheckDeclines("wind_bad_sand", "erode_wind", Args({}, {}, {{"a", t}, {"b", TestMask(9, 9, 1)}}));
  CheckDeclines("wind_no_input", "erode_wind", Args({}, {}, {}));
}

// ---- erode_glacier ---------------------------------------------------------------------------------------------------------------------------------

static void TestGlacier()
{
  const GridPtr t = TestTerrain(67, 59, 4.0f, 101, 200.0);
  const GridPtr acc = TestMask(67, 59, 14);
  const Num ice = {{"iterations", 25}, {"ela", 70.0}, {"grad", 0.08}, {"flow", 0.2}, {"erode", 0.01}};
  CheckSame("glacier_default", "erode_glacier", Args(ice, {}, {{"a", t}}), 2e-4, nullptr, true);
  {
    Grid g;
    RunGpu("erode_glacier", Args(ice, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 0.01, "erode_glacier carves the terrain (the comparison is not vacuous)");
    Grid ig;
    RunGpu("erode_glacier", Args(ice, {{"output", "ice"}}, {{"a", t}}), ig);
    double mx = 0.0;
    for (const float v : ig.v) mx = std::max(mx, static_cast<double>(v));
    CHECK(mx > 0.5, "erode_glacier grows ice (max %.3f)", mx);
  }
  for (const char* o : {"ice", "cover", "carve"}) {
    CheckSame((std::string("glacier_") + o).c_str(), "erode_glacier", Args(ice, {{"output", o}}, {{"a", t}, {"b", acc}}), 2e-4, nullptr, true);
  }
  for (const double pw : {1.0, 2.0, 1.5, 0.8, 2.7}) {
    Num n = ice;
    n["erode_pow"] = pw;
    n["iterations"] = 14;
    CheckSame(("glacier_pow" + std::to_string(static_cast<int>(pw * 10))).c_str(), "erode_glacier", Args(n, {}, {{"a", t}}), 2e-4, nullptr, true);
  }
  {
    Num n = ice;
    n["dt"] = 0.05;   // a small time step limit, and a limit that never binds
    n["max_ice"] = 8;
    n["acc_max"] = 2;
    n["abl_max"] = 0.2;
    n["max_cut"] = 1.5;
    CheckSame("glacier_limits", "erode_glacier", Args(n, {}, {{"a", t}}), 2e-4, nullptr, true);
    n["dt"] = 100.0;
    n["flow"] = 0.0005;
    CheckSame("glacier_big_dt", "erode_glacier", Args(n, {}, {{"a", t}}), 2e-4, nullptr, true);
  }
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 3.0f, 5, 150.0);
    CheckSame(("glacier_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "erode_glacier", Args({{"iterations", 8}, {"ela", 60.0}, {"grad", 0.1}, {"flow", 0.3}}, {}, {{"a", s}}), 2e-4, nullptr, true);
  }
  CheckDeclines("glacier_zero_iterations", "erode_glacier", Args({{"iterations", 0}}, {}, {{"a", t}}));
  CheckDeclines("glacier_bad_output", "erode_glacier", Args({}, {{"output", "bogus"}}, {{"a", t}}));
  CheckDeclines("glacier_bad_acc", "erode_glacier", Args({}, {}, {{"a", t}, {"b", TestMask(9, 9, 1)}}));
  CheckDeclines("glacier_no_input", "erode_glacier", Args({}, {}, {}));
}

// ---- thermal ---------------------------------------------------------------------------------------------------------------------------------------

static void TestThermal()
{
  const GridPtr t = TestTerrain(77, 51, 2.0f, 111, 100.0);
  const GridPtr mask = TestMask(77, 51, 15);
  CheckSame("thermal_op_default", "thermal", Args({}, {}, {{"a", t}}), 1e-5);
  {
    Grid g;
    RunGpu("thermal", Args({}, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 1e-2, "thermal changes the terrain (the comparison is not vacuous)");
  }
  CheckSame("thermal_op_params", "thermal", Args({{"iterations", 33}, {"talus", 0.25}, {"rate", 0.9}}, {}, {{"a", t}}), 1e-5);
  CheckSame("thermal_op_mask", "thermal", Args({{"iterations", 11}, {"talus", 0.3}}, {}, {{"a", t}, {"mask", mask}}), 1e-5);
  auto like = std::make_shared<Grid>(*t);
  like->ox = 5.0f;
  CheckSame("thermal_op_like", "thermal", Args({{"iterations", 4}}, {}, {{"a", t}, {"like", like}}), 1e-5);
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 70.0);
    CheckSame(("thermal_op_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "thermal", Args({{"iterations", 6}, {"talus", 0.2}}, {}, {{"a", s}}), 1e-5);
  }
  CheckDeclines("thermal_op_zero_iterations", "thermal", Args({{"iterations", 0}}, {}, {{"a", t}}));
  CheckDeclines("thermal_op_bad_mask", "thermal", Args({}, {}, {{"a", t}, {"mask", TestMask(9, 9, 1)}}));
  CheckDeclines("thermal_op_no_input", "thermal", Args({}, {}, {}));
}

// ---- rockiness -------------------------------------------------------------------------------------------------------------------------------------

static void TestRockiness()
{
  const GridPtr t = TestTerrain(83, 61, 2.0f, 121, 120.0, 4000.5, -1234.25);
  auto soil = std::make_shared<Grid>(*TestMask(83, 61, 16));
  for (float& v : soil->v) v *= 2.5f;
  CheckSame("rockiness_default", "rockiness", Args({}, {}, {{"a", t}}), 1e-5);
  {
    Grid g;
    RunGpu("rockiness", Args({}, {}, {{"a", t}}), g);
    double lo = 1e9, hi = -1e9;
    for (const float v : g.v) {
      lo = std::min(lo, static_cast<double>(v));
      hi = std::max(hi, static_cast<double>(v));
    }
    CHECK(hi - lo > 0.3, "rockiness has a range (%.3f .. %.3f): the comparison is not vacuous", lo, hi);
  }
  CheckSame("rockiness_no_breakup", "rockiness", Args({{"breakup", 0.0}}, {}, {{"a", t}}), 1e-5);
  CheckSame("rockiness_soil", "rockiness", Args({{"soil_full", 2.0}, {"w_soil", 1.7}}, {}, {{"a", t}, {"b", soil}}), 1e-5);
  CheckSame("rockiness_params", "rockiness", Args({{"slope_lo", 10.0}, {"slope_hi", 30.0}, {"radius", 20.0}, {"gain", 30.0}, {"w_slope", 0.4}, {"w_convex", 1.3}, {"breakup", 0.4}, {"seed", 7}}, {}, {{"a", t}}), 1e-5);
  CheckSame("rockiness_weights_zero", "rockiness", Args({{"w_slope", 0.0}, {"w_convex", 0.0}, {"breakup", 0.3}}, {}, {{"a", t}}), 1e-5);
  CheckSame("rockiness_far", "rockiness", Args({{"breakup", 0.5}}, {}, {{"a", TestTerrain(40, 40, 2.0f, 5, 100.0, 40000.0, 36000.0)}}), 1e-5);
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 80.0);
    CheckSame(("rockiness_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "rockiness", Args({{"radius", 3.0}}, {}, {{"a", s}}), 1e-5);
  }
  auto like = std::make_shared<Grid>(*t);
  like->cell = 3.0f;
  CheckSame("rockiness_like", "rockiness", Args({}, {}, {{"a", t}, {"like", like}}), 1e-5);
  CheckDeclines("rockiness_bad_soil", "rockiness", Args({}, {}, {{"a", t}, {"b", TestMask(9, 9, 1)}}));
  CheckDeclines("rockiness_no_input", "rockiness", Args({}, {}, {}));
}

// ---- stratify --------------------------------------------------------------------------------------------------------------------------------------

static void TestStratify()
{
  const GridPtr t = TestTerrain(81, 63, 2.0f, 131, 100.0, 512.0, 256.0);
  const GridPtr mask = TestMask(81, 63, 17);
  CheckSame("stratify_default", "stratify", Args({{"warp", 3.0}}, {}, {{"a", t}}), 2e-4);
  {
    Grid g;
    RunGpu("stratify", Args({{"warp", 3.0}}, {}, {{"a", t}}), g);
    CHECK(Compare(g, *t).maxAbs > 0.3, "stratify changes the terrain (the comparison is not vacuous)");
  }
  CheckSame("stratify_flat", "stratify", Args({}, {}, {{"a", t}}), 2e-4);
  CheckSame("stratify_tilt", "stratify", Args({{"tilt_x", 0.2}, {"tilt_z", -0.35}, {"thickness", 5.0}, {"variation", 0.9}, {"sharpness", 0.95}}, {}, {{"a", t}}), 2e-4);
  CheckSame("stratify_warp", "stratify", Args({{"warp", 12.0}, {"warp_size", 40.0}, {"seed", 9}, {"strength", 0.8}}, {}, {{"a", t}}), 2e-4);
  CheckSame("stratify_slope", "stratify", Args({{"slope_lo", 0.2}, {"slope_hi", 0.9}, {"warp", 4.0}}, {}, {{"a", t}}), 2e-4);
  CheckSame("stratify_mask", "stratify", Args({{"sharpness", 0.2}, {"thickness", 1.5}, {"warp", 2.0}}, {}, {{"a", t}, {"mask", mask}}), 2e-4);
  CheckSame("stratify_thin", "stratify", Args({{"thickness", 0.05}, {"variation", 0.0}, {"warp", 1.0}}, {}, {{"a", t}}), 2e-4);   // thickness is floored at 0.2
  // heights beyond the 2048 layers of either side clamp to the outermost layer
  {
    auto tall = std::make_shared<Grid>(*t);
    for (float& v : tall->v) v = v * 300.0f - 9000.0f;
    CheckSame("stratify_tall", "stratify", Args({{"thickness", 3.0}, {"warp", 5.0}}, {}, {{"a", tall}}), 2e-4);
  }
  CheckSame("stratify_far", "stratify", Args({{"warp", 5.0}, {"tilt_x", 0.05}}, {}, {{"a", TestTerrain(40, 40, 2.0f, 5, 100.0, 40000.0, 36000.0)}}), 2e-3);
  for (const auto& sz : kOddSizes) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 80.0);
    CheckSame(("stratify_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "stratify", Args({{"warp", 2.0}, {"slope_lo", 0.1}, {"slope_hi", 0.5}}, {}, {{"a", s}}), 2e-4);
  }
  auto like = std::make_shared<Grid>(*t);
  like->oz = 99.0f;
  CheckSame("stratify_like", "stratify", Args({{"warp", 1.0}}, {}, {{"a", t}, {"like", like}}), 2e-4);
  CheckDeclines("stratify_bad_mask", "stratify", Args({{"warp", 1.0}}, {}, {{"a", t}, {"mask", TestMask(9, 9, 1)}}));
  CheckDeclines("stratify_no_input", "stratify", Args({}, {}, {}));
}

// ---- wetness ---------------------------------------------------------------------------------------------------------------------------------------

/** A terrain whose water runs along a serpentine channel: the longest possible flow path, crossing many tile borders. */
static GridPtr SnakeTerrain(const int w, const int h)
{
  auto g = std::make_shared<Grid>(w, h, 2.0f, 0.0f, 0.0f, 500.0f);
  int step = 0;
  for (int z = 1; z < h; z += 3) {
    const int row = (z / 3) % 2;
    for (int i = 0; i < w; ++i) {
      const int x = row == 0 ? i : w - 1 - i;
      g->At(x, z) = 400.0f - 0.01f * static_cast<float>(step++);
    }
    if (z + 3 < h) {
      for (int dz = 1; dz <= 2; ++dz) g->At(row == 0 ? w - 1 : 0, z + dz) = 400.0f - 0.01f * static_cast<float>(step++);
    }
  }
  return g;
}

static void TestWetness()
{
  const GridPtr t = TestTerrain(100, 87, 2.0f, 141, 80.0);
  for (const char* mode : {"wet", "twi", "area"}) {
    CheckSame((std::string("wetness_") + mode).c_str(), "wetness", Args({}, {{"mode", mode}}, {{"a", t}}), 1e-5);
  }
  {
    Grid g;
    RunGpu("wetness", Args({}, {{"mode", "area"}}, {{"a", t}}), g);
    double mx = 0.0;
    for (const float v : g.v) mx = std::max(mx, static_cast<double>(v));
    CHECK(mx > 0.3, "wetness finds channels (largest catchment %.3f of the log scale)", mx);
  }
  CheckSame("wetness_smooth", "wetness", Args({{"smooth", 7.0}, {"lo", 2.0}, {"hi", 9.0}}, {}, {{"a", t}}), 1e-5);
  CheckSame("wetness_p1", "wetness", Args({{"p", 1.0}}, {{"mode", "twi"}}, {{"a", t}}), 1e-5);
  CheckSame("wetness_p3", "wetness", Args({{"p", 3.0}}, {{"mode", "twi"}}, {{"a", t}}), 1e-5);
  CheckSame("wetness_p_clamp", "wetness", Args({{"p", 20.0}}, {{"mode", "area"}}, {{"a", t}}), 1e-5);
  // the longest flow path: a serpentine channel (about 90 tile borders), and a tilted plane (every path crosses the whole field)
  CheckSame("wetness_snake", "wetness", Args({}, {{"mode", "area"}}, {{"a", SnakeTerrain(70, 70)}}), 1e-5);
  {
    auto plane = std::make_shared<Grid>(150, 40, 2.0f, 0.0f, 0.0f);
    for (int z = 0; z < plane->h; ++z) {
      for (int x = 0; x < plane->w; ++x) plane->At(x, z) = 300.0f - 1.5f * static_cast<float>(x) - 0.3f * static_cast<float>(z) + 0.01f * static_cast<float>((x * 7 + z * 13) % 5);
    }
    CheckSame("wetness_plane", "wetness", Args({}, {{"mode", "area"}}, {{"a", plane}}), 1e-5);
  }
  // flats, equal heights and pits: no flow between equal samples
  CheckSame("wetness_flat", "wetness", Args({}, {{"mode", "area"}}, {{"a", std::make_shared<Grid>(40, 40, 1.0f, 0.0f, 0.0f, 5.0f)}}), 0.0);
  for (const std::pair<int, int>& sz : {std::pair<int, int>{32, 32}, {33, 33}, {64, 31}, {31, 65}, {1, 37}, {41, 1}, {1, 1}, {17, 33}, {2, 2}, {5, 5}}) {
    const GridPtr s = TestTerrain(sz.first, sz.second, 1.5f, 5, 80.0);
    CheckSame(("wetness_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "wetness", Args({{"smooth", 3.0}}, {}, {{"a", s}}), 1e-5);
  }
  auto like = std::make_shared<Grid>(*t);
  like->ox = 7.0f;
  CheckSame("wetness_like", "wetness", Args({}, {{"mode", "twi"}}, {{"a", t}, {"like", like}}), 1e-5);
  CheckDeclines("wetness_bad_mode", "wetness", Args({}, {{"mode", "bogus"}}, {{"a", t}}));
  CheckDeclines("wetness_no_input", "wetness", Args({}, {}, {}));
}

// ---- the work gates on the default policy ---------------------------------------------------------------------------------------------------------------

/** On the default ed_gpu_min_cells a job goes to the GPU when it is worth about 10 ms of CPU time (WorthIt): cells * taps per cell. A bare 3 x 3 stencil only from 16 x min cells. */
static void TestGates()
{
  gpu::GetPolicy().minCells = 65536.0f;
  const GridPtr f = TestTerrain(640, 640, 1.0f, 3, 30.0);   // 409600 samples
  const auto gate = [&](const char* label, const std::string& op, const OpArgs& a, const bool expectGpu) {
    Grid g;
    bool onGpu = !expectGpu;
    RunGpu(op, a, g, &onGpu);
    CHECK(onGpu == expectGpu, "%s: %s on the default policy", label, expectGpu ? "should go to the GPU" : "should stay on the CPU");
  };
  gate("morph cross r1", "morph", Args({{"radius", 1.0}}, {{"op", "dilate"}, {"shape", "cross"}}, {{"a", f}}), false);
  gate("morph disc r6", "morph", Args({{"radius", 6.0}}, {{"op", "dilate"}, {"shape", "disc"}}, {{"a", f}}), true);
  gate("convolve 3x3", "convolve", ConvArgs({0, 1, 0, 1, 1, 1, 0, 1, 0}, f), false);
  gate("convolve 15x15", "convolve", ConvArgs(std::vector<double>(225, 1.0), f), true);
  gate("edge sobel", "edge", Args({}, {{"mode", "sobel"}}, {{"a", f}}), false);
  gate("edge sobel smooth 5", "edge", Args({{"smooth", 5.0}}, {{"mode", "sobel"}}, {{"a", f}}), true);
  gate("unsharp r1", "unsharp", Args({{"radius", 1.0}}, {}, {{"a", f}}), false);
  gate("unsharp r6", "unsharp", Args({{"radius", 6.0}}, {}, {{"a", f}}), true);
  gate("blur_slope r8", "blur_slope", Args({{"radius", 8.0}}, {}, {{"a", f}}), true);
  gate("slope_limit", "slope_limit", Args({}, {}, {{"a", f}}), true);
  gate("wetness", "wetness", Args({}, {}, {{"a", TestTerrain(160, 160, 1.0f, 3, 30.0)}}), true);
  gate("wetness 100 x 100", "wetness", Args({}, {}, {{"a", TestTerrain(100, 100, 1.0f, 3, 30.0)}}), false);
  gate("rockiness without noise", "rockiness", Args({{"breakup", 0.0}}, {}, {{"a", f}}), false);
  gate("rockiness", "rockiness", Args({}, {}, {{"a", f}}), true);
  gate("stratify without warp", "stratify", Args({}, {}, {{"a", f}}), false);
  gate("stratify warp", "stratify", Args({{"warp", 2.0}}, {}, {{"a", f}}), true);
  // iterated ops have their own minimum (16384 samples)
  gate("settle 128 x 128", "settle", Args({{"iterations", 3}}, {}, {{"a", TestTerrain(128, 128, 1.0f, 3, 30.0)}}), true);
  gate("settle 100 x 100", "settle", Args({{"iterations", 3}}, {}, {{"a", TestTerrain(100, 100, 1.0f, 3, 30.0)}}), false);
  gpu::GetPolicy().minCells = 0.0f;
}

int main(int argc, char** argv)
{
  for (int i = 1; i < argc; ++i) gVerbose = gVerbose || std::strcmp(argv[i], "-v") == 0;
  gpu::EmuBackend backend;
  gpu::SetBackendForTest(&backend);
  gpu::Install();
  gpu::GetPolicy().enabled = true;
  gpu::GetPolicy().minCells = 0.0f;
  gpu::GetPolicy().selfTestFirst = false;
  field::SetThreads(4);
  TestSettle();
  TestThermalMap();
  TestThermal();
  TestMorph();
  TestConvolve();
  TestBlurBased();
  TestSlopeLimit();
  TestRockiness();
  TestStratify();
  TestWetness();
  TestGates();
  TestRockSoil();
  TestSnow();
  TestWind();
  TestGlacier();
  gpu::SetBackendForTest(nullptr);
  std::printf("EdGpuFieldTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
