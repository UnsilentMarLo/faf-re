// EDITOR: the FAF vault client natives (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP4): upstream's
// EdFafApi / EdVault / EdHttp (faf_editor_logic) bound to Lua.
//
// The library does the work (the FAF login by Hydra's device flow, the map API's filters and answers, the map archive
// download and install); this unit only turns tables into structs and back. Every request runs on a worker thread of
// the library and is POLLED from Lua, so the UI thread never waits for the network:
//
//   ed_vault_start{ tokenFile, pictureCache [, resume] } -> true     once, when the vault is first shown (nothing
//   touches the network before); with a token file that holds a
//                                                                     sign-in the session logs in with it (`resume`,
//                                                                     default true). The file is encrypted for this
//                                                                     Windows user (DPAPI).
//   ed_vault_status() -> { state = 'loggedout' | 'working' | 'waiting' | 'loggedin', user, message, userCode,
//   verifyUrl, secondsLeft, searching, openPage }
//                                                                     `openPage` is true ONCE after StartLogin: the
//                                                                     caller opens `verifyUrl` (ed_open_url).
//   ed_vault_login() / ed_vault_cancel_login() / ed_vault_logout()    logout also empties the token file
//   ed_vault_search(query, page) -> true | nil, why                   the search needs a sign-in (the map API answers
//   401 without one); `query` below, `page` from 1 ed_vault_poll() -> nil | { maps = { {name, folder, author,
//   description, thumbnail, version, players, width, height, gamesPlayed, ranked}, ... }, pages } ed_vault_picture(url)
//   -> path | false | nil                       a map's picture as a file of the preview cache (nil while it loads,
//   false when it can't be had) ed_vault_download(nameOrLink, mapsFolder) -> { folder, url } | nil, why      downloads
//   the archive and installs it into mapsFolder/<folder>/ ed_vault_download_status() -> { state = 'idle' | 'working' |
//   'done' | 'failed', message, received, total, map, folder }     (`folder` = the disk folder of a finished one)
//   ed_vault_download_reset()                                         back to idle once the result was taken
//   ed_vault_maps_folder() -> path        where downloads go: <vault>\maps of the FAF client's vault
//   (`custom_vault_path` of fa_path.lua, which the init file mounts the maps of),
//                                         else <Documents>\My Games\Gas Powered Games\Supreme Commander Forged
//                                         Alliance\maps
//   ed_vault_scenario(folder) -> stem | nil    the `<stem>` of the `<stem>_scenario.lua` in a map folder on disk
//   ed_open_url(url) -> true | nil, why                               http(s) links only, in the default browser
//
// Query tables (the FAF client's map filters): { text, author, uploadedFrom, uploadedTo ('YYYY-MM-DD'), widths = {k,
// ...}, heights = {k, ...} (k = 0..6 into the sizes of ed_vault_meta), minPlayers, maxPlayers, minScore, maxScore,
// rankedOnly, sortProperty, descending }; a missing field is "any".
//
// Pure helpers (no network, no state; what the selftest and the panel's "show query" use):
//   ed_vault_meta() -> { sizes = { {name, pixels}, ... }, sorts = { {property, label}, ... } }
//   ed_vault_filter(query) -> RSQL text as the FAF client writes it      ed_vault_url(query, page) -> the map API's
//   link ed_vault_query_text(query) -> line       ed_vault_query_parse(line) -> query | nil          (saved queries)
//   ed_vault_parse_search(json) -> { maps, pages } | nil, why         ed_vault_parse_name(text) -> { folder, url } |
//   nil, why ed_vault_seal_test(file, text) -> bool    a DPAPI file as the session keeps its sign-in (test runs seal a
//   made-up token and watch the session pick it up)
//
// What never happens: a token in a log line, a result table or an error message. The session's own log lines
// (SetFafLog) carry states and the FAF name only; the natives return `userCode` and `verifyUrl` (the device-flow page,
// which has the one-time user code in it) while a sign-in waits, and never the access or refresh token.

#include <dpapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "editor/doc/EdFafApi.h"
#include "editor/doc/EdVault.h"
#include "gpg/core/utils/Logging.h"
#include "moho/editor/EdUpCommon.h"
#include "moho/misc/StartupHelpers.h"

namespace
{
  namespace up = moho::editor::up;
  namespace fe = faf::editor;
  namespace fs = std::filesystem;

  /** The vault's state: one session and one download for the editor. Never destroyed: a request that is still on the
   * wire at exit must not delay it. */
  struct Vault
  {
    fe::EdFafSession session;
    fe::EdVaultDownload download;
    bool started = false;
    fs::path tokenFile;
    fs::path pictureCache;
  };

  Vault& State()
  {
    static Vault* const vault = new Vault();
    return *vault;
  }

  void LogLine(
    const char* const line
  )
  {
    gpg::Logf("%s", line);
  }

  [[nodiscard]] fs::path PathOf(
    const std::string& text
  )
  {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
  }

  [[nodiscard]] std::string PathText(
    const fs::path& path
  )
  {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
  }

  // ---- queries
  // --------------------------------------------------------------------------------------------------------------------------------------

  [[nodiscard]] unsigned BitsOf(
    lua_State* const L,
    const int index,
    const char* const key
  )
  {
    unsigned bits = 0;
    for (const double k : up::FieldNumberList(L, index, key)) {
      if (k >= 0.0 && k < 7.0) {
        bits |= 1U << static_cast<unsigned>(k);
      }
    }
    return bits;
  }

  void SetBitsList(
    lua_State* const L,
    const char* const key,
    const unsigned bits
  )
  {
    std::vector<double> list;
    for (unsigned k = 0; k < 7; ++k) {
      if ((bits & (1U << k)) != 0) {
        list.push_back(static_cast<double>(k));
      }
    }
    up::SetNumberList(L, key, list);
  }

  [[nodiscard]] fe::VaultQuery ReadQuery(
    lua_State* const L,
    const int index
  )
  {
    fe::VaultQuery q;
    q.text = up::FieldString(L, index, "text");
    q.author = up::FieldString(L, index, "author");
    q.uploadedFrom = up::FieldString(L, index, "uploadedFrom");
    q.uploadedTo = up::FieldString(L, index, "uploadedTo");
    q.widths = BitsOf(L, index, "widths");
    q.heights = BitsOf(L, index, "heights");
    q.minPlayers = up::FieldInt(L, index, "minPlayers", 0);
    q.maxPlayers = up::FieldInt(L, index, "maxPlayers", 0);
    q.minScore = static_cast<float>(up::FieldNumber(L, index, "minScore", 0.0));
    q.maxScore = static_cast<float>(up::FieldNumber(L, index, "maxScore", 5.0));
    q.rankedOnly = up::FieldBool(L, index, "rankedOnly", false);
    const std::string sort = up::FieldString(L, index, "sortProperty");
    if (!sort.empty()) {
      q.sortProperty = sort;
    }
    q.descending = up::FieldBool(L, index, "descending", true);
    return q;
  }

  void PushQuery(
    lua_State* const L,
    const fe::VaultQuery& q
  )
  {
    lua_newtable(L);
    up::SetString(L, "text", q.text);
    up::SetString(L, "author", q.author);
    up::SetString(L, "uploadedFrom", q.uploadedFrom);
    up::SetString(L, "uploadedTo", q.uploadedTo);
    SetBitsList(L, "widths", q.widths);
    SetBitsList(L, "heights", q.heights);
    up::SetNumber(L, "minPlayers", q.minPlayers);
    up::SetNumber(L, "maxPlayers", q.maxPlayers);
    up::SetNumber(L, "minScore", q.minScore);
    up::SetNumber(L, "maxScore", q.maxScore);
    up::SetBool(L, "rankedOnly", q.rankedOnly);
    up::SetString(L, "sortProperty", q.sortProperty);
    up::SetBool(L, "descending", q.descending);
  }

  void PushMaps(
    lua_State* const L,
    const std::vector<fe::VaultMap>& maps,
    const int pages
  )
  {
    lua_newtable(L);
    lua_pushstring(L, "maps");
    lua_newtable(L);
    for (std::size_t i = 0; i < maps.size(); ++i) {
      const fe::VaultMap& m = maps[i];
      lua_newtable(L);
      up::SetString(L, "name", m.name);
      up::SetString(L, "folder", m.folder);
      up::SetString(L, "author", m.author);
      up::SetString(L, "description", m.description);
      up::SetString(L, "thumbnail", m.thumbnail);
      up::SetString(L, "version", m.version);
      up::SetNumber(L, "players", m.players);
      up::SetNumber(L, "width", m.width);
      up::SetNumber(L, "height", m.height);
      up::SetNumber(L, "gamesPlayed", m.gamesPlayed);
      up::SetBool(L, "ranked", m.ranked);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
    up::SetNumber(L, "pages", pages);
  }

  // ---- natives: pure helpers
  // ------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_VaultMeta(
    lua_State* const L
  )
  {
    lua_newtable(L);
    lua_pushstring(L, "sizes");
    lua_newtable(L);
    for (int k = 0; k < 7; ++k) {
      lua_newtable(L);
      up::SetString(L, "name", fe::kVaultSizeNames[k]);
      up::SetNumber(L, "pixels", fe::kVaultSizes[k]);
      lua_rawseti(L, -2, k + 1);
    }
    lua_settable(L, -3);
    lua_pushstring(L, "sorts");
    lua_newtable(L);
    int i = 1;
    for (const fe::VaultSortKey& key : fe::kVaultSortKeys) {
      lua_newtable(L);
      up::SetString(L, "property", key.property);
      up::SetString(L, "label", key.label);
      lua_rawseti(L, -2, i++);
    }
    lua_settable(L, -3);
    return 1;
  }

  int __cdecl cfunc_VaultFilter(
    lua_State* const L
  )
  {
    const std::string text = fe::VaultQueryFilter(ReadQuery(L, 1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  int __cdecl cfunc_VaultUrl(
    lua_State* const L
  )
  {
    const std::string text = fe::VaultSearchUrl(ReadQuery(L, 1), static_cast<int>(luaL_optnumber(L, 2, 1)));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  int __cdecl cfunc_VaultQueryText(
    lua_State* const L
  )
  {
    const std::string text = fe::VaultQueryText(ReadQuery(L, 1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  int __cdecl cfunc_VaultQueryParse(
    lua_State* const L
  )
  {
    fe::VaultQuery q;
    if (!fe::VaultQueryFromText(up::ArgString(L, 1, "ed_vault_query_parse"), q)) {
      return up::Fail(L, "not a saved query");
    }
    PushQuery(L, q);
    return 1;
  }

  int __cdecl cfunc_VaultParseSearch(
    lua_State* const L
  )
  {
    std::vector<fe::VaultMap> maps;
    int pages = 0;
    std::string error;
    if (!fe::ParseVaultSearch(up::ArgString(L, 1, "ed_vault_parse_search"), maps, pages, error)) {
      return up::Fail(L, error);
    }
    PushMaps(L, maps, pages);
    return 1;
  }

  int __cdecl cfunc_VaultParseName(
    lua_State* const L
  )
  {
    fe::VaultMapName name;
    std::string error;
    if (!fe::ParseVaultMapName(up::ArgString(L, 1, "ed_vault_parse_name"), name, error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    up::SetString(L, "folder", name.folder);
    up::SetString(L, "url", name.url);
    return 1;
  }

  /** The vault the FAF client set: `custom_vault_path` in fa_path.lua (the file the init file reads, in the folder
   * above the init file's). Empty without one. */
  [[nodiscard]] fs::path ClientVaultFolder()
  {
    const fs::path script = moho::DISK_GetDataPathScriptFile();
    if (script.empty()) {
      return {};
    }
    std::ifstream file(script.parent_path().parent_path() / "fa_path.lua");
    std::string line;
    while (std::getline(file, line)) {
      const std::size_t name = line.find("custom_vault_path");
      const std::size_t open = name == std::string::npos ? name : line.find('"', name);
      const std::size_t close = open == std::string::npos ? open : line.find('"', open + 1);
      if (close == std::string::npos) {
        continue;
      }
      const std::string value = line.substr(open + 1, close - open - 1);
      fs::path folder = PathOf(value);
      // The client's "relative game paths" option writes it without the home folder.
      if (!folder.empty() && !folder.has_root_name()) {
        PWSTR profile = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) {
          folder = fs::path(profile) / folder.relative_path();
          ::CoTaskMemFree(profile);
        }
      }
      return folder;
    }
    return {};
  }

  int __cdecl cfunc_VaultMapsFolder(
    lua_State* const L
  )
  {
    fs::path vault = ClientVaultFolder();
    std::error_code ec;
    if (vault.empty() || !fs::is_directory(vault, ec)) {
      vault.clear();
      PWSTR documents = nullptr;
      if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        vault = fs::path(documents) / "My Games" / "Gas Powered Games" / "Supreme Commander Forged Alliance";
        ::CoTaskMemFree(documents);
      }
    }
    if (vault.empty()) {
      return up::Fail(L, "no vault folder");
    }
    const std::string text = PathText(vault / "maps");
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  int __cdecl cfunc_VaultScenario(
    lua_State* const L
  )
  {
    const fs::path folder = PathOf(up::ArgString(L, 1, "ed_vault_scenario"));
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
      std::string name = entry.path().filename().string();
      std::string lower = name;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      constexpr char kTail[] = "_scenario.lua";
      constexpr std::size_t kTailSize = sizeof(kTail) - 1;
      if (lower.size() > kTailSize && lower.compare(lower.size() - kTailSize, kTailSize, kTail) == 0) {
        name.resize(name.size() - kTailSize);
        lua_pushlstring(L, name.data(), name.size());
        return 1;
      }
    }
    lua_pushnil(L);
    return 1;
  }

  int __cdecl cfunc_VaultSealTest(
    lua_State* const L
  )
  {
    const fs::path file = PathOf(up::ArgString(L, 1, "ed_vault_seal_test"));
    std::string text = up::ArgString(L, 2, "ed_vault_seal_test");
    DATA_BLOB input{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(text.data())};
    DATA_BLOB output{};
    bool ok = false;
    if (::CryptProtectData(&input, L"FAF map editor login", nullptr, nullptr, nullptr, 0, &output)) {
      std::error_code ec;
      fs::create_directories(file.parent_path(), ec);
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
      ok = static_cast<bool>(out);
      ::LocalFree(output.pbData);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  // ---- natives: the session
  // ---------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_VaultStart(
    lua_State* const L
  )
  {
    Vault& v = State();
    const fs::path tokenFile = PathOf(up::FieldString(L, 1, "tokenFile"));
    const fs::path cache = PathOf(up::FieldString(L, 1, "pictureCache"));
    if (v.started) {
      lua_pushboolean(L, 1);
      return 1;
    }
    v.started = true;
    v.tokenFile = tokenFile;
    v.pictureCache = cache;
    fe::SetFafLog(&LogLine);
    if (!cache.empty()) {
      v.session.SetPictureCache(cache);
    }
    if (!tokenFile.empty()) {
      v.session.SetTokenFile(tokenFile);
      if (up::FieldBool(L, 1, "resume", true)) {
        v.session.Resume();
      }
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  [[nodiscard]] const char* StateName(
    const fe::EdFafSession::State state
  )
  {
    switch (state) {
    case fe::EdFafSession::State::Working:
      return "working";
    case fe::EdFafSession::State::WaitingForApproval:
      return "waiting";
    case fe::EdFafSession::State::LoggedIn:
      return "loggedin";
    case fe::EdFafSession::State::LoggedOut:
    default:
      return "loggedout";
    }
  }

  int __cdecl cfunc_VaultStatus(
    lua_State* const L
  )
  {
    Vault& v = State();
    const fe::EdFafSession::State state = v.session.GetState();
    lua_newtable(L);
    up::SetString(L, "state", StateName(state));
    up::SetString(L, "user", v.session.UserName());
    up::SetString(L, "message", v.session.Message());
    up::SetBool(L, "searching", v.session.Searching());
    if (state == fe::EdFafSession::State::WaitingForApproval) {
      up::SetString(L, "userCode", v.session.UserCode());
      up::SetString(L, "verifyUrl", v.session.VerifyUrl());
      up::SetNumber(L, "secondsLeft", v.session.SecondsLeft());
    }
    up::SetBool(L, "openPage", v.session.TakeOpenPage());
    return 1;
  }

  int __cdecl cfunc_VaultLogin(
    lua_State*
  )
  {
    State().session.StartLogin();
    return 0;
  }

  int __cdecl cfunc_VaultCancelLogin(
    lua_State*
  )
  {
    State().session.CancelLogin();
    return 0;
  }

  int __cdecl cfunc_VaultLogout(
    lua_State*
  )
  {
    State().session.Logout();
    return 0;
  }

  int __cdecl cfunc_VaultSearch(
    lua_State* const L
  )
  {
    Vault& v = State();
    if (v.session.GetState() != fe::EdFafSession::State::LoggedIn) {
      return up::Fail(L, "sign in to FAF to search the vault");
    }
    if (v.session.Searching()) {
      return up::Fail(L, "a search is still running");
    }
    v.session.Search(ReadQuery(L, 1), std::max(1, static_cast<int>(luaL_optnumber(L, 2, 1))));
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_VaultPoll(
    lua_State* const L
  )
  {
    std::vector<fe::VaultMap> maps;
    int pages = 0;
    if (!State().session.TakeResults(maps, pages)) {
      lua_pushnil(L);
      return 1;
    }
    PushMaps(L, maps, pages);
    return 1;
  }

  /** A picture's file in the preview cache: <cache>/<small|large>/<last part of the link> (what EdFafSession::Picture
   * names it). */
  [[nodiscard]] fs::path PictureFile(
    const std::string& url
  )
  {
    const Vault& v = State();
    if (v.pictureCache.empty() || url.empty()) {
      return {};
    }
    const std::string file = url.substr(url.find_last_of('/') + 1);
    if (file.empty() || file.find_first_of("\\:") != std::string::npos || file == "." || file == "..") {
      return {};
    }
    const char* const size = url.find("/small/") != std::string::npos ? "small" : "large";
    return v.pictureCache / size / PathOf(file);
  }

  int __cdecl cfunc_VaultPicture(
    lua_State* const L
  )
  {
    const std::string url = up::ArgString(L, 1, "ed_vault_picture");
    const fs::path file = PictureFile(url);
    if (file.empty()) {
      lua_pushboolean(L, 0);
      return 1;
    }
    std::error_code ec;
    if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) > 1) {
      const std::string text = PathText(file);
      lua_pushlstring(L, text.data(), text.size());
      return 1;
    }
    // Not on disk: the session reads the FAF client's cache or downloads it (on its worker thread) and answers with the
    // bytes once it has them.
    const std::string bytes = State().session.Picture(url);
    if (bytes.empty()) {
      lua_pushnil(L);
      return 1;
    }
    if (bytes.size() <= 1) {
      lua_pushboolean(L, 0); // tried and failed
      return 1;
    }
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      lua_pushboolean(L, 0);
      return 1;
    }
    out.close();
    const std::string text = PathText(file);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  // ---- natives: the download
  // ---------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_VaultDownload(
    lua_State* const L
  )
  {
    fe::VaultMapName map;
    std::string error;
    if (!fe::ParseVaultMapName(up::ArgString(L, 1, "ed_vault_download"), map, error)) {
      return up::Fail(L, error);
    }
    const std::string maps = up::ArgString(L, 2, "ed_vault_download");
    if (maps.empty()) {
      return up::Fail(L, "no maps folder to install into");
    }
    if (!State().download.Start(map, PathOf(maps))) {
      return up::Fail(L, "another download is running");
    }
    gpg::Logf("editor: vault: downloading %s", map.url.c_str());
    lua_newtable(L);
    up::SetString(L, "folder", map.folder);
    up::SetString(L, "url", map.url);
    return 1;
  }

  int __cdecl cfunc_VaultDownloadStatus(
    lua_State* const L
  )
  {
    const fe::EdVaultDownload& d = State().download;
    const char* name = "idle";
    switch (d.GetState()) {
    case fe::EdVaultDownload::State::Working:
      name = "working";
      break;
    case fe::EdVaultDownload::State::Done:
      name = "done";
      break;
    case fe::EdVaultDownload::State::Failed:
      name = "failed";
      break;
    case fe::EdVaultDownload::State::Idle:
    default:
      break;
    }
    lua_newtable(L);
    up::SetString(L, "state", name);
    up::SetString(L, "message", d.Message());
    up::SetNumber(L, "received", static_cast<double>(d.Received()));
    up::SetNumber(L, "total", static_cast<double>(d.Total()));
    up::SetString(L, "map", d.Map().folder);
    if (d.GetState() == fe::EdVaultDownload::State::Done) {
      up::SetString(L, "folder", PathText(d.Folder()));
    }
    return 1;
  }

  int __cdecl cfunc_VaultDownloadReset(
    lua_State*
  )
  {
    State().download.Reset();
    return 0;
  }

  // ---- natives: links
  // ----------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_OpenUrl(
    lua_State* const L
  )
  {
    const std::string url = up::ArgString(L, 1, "ed_open_url");
    // Only web links: the sign-in page and a map's page. A file path or another scheme would run a program.
    const bool web = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
    if (!web || url.size() > 2048 || url.find_first_of("\r\n\"") != std::string::npos) {
      return up::Fail(L, "only http and https links are opened");
    }
    const int wide = ::MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring link(static_cast<std::size_t>(std::max(wide, 1)), L'\0');
    if (wide > 0) {
      (void)::MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, link.data(), wide);
      link.resize(static_cast<std::size_t>(wide - 1));
    }
    // ShellExecute needs COM set up on the thread that calls it.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto result =
      reinterpret_cast<std::intptr_t>(::ShellExecuteW(nullptr, L"open", link.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (SUCCEEDED(com)) {
      ::CoUninitialize();
    }
    if (result <= 32) {
      char why[96];
      (void)std::snprintf(
        why, sizeof(why), "the browser could not be started (ShellExecute %lld)", static_cast<long long>(result)
      );
      gpg::Logf("editor: couldn't open a link in the browser (ShellExecute %lld)", static_cast<long long>(result));
      return up::Fail(L, why);
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  const up::Natives gEdUpVaultNatives{{
    {"ed_vault_start", &cfunc_VaultStart, "ed_vault_start{tokenFile, pictureCache, resume} -> true"},
    {"ed_vault_status",
     &cfunc_VaultStatus,
     "ed_vault_status() -> { state, user, message, userCode, verifyUrl, secondsLeft, searching, openPage }"},
    {"ed_vault_login", &cfunc_VaultLogin, "ed_vault_login()"},
    {"ed_vault_cancel_login", &cfunc_VaultCancelLogin, "ed_vault_cancel_login()"},
    {"ed_vault_logout", &cfunc_VaultLogout, "ed_vault_logout()"},
    {"ed_vault_search", &cfunc_VaultSearch, "ed_vault_search(query, page) -> true | nil, why"},
    {"ed_vault_poll", &cfunc_VaultPoll, "ed_vault_poll() -> nil | { maps, pages }"},
    {"ed_vault_picture", &cfunc_VaultPicture, "ed_vault_picture(url) -> path | false | nil"},
    {"ed_vault_download",
     &cfunc_VaultDownload,
     "ed_vault_download(nameOrLink, mapsFolder) -> { folder, url } | nil, why"},
    {"ed_vault_download_status",
     &cfunc_VaultDownloadStatus,
     "ed_vault_download_status() -> { state, message, received, total, map, folder }"},
    {"ed_vault_download_reset", &cfunc_VaultDownloadReset, "ed_vault_download_reset()"},
    {"ed_vault_maps_folder", &cfunc_VaultMapsFolder, "ed_vault_maps_folder() -> path | nil, why"},
    {"ed_vault_scenario", &cfunc_VaultScenario, "ed_vault_scenario(folder) -> stem | nil"},
    {"ed_open_url", &cfunc_OpenUrl, "ed_open_url(url) -> true | nil, why"},
    {"ed_vault_meta", &cfunc_VaultMeta, "ed_vault_meta() -> { sizes, sorts }"},
    {"ed_vault_filter", &cfunc_VaultFilter, "ed_vault_filter(query) -> RSQL text"},
    {"ed_vault_url", &cfunc_VaultUrl, "ed_vault_url(query, page) -> link"},
    {"ed_vault_query_text", &cfunc_VaultQueryText, "ed_vault_query_text(query) -> line"},
    {"ed_vault_query_parse", &cfunc_VaultQueryParse, "ed_vault_query_parse(line) -> query | nil, why"},
    {"ed_vault_parse_search", &cfunc_VaultParseSearch, "ed_vault_parse_search(json) -> { maps, pages } | nil, why"},
    {"ed_vault_parse_name", &cfunc_VaultParseName, "ed_vault_parse_name(text) -> { folder, url } | nil, why"},
    {"ed_vault_seal_test", &cfunc_VaultSealTest, "ed_vault_seal_test(file, text) -> bool"},
  }};
} // namespace
