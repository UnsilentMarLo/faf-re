// EDITOR: natives for the look of the editor's main window (SC Map Studio branding). The names start with ed_app_ because ed_window_* is the pop-out window API (EdPopout.cpp).
//
//   ed_app_set_title(text) -> true | nil, message     the title bar text of the main window (the engine creates it as "Forged Alliance")
//   ed_app_set_icon(path)  -> true | nil, message     the window icon (title bar, taskbar, Alt+Tab) from an .ico file on disk (PNG or bitmap entries);
//                                                        also set as the class icon so the pop-out windows of the editor share it
//
// The main window is found once: the visible top-level window of this process that has a caption and no owner, preferring the one that still carries the engine's own title.

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace
{
  HWND gMain = nullptr;

  std::wstring Wide(const char* const utf8)
  {
    if (utf8 == nullptr || *utf8 == '\0') {
      return std::wstring();
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0') {
      out.pop_back();
    }
    return out;
  }

  struct FindState
  {
    DWORD pid = 0;
    HWND best = nullptr;
    bool bestIsEngineTitle = false;
  };

  BOOL CALLBACK EnumProc(HWND h, LPARAM param)
  {
    auto* const st = reinterpret_cast<FindState*>(param);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != st->pid || !::IsWindowVisible(h) || ::GetWindow(h, GW_OWNER) != nullptr) {
      return TRUE;
    }
    if ((::GetWindowLongW(h, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) {
      return TRUE;
    }
    wchar_t title[256] = {};
    ::GetWindowTextW(h, title, 255);
    const bool engineTitle = (std::wstring(title) == L"Forged Alliance") || (std::wstring(title) == L"FAF Studio");
    if (st->best == nullptr || (engineTitle && !st->bestIsEngineTitle)) {
      st->best = h;
      st->bestIsEngineTitle = engineTitle;
    }
    return TRUE;
  }

  HWND MainWindow()
  {
    if (gMain != nullptr && ::IsWindow(gMain)) {
      return gMain;
    }
    FindState st;
    st.pid = ::GetCurrentProcessId();
    ::EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&st));
    gMain = st.best;
    return gMain;
  }

  int Fail(lua_State* const L, const char* const message)
  {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
  }

  int __cdecl cfunc_EdWindowSetTitle(lua_State* const L)
  {
    const char* const text = lua_tostring(L, 1);
    if (text == nullptr) {
      return Fail(L, "ed_app_set_title: argument 1 must be a string");
    }
    HWND const h = MainWindow();
    if (h == nullptr) {
      return Fail(L, "no main window");
    }
    const std::wstring wide = Wide(text);
    if (!::SetWindowTextW(h, wide.c_str())) {
      return Fail(L, "SetWindowText failed");
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_EdWindowSetIcon(lua_State* const L)
  {
    const char* const path = lua_tostring(L, 1);
    if (path == nullptr) {
      return Fail(L, "ed_app_set_icon: argument 1 must be a string");
    }
    HWND const h = MainWindow();
    if (h == nullptr) {
      return Fail(L, "no main window");
    }
    const std::wstring wide = Wide(path);
    const int bigX = ::GetSystemMetrics(SM_CXICON), bigY = ::GetSystemMetrics(SM_CYICON);
    const int smallX = ::GetSystemMetrics(SM_CXSMICON), smallY = ::GetSystemMetrics(SM_CYSMICON);
    HICON const big = static_cast<HICON>(::LoadImageW(nullptr, wide.c_str(), IMAGE_ICON, bigX, bigY, LR_LOADFROMFILE));
    HICON const small = static_cast<HICON>(::LoadImageW(nullptr, wide.c_str(), IMAGE_ICON, smallX, smallY, LR_LOADFROMFILE));
    if (big == nullptr && small == nullptr) {
      return Fail(L, "cannot load the icon file");
    }
    if (big != nullptr) {
      ::SendMessageW(h, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
      ::SetClassLongPtrW(h, GCLP_HICON, reinterpret_cast<LONG_PTR>(big));
    }
    if (small != nullptr) {
      ::SendMessageW(h, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
      ::SetClassLongPtrW(h, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small));
    }
    lua_pushboolean(L, 1);
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

  struct EdWindowLuaBootstrap
  {
    EdWindowLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_app_set_title", &cfunc_EdWindowSetTitle, nullptr, "<global>", "ed_app_set_title(text) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_app_set_icon", &cfunc_EdWindowSetIcon, nullptr, "<global>", "ed_app_set_icon(path) -> true | nil, message");
    }
  };

  const EdWindowLuaBootstrap gEdWindowLuaBootstrap{};
} // namespace
