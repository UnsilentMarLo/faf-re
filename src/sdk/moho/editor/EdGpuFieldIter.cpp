// EDITOR GPU compute: GPU versions of the iterated stencil ops of the field engine that are not erosion solvers of their own: settle, thermal_map (EdFieldEro2.cpp). Each op reads its options exactly
// like the CPU op, answers Unsupported for what it does not handle (the CPU op then runs and reports bad input itself) and keeps the field on the device for all iterations (ping-pong buffers,
// RunIterations: time limited submissions), so only one upload and one download cross the bus. The arithmetic is the CPU op's, in the same order. docs/projects/editor/GPU_COMPUTE.md, "More field ops (GP4)".
#pragma fp_contract(off)

#include "moho/editor/EdGpuGen.h"

namespace moho::editor::gpu::hk
{
  using uint = std::uint32_t;
#include "moho/editor/gpu/field_settle_params.hlsli"
} // namespace moho::editor::gpu::hk

namespace moho::editor::gpu
{
  using namespace gen;

  namespace
  {
    inline float Clamp01(const float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    /** The `iterations` option the CPU ops read (EdFieldEro2.cpp ItersOf). */
    inline int ItersOf(const OpArgs& a, const int fallback, const int cap = 20000) { return std::max(0, std::min(cap, static_cast<int>(a.p.N("iterations", fallback)))); }

    /** `a` and the optional same-shaped inputs: false = the CPU op reports the error (or has nothing to do). */
    const Grid* Source(const OpArgs& a)
    {
      const Grid* src = In(a, "a");
      if (src == nullptr || src->w < 1 || src->h < 1) return nullptr;
      if (const Grid* like = In(a, "like"); like != nullptr && !like->SameShape(*src)) return nullptr;
      return src;
    }

    /** Puts the chosen result into `out` and the others into the extra outputs (EdFieldEro2.cpp Emit). false = unknown output name (the CPU op reports it). */
    bool Emit(const OpArgs& a, Grid& out, std::map<std::string, Grid>& all, const char* const fallback)
    {
      const auto it = all.find(a.p.S("output", fallback));
      if (it == all.end()) return false;
      const std::string want = it->first;
      for (auto& kv : all) {
        if (kv.first != want && a.extra != nullptr) (*a.extra)[kv.first] = std::move(kv.second);
      }
      out = std::move(it->second);
      return true;
    }

    // ---- settle -------------------------------------------------------------------------------------------------------------------------------------

    OpResult OpSettleGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Source(a);
      const Grid* mask = In(a, "mask");
      if (src == nullptr || (mask != nullptr && !mask->SameShape(*src))) return OpResult::Unsupported;
      const std::string want = a.p.S("output", "height");
      if (want != "height" && want != "deposit") return OpResult::Unsupported;
      const int iterations = ItersOf(a, 20);
      if (iterations == 0) return OpResult::Unsupported;
      if (!Fits(ex, src->Count(), 5)) return OpResult::Unsupported;
      const KernelInfo* k = FindKernel("field_settle");
      if (k == nullptr) { err = "kernel field_settle is missing"; return OpResult::Failed; }
      hk::SettleParams p{};
      p.w = static_cast<std::uint32_t>(src->w);
      p.h = static_cast<std::uint32_t>(src->h);
      p.rate = std::max(0.0f, std::min(1.0f, static_cast<float>(a.p.N("rate", 0.4))));
      p.cap = std::max(0.0f, static_cast<float>(a.p.N("cap", 0.5)));
      p.slopeLo = static_cast<float>(a.p.N("slope_lo", 0.2));
      p.slopeHi = static_cast<float>(a.p.N("slope_hi", 0.6));
      p.cell = src->cell;
      PingPong pp;
      Field dep;
      MaskBind mb;
      if (!UploadGrid(ex, *src, pp.f[0], err) || !MakeField(ex, src->w, src->h, pp.f[1], err) || !MakeField(ex, src->w, src->h, dep, err) || !FillBuffer(ex, dep.R(), 0u, err) ||
          !BindMask(ex, a, mb, err)) {
        return OpResult::Failed;
      }
      p.hasMask = mb.has ? 1u : 0u;
      const bool ok = RunIterations(ex, static_cast<u32>(iterations), 60.0, [&](const u32, std::string& e) {
        Bindings b;
        b.Srv(0, pp.Src().R()).Srv(1, mb.R()).Uav(0, pp.Dst().R()).Uav(1, dep.R());
        if (!DispatchGrid(ex, *k, p.w, p.h, p, b, e)) return false;
        pp.Flip();
        return true;
      }, err);
      if (!ok) return OpResult::Failed;
      std::map<std::string, Grid> all;
      all["height"] = Grid(src->w, src->h, src->cell, src->ox, src->oz);
      all["deposit"] = Grid(src->w, src->h, src->cell, src->ox, src->oz);
      if (!DownloadGrid(ex, pp.Src(), all["height"], err)) return OpResult::Failed;
      if (want == "deposit" || a.extra != nullptr) {
        Grid sum(src->w, src->h, src->cell, src->ox, src->oz);
        if (!DownloadGrid(ex, dep, sum, err)) return OpResult::Failed;
        const float sc = std::max(1e-4f, static_cast<float>(a.p.N("deposit_scale", 1.0)));
        for (std::size_t i = 0; i < sum.v.size(); ++i) all["deposit"].v[i] = Clamp01(sum.v[i] / sc);
      }
      Grid result;
      if (!Emit(a, result, all, "height")) return OpResult::Unsupported;
      out = std::move(result);
      return OpResult::Done;
    }

    struct Registrar
    {
      Registrar() { RegisterGpuOp("settle", &OpSettleGpu, 16384.0f); }
    };
    const Registrar gRegistrar;
  } // namespace
} // namespace moho::editor::gpu
