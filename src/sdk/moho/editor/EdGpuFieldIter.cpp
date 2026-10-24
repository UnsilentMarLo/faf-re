// EDITOR GPU compute: GPU versions of the iterated stencil ops of the field engine that are not erosion solvers of their own: settle, thermal_map (EdFieldEro2.cpp). Each op reads its options exactly
// like the CPU op, answers Unsupported for what it does not handle (the CPU op then runs and reports bad input itself) and keeps the field on the device for all iterations (ping-pong buffers,
// RunIterations: time limited submissions), so only one upload and one download cross the bus. The arithmetic is the CPU op's, in the same order. docs/projects/editor/GPU_COMPUTE.md, "More field ops (GP4)".
#pragma fp_contract(off)

#include "moho/editor/EdGpuFieldCommon.h"

namespace moho::editor::gpu::hk
{
  using uint = std::uint32_t;
#include "moho/editor/gpu/field_settle_params.hlsli"
#include "moho/editor/gpu/field_slide_params.hlsli"
} // namespace moho::editor::gpu::hk

namespace moho::editor::gpu
{
  using namespace fops;

  namespace
  {
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

    // ---- slides of a loose layer (thermal_map) -------------------------------------------------------------------------------------------------------

    /**
     * The two passes of EdFieldEro2.cpp SlideT on the device. Buffers the op does not use are bound to a one element dummy (every slot a kernel declares must be bound); the flags in the
     * constant buffer keep the kernels away from them.
     */
    struct Slider
    {
      const KernelInfo* ka = nullptr;
      const KernelInfo* kb = nullptr;
      hk::SlideParams p{};
      Field share, moved;
      std::unique_ptr<Resource> dummy, dummyW1, dummyW2;   // one element stand-ins: a read-only one, and one per unused written slot (a resource is never both SRV and UAV of a dispatch)
      const Resource* rock = nullptr;
      const Resource* tmap = nullptr;
      const Resource* rmask = nullptr;
      const Resource* gain = nullptr;
      const Resource* lost = nullptr;

      bool Init(Executor& ex, const int w, const int h, std::string& err)
      {
        ka = FindKernel("field_slide_a");
        kb = FindKernel("field_slide_b");
        if (ka == nullptr || kb == nullptr) {
          err = "kernel field_slide_a / field_slide_b is missing";
          return false;
        }
        p.w = static_cast<std::uint32_t>(w);
        p.h = static_cast<std::uint32_t>(h);
        const float zero = 0.0f;
        return MakeField(ex, w, h, share, err) && MakeField(ex, w, h, moved, err) && MakeBuffer(ex, 1, dummy, err) && UploadBuffer(ex, *dummy, &zero, 4, err) && MakeBuffer(ex, 1, dummyW1, err) &&
               UploadBuffer(ex, *dummyW1, &zero, 4, err) && MakeBuffer(ex, 1, dummyW2, err) && UploadBuffer(ex, *dummyW2, &zero, 4, err);
      }

      const Resource& Or(const Resource* r) const { return r != nullptr ? *r : *dummy; }

      /** One slide step: `loose` -> `next`. */
      bool Step(Executor& ex, const Resource& loose, Resource& next, std::string& err)
      {
        Bindings a;
        a.Srv(0, loose).Srv(1, Or(rock)).Srv(2, Or(tmap)).Srv(3, Or(rmask)).Uav(0, share.R()).Uav(1, moved.R());
        if (!DispatchGrid(ex, *ka, p.w, p.h, p, a, err)) return false;
        Bindings b;
        b.Srv(0, loose).Srv(1, Or(rock)).Srv(2, Or(tmap)).Srv(3, share.R()).Srv(4, moved.R()).Uav(0, next).Uav(1, gain != nullptr ? *gain : *dummyW1).Uav(2, lost != nullptr ? *lost : *dummyW2);
        return DispatchGrid(ex, *kb, p.w, p.h, p, b, err);
      }
    };

    OpResult OpThermalMapGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Source(a);
      const Grid* tmap = In(a, "b");
      const Grid* mask = In(a, "mask");
      if (src == nullptr || (tmap != nullptr && !tmap->SameShape(*src)) || (mask != nullptr && !mask->SameShape(*src))) return OpResult::Unsupported;
      const std::string want = a.p.S("output", "height");
      if (want != "height" && want != "scree" && want != "moved") return OpResult::Unsupported;
      const int iterations = ItersOf(a, 30);
      if (iterations == 0) return OpResult::Unsupported;
      const int w = src->w, h = src->h;
      if (!Fits(ex, src->Count(), 10)) return OpResult::Unsupported;
      Slider sl;
      if (!sl.Init(ex, w, h, err)) return OpResult::Failed;
      sl.p.cell = src->cell;
      sl.p.rate = std::max(0.0f, std::min(1.0f, static_cast<float>(a.p.N("rate", 0.5))));
      sl.p.talus = static_cast<float>(a.p.N("talus", 0.6));
      const bool needSums = want != "height" || a.extra != nullptr;
      PingPong pp;
      Field tm, rm, gain, lost;
      if (!UploadGrid(ex, *src, pp.f[0], err) || !MakeField(ex, w, h, pp.f[1], err)) return OpResult::Failed;
      if (tmap != nullptr) {
        // the talus of each cell: lo + (hi - lo) * map, as the CPU op forms it
        const float lo = std::max(0.0f, static_cast<float>(a.p.N("talus_lo", 0.35))), hi = std::max(0.0f, static_cast<float>(a.p.N("talus_hi", 1.0)));
        std::vector<float> talus(src->Count());
        for (std::size_t i = 0; i < talus.size(); ++i) talus[i] = lo + (hi - lo) * Clamp01(tmap->v[i]);
        if (!MakeField(ex, w, h, tm, err) || !UploadField(ex, tm, talus.data(), err)) return OpResult::Failed;
        sl.tmap = &tm.R();
        sl.p.hasMap = 1;
      }
      if (mask != nullptr) {
        if (!UploadGrid(ex, *mask, rm, err)) return OpResult::Failed;
        sl.rmask = &rm.R();
        sl.p.hasMask = 1;
      }
      if (needSums) {
        if (!MakeField(ex, w, h, gain, err) || !MakeField(ex, w, h, lost, err) || !FillBuffer(ex, gain.R(), 0u, err) || !FillBuffer(ex, lost.R(), 0u, err)) return OpResult::Failed;
        sl.gain = &gain.R();
        sl.lost = &lost.R();
        sl.p.hasGain = 1;
        sl.p.hasLost = 1;
      }
      const bool ok = RunIterations(ex, static_cast<u32>(iterations), 60.0, [&](const u32, std::string& e) {
        if (!sl.Step(ex, pp.Src().R(), pp.Dst().R(), e)) return false;
        pp.Flip();
        return true;
      }, err);
      if (!ok) return OpResult::Failed;
      std::map<std::string, Grid> all;
      all["height"] = Grid(w, h, src->cell, src->ox, src->oz);
      all["scree"] = Grid(w, h, src->cell, src->ox, src->oz);
      all["moved"] = Grid(w, h, src->cell, src->ox, src->oz);
      if (!DownloadGrid(ex, pp.Src(), all["height"], err)) return OpResult::Failed;
      if (needSums) {
        Grid g(w, h, src->cell, src->ox, src->oz), l(w, h, src->cell, src->ox, src->oz);
        if (!DownloadGrid(ex, gain, g, err) || !DownloadGrid(ex, lost, l, err)) return OpResult::Failed;
        const float sc = std::max(1e-4f, static_cast<float>(a.p.N("scree_scale", 1.0)));
        for (std::size_t i = 0; i < g.v.size(); ++i) {
          all["scree"].v[i] = Clamp01(g.v[i] / sc);
          all["moved"].v[i] = Clamp01(l.v[i] / sc);
        }
      }
      Grid result;
      if (!Emit(a, result, all, "height")) return OpResult::Unsupported;
      out = std::move(result);
      return OpResult::Done;
    }

    struct Registrar
    {
      Registrar()
      {
        RegisterGpuOp("settle", &OpSettleGpu, 16384.0f);
        RegisterGpuOp("thermal_map", &OpThermalMapGpu, 16384.0f);
      }
    };
    const Registrar gRegistrar;
  } // namespace
} // namespace moho::editor::gpu
