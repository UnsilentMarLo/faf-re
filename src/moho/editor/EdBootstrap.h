// EDITOR: start-up of the editor build when it is started without arguments (a double click), so no launcher script is needed.
//
// What the packaged launcher (editor/packaging/launch.ps1) used to do, now inside the exe:
//   * the process becomes DPI aware (Windows no longer stretches the window to the display scale, which blurred everything);
//   * the FAForever data folder is found (FAF_DATA, else %ProgramData%\FAForever) and the files of it the editor needs are made available next to the exe:
//     init_faf.lua / SupComDataPath.lua copied into bin\, fa_path.lua into the folder above, folder junctions for cache, gamedata, movies, sounds, themes;
//   * the user folder (%APPDATA%\FAFStudio, FAFSTUDIO_USER) and the working folders (mapout, modproj, bin\config) are created;
//   * the arguments an editor start needs are added when they are not given: /init init_editor.lua, /editor, /nomovie, /nosound, /windowed <w> <h>, /log, /eduser;
//   * the UI scale of the first run follows the Windows display scale (environment variable FAFSTUDIO_UISCALE, read by init_editor.lua).
// Anything that goes wrong is reported in a message box and the process ends cleanly, instead of crashing in the engine's start-up
// (with FAFSTUDIO_NOGUI set the text goes to <user folder>\logs\boot_message.txt instead of a message box: automated runs).
//
// An exe is an editor build when init_editor.lua lies next to it or when it is started with /editor. A start that has /init is left alone (scripts, tests): only the DPI
// awareness is applied. FAFSTUDIO_DPI=0 turns the DPI awareness off. The module uses nothing of the engine, so tests/EdBootstrapTest.cpp builds it on its own.

#pragma once

#include <string>
#include <vector>

namespace moho::editor
{
  struct BootResult
  {
    std::vector<std::string> args; ///< argv[1..] the process should run with
    bool changed = false;          ///< args differ from the ones the process was started with
    bool exitNow = false;          ///< a message was shown: end the process with exitCode
    int exitCode = 0;
  };

  using BootMessageFn = void (*)(const std::wstring& title, const std::wstring& text);

  /** Replaces the message box (tests). nullptr restores MessageBoxW. */
  void SetBootMessageHandler(BootMessageFn fn);

  using BootLinkFn = bool (*)(const std::wstring& link, const std::wstring& target);

  /** Replaces the creation of folder links (tests on systems that cannot make them, such as Wine). nullptr restores junctions with a symbolic link as the fallback. */
  void SetBootLinkHandler(BootLinkFn fn);

  /** exePath: full path of the running exe; args: its argv[1..]. */
  BootResult PrepareEditorStart(const std::wstring& exePath, const std::vector<std::string>& args);
} // namespace moho::editor
