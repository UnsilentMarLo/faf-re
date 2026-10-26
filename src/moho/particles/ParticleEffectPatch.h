#pragma once

#include <string>

// Source-level patch of FAF's particle.fx for the low-resolution large-particle path (docs/PERFORMANCE.md, "Large particles in a
// lower-resolution target"; the runtime is ParticleLowRes.cpp).
//
// The game's effect files are user data and are never copied into this repository: the patch runs on the text that is about to be
// compiled (CD3DEffect::InitEffectFromFile), on the Direct3D 11 and 12 (D3D10 effect dialect) backends only. Pure text in, text out;
// an unexpected source leaves the text untouched and says why in `error`, so the effect still compiles, without the path.
//
// What the patched effect has on top of the original:
//   - LRParams (x: 0 draw every particle, 1 only the quads below the size threshold, 2 only the quads at or above it; y: the
//     threshold in pixels^2 at full resolution; zw: half the viewport size). WorldVS and WorldVSAlign collapse the other quads to
//     nothing when x is not 0, so one instanced draw of a bucket can be issued twice, once per resolution, from the same data.
//   - for every WorldVS/WorldVSAlign technique with the ADD, ALPHABLEND or PREMODALPHA blend, a `<name>_LR` twin that accumulates into
//     the low-resolution target (premultiplied: colour*alpha, coverage in alpha) and tests depth against the downsampled scene depth
//     in the pixel shader;
//   - LRDownsample2 / LRDownsample4: the farthest scene depth of the footprint of each low-resolution pixel (packed into RG16);
//   - LRComposite_ADD / LRComposite_OVER: depth-aware upsample of the low-resolution target onto the scene.
namespace moho
{
  struct ParticleEffectPatchResult
  {
    bool patched = false;
    int techniques = 0; // low-resolution twins added
    std::string error;  // why nothing was patched, when `patched` is false
  };

  ParticleEffectPatchResult PatchParticleEffect(std::string& source);

  // Whether the effect that is loaded now has the patch (set by CD3DEffect::InitEffectFromFile, read by ParticleLowRes).
  void SetParticleEffectPatched(bool patched) noexcept;
  [[nodiscard]] bool ParticleEffectPatched() noexcept;
} // namespace moho
