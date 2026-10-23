// EDITOR: scene activity natives (see EdActivity.h for the contract and the proposed use by the frame pacer).
#include "moho/editor/EdActivity.h"

#include <cstring>

#include <Windows.h>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/render/RCamManager.h"
#include "moho/render/camera/CameraImpl.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"

namespace moho
{
  extern bool ren_SkyDome;
}

namespace moho::editor::activity
{
  namespace
  {
    Tracker gTracker;

    struct CameraStamp
    {
      bool valid = false;
      float viewProjection[16] = {};
      float viewport[16] = {};
    };
    CameraStamp gCameraStamp;
    std::int32_t gLastGameTick = -1;

    // input of the whole system as the OS reports it (GetLastInputInfo); `inputs` counts the changes seen
    std::uint32_t gLastInputTick = 0;
    std::uint32_t gInputs = 0;
    std::uint32_t gLastChangeTick = 0;
    bool gChangeTickSet = false;

    std::uint32_t NowTick()
    {
      return static_cast<std::uint32_t>(GetTickCount());
    }

    /** Every change goes through here: the tracker's epoch moves and the time of the change is kept (sinceChangeMs). */
    void TouchNow(const std::uint32_t reasons)
    {
      if (reasons != 0u) {
        gTracker.Touch(reasons);
        gLastChangeTick = NowTick();
        gChangeTickSet = true;
      }
    }

    /** Polls the OS for the last input time; returns the tick of the last input. */
    std::uint32_t PollInput()
    {
      LASTINPUTINFO info{};
      info.cbSize = sizeof(info);
      if (GetLastInputInfo(&info) != 0) {
        if (info.dwTime != gLastInputTick) {
          gLastInputTick = info.dwTime;
          ++gInputs;
        }
      }
      return gLastInputTick;
    }

    bool OurProcessHasForeground()
    {
      const HWND foreground = GetForegroundWindow();
      if (foreground == nullptr) {
        return false;
      }
      DWORD pid = 0;
      GetWindowThreadProcessId(foreground, &pid);
      return pid == GetCurrentProcessId();
    }

    /** Looks at the world camera; touches kCamera when it (or its viewport) differs from the last look. */
    void PollCamera()
    {
      RCamManager* const manager = CAM_GetManager();
      CameraImpl* const camera = manager != nullptr ? manager->GetCamera("WorldCamera") : nullptr;
      if (camera == nullptr) {
        return;
      }
      const GeomCamera3& view = camera->CameraGetView();
      CameraStamp now;
      now.valid = true;
      static_assert(sizeof(now.viewProjection) == sizeof(view.viewProjection), "GeomCamera3::viewProjection is 16 floats");
      static_assert(sizeof(now.viewport) == sizeof(view.viewport), "GeomCamera3::viewport is 16 floats");
      std::memcpy(now.viewProjection, &view.viewProjection, sizeof(now.viewProjection));
      std::memcpy(now.viewport, &view.viewport, sizeof(now.viewport));
      if (gCameraStamp.valid && (std::memcmp(gCameraStamp.viewProjection, now.viewProjection, sizeof(now.viewProjection)) != 0 ||
                                 std::memcmp(gCameraStamp.viewport, now.viewport, sizeof(now.viewport)) != 0)) {
        TouchNow(kCamera);
      }
      gCameraStamp = now;
    }

    std::uint32_t AmbientBits()
    {
      std::uint32_t bits = 0;
      CWldSession* const session = WLD_GetActiveSession();
      if (session != nullptr) {
        if (session->mWldMap != nullptr && session->mWldMap->mTerrainRes != nullptr && session->mWldMap->mTerrainRes->mMap != nullptr &&
            session->mWldMap->mTerrainRes->mMap->mWaterEnabled != 0) {
          bits |= kAmbientWater;
        }
        if (gLastGameTick >= 0 && session->mGameTick != gLastGameTick) {
          bits |= kAmbientSim;
        }
        gLastGameTick = session->mGameTick;
      }
      if (ren_SkyDome) {
        bits |= kAmbientSky;
      }
      return bits;
    }

    // ed_scene_dirty([consume]) -> epoch, reasons, ambient
    int __cdecl cfunc_EdSceneDirty(lua_State* const L)
    {
      const bool consume = lua_type(L, 1) == LUA_TNIL || lua_toboolean(L, 1) != 0;
      PollCamera();
      const std::uint32_t reasons = gTracker.Take(consume);
      lua_pushnumber(L, static_cast<lua_Number>(gTracker.Epoch()));
      lua_pushnumber(L, static_cast<lua_Number>(reasons));
      lua_pushnumber(L, static_cast<lua_Number>(AmbientBits()));
      return 3;
    }

    // ed_scene_touch([reasons]) -> epoch
    int __cdecl cfunc_EdSceneTouch(lua_State* const L)
    {
      const std::uint32_t reasons = lua_type(L, 1) == LUA_TNUMBER ? static_cast<std::uint32_t>(lua_tonumber(L, 1)) : static_cast<std::uint32_t>(kScript);
      TouchNow(reasons != 0u ? reasons : static_cast<std::uint32_t>(kScript));
      lua_pushnumber(L, static_cast<lua_Number>(gTracker.Epoch()));
      return 1;
    }

    void PutNumber(lua_State* const L, const char* const key, const double value)
    {
      lua_pushstring(L, key);
      lua_pushnumber(L, static_cast<lua_Number>(value));
      lua_settable(L, -3);
    }

    // ed_scene_activity() -> { idleMs, focused, sinceChangeMs, inputs, wait }
    //   idleMs         ms since the last keyboard / mouse input anywhere in the system (GetLastInputInfo)
    //   focused        1 when a window of this process has the foreground
    //   sinceChangeMs  ms since the scene epoch last moved (as seen by ed_scene_dirty; poll it for this to be current)
    //   inputs         how many input changes were seen by the polls so far
    //   wait           RecommendedWaitMs for a hold time of 2 s: 0 = draw at the full rate, else the longest the pacer may wait between frames
    int __cdecl cfunc_EdSceneActivity(lua_State* const L)
    {
      PollCamera();
      const std::uint32_t now = NowTick();
      const std::uint32_t lastInput = PollInput();
      const std::uint32_t idle = ElapsedMs(now, lastInput);
      // no change seen yet: count from the first look
      if (!gChangeTickSet) {
        gChangeTickSet = true;
        gLastChangeTick = now;
      }
      const std::uint32_t sinceChange = ElapsedMs(now, gLastChangeTick);
      lua_newtable(L);
      PutNumber(L, "idleMs", idle);
      PutNumber(L, "focused", OurProcessHasForeground() ? 1.0 : 0.0);
      PutNumber(L, "sinceChangeMs", sinceChange);
      PutNumber(L, "inputs", gInputs);
      PutNumber(L, "wait", RecommendedWaitMs(idle, sinceChange, AmbientBits(), gTracker.Take(false) != 0u, 2000u));
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

    struct EdActivityLuaBootstrap
    {
      EdActivityLuaBootstrap()
      {
        auto& set = UserLuaInitSet();
        (void)new CScrLuaBinder(set, "ed_scene_dirty", &cfunc_EdSceneDirty, nullptr, "<global>",
                                "ed_scene_dirty([consume=true]) -> epoch, reasons (1 camera, 2 overlay, 4 heights, 8 script), ambient (1 water, 2 sky, 4 simulation ticks)");
        (void)new CScrLuaBinder(set, "ed_scene_touch", &cfunc_EdSceneTouch, nullptr, "<global>", "ed_scene_touch([reasons=8]) -> epoch");
        (void)new CScrLuaBinder(set, "ed_scene_activity", &cfunc_EdSceneActivity, nullptr, "<global>",
                                "ed_scene_activity() -> { idleMs, focused, sinceChangeMs, inputs, wait }");
      }
    };

    const EdActivityLuaBootstrap gEdActivityLuaBootstrap{};
  } // namespace

  void Touch(const std::uint32_t reasons) noexcept
  {
    TouchNow(reasons);
  }
} // namespace moho::editor::activity
