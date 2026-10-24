// EDITOR GPU compute: GPU versions of heavy filter ops (occlusion, bilateral, diffuse, blur_dir, dwarp and the slope / curvature / aspect / normal_map / hillshade family). Each op reads its
// options exactly like the CPU op (EdFieldFilt.cpp, EdFieldCore.cpp, EdFieldGen.cpp) and answers Unsupported for what it does not handle, so the CPU op keeps working. A filter transfers its
// input and its result over the bus: ops whose arithmetic is a few operations per sample (the slope family) only go to the GPU on big fields (kMinCheap samples).
// docs/projects/editor/GPU_COMPUTE.md, "Generator and filter kernels".
#pragma fp_contract(off)

#include "moho/editor/EdGpuGen.h"

namespace moho::editor::gpu::hk
{
  using uint = std::uint32_t;
#include "moho/editor/gpu/filt_bilateral_params.hlsli"
#include "moho/editor/gpu/filt_blurdir_params.hlsli"
#include "moho/editor/gpu/filt_diffuse_params.hlsli"
#include "moho/editor/gpu/filt_dwarp_params.hlsli"
#include "moho/editor/gpu/filt_occlusion_params.hlsli"
} // namespace moho::editor::gpu::hk

namespace moho::editor::gpu
{
  using namespace gen;

  namespace
  {
    constexpr float kPi = 3.14159265358979f;

    /** Uploads the source `a`, makes the result field. The output grid takes its metadata from the op's shape (`like`, else `a`). */
    struct Job
    {
      const Grid* src = nullptr;
      Shape shape;
      Field in;
      Field res;
    };

    /** Reads `a` and the output shape; false = Unsupported (the CPU op reports bad input itself). */
    bool Begin(Executor& ex, const OpArgs& a, const int fields, Job& j)
    {
      j.src = In(a, "a");
      if (j.src == nullptr || j.src->w < 1 || j.src->h < 1 || !ShapeOf(a, j.shape)) return false;
      return Fits(ex, j.shape.Count(), fields);
    }

    /** `mask` must be absent or of the source's shape (else the CPU op reports it). */
    bool MaskOk(const OpArgs& a, const Job& j) { const Grid* m = In(a, "mask"); return m == nullptr || m->SameShape(*j.src); }

    bool Upload(Executor& ex, Job& j, std::string& err) { return UploadGrid(ex, *j.src, j.in, err) && MakeField(ex, j.shape.w, j.shape.h, j.res, err); }

    OpResult Finish(Executor& ex, Job& j, Grid& out, std::string& err)
    {
      Grid result = NewGrid(j.shape);
      if (!DownloadGrid(ex, j.res, result, err)) return OpResult::Failed;
      out = std::move(result);
      return OpResult::Done;
    }

    // ---- occlusion --------------------------------------------------------------------------------------------------------------------------------

    OpResult OpOcclusionGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      Job j;
      if (!Begin(ex, a, 2, j)) return OpResult::Unsupported;
      const std::string mode = a.p.S("mode", "cavity");
      if (mode != "cavity" && mode != "convex" && mode != "both") return OpResult::Unsupported;
      const Grid& src = *j.src;
      const int m = mode == "cavity" ? 0 : (mode == "convex" ? 1 : 2);
      const int dirs = std::max(4, std::min(32, static_cast<int>(a.p.N("directions", 12))));
      const float radius = std::max(static_cast<float>(src.cell), static_cast<float>(a.p.N("radius", 30.0)));
      const float gain = static_cast<float>(a.p.N("gain", 1.0));
      const int steps = std::max(2, std::min(32, static_cast<int>(std::lround(radius / src.cell))));
      std::vector<float> ray(static_cast<std::size_t>(steps + 2 * dirs));
      for (int s = 0; s < steps; ++s) {
        const float t = static_cast<float>(s + 1) / static_cast<float>(steps);
        ray[static_cast<std::size_t>(s)] = std::max(1.0f, (radius / src.cell) * t * t);
      }
      for (int d = 0; d < dirs; ++d) {
        const float th = 2.0f * kPi * static_cast<float>(d) / static_cast<float>(dirs);
        ray[static_cast<std::size_t>(steps + d)] = std::cos(th);
        ray[static_cast<std::size_t>(steps + dirs + d)] = std::sin(th);
      }
      const KernelInfo* k = FindKernel("filt_occlusion");
      if (k == nullptr) { err = "kernel filt_occlusion is missing"; return OpResult::Failed; }
      std::unique_ptr<Resource> rayBuf;
      if (!Upload(ex, j, err) || !UploadVec(ex, ray, rayBuf, err)) return OpResult::Failed;
      hk::OcclusionParams p{};
      p.w = static_cast<std::uint32_t>(src.w);
      p.h = static_cast<std::uint32_t>(src.h);
      p.dirs = static_cast<std::uint32_t>(dirs);
      p.steps = static_cast<std::uint32_t>(steps);
      p.mode = static_cast<std::uint32_t>(m);
      p.cell = src.cell;
      p.gain = gain;
      Bindings b;
      b.Srv(1, *rayBuf).Srv(2, j.in.R()).Uav(0, j.res.R());
      if (!DispatchBands(ex, *k, p.w, p.h, static_cast<double>(dirs) * static_cast<double>(steps) * 0.12, p, b, err)) return OpResult::Failed;
      return Finish(ex, j, out, err);
    }

    /** The optional `mask` input as a field (or a one element dummy: every slot a kernel declares must be bound). */
    struct MaskBind
    {
      Field field;
      std::unique_ptr<Resource> dummy;
      bool has = false;
      [[nodiscard]] const Resource& R() const { return has ? field.R() : *dummy; }
    };

    bool BindMask(Executor& ex, const OpArgs& a, MaskBind& m, std::string& err)
    {
      if (const Grid* g = In(a, "mask"); g != nullptr) {
        m.has = true;
        return UploadGrid(ex, *g, m.field, err);
      }
      const float zero = 0.0f;
      return MakeBuffer(ex, 1, m.dummy, err) && UploadBuffer(ex, *m.dummy, &zero, 4, err);
    }

    // ---- bilateral ----------------------------------------------------------------------------------------------------------------------------------

    OpResult OpBilateralGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      Job j;
      if (!Begin(ex, a, 3, j) || !MaskOk(a, j)) return OpResult::Unsupported;
      const Grid& src = *j.src;
      const int r = std::max(1, std::min(12, static_cast<int>(std::lround(a.p.N("radius", 6.0) / static_cast<double>(src.cell)))));
      const float sr = std::max(1e-4f, static_cast<float>(a.p.N("sigma_r", 2.0)));
      const float sigS = std::max(0.3f, static_cast<float>(r) * 0.5f);
      std::vector<float> ws(static_cast<std::size_t>((2 * r + 1) * (2 * r + 1)));
      for (int dz = -r; dz <= r; ++dz) {
        for (int dx = -r; dx <= r; ++dx) ws[static_cast<std::size_t>((dz + r) * (2 * r + 1) + dx + r)] = std::exp(-0.5f * static_cast<float>(dx * dx + dz * dz) / (sigS * sigS));
      }
      const KernelInfo* k = FindKernel("filt_bilateral");
      if (k == nullptr) { err = "kernel filt_bilateral is missing"; return OpResult::Failed; }
      std::unique_ptr<Resource> wsBuf;
      MaskBind mask;
      if (!Upload(ex, j, err) || !UploadVec(ex, ws, wsBuf, err) || !BindMask(ex, a, mask, err)) return OpResult::Failed;
      hk::BilateralParams p{};
      p.w = static_cast<std::uint32_t>(src.w);
      p.h = static_cast<std::uint32_t>(src.h);
      p.rad = static_cast<std::uint32_t>(r);
      p.hasMask = mask.has ? 1u : 0u;
      p.inv = 1.0f / (sr * sr);
      Bindings b;
      b.Srv(1, *wsBuf).Srv(2, j.in.R()).Srv(3, mask.R()).Uav(0, j.res.R());
      if (!DispatchBands(ex, *k, p.w, p.h, static_cast<double>((2 * r + 1) * (2 * r + 1)) * 0.1, p, b, err)) return OpResult::Failed;
      return Finish(ex, j, out, err);
    }

    // ---- diffuse (iterated) -------------------------------------------------------------------------------------------------------------------------

    OpResult OpDiffuseGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      Job j;
      if (!Begin(ex, a, 4, j) || !MaskOk(a, j)) return OpResult::Unsupported;
      const Grid& src = *j.src;
      const int iterations = std::max(0, std::min(5000, static_cast<int>(a.p.N("iterations", 20))));
      if (iterations == 0) return OpResult::Unsupported;
      const float kappa = std::max(1e-4f, static_cast<float>(a.p.N("kappa", 0.4)));
      const float rate = std::max(0.0f, std::min(0.25f, static_cast<float>(a.p.N("rate", 0.2))));
      const KernelInfo* k = FindKernel("filt_diffuse");
      if (k == nullptr) { err = "kernel filt_diffuse is missing"; return OpResult::Failed; }
      PingPong pp;
      MaskBind mask;
      if (!UploadGrid(ex, src, pp.f[0], err) || !MakeField(ex, src.w, src.h, pp.f[1], err) || !BindMask(ex, a, mask, err)) return OpResult::Failed;
      hk::DiffuseParams p{};
      p.w = static_cast<std::uint32_t>(src.w);
      p.h = static_cast<std::uint32_t>(src.h);
      p.rows = p.h;
      p.expo = a.p.S("g", "lorentz") == "exp" ? 1u : 0u;
      p.hasMask = mask.has ? 1u : 0u;
      p.invK2 = 1.0f / (kappa * kappa);
      p.invCell = 1.0f / src.cell;
      p.rate = rate;
      const bool ok = RunIterations(ex, static_cast<u32>(iterations), 60.0, [&](const u32, std::string& e) {
        Bindings b;
        b.Srv(1, pp.Src().R()).Srv(2, mask.R()).Uav(0, pp.Dst().R());
        if (!DispatchGrid(ex, *k, p.w, p.h, p, b, e)) return false;
        pp.Flip();
        return true;
      }, err);
      if (!ok) return OpResult::Failed;
      Grid result = NewGrid(j.shape);
      if (!DownloadGrid(ex, pp.Src(), result, err)) return OpResult::Failed;
      out = std::move(result);
      return OpResult::Done;
    }

    // ---- blur_dir ---------------------------------------------------------------------------------------------------------------------------------

    OpResult OpBlurDirGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      Job j;
      if (!Begin(ex, a, 3, j) || !MaskOk(a, j)) return OpResult::Unsupported;
      const Grid& src = *j.src;
      const double ang = a.p.N("angle", 0.0) * 3.14159265358979323846 / 180.0;
      const float ux = static_cast<float>(std::cos(ang)), uz = static_cast<float>(std::sin(ang));
      const float len = std::max(0.0f, static_cast<float>(a.p.N("length", 20.0) / static_cast<double>(src.cell)));
      const std::string shape = a.p.S("shape", "box");
      const int half = std::max(0, std::min(256, static_cast<int>(std::ceil(len * 0.5f))));
      if (half == 0) return OpResult::Unsupported;
      std::vector<float> wt(static_cast<std::size_t>(2 * half + 1));
      float wsum = 0.0f;
      for (int i = -half; i <= half; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(half);
        float v = 1.0f;
        if (shape == "tri") v = 1.0f - std::fabs(t);
        else if (shape == "gauss") v = std::exp(-2.0f * t * t);
        wt[static_cast<std::size_t>(i + half)] = v;
        wsum += v;
      }
      const KernelInfo* k = FindKernel("filt_blurdir");
      if (k == nullptr) { err = "kernel filt_blurdir is missing"; return OpResult::Failed; }
      std::unique_ptr<Resource> wtBuf;
      MaskBind mask;
      if (!Upload(ex, j, err) || !UploadVec(ex, wt, wtBuf, err) || !BindMask(ex, a, mask, err)) return OpResult::Failed;
      hk::BlurDirParams p{};
      p.w = static_cast<std::uint32_t>(src.w);
      p.h = static_cast<std::uint32_t>(src.h);
      p.taps = static_cast<std::uint32_t>(half);
      p.hasMask = mask.has ? 1u : 0u;
      p.ux = ux;
      p.uz = uz;
      p.wsum = wsum;
      Bindings b;
      b.Srv(1, *wtBuf).Srv(2, j.in.R()).Srv(3, mask.R()).Uav(0, j.res.R());
      if (!DispatchBands(ex, *k, p.w, p.h, static_cast<double>(2 * half + 1) * 0.08, p, b, err)) return OpResult::Failed;
      return Finish(ex, j, out, err);
    }

    // ---- dwarp --------------------------------------------------------------------------------------------------------------------------------------

    OpResult OpDWarpGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      Job j;
      if (!Begin(ex, a, 3, j) || !MaskOk(a, j)) return OpResult::Unsupported;
      const Grid& src = *j.src;
      const std::string kind = a.p.S("kind", "fbm");
      if (kind != "fbm" && kind != "curl" && kind != "swirl" && kind != "ripple" && kind != "pinch") return OpResult::Unsupported;
      const int k = kind == "fbm" ? 0 : (kind == "curl" ? 1 : (kind == "swirl" ? 2 : (kind == "ripple" ? 3 : 4)));
      const std::string edge = a.p.S("edge", "");
      hk::DWarpParams p{};
      p.w = static_cast<std::uint32_t>(src.w);
      p.h = static_cast<std::uint32_t>(src.h);
      p.kind = static_cast<std::uint32_t>(k);
      p.iters = static_cast<std::uint32_t>(std::max(1, std::min(3, static_cast<int>(a.p.N("iterations", 1)))));
      p.octaves = static_cast<std::uint32_t>(std::max(1, std::min(8, static_cast<int>(a.p.N("octaves", 3)))));
      p.edge = edge == "wrap" ? 1u : (edge == "mirror" ? 2u : 0u);
      p.seed = static_cast<std::uint32_t>(static_cast<std::int64_t>(a.p.N("seed", 1)));
      const double strength = a.p.N("strength", 40.0);
      const double size = std::max(1.0, a.p.N("size", 100.0));
      const double cell = static_cast<double>(j.shape.cell), wox = static_cast<double>(j.shape.ox), woz = static_cast<double>(j.shape.oz);
      const double cxw = a.p.Has("cx") ? a.p.N("cx", 0) : wox + 0.5 * (j.shape.w - 1) * cell;
      const double czw = a.p.Has("cz") ? a.p.N("cz", 0) : woz + 0.5 * (j.shape.h - 1) * cell;
      const double ang = a.p.N("angle", 0.0) * 3.14159265358979323846 / 180.0;
      const double rx = std::cos(ang), rz = std::sin(ang);
      const double wl = std::max(1.0, a.p.N("wavelength", 60.0));
      p.strength = static_cast<float>(strength);
      p.size = static_cast<float>(size);
      p.invSize = static_cast<float>(1.0 / size);
      p.cell = static_cast<float>(cell);
      p.invCell = static_cast<float>(1.0 / static_cast<double>(src.cell));
      p.e0x = static_cast<float>(wox - cxw);
      p.e0z = static_cast<float>(woz - czw);
      const double s0 = (-wox * rz + woz * rx) / wl;
      p.fu0 = static_cast<float>(s0 - std::floor(s0));
      p.pa = static_cast<float>(-cell * rz / wl);
      p.pb = static_cast<float>(cell * rx / wl);
      p.phase = static_cast<float>(a.p.N("phase", 0.0));
      p.rx = static_cast<float>(rx);
      p.rz = static_cast<float>(rz);
      p.rotK = static_cast<float>(strength * 3.14159265358979323846 / 180.0);
      std::vector<float> tab(32u, 0.0f);
      const auto putMap = [&](const int rec, const double sx, const double sz) {
        const double inv = 1.0 / size, aa = cell * inv;
        return PutMap(tab, rec, (wox + sx) * inv, (woz + sz) * inv, aa, 0.0, 0.0, aa);
      };
      if (!putMap(0, 0.0, 0.0) || !putMap(1, 17.3, -41.9)) return OpResult::Unsupported;
      const KernelInfo* ki = FindKernel("filt_dwarp");
      if (ki == nullptr) { err = "kernel filt_dwarp is missing"; return OpResult::Failed; }
      std::unique_ptr<Resource> octBuf;
      MaskBind mask;
      if (!Upload(ex, j, err) || !UploadVec(ex, tab, octBuf, err) || !BindMask(ex, a, mask, err)) return OpResult::Failed;
      p.hasMask = mask.has ? 1u : 0u;
      Bindings b;
      b.Srv(1, *octBuf).Srv(2, j.in.R()).Srv(3, mask.R()).Uav(0, j.res.R());
      const double units = k == 0 ? 2.0 * p.iters * p.octaves : (k == 1 ? static_cast<double>(p.octaves) : 1.0);
      if (!DispatchBands(ex, *ki, p.w, p.h, units, p, b, err)) return OpResult::Failed;
      return Finish(ex, j, out, err);
    }

    struct Registrar
    {
      Registrar()
      {
        RegisterGpuOp("occlusion", &OpOcclusionGpu);
        RegisterGpuOp("bilateral", &OpBilateralGpu);
        RegisterGpuOp("diffuse", &OpDiffuseGpu);
        RegisterGpuOp("blur_dir", &OpBlurDirGpu);
        RegisterGpuOp("dwarp", &OpDWarpGpu);
      }
    };
    const Registrar gRegistrar;
  } // namespace
} // namespace moho::editor::gpu
