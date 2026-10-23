// EDITOR: frame pacing policy of the editor's main loop, engine independent so it is tested on its own (moho/editor/tests/EdPerfPolicyTest.cpp).
//
// Why a limiter at all (docs/projects/editor/PERFORMANCE.md, section "Engine CPU"): the recovered main loop (WinApp.cpp, `CScApp::Main`) invalidates the window at the end of
// every iteration (`CD3DDevice::Refresh`), the WM_PAINT it causes presents the previous frame and renders the next one (`CD3DDevice::Paint`), and nothing in the loop sleeps
// between two frames: the only pacing is the swap chain's vertical sync (`context.mVSync`, PresentationInterval ONE). With vsync off, or while the driver spins inside Present,
// the main thread draws as fast as it can, a full core and a full GPU for a picture nobody can tell from one drawn 300 times a second.
//
// The rules (every number is a console variable, 0 switches a cap off):
//   * active    `ed_fps_limit`       default 300: far above any monitor, so it never shows; it only stops an unbounded loop.
//   * loading   `ed_fps_loading`     default 60 : while a map is being loaded the loader thread does the work, the main thread only draws the loading screen.
//   * background `ed_fps_background` default 60 : the editor window is not the foreground window of the system (the user works in another program).
//   * idle      `ed_fps_idle`        default 0 (off): no keyboard / mouse input anywhere for `ed_idle_seconds`. A power saving option; off by default because a test run
//                                    that is only watched, or a camera animation, is "idle" too and must not lose smoothness.
// The effective cap is the smallest non-zero cap that applies. Input never waits for a frame: the wait is interrupted by any message (see EdPerf.cpp).
#pragma once

#include <algorithm>
#include <cstdint>

namespace moho::editor::perf
{
  /** The caps in frames per second (0 = no cap of this kind) and the idle delay. */
  struct Caps
  {
    float active = 300.0f;
    float loading = 60.0f;
    float background = 60.0f;
    float idle = 0.0f;
    float idleSeconds = 10.0f;
  };

  /** What the main loop knows about the moment. */
  struct Situation
  {
    bool minimized = false;    // a minimized window is not drawn at all: the loop's own wake-up timer paces it
    bool foreground = true;    // the editor (any of its windows) is the foreground window
    bool loading = false;      // a map is being loaded (world session preload / load phase)
    double idleSeconds = 0.0;  // seconds since the last keyboard / mouse input of the whole system
  };

  /** Lowest cap anyone may ask for: a cap of 1 frame per second would make the editor look frozen. */
  constexpr float kMinCap = 5.0f;

  /** The frame rate cap that applies now, 0 = none. */
  [[nodiscard]] inline float EffectiveCap(const Caps& caps, const Situation& s) noexcept
  {
    if (s.minimized) {
      return 0.0f;
    }
    float cap = 0.0f;
    const auto apply = [&cap](const float v) {
      if (v > 0.0f) {
        const float clamped = std::max(v, kMinCap);
        if (cap <= 0.0f || clamped < cap) {
          cap = clamped;
        }
      }
    };
    apply(caps.active);
    if (s.loading) {
      apply(caps.loading);
    }
    if (!s.foreground) {
      apply(caps.background);
    }
    if (caps.idle > 0.0f && s.idleSeconds >= static_cast<double>(caps.idleSeconds)) {
      apply(caps.idle);
    }
    return cap;
  }

  /** Name of the state that decides the cap, for the statistics. */
  [[nodiscard]] inline const char* StateName(const Situation& s) noexcept
  {
    if (s.minimized) {
      return "minimized";
    }
    if (s.loading) {
      return "loading";
    }
    if (!s.foreground) {
      return "background";
    }
    return "active";
  }

  /**
   * Keeps a steady cadence without ever catching up: the start of a frame is scheduled `period` after the start of the one before; a frame that came late by less than a
   * period keeps the grid (no drift), a later one restarts it (no burst of frames to make up for a stall).
   * Times are in nanoseconds of any steady clock.
   */
  class Pacer
  {
  public:
    static constexpr std::int64_t kNsPerSecond = 1000000000;

    /** Nanoseconds to wait before the next frame may start; 0 = start now. `cap` <= 0 never waits. */
    [[nodiscard]] std::int64_t Remaining(const std::int64_t now, const float cap) const noexcept
    {
      if (cap <= 0.0f || !started_) {
        return 0;
      }
      const std::int64_t target = lastStart_ + Period(cap);
      return target > now ? target - now : 0;
    }

    /** A frame starts at `now` (call once per frame, after any wait). */
    void FrameStarted(const std::int64_t now, const float cap) noexcept
    {
      if (cap <= 0.0f || !started_) {
        lastStart_ = now;
        started_ = true;
        return;
      }
      const std::int64_t period = Period(cap);
      const std::int64_t target = lastStart_ + period;
      // on time or late by less than one period: keep the grid; early (an interrupted wait) or very late: restart it
      lastStart_ = (now >= target && now - target <= period) ? target : now;
    }

    void Reset() noexcept { started_ = false; }

    [[nodiscard]] static std::int64_t Period(const float cap) noexcept
    {
      return static_cast<std::int64_t>(static_cast<double>(kNsPerSecond) / static_cast<double>(std::max(cap, kMinCap)));
    }

  private:
    std::int64_t lastStart_ = 0;
    bool started_ = false;
  };
} // namespace moho::editor::perf
