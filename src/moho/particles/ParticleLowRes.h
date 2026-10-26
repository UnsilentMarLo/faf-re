#pragma once

#include <cstdint>

// Large particles in a lower-resolution target (docs/PERFORMANCE.md, "Large particles in a lower-resolution target").
//
// The explosions of the game are a few hundred quads of 100,000 pixels and more, drawn over each other with blending: 100 and more
// screens of overdraw, which costs blend/ROP bandwidth and nothing else. This path draws the quads at or above a size threshold into a
// half (or quarter) resolution target and composites it onto the scene; everything smaller stays at full resolution.
//
// How (all of it behind the effect patch of ParticleEffectPatch.h, so Direct3D 11 and 12 and nothing else):
//   - the scene depth is downsampled once per RenderEffects call (the farthest depth of each low-resolution pixel's footprint);
//   - for a run of buckets (same sort scalar, same blend class) the large quads are drawn into a cleared accumulation target with the
//     `_LR` techniques (depth-tested against the downsampled depth in the pixel shader), the target is composited onto the scene with a
//     depth-aware upsample, then the small quads of the same buckets are drawn at full resolution;
//   - one instanced draw per bucket is issued twice from the same data; the vertex shader collapses the quads of the other size class
//     (the size is a function of the instance data and the camera only, so both draws agree on which quad is which).
// ADD accumulates exactly; ALPHABLEND and PREMODALPHA accumulate as premultiplied "over" (colour*alpha, coverage in alpha), which is
// exact for any order within the run. MODULATE*, REFRACT and TLight buckets are not touched; they end a run.
namespace moho
{
  class GeomCamera3;
  struct SParticleRenderBucket;

  /** 0 off, 1 half resolution, 2 quarter resolution (console variable `ren_ParticleLowRes`; FAF_PARTICLE_LOWRES overrides it). */
  extern int ren_ParticleLowRes;
  /** Side in pixels (full resolution) of a square quad that counts as large (`ren_ParticleLowResSize`; FAF_PARTICLE_LOWRES_SIZE). */
  extern float ren_ParticleLowResSize;

  /** Whether the active backend can run the path at all (Direct3D 11 or 12, effect patched). The settings registry asks this. */
  [[nodiscard]] bool ParticleLowResSupported();

  /** Whether a bucket may take part (a world-particle technique with the ADD, ALPHABLEND or PREMODALPHA blend). */
  [[nodiscard]] bool ParticleLowResEligible(const SParticleRenderBucket& bucket) noexcept;

  /** 0 for the additive class, 1 for the "over" class (ALPHABLEND, PREMODALPHA). */
  [[nodiscard]] int ParticleLowResClass(const SParticleRenderBucket& bucket) noexcept;

  namespace particlelowres
  {
    /**
     * Starts one RenderEffects call (the output target and viewport are the scene's). False: the path is off or unusable right now,
     * and nothing in the other calls may be used.
     */
    [[nodiscard]] bool Begin(const GeomCamera3& camera);

    /** Whether a prepared bucket may hold a quad that is large from this camera (false: draw it at full resolution as ever). */
    [[nodiscard]] bool MayHaveLargeQuads(const SParticleRenderBucket& bucket);

    /** Opens a run: switches to the accumulation target. `overClass` as ParticleLowResClass. */
    void BeginAccumulate();

    /** Composites the accumulation onto the scene, restores the scene target and selects the small quads. */
    void Composite(bool overClass);

    /** Closes a run: every quad is drawn again (LRParams mode 0). */
    void EndRun();

    /** Ends the RenderEffects call. */
    void End();
  } // namespace particlelowres
} // namespace moho
