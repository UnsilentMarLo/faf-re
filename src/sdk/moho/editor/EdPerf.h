// EDITOR: main loop frame pacing and CPU statistics of the editor (docs/projects/editor/PERFORMANCE.md, section "Engine CPU"). Policy: EdPerfPolicy.h.
//
// All of it runs on the main thread (the loop in WinApp.cpp, CD3DDevice::Paint and the UI Lua natives are the same thread), so the state needs no locks.
//
// Console variables (switch any of them off with 0, the limiter as a whole with `ed_fps_throttle 0`):
//   ed_fps_throttle   1      the limiter at all
//   ed_fps_limit      300    frames per second while the editor is in use
//   ed_fps_loading    60     while a map loads
//   ed_fps_background 60     while another program has the focus
//   ed_fps_idle       0      after `ed_idle_seconds` without input (power saving; off)
//   ed_idle_seconds   10
//   ed_present_wait   1      DeviceD3D9::Present waits for the GPU in short sleeps instead of a spin that keeps a core at 100 percent (EdPerf.cpp, PresentSleep)
//   ed_par_normalmap  1      big normal map updates (map load, whole map edits) use the worker pool of the field engine (CWldMap.cpp, UpdateNormalMap)
//   ed_ui_gc_kb       8192   the UI state's forced full GC (every 5 s) runs only after this much new Lua heap, or after ed_gc_max_seconds; 0 = always (the original)
//   ed_sim_gc_kb      8192   the same for the sim state (every 70 ticks)
//   ed_gc_max_seconds 60
// Lua natives:
//   ed_perf_loop([reset])  -> { fps, frameMs, maxFrameMs, mainMs, dispatchMs, waitMs, idleMs, presentMs, renderMs, cap, state, vsync, frames }
//   ed_perf_threads()      -> { { id, name, cpu, totalMs, main }, ... }  cpu = percent of one core since the last call
#pragma once

#include <cstdint>

namespace moho::editor::perf
{
  /** Nanoseconds of a steady clock. */
  [[nodiscard]] std::int64_t NowNs() noexcept;

  /**
   * Called by the main loop right before a frame (`CScApp::Main`). Waits until the frame rate cap allows the frame. Returns true when a window message arrived during the
   * wait: the caller dispatches it first and calls again, so input is never held back by the limiter. `mainWindow` is the HWND of the editor window (may be null).
   */
  [[nodiscard]] bool ThrottleBeforeFrame(void* mainWindow);

  /** Brackets `CScApp::Main` (the frame's logic and the invalidate that causes the paint). */
  void FrameBegin() noexcept;
  void FrameEnd() noexcept;

  /** `ed_par_normalmap` (default 1): CWldTerrainRes::UpdateNormalMap splits big updates over the field engine's worker pool. */
  [[nodiscard]] bool ParallelNormalMapEnabled() noexcept;

  /**
   * The forced full Lua collections (`CUIManager::Frame` every 5 s of frame time on the main thread, `Sim::AdvanceBeat` every 70 ticks on the Sim thread): `Due` says whether
   * this one is worth its pause (heap grew by `ed_ui_gc_kb` / `ed_sim_gc_kb` KiB since the last one, or `ed_gc_max_seconds` passed), `Done` is called after the collection with the
   * heap size. See GcGate in EdPerfPolicy.h. `heapKb` = `lua_getgccount(state)`.
   */
  [[nodiscard]] bool UiGcDue(int heapKb, float dtSeconds) noexcept;
  void UiGcDone(int heapKb) noexcept;
  [[nodiscard]] bool SimGcDue(int heapKb, float dtSeconds) noexcept;
  void SimGcDone(int heapKb) noexcept;

  /** Time the loop spent blocked in the OS wait after the frame (nothing to do). */
  void NoteIdleWait(std::int64_t startNs) noexcept;
  /** `device->Present()` and the render pass of `CD3DDevice::Paint`. */
  void NotePresent(std::int64_t startNs) noexcept;
  void NoteRender(std::int64_t startNs) noexcept;
} // namespace moho::editor::perf
