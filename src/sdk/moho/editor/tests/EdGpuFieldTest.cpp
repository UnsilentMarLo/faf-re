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
#include "moho/editor/gpu/field_settle_params.hlsli"
#include "moho/editor/gpu/field_slide_params.hlsli"
#include "moho/editor/gpu/field_blurslope_params.hlsli"
#include "moho/editor/gpu/field_convolve_params.hlsli"
#include "moho/editor/gpu/field_edge_params.hlsli"
#include "moho/editor/gpu/field_morph_params.hlsli"
#include "moho/editor/gpu/field_sub_params.hlsli"
#include "moho/editor/gpu/field_unsharp_params.hlsli"

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
  // a tiny window on the default policy stays on the CPU (the transfer costs more than the work); a big one does not
  {
    gpu::GetPolicy().minCells = 65536.0f;
    const GridPtr big = TestTerrain(300, 300, 1.0f, 3, 30.0);
    Grid g;
    bool onGpu = true;
    RunGpu("morph", Args({{"radius", 1.0}}, {{"op", "dilate"}, {"shape", "cross"}}, {{"a", big}}), g, &onGpu);
    CHECK(!onGpu, "morph with a 3 sample cross stays on the CPU on the default policy");
    RunGpu("morph", Args({{"radius", 6.0}}, {{"op", "dilate"}, {"shape", "disc"}}, {{"a", big}}), g, &onGpu);
    CHECK(onGpu, "morph with a 13 sample disc goes to the GPU on the default policy");
    gpu::GetPolicy().minCells = 0.0f;
  }
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
  {
    gpu::GetPolicy().minCells = 65536.0f;
    const GridPtr bigf = TestTerrain(300, 300, 1.0f, 3, 30.0);
    Grid g;
    bool onGpu = true;
    RunGpu("convolve", ConvArgs({0, 1, 0, 1, 1, 1, 0, 1, 0}, bigf), g, &onGpu);
    CHECK(onGpu, "a 3 x 3 convolution goes to the GPU on the default policy");
    RunGpu("convolve", ConvArgs({1.0}, bigf), g, &onGpu);
    CHECK(!onGpu, "a 1 x 1 kernel stays on the CPU on the default policy");
    gpu::GetPolicy().minCells = 0.0f;
  }
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
  // size gates on the default policy: a bare stencil needs a big field, blur based ops always go
  {
    gpu::GetPolicy().minCells = 65536.0f;
    const GridPtr mid = TestTerrain(300, 300, 1.0f, 3, 30.0);
    Grid g;
    bool onGpu = true;
    RunGpu("edge", Args({}, {{"mode", "sobel"}}, {{"a", mid}}), g, &onGpu);
    CHECK(!onGpu, "a bare sobel on 300 x 300 stays on the CPU on the default policy");
    RunGpu("edge", Args({{"smooth", 5.0}}, {{"mode", "sobel"}}, {{"a", mid}}), g, &onGpu);
    CHECK(onGpu, "a smoothed sobel goes to the GPU on the default policy");
    RunGpu("unsharp", Args({{"radius", 6.0}}, {}, {{"a", mid}}), g, &onGpu);
    CHECK(onGpu, "an unsharp mask with a 6 sample blur goes to the GPU");
    gpu::GetPolicy().minCells = 0.0f;
  }
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
  TestMorph();
  TestConvolve();
  TestBlurBased();
  gpu::SetBackendForTest(nullptr);
  std::printf("EdGpuFieldTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
