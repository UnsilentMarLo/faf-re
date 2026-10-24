// EDITOR GPU compute: GPU versions of more filter ops of the field engine (EdFieldFilt.cpp, EdFieldSc.cpp): morph, convolve, unsharp, edge, blur_slope, slope_limit ... Each op reads its options
// exactly like the CPU op and answers Unsupported for what it does not handle (the CPU op then runs and reports bad input itself). Ops whose cost per sample is small go to the GPU only when
// the window is big enough to pay for the transfer (fops::WorthIt). docs/projects/editor/GPU_COMPUTE.md, "More field ops (GP4)".
#pragma fp_contract(off)

#include "moho/editor/EdFieldUtil.h"
#include "moho/editor/EdGpuFieldCommon.h"

namespace moho::editor::gpu::hk
{
  using uint = std::uint32_t;
#include "moho/editor/gpu/field_convolve_params.hlsli"
#include "moho/editor/gpu/field_morph_params.hlsli"
#include "moho/editor/gpu/field_sub_params.hlsli"
} // namespace moho::editor::gpu::hk

namespace moho::editor::gpu
{
  using namespace fops;

  namespace
  {
    /** Window size in samples from world units (EdFieldFilt.cpp SamplesOf). */
    inline int SamplesOf(const double units, const Grid& g, const int lo, const int hi)
    {
      return std::max(lo, std::min(hi, static_cast<int>(std::lround(units / static_cast<double>(g.cell)))));
    }

    // ---- morph --------------------------------------------------------------------------------------------------------------------------------------

    struct MorphJob
    {
      Executor* ex = nullptr;
      const KernelInfo* km = nullptr;
      int w = 0, h = 0, r = 0, shape = 0;   // shape as the CPU op: 0 square, 1 disc, 2 cross, 3 diamond
      double unitsPerCell = 1.0;

      /** One dilation (isMax) or erosion of `in` into `out`; the square element takes `tmp` as the in-between field (two separable passes). */
      bool Pass(const Resource& in, Resource& out, Resource& tmp, const bool isMax, std::string& err)
      {
        hk::MorphParams p{};
        p.w = static_cast<std::uint32_t>(w);
        p.h = static_cast<std::uint32_t>(h);
        p.rad = static_cast<std::uint32_t>(r);
        p.isMax = isMax ? 1u : 0u;
        if (shape == 0) {
          p.shape = 0;
          Bindings b0;
          b0.Srv(0, in).Uav(0, tmp);
          if (!DispatchBands(*ex, *km, p.w, p.h, unitsPerCell, p, b0, err)) return false;
          p.shape = 1;
          Bindings b1;
          b1.Srv(0, tmp).Uav(0, out);
          return DispatchBands(*ex, *km, p.w, p.h, unitsPerCell, p, b1, err);
        }
        p.shape = shape == 1 ? 2u : (shape == 2 ? 4u : 3u);
        Bindings b;
        b.Srv(0, in).Uav(0, out);
        return DispatchBands(*ex, *km, p.w, p.h, unitsPerCell, p, b, err);
      }
    };

    OpResult OpMorphGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = SourceStrict(a);
      if (src == nullptr) return OpResult::Unsupported;
      const std::string op = a.p.S("op", "dilate");
      static const char* const ops[] = {"dilate", "erode", "open", "close", "gradient", "tophat", "bothat"};
      int k = -1;
      for (int i = 0; i < 7; ++i) {
        if (op == ops[i]) k = i;
      }
      const std::string sh = a.p.S("shape", "square");
      const int shape = sh == "square" ? 0 : (sh == "disc" ? 1 : (sh == "cross" ? 2 : (sh == "diamond" ? 3 : -1)));
      if (k < 0 || shape < 0) return OpResult::Unsupported;   // the CPU op reports the bad name
      const int r = SamplesOf(a.p.N("radius", 4.0), *src, 1, shape == 0 ? 32 : 16);
      // taps of one dilation / erosion: two windows for the square, the candidate positions of the window otherwise
      const double tapsPerPass = shape == 0 ? 2.0 * (2.0 * r + 1.0) : (shape == 2 ? 2.0 * (2.0 * r + 1.0) : (shape == 3 ? (r + 1.0) * (r + 1.0) * 2.0 : (2.0 * r + 1.0) * (2.0 * r + 1.0)));
      const int passes = k < 2 ? 1 : 2;
      if (!WorthIt(tapsPerPass * passes)) return OpResult::Unsupported;
      if (!Fits(ex, src->Count(), 5)) return OpResult::Unsupported;
      MorphJob job;
      job.ex = &ex;
      job.km = FindKernel("field_morph");
      const KernelInfo* ks = FindKernel("field_sub");
      if (job.km == nullptr || ks == nullptr) { err = "kernel field_morph / field_sub is missing"; return OpResult::Failed; }
      job.w = src->w;
      job.h = src->h;
      job.r = r;
      job.shape = shape;
      job.unitsPerCell = tapsPerPass * 0.06;
      Field in, t1, t2, tmp, res;
      if (!UploadGrid(ex, *src, in, err) || !MakeField(ex, job.w, job.h, t1, err) || !MakeField(ex, job.w, job.h, t2, err) || !MakeField(ex, job.w, job.h, tmp, err) ||
          !MakeField(ex, job.w, job.h, res, err)) {
        return OpResult::Failed;
      }
      Field* result = &t1;
      bool ok = true;
      switch (k) {
        case 0: ok = job.Pass(in.R(), t1.R(), tmp.R(), true, err); break;
        case 1: ok = job.Pass(in.R(), t1.R(), tmp.R(), false, err); break;
        case 2: ok = job.Pass(in.R(), t2.R(), tmp.R(), false, err) && job.Pass(t2.R(), t1.R(), tmp.R(), true, err); break;
        case 3: ok = job.Pass(in.R(), t2.R(), tmp.R(), true, err) && job.Pass(t2.R(), t1.R(), tmp.R(), false, err); break;
        default: {
          // gradient = dilate(a) - erode(a); tophat = a - open(a); bothat = close(a) - a
          const Resource* x = nullptr;
          const Resource* y = nullptr;
          if (k == 4) {
            ok = job.Pass(in.R(), t1.R(), tmp.R(), true, err) && job.Pass(in.R(), t2.R(), tmp.R(), false, err);
            x = &t1.R();
            y = &t2.R();
          } else if (k == 5) {
            ok = job.Pass(in.R(), t1.R(), tmp.R(), false, err) && job.Pass(t1.R(), t2.R(), tmp.R(), true, err);
            x = &in.R();
            y = &t2.R();
          } else {
            ok = job.Pass(in.R(), t1.R(), tmp.R(), true, err) && job.Pass(t1.R(), t2.R(), tmp.R(), false, err);
            x = &t2.R();
            y = &in.R();
          }
          if (ok) {
            hk::SubParams sp{};
            sp.w = static_cast<std::uint32_t>(job.w);
            sp.h = static_cast<std::uint32_t>(job.h);
            Bindings b;
            b.Srv(0, *x).Srv(1, *y).Uav(0, res.R());
            ok = DispatchGrid(ex, *ks, sp.w, sp.h, sp, b, err);
          }
          result = &res;
          break;
        }
      }
      if (!ok) return OpResult::Failed;
      Grid g = LikeOf(*src);
      if (!DownloadGrid(ex, *result, g, err)) return OpResult::Failed;
      out = std::move(g);
      return OpResult::Done;
    }

    // ---- convolve -----------------------------------------------------------------------------------------------------------------------------------

    OpResult OpConvolveGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Source(a);
      const std::vector<double>* kv = a.p.A("kernel");
      Shape shape;
      if (src == nullptr || kv == nullptr || kv->empty() || !ShapeOf(a, shape)) return OpResult::Unsupported;
      const int n = static_cast<int>(std::lround(std::sqrt(static_cast<double>(kv->size()))));
      if (n * n != static_cast<int>(kv->size()) || n % 2 == 0 || n > 15) return OpResult::Unsupported;   // the CPU op reports the bad kernel
      if (!WorthIt(static_cast<double>(n) * n) || !Fits(ex, src->Count(), 2)) return OpResult::Unsupported;
      std::vector<float> k(kv->size());
      double sum = 0.0;
      for (std::size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<float>((*kv)[i]);
        sum += static_cast<double>(k[i]);
      }
      float mul = static_cast<float>(a.p.N("scale", 1.0));
      if (a.p.N("normalize", 1) != 0 && std::fabs(sum) > 1e-6) mul /= static_cast<float>(sum);
      hk::ConvolveParams p{};
      p.w = static_cast<std::uint32_t>(src->w);
      p.h = static_cast<std::uint32_t>(src->h);
      p.n = static_cast<std::uint32_t>(n);
      p.edge = static_cast<std::uint32_t>(field::t7::ParseEdge(a, field::t7::kClamp));
      p.mul = mul;
      p.bias = static_cast<float>(a.p.N("bias", 0.0));
      const KernelInfo* kc = FindKernel("field_convolve");
      if (kc == nullptr) { err = "kernel field_convolve is missing"; return OpResult::Failed; }
      Field in, res;
      std::unique_ptr<Resource> kbuf;
      if (!UploadGrid(ex, *src, in, err) || !MakeField(ex, src->w, src->h, res, err) || !UploadVec(ex, k, kbuf, err)) return OpResult::Failed;
      Bindings b;
      b.Srv(0, in.R()).Srv(1, *kbuf).Uav(0, res.R());
      if (!DispatchBands(ex, *kc, p.w, p.h, static_cast<double>(n) * n * 0.05, p, b, err)) return OpResult::Failed;
      Grid g = NewGrid(shape);   // placed like the CPU op places it (`like`, else `a`)
      if (!DownloadGrid(ex, res, g, err)) return OpResult::Failed;
      out = std::move(g);
      return OpResult::Done;
    }

    struct Registrar
    {
      Registrar()
      {
        RegisterGpuOp("morph", &OpMorphGpu);
        RegisterGpuOp("convolve", &OpConvolveGpu);
      }
    };
    const Registrar gRegistrar;
  } // namespace
} // namespace moho::editor::gpu
