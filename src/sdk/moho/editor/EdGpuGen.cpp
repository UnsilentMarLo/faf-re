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
#include "moho/editor/gpu/gen_fbmd_params.hlsli"
#include "moho/editor/gpu/gen_fractal_params.hlsli"
#include "moho/editor/gpu/gen_musgrave_params.hlsli"
} // namespace moho::editor::gpu::hk

namespace moho::editor::gpu
{
  using namespace gen;
  namespace fractal = field::fractal;

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

    // ---- fractal ------------------------------------------------------------------------------------------------------------------------------------

    OpResult OpFractalGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      Shape sh;
      if (!ShapeOf(a, sh) || !Fits(ex, sh.Count(), 1)) return OpResult::Unsupported;
      fractal::Setup s;
      std::string why;
      if (!fractal::Read(a, sh.cell, s, why)) return OpResult::Unsupported;   // a bad option: the CPU op reports it
      constexpr int kRecords = 19;
      std::vector<float> tab(static_cast<std::size_t>(kRecords) * 16u, 0.0f);
      std::vector<std::uint32_t> seeds(16u + 12u, 0u);
      const double wox = static_cast<double>(sh.ox), woz = static_cast<double>(sh.oz), cell = static_cast<double>(sh.cell);
      for (int o = 0; o < s.octaves; ++o) {
        double c = 1.0, sn = 0.0, fox = 0.0, foz = 0.0;
        if (s.rotate) {
          const field::OctaveFrame& fr = field::OctaveFrameOf(o);
          c = fr.c;
          sn = fr.s;
          fox = fr.ox;
          foz = fr.oz;
        }
        if (!PutOctaveMap(tab, o, wox, woz, cell, s.freq[o], c, sn, fox, foz)) return OpResult::Unsupported;
        float* r = tab.data() + static_cast<std::size_t>(o) * 16u;
        r[8] = s.amps[o];
        r[9] = static_cast<float>(s.move[o]);
        r[10] = static_cast<float>(c);
        r[11] = static_cast<float>(sn);
        r[12] = static_cast<float>(s.freq[o] * s.scale);
        r[13] = s.fade[o];
        seeds[static_cast<std::size_t>(o)] = s.octSeed[o];
      }
      if (s.warp > 0.0) {
        double freq = 1.0 / s.warpScale;
        for (int o = 0; o < 3; ++o) {
          double c = 1.0, sn = 0.0, fox = 0.0, foz = 0.0;
          if (s.rotate) {
            const field::OctaveFrame& fr = field::OctaveFrameOf(o + 5);
            c = fr.c;
            sn = fr.s;
            fox = fr.ox;
            foz = fr.oz;
          }
          if (!PutOctaveMap(tab, 16 + o, wox, woz, cell, freq, c, sn, fox, foz)) return OpResult::Unsupported;
          freq *= 2.0;
          for (std::uint32_t salt = 1; salt <= 4; ++salt) {
            seeds[16u + (salt - 1u) * 3u + static_cast<std::uint32_t>(o)] = Hash32(s.seed * 977U + salt * 31U + static_cast<std::uint32_t>(o) * 6151U + 5U);
          }
        }
      }
      std::unique_ptr<Resource> octBuf, seedBuf;
      if (!UploadVec(ex, tab, octBuf, err) || !UploadVec(ex, seeds, seedBuf, err)) return OpResult::Failed;
      hk::FractalParams p{};
      p.w = static_cast<std::uint32_t>(sh.w);
      p.octaves = static_cast<std::uint32_t>(s.octaves);
      p.kind = static_cast<std::uint32_t>(s.kind);
      p.style = static_cast<std::uint32_t>(s.style);
      p.warpLevels = static_cast<std::uint32_t>(s.warpLevels);
      p.warp = static_cast<float>(s.warp);
      p.slopeDetail = s.slopeDetail;
      p.erosion = s.erosion;
      p.sharp = s.sharp;
      p.norm = s.norm;
      p.fit = s.fit;
      p.invCell = static_cast<float>(1.0 / cell);
      Bindings b;
      b.Srv(1, *octBuf).Srv(2, *seedBuf);
      const double units = static_cast<double>(s.octaves) * (s.style == fractal::kErosive ? 1.2 : 1.0) + (s.warp > 0.0 ? 6.0 * s.warpLevels : 0.0);
      return RunGenerator(ex, "gen_fractal", sh, units, p, b, out, err);
    }

    // ---- musgrave, fbm_damped ----------------------------------------------------------------------------------------------------------------------

    /** The octave table of a plain octave stack: octave o at frequency 1 / scale * lacunarity^o, in its own frame when `rotate` (field 8 is left to the caller). false: Unsupported. */
    bool FillOctaves(std::vector<float>& tab, std::vector<std::uint32_t>& seeds, const int octaves, const std::uint32_t seed, const double scale, const double lacunarity, const bool rotate, const Shape& sh)
    {
      tab.assign(static_cast<std::size_t>(octaves) * 16u, 0.0f);
      seeds.assign(static_cast<std::size_t>(octaves), 0u);
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
        if (!PutOctaveMap(tab, o, static_cast<double>(sh.ox), static_cast<double>(sh.oz), static_cast<double>(sh.cell), freq, c, sn, fox, foz)) return false;
        tab[static_cast<std::size_t>(o) * 16u + 10u] = static_cast<float>(c);
        tab[static_cast<std::size_t>(o) * 16u + 11u] = static_cast<float>(sn);
        freq *= lacunarity;
      }
      return true;
    }

    OpResult OpMusgraveGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      Shape sh;
      if (!ShapeOf(a, sh) || !Fits(ex, sh.Count(), 1)) return OpResult::Unsupported;
      const std::uint32_t seed = static_cast<std::uint32_t>(static_cast<std::int64_t>(a.p.N("seed", 1)));
      const double scale = std::max(1e-3, a.p.N("scale", 100.0));
      const int octaves = std::max(1, std::min(16, static_cast<int>(a.p.N("octaves", 6))));
      const float lac = std::max(1.1f, static_cast<float>(a.p.N("lacunarity", 2.0)));
      const float hExp = static_cast<float>(a.p.N("h", 1.0));
      const float offset = static_cast<float>(a.p.N("offset", 1.0));
      const float gain = static_cast<float>(a.p.N("gain", 2.0));
      const bool rotate = a.p.N("rotate", 0.0) >= 0.5;
      const std::string mode = a.p.S("mode", "ridged");
      if (mode != "ridged" && mode != "hybrid" && mode != "hetero") return OpResult::Unsupported;
      const int kind = mode == "ridged" ? 0 : (mode == "hybrid" ? 1 : 2);
      std::vector<float> tab;
      std::vector<std::uint32_t> seeds;
      if (!FillOctaves(tab, seeds, octaves, seed, scale, static_cast<double>(lac), rotate, sh)) return OpResult::Unsupported;
      float ampSum = 0.0f;
      for (int o = 0; o < octaves; ++o) {
        const float amp = std::pow(lac, -hExp * static_cast<float>(o));
        tab[static_cast<std::size_t>(o) * 16u + 8u] = amp;
        ampSum += amp;
      }
      std::unique_ptr<Resource> octBuf, seedBuf;
      if (!UploadVec(ex, tab, octBuf, err) || !UploadVec(ex, seeds, seedBuf, err)) return OpResult::Failed;
      hk::MusgraveParams p{};
      p.w = static_cast<std::uint32_t>(sh.w);
      p.octaves = static_cast<std::uint32_t>(octaves);
      p.kind = static_cast<std::uint32_t>(kind);
      p.fine = rotate ? 1u : 0u;
      p.offset = offset;
      p.gain = gain;
      p.divisor = kind == 0 ? ampSum * std::max(1e-3f, offset * offset) : ampSum * std::max(1e-3f, offset + 1.0f);
      Bindings b;
      b.Srv(1, *octBuf).Srv(2, *seedBuf);
      return RunGenerator(ex, "gen_musgrave", sh, static_cast<double>(octaves), p, b, out, err);
    }

    OpResult OpFbmDampedGpu(Executor& ex, const OpArgs& a, Grid& out, std::string& err)
    {
      Shape sh;
      if (!ShapeOf(a, sh) || !Fits(ex, sh.Count(), 1)) return OpResult::Unsupported;
      const std::uint32_t seed = static_cast<std::uint32_t>(static_cast<std::int64_t>(a.p.N("seed", 1)));
      const double scale = std::max(1e-3, a.p.N("scale", 150.0));
      const int octaves = std::max(1, std::min(16, static_cast<int>(a.p.N("octaves", 8))));
      const float pers = static_cast<float>(a.p.N("persistence", 0.5));
      const float lac = std::max(1.1f, static_cast<float>(a.p.N("lacunarity", 2.0)));
      const float damp = std::max(0.0f, static_cast<float>(a.p.N("damp", 1.0)));
      const bool rotate = a.p.N("rotate", 0.0) >= 0.5;
      std::vector<float> tab;
      std::vector<std::uint32_t> seeds;
      if (!FillOctaves(tab, seeds, octaves, seed, scale, static_cast<double>(lac), rotate, sh)) return OpResult::Unsupported;
      std::unique_ptr<Resource> octBuf, seedBuf;
      if (!UploadVec(ex, tab, octBuf, err) || !UploadVec(ex, seeds, seedBuf, err)) return OpResult::Failed;
      hk::FbmdParams p{};
      p.w = static_cast<std::uint32_t>(sh.w);
      p.octaves = static_cast<std::uint32_t>(octaves);
      p.fine = rotate ? 1u : 0u;
      p.persistence = pers;
      p.damp = damp;
      Bindings b;
      b.Srv(1, *octBuf).Srv(2, *seedBuf);
      return RunGenerator(ex, "gen_fbmd", sh, static_cast<double>(octaves) * 1.1, p, b, out, err);
    }

    struct Registrar
    {
      Registrar()
      {
        RegisterGpuOp("perlin", &OpPerlinGpu);
        RegisterGpuOp("simplex", &OpSimplexGpu);
        RegisterGpuOp("ridged", &OpRidgedGpu);
        RegisterGpuOp("billow", &OpBillowGpu);
        RegisterGpuOp("fractal", &OpFractalGpu);
        RegisterGpuOp("musgrave", &OpMusgraveGpu);
        RegisterGpuOp("fbm_damped", &OpFbmDampedGpu);
      }
    };
    const Registrar gRegistrar;
  } // namespace
} // namespace moho::editor::gpu
