#pragma once

namespace moho
{
  class GeomCamera3;
  class CD3DPrimBatcher;

  /**
   * Editor addition: retained world-space line overlay (areas, outlines) drawn by the world view.
   *
   * Lua registers polylines once (`ed_overlay_set(id, points, color, ...)`), the world view draws them every frame as
   * terrain-hugging ribbons in screen space: the line has a width in world units (so it looks like a stripe painted on the
   * ground up close) but never gets thinner than `minPx` (so it stays visible from far away): distance-scaled thickness.
   * Called from `CUIWorldView::Render` after the projectile arcs; main thread only.
   */
  namespace edoverlay
  {
    void Render(GeomCamera3* camera, CD3DPrimBatcher* batcher);

    /** The height samples [x0, x1] x [z0, z1] were written (the document's height sink): the slope view re-rates what looks at them, projected overlay grids move with the ground. */
    void NotifyHeights(int x0, int z0, int x1, int z1);

    /** Every height may have changed (a document was attached anew). */
    void NotifyHeightsAll();
  } // namespace edoverlay
} // namespace moho
