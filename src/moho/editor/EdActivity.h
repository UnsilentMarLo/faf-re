// EDITOR: "is the picture the same as the last one?" - the signals a frame pacer (EdPerf.cpp / EdPerfPolicy.h) can use to draw less while nothing happens, without taking a frame
// away from the user while something does. Engine independent logic, tested on its own (moho/editor/tests/EdRenderOptTest.cpp); the natives are in EdActivity.cpp:
//
//   ed_scene_dirty([consume])   -> epoch, reasons, ambient      the scene epoch (grows with every change), the reasons that were raised since the last call that consumed them
//                                                                (consume defaults to true), the ambient animation bits
//   ed_scene_activity()         -> table { idleMs, focused, sinceChangeMs, inputs }
//   ed_scene_touch([reasons])   -> epoch                        Lua marks a change the natives cannot see (a tool, the history, a panel that changes the picture)
//
// What makes the scene dirty (`Reason`): the world camera moved or its viewport changed (checked when ed_scene_dirty is called), the native overlay changed (every ed_overlay_*
// setter), the height field changed (NotifyHeights), Lua touched it. Not a reason: the ambient animation (water, clouds, particles): it is reported in `ambient` instead, because
// it never stops, and a still scene with moving water still has to be drawn - but 30 times a second is as smooth as it gets, drawing it at 300 is waste.
//
// Proposed use by the frame pacer (not applied here, the main loop belongs to EdPerf): while `RecommendedCapFps` is > 0 the pacer may wait that long between frames; any input,
// dirty reason or a pending Lua touch makes it 0 (no cap beyond the user's ed_fps_limit) on the next frame, and the pacer's wait is interrupted by any window message anyway.
#pragma once

#include <cstdint>

namespace moho::editor::activity
{
  enum Reason : std::uint32_t
  {
    kCamera = 1,    // the world camera moved / its viewport changed
    kOverlay = 2,   // a native overlay changed (lines, icons, decals, slope view, grids, fields)
    kHeights = 4,   // the height field changed
    kScript = 8,    // Lua called ed_scene_touch
  };

  enum Ambient : std::uint32_t
  {
    kAmbientWater = 1,   // the map has water: the surface moves
    kAmbientSky = 2,     // the sky dome is drawn: the clouds drift
    kAmbientSim = 4,     // the simulation ticks (units, effects)
  };

  /** Epoch and the reasons raised since they were last taken. */
  class Tracker
  {
  public:
    void Touch(const std::uint32_t reasons) noexcept
    {
      if (reasons != 0u) {
        pending_ |= reasons;
        ++epoch_;
      }
    }

    [[nodiscard]] std::uint32_t Epoch() const noexcept { return epoch_; }

    /** The reasons raised since the last Take; clears them when `consume`. */
    std::uint32_t Take(const bool consume) noexcept
    {
      const std::uint32_t reasons = pending_;
      if (consume) {
        pending_ = 0u;
      }
      return reasons;
    }

  private:
    std::uint32_t epoch_ = 0;
    std::uint32_t pending_ = 0;
  };

  /** Milliseconds from `then` to `now` of a 32 bit millisecond clock (GetTickCount): right across the wrap every 49.7 days. */
  [[nodiscard]] constexpr std::uint32_t ElapsedMs(const std::uint32_t now, const std::uint32_t then) noexcept
  {
    return now - then;
  }

  /**
   * How long the pacer may wait between two frames, in milliseconds, 0 = not at all. `holdMs`: how long nothing may have happened before the picture counts as still (default
   * 2000: a user who stops to think and moves on gets the full rate back with the first mouse move, which interrupts the wait).
   * Still scene, no ambient animation: one frame in 100 ms (the picture does not change, the frame only keeps the window alive for the compositor); with ambient animation
   * (water, clouds): 33 ms. A scene that is not still, or a simulation that ticks: 0.
   */
  [[nodiscard]] constexpr std::uint32_t RecommendedWaitMs(const std::uint32_t idleMs, const std::uint32_t sinceChangeMs, const std::uint32_t ambient, const bool pendingChange,
                                                          const std::uint32_t holdMs) noexcept
  {
    if (pendingChange || (ambient & kAmbientSim) != 0u || idleMs < holdMs || sinceChangeMs < holdMs) {
      return 0u;   // a running simulation moves units and effects: the full rate
    }
    return ambient != 0u ? 33u : 100u;
  }

  /** Raised by the natives of the editor (the overlay setters, NotifyHeights); read by ed_scene_dirty. */
  void Touch(std::uint32_t reasons) noexcept;
} // namespace moho::editor::activity
