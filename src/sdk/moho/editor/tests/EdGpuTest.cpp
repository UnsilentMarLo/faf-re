// Stand-alone test of the GPU compute foundation (EdGpu.h, EdGpuOps.h, gpu/*.hlsl; docs/projects/editor/GPU_COMPUTE.md). No GPU is needed: the kernels are compiled as C++ through
// gpu/hlsl_cpp_shim.h and run by the CPU emulator (tests/EdGpuEmu.h), and compared with the CPU ops.
//   g++ -std=c++20 -O2 -ffp-contract=off -I src/sdk src/sdk/moho/editor/tests/EdGpuTest.cpp src/sdk/moho/editor/EdFieldCore.cpp src/sdk/moho/editor/EdGpuOps.cpp src/sdk/moho/editor/EdGpu.cpp -o EdGpuTest -pthread
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdGpuTest.cpp EdFieldCore.cpp EdGpuOps.cpp EdGpu.cpp   (scripts/ed_native_tests.py)
// On Windows the test also tries the real Direct3D 11 backend: it prints what it found and runs the self test when a device exists (a machine without one is not a failure).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "moho/editor/EdFieldCore.h"
#include "moho/editor/EdGpu.h"
#include "moho/editor/EdGpuOps.h"

// ---- the kernel language, then every kernel as C++ in its own namespace ----------------------------------------------------------------------------
#include "moho/editor/gpu/hlsl_cpp_shim.h"
#include "moho/editor/tests/EdGpuEmu.h"

// every .hlsli at global scope first (a kernel namespace sees them; their include guards make the kernels' own #include lines no-ops)
#include "moho/editor/gpu/common.hlsli"
#include "moho/editor/gpu/blur_params.hlsli"
#include "moho/editor/gpu/field_params.hlsli"
#include "moho/editor/gpu/reduce_params.hlsli"
#include "moho/editor/gpu/scale_params.hlsli"

EMU_KERNEL_BEGIN(k_blur_pass)
#include "moho/editor/gpu/blur_pass.hlsl"
EMU_KERNEL_END(blur_pass)

EMU_KERNEL_BEGIN(k_buf_to_tex)
#include "moho/editor/gpu/buf_to_tex.hlsl"
EMU_KERNEL_END(buf_to_tex)

EMU_KERNEL_BEGIN(k_reduce_sum)
#include "moho/editor/gpu/reduce_sum.hlsl"
EMU_KERNEL_END(reduce_sum)

#include "moho/editor/tests/EdGpuErosionKernels.inc"      // gpu/erosion_*.hlsl (GP2)

EMU_KERNEL_BEGIN(k_scale_offset)
#include "moho/editor/gpu/scale_offset.hlsl"
EMU_KERNEL_END(scale_offset)

EMU_KERNEL_BEGIN(k_tex_resample)
#include "moho/editor/gpu/tex_resample.hlsl"
EMU_KERNEL_END(tex_resample)

using namespace moho::editor;
using field::Grid;
using field::OpArgs;

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

static_assert(sizeof(BlurParams) % 16 == 0 && sizeof(FieldParams) % 16 == 0 && sizeof(ReduceParams) % 16 == 0 && sizeof(ScaleParams) % 16 == 0, "constant buffer structs are padded to 16 bytes");

static std::shared_ptr<Grid> MakeGrid(const int w, const int h, const float cell, const std::uint32_t seed)
{
  auto g = std::make_shared<Grid>(w, h, cell, 3.0f, -5.0f);
  std::uint32_t s = seed * 2654435761u + 12345u;
  for (float& v : g->v) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    v = static_cast<float>(s >> 8) * (1.0f / 16777216.0f) * 50.0f - 10.0f;
  }
  return g;
}

static double MaxDiff(const Grid& a, const Grid& b)
{
  if (!a.SameShape(b)) return 1e30;
  double worst = 0.0;
  for (std::size_t i = 0; i < a.v.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(a.v[i]) - static_cast<double>(b.v[i])));
  return worst;
}

// ---- shim -------------------------------------------------------------------------------------------------------------------------------------------

static void TestShim()
{
  CHECK(saturate(-0.5f) == 0.0f && saturate(1.5f) == 1.0f && saturate(0.25f) == 0.25f, "saturate");
  CHECK(lerp(2.0f, 4.0f, 0.25f) == 2.5f, "lerp");
  CHECK(std::fabs(frac(2.75f) - 0.75f) < 1e-7f && std::fabs(frac(-0.25f) - 0.75f) < 1e-7f, "frac like HLSL (x - floor(x))");
  CHECK(step(0.5f, 0.7f) == 1.0f && step(0.5f, 0.2f) == 0.0f && step(0.5f, 0.5f) == 1.0f, "step");
  CHECK(smoothstep(0.0f, 1.0f, 0.5f) == 0.5f && smoothstep(0.0f, 1.0f, -1.0f) == 0.0f && smoothstep(0.0f, 1.0f, 2.0f) == 1.0f, "smoothstep");
  CHECK(min(3, 4) == 3 && max(3u, 4u) == 4u && clamp(5, 0, 2) == 2 && min(1.5f, -2.0f) == -2.0f, "scalar min / max / clamp");
  CHECK(asuint(1.0f) == 0x3F800000u && asfloat(0x40000000u) == 2.0f && asint(-1.0f) == static_cast<int>(0xBF800000u), "bit casts");
  CHECK(countbits(0xF0F0u) == 8u, "countbits");

  const float3 a(1.0f, 2.0f, 3.0f);
  const float3 b(4.0f, 5.0f, 6.0f);
  CHECK(dot(a, b) == 32.0f, "dot");
  CHECK(std::fabs(length(float2(3.0f, 4.0f)) - 5.0f) < 1e-6f, "length");
  const float3 s = a + b * 2.0f - float3(1.0f);
  CHECK(s.x == 8.0f && s.y == 11.0f && s.z == 14.0f, "vector operators and scalar promotion");
  const float3 f = floor(float3(1.5f, -1.5f, 2.0f));
  CHECK(f.x == 1.0f && f.y == -2.0f && f.z == 2.0f, "floor on vectors");
  const float2 m = max(float2(1.0f, 5.0f), 2.0f);
  CHECK(m.x == 2.0f && m.y == 5.0f, "max of a vector and a scalar");
  const float2 c = clamp(float2(-1.0f, 3.0f), 0.0f, 2.0f);
  CHECK(c.x == 0.0f && c.y == 2.0f, "clamp of a vector with scalar bounds");
  const float4 q = lerp(float4(0.0f), float4(10.0f), 0.5f);
  CHECK(q.w == 5.0f, "lerp of vectors with a scalar t");
  const uint3 u3 = uint3(1u, 2u, 3u);
  const int3 i3 = int3(u3);
  CHECK(i3.z == 3, "explicit vector conversion");
  CHECK(cross(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f)).z == 1.0f, "cross");
  float sn = 0.0f, cs = 0.0f;
  sincos(0.0f, sn, cs);
  CHECK(sn == 0.0f && cs == 1.0f, "sincos");

  uint v = 5u, orig = 0u;
  InterlockedAdd(v, 3u);
  InterlockedAdd(v, 2u, orig);
  CHECK(v == 10u && orig == 8u, "InterlockedAdd");
  InterlockedMax(v, 7u);
  InterlockedMin(v, 4u);
  CHECK(v == 4u, "InterlockedMax / Min");
  InterlockedCompareExchange(v, 4u, 9u, orig);
  CHECK(v == 9u && orig == 4u, "InterlockedCompareExchange");

  CHECK(Hash32(1u) != Hash32(2u) && Hash32(0u) == 0u, "Hash32 (common.hlsli) is a bijection with a fixed zero");
  CHECK(HashToUnit(0xFFFFFFFFu) < 1.0f && HashToUnit(0u) == 0.0f, "HashToUnit stays in [0, 1)");
  CHECK(ClampIndex(-3, 10) == 0 && ClampIndex(12, 10) == 9 && FieldIndex(2u, 3u, 10u) == 32u, "common.hlsli helpers");

  emu::Texture2D tex;
  const float texels[4] = {0.0f, 1.0f, 2.0f, 3.0f};
  emu::ResView tv;
  tv.data = const_cast<float*>(texels);
  tv.w = 2;
  tv.h = 2;
  tv.texture = true;
  std::string err;
  CHECK(tex.Bind(tv, err), "bind texture");
  emu::SamplerState lin;
  emu::SamplerState pt;
  pt.slot = 1;
  CHECK(std::fabs(tex.SampleLevel(lin, float2(0.5f, 0.5f), 0.0f) - 1.5f) < 1e-6f, "bilinear at the centre of the four texels = their mean");
  CHECK(tex.SampleLevel(lin, float2(0.25f, 0.25f), 0.0f) == 0.0f, "bilinear at a texel centre = the texel");
  CHECK(tex.SampleLevel(lin, float2(-1.0f, 0.25f), 0.0f) == 0.0f, "clamp to edge");
  CHECK(tex.SampleLevel(pt, float2(0.75f, 0.75f), 0.0f) == 3.0f, "point sampling");
  CHECK(emu::gOob == 0, "no out of range access so far");
}

// ---- kernels on the emulator -------------------------------------------------------------------------------------------------------------------------

// Kernels that another test binary emulates and checks (EdGpuGenTest: gen_*, filt_*; the erosion tests: erosion_*): this test does not need an EMU_KERNEL block for them.
static bool OwnedByAnotherTest(const char* const name)
{
  return std::strncmp(name, "gen_", 4) == 0 || std::strncmp(name, "filt_", 5) == 0 || std::strncmp(name, "erosion_", 8) == 0;
}

static void TestKernelTable()
{
  int mine = 0;
  for (int i = 0; i < gpu::KernelCount(); ++i) mine += OwnedByAnotherTest(gpu::KernelAt(i).name) ? 0 : 1;
  int emulated = 0;
  for (const auto& kv : emu::Kernels()) emulated += OwnedByAnotherTest(kv.first.c_str()) ? 0 : 1;
  CHECK(mine > 0 && mine == emulated, "the generated table has %d kernels for this test and it emulates %d: every gpu/*.hlsl needs an EMU_KERNEL block here", mine, emulated);
  for (int i = 0; i < gpu::KernelCount(); ++i) {
    const gpu::KernelInfo& k = gpu::KernelAt(i);
    if (OwnedByAnotherTest(k.name)) continue;
    const auto it = emu::Kernels().find(k.name);
    CHECK(it != emu::Kernels().end(), "kernel %s has an emulation in this test", k.name);
    if (it == emu::Kernels().end()) continue;
    const emu::KernelEmu& e = *it->second;
    CHECK(static_cast<gpu::u32>(e.tx) == k.tx && static_cast<gpu::u32>(e.ty) == k.ty && static_cast<gpu::u32>(e.tz) == k.tz, "kernel %s: thread group of the C++ side equals the generated table", k.name);
    gpu::u32 srv = 0, uav = 0;
    for (const auto& r : e.srv) srv |= 1u << r.slot;
    for (const auto& r : e.uav) uav |= 1u << r.slot;
    CHECK(srv == k.srvMask && uav == k.uavMask, "kernel %s: resource slots of the C++ side (%x/%x) equal the generated table (%x/%x)", k.name, srv, uav, k.srvMask, k.uavMask);
    CHECK((e.cbData != nullptr) == (k.hasCb != 0), "kernel %s: constant buffer presence", k.name);
    CHECK(std::strstr(k.source, "CSMain") != nullptr && std::strstr(k.source, "#define CBUFFER") != nullptr, "kernel %s: source has the prelude and the entry point", k.name);
  }
  CHECK(gpu::FindKernel("blur_pass") != nullptr && gpu::FindKernel("nope") == nullptr, "FindKernel");
}

static void TestScaleOffset()
{
  gpu::EmuExecutor ex;
  std::string err;
  const gpu::KernelInfo* k = gpu::FindKernel("scale_offset");
  const std::uint32_t n = 1000;   // not a multiple of the group size: the guard in the kernel matters
  std::vector<float> in(n), out(n, -1.0f);
  for (std::uint32_t i = 0; i < n; ++i) in[i] = static_cast<float>(i) * 0.5f - 100.0f;
  std::unique_ptr<gpu::Resource> a, b;
  CHECK(gpu::MakeBuffer(ex, n, a, err) && gpu::MakeBuffer(ex, n, b, err) && gpu::UploadBuffer(ex, *a, in.data(), n * 4, err), "buffers: %s", err.c_str());
  ScaleParams p{};
  p.count = n;
  p.scale = 2.5f;
  p.offset = 7.0f;
  gpu::Bindings bind;
  bind.Srv(0, *a).Uav(0, *b);
  CHECK(gpu::DispatchGrid(ex, *k, n, 1, p, bind, err), "dispatch: %s", err.c_str());
  CHECK(ex.Download(*b, out.data(), n * 4, err), "download: %s", err.c_str());
  bool same = true;
  for (std::uint32_t i = 0; i < n; ++i) same = same && out[i] == in[i] * 2.5f + 7.0f;
  CHECK(same, "scale_offset equals the CPU result");
  CHECK(emu::gOob == 0, "scale_offset made %d out of range accesses", emu::gOob);

  // misuse is refused with a message, not a crash
  gpu::Bindings none;
  CHECK(!gpu::DispatchGrid(ex, *k, n, 1, p, none, err) && !err.empty(), "unbound slots are refused: %s", err.c_str());
  gpu::Bindings both;
  both.Srv(0, *a).Uav(0, *a);
  CHECK(!gpu::DispatchGrid(ex, *k, n, 1, p, both, err), "a resource as SRV and UAV of one dispatch is refused");
  CHECK(!ex.Dispatch(*k, gpu::Groups{1, 1, 1}, nullptr, 0, bind, err), "a missing constant buffer is refused");
  CHECK(!gpu::DispatchGrid(ex, *k, 0, 1, p, bind, err), "an empty dispatch is refused");
}

static void TestReduceBarriers()
{
  gpu::EmuExecutor ex;
  std::string err;
  const gpu::KernelInfo* k = gpu::FindKernel("reduce_sum");
  for (const std::uint32_t n : {1u, 255u, 256u, 257u, 5000u}) {
    const std::uint32_t groups = (n + 255) / 256;
    std::vector<float> in(n), out(groups, -1.0f);
    for (std::uint32_t i = 0; i < n; ++i) in[i] = static_cast<float>((i * 7) % 11);
    std::unique_ptr<gpu::Resource> a, b;
    CHECK(gpu::MakeBuffer(ex, n, a, err) && gpu::MakeBuffer(ex, groups, b, err) && gpu::UploadBuffer(ex, *a, in.data(), n * 4, err), "buffers: %s", err.c_str());
    ReduceParams p{};
    p.count = n;
    gpu::Bindings bind;
    bind.Srv(0, *a).Uav(0, *b);
    CHECK(ex.Dispatch(*k, gpu::Groups{groups, 1, 1}, &p, sizeof(p), bind, err), "dispatch: %s", err.c_str());
    CHECK(ex.Download(*b, out.data(), groups * 4, err), "download: %s", err.c_str());
    bool same = true;
    for (std::uint32_t g = 0; g < groups; ++g) {
      float expect = 0.0f;
      for (std::uint32_t i = g * 256; i < std::min(n, g * 256 + 256); ++i) expect += in[i];
      same = same && out[g] == expect;
    }
    CHECK(same, "reduce_sum with barriers is right for n = %u", n);
  }
  CHECK(emu::gOob == 0, "reduce_sum made %d out of range accesses", emu::gOob);
}

static void TestBlurKernelsDirect()
{
  // the op through the interceptor on the emulator, several shapes (the group size is 16 x 16: sizes around it and degenerate ones)
  gpu::EmuBackend backend;
  gpu::SetBackendForTest(&backend);
  gpu::Install();
  gpu::GetPolicy().enabled = true;
  gpu::GetPolicy().minCells = 0.0f;
  gpu::GetPolicy().selfTestFirst = false;
  struct Shape { int w, h; float cell; double radius; const char* op; };
  const Shape shapes[] = {{64, 64, 1.0f, 3.0, "blur"},  {33, 17, 2.0f, 5.0, "blur"}, {1, 40, 1.0f, 2.0, "blur"}, {40, 1, 1.0f, 2.0, "blur"}, {5, 5, 1.0f, 20.0, "blur"},
                          {64, 48, 1.0f, 4.0, "box"},   {31, 33, 0.5f, 3.0, "box"},   {100, 70, 4.0f, 30.0, "blur"}, {16, 16, 1.0f, 1.0, "box"}};
  int index = 0;
  for (const Shape& s : shapes) {
    const auto in = MakeGrid(s.w, s.h, s.cell, static_cast<std::uint32_t>(++index));
    OpArgs args;
    args.in["a"] = in;
    args.p.num["radius"] = s.radius;
    Grid viaEngine, cpu;
    std::string err1, err2;
    const auto before = field::OpStats(false);
    const std::uint64_t gpuBefore = before.count(s.op) ? before.at(s.op).gpuCalls : 0;
    CHECK(field::RunOp(s.op, args, viaEngine, err1), "%s %dx%d: RunOp: %s", s.op, s.w, s.h, err1.c_str());
    CHECK(field::RunOpCpu(s.op, args, cpu, err2), "%s %dx%d: CPU: %s", s.op, s.w, s.h, err2.c_str());
    const auto after = field::OpStats(false);
    CHECK(after.count(s.op) && after.at(s.op).gpuCalls == gpuBefore + 1, "%s %dx%d ran on the (emulated) GPU", s.op, s.w, s.h);
    const double worst = MaxDiff(viaEngine, cpu);
    CHECK(worst <= 1e-5, "%s %dx%d radius %g: GPU kernels differ from the CPU op by %g", s.op, s.w, s.h, s.radius, worst);
    CHECK(viaEngine.cell == cpu.cell && viaEngine.ox == cpu.ox && viaEngine.oz == cpu.oz, "%s %dx%d: placement of the result", s.op, s.w, s.h);
  }
  CHECK(emu::gOob == 0, "blur_pass made %d out of range accesses", emu::gOob);
  gpu::SetBackendForTest(nullptr);
}

// ---- policy and fall-back ----------------------------------------------------------------------------------------------------------------------------

static void TestPolicyAndFallback()
{
  gpu::EmuBackend backend;
  gpu::SetBackendForTest(&backend);
  gpu::Install();
  gpu::Policy& policy = gpu::GetPolicy();
  policy.enabled = true;
  policy.minCells = 1000.0f;
  policy.selfTestFirst = false;
  const auto in = MakeGrid(64, 64, 1.0f, 99);
  OpArgs args;
  args.in["a"] = in;
  args.p.num["radius"] = 3.0;
  Grid cpu;
  std::string err;
  CHECK(field::RunOpCpu("blur", args, cpu, err), "CPU reference: %s", err.c_str());

  const auto gpuCalls = [&]() { const auto s = field::OpStats(false); return s.count("blur") ? s.at("blur").gpuCalls : 0ull; };
  const auto fallbacks = [&]() { const auto s = field::OpStats(false); return s.count("blur") ? s.at("blur").fallbacks : 0ull; };
  Grid out;

  // normal
  std::uint64_t g0 = gpuCalls();
  CHECK(field::RunOp("blur", args, out, err) && gpuCalls() == g0 + 1 && MaxDiff(out, cpu) <= 1e-5, "normal run is on the GPU");

  // switched off
  policy.enabled = false;
  g0 = gpuCalls();
  out = Grid();
  CHECK(field::RunOp("blur", args, out, err) && gpuCalls() == g0 && MaxDiff(out, cpu) == 0.0, "ed_gpu_compute 0: CPU");
  policy.enabled = true;

  // too small
  policy.minCells = 5000.0f;
  g0 = gpuCalls();
  CHECK(field::RunOp("blur", args, out, err) && gpuCalls() == g0, "below ed_gpu_min_cells (4096 < 5000): CPU");
  policy.minCells = 1000.0f;

  // no GPU implementation of the op
  OpArgs inv;
  inv.in["a"] = in;
  Grid invOut;
  CHECK(field::RunOp("invert", inv, invOut, err), "an op without a GPU version runs on the CPU: %s", err.c_str());

  // parameters the GPU version does not handle: mask, radius 0, a `like` of another size
  {
    OpArgs masked = args;
    masked.in["mask"] = MakeGrid(64, 64, 1.0f, 5);
    Grid m1, m2;
    g0 = gpuCalls();
    const std::uint64_t f0 = fallbacks();
    // a mask of the field's shape is blended by the filt_lerpmask kernel (emulated and compared in EdGpuGenTest); a mask of another size is ignored by the CPU op, so it is here
    masked.in["mask"] = MakeGrid(7, 5, 1.0f, 5);
    CHECK(field::RunOp("blur", masked, m1, err) && field::RunOpCpu("blur", masked, m2, err) && MaxDiff(m1, m2) <= 1e-5 && gpuCalls() == g0 + 1 && fallbacks() == f0, "a mask of another size is ignored like on the CPU");
    OpArgs zero = args;
    zero.p.num["radius"] = 0.0;
    g0 = gpuCalls();
    CHECK(field::RunOp("blur", zero, m1, err) && gpuCalls() == g0, "radius 0: CPU");
  }

  // a failure in the middle of the op: the CPU op runs, `out` is untouched by the GPU attempt, the fall-back is counted and explained
  {
    backend.exec.dispatches = 0;
    backend.exec.failAtDispatch = 1;   // the second pass
    Grid res;
    const std::uint64_t f0 = fallbacks();
    g0 = gpuCalls();
    CHECK(field::RunOp("blur", args, res, err) && MaxDiff(res, cpu) == 0.0, "a failing kernel falls back to the CPU result");
    CHECK(fallbacks() == f0 + 1 && gpuCalls() == g0, "the fall-back is counted");
    CHECK(gpu::LastOpError().find("injected failure") != std::string::npos, "the reason is kept: '%s'", gpu::LastOpError().c_str());
    CHECK(!backend.failed, "an ordinary failure does not switch the backend off");
    backend.exec.failAtDispatch = -1;
  }

  // TryRun leaves `out` untouched when it declines
  {
    Grid sentinel(3, 3, 1.0f, 0.0f, 0.0f, 7.0f);
    backend.exec.dispatches = 0;
    backend.exec.failAtDispatch = 0;
    const bool handled = gpu::TryRun("blur", args, sentinel);
    CHECK(!handled && sentinel.w == 3 && sentinel.v[0] == 7.0f, "TryRun declines without touching out");
    backend.exec.failAtDispatch = -1;
  }

  // a lost device: the backend is marked failed and later ops do not try again
  {
    backend.exec.dispatches = 0;
    backend.exec.lostAtDispatch = 0;
    Grid res;
    CHECK(field::RunOp("blur", args, res, err) && MaxDiff(res, cpu) == 0.0, "a lost device falls back to the CPU result");
    CHECK(backend.failed && backend.markFailedCalls >= 1, "the backend is off for the session after a lost device");
    const int calls = backend.exec.dispatches;
    g0 = gpuCalls();
    CHECK(field::RunOp("blur", args, res, err) && backend.exec.dispatches == calls && gpuCalls() == g0, "no further attempt on the GPU");
    backend.Reset();
  }

  // busy: another thread holds the GPU, the op does not wait
  {
    gpu::Lease hold(true);
    CHECK(static_cast<bool>(hold), "lease taken");
    bool done = false;
    Grid res;
    std::thread other([&]() {
      std::string e;
      g0 = gpuCalls();
      done = field::RunOp("blur", args, res, e) && MaxDiff(res, cpu) == 0.0 && gpuCalls() == g0;
    });
    other.join();
    CHECK(done, "a busy GPU sends the op to the CPU at once");
    gpu::Lease nested(false);
    CHECK(!nested && nested.Busy(), "a thread cannot take a second lease");
  }

  // unavailable backend
  {
    backend.available = false;
    Grid res;
    g0 = gpuCalls();
    CHECK(field::RunOp("blur", args, res, err) && gpuCalls() == g0 && MaxDiff(res, cpu) == 0.0, "an unavailable backend: CPU");
    backend.available = true;
  }
  gpu::SetBackendForTest(nullptr);
}

static void TestThreads()
{
  gpu::EmuBackend backend;
  gpu::SetBackendForTest(&backend);
  gpu::Install();
  gpu::GetPolicy().enabled = true;
  gpu::GetPolicy().minCells = 0.0f;
  gpu::GetPolicy().selfTestFirst = false;
  const auto in = MakeGrid(48, 40, 1.0f, 17);
  OpArgs args;
  args.in["a"] = in;
  args.p.num["radius"] = 2.0;
  Grid cpu;
  std::string err;
  CHECK(field::RunOpCpu("blur", args, cpu, err), "reference");
  std::vector<std::thread> pool;
  std::vector<int> ok(8, 0);
  for (int t = 0; t < 8; ++t) {
    pool.emplace_back([&, t]() {
      int good = 0;
      for (int i = 0; i < 6; ++i) {
        Grid res;
        std::string e;
        if (field::RunOp("blur", args, res, e) && MaxDiff(res, cpu) <= 1e-5) ++good;
      }
      ok[static_cast<std::size_t>(t)] = good;
    });
  }
  for (std::thread& th : pool) th.join();
  for (int t = 0; t < 8; ++t) CHECK(ok[static_cast<std::size_t>(t)] == 6, "thread %d got the right result every time (%d / 6)", t, ok[static_cast<std::size_t>(t)]);
  gpu::SetBackendForTest(nullptr);
}

static void TestSelfTestFirst()
{
  gpu::Policy& policy = gpu::GetPolicy();
  policy.enabled = true;
  policy.minCells = 0.0f;
  policy.selfTestFirst = true;
  const auto in = MakeGrid(40, 30, 1.0f, 8);
  OpArgs args;
  args.in["a"] = in;
  args.p.num["radius"] = 2.0;
  Grid cpu, out;
  std::string err;
  CHECK(field::RunOpCpu("blur", args, cpu, err), "reference");

  {   // a healthy device: the self test runs once, then the op
    gpu::EmuBackend backend;
    gpu::SetBackendForTest(&backend);
    gpu::Install();
    gpu::ResetSelfTestState();
    CHECK(!gpu::SelfTestPassed(), "no self test yet");
    CHECK(field::RunOp("blur", args, out, err) && MaxDiff(out, cpu) <= 1e-5, "first op after a self test");
    CHECK(gpu::SelfTestPassed(), "the self test passed and is remembered");
    const int after = backend.exec.dispatches;
    CHECK(after > 4, "the self test dispatched kernels (%d)", after);
    CHECK(field::RunOp("blur", args, out, err) && backend.exec.dispatches == after + 2, "the second op runs no self test (two blur passes)");
    CHECK(!backend.failed, "backend still on");
  }
  {   // a device that fails the self test is switched off and the op still gets its CPU result
    gpu::EmuBackend backend;
    backend.exec.failAtDispatch = 0;
    gpu::SetBackendForTest(&backend);
    gpu::ResetSelfTestState();
    out = Grid();
    CHECK(field::RunOp("blur", args, out, err) && MaxDiff(out, cpu) == 0.0, "a failing self test: CPU result");
    CHECK(backend.failed && !gpu::SelfTestPassed(), "the backend is off after a failed self test");
    CHECK(gpu::LastOpError().find("self test failed") != std::string::npos, "reason: '%s'", gpu::LastOpError().c_str());
  }
  {   // selfTestFirst off: straight to the op
    gpu::EmuBackend backend;
    gpu::SetBackendForTest(&backend);
    gpu::ResetSelfTestState();
    policy.selfTestFirst = false;
    CHECK(field::RunOp("blur", args, out, err) && backend.exec.dispatches == 2 && !gpu::SelfTestPassed(), "no self test when ed_gpu_selftest_first is 0");
  }
  gpu::SetBackendForTest(nullptr);
  policy.selfTestFirst = true;
}

// ---- self test, iteration helper -------------------------------------------------------------------------------------------------------------------

static void TestSelfTestOnEmulator()
{
  gpu::EmuExecutor ex;
  const gpu::SelfTestReport rep = gpu::RunSelfTest(ex);
  for (const gpu::SelfTestPart& p : rep.parts) CHECK(p.ok, "self test part '%s': %s", p.name.c_str(), p.detail.c_str());
  CHECK(rep.ok && rep.texturesOk && rep.parts.size() == 4 && rep.error.empty(), "the self test passes on the emulator (error '%s')", rep.error.c_str());

  gpu::EmuExecutor noTex;
  noTex.texturesSupported = false;
  const gpu::SelfTestReport r2 = gpu::RunSelfTest(noTex);
  CHECK(r2.ok && !r2.texturesOk, "without textures the buffer parts still pass and the texture part is reported");

  gpu::EmuExecutor broken;
  broken.failAtDispatch = 0;
  const gpu::SelfTestReport r3 = gpu::RunSelfTest(broken);
  CHECK(!r3.ok, "a failing device fails the self test");

  CHECK(emu::gOob == 0, "texture kernels made %d out of range accesses", emu::gOob);
}

static void TestIterations()
{
  CHECK(gpu::NextBatch(1, 10.0, 60.0, 100) == 4, "first batch grows at most 4 times");
  CHECK(gpu::NextBatch(4, 40.0, 60.0, 100) == 6, "6 iterations of 10 ms fill 60 ms");
  CHECK(gpu::NextBatch(8, 2000.0, 60.0, 100) == 1, "a slow iteration shrinks the batch to one");
  CHECK(gpu::NextBatch(4, 40.0, 60.0, 3) == 3, "never more than remains");
  CHECK(gpu::NextBatch(4, 40.0, 60.0, 0) == 0, "nothing remains");
  CHECK(gpu::NextBatch(2, 0.0, 60.0, 100) == 8, "an unmeasurable batch grows at the cap");

  gpu::EmuExecutor ex;
  std::string err;
  std::vector<unsigned> seen;
  gpu::IterStats st;
  CHECK(gpu::RunIterations(ex, 100, 60.0, [&](gpu::u32 i, std::string&) { seen.push_back(i); return true; }, err, &st), "RunIterations: %s", err.c_str());
  bool inOrder = seen.size() == 100;
  for (std::size_t i = 0; i < seen.size(); ++i) inOrder = inOrder && seen[i] == i;
  CHECK(inOrder, "every iteration ran once, in order");
  CHECK(st.iterations == 100 && st.submits >= 1 && st.submits < 100 && static_cast<int>(ex.syncs) == static_cast<int>(st.submits), "fast steps are batched: %u submits for 100 iterations", st.submits);

  // slow steps: the batches stay near the target (each iteration ~4 ms, target 40 ms)
  gpu::IterStats slow;
  CHECK(gpu::RunIterations(ex, 60, 40.0, [&](gpu::u32, std::string&) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); return true; }, err, &slow), "slow RunIterations");
  CHECK(slow.iterations == 60 && slow.largestBatch >= 3 && slow.maxSubmitMs < 400.0, "slow steps: largest batch %u, longest submit %.1f ms", slow.largestBatch, slow.maxSubmitMs);

  // a failing step stops the loop
  gpu::IterStats bad;
  int ran = 0;
  CHECK(!gpu::RunIterations(ex, 50, 60.0, [&](gpu::u32 i, std::string& e) { ++ran; if (i == 7) { e = "boom"; return false; } return true; }, err, &bad) && err == "boom" && ran == 8 && bad.iterations == 7, "a failing step stops the loop with its message");
  // a failing Sync stops the loop
  gpu::EmuExecutor lost;
  lost.lost = true;
  CHECK(!gpu::RunIterations(lost, 5, 60.0, [&](gpu::u32, std::string&) { return true; }, err), "a lost device stops RunIterations");
}

// ---- engine side ---------------------------------------------------------------------------------------------------------------------------------

static void TestFieldHelpers()
{
  gpu::EmuExecutor ex;
  std::string err;
  const auto g = MakeGrid(37, 29, 2.0f, 3);
  gpu::Field f;
  CHECK(gpu::UploadGrid(ex, *g, f, err) && f.w == 37 && f.h == 29, "UploadGrid: %s", err.c_str());
  Grid back(37, 29, 2.0f, 3.0f, -5.0f);
  CHECK(gpu::DownloadGrid(ex, f, back, err) && MaxDiff(back, *g) == 0.0, "upload + download round trip");
  Grid wrong(36, 29, 2.0f, 0.0f, 0.0f);
  CHECK(!gpu::DownloadGrid(ex, f, wrong, err), "download into a grid of another size is refused");
  gpu::Field huge;
  ex.maxBytes = 1000;
  CHECK(!gpu::MakeField(ex, 100, 100, huge, err), "a field larger than MaxBytes is refused: %s", err.c_str());
  {
    std::unique_ptr<gpu::Resource> counters;
    std::vector<std::uint32_t> back(10, 1u);
    CHECK(gpu::MakeBuffer(ex, 10, counters, err) && gpu::FillBuffer(ex, *counters, 0xA5A5A5A5u, err) && ex.Download(*counters, back.data(), 40, err) && back[0] == 0xA5A5A5A5u && back[9] == 0xA5A5A5A5u, "FillBuffer: %s", err.c_str());
  }
  gpu::Groups gr;
  const gpu::KernelInfo* k = gpu::FindKernel("blur_pass");
  CHECK(gpu::GroupsFor(*k, 33, 17, gr, err) && gr.x == 3 && gr.y == 2, "GroupsFor rounds up");
  CHECK(!gpu::GroupsFor(*k, 16 * 70000, 1, gr, err), "too many groups are refused");
  gpu::PingPong pp;
  gpu::EmuExecutor ex2;
  CHECK(pp.Make(ex2, 4, 4, err), "PingPong");
  const gpu::Resource* a = &pp.Src().R();
  pp.Flip();
  CHECK(&pp.Dst().R() == a && &pp.Src().R() != a, "Flip swaps source and destination");

  // texture view of a field and back through a sampling kernel at the same size: exact
  gpu::Field f2;
  std::unique_ptr<gpu::Resource> tex;
  const auto g2 = MakeGrid(20, 12, 1.0f, 4);
  CHECK(gpu::UploadGrid(ex2, *g2, f2, err) && gpu::MakeTextureFromField(ex2, f2, tex, err), "MakeTextureFromField: %s", err.c_str());
  gpu::Field out;
  CHECK(gpu::MakeField(ex2, 20, 12, out, err), "output field");
  FieldParams fp{};
  fp.w = 20; fp.h = 12; fp.srcW = 20; fp.srcH = 12;
  gpu::Bindings bind;
  bind.Srv(0, *tex).Uav(0, out.R());
  CHECK(gpu::DispatchGrid(ex2, *gpu::FindKernel("tex_resample"), 20, 12, fp, bind, err), "tex_resample: %s", err.c_str());
  Grid same(20, 12, 1.0f, 3.0f, -5.0f);
  CHECK(gpu::DownloadGrid(ex2, out, same, err) && MaxDiff(same, *g2) < 1e-4, "sampling a texture at its own texel centres returns the texels");
}

static void TestCoreHooks()
{
  // RunOp with no interceptor, and the stats table
  field::SetOpInterceptor(nullptr);
  (void)field::OpStats(true);
  OpArgs args;
  args.in["a"] = MakeGrid(8, 8, 1.0f, 1);
  Grid out;
  std::string err;
  CHECK(field::RunOp("invert", args, out, err), "invert: %s", err.c_str());
  CHECK(!field::RunOp("no_such_op", args, out, err) && err.find("unknown op") != std::string::npos, "unknown op");
  const auto st = field::OpStats(true);
  CHECK(st.count("invert") && st.at("invert").cpuCalls == 1 && st.at("invert").gpuCalls == 0, "CPU runs are counted");
  CHECK(field::OpStats(false).empty(), "OpStats(reset) clears the table");
  CHECK(gpu::CellsOf(args) == 64.0, "CellsOf from the input");
  OpArgs gen;
  gen.p.num["width"] = 100;
  gen.p.num["height"] = 50;
  CHECK(gpu::CellsOf(gen) == 5000.0, "CellsOf from width x height");
  const std::vector<std::string> names = gpu::GpuOpNames();
  CHECK(std::find(names.begin(), names.end(), "blur") != names.end() && std::find(names.begin(), names.end(), "box") != names.end(), "blur and box are registered");
}

// ---- the real backend (Windows with Direct3D 11) ------------------------------------------------------------------------------------------------

static void TestRealBackend()
{
  gpu::Backend& real = gpu::CurrentBackend();
  const gpu::BackendInfo info = real.Info(true);
  std::printf("real GPU backend: compiled in %s, tried %s, available %s, adapter '%s', %llu MB, feature level %s, compiler %s, textures %s%s%s\n", info.compiledIn ? "yes" : "no",
              info.tried ? "yes" : "no", info.available ? "yes" : "no", info.adapter.c_str(), static_cast<unsigned long long>(info.vramMB), info.featureLevel.c_str(),
              info.compiler.c_str(), info.textures ? "yes" : "no", info.lastError.empty() ? "" : ", last error: ", info.lastError.c_str());
  if (!info.available) {
    std::printf("  (no usable Direct3D 11 device here: the real self test is skipped)\n");
    return;
  }
  const gpu::SelfTestReport rep = gpu::RunSelfTest();
  for (const gpu::SelfTestPart& p : rep.parts) std::printf("  %-50s %s  %.2f ms  %s\n", p.name.c_str(), p.ok ? "ok  " : "FAIL", p.ms, p.detail.c_str());
  std::printf("  real self test: %s in %.1f ms%s%s\n", rep.ok ? "ok" : "FAILED", rep.ms, rep.error.empty() ? "" : ", ", rep.error.c_str());
  CHECK(rep.ok, "the self test on the real device passes (%s)", rep.error.c_str());
}

int main()
{
  TestShim();
  TestKernelTable();
  TestScaleOffset();
  TestReduceBarriers();
  TestBlurKernelsDirect();
  TestPolicyAndFallback();
  TestThreads();
  TestSelfTestFirst();
  TestSelfTestOnEmulator();
  TestIterations();
  TestFieldHelpers();
  TestCoreHooks();
  TestRealBackend();
  std::printf("EdGpuTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
