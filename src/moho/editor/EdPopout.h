#pragma once

namespace moho
{
  /**
   * Editor addition (docs/projects/editor_research/SPIKE_EXTRA_WINDOWS.md, design B): pop-out windows for editor panels.
   *
   * A pop-out is an ordinary wx top-level frame with one child window, a Direct3D 11 swap chain of its own created on the engine's
   * device (not a GAL head) and an extra Maui root frame registered through `CUIManager::AddPopoutFrame`. It is drawn once
   * per engine frame by `RenderAll`, called at the end of `WRenViewport::D3DWindowOnDeviceRender`.
   * Lua API (bound in EdPopout.cpp): ed_window_create / destroy / get_rect / set_rect / capture.
   */
  namespace edpopout
  {
    /** Draws every open pop-out into its swap chain and presents it. Main thread only, after the main frame's scene. */
    void RenderAll();

    /** Destroys every open pop-out (windows, swap chains, root frames). Call before the UI manager and the device go away. */
    void DestroyAll();
  } // namespace edpopout
} // namespace moho
