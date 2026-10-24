// Test of the GPU erosion ops (EdGpuErosion.cpp, gpu/erosion_*.hlsl; docs/projects/editor/GPU_COMPUTE.md, "Erosion kernels"). No GPU is needed: the kernels run as C++ on the CPU emulator.
//   g++ -std=c++20 -O2 -ffp-contract=off -I src/sdk src/sdk/moho/editor/tests/EdGpuErosionTest.cpp src/sdk/moho/editor/EdFieldCore.cpp src/sdk/moho/editor/EdErosion.cpp \
//       src/sdk/moho/editor/EdGpuOps.cpp src/sdk/moho/editor/EdGpu.cpp src/sdk/moho/editor/EdGpuErosion.cpp -pthread -o EdGpuErosionTest && ./EdGpuErosionTest [lab_dir]
//
// The GPU ops are NOT the CPU ops with other hardware: the hydraulic solver has eight pipes with friction, a depth dependent capacity and a spread ground update, the droplets add to the ground
// through integer atomics in no particular order. So nothing here compares sample values with the CPU op except where the maths is the same (the thermal slides, the no-change cases).
// What is tested are PROPERTIES: mass, bounds, finite numbers, erosion lowers peaks and fills basins, fine structure (no cell wide grooves, no grid direction) not worse than the CPU op,
// masks, small / non square fields, parameter extremes, the Unsupported / fall-back paths, no out of range access in the emulator, and (lab_dir given) the pictures.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "moho/editor/EdErosion.h"
#include "moho/editor/EdFieldCore.h"
#include "moho/editor/EdGpu.h"
#include "moho/editor/EdGpuOps.h"

#include "moho/editor/gpu/hlsl_cpp_shim.h"
#include "moho/editor/tests/EdGpuEmu.h"
#include "moho/editor/gpu/common.hlsli"
#include "moho/editor/gpu/blur_params.hlsli"

EMU_KERNEL_BEGIN(k_blur_pass)
#include "moho/editor/gpu/blur_pass.hlsl"
EMU_KERNEL_END(blur_pass)

#include "moho/editor/tests/EdGpuErosionKernels.inc"

using namespace moho::editor;
using field::Grid;
using field::GridPtr;
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

static gpu::EmuBackend gBackend;

// ---- helpers -----------------------------------------------------------------------------------------------------------------------------------

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

static void ParseParams(const std::string& text, field::Params& p)
{
  for (const std::string& kv : Split(text, ',')) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    char* end = nullptr;
    const double n = std::strtod(v.c_str(), &end);
    if (!v.empty() && *end == 0) p.num[k] = n;
    else p.str[k] = v;
  }
}

static double SumOf(const std::vector<float>& v)
{
  double s = 0.0;
  for (const float x : v) s += x;
  return s;
}

static bool AllFinite(const Grid& g)
{
  for (const float x : g.v)
    if (!std::isfinite(x)) return false;
  return true;
}

static double MaxDiff(const Grid& a, const Grid& b)
{
  if (!a.SameShape(b)) return 1e30;
  double worst = 0.0;
  for (std::size_t i = 0; i < a.v.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(a.v[i]) - b.v[i]));
  return worst;
}

static double MeanAbsChange(const Grid& before, const Grid& after, const int margin = 0)
{
  double s = 0.0;
  std::size_t n = 0;
  for (int z = margin; z < before.h - margin; ++z)
    for (int x = margin; x < before.w - margin; ++x) {
      s += std::fabs(static_cast<double>(after.At(x, z)) - before.At(x, z));
      ++n;
    }
  return n > 0 ? s / static_cast<double>(n) : 0.0;
}

static double StdDev(const Grid& g)
{
  const double mean = SumOf(g.v) / static_cast<double>(g.v.size());
  double s = 0.0;
  for (const float x : g.v) s += (x - mean) * (x - mean);
  return std::sqrt(s / static_cast<double>(g.v.size()));
}

// ---- terrains (cell 1 world unit) -------------------------------------------------------------------------------------------------------------

/** Five mounds on a plain with a little undulation, w x h samples. */
static GridPtr Mounds(const int w, const int h)
{
  auto g = std::make_shared<Grid>(w, h, 1.0f, 0.0f, 0.0f);
  struct M { double x, z, r, a; };
  static const M mounds[] = {{0.28, 0.34, 0.12, 14.0}, {0.60, 0.28, 0.095, 10.0}, {0.72, 0.62, 0.14, 16.0}, {0.35, 0.72, 0.105, 11.0}, {0.50, 0.50, 0.08, 8.0}};
  const double n = std::min(w, h);
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      double v = 0.0;
      for (const M& m : mounds) {
        const double dx = x - m.x * w, dz = z - m.z * h, r = m.r * n;
        v += m.a * std::exp(-(dx * dx + dz * dz) / (r * r));
      }
      v += 1.5 * std::sin(0.045 * x + 0.5) * std::cos(0.037 * z) + 0.8 * std::sin(0.11 * x + 0.17 * z);
      g->At(x, z) = static_cast<float>(v + 3.0);
    }
  }
  return g;
}

/** A rough hillside: broad ramp, a ridge and hash noise of three scales (steep and flat parts, pits). */
static GridPtr Rough(const int w, const int h, const std::uint32_t seed)
{
  auto g = std::make_shared<Grid>(w, h, 1.0f, 0.0f, 0.0f);
  const auto lattice = [&](const int ix, const int iz, const std::uint32_t o) {
    std::uint32_t k = static_cast<std::uint32_t>(ix) * 73856093u ^ static_cast<std::uint32_t>(iz) * 19349663u ^ (o + seed) * 83492791u;
    k ^= k >> 16;
    k *= 0x7feb352du;
    k ^= k >> 15;
    k *= 0x846ca68bu;
    k ^= k >> 16;
    return static_cast<double>(k >> 8) / 16777216.0;
  };
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      double v = 0.25 * x + 0.1 * z + 14.0 * std::exp(-std::pow((x - 0.55 * w) / (0.18 * w), 2.0));
      double amp = 9.0;
      for (std::uint32_t o = 0; o < 3; ++o, amp *= 0.45) {
        const double sc = 16.0 / static_cast<double>(1u << o);
        const double u = x / sc, t = z / sc;
        const int iu = static_cast<int>(std::floor(u)), it = static_cast<int>(std::floor(t));
        const double fu = u - iu, ft = t - it;
        const double su = fu * fu * (3 - 2 * fu), st = ft * ft * (3 - 2 * ft);
        const double a = lattice(iu, it, o) * (1 - su) + lattice(iu + 1, it, o) * su;
        const double b = lattice(iu, it + 1, o) * (1 - su) + lattice(iu + 1, it + 1, o) * su;
        v += amp * (a * (1 - st) + b * st);
      }
      g->At(x, z) = static_cast<float>(v);
    }
  }
  return g;
}

/** A tilted plane (slope `tilt` rise per unit, direction `angle` degrees) with a faint bump so water has something to do. */
static GridPtr Tilted(const int n, const double angle, const double tilt)
{
  auto g = std::make_shared<Grid>(n, n, 1.0f, 0.0f, 0.0f);
  const double c = std::cos(angle * 3.14159265358979 / 180.0), s = std::sin(angle * 3.14159265358979 / 180.0);
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) {
      const double u = (x - 0.5 * n) * c + (z - 0.5 * n) * s, v = -(x - 0.5 * n) * s + (z - 0.5 * n) * c;
      g->At(x, z) = static_cast<float>(40.0 - tilt * u + 0.6 * std::sin(0.31 * v) * std::sin(0.23 * u + 1.0) + 0.4 * std::sin(0.8 * u + 0.5 * v));
    }
  return g;
}

// ---- running both ----------------------------------------------------------------------------------------------------------------------------

struct Pair
{
  Grid cpu, gpu;
  std::map<std::string, Grid> extraCpu, extraGpu;
  bool cpuOk = false, gpuOk = false, usedGpu = false;
  std::string cpuErr, gpuErr;
};

static OpArgs ArgsOf(const std::string& params, const GridPtr& in, const GridPtr& mask)
{
  OpArgs a;
  ParseParams(params, a.p);
  a.in["a"] = in;
  if (mask) a.in["mask"] = mask;
  return a;
}

static Pair Both(const std::string& op, const std::string& params, const GridPtr& in, const GridPtr& mask = nullptr, const bool extras = false)
{
  Pair r;
  {
    OpArgs a = ArgsOf(params, in, mask);
    if (extras) a.extra = &r.extraCpu;
    r.cpuOk = field::RunOpCpu(op, a, r.cpu, r.cpuErr);
  }
  {
    OpArgs a = ArgsOf(params, in, mask);
    if (extras) a.extra = &r.extraGpu;
    (void)field::OpStats(true);
    emu::gOob = 0;
    r.gpuOk = field::RunOp(op, a, r.gpu, r.gpuErr);
    const auto st = field::OpStats(true);
    const auto it = st.find(op);
    r.usedGpu = it != st.end() && it->second.gpuCalls > 0;
  }
  return r;
}

// ---- fine structure (the lab's measures: cell wide grooves, grid directions) ----------------------------------------------------------------

static std::vector<float> BlurField(std::vector<float> v, const int w, const int h, const int passes)
{
  std::vector<float> tmp(v.size());
  for (int p = 0; p < passes; ++p) {
    for (int z = 0; z < h; ++z)
      for (int x = 0; x < w; ++x) tmp[z * w + x] = 0.25f * v[z * w + std::max(0, x - 1)] + 0.5f * v[z * w + x] + 0.25f * v[z * w + std::min(w - 1, x + 1)];
    for (int z = 0; z < h; ++z)
      for (int x = 0; x < w; ++x) v[z * w + x] = 0.25f * tmp[std::max(0, z - 1) * w + x] + 0.5f * tmp[z * w + x] + 0.25f * tmp[std::min(h - 1, z + 1) * w + x];
  }
  return v;
}

struct VeinStats
{
  double lap1 = 0.0;     // mean |Laplacian at 1 cell| of the change over the mean |change|: a groove of 1 cell width is all Laplacian
  double cover = 0.0;    // (sum |change|)^2 / (n sum change^2): 1 = the change is the same everywhere, small = it sits in a few lines
  double axes = 0.0, diagonals = 0.0;     // orientation of the fine change: power in the 10 degrees around the grid axes / diagonals over the mean (1 = none preferred)
};

static VeinStats MeasureVeins(const Grid& before, const Grid& after)
{
  const int w = before.w, h = before.h, m = 8;
  std::vector<float> d(before.v.size());
  for (std::size_t i = 0; i < d.size(); ++i) d[i] = before.v[i] - after.v[i];
  VeinStats r;
  double sumAbs = 0.0, sumLap = 0.0, sumSq = 0.0;
  std::size_t nl = 0;
  for (int z = m; z < h - m; ++z)
    for (int x = m; x < w - m; ++x) {
      const std::size_t i = static_cast<std::size_t>(z) * w + x;
      sumAbs += std::fabs(d[i]);
      sumSq += static_cast<double>(d[i]) * d[i];
      sumLap += std::fabs(4.0 * (after.v[i] - before.v[i]) - (after.v[i - 1] - before.v[i - 1]) - (after.v[i + 1] - before.v[i + 1]) - (after.v[i - w] - before.v[i - w]) - (after.v[i + w] - before.v[i + w]));
      ++nl;
    }
  r.lap1 = (sumLap / std::max<std::size_t>(1, nl)) / std::max(1e-9, sumAbs / std::max<std::size_t>(1, nl));
  r.cover = (sumAbs * sumAbs) / std::max(1e-9, static_cast<double>(nl) * sumSq);
  std::vector<float> hp(d.size());
  const std::vector<float> b3 = BlurField(d, w, h, 18);
  for (std::size_t i = 0; i < d.size(); ++i) hp[i] = d[i] - b3[i];
  double hist[36] = {};
  double total = 0.0;
  for (int z = m + 1; z < h - m - 1; ++z)
    for (int x = m + 1; x < w - m - 1; ++x) {
      const std::size_t i = static_cast<std::size_t>(z) * w + x;
      const double gx = 0.5 * (hp[i + 1] - hp[i - 1]), gz = 0.5 * (hp[i + w] - hp[i - w]);
      const double p = gx * gx + gz * gz;
      double a = std::atan2(gz, gx) * 180.0 / 3.14159265358979323846;
      if (a < 0.0) a += 180.0;
      if (a >= 180.0) a -= 180.0;
      hist[std::min(35, static_cast<int>(a / 5.0))] += p;
      total += p;
    }
  if (total > 0.0) {
    for (double& x : hist) x = x / total * 36.0;
    r.axes = 0.25 * (hist[0] + hist[17] + hist[18] + hist[35]);
    r.diagonals = 0.25 * (hist[8] + hist[9] + hist[26] + hist[27]);
  }
  return r;
}

/** Mean change over the highest / lowest `share` of the input's samples. */
static void PeakBasinChange(const Grid& before, const Grid& after, const double share, double& peaks, double& basins)
{
  std::vector<std::size_t> idx(before.v.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&](const std::size_t a, const std::size_t b) { return before.v[a] < before.v[b]; });
  const std::size_t k = std::max<std::size_t>(1, static_cast<std::size_t>(share * static_cast<double>(idx.size())));
  peaks = basins = 0.0;
  for (std::size_t i = 0; i < k; ++i) {
    basins += after.v[idx[i]] - before.v[idx[i]];
    peaks += after.v[idx[idx.size() - 1 - i]] - before.v[idx[idx.size() - 1 - i]];
  }
  peaks /= static_cast<double>(k);
  basins /= static_cast<double>(k);
}

// ---- tests -----------------------------------------------------------------------------------------------------------------------------------

static void TestRegistration()
{
  const auto names = gpu::GpuOpNames();
  const auto has = [&](const char* n) { return std::find(names.begin(), names.end(), std::string(n)) != names.end(); };
  CHECK(has("erode_hydraulic") && has("erode_droplet"), "both erosion ops are registered");
  CHECK(gpu::GpuOpMinCells("erode_hydraulic") == 4096.0f && gpu::GpuOpMinCells("erode_droplet") == 4096.0f, "the erosion ops have their own minimum field size (erosion pays off on small fields)");
  CHECK(gpu::GpuOpMinCells("blur") < 0.0f, "an op without an own minimum has none");
  for (const char* k : {"erosion_hydro_flux", "erosion_hydro_water", "erosion_hydro_apply", "erosion_hydro_transport", "erosion_hydro_settle", "erosion_thermal_a", "erosion_thermal_b", "erosion_drop", "erosion_drop_resolve",
                        "erosion_steer", "erosion_delta", "erosion_blend", "erosion_copy"})
    CHECK(gpu::FindKernel(k) != nullptr, "kernel %s is generated", k);

  // the per op threshold: the default policy (65536) would keep 70 x 70 on the CPU, the erosion ops take it from 4096 samples
  const GridPtr big = Mounds(70, 70), tiny = Mounds(60, 60);
  gpu::GetPolicy().minCells = 65536.0f;
  Pair a = Both("erode_hydraulic", "iterations=4", big);
  Pair b = Both("erode_hydraulic", "iterations=4", tiny);
  CHECK(a.cpuOk && a.gpuOk && a.usedGpu, "a 70 x 70 field runs on the GPU although ed_gpu_min_cells is 65536");
  CHECK(b.cpuOk && b.gpuOk && !b.usedGpu, "a 60 x 60 field (3600 samples) stays on the CPU");
  gpu::GetPolicy().minCells = 0.0f;
  Pair c = Both("erode_hydraulic", "iterations=4", tiny);
  CHECK(c.usedGpu, "ed_gpu_min_cells 0 sends everything to the GPU");
}

/** The thermal slides are the CPU's own maths: with no water the hydraulic op is only thermal, and the two must agree. */
static void TestThermalParity()
{
  const GridPtr t = Mounds(75, 49);     // not a multiple of the 16 x 16 thread group, not square
  for (const char* edge : {"pinned", "open", "closed"}) {
    for (const char* extra : {"", ",protect=0.8"}) {
      const std::string params = std::string("iterations=12,rain=0,thermal=0.5,thermal_every=1,talus=0.3,smooth=0,edge=") + edge + extra;
      Pair p = Both("erode_hydraulic", params, t);
      CHECK(p.cpuOk && p.gpuOk && p.usedGpu, "thermal only (%s%s): both ran, the GPU one on the GPU", edge, extra);
      const double d = MaxDiff(p.cpu, p.gpu);
      CHECK(d < 2e-4, "thermal slides on the GPU equal the CPU's (%s%s): largest difference %.2e", edge, extra, d);
      CHECK(emu::gOob == 0, "thermal only: %d out of range accesses", emu::gOob);
      CHECK(MeanAbsChange(*t, p.gpu) > 0.01, "the thermal test moved something (%.4f)", MeanAbsChange(*t, p.gpu));
    }
  }
  // no water and no slides: nothing changes
  Pair n = Both("erode_hydraulic", "iterations=10,rain=0,thermal=0", t);
  CHECK(n.usedGpu && MaxDiff(n.gpu, *t) < 1e-6, "no rain, no slides: the field is unchanged (%.2e)", MaxDiff(n.gpu, *t));
}

static void TestHydraulicProperties()
{
  const GridPtr t = Mounds(96, 80);
  // mass: closed edges, no filter, all sediment settled at the end: what was dug is somewhere else
  {
    Pair p = Both("erode_hydraulic", "iterations=150,smooth=0,edge=closed,rain=0.006", t, nullptr, true);
    CHECK(p.gpuOk && p.usedGpu, "hydraulic closed edge ran on the GPU");
    CHECK(AllFinite(p.gpu), "no NaN / inf");
    const double moved = MeanAbsChange(*t, p.gpu) * t->v.size();
    const double net = SumOf(p.gpu.v) - SumOf(t->v);
    CHECK(moved > 50.0, "the run did something (total movement %.1f)", moved);
    CHECK(std::fabs(net) < 2e-3 * moved + 1e-2, "closed edges conserve the ground: net change %.4f of %.1f moved", net, moved);
    CHECK(emu::gOob == 0, "%d out of range accesses", emu::gOob);
    // the masks of the run: wear and deposit add up to the change
    const auto w = p.extraGpu.find("wear"), d = p.extraGpu.find("deposit");
    CHECK(w != p.extraGpu.end() && d != p.extraGpu.end() && AllFinite(w->second) && AllFinite(d->second), "the editor's extra outputs (wear, deposit) are there and finite");
    if (w != p.extraGpu.end() && d != p.extraGpu.end()) {
      double minV = 1e30;
      for (const float x : w->second.v) minV = std::min<double>(minV, x);
      for (const float x : d->second.v) minV = std::min<double>(minV, x);
      CHECK(minV >= -1e-6, "wear and deposit are never negative (%.2e)", minV);
      CHECK(std::fabs((SumOf(d->second.v) - SumOf(w->second.v)) - net) < 2e-3 * moved + 1e-2, "deposit - wear = net change");
    }
    CHECK(p.extraGpu.count("accum") && p.extraGpu.count("flow") && p.extraGpu.count("water") && p.extraGpu.count("sediment"), "all extra outputs of the CPU op exist");
  }
  // erosion lowers the peaks and raises the basins
  {
    Pair p = Both("erode_hydraulic", "iterations=300,smooth=1,edge=open", t);
    double peaks = 0, basins = 0, cpuPeaks = 0, cpuBasins = 0;
    PeakBasinChange(*t, p.gpu, 0.05, peaks, basins);
    PeakBasinChange(*t, p.cpu, 0.05, cpuPeaks, cpuBasins);
    std::printf("hydraulic 300: top 5%% change %.3f (cpu %.3f), bottom 5%% change %.3f (cpu %.3f)\n", peaks, cpuPeaks, basins, cpuBasins);
    CHECK(peaks < -0.05, "peaks are lowered (%.3f)", peaks);
    CHECK(basins - peaks > 0.3, "the low ground gains against the peaks (%.3f vs %.3f)", basins, peaks);
    const double rg = StdDev(p.gpu) / StdDev(*t), rc = StdDev(p.cpu) / StdDev(*t);
    std::printf("relief (std) after / before: gpu %.3f cpu %.3f\n", rg, rc);
    CHECK(rg > 0.75 && rg < 1.05, "the relief is neither melted nor inflated (std ratio %.3f)", rg);
    CHECK(rg > 0.85 * rc, "the GPU keeps at least as much relief as the CPU op (%.3f vs %.3f)", rg, rc);
  }
  // bounds
  {
    Pair p = Both("erode_hydraulic", "iterations=200,max_change=1.5,keep_range=1", t);
    double lo = 1e30, hi = -1e30;
    for (const float x : t->v) {
      lo = std::min<double>(lo, x);
      hi = std::max<double>(hi, x);
    }
    double worst = 0.0, outLo = 1e30, outHi = -1e30;
    for (std::size_t i = 0; i < t->v.size(); ++i) {
      worst = std::max(worst, std::fabs(static_cast<double>(p.gpu.v[i]) - t->v[i]));
      outLo = std::min<double>(outLo, p.gpu.v[i]);
      outHi = std::max<double>(outHi, p.gpu.v[i]);
    }
    CHECK(worst <= 1.5 + 1e-4, "max_change 1.5 holds (largest change %.4f)", worst);
    CHECK(outLo >= lo - 1.5 - 1e-3 && outHi <= hi + 1.5 + 1e-3, "the result stays within the input range +- max_change");
  }
  // open edge: the map edge is no wall of untouched ground (the CPU op's rule): the border takes the change of its neighbour
  {
    Pair p = Both("erode_hydraulic", "iterations=120,edge=open", t);
    double step = 0.0;
    for (int x = 1; x < t->w - 1; ++x) step += std::fabs((p.gpu.At(x, 0) - t->At(x, 0)) - (p.gpu.At(x, 1) - t->At(x, 1)));
    CHECK(step / (t->w - 2) < 1e-3, "the open edge row takes the change of its neighbour (%.5f)", step / (t->w - 2));
  }
  // pinned: the outer ring keeps its height
  {
    Pair p = Both("erode_hydraulic", "iterations=80,edge=pinned", t);
    double worst = 0.0;
    for (int x = 0; x < t->w; ++x) worst = std::max({worst, std::fabs(static_cast<double>(p.gpu.At(x, 0) - t->At(x, 0))), std::fabs(static_cast<double>(p.gpu.At(x, t->h - 1) - t->At(x, t->h - 1)))});
    for (int z = 0; z < t->h; ++z) worst = std::max({worst, std::fabs(static_cast<double>(p.gpu.At(0, z) - t->At(0, z))), std::fabs(static_cast<double>(p.gpu.At(t->w - 1, z) - t->At(t->w - 1, z)))});
    CHECK(worst < 1e-5, "a pinned ring keeps its height (%.2e)", worst);
  }
  // conserve: the volume the run lost over an open edge is put back
  {
    Pair p = Both("erode_hydraulic", "iterations=150,edge=open,smooth=0,conserve=1", t);
    const double moved = MeanAbsChange(*t, p.gpu) * t->v.size();
    CHECK(std::fabs(SumOf(p.gpu.v) - SumOf(t->v)) < 1e-3 * moved + 1e-2, "conserve 1 keeps the volume (%.4f of %.1f)", SumOf(p.gpu.v) - SumOf(t->v), moved);
  }
  // masks: erodibility 0 everywhere = nothing happens; half the map protected = that half changes much less
  {
    auto zero = std::make_shared<Grid>(t->w, t->h, 1.0f, 0.0f, 0.0f);
    Pair p = Both("erode_hydraulic", "iterations=60", t, zero);
    CHECK(p.usedGpu && MaxDiff(p.gpu, *t) < 1e-5, "a mask of zeros protects everything (%.2e)", MaxDiff(p.gpu, *t));
    auto half = std::make_shared<Grid>(t->w, t->h, 1.0f, 0.0f, 0.0f);
    for (int z = 0; z < t->h; ++z)
      for (int x = 0; x < t->w; ++x) half->At(x, z) = x < t->w / 2 ? 0.0f : 1.0f;
    Pair q = Both("erode_hydraulic", "iterations=150,vary=0,protect=0,rain_vary=0", t, half);
    double l = 0.0, r = 0.0;
    for (int z = 4; z < t->h - 4; ++z)
      for (int x = 4; x < t->w - 4; ++x) (x < t->w / 2 - 4 ? l : r) += std::fabs(q.gpu.At(x, z) - t->At(x, z));
    std::printf("mask halves: |change| protected %.2f, free %.2f\n", l, r);
    CHECK(l < 0.3 * r, "the protected half changes much less than the free half (%.2f vs %.2f)", l, r);
  }
  // vary / rain_vary / protect factors are the CPU's (shared code) and work on the GPU path too
  {
    Pair p = Both("erode_hydraulic", "iterations=80,vary=0.6,rain_vary=0.7,protect=0.7,keep_range=1,seed=5", t);
    CHECK(p.usedGpu && AllFinite(p.gpu), "vary / rain_vary / protect run");
    CHECK(MeanAbsChange(*t, p.gpu) > 0.01, "and still erode");
  }
}

/** A tilted plane in four directions: the result must not prefer the grid (eight pipes). */
static void TestHydraulicIsotropy()
{
  double change[4], cpuChange[4];
  int k = 0;
  for (const double angle : {0.0, 45.0, 22.5, 67.5}) {
    const GridPtr t = Tilted(144, angle, 0.45);
    Pair p = Both("erode_hydraulic", "iterations=150,edge=open,vary=0,rain_vary=0,protect=0,smooth=0", t);
    change[k] = MeanAbsChange(*t, p.gpu, 40);
    cpuChange[k] = MeanAbsChange(*t, p.cpu, 40);
    ++k;
  }
  std::printf("isotropy: mean |change| for tilts 0 / 45 / 22.5 / 67.5 degrees: gpu %.4f %.4f %.4f %.4f, cpu %.4f %.4f %.4f %.4f\n", change[0], change[1], change[2], change[3], cpuChange[0], cpuChange[1], cpuChange[2], cpuChange[3]);
  const double ratio = change[1] / std::max(1e-9, change[0]);
  CHECK(ratio > 0.8 && ratio < 1.25, "flow along the diagonal erodes like flow along an axis (ratio %.2f)", ratio);
  const double lo = *std::min_element(change, change + 4), hi = *std::max_element(change, change + 4);
  CHECK(hi < 1.25 * lo + 1e-4, "no direction of the tilt is preferred (%.4f .. %.4f)", lo, hi);
}

static void TestHydraulicStructure()
{
  const GridPtr t = Mounds(128, 128);
  Pair p = Both("erode_hydraulic", "iterations=250", t);
  const VeinStats g = MeasureVeins(*t, p.gpu), c = MeasureVeins(*t, p.cpu);
  std::printf("hydraulic structure: 1 cell Laplacian share gpu %.3f cpu %.3f; cover gpu %.2f cpu %.2f; axes gpu %.2f cpu %.2f; diagonals gpu %.2f cpu %.2f\n", g.lap1, c.lap1, g.cover, c.cover, g.axes, c.axes, g.diagonals, c.diagonals);
  CHECK(g.lap1 < 1.25 * c.lap1 + 0.02, "no more cell wide grooves than the CPU op (%.3f vs %.3f)", g.lap1, c.lap1);
  CHECK(g.axes > 0.5 && g.axes < 1.7 && g.diagonals > 0.5 && g.diagonals < 1.7, "the fine change prefers no grid direction (axes %.2f, diagonals %.2f)", g.axes, g.diagonals);
  // the spread ground update is what keeps the grooves wide
  Pair q = Both("erode_hydraulic", "iterations=250,erode_spread=0", t);
  const VeinStats s0 = MeasureVeins(*t, q.gpu);
  std::printf("hydraulic structure without the spread: 1 cell Laplacian share %.3f\n", s0.lap1);
  CHECK(g.lap1 < s0.lap1, "erode_spread widens the grooves (%.3f < %.3f)", g.lap1, s0.lap1);
}

static void TestDropletProperties()
{
  const GridPtr t = Mounds(110, 90);
  // mass: what the droplets dug and dropped is the change (no filters here)
  {
    Pair p = Both("erode_droplet", "density=1.5,smooth=0,smooth_passes=0,creep=0,rounds=8,seed=3", t, nullptr, true);
    CHECK(p.gpuOk && p.usedGpu, "droplets ran on the GPU");
    CHECK(AllFinite(p.gpu), "no NaN / inf");
    CHECK(emu::gOob == 0, "%d out of range accesses", emu::gOob);
    const double net = SumOf(p.gpu.v) - SumOf(t->v);
    const double moved = MeanAbsChange(*t, p.gpu) * t->v.size();
    CHECK(moved > 20.0, "the droplets did something (%.1f)", moved);
    const auto w = p.extraGpu.find("wear"), d = p.extraGpu.find("deposit");
    CHECK(w != p.extraGpu.end() && d != p.extraGpu.end(), "wear and deposit are there");
    if (w != p.extraGpu.end() && d != p.extraGpu.end()) {
      CHECK(std::fabs((SumOf(d->second.v) - SumOf(w->second.v)) - net) < 1e-3 * moved + 1e-2, "deposit - wear = net change (%.4f vs %.4f)", SumOf(d->second.v) - SumOf(w->second.v), net);
      // droplets that run off the map take their load: the ground can only lose, never gain more than it dug
      CHECK(net < 0.2 * moved, "the ground does not gain out of nothing (net %.2f of %.1f)", net, moved);
    }
    // the same run on the CPU digs about as much: the change is the same size
    const double cm = MeanAbsChange(*t, p.cpu) * t->v.size();
    std::printf("droplets: total movement gpu %.1f, cpu %.1f; net gpu %.2f, cpu %.2f\n", moved, cm, net, SumOf(p.cpu.v) - SumOf(t->v));
    CHECK(moved > 0.6 * cm && moved < 1.6 * cm, "as much material moves as on the CPU (%.1f vs %.1f)", moved, cm);
  }
  // peaks and basins, relief, grooves
  {
    Pair p = Both("erode_droplet", "density=2,rounds=16", t);
    double peaks = 0, basins = 0, cpuPeaks = 0, cpuBasins = 0;
    PeakBasinChange(*t, p.gpu, 0.05, peaks, basins);
    PeakBasinChange(*t, p.cpu, 0.05, cpuPeaks, cpuBasins);
    std::printf("droplets: top 5%% change %.3f (cpu %.3f), bottom 5%% change %.3f (cpu %.3f)\n", peaks, cpuPeaks, basins, cpuBasins);
    CHECK(peaks < -0.03, "peaks are lowered (%.3f)", peaks);
    CHECK(basins - peaks > 0.2, "the low ground gains against the peaks (%.3f vs %.3f)", basins, peaks);
    const double rg = StdDev(p.gpu) / StdDev(*t), rc = StdDev(p.cpu) / StdDev(*t);
    CHECK(rg > 0.8 && rg < 1.05 && rg > 0.9 * rc, "the relief stays (std ratio %.3f, cpu %.3f)", rg, rc);
    const VeinStats g = MeasureVeins(*t, p.gpu), c = MeasureVeins(*t, p.cpu);
    std::printf("droplets structure: 1 cell Laplacian share gpu %.3f cpu %.3f; cover gpu %.2f cpu %.2f; axes %.2f diagonals %.2f\n", g.lap1, c.lap1, g.cover, c.cover, g.axes, g.diagonals);
    CHECK(g.lap1 < 1.3 * c.lap1 + 0.01, "grooves are not thinner than the CPU's (%.3f vs %.3f)", g.lap1, c.lap1);
    CHECK(g.axes > 0.5 && g.axes < 1.7 && g.diagonals > 0.5 && g.diagonals < 1.7, "no grid direction (axes %.2f, diagonals %.2f)", g.axes, g.diagonals);
    CHECK(g.cover > 0.7 * c.cover, "the change is spread as widely as on the CPU (%.2f vs %.2f)", g.cover, c.cover);
  }
  // creep widens the grooves (as on the CPU)
  {
    Pair a = Both("erode_droplet", "density=2,rounds=16,creep=0", t);
    Pair b = Both("erode_droplet", "density=2,rounds=16,creep=2", t);
    const VeinStats s0 = MeasureVeins(*t, a.gpu), s2 = MeasureVeins(*t, b.gpu);
    std::printf("creep: 1 cell Laplacian share creep 0 %.3f, creep 2 %.3f\n", s0.lap1, s2.lap1);
    CHECK(s2.lap1 < 0.8 * s0.lap1, "creep leaves wider grooves (%.3f vs %.3f)", s2.lap1, s0.lap1);
  }
  // bounds
  {
    Pair p = Both("erode_droplet", "density=2,rounds=8,max_change=1.0,keep_range=1", t);
    double worst = 0.0;
    for (std::size_t i = 0; i < t->v.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(p.gpu.v[i]) - t->v[i]));
    CHECK(worst <= 1.0 + 1e-4, "max_change 1 holds (%.4f)", worst);
  }
  // masks
  {
    auto zero = std::make_shared<Grid>(t->w, t->h, 1.0f, 0.0f, 0.0f);
    Pair p = Both("erode_droplet", "density=1,rounds=4,creep=0,smooth=0", t, zero);
    CHECK(p.usedGpu && MaxDiff(p.gpu, *t) < 1e-3, "a mask of zeros: droplets dig nothing (largest change %.4f)", MaxDiff(p.gpu, *t));
    auto half = std::make_shared<Grid>(t->w, t->h, 1.0f, 0.0f, 0.0f);
    for (int z = 0; z < t->h; ++z)
      for (int x = 0; x < t->w; ++x) half->At(x, z) = x < t->w / 2 ? 0.0f : 1.0f;
    Pair q = Both("erode_droplet", "density=2,rounds=8,vary=0,protect=0,rain_vary=0", t, half);
    double l = 0.0, r = 0.0;
    for (int z = 4; z < t->h - 4; ++z)
      for (int x = 4; x < t->w - 4; ++x) (x < t->w / 2 - 6 ? l : r) += std::fabs(q.gpu.At(x, z) - t->At(x, z));
    std::printf("droplet mask halves: |change| protected %.2f, free %.2f\n", l, r);
    CHECK(l < 0.45 * r, "the protected half changes much less (%.2f vs %.2f)", l, r);
    Pair v = Both("erode_droplet", "density=1,rounds=4,vary=0.6,rain_vary=0.8,protect=0.7,drop_vary=0.8,seed=9", t);
    CHECK(v.usedGpu && AllFinite(v.gpu) && MeanAbsChange(*t, v.gpu) > 0.005, "vary / rain_vary / protect / drop_vary run");
  }
  // thermal slides after the droplets (GPU kernels)
  {
    Pair a = Both("erode_droplet", "density=1,rounds=4,thermal=0,thermal_steps=0", t);
    Pair b = Both("erode_droplet", "density=1,rounds=4,thermal=0.5,thermal_steps=6,talus=0.2", t);
    CHECK(b.usedGpu && AllFinite(b.gpu), "thermal steps run");
    CHECK(MaxDiff(a.gpu, b.gpu) > 0.05, "thermal steps change the result");
  }
}

static void TestEdgesAndSmallFields()
{
  for (const auto& wh : std::vector<std::pair<int, int>>{{4, 4}, {5, 40}, {40, 5}, {17, 33}, {20, 13}, {64, 3}, {1, 50}}) {
    const GridPtr t = Rough(wh.first, wh.second, 4);
    for (const char* op : {"erode_hydraulic", "erode_droplet"}) {
      const std::string params = std::string(op) == "erode_hydraulic" ? "iterations=30,thermal=0.3" : "density=2,rounds=4,lifetime=20,thermal=0.3,thermal_steps=2";
      Pair p = Both(op, params, t);
      CHECK(p.gpuOk == p.cpuOk, "%s %d x %d: GPU and CPU agree whether the op works", op, wh.first, wh.second);
      if (p.gpuOk) {
        CHECK(p.gpu.w == t->w && p.gpu.h == t->h && AllFinite(p.gpu), "%s %d x %d: right size, finite", op, wh.first, wh.second);
        if (p.usedGpu) {
          CHECK(emu::gOob == 0, "%s %d x %d: %d out of range accesses", op, wh.first, wh.second, emu::gOob);
          double lo = 1e30, hi = -1e30;
          for (const float x : t->v) {
            lo = std::min<double>(lo, x);
            hi = std::max<double>(hi, x);
          }
          const double span = hi - lo;
          bool inside = true;
          for (const float x : p.gpu.v) inside = inside && x > lo - span && x < hi + span;
          CHECK(inside, "%s %d x %d: stays within the input range +- its span", op, wh.first, wh.second);
        }
      }
    }
  }
}

static void TestParameterExtremes()
{
  const GridPtr t = Rough(60, 52, 7);
  const char* hydro[] = {
      "iterations=1",
      "iterations=40,rain=0.05,evaporation=0.15,gain=0.25,capacity=4,erosion=0.6,deposition=0.6,depth_max=0.01,min_slope=0.2,cut_limit=0",
      "iterations=40,rain=0.0005,evaporation=0.002,gain=0.01,capacity=0.05,erosion=0.01,deposition=0.01,depth_max=0.5,min_slope=0,cut_limit=1,max_speed=0.1",
      "iterations=30,thermal=1,thermal_every=1,talus=0.1,settle=0",
      "iterations=30,thermal=1,thermal_every=10,talus=2,edge=closed,masks=0",
      "iterations=30,max_speed=5,cut_limit=0.05,slope_cap=0.05,speed_pow=3,depth_gain=16,shear_floor=1,erode_spread=1,rain_noise=1,flux_damp=1",
      "iterations=30,flux_damp=0,speed_pow=0.25,erode_spread=0,depth_gain=1,rain_noise=0",
      "iterations=30,output=sediment", "iterations=30,output=water", "iterations=30,output=flow", "iterations=30,output=wear", "iterations=30,output=deposit",
      "iterations=40,vary=1,rain_vary=1,protect=1,keep_range=1,max_change=0.5,conserve=1,smooth=0.5,smooth_passes=3",
      "iterations=20,masks=0",
  };
  for (const char* params : hydro) {
    Pair p = Both("erode_hydraulic", params, t);
    CHECK(p.cpuOk && p.gpuOk && p.usedGpu, "hydraulic [%s]: ran on the GPU", params);
    CHECK(AllFinite(p.gpu), "hydraulic [%s]: finite", params);
    CHECK(emu::gOob == 0, "hydraulic [%s]: %d out of range accesses", params, emu::gOob);
  }
  const char* drop[] = {
      "density=0.05,rounds=1,lifetime=1,radius=1",
      "density=3,rounds=64,lifetime=200,radius=8,inertia=1,capacity=16,erode=1,deposit=1,gravity=16,drain_pits=0",
      "density=1,rounds=2,lifetime=5,radius=12,accel=0,drop_vary=1,creep=8,speed=0,water=0",
      "density=1,rounds=3,evaporate=0.1,min_slope=0.2,round_cut=0.01,drain_pits=1,steer_every=1",
      "density=1,rounds=3,smooth=1,smooth_passes=6,thermal=1,thermal_steps=20,talus=0.1",
      "density=1,rounds=3,masks=0", "density=1,rounds=3,output=wear", "density=1,rounds=3,output=deposit", "density=1,rounds=3,output=flow",
      "density=1,rounds=5,vary=1,rain_vary=1,protect=1,keep_range=1,max_change=0.3,conserve=1,drop_vary=1",
  };
  for (const char* params : drop) {
    Pair p = Both("erode_droplet", params, t);
    CHECK(p.cpuOk && p.gpuOk && p.usedGpu, "droplet [%s]: ran on the GPU", params);
    CHECK(AllFinite(p.gpu), "droplet [%s]: finite", params);
    CHECK(emu::gOob == 0, "droplet [%s]: %d out of range accesses", params, emu::gOob);
  }
}

/** A field of 1.1 million samples: several droplet batches per round (the dispatch limit), a grid of more than 65535 thread groups in x for a 1D style index would fail, here every kernel is 2D. */
static void TestLargeField()
{
  const GridPtr t = Rough(1100, 1000, 11);
  Pair h = Both("erode_hydraulic", "iterations=2,masks=0", t);
  CHECK(h.cpuOk && h.gpuOk && h.usedGpu && AllFinite(h.gpu), "a 1100 x 1000 field: hydraulic on the GPU");
  CHECK(emu::gOob == 0, "large hydraulic: %d out of range accesses", emu::gOob);
  Pair d = Both("erode_droplet", "density=2,rounds=2,lifetime=6,radius=1,creep=0,smooth_passes=0,drain_pits=0,masks=0", t);
  CHECK(d.cpuOk && d.gpuOk && d.usedGpu && AllFinite(d.gpu), "a 1100 x 1000 field: droplets on the GPU (several batches per round)");
  CHECK(emu::gOob == 0, "large droplets: %d out of range accesses", emu::gOob);
  CHECK(MeanAbsChange(*t, d.gpu) > 0.0005, "and they dig (%.5f)", MeanAbsChange(*t, d.gpu));
}

static void TestUnsupportedAndFallback()
{
  const GridPtr t = Mounds(70, 70);
  for (const char* op : {"erode_hydraulic", "erode_droplet"}) {
    // cpu_only: the node's backend choice
    Pair p = Both(op, "cpu_only=1,iterations=5,density=1,rounds=2", t);
    CHECK(p.cpuOk && p.gpuOk && !p.usedGpu, "%s: cpu_only=1 keeps the op on the CPU", op);
    CHECK(MaxDiff(p.cpu, p.gpu) == 0.0, "%s: and gives the CPU's own result", op);
    // a mask of the wrong size: the CPU op reports it, the GPU op steps aside
    auto wrong = std::make_shared<Grid>(30, 30, 1.0f, 0.0f, 0.0f);
    Pair m = Both(op, "iterations=5,density=1,rounds=2", t, wrong);
    CHECK(!m.cpuOk && !m.gpuOk && !m.usedGpu && !m.gpuErr.empty(), "%s: a mask of the wrong size is the CPU op's error (%s)", op, m.gpuErr.c_str());
    // an unknown output
    Pair o = Both(op, "iterations=5,density=1,rounds=2,output=nonsense", t);
    CHECK(!o.cpuOk && !o.gpuOk && !o.usedGpu, "%s: an unknown output is the CPU op's error", op);
    // nothing to do
    Pair z = Both(op, "iterations=0,density=0", t);
    CHECK(z.cpuOk == z.gpuOk && !z.usedGpu, "%s: no iterations / no droplets go to the CPU op", op);
    // a failing dispatch: the CPU result is delivered, the fall-back is counted
    for (const int at : {0, 3, 9}) {
      gBackend.exec.failAtDispatch = gBackend.exec.dispatches + at;
      OpArgs a = ArgsOf("iterations=6,density=1,rounds=2,seed=2", t, nullptr);
      Grid out;
      std::string err;
      (void)field::OpStats(true);
      const bool ok = field::RunOp(op, a, out, err);
      const auto st = field::OpStats(true);
      gBackend.exec.failAtDispatch = -1;
      CHECK(ok && st.count(op) && st.at(op).fallbacks == 1 && st.at(op).gpuCalls == 0, "%s: a failing dispatch (#%d) falls back to the CPU op", op, at);
      Grid cpu;
      OpArgs b = ArgsOf("iterations=6,density=1,rounds=2,seed=2", t, nullptr);
      CHECK(field::RunOpCpu(op, b, cpu, err) && MaxDiff(cpu, out) == 0.0, "%s: the fall-back result is the CPU op's", op);
    }
  }
  // a lost device switches the backend off, the op still gives a result
  gBackend.exec.lostAtDispatch = gBackend.exec.dispatches + 2;
  OpArgs a = ArgsOf("iterations=6", t, nullptr);
  Grid out;
  std::string err;
  CHECK(field::RunOp("erode_hydraulic", a, out, err) && AllFinite(out), "a lost device: the CPU op delivers");
  CHECK(gBackend.markFailedCalls >= 1, "and the backend is marked failed");
  gBackend.Reset();
  gBackend.exec.lostAtDispatch = -1;
  // not enough device memory: the op steps aside (no failure counted)
  gBackend.exec.maxBytes = 100 * 1024;
  Pair big = Both("erode_hydraulic", "iterations=3", t);
  CHECK(big.cpuOk && big.gpuOk && !big.usedGpu, "a field that does not fit the device memory budget runs on the CPU");
  Pair bigD = Both("erode_droplet", "density=1,rounds=2", t);
  CHECK(bigD.cpuOk && bigD.gpuOk && !bigD.usedGpu, "the same for droplets");
  gBackend.exec.maxBytes = 1ull << 30;
}

/** Quality pictures of the lab (an argument: the folder). */
static void WriteRaw(const std::string& path, const Grid& g)
{
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(g.v.data(), 4, g.v.size(), f);
    std::fclose(f);
  }
}

static void Lab(const std::string& dir)
{
  const GridPtr t = Mounds(256, 256);
  WriteRaw(dir + "/lab_input.raw", *t);
  for (const char* op : {"erode_hydraulic", "erode_droplet"}) {
    Pair p = Both(op, std::string(op) == "erode_hydraulic" ? "iterations=300" : "density=2,rounds=32", t);
    WriteRaw(dir + "/lab_" + op + "_cpu.raw", p.cpu);
    WriteRaw(dir + "/lab_" + op + "_gpu.raw", p.gpu);
  }
  std::printf("lab pictures (raw 256 x 256 floats) written to %s\n", dir.c_str());
}

int main(int argc, char** argv)
{
  gpu::SetBackendForTest(&gBackend);
  gpu::Install();
  gpu::GetPolicy().minCells = 0.0f;
  gpu::GetPolicy().selfTestFirst = false;
  gpu::GetPolicy().enabled = true;
  TestRegistration();
  TestThermalParity();
  TestHydraulicProperties();
  TestHydraulicIsotropy();
  TestHydraulicStructure();
  TestDropletProperties();
  TestEdgesAndSmallFields();
  TestParameterExtremes();
  TestLargeField();
  TestUnsupportedAndFallback();
  if (argc > 1) Lab(argv[1]);
  std::printf("EdGpuErosionTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
