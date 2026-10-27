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
//     the low-resolution target and is depth-tested by the hardware against the downsampled scene depth (the target's own depth
//     buffer). The accumulation holds S (rgb) and coverage A (alpha): an "over" layer (ALPHABLEND, PREMODALPHA) does
//     S = c*a + S*(1-a), A = a + A*(1-a) (premultiplied "over", the backends blend alpha with the colour factors), an ADD layer
//     does S += c*a and leaves A alone (colour-only writes). Any order of the two kinds composes exactly: scene' = S + scene*(1-A);
//   - LRDownsample2 / LRDownsample4: the farthest scene depth of the footprint of each low-resolution pixel, written as depth;
//   - LRComposite: the depth-aware upsample of S and A onto the scene (the nearest low-resolution pixel in depth at depth edges).
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
