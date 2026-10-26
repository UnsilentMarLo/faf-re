#pragma once

#include <cstdint>
#include <string_view>

namespace gpg::gal
{
  // Forced anisotropic texture filtering (docs/RENDERER_BACKENDS.md, "Anisotropic filtering").
  //
  // The shipped effects pick their own samplers, almost all of them trilinear, so terrain, decals and meshes blur at grazing angles. Each
  // backend asks `ForceAnisotropicFilter` where it creates the sampler an effect uses and gets the anisotropic form of a linear filter back. Only
  // linear min/mag filters of effect samplers are touched: point samplers, comparison (shadow) samplers, samplers that already are
  // anisotropic, the engine's own blit/post-process samplers and the data/LUT samplers (`IsDataSampler`) stay as authored.
  //
  // The level (0 = off, 1 = 2x, 2 = 4x, 3 = 8x, 4 = 16x) is the console variable `ren_Anisotropy` (moho::render, src/moho/render/AnisotropicFilter.cpp,
  // the render feature `aniso_quality`), which writes this variable directly; the devices read it when they build a sampler. A device that
  // caches samplers per program compares `ForcedAnisotropyEpoch()`, which changes with the level, to know when to rebuild them.
  inline int gForcedAnisotropyLevel = 0;

  // The level of the render feature as samples.
  [[nodiscard]] constexpr int AnisotropyForLevel(const int level) noexcept
  {
    return level <= 0 ? 1 : (level >= 4 ? 16 : (2 << (level - 1)));
  }

  // Maximum anisotropy in samples: 1 = off, 2, 4, 8 or 16.
  [[nodiscard]] inline int ForcedAnisotropy() noexcept
  {
    return AnisotropyForLevel(gForcedAnisotropyLevel);
  }

  // Never 0, so a device that starts with epoch 0 builds its first (authored or forced) samplers on the first use.
  [[nodiscard]] inline unsigned ForcedAnisotropyEpoch() noexcept
  {
    return static_cast<unsigned>(gForcedAnisotropyLevel < 0 ? 0 : (gForcedAnisotropyLevel > 4 ? 4 : gForcedAnisotropyLevel)) + 1U;
  }

  // Samplers whose textures are tables, not pictures: they are read with computed coordinates or an explicit level, and a wider footprint
  // would blur the lookup (the dynamic-light data/grid textures, the BRDF and anisotropy lookup, ramps).
  [[nodiscard]] inline bool IsDataSampler(const std::string_view name) noexcept
  {
    constexpr std::string_view kMarkers[] = {"dynlight", "lookup", "anisotropic", "ramp", "fresnel", "falloff"};
    for (const std::string_view marker : kMarkers) {
      if (name.size() < marker.size()) {
        continue;
      }
      for (std::size_t at = 0; at + marker.size() <= name.size(); ++at) {
        bool same = true;
        for (std::size_t i = 0; i < marker.size() && same; ++i) {
          char c = name[at + i];
          if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
          }
          same = (c == marker[i]);
        }
        if (same) {
          return true;
        }
      }
    }
    return false;
  }

  // Whether an effect sampler gets forced anisotropy at `samples` (> 1): a 2D sampler of a texture the caller found to be a world texture (a
  // picture with a mip chain, not a render target), not a data/LUT sampler, with a mip range and a linear filter. On true the sampler is
  // rewritten (`filter`, `maxAnisotropy`). `Sampler` is effects::SamplerDesc.
  template <class Sampler>
  [[nodiscard]] inline bool ForceAnisotropicSampler(Sampler& desc, const int samples) noexcept;

  // `filter` is a D3D11_FILTER / D3D12_FILTER value (bit 0 mip linear, 2 mag linear, 4 min linear, 6 anisotropic, 7 comparison). A filter
  // with linear minification and magnification that is neither anisotropic nor a comparison becomes the anisotropic filter (0x55); the mip
  // mode changes with it because hardware anisotropy always blends levels. Returns true when the filter changed.
  [[nodiscard]] inline bool ForceAnisotropicFilter(std::uint32_t& filter) noexcept
  {
    constexpr std::uint32_t kMipLinear = 0x01U, kMagLinear = 0x04U, kMinLinear = 0x10U, kAnisotropic = 0x40U, kComparison = 0x80U;
    if ((filter & (kAnisotropic | kComparison)) != 0U || (filter & (kMagLinear | kMinLinear)) != (kMagLinear | kMinLinear)) {
      return false;
    }
    filter = kAnisotropic | kMinLinear | kMagLinear | kMipLinear;
    return true;
  }

  template <class Sampler>
  inline bool ForceAnisotropicSampler(Sampler& desc, const int samples) noexcept
  {
    if (samples <= 1 || desc.kind != Sampler::Kind::Texture2D || desc.maxLod <= 0.5f || IsDataSampler(desc.name) ||
        !ForceAnisotropicFilter(desc.filter)) {
      return false;
    }
    desc.maxAnisotropy = static_cast<std::uint32_t>(samples);
    return true;
  }
} // namespace gpg::gal
