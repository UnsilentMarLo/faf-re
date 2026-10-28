#include "moho/render/FrameLimiter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <windows.h>

#include "gpg/core/utils/Logging.h"
#include "moho/console/CConCommand.h"
#include "moho/misc/StartupHelpers.h"
#include "moho/render/RenderFeatures.h"

namespace moho
{
  int ren_FpsLimit = 0;
  float ren_MaxFps = 0.0f;

  namespace
  {
    // The levels of the Options entry, frames per second; index 0 is off.
    constexpr float kLevelFps[] = {0.0f, 30.0f, 60.0f, 90.0f, 120.0f, 144.0f, 165.0f, 200.0f, 240.0f};
    constexpr int kLevelCount = static_cast<int>(sizeof(kLevelFps) / sizeof(kLevelFps[0]));

    TConVar<int> gTConVar_ren_FpsLimit(
      "ren_FpsLimit",
      "Frame rate limit: the level of the Options > Video entry. 0 = off (the default), 1 = 30, 2 = 60, 3 = 90, 4 = 120, 5 = 144, 6 = 165, 7 = 200, "
      "8 = 240 frames per second. See ren_MaxFps for an exact value.",
      &ren_FpsLimit
    );

    TConVar<float> gTConVar_ren_MaxFps(
      "ren_MaxFps",
      "Frame rate limit in frames per second, any value (above 0 it wins over ren_FpsLimit); 0 = use ren_FpsLimit. Also /maxfps N on the command "
      "line. With vsync on the display's refresh rate paces the frames too: pick a limit that divides it (a quarter, a half) or switch vsync off.",
      &ren_MaxFps
    );

    // The Options > Video entry (render features registry). Pure CPU pacing, so every API supports it; applies at once.
    const bool gRegisteredFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "fps_limit";
      feature.title = "Frame Rate Limit";
      feature.tip = "Caps the frames per second the game draws. A cap saves power, heat and fan noise, and avoids the coil whine some graphics cards "
                    "make when they run at several hundred frames per second. With vsync on, a limit that divides the screen's refresh rate (a half or "
                    "a quarter) keeps the frames evenly spaced. Render only, never affects the game. Takes effect at once.";
      feature.console = "ren_FpsLimit";
      feature.states = {{"Off", "off"}, {"30 fps", "fps30"}, {"60 fps", "fps60"}, {"90 fps", "fps90"}, {"120 fps", "fps120"},
                        {"144 fps", "fps144"}, {"165 fps", "fps165"}, {"200 fps", "fps200"}, {"240 fps", "fps240"}};
      feature.defaultLevel = 0;
      feature.needsRestart = false;
      feature.apis[static_cast<int>(Api::D3D9)].supported = true;
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) { ren_FpsLimit = level; };
      render::RegisterFeature(feature);
      return true;
    }();

    void ApplyCommandLine()
    {
      static bool applied = false;
      if (applied) {
        return;
      }
      applied = true;
      msvc8::vector<msvc8::string> args;
      if (!CFG_GetArgOption("/maxfps", 1U, &args) || args.empty()) {
        return;
      }
      char* end = nullptr;
      const double value = std::strtod(args[0].c_str(), &end);
      if (end == args[0].c_str() || !std::isfinite(value) || value < 0.0) {
        gpg::Warnf("/maxfps %s is not a number of frames per second; the frame rate limit stays as it was", args[0].c_str());
        return;
      }
      ren_MaxFps = static_cast<float>(value);
    }

    std::int64_t Frequency()
    {
      static const std::int64_t frequency = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return std::max<std::int64_t>(1, f.QuadPart);
      }();
      return frequency;
    }

    std::int64_t Now()
    {
      LARGE_INTEGER t;
      ::QueryPerformanceCounter(&t);
      return t.QuadPart;
    }

    // A timer that wakes within about half a millisecond (Windows 10 1803 and later); 0 when the system has none.
    HANDLE HighResolutionTimer()
    {
      static HANDLE timer = [] {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        constexpr DWORD kHighResolution = 0x00000002;
#else
        constexpr DWORD kHighResolution = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
#endif
        return ::CreateWaitableTimerExW(nullptr, nullptr, kHighResolution, TIMER_ALL_ACCESS);
      }();
      return timer;
    }

    // Sleeps until `target` (a QueryPerformanceCounter value): a timer for the bulk, a short spin for the last stretch so the frame is not late.
    void WaitUntil(const std::int64_t target)
    {
      const std::int64_t spinTicks = Frequency() / 2500; // 0.4 ms
      std::int64_t now = Now();
      if (target - now > spinTicks) {
        const std::int64_t sleepTicks = target - now - spinTicks;
        if (HANDLE timer = HighResolutionTimer(); timer != nullptr) {
          LARGE_INTEGER due;
          due.QuadPart = -(sleepTicks * 10000000LL / Frequency()); // 100 ns units, negative = relative
          if (due.QuadPart < 0 && ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            ::WaitForSingleObject(timer, INFINITE);
          }
        } else {
          const std::int64_t milliseconds = sleepTicks * 1000 / Frequency();
          if (milliseconds > 1) {
            ::Sleep(static_cast<DWORD>(milliseconds - 1));
          }
        }
        now = Now();
      }
      while (now < target) {
        ::YieldProcessor();
        now = Now();
      }
    }
  } // namespace

  float FrameLimitFps()
  {
    ApplyCommandLine();
    if (ren_MaxFps > 0.0f && std::isfinite(ren_MaxFps)) {
      return std::min(ren_MaxFps, 10000.0f);
    }
    const int level = std::clamp(ren_FpsLimit, 0, kLevelCount - 1);
    return kLevelFps[level];
  }

  void FrameLimiterWait()
  {
    static std::int64_t next = 0; // the due time of the frame that just ended
    static float lastFps = 0.0f;

    const float fps = FrameLimitFps();
    if (!(fps > 0.0f)) {
      next = 0;
      lastFps = 0.0f;
      return;
    }
    const std::int64_t now = Now();
    if (next == 0 || fps != lastFps) {
      lastFps = fps;
      next = now; // a new limit starts a new grid; this frame is on time
      return;
    }
    const std::int64_t interval = std::max<std::int64_t>(1, static_cast<std::int64_t>(static_cast<double>(Frequency()) / fps));
    next += interval;
    if (now >= next) {
      if (now - next > interval) {
        next = now; // more than a frame late (loading, a stall): restart the grid, do not race to catch up
      }
      return;
    }
    WaitUntil(next);
  }
} // namespace moho
