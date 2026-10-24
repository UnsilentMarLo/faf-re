// EDITOR GPU compute: GPU versions of the generator ops (noise: perlin simplex ridged billow fractal musgrave fbm_damped; cellular: voronoi cells cracks; craters dunes island). Generators have no
// input field, so the only transfer is the download of the result: they are the best case for the GPU. Each op reads its options exactly like the CPU op (the same clamps; `fractal` through
// EdFieldFractal.h) and answers Unsupported for what it does not handle, so the CPU op keeps working. docs/projects/editor/GPU_COMPUTE.md, "Generator and filter kernels".
#pragma fp_contract(off)

#include "moho/editor/EdGpuGen.h"

#include "moho/editor/EdFieldFractal.h"

namespace moho::editor::gpu::hk
{
  using uint = std::uint32_t;
#include "moho/editor/gpu/gen_fbm_params.hlsli"
} // namespace moho::editor::gpu::hk

namespace moho::editor::gpu
{
  using namespace gen;

  namespace
  {
    inline std::uint32_t Hash32(std::uint32_t x)
    {
      x ^= x >> 16;
      x *= 0x7feb352dU;
      x ^= x >> 15;
      x *= 0x846ca68bU;
      x ^= x >> 16;
      return x;
    }

    // ---- perlin, simplex, ridged, billow --------------------------------------------------------------------------------------------------------

    OpResult OpNoiseGpu(Executor& ex, const OpArgs& a, const int kind, const char* const defaultBasis, Grid& out, std::string& err)
    {
      Shape s;
      if (!ShapeOf(a, s) || !Fits(ex, s.Count(), 1)) return OpResult::Unsupported;
      const std::uint32_t seed = static_cast<std::uint32_t>(static_cast<std::int64_t>(a.p.N("seed", 1)));
      const double scale = std::max(1e-3, a.p.N("scale", 100.0));
      const int octaves = std::max(1, std::min(16, static_cast<int>(a.p.N("octaves", 5))));
      const float persistence = static_cast<float>(a.p.N("persistence", 0.5));
      const float lacunarity = static_cast<float>(a.p.N("lacunarity", 2.0));
      const int basis = (a.p.S("basis", defaultBasis) == "simplex") ? 1 : 0;
      const double shiftX = a.p.N("shiftx", 0.0);
      const double shiftZ = a.p.N("shiftz", 0.0);
      const bool rotate = a.p.N("rotate", 0.0) >= 0.5;
      std::vector<float> tab(static_cast<std::size_t>(octaves) * 16u, 0.0f);
      std::vector<std::uint32_t> seeds(static_cast<std::size_t>(octaves));
      float amp = 1.0f;
      float norm = 0.0f;
      double freq = 1.0 / scale;
      for (int o = 0; o < octaves; ++o) {
        seeds[static_cast<std::size_t>(o)] = Hash32(seed * 131U + static_cast<std::uint32_t>(o) * 7919U + 17U);
        double c = 1.0, sn = 0.0, fox = 0.0, foz = 0.0;
        if (rotate) {
          const field::OctaveFrame& fr = field::OctaveFrameOf(o);
          c = fr.c;
          sn = fr.s;
          fox = fr.ox;
          foz = fr.oz;
        }
        const double wox = static_cast<double>(s.ox) + shiftX, woz = static_cast<double>(s.oz) + shiftZ;
        const bool ok = basis == 1 ? PutSimplexMap(tab, o, wox, woz, s.cell, freq, c, sn, fox, foz) : PutOctaveMap(tab, o, wox, woz, s.cell, freq, c, sn, fox, foz);
        if (!ok) return OpResult::Unsupported;
        tab[static_cast<std::size_t>(o) * 16u + 8u] = amp;
        norm += amp;
        amp *= persistence;
        freq *= static_cast<double>(lacunarity);
      }
      std::unique_ptr<Resource> octBuf, seedBuf;
      if (!UploadVec(ex, tab, octBuf, err) || !UploadVec(ex, seeds, seedBuf, err)) return OpResult::Failed;
      hk::FbmParams p{};
      p.w = static_cast<std::uint32_t>(s.w);
      p.octaves = static_cast<std::uint32_t>(octaves);
      p.kind = static_cast<std::uint32_t>(kind);
      p.basis = static_cast<std::uint32_t>(basis);
      p.fine = rotate ? 1u : 0u;
      p.norm = norm;
      Bindings b;
      b.Srv(1, *octBuf).Srv(2, *seedBuf);
      return RunGenerator(ex, "gen_fbm", s, static_cast<double>(octaves) * (basis == 1 ? 0.7 : 1.0), p, b, out, err);
    }

    OpResult OpPerlinGpu(Executor& ex, const OpArgs& a, Grid& o, std::string& e) { return OpNoiseGpu(ex, a, 0, "perlin", o, e); }
    OpResult OpSimplexGpu(Executor& ex, const OpArgs& a, Grid& o, std::string& e) { return OpNoiseGpu(ex, a, 0, "simplex", o, e); }
    OpResult OpRidgedGpu(Executor& ex, const OpArgs& a, Grid& o, std::string& e) { return OpNoiseGpu(ex, a, 1, "perlin", o, e); }
    OpResult OpBillowGpu(Executor& ex, const OpArgs& a, Grid& o, std::string& e) { return OpNoiseGpu(ex, a, 2, "perlin", o, e); }

    struct Registrar
    {
      Registrar()
      {
        RegisterGpuOp("perlin", &OpPerlinGpu);
        RegisterGpuOp("simplex", &OpSimplexGpu);
        RegisterGpuOp("ridged", &OpRidgedGpu);
        RegisterGpuOp("billow", &OpBillowGpu);
      }
    };
    const Registrar gRegistrar;
  } // namespace
} // namespace moho::editor::gpu
