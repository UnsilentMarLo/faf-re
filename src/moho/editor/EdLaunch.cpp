// EDITOR: the launch failure of a command-line map, see EdLaunch.h.
//
//   ed_launch_error([keep]) -> message | nil     the last launch failure ("Unable to launch <map>:\n<reason>"); reading it clears it unless `keep` is true
#include "moho/editor/EdLaunch.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include <Windows.h>

#include "gpg/core/utils/Logging.h"
#include "gpg/core/utils/Global.h"
#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/misc/StartupHelpers.h"

namespace
{
  std::mutex gLock;
  std::string gMessage;                 // the pending failure ("" = none)
  std::atomic<unsigned> gSerial{0};     // bumped by every failure; the delayed box belongs to one of them

  void ShowBoxLater(const unsigned serial, const std::string message)
  {
    std::thread([serial, message]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(moho::editor::launch::kBoxDelayMs));
      {
        const std::lock_guard<std::mutex> lock(gLock);
        if (gSerial.load() != serial || gMessage.empty()) {
          return;               // the editor's UI took it (or a later failure replaced it)
        }
      }
      int length = ::MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
      std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 1), L'\0');
      if (length > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wide.data(), length);
      }
      // no owner window and no wait in the main loop: the box lives on this thread only
      (void)::MessageBoxW(nullptr, wide.c_str(), L"FAF Studio", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }).detach();
  }

  int __cdecl cfunc_EdLaunchError(lua_State* const L)
  {
    const bool keep = lua_toboolean(L, 1) != 0;
    const std::lock_guard<std::mutex> lock(gLock);
    if (gMessage.empty()) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushstring(L, gMessage.c_str());
    if (!keep) {
      gMessage.clear();
    }
    return 1;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& UserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  struct EdLaunchLuaBootstrap
  {
    EdLaunchLuaBootstrap()
    {
      (void)new moho::CScrLuaBinder(UserLuaInitSet(), "ed_launch_error", &cfunc_EdLaunchError, nullptr, "<global>",
                                    "ed_launch_error([keep]) -> the message of the last failed map launch, or nil (cleared by reading unless keep)");
    }
  };

  const EdLaunchLuaBootstrap gEdLaunchLuaBootstrap{};
} // namespace

namespace moho::editor::launch
{
  bool Active()
  {
    return moho::CFG_GetArgOption("/editor", 0, nullptr);
  }

  void ReportFailure(const char* const mapName, const char* const reason)
  {
    std::string message = std::string("Unable to launch ") + (mapName != nullptr ? mapName : "") + ":\n" + (reason != nullptr ? reason : "unknown error");
    gpg::Warnf("EDITOR: %s", message.c_str());
    unsigned serial;
    {
      const std::lock_guard<std::mutex> lock(gLock);
      gMessage = message;
      serial = ++gSerial;
    }
    ShowBoxLater(serial, message);
  }
} // namespace moho::editor::launch
