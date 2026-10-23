// EDITOR: main loop frame pacing and CPU statistics, see EdPerf.h and EdPerfPolicy.h.
//
// Thread model: everything here runs on the main thread (the wx loop in WinApp.cpp, CD3DDevice::Paint, the UI Lua state). No D3D, no Lua and no engine state is touched from
// any other thread; the one worker-free trick is a waitable timer the main thread sleeps on.

#include "moho/editor/EdPerf.h"
#include "moho/editor/EdPerfPolicy.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gpg/gal/Device.hpp"
#include "gpg/gal/DeviceContext.hpp"
#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/console/CConCommand.h"
#include "moho/core/Thread.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/sim/CWldSession.h"

namespace gpg::gal
{
  extern bool (*gPresentWaitSleep)();   // D3D9Interfaces.cpp
}

namespace moho::editor::perf
{
  namespace
  {
    // ---- console variables ---------------------------------------------------------------------------------------------------------------------

    bool gThrottleOn = true;
    float gFpsLimit = 300.0f;
    float gFpsLoading = 60.0f;
    float gFpsBackground = 60.0f;
    float gFpsIdle = 0.0f;
    float gIdleSeconds = 10.0f;

    TConVar<bool> gConThrottle("ed_fps_throttle", "Editor: 1 = the frame rate limiter of the main loop is on (ed_fps_limit, ed_fps_loading, ed_fps_background, ed_fps_idle)", &gThrottleOn);
    TConVar<float> gConLimit("ed_fps_limit", "Editor: frames per second cap while the editor is in use, 0 = none (default 300)", &gFpsLimit);
    TConVar<float> gConLoading("ed_fps_loading", "Editor: frames per second cap while a map loads, 0 = none (default 60)", &gFpsLoading);
    TConVar<float> gConBackground("ed_fps_background", "Editor: frames per second cap while another program has the focus, 0 = none (default 60)", &gFpsBackground);
    TConVar<float> gConIdle("ed_fps_idle", "Editor: frames per second cap after ed_idle_seconds without keyboard or mouse input, 0 = none (default 0)", &gFpsIdle);
    bool gPresentWait = true;
    TConVar<bool> gConPresentWait("ed_present_wait", "Editor: 1 = Present waits for the GPU in short sleeps instead of a spin that keeps a core at 100 percent (default 1)", &gPresentWait);
    bool gParallelNormalMap = true;
    TConVar<bool> gConParNormalMap("ed_par_normalmap", "Editor: 1 = big normal map updates (map load, whole map edits) use the worker pool (default 1)", &gParallelNormalMap);
    float gUiGcKb = 8192.0f;
    float gSimGcKb = 8192.0f;
    float gGcMaxSeconds = 60.0f;
    TConVar<float> gConUiGc("ed_ui_gc_kb", "Editor: the UI Lua state's forced full collection (every 5 s) runs only after this many KiB of new heap, 0 = always (default 8192)", &gUiGcKb);
    TConVar<float> gConSimGc("ed_sim_gc_kb", "Editor: the sim Lua state's forced full collection (every 7 s) runs only after this many KiB of new heap, 0 = always (default 8192)", &gSimGcKb);
    TConVar<float> gConGcMax("ed_gc_max_seconds", "Editor: longest time between two forced Lua collections while the heap does not grow (default 60)", &gGcMaxSeconds);
    GcGate gUiGate;
    GcGate gSimGate;
    TConVar<float> gConIdleSeconds("ed_idle_seconds", "Editor: seconds without input before ed_fps_idle applies (default 10)", &gIdleSeconds);

    // ---- clock ----------------------------------------------------------------------------------------------------------------------------------

    std::int64_t Elapsed(const std::int64_t startNs) noexcept
    {
      return NowNs() - startNs;
    }

    // ---- what the system says (sampled a few times a second, not every frame) -------------------------------------------------------------------

    struct Sampled
    {
      std::int64_t at = 0;
      bool foreground = true;
      double idleSeconds = 0.0;
    } gSampled;

    constexpr std::int64_t kSampleIntervalNs = 200 * 1000 * 1000;

    void RefreshSampled(const std::int64_t now) noexcept
    {
      if (gSampled.at != 0 && now - gSampled.at < kSampleIntervalNs) {
        return;
      }
      gSampled.at = now;

      // The editor owns the foreground window when any of its windows (main window, pop-outs, dialogs) does.
      HWND const fg = ::GetForegroundWindow();
      DWORD pid = 0;
      if (fg != nullptr) {
        (void)::GetWindowThreadProcessId(fg, &pid);
      }
      gSampled.foreground = fg != nullptr && pid == ::GetCurrentProcessId();

      LASTINPUTINFO info{};
      info.cbSize = sizeof(info);
      gSampled.idleSeconds = ::GetLastInputInfo(&info) != FALSE ? static_cast<double>(::GetTickCount() - info.dwTime) / 1000.0 : 0.0;
    }

    // ---- the wait --------------------------------------------------------------------------------------------------------------------------------

    /**
     * Sleeps without a core: a high resolution waitable timer for all but the last fraction of a millisecond, a short spin for the rest (a plain Sleep wakes up to 15 ms late,
     * which at 300 frames per second would be a visible stutter). Without high resolution timers (before Windows 10 1803) the timer resolution is raised to 1 ms instead.
     */
    class Waiter
    {
    public:
      /** Waits until `untilNs`. With `interruptible` any new window message ends the wait early and true is returned. */
      bool Wait(const std::int64_t untilNs, const bool interruptible)
      {
        Init();
        const std::int64_t lead = highRes_ ? 600 * 1000 : 1500 * 1000;
        const std::int64_t coarse = untilNs - lead - NowNs();
        if (timer_ != nullptr && coarse > 200 * 1000) {
          LARGE_INTEGER due;
          due.QuadPart = -(coarse / 100);   // relative, 100 ns units
          if (::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
            const DWORD result = interruptible ? ::MsgWaitForMultipleObjectsEx(1, &timer_, INFINITE, QS_ALLINPUT, 0) : ::WaitForSingleObject(timer_, INFINITE);
            if (result != WAIT_OBJECT_0) {
              (void)::CancelWaitableTimer(timer_);
              return interruptible && result == WAIT_OBJECT_0 + 1;
            }
          }
        }
        while (NowNs() < untilNs) {
          ::YieldProcessor();
        }
        return false;
      }

    private:
      void Init()
      {
        if (inited_) {
          return;
        }
        inited_ = true;
        HMODULE const kernel = ::GetModuleHandleW(L"kernel32.dll");
        using CreateExFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        const auto createEx = kernel != nullptr ? reinterpret_cast<CreateExFn>(reinterpret_cast<void*>(::GetProcAddress(kernel, "CreateWaitableTimerExW"))) : nullptr;
        if (createEx != nullptr) {
          timer_ = createEx(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, 0x1F0003 /* TIMER_ALL_ACCESS */);
        }
        highRes_ = timer_ != nullptr;
        if (timer_ == nullptr) {
          timer_ = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
          if (HMODULE const winmm = ::LoadLibraryW(L"winmm.dll"); winmm != nullptr) {
            using BeginFn = UINT(WINAPI*)(UINT);
            if (const auto begin = reinterpret_cast<BeginFn>(reinterpret_cast<void*>(::GetProcAddress(winmm, "timeBeginPeriod"))); begin != nullptr) {
              (void)begin(1);
            }
          }
        }
      }

      HANDLE timer_ = nullptr;
      bool highRes_ = false;
      bool inited_ = false;
    } gWaiter;

    Pacer gPacer;
    int gInterrupts = 0;   // wait interruptions since the last frame: bounded so a stuck message can never keep the frame from running
    constexpr int kMaxInterrupts = 3;
    float gLastCap = 0.0f;
    const char* gLastState = "active";

    // ---- statistics ------------------------------------------------------------------------------------------------------------------------------

    struct Window
    {
      std::int64_t startNs = 0;
      int frames = 0;
      double mainNs = 0;
      double waitNs = 0;
      double idleNs = 0;
      double presentNs = 0;
      double renderNs = 0;
      double intervalNs = 0;
      double maxIntervalNs = 0;
      int intervals = 0;
    };
    Window gAcc;
    struct Published
    {
      std::int64_t atNs = 0;
      double fps = 0, frameMs = 0, maxFrameMs = 0, mainMs = 0, dispatchMs = 0, waitMs = 0, idleMs = 0, presentMs = 0, renderMs = 0;
      long frames = 0;
    } gLast;
    double gRunningMaxMs = 0;
    std::int64_t gLastBeginNs = 0;
    std::int64_t gMainStartNs = 0;
    long gTotalFrames = 0;

    void Publish(const std::int64_t now)
    {
      const double seconds = static_cast<double>(now - gAcc.startNs) / 1e9;
      if (gAcc.frames > 0 && seconds > 0.0) {
        const double n = static_cast<double>(gAcc.frames);
        const double ms = 1.0 / 1e6;
        gLast.atNs = now;
        gLast.frames = gTotalFrames;
        gLast.fps = n / seconds;
        gLast.mainMs = gAcc.mainNs / n * ms;
        gLast.waitMs = gAcc.waitNs / n * ms;
        gLast.idleMs = gAcc.idleNs / n * ms;
        gLast.presentMs = gAcc.presentNs / n * ms;
        gLast.renderMs = gAcc.renderNs / n * ms;
        gLast.frameMs = gAcc.intervals > 0 ? gAcc.intervalNs / static_cast<double>(gAcc.intervals) * ms : 0.0;
        gLast.maxFrameMs = gAcc.maxIntervalNs * ms;
        // everything between two frames that is not the frame's own logic, a wait or the OS wait: message handling, the paint with its Present and the render pass
        gLast.dispatchMs = std::max(0.0, gLast.frameMs - gLast.mainMs - gLast.waitMs - gLast.idleMs);
        gRunningMaxMs = std::max(gRunningMaxMs, gLast.maxFrameMs);
      }
      gAcc = Window{};
      gAcc.startNs = now;
    }
  } // namespace

  std::int64_t NowNs() noexcept
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  namespace
  {
    /**
     * `DeviceD3D9::Present` (gpg/gal/backends/d3d9/D3D9Interfaces.cpp) waits for the GPU to reach the frame's start marker (an event query) before it presents the frame. The
     * original polls with a flush flag as fast as it can: a core at 100 percent for as long as the GPU is behind (a frame, or the vsync wait of the one before it). The library
     * calls this hook after 100 microseconds of spinning: it sleeps about 0.3 ms on a high resolution timer and the poll goes on. Returning false (switched off with
     * `ed_present_wait 0`, or no high resolution timer: before Windows 10 1803) keeps the original loop.
     */
    bool PresentSleep()
    {
      if (!gPresentWait) {
        return false;
      }
      // One high resolution timer per thread (Present is the main thread's, but nothing here assumes it).
      thread_local HANDLE timer = [] {
        HMODULE const kernel = ::GetModuleHandleW(L"kernel32.dll");
        using CreateExFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        const auto createEx = kernel != nullptr ? reinterpret_cast<CreateExFn>(reinterpret_cast<void*>(::GetProcAddress(kernel, "CreateWaitableTimerExW"))) : nullptr;
        return createEx != nullptr ? createEx(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, 0x1F0003 /* TIMER_ALL_ACCESS */) : HANDLE(nullptr);
      }();
      if (timer == nullptr) {
        return false;
      }
      LARGE_INTEGER due;
      due.QuadPart = -3000;   // 0.3 ms, relative, 100 ns units
      if (::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) == FALSE) {
        return false;
      }
      (void)::WaitForSingleObject(timer, 100);   // the 100 ms is a guard, the timer fires after about 0.3 to 1 ms
      return true;
    }

    struct PresentSleepInstaller
    {
      PresentSleepInstaller() { gpg::gal::gPresentWaitSleep = &PresentSleep; }
    };
    const PresentSleepInstaller gPresentSleepInstaller{};
  } // namespace

  bool UiGcDue(const int heapKb, const float dtSeconds) noexcept
  {
    return gUiGate.Due(heapKb, dtSeconds, static_cast<long>(gUiGcKb), gGcMaxSeconds);
  }

  void UiGcDone(const int heapKb) noexcept
  {
    gUiGate.Done(heapKb);
  }

  bool SimGcDue(const int heapKb, const float dtSeconds) noexcept
  {
    return gSimGate.Due(heapKb, dtSeconds, static_cast<long>(gSimGcKb), gGcMaxSeconds);
  }

  void SimGcDone(const int heapKb) noexcept
  {
    gSimGate.Done(heapKb);
  }

  bool ParallelNormalMapEnabled() noexcept
  {
    return gParallelNormalMap;
  }

  bool ThrottleBeforeFrame(void* const mainWindow)
  {
    if (!gThrottleOn) {
      gPacer.Reset();
      gLastCap = 0.0f;
      return false;
    }

    const std::int64_t now = NowNs();
    RefreshSampled(now);

    Situation s;
    s.minimized = mainWindow != nullptr && ::IsIconic(static_cast<HWND>(mainWindow)) != FALSE;
    s.foreground = gSampled.foreground;
    s.idleSeconds = gSampled.idleSeconds;
    const EWldFrameAction action = WLD_GetFrameAction();
    s.loading = action == EWldFrameAction::Preload || action == EWldFrameAction::Loading;

    Caps caps;
    caps.active = gFpsLimit;
    caps.loading = gFpsLoading;
    caps.background = gFpsBackground;
    caps.idle = gFpsIdle;
    caps.idleSeconds = gIdleSeconds;
    const float cap = EffectiveCap(caps, s);
    gLastCap = cap;
    gLastState = StateName(s);

    const std::int64_t remaining = gPacer.Remaining(now, cap);
    if (remaining > 0) {
      const bool arrived = gWaiter.Wait(now + remaining, gInterrupts < kMaxInterrupts);
      gAcc.waitNs += static_cast<double>(Elapsed(now));
      if (arrived) {
        ++gInterrupts;
        return true;   // input first; the loop calls again and the remaining time is recomputed
      }
    }
    gInterrupts = 0;
    gPacer.FrameStarted(NowNs(), cap);
    return false;
  }

  void FrameBegin() noexcept
  {
    const std::int64_t now = NowNs();
    if (gAcc.startNs == 0) {
      gAcc.startNs = now;
    }
    if (gLastBeginNs != 0) {
      const double interval = static_cast<double>(now - gLastBeginNs);
      gAcc.intervalNs += interval;
      gAcc.maxIntervalNs = std::max(gAcc.maxIntervalNs, interval);
      ++gAcc.intervals;
    }
    gLastBeginNs = now;
    gMainStartNs = now;
    ++gAcc.frames;
    ++gTotalFrames;
  }

  void FrameEnd() noexcept
  {
    const std::int64_t now = NowNs();
    gAcc.mainNs += static_cast<double>(now - gMainStartNs);
    if (now - gAcc.startNs >= 1000 * 1000 * 1000) {
      Publish(now);
    }
  }

  void NoteIdleWait(const std::int64_t startNs) noexcept
  {
    gAcc.idleNs += static_cast<double>(Elapsed(startNs));
  }

  void NotePresent(const std::int64_t startNs) noexcept
  {
    gAcc.presentNs += static_cast<double>(Elapsed(startNs));
  }

  void NoteRender(const std::int64_t startNs) noexcept
  {
    gAcc.renderNs += static_cast<double>(Elapsed(startNs));
  }

  // ---- Lua -------------------------------------------------------------------------------------------------------------------------------------

  namespace
  {
    void SetNumber(lua_State* const L, const char* const key, const double value)
    {
      lua_pushstring(L, key);
      lua_pushnumber(L, value);
      lua_settable(L, -3);
    }

    void SetString(lua_State* const L, const char* const key, const char* const value)
    {
      lua_pushstring(L, key);
      lua_pushstring(L, value);
      lua_settable(L, -3);
    }

    void SetBool(lua_State* const L, const char* const key, const bool value)
    {
      lua_pushstring(L, key);
      lua_pushboolean(L, value ? 1 : 0);
      lua_settable(L, -3);
    }

    // ed_perf_loop([reset]) -> { fps, frameMs, maxFrameMs, mainMs, dispatchMs, waitMs, idleMs, presentMs, renderMs, cap, state, vsync, frames }
    //   Averages per frame over the last full second (zeros while the loop did not run for two seconds: a minimized window). mainMs = the frame's logic (sim sync, UI Lua,
    //   camera, the invalidate); dispatchMs = message handling + paint (Present + render submit, below); waitMs = what the limiter slept; idleMs = OS wait after the frame;
    //   presentMs = time inside Present (the vsync wait); renderMs = CPU time recording the frame. maxFrameMs = the longest frame start to frame start since the last reset.
    //   cap = frames per second cap now (0 = none), state = active | loading | background | minimized.
    int __cdecl cfunc_EdPerfLoop(lua_State* const L)
    {
      if (lua_toboolean(L, 1) != 0) {
        gRunningMaxMs = 0;
      }
      const bool fresh = gLast.atNs != 0 && NowNs() - gLast.atNs < 2LL * 1000 * 1000 * 1000;
      lua_newtable(L);
      SetNumber(L, "fps", fresh ? gLast.fps : 0.0);
      SetNumber(L, "frameMs", fresh ? gLast.frameMs : 0.0);
      SetNumber(L, "maxFrameMs", gRunningMaxMs);
      SetNumber(L, "mainMs", fresh ? gLast.mainMs : 0.0);
      SetNumber(L, "dispatchMs", fresh ? gLast.dispatchMs : 0.0);
      SetNumber(L, "waitMs", fresh ? gLast.waitMs : 0.0);
      SetNumber(L, "idleMs", fresh ? gLast.idleMs : 0.0);
      SetNumber(L, "presentMs", fresh ? gLast.presentMs : 0.0);
      SetNumber(L, "renderMs", fresh ? gLast.renderMs : 0.0);
      SetNumber(L, "cap", gLastCap);
      SetString(L, "state", gLastState);
      bool vsync = false;
      if (gpg::gal::Device::IsReady()) {
        const gpg::gal::DeviceContext* const context = gpg::gal::Device::GetInstance()->GetDeviceContext();
        vsync = context != nullptr && context->mVSync;
      }
      SetBool(L, "vsync", vsync);
      SetNumber(L, "frames", static_cast<double>(gTotalFrames));
      return 1;
    }

    struct ThreadRow
    {
      DWORD id = 0;
      double cpu = 0;
      double totalMs = 0;
      std::string name;
    };

    std::map<DWORD, ULONGLONG> gPrevTimes;   // thread id -> user + kernel time (100 ns) at the last ed_perf_threads
    std::int64_t gPrevThreadsAtNs = 0;

    std::string ThreadName(HANDLE const thread)
    {
      using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
      static const GetDescFn getDesc = [] {
        HMODULE const kernel = ::GetModuleHandleW(L"kernel32.dll");
        return kernel != nullptr ? reinterpret_cast<GetDescFn>(reinterpret_cast<void*>(::GetProcAddress(kernel, "GetThreadDescription"))) : nullptr;
      }();
      if (getDesc == nullptr) {
        return std::string();
      }
      PWSTR wide = nullptr;
      if (FAILED(getDesc(thread, &wide)) || wide == nullptr) {
        return std::string();
      }
      std::string out;
      const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
      if (bytes > 1) {
        out.resize(static_cast<std::size_t>(bytes));
        (void)::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
        out.resize(static_cast<std::size_t>(bytes) - 1);
      }
      (void)::LocalFree(wide);
      return out;
    }

    // ed_perf_threads() -> { { id, name, cpu, totalMs, main }, ... } sorted by cpu (at most 32).
    //   cpu = percent of one core the thread used since the previous call (the first call has no interval and reports 0), totalMs = CPU time since the thread started.
    //   Names come from SetThreadDescription (the engine names its threads: Sim, Issue, Map loader ...); the editor's field workers have none.
    int __cdecl cfunc_EdPerfThreads(lua_State* const L)
    {
      const std::int64_t now = NowNs();
      const double wallTicks = gPrevThreadsAtNs != 0 ? static_cast<double>(now - gPrevThreadsAtNs) / 100.0 : 0.0;   // 100 ns units
      std::vector<ThreadRow> rows;
      std::map<DWORD, ULONGLONG> times;

      HANDLE const snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD pid = ::GetCurrentProcessId();
        for (BOOL ok = ::Thread32First(snapshot, &entry); ok != FALSE; ok = ::Thread32Next(snapshot, &entry)) {
          if (entry.th32OwnerProcessID != pid) {
            continue;
          }
          HANDLE const thread = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
          if (thread == nullptr) {
            continue;
          }
          FILETIME created{}, exited{}, kernel{}, user{};
          if (::GetThreadTimes(thread, &created, &exited, &kernel, &user) != FALSE) {
            ULARGE_INTEGER k{}, u{};
            k.LowPart = kernel.dwLowDateTime;
            k.HighPart = kernel.dwHighDateTime;
            u.LowPart = user.dwLowDateTime;
            u.HighPart = user.dwHighDateTime;
            const ULONGLONG total = k.QuadPart + u.QuadPart;
            ThreadRow row;
            row.id = entry.th32ThreadID;
            row.totalMs = static_cast<double>(total) / 10000.0;
            const auto prev = gPrevTimes.find(row.id);
            row.cpu = (prev != gPrevTimes.end() && wallTicks > 0.0) ? static_cast<double>(total - prev->second) / wallTicks * 100.0 : 0.0;
            row.name = ThreadName(thread);
            times[row.id] = total;
            rows.push_back(std::move(row));
          }
          (void)::CloseHandle(thread);
        }
        (void)::CloseHandle(snapshot);
      }
      gPrevTimes.swap(times);
      gPrevThreadsAtNs = now;

      std::sort(rows.begin(), rows.end(), [](const ThreadRow& a, const ThreadRow& b) { return a.cpu > b.cpu || (a.cpu == b.cpu && a.totalMs > b.totalMs); });
      const DWORD mainId = static_cast<DWORD>(THREAD_GetMainThreadId());
      lua_newtable(L);
      int index = 0;
      for (const ThreadRow& row : rows) {
        if (index >= 32) {
          break;
        }
        lua_pushnumber(L, static_cast<lua_Number>(++index));
        lua_newtable(L);
        SetNumber(L, "id", static_cast<double>(row.id));
        SetString(L, "name", row.name.c_str());
        SetNumber(L, "cpu", row.cpu);
        SetNumber(L, "totalMs", row.totalMs);
        SetBool(L, "main", row.id == mainId);
        lua_settable(L, -3);
      }
      return 1;
    }

    [[nodiscard]] CScrLuaInitFormSet& UserLuaInitSet()
    {
      if (CScrLuaInitFormSet* const set = SCR_FindLuaInitFormSet("User"); set != nullptr) {
        return *set;
      }
      static CScrLuaInitFormSet fallbackSet("User");
      return fallbackSet;
    }

    struct EdPerfLuaBootstrap
    {
      EdPerfLuaBootstrap()
      {
        auto& set = UserLuaInitSet();
        (void)new CScrLuaBinder(set, "ed_perf_loop", &cfunc_EdPerfLoop, nullptr, "<global>",
                                "ed_perf_loop([reset]) -> { fps, frameMs, maxFrameMs, mainMs, dispatchMs, waitMs, idleMs, presentMs, renderMs, cap, state, vsync, frames }");
        (void)new CScrLuaBinder(set, "ed_perf_threads", &cfunc_EdPerfThreads, nullptr, "<global>",
                                "ed_perf_threads() -> { { id, name, cpu, totalMs, main }, ... } sorted by cpu (percent of one core since the last call)");
      }
    };

    const EdPerfLuaBootstrap gEdPerfLuaBootstrap{};
  } // namespace
} // namespace moho::editor::perf
