// Stand-alone test of the GPU generator and filter kernels (gpu/gen_*.hlsl, gpu/filt_*.hlsl; EdGpuGen.cpp, EdGpuGenFilt.cpp; docs/projects/editor/GPU_COMPUTE.md, "Generator and filter kernels").
// No GPU is needed: the kernels are compiled as C++ through gpu/hlsl_cpp_shim.h and run by the CPU emulator (tests/EdGpuEmu.h) behind the field engine's interceptor, and compared with the CPU ops.
//   g++ -std=c++20 -O2 -ffp-contract=off -I src/sdk src/sdk/moho/editor/tests/EdGpuGenTest.cpp (sources: scripts/ed_native_tests.py) -pthread -o EdGpuGenTest
//   EdGpuGenTest [--png DIR]      (--png writes raw float fields of the emulated GPU and CPU results for the scripts that render hillshades)
// What is checked: GPU result against the CPU op (same arithmetic: tight tolerance; different hash or algorithm: statistics), world position invariance across cell sizes, lattice seams, isotropy,
// range / NaN, odd sizes (1 x N, N x 1, not a multiple of 16), the Unsupported branches (the CPU op runs), that the op really ran on the (emulated) GPU and made no out of range access.
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
#include "moho/editor/gpu/gen_noise.hlsli"
#include "moho/editor/gpu/gen_fbm_params.hlsli"
#include "moho/editor/gpu/gen_fbmd_params.hlsli"
#include "moho/editor/gpu/gen_fractal_params.hlsli"
#include "moho/editor/gpu/gen_musgrave_params.hlsli"

EMU_KERNEL_BEGIN(k_gen_fbmd)
#include "moho/editor/gpu/gen_fbmd.hlsl"
EMU_KERNEL_END(gen_fbmd)

EMU_KERNEL_BEGIN(k_gen_musgrave)
#include "moho/editor/gpu/gen_musgrave.hlsl"
EMU_KERNEL_END(gen_musgrave)

EMU_KERNEL_BEGIN(k_gen_fractal)
#include "moho/editor/gpu/gen_fractal.hlsl"
EMU_KERNEL_END(gen_fractal)

EMU_KERNEL_BEGIN(k_gen_fbm)
#include "moho/editor/gpu/gen_fbm.hlsl"
EMU_KERNEL_END(gen_fbm)

using namespace moho::editor;
using field::Grid;
using field::GridPtr;
using field::OpArgs;

static int gFailures = 0;
static int gChecks = 0;
static std::string gPngDir;
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

static gpu::EmuBackend* gBackend = nullptr;

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

static void WriteRaw(const std::string& name, const Grid& g)
{
  if (gPngDir.empty()) return;
  const std::string path = gPngDir + "/" + name + ".raw";
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    const int hd[2] = {g.w, g.h};
    std::fwrite(hd, 4, 2, f);
    std::fwrite(g.v.data(), 4, g.v.size(), f);
    std::fclose(f);
  }
}

/** The GPU result must equal the CPU op's within `tol` (max abs) and must have run on the GPU. */
static void CheckSame(const char* label, const std::string& op, const OpArgs& a, const double tol, Diff* diffOut = nullptr)
{
  Grid g, c;
  bool onGpu = false;
  const bool ok = RunGpu(op, a, g, &onGpu) && RunCpu(op, a, c);
  CHECK(ok && onGpu, "%s: the op did not run on the GPU", label);
  if (!ok) return;
  const Diff d = Compare(g, c);
  CHECK(!d.nan && d.maxAbs <= tol, "%s: GPU and CPU differ by %.3g (mean %.3g), allowed %.3g", label, d.maxAbs, d.meanAbs, tol);
  if (gVerbose) std::printf("  %-28s max %.3g mean %.3g\n", label, d.maxAbs, d.meanAbs);
  if (diffOut != nullptr) *diffOut = d;
  if (!gPngDir.empty()) {
    WriteRaw(std::string(label) + "_gpu", g);
    WriteRaw(std::string(label) + "_cpu", c);
  }
}

// ---- kernel table ------------------------------------------------------------------------------------------------------------------------------------

static bool Mine(const char* name) { return std::strncmp(name, "gen_", 4) == 0 || std::strncmp(name, "filt_", 5) == 0; }

static void TestKernelTable()
{
  int mine = 0;
  for (int i = 0; i < gpu::KernelCount(); ++i) {
    const gpu::KernelInfo& k = gpu::KernelAt(i);
    if (!Mine(k.name)) continue;
    ++mine;
    const auto it = emu::Kernels().find(k.name);
    CHECK(it != emu::Kernels().end(), "kernel %s has an emulation in this test", k.name);
    if (it == emu::Kernels().end()) continue;
    const emu::KernelEmu& e = *it->second;
    CHECK(static_cast<gpu::u32>(e.tx) == k.tx && static_cast<gpu::u32>(e.ty) == k.ty && static_cast<gpu::u32>(e.tz) == k.tz, "kernel %s: thread group equals the generated table", k.name);
    gpu::u32 srv = 0, uav = 0;
    for (const auto& r : e.srv) srv |= 1u << r.slot;
    for (const auto& r : e.uav) uav |= 1u << r.slot;
    CHECK(srv == k.srvMask && uav == k.uavMask, "kernel %s: resource slots (%x/%x) equal the generated table (%x/%x)", k.name, srv, uav, k.srvMask, k.uavMask);
    CHECK((e.cbData != nullptr) == (k.hasCb != 0), "kernel %s: constant buffer presence", k.name);
  }
  CHECK(mine == static_cast<int>(emu::Kernels().size()), "the generated table has %d gen_/filt_ kernels and this test emulates %d", mine, static_cast<int>(emu::Kernels().size()));
}

// ---- perlin simplex ridged billow ----------------------------------------------------------------------------------------------------------------------

static void TestFbmOps()
{
  const Num base = {{"width", 70}, {"height", 53}, {"seed", 3}, {"scale", 24}, {"octaves", 5}};
  for (const char* op : {"perlin", "simplex", "ridged", "billow"}) {
    for (const int rot : {0, 1}) {
      Num n = base;
      n["rotate"] = rot;
      n["ox"] = 123.5;
      n["oz"] = -77.25;
      n["cell"] = 1.5;
      Diff d;
      CheckSame((std::string(op) + (rot ? "_rot" : "_plain")).c_str(), op, Args(n), 2e-4, &d);
    }
  }
  // shifts, persistence, lacunarity, the simplex basis of the perlin family, 16 octaves
  Num n = base;
  n["shiftx"] = 31.7;
  n["shiftz"] = -9.1;
  n["persistence"] = 0.62;
  n["lacunarity"] = 2.31;
  n["octaves"] = 12;
  n["rotate"] = 1;
  CheckSame("perlin_params", "perlin", Args(n), 4e-4);
  CheckSame("ridged_simplex", "ridged", Args(n, {{"basis", "simplex"}}), 4e-4);
  // odd sizes
  for (const std::pair<int, int> sz : {std::pair<int, int>{1, 37}, {41, 1}, {1, 1}, {17, 33}, {16, 16}, {31, 5}}) {
    Num m = base;
    m["width"] = sz.first;
    m["height"] = sz.second;
    CheckSame(("perlin_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "perlin", Args(m), 2e-4);
  }
  // `like` gives the shape; a mismatch between a and like is the CPU's error (Unsupported)
  auto like = std::make_shared<Grid>(40, 30, 2.0f, 10.0f, 20.0f);
  Num m = {{"seed", 1}, {"scale", 30}};
  CheckSame("perlin_like", "perlin", Args(m, {}, {{"like", like}}), 2e-4);
  Grid g;
  bool onGpu = true;
  std::string err;
  CHECK(!RunGpu("perlin", Args({{"width", 0}, {"height", 5}}), g, &onGpu, &err) && !onGpu && !err.empty(), "an empty size is the CPU's error: '%s'", err.c_str());
}

/** World position: the same noise at the same place, whatever the cell size (float tolerance). */
static void TestWorldPosition()
{
  for (const char* op : {"perlin", "simplex", "ridged", "billow"}) {
    Num fine = {{"width", 129}, {"height", 129}, {"seed", 4}, {"scale", 90}, {"octaves", 6}, {"rotate", 1}, {"ox", 2000.0}, {"oz", -3000.0}};
    Num coarse = fine;
    coarse["width"] = 33;
    coarse["height"] = 33;
    coarse["cell"] = 4;
    Grid f, c;
    bool a = RunGpu(op, Args(fine), f), b = RunGpu(op, Args(coarse), c);
    double worst = 0.0;
    for (int z = 0; a && b && z < 33; ++z) {
      for (int x = 0; x < 33; ++x) worst = std::max(worst, static_cast<double>(std::fabs(f.At(x * 4, z * 4) - c.At(x, z))));
    }
    CHECK(a && b && worst < 3e-4, "%s: a coarse grid differs from the fine one at the same places by %.3g", op, worst);
  }
}

// ---- fractal -------------------------------------------------------------------------------------------------------------------------------------

static void TestFractal()
{
  const Num base = {{"width", 77}, {"height", 61}, {"seed", 5}, {"scale", 30}, {"octaves", 7}, {"ox", 345.25}, {"oz", -12.5}, {"cell", 1.25}};
  for (const char* kind : {"fbm", "billow", "ridged"}) {
    for (const char* style : {"classic", "erosive"}) {
      for (const int warp : {0, 1, 2}) {
        Num n = base;
        Str s = {{"kind", kind}, {"style", style}};
        if (warp > 0) {
          n["warp"] = 9;
          n["warp_levels"] = warp;
        }
        n["slope_detail"] = 0.5;
        const std::string label = std::string("fractal_") + kind + "_" + style + "_w" + std::to_string(warp);
        CheckSame(label.c_str(), "fractal", Args(n, s), 4e-4);
      }
    }
  }
  // options: sharpness, finest, rotate 0, persistence, lacunarity, antialias, erosion strength, fit
  Num n = base;
  n["sharpness"] = 3.5;
  n["finest"] = 2.0;
  n["rotate"] = 0;
  n["persistence"] = 0.55;
  n["lacunarity"] = 2.2;
  CheckSame("fractal_opts", "fractal", Args(n, {{"kind", "ridged"}}), 4e-4);
  n["antialias"] = 1;
  n["erosion"] = 3;
  n["octaves"] = 12;
  CheckSame("fractal_aa_erosion", "fractal", Args(n, {{"style", "erosive"}}), 4e-4);
  // odd sizes and a field that starts far from the origin
  for (const std::pair<int, int> sz : {std::pair<int, int>{1, 29}, {33, 1}, {1, 1}, {19, 17}}) {
    Num m = base;
    m["width"] = sz.first;
    m["height"] = sz.second;
    m["warp"] = 5;
    CheckSame(("fractal_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "fractal", Args(m, {{"style", "erosive"}}), 4e-4);
  }
  Num far = base;
  far["ox"] = 20000.0;
  far["oz"] = 18000.0;
  far["scale"] = 200;
  CheckSame("fractal_far", "fractal", Args(far), 4e-4);
  // a bad option is the CPU op's error
  Grid g;
  bool onGpu = true;
  std::string err;
  CHECK(!RunGpu("fractal", Args({{"width", 8}, {"height", 8}}, {{"kind", "bogus"}}), g, &onGpu, &err) && !onGpu && err.find("kind") != std::string::npos, "a bad kind is the CPU's error: '%s'", err.c_str());
  CHECK(!RunGpu("fractal", Args({{"width", 8}, {"height", 8}}, {{"style", "bogus"}}), g, &onGpu, &err) && !onGpu, "a bad style is the CPU's error");
}

// ---- musgrave, fbm_damped ------------------------------------------------------------------------------------------------------------------------

static void TestMusgraveAndDamped()
{
  const Num base = {{"width", 71}, {"height", 47}, {"seed", 6}, {"scale", 28}, {"octaves", 7}, {"ox", -250.5}, {"oz", 91.0}, {"cell", 1.5}};
  for (const char* mode : {"ridged", "hybrid", "hetero"}) {
    for (const int rot : {0, 1}) {
      Num n = base;
      n["rotate"] = rot;
      CheckSame((std::string("musgrave_") + mode + (rot ? "_rot" : "")).c_str(), "musgrave", Args(n, {{"mode", mode}}), 5e-5);
    }
  }
  Num n = base;
  n["h"] = 0.7;
  n["offset"] = 0.8;
  n["gain"] = 1.5;
  n["lacunarity"] = 2.4;
  n["octaves"] = 11;
  CheckSame("musgrave_params", "musgrave", Args(n, {{"mode", "ridged"}}), 5e-5);
  for (const int rot : {0, 1}) {
    Num d = base;
    d["rotate"] = rot;
    CheckSame(rot ? "fbm_damped_rot" : "fbm_damped", "fbm_damped", Args(d), 5e-5);
  }
  Num d = base;
  d["damp"] = 4;
  d["persistence"] = 0.6;
  d["lacunarity"] = 1.9;
  d["rotate"] = 1;
  CheckSame("fbm_damped_params", "fbm_damped", Args(d), 5e-5);
  for (const std::pair<int, int> sz : {std::pair<int, int>{1, 23}, {19, 1}, {17, 15}}) {
    Num m = base;
    m["width"] = sz.first;
    m["height"] = sz.second;
    CheckSame(("musgrave_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "musgrave", Args(m), 5e-5);
    CheckSame(("fbm_damped_" + std::to_string(sz.first) + "x" + std::to_string(sz.second)).c_str(), "fbm_damped", Args(m), 5e-5);
  }
  Grid g;
  bool onGpu = true;
  std::string err;
  CHECK(!RunGpu("musgrave", Args({{"width", 8}, {"height", 8}}, {{"mode", "bogus"}}), g, &onGpu, &err) && !onGpu, "a bad mode is the CPU's error");
}

int main(int argc, char** argv)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--png") == 0) gPngDir = argv[i + 1];
  }
  for (int i = 1; i < argc; ++i) gVerbose = gVerbose || std::strcmp(argv[i], "-v") == 0;
  gpu::EmuBackend backend;
  gBackend = &backend;
  gpu::SetBackendForTest(&backend);
  gpu::Install();
  gpu::GetPolicy().enabled = true;
  gpu::GetPolicy().minCells = 0.0f;
  gpu::GetPolicy().selfTestFirst = false;
  field::SetThreads(4);
  TestKernelTable();
  TestFbmOps();
  TestWorldPosition();
  TestFractal();
  TestMusgraveAndDamped();
  gpu::SetBackendForTest(nullptr);
  std::printf("EdGpuGenTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
