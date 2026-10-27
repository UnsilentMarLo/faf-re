// EDITOR: post-process anti-aliasing (FXAA 3.11) for the world view, for every adapter and without a multisampled swap chain
// (docs/projects/editor/FXAA.md). The shader is editor/runtime/override/effects/fxaa.fx.
//
// Console variables:
//   ed_fxaa          0  1 = the scene is drawn into an off-screen target and smoothed by one full-screen pass on its way to the back buffer; the Maui UI is drawn on top, unfiltered
//   ed_fxaa_subpix   0.75   0..1  how much thin lines and single pixels are blended (FXAA "quality subpix")
//   ed_fxaa_edge     0.166  edges with less contrast than this fraction of the local maximum are left alone (FXAA "edge threshold"; 0.125 / 0.063: more edges found, softer texture)
//   ed_fxaa_edge_min 0.0833 edges with less absolute luma contrast than this are left alone (FXAA "edge threshold min"; raise it to keep fine texture detail)
//
//   ed_fxaa_passes   1      draws the pass this many times per frame (1..40): for measuring its cost, the picture does not change
//   ed_fxaa_time     0      1 = measure the GPU time of the copy and the passes with D3D9 timestamp queries, logged every 120 frames
//
// FXAA only runs on a back buffer without swap chain anti-aliasing (a multisampled back buffer cannot be the target of the copy the pass needs); with MSAA on it stays off.
#pragma once

#include <vector>

#include "boost/shared_ptr.h"

namespace gpg::gal
{
  class RenderTarget;
}

namespace moho
{
  class CD3DDevice;
  extern bool ed_fxaa;
  extern float ed_fxaa_subpix;
  extern float ed_fxaa_edge;
  extern float ed_fxaa_edge_min;
  extern int ed_fxaa_passes;
  extern bool ed_fxaa_time;
} // namespace moho

namespace moho::edfxaa
{
  /**
   * One frame of one head. Created at the top of WRenViewport::Render; when FXAA is wanted it redirects the head's render target to the off-screen scene target, so every pass of
   * the frame (clear, sky, terrain, meshes, water, effects, the world overlay) draws there. Resolve() puts the real back buffer back, copies the scene to it and smooths the world
   * view rectangles; whatever draws afterwards (Maui, cartographic, silhouettes) draws unfiltered on top. The destructor restores the real target on every way out (an exception
   * from a pass included).
   */
  class FrameScope
  {
  public:
    FrameScope(CD3DDevice* device, int head, bool allowed);
    ~FrameScope();
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;

    /** True while the head's render target is the off-screen scene target. */
    bool Active() const { return mActive; }

    /** A rectangle (window pixels) of the scene that gets smoothed: the world view's screen rectangle. */
    void AddRect(int x, int y, int width, int height);

    /** Puts the real back buffer back, copies the scene into it and runs the FXAA pass over the rectangles. A no-op when not active. */
    void Resolve();

  private:
    void Restore();

    CD3DDevice* mDevice;
    int mHead;
    bool mActive = false;
    void* mHeadLock = nullptr;   // the head's CD3DRenderTarget (kept as a pointer: it is owned by the device)
    boost::shared_ptr<gpg::gal::RenderTarget> mSaved;   // the real back buffer surface while the head points at the scene target
    struct Rect
    {
      int x, y, w, h;
    };
    std::vector<Rect> mRects;
  };

  /** Drops the off-screen target and the quad: call before every device Reset and at shutdown (WRenViewport::D3DWindowOnDeviceExit). */
  void ReleaseDeviceResources();
} // namespace moho::edfxaa
