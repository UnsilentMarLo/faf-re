// Test of moho/editor/EdBootstrap.cpp on its own (no engine): cross-compiled with mingw-w64 and run under Wine, or built with MSVC.
//   edbootstrap_test.exe <empty work folder>
// Builds a fake FAF data folder and a fake editor install in the work folder and checks what a start without arguments does.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "moho/editor/EdBootstrap.h"

namespace fs = std::filesystem;
using moho::editor::BootResult;
using moho::editor::PrepareEditorStart;

static int gFailed = 0;
static std::wstring gLastMessage;

static void Check(bool ok, const char* what)
{
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++gFailed;
  }
}

static void Write(const fs::path& p, const std::string& text)
{
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

static bool Has(const std::vector<std::string>& v, const std::string& s)
{
  for (const auto& a : v) {
    if (a == s) {
      return true;
    }
  }
  return false;
}

static std::string After(const std::vector<std::string>& v, const std::string& s)
{
  for (size_t i = 0; i + 1 < v.size(); ++i) {
    if (v[i] == s) {
      return v[i + 1];
    }
  }
  return std::string();
}

static void MakeFaf(const fs::path& faf, const std::string& marker)
{
  Write(faf / "gamedata" / (marker + ".nx2"), "x");
  Write(faf / "fa_path.lua", "fa_path = 'C:/fa'\n");
  Write(faf / "bin" / "init_faf.lua", "-- init " + marker + "\n");
  Write(faf / "bin" / "SupComDataPath.lua", "-- data path\n");
  for (const char* d : {"cache", "movies", "sounds", "themes"}) {
    fs::create_directories(faf / d);
  }
}

// Wine can neither make junctions nor symbolic links reliably: the test then lets the bootstrap "link" by copying and skips what needs a real link.
static bool CopyLink(const std::wstring& link, const std::wstring& target)
{
  std::error_code ec;
  fs::copy(target, link, fs::copy_options::recursive, ec);
  return !ec;
}

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::printf("usage: edbootstrap_test <work folder>\n");
    return 2;
  }
  const fs::path work = fs::absolute(argv[1]);
  fs::remove_all(work);
  fs::create_directories(work);
  moho::editor::SetBootMessageHandler([](const std::wstring&, const std::wstring& text) { gLastMessage = text; });
  const bool copyLinks = argc > 2 && std::string(argv[2]) == "--copy-links";
  if (copyLinks) {
    moho::editor::SetBootLinkHandler(&CopyLink);
    std::printf("(links are made by copying: junction checks skipped)\n");
  }

  // ---- a fresh install and a FAF data folder ------------------------------------------------------------------------------------------
  const fs::path root = work / "install";
  const fs::path exe = root / "bin" / "FAFStudio.exe";
  Write(root / "bin" / "init_editor.lua", "-- editor init\n");
  const fs::path faf = work / "faf";
  MakeFaf(faf, "a");
  ::SetEnvironmentVariableW(L"FAF_DATA", faf.c_str());
  ::SetEnvironmentVariableW(L"FAFSTUDIO_USER", (work / "user").c_str());
  ::SetEnvironmentVariableW(L"FAFSTUDIO_UISCALE", nullptr);
  ::SetEnvironmentVariableW(L"FAFSTUDIO_DPI", nullptr);

  BootResult r = PrepareEditorStart(exe.wstring(), {});
  if (r.exitNow) {
    std::wprintf(L"  message: %ls\n", gLastMessage.c_str());
  }
  Check(!r.exitNow && r.changed, "no arguments: the start goes on with changed arguments");
  Check(Has(r.args, "/init") && After(r.args, "/init") == "init_editor.lua", "adds /init init_editor.lua");
  Check(Has(r.args, "/editor") && Has(r.args, "/nomovie") && Has(r.args, "/nosound"), "adds /editor /nomovie /nosound");
  Check(Has(r.args, "/windowed") && std::stoi(After(r.args, "/windowed")) >= 640, "adds /windowed with a size");
  Check(Has(r.args, "/log") && After(r.args, "/log").find("editor.log") != std::string::npos, "adds /log <user>\\logs\\editor.log");
  Check(Has(r.args, "/eduser") && !After(r.args, "/eduser").empty(), "adds /eduser <user folder>");
  Check(fs::exists(root / "bin" / "init_faf.lua") && fs::exists(root / "bin" / "SupComDataPath.lua") && fs::exists(root / "fa_path.lua"), "copies init_faf.lua, SupComDataPath.lua and fa_path.lua");
  Check(fs::exists(root / "gamedata" / "a.nx2"), "gamedata is linked to the FAF data (a file of it is reachable)");
  if (!copyLinks) {
    const DWORD attr = ::GetFileAttributesW((root / "gamedata").c_str());
    Check(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0, "gamedata is a reparse point (junction), not a copy");
  }
  Check(fs::exists(root / "cache") && fs::exists(root / "movies") && fs::exists(root / "sounds") && fs::exists(root / "themes"), "cache, movies, sounds, themes are linked");
  Check(fs::exists(work / "user" / "logs") && fs::exists(root / "mapout") && fs::exists(root / "modproj") && fs::exists(root / "bin" / "config"), "creates the user folder and the working folders");
  Check(fs::exists(root / "bin" / "editor_mods.txt") && fs::exists(root / "bin" / "editor_mods_trial.txt"), "creates editor_mods.txt and editor_mods_trial.txt");
  wchar_t scale[32] = {};
  const DWORD n = ::GetEnvironmentVariableW(L"FAFSTUDIO_UISCALE", scale, 32);
  Check(n > 0 && std::stod(std::wstring(scale)) >= 1.0, "sets FAFSTUDIO_UISCALE from the display scale");

  // ---- the same again: nothing breaks, the old log is kept as editor.previous.log -----------------------------------------------------
  Write(work / "user" / "logs" / "editor.log", "last run");
  r = PrepareEditorStart(exe.wstring(), {});
  Check(!r.exitNow && fs::exists(root / "gamedata" / "a.nx2"), "a second start is the same (junctions stay)");
  Check(fs::exists(work / "user" / "logs" / "editor.previous.log") && !fs::exists(work / "user" / "logs" / "editor.log"), "the previous log is kept as editor.previous.log");

  // ---- the FAF data moves: the junction follows ---------------------------------------------------------------------------------------
  const fs::path faf2 = work / "faf2";
  MakeFaf(faf2, "b");
  ::SetEnvironmentVariableW(L"FAF_DATA", faf2.c_str());
  r = PrepareEditorStart(exe.wstring(), {});
  if (!copyLinks) {
    Check(fs::exists(root / "gamedata" / "b.nx2") && !fs::exists(root / "gamedata" / "a.nx2"), "a changed FAF_DATA re-points the junction");
  }
  Check(std::ifstream(root / "bin" / "init_faf.lua").good(), "init_faf.lua stays");

  // ---- arguments the user gave stay and are not repeated ---------------------------------------------------------------------------------
  r = PrepareEditorStart(exe.wstring(), {"/map", "mymap", "/windowed", "800", "600", "/log", "x.log"});
  Check(After(r.args, "/map") == "mymap" && After(r.args, "/windowed") == "800" && After(r.args, "/log") == "x.log", "given arguments stay (/map, /windowed, /log)");
  int windowed = 0, logs = 0;
  for (const auto& a : r.args) {
    windowed += a == "/windowed";
    logs += a == "/log";
  }
  Check(windowed == 1 && logs == 1, "/windowed and /log are not added a second time");
  Check(Has(r.args, "/init") && Has(r.args, "/eduser"), "the rest is still added");

  // ---- an explicit start is left alone ---------------------------------------------------------------------------------------------------
  r = PrepareEditorStart(exe.wstring(), {"/init", "init_editor.lua", "/editor", "/map", "m"});
  Check(!r.changed && !r.exitNow && r.args.size() == 5, "with /init nothing is added");

  // ---- the plain game exe is not touched --------------------------------------------------------------------------------------------------
  const fs::path game = work / "game" / "bin" / "ForgedAlliance.exe";
  fs::create_directories(game.parent_path());
  r = PrepareEditorStart(game.wstring(), {"/foo"});
  Check(!r.changed && !r.exitNow && r.args.size() == 1, "an exe without init_editor.lua and without /editor is not changed");

  // ---- no FAF data anywhere: a message, no crash ------------------------------------------------------------------------------------------
  const fs::path root2 = work / "install2";
  Write(root2 / "bin" / "init_editor.lua", "--\n");
  ::SetEnvironmentVariableW(L"FAF_DATA", (work / "nothing").c_str());
  gLastMessage.clear();
  r = PrepareEditorStart((root2 / "bin" / "FAFStudio.exe").wstring(), {});
  Check(r.exitNow && r.exitCode == 2 && gLastMessage.find(L"Missing") != std::wstring::npos && gLastMessage.find(L"gamedata") != std::wstring::npos, "missing FAF data: message with what is missing, exit code 2");

  // ---- the same without a message box (automated runs) -------------------------------------------------------------------------------------
  ::SetEnvironmentVariableW(L"FAFSTUDIO_NOGUI", L"1");
  gLastMessage.clear();
  r = PrepareEditorStart((root2 / "bin" / "FAFStudio.exe").wstring(), {});
  std::ifstream boot(work / "user" / "logs" / "boot_message.txt", std::ios::binary);
  std::string bootText((std::istreambuf_iterator<char>(boot)), std::istreambuf_iterator<char>());
  Check(r.exitNow && r.exitCode == 2 && gLastMessage.empty() && bootText.find("Missing") != std::string::npos, "FAFSTUDIO_NOGUI: the message goes to logs\\boot_message.txt, no message box");
  ::SetEnvironmentVariableW(L"FAFSTUDIO_NOGUI", nullptr);

  // ---- already set up, FAF data gone (moved install): starts from what is there ------------------------------------------------------
  r = PrepareEditorStart(exe.wstring(), {});
  Check(!r.exitNow && Has(r.args, "/init"), "set up before and FAF data not found now: starts anyway");

  std::printf("%s\n", gFailed == 0 ? "ALL PASS" : "FAILURES");
  return gFailed == 0 ? 0 : 1;
}
