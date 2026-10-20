// EDITOR: start-up of the editor build without arguments. See EdBootstrap.h.

#include "moho/editor/EdBootstrap.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace moho::editor
{
  namespace
  {
    BootMessageFn gMessageHandler = nullptr;
    BootLinkFn gLinkHandler = nullptr;

    void Message(const std::wstring& text)
    {
      if (gMessageHandler != nullptr) {
        gMessageHandler(L"FAF Studio", text);
        return;
      }
      (void)::MessageBoxW(nullptr, text.c_str(), L"FAF Studio", MB_OK | MB_ICONERROR);
    }

    std::wstring GetEnv(const wchar_t* name)
    {
      const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
      if (needed == 0) {
        return std::wstring();
      }
      std::wstring value(needed, L'\0');
      const DWORD got = ::GetEnvironmentVariableW(name, value.data(), needed);
      value.resize(got);
      return value;
    }

    bool HasArg(const std::vector<std::string>& args, const char* name)
    {
      for (const std::string& a : args) {
        if (a.size() == std::strlen(name)
            && std::equal(a.begin(), a.end(), name, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); })) {
          return true;
        }
      }
      return false;
    }

    bool Exists(const fs::path& p)
    {
      std::error_code ec;
      return fs::exists(p, ec);
    }

    // ---- DPI ------------------------------------------------------------------------------------------------------------------------------

    /**
     * A process that is not DPI aware is drawn at 96 dpi and stretched to the display scale by Windows: at 125 % every edge and glyph is resampled and the text is blurry.
     * System DPI awareness (not per monitor: the engine does not handle WM_DPICHANGED) must be set before the first window exists.
     */
    void MakeDpiAware()
    {
      HMODULE user = ::GetModuleHandleW(L"user32.dll");
      if (user == nullptr) {
        return;
      }
      using SetContextFn = BOOL(WINAPI*)(HANDLE);
      const auto setContext = reinterpret_cast<SetContextFn>(reinterpret_cast<void*>(::GetProcAddress(user, "SetProcessDpiAwarenessContext")));
      if (setContext != nullptr && setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-2)))) { // DPI_AWARENESS_CONTEXT_SYSTEM_AWARE
        return;
      }
      using SetAwareFn = BOOL(WINAPI*)();
      const auto setAware = reinterpret_cast<SetAwareFn>(reinterpret_cast<void*>(::GetProcAddress(user, "SetProcessDPIAware")));
      if (setAware != nullptr) {
        (void)setAware();
      }
    }

    /** Display scale in 0.25 steps (the UI scale buttons step by 0.25), 1 to 3. Only meaningful after MakeDpiAware(). */
    float DisplayScale()
    {
      UINT dpi = 96;
      HMODULE user = ::GetModuleHandleW(L"user32.dll");
      using GetDpiFn = UINT(WINAPI*)();
      const auto getDpi = user ? reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(::GetProcAddress(user, "GetDpiForSystem"))) : nullptr;
      if (getDpi != nullptr) {
        dpi = getDpi();
      } else if (HDC dc = ::GetDC(nullptr)) {
        dpi = static_cast<UINT>(::GetDeviceCaps(dc, LOGPIXELSX));
        (void)::ReleaseDC(nullptr, dc);
      }
      if (dpi < 96) {
        dpi = 96;
      }
      float scale = static_cast<float>(dpi) / 96.0f;
      scale = static_cast<float>(static_cast<int>(scale * 4.0f + 0.5f)) / 4.0f;
      return (std::min)(3.0f, (std::max)(1.0f, scale));
    }

    // ---- junctions ------------------------------------------------------------------------------------------------------------------------

    struct MountPointBuffer
    {
      ULONG tag;
      USHORT dataLength;
      USHORT reserved;
      USHORT substituteOffset;
      USHORT substituteLength;
      USHORT printOffset;
      USHORT printLength;
      WCHAR path[1];
    };

    std::wstring Canonical(const std::wstring& path)
    {
      std::wstring s = path;
      if (s.compare(0, 4, L"\\??\\") == 0) {
        s.erase(0, 4);
      }
      std::replace(s.begin(), s.end(), L'/', L'\\');
      while (s.size() > 3 && s.back() == L'\\') {
        s.pop_back();
      }
      std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
      return s;
    }

    bool IsReparsePoint(const fs::path& p)
    {
      const DWORD attr = ::GetFileAttributesW(p.c_str());
      return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    struct SymlinkBuffer
    {
      ULONG tag;
      USHORT dataLength;
      USHORT reserved;
      USHORT substituteOffset;
      USHORT substituteLength;
      USHORT printOffset;
      USHORT printLength;
      ULONG flags;
      WCHAR path[1];
    };

    /** Target of a junction or a directory symbolic link, empty when `link` is neither. */
    std::wstring LinkTarget(const fs::path& link)
    {
      HANDLE h = ::CreateFileW(link.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        return std::wstring();
      }
      std::vector<unsigned char> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
      DWORD returned = 0;
      std::wstring target;
      if (::DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        const auto* mp = reinterpret_cast<const MountPointBuffer*>(buffer.data());
        const auto* sl = reinterpret_cast<const SymlinkBuffer*>(buffer.data());
        if (mp->tag == IO_REPARSE_TAG_MOUNT_POINT) {
          target.assign(mp->path + mp->substituteOffset / sizeof(WCHAR), mp->substituteLength / sizeof(WCHAR));
        } else if (sl->tag == IO_REPARSE_TAG_SYMLINK) {
          target.assign(sl->path + sl->substituteOffset / sizeof(WCHAR), sl->substituteLength / sizeof(WCHAR));
        }
      }
      ::CloseHandle(h);
      return target;
    }

    bool CreateJunction(const fs::path& link, const fs::path& target)
    {
      std::error_code ec;
      fs::create_directory(link, ec);
      if (ec) {
        return false;
      }
      HANDLE h = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        (void)::RemoveDirectoryW(link.c_str());
        return false;
      }
      std::wstring print = target.wstring();
      std::replace(print.begin(), print.end(), L'/', L'\\');
      while (print.size() > 3 && print.back() == L'\\') {
        print.pop_back();
      }
      const std::wstring substitute = L"\\??\\" + print;
      const size_t pathBytes = (substitute.size() + 1 + print.size() + 1) * sizeof(WCHAR);
      const size_t total = offsetof(MountPointBuffer, path) + pathBytes;
      std::vector<unsigned char> buffer(total, 0);
      auto* rp = reinterpret_cast<MountPointBuffer*>(buffer.data());
      rp->tag = IO_REPARSE_TAG_MOUNT_POINT;
      rp->dataLength = static_cast<USHORT>(total - 8); // everything after the 8 byte header
      rp->substituteOffset = 0;
      rp->substituteLength = static_cast<USHORT>(substitute.size() * sizeof(WCHAR));
      rp->printOffset = static_cast<USHORT>((substitute.size() + 1) * sizeof(WCHAR));
      rp->printLength = static_cast<USHORT>(print.size() * sizeof(WCHAR));
      std::memcpy(rp->path, substitute.c_str(), (substitute.size() + 1) * sizeof(WCHAR));
      std::memcpy(rp->path + substitute.size() + 1, print.c_str(), (print.size() + 1) * sizeof(WCHAR));
      DWORD returned = 0;
      const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(total), nullptr, 0, &returned, nullptr);
      ::CloseHandle(h);
      if (!ok) {
        (void)::RemoveDirectoryW(link.c_str());
        return false;
      }
      return true;
    }

    /** Folders on a drive that cannot hold junctions: a directory symbolic link (needs Developer Mode or the privilege; the flag 2 asks for the unprivileged form). */
    bool CreateDirectorySymlink(const fs::path& link, const fs::path& target)
    {
      using CreateSymlinkFn = BOOLEAN(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
      HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
      const auto create = k32 ? reinterpret_cast<CreateSymlinkFn>(reinterpret_cast<void*>(::GetProcAddress(k32, "CreateSymbolicLinkW"))) : nullptr;
      if (create == nullptr) {
        return false;
      }
      return create(link.c_str(), target.c_str(), 0x1 /* SYMBOLIC_LINK_FLAG_DIRECTORY */ | 0x2 /* ..._ALLOW_UNPRIVILEGED_CREATE */) != 0
             || create(link.c_str(), target.c_str(), 0x1) != 0;
    }

    /** `link` becomes a junction (else a directory symbolic link) to `target`. One that already points there stays; one to another place is replaced; a real folder is left alone. */
    void EnsureJunction(const fs::path& link, const fs::path& target)
    {
      if (::GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES) { // (also true for a junction whose target is gone)
        if (!IsReparsePoint(link)) {
          return; // a real folder: not ours to replace
        }
        if (Canonical(LinkTarget(link)) == Canonical(target.wstring())) {
          return;
        }
        (void)::RemoveDirectoryW(link.c_str()); // removes the junction, never the folder it points to
      }
      if (gLinkHandler != nullptr) {
        (void)gLinkHandler(link.wstring(), target.wstring());
      } else if (!CreateJunction(link, target)) {
        (void)CreateDirectorySymlink(link, target);
      }
    }

    // ---- files ----------------------------------------------------------------------------------------------------------------------------

    /** Copies src over dst when dst is missing, has another size or is older than src (FAF updates these files). */
    void RefreshCopy(const fs::path& src, const fs::path& dst)
    {
      std::error_code ec;
      bool copy = !fs::exists(dst, ec);
      if (!copy) {
        const auto srcSize = fs::file_size(src, ec);
        const auto dstSize = fs::file_size(dst, ec);
        copy = srcSize != dstSize || fs::last_write_time(dst, ec) < fs::last_write_time(src, ec);
      }
      if (copy) {
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
      }
    }

    /** UTF-8 text file (for the message of a start that cannot go on, when no message box may open). */
    void WriteText(const fs::path& file, const std::wstring& text)
    {
      std::string utf8;
      const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
      if (n > 0) {
        utf8.resize(static_cast<size_t>(n));
        (void)::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
      }
      HANDLE h = ::CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        (void)::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        ::CloseHandle(h);
      }
    }

    void Touch(const fs::path& file)
    {
      if (!Exists(file)) {
        HANDLE h = ::CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
          ::CloseHandle(h);
        }
      }
    }

    // ---- arguments ------------------------------------------------------------------------------------------------------------------------

    /** The engine's argv is in the ANSI code page. False when a character would not survive (a user name with other letters). */
    bool ToAcp(const std::wstring& wide, std::string& out)
    {
      out.clear();
      if (wide.empty()) {
        return true;
      }
      BOOL usedDefault = FALSE;
      const int n = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, &usedDefault);
      if (n <= 0) {
        return false;
      }
      out.resize(static_cast<size_t>(n));
      (void)::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), static_cast<int>(wide.size()), out.data(), n, nullptr, &usedDefault);
      return usedDefault == FALSE;
    }

    /** A folder as an argument: its path, else its 8.3 path when the folder exists and the path has characters the ANSI code page lacks. */
    bool DirArg(const fs::path& dir, std::string& out)
    {
      if (ToAcp(dir.wstring(), out)) {
        return true;
      }
      std::vector<wchar_t> buffer(32768);
      const DWORD n = ::GetShortPathNameW(dir.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
      return n > 0 && n < buffer.size() && ToAcp(std::wstring(buffer.data(), n), out);
    }

    std::wstring Wide(const char* text)
    {
      std::wstring w;
      for (const char* p = text; *p != '\0'; ++p) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
      }
      return w;
    }
  } // namespace

  void SetBootMessageHandler(const BootMessageFn fn)
  {
    gMessageHandler = fn;
  }

  void SetBootLinkHandler(const BootLinkFn fn)
  {
    gLinkHandler = fn;
  }

  BootResult PrepareEditorStart(const std::wstring& exePath, const std::vector<std::string>& args)
  {
    BootResult result;
    result.args = args;

    const fs::path bin = fs::path(exePath).parent_path();
    const fs::path root = bin.parent_path();
    const bool initNextToExe = Exists(bin / L"init_editor.lua");
    const bool editor = initNextToExe || HasArg(args, "/editor");
    if (!editor) {
      return result; // the plain game executable
    }

    if (GetEnv(L"FAFSTUDIO_DPI") != L"0") {
      MakeDpiAware();
    }
    if (HasArg(args, "/init")) {
      return result; // an explicit start (scripts, tests): nothing else is added
    }

    fs::path user = GetEnv(L"FAFSTUDIO_USER");
    if (user.empty()) {
      const std::wstring appData = GetEnv(L"APPDATA");
      user = appData.empty() ? (root / L"user") : (fs::path(appData) / L"FAFStudio");
    }

    auto fail = [&result, &user](const std::wstring& text, const int code) {
      if (!GetEnv(L"FAFSTUDIO_NOGUI").empty()) {
        // no message box (automated runs): the text goes to <user>\logs\boot_message.txt
        std::error_code ec;
        fs::create_directories(user / L"logs", ec);
        WriteText(user / L"logs" / L"boot_message.txt", text);
      } else {
        Message(text);
      }
      result.exitNow = true;
      result.exitCode = code;
      return result;
    };

    if (!initNextToExe) {
      return fail(L"init_editor.lua is missing next to the program (" + (bin / L"init_editor.lua").wstring() + L"). The installation is incomplete.", 4);
    }

    // Lua writes and reads files relative to the working folder (editor_boot.txt, editor_mods.txt ...): it must be bin\, whatever started the program (a shortcut, a console).
    (void)::SetCurrentDirectoryW(bin.c_str());

    // ---- the FAF data -------------------------------------------------------------------------------------------------------------------
    fs::path faf = GetEnv(L"FAF_DATA");
    if (faf.empty()) {
      std::wstring programData = GetEnv(L"ProgramData");
      if (programData.empty()) {
        programData = L"C:\\ProgramData";
      }
      faf = fs::path(programData) / L"FAForever";
    }
    const bool fafComplete = Exists(faf / L"gamedata") && Exists(faf / L"fa_path.lua") && Exists(faf / L"bin" / L"init_faf.lua") && Exists(faf / L"bin" / L"SupComDataPath.lua");
    const bool localComplete = Exists(bin / L"init_faf.lua") && Exists(root / L"fa_path.lua") && Exists(root / L"gamedata");
    if (!fafComplete && !localComplete) {
      std::wstring missing;
      for (const wchar_t* need : {L"gamedata", L"fa_path.lua", L"bin\\init_faf.lua", L"bin\\SupComDataPath.lua"}) {
        if (!Exists(faf / need)) {
          missing += (missing.empty() ? L"" : L", ") + std::wstring(need);
        }
      }
      return fail(L"FAF Studio needs Supreme Commander: Forged Alliance with the FAForever client installed, and the game data of it.\n\nLooked in: " + faf.wstring() + L"\nMissing: " + missing
                    + L"\n\nInstall the FAForever client and start it once (it downloads the data), or set the environment variable FAF_DATA to the folder that holds 'gamedata'.\n"
                      L"FAF Studio does not contain any game data.",
                  2);
    }
    if (fafComplete) {
      RefreshCopy(faf / L"bin" / L"init_faf.lua", bin / L"init_faf.lua");
      RefreshCopy(faf / L"bin" / L"SupComDataPath.lua", bin / L"SupComDataPath.lua");
      RefreshCopy(faf / L"fa_path.lua", root / L"fa_path.lua");
      for (const wchar_t* name : {L"cache", L"gamedata", L"movies", L"sounds", L"themes"}) {
        if (Exists(faf / name)) {
          EnsureJunction(root / name, faf / name);
        }
      }
    }
    if (!Exists(root / L"gamedata") || !Exists(bin / L"init_faf.lua") || !Exists(root / L"fa_path.lua")) {
      return fail(L"The FAF data could not be linked into " + root.wstring()
                    + L" (gamedata, fa_path.lua or bin\\init_faf.lua is still missing).\nMake sure the folder is writable (not under C:\\Program Files), or set FAF_DATA.",
                  3);
    }

    // ---- folders and files the editor works in ------------------------------------------------------------------------------------------
    std::error_code ec;
    fs::create_directories(user / L"logs", ec);
    fs::create_directories(root / L"mapout", ec);
    fs::create_directories(root / L"modproj", ec);
    fs::create_directories(bin / L"config", ec);
    Touch(bin / L"editor_mods.txt");
    Touch(bin / L"editor_mods_trial.txt");
    const fs::path log = user / L"logs" / L"editor.log";
    if (Exists(log)) {
      fs::rename(log, user / L"logs" / L"editor.previous.log", ec);
      if (ec) {
        fs::remove(user / L"logs" / L"editor.previous.log", ec);
        fs::rename(log, user / L"logs" / L"editor.previous.log", ec);
      }
    }

    // ---- the first-run UI scale follows the Windows display scale ---------------------------------------------------------------------
    if (GetEnv(L"FAFSTUDIO_UISCALE").empty() && GetEnv(L"FAFSTUDIO_DPI") != L"0") {
      char text[32];
      std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(DisplayScale()));
      (void)::SetEnvironmentVariableW(L"FAFSTUDIO_UISCALE", Wide(text).c_str());
    }

    // ---- the arguments ----------------------------------------------------------------------------------------------------------------------
    auto add = [&result](const char* a) { result.args.emplace_back(a); };
    add("/init");
    add("init_editor.lua");
    if (!HasArg(args, "/editor")) {
      add("/editor");
    }
    if (!HasArg(args, "/nomovie")) {
      add("/nomovie");
    }
    if (!HasArg(args, "/nosound")) {
      add("/nosound");
    }
    if (!HasArg(args, "/windowed") && !HasArg(args, "/fullscreen")) {
      RECT area{0, 0, 1280, 720};
      (void)::SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
      const long w = (std::min)(1920L, static_cast<long>((area.right - area.left) * 0.9));
      const long h = (std::min)(1080L, static_cast<long>((area.bottom - area.top) * 0.9));
      add("/windowed");
      result.args.push_back(std::to_string((std::max)(640L, w)));
      result.args.push_back(std::to_string((std::max)(480L, h)));
    }
    std::string text;
    if (!HasArg(args, "/log") && DirArg(user / L"logs", text)) {
      add("/log");
      result.args.push_back(text + "\\editor.log");
    }
    if (!HasArg(args, "/eduser") && DirArg(user, text)) {
      add("/eduser");
      result.args.push_back(text);
    }
    result.changed = true;
    return result;
  }
} // namespace moho::editor
