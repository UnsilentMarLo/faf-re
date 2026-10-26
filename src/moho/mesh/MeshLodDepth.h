#pragma once

namespace moho
{
  /**
   * FAF Studio fix (no address; the shipped body is inline in MeshRenderer::Batch, 0x007DFB7F..0x007DFBB9).
   *
   * What it does:
   * The depth a mesh instance is LOD-selected and dissolved at: the world position dotted against the 4-float
   * "fade plane" `(a, b, c, d)` that `MeshRenderer::Batch` receives as its last argument, which every caller
   * fills with the camera's `viewport.r[1]` (view-space depth scaled to the FOV and the camera LOD scale).
   *
   * History: the reconstruction read these four floats from the first 16 bytes of the camera (the orientation
   * quaternion) instead, so the "distance" was `q.w*x + q.x*y + q.y*z + q.z` - unrelated to how far the mesh is
   * from the eye, different for every heading, and sign-flipped when the heading was reached by turning the
   * other way round (q and -q are the same rotation). Units went to a lower LOD and vanished just by turning.
   *
   * Plain floats on purpose: this header has no engine dependencies so the stand-alone tests can include it.
   */
  [[nodiscard]] inline float MeshLodDepth(
    const float planeA,
    const float planeB,
    const float planeC,
    const float planeD,
    const float x,
    const float y,
    const float z
  ) noexcept
  {
    return (planeA * x) + (planeB * y) + (planeC * z) + planeD;
  }
} // namespace moho
