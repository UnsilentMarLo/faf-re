// EDITOR: render-side switches and per-frame counters of the editor (docs/projects/editor/PERFORMANCE.md, section "GPU and render").
//
// Console variables (all default to the setting that gives the same picture as before, only with less work; 0 switches an optimisation off):
//   ren_ShadowCache         1    skip the shadow pass when the light camera, the settings and the scene are the same as when the map in the render target was drawn (EdShadowCache.h)
//   ren_ShadowCacheTrees    0    1: a map drawn with wind swaying trees (UndulatingDepthClip) may be reused too (the sway is a fraction of a texel; changes the picture by that much)
//   ren_ShadowCacheMaxAge   300  the held shadow map is drawn again after this many reused frames (0: never)
//   ren_WaterSkipDisabled   1    a map with water switched off (water_on 0) skips the reflection pass, the water mask, the refraction copy and the water surface
//   ui_CullOffscreen        1    Maui bitmaps / borders / texts that lie completely outside the window are not built and queued (EdUiCull.h)
//   ui_SkipTransparent      1    Maui bitmaps / borders whose alpha is 0 are not drawn
//   ren_AtlasFastLookup     1    the texture atlas finds a texture it holds by pointer (hash) before it walks its ordered set
// Lua: ed_render_stats() -> table (see EdRenderStats.cpp).
//
// All counters are plain integers: everything that writes them runs on the main thread (the render pass, the UI pass and the Lua natives are the same thread).
#pragma once

#include <chrono>
#include <cstdint>

namespace moho
{
  extern bool ren_ShadowCache;
  extern bool ren_ShadowCacheTrees;
  extern int ren_ShadowCacheMaxAge;
  extern bool ren_WaterSkipDisabled;
  extern bool ui_CullOffscreen;
  extern bool ui_SkipTransparent;
  extern bool ren_AtlasFastLookup;
}

namespace moho::editor::renderstats
{
  /** What one frame did. Counters of the frame that is being built are in `Cur()`, the numbers of the last complete frame in `Last()`. */
  struct Frame
  {
    // from the graphics backend (every DrawPrimitive / DrawIndexedPrimitive of the frame, whatever issued it)
    std::uint32_t drawCalls = 0;
    std::uint32_t primitives = 0;
    std::uint32_t vertices = 0;
    // the prim batcher (UI, text, editor overlay, ranges ...): one flush is one draw call
    std::uint32_t batcherFlushes = 0;
    std::uint32_t batcherVertices = 0;
    std::uint32_t batcherIndices = 0;
    // texture lookups of the batcher
    std::uint32_t atlasLookups = 0;
    std::uint32_t atlasFastHits = 0;
    std::uint32_t atlasAdds = 0;
    std::uint32_t atlasResets = 0;
    // Maui controls
    std::uint32_t uiBitmapsDrawn = 0;
    std::uint32_t uiBitmapsCulled = 0;       // outside the window
    std::uint32_t uiBitmapsTransparent = 0;  // alpha 0
    std::uint32_t uiBordersDrawn = 0;
    std::uint32_t uiBordersCulled = 0;
    std::uint32_t uiTextsDrawn = 0;
    std::uint32_t uiTextsCulled = 0;
    // the editor's native overlay (EdOverlay.cpp edoverlay::Render): its share of the batcher's work
    std::uint32_t overlayFlushes = 0;
    std::uint32_t overlayVertices = 0;
    // shadow map
    std::uint32_t shadowRendered = 0;        // 1 when this frame drew the map
    std::uint32_t shadowReused = 0;          // 1 when it was reused
    // water
    std::uint32_t waterSkipped = 0;          // 1 when the water passes were left out (water switched off)
    // CPU milliseconds on the render thread (submission, not GPU time)
    double uiMs = 0.0;                       // CUIManager::RenderFrames + DrawUI
    double shadowMs = 0.0;                   // Shadow::RenderFrameShadows (cache check, batch, depth pass, blur)
    double shadowSceneMs = 0.0;              // of that: the signature of the scene
    double renderMs = 0.0;                   // WRenViewport::Render (all heads)
    double overlayMs = 0.0;                  // edoverlay::Render (building and queueing the quads, and the flushes it causes)
  };

  inline Frame gCur;
  inline Frame gLast;
  inline std::uint64_t gFrames = 0;

  /** Cumulative since start (not per frame). */
  inline std::uint64_t gShadowHits = 0;
  inline std::uint64_t gShadowMisses = 0;
  inline std::uint64_t gWaterSkippedFrames = 0;

  inline double NowMs() noexcept
  {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
  }

  /** Adds the time between construction and destruction to a counter. */
  class ScopedMs
  {
  public:
    explicit ScopedMs(double& sink) noexcept
      : sink_(sink)
      , start_(NowMs())
    {}
    ~ScopedMs() { sink_ += NowMs() - start_; }
    ScopedMs(const ScopedMs&) = delete;
    ScopedMs& operator=(const ScopedMs&) = delete;

  private:
    double& sink_;
    double start_;
  };

  /**
   * Called once per frame, right before the graphics backend's draw statistics are taken (WRenViewport::D3DWindowOnDeviceRender): `backend*` are the submissions since the
   * last call, i.e. those of the frame that has just ended. The counters of that frame become `Last()`, the new frame starts at zero.
   */
  inline void NextFrame(const std::uint32_t backendDrawCalls, const std::uint32_t backendPrimitives, const std::uint32_t backendVertices) noexcept
  {
    gLast = gCur;
    gLast.drawCalls = backendDrawCalls;
    gLast.primitives = backendPrimitives;
    gLast.vertices = backendVertices;
    gCur = Frame{};
    ++gFrames;
  }
} // namespace moho::editor::renderstats
