// EDITOR: the "up" natives of the exact save (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP3):
// upstream's classic `.scmap` writer with its stored records, the text-preserving `_save.lua` / `_scenario.lua` edit
// and the vault zip, bound to Lua over EdUpCommon.h. The logic is upstream's (src/editor/doc/EdMapFile, EdImageChunks,
// EdSaveFile, EdLuaText, EdPackage); this unit holds only what ours adds to it: where the "unchanged" evidence comes
// from (the live heights, water and masks of the session, not an EdDocument), the round trip check, and the table <->
// struct glue.
//
//   ed_mapfile_capture(vfsMapPath)       -> { images = n, props = n, preview = bytes } | nil, message
//        Reads the records of the file the open session's map was loaded from (the preview chunk, the DDS image chunks,
//        the props' 15 floats) and remembers what the writer needs to tell "unchanged" later: a hash of the heights,
//        the water levels and planes, the strata mask revision. Call it right after the map is loaded, before anything
//        is edited (map.lua LoadDocument does).
//   ed_mapfile_state()                   -> { captured, heights, water, masks, images, props } (heights / water /
//   masks: still as captured) ed_mapfile_save(diskPath[, { engine = bool }]) -> { writer = 'exact' | 'engine', kept =
//   n, images = n } | nil, message
//        'exact': upstream's WriteMapFile with the captured records: an untouched map writes the bytes it was read
//        from, an edited one changes only what was edited. 'engine': CWldMap::MapSaveToDiskPath, used when `engine =
//        true` (mesh terrain maps), when nothing was captured, or when the captured file was not a classic v2 `.scmap`
//        (container 3 / 4 maps keep the engine's own writer; this writer is version 60 only).
//   ed_mapfile_roundtrip(folder[, deep]) -> { ok, stable, sameAsOriginal, differences } | nil, message
//        The save check of the editor's board task WPE.5 on the open map (upstream's EdRoundTrip without EdDocument):
//        saves into `folder`, saves again, compares the two files and the original; `deep` also reads the file back
//        into a second map and compares what was read.
//   ed_game_file_read(vfsPath)           -> string or nil     a whole game file (folder or archive)
//   ed_savefile_entries(text)            -> { markers, areas, armies, chains } | nil, message         the blocks of a
//   `_save.lua` as SaveFile parses them ed_savefile_write(text, entries)     -> text | nil, message     the file's text
//   with the entries of the given blocks written back, the rest as it was ed_luatext_fields(text, name)        -> {
//   {key, text, from, to}, ... }, { from, to } | nil, message     the fields of `name = { ... }` in file order (1-based
//   inclusive text positions), and the table's own span ed_package_plan(diskFolder)          -> { folder, version,
//   files = n, errors = {..}, warnings = {..} } ed_package_start(diskFolder, zipPath) -> job ; ed_package_poll(job) ->
//   { done, progress, ok, error, errors, warnings, files, bytes } ; _cancel / _free

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "editor/doc/EdImageChunks.h"
#include "editor/doc/EdLuaText.h"
#include "editor/doc/EdMapFile.h"
#include "editor/doc/EdPackage.h"
#include "editor/doc/EdSaveFile.h"
#include "lua/LuaObject.h"
#include "moho/editor/EdStrata.h"
#include "moho/editor/EdUpCommon.h"
#include "moho/editor/EditorDocument.h"
#include "moho/misc/ID3DDeviceResources.h"
#include "moho/render/textures/CD3DDynamicTextureSheet.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"
#include "moho/terrain/CWldTerrainRes.h"
#include "moho/terrain/StratumMaterial.h"

namespace
{
  namespace up = moho::editor::up;
  namespace fs = std::filesystem;
  using faf::editor::MapFileRecords;

  // ---- what the writer knows of the map as it was loaded
  // -------------------------------------------------------------------------------------------

  struct WaterSources
  {
    std::uint8_t enabled = 0;
    float elevation = 0.0f;
    float deep = 0.0f;
    float abyss = 0.0f;
    std::uint64_t planes =
      0; // foam, flatness and depth bias: the water map is made from them, the levels and the heights
    bool operator==(
      const WaterSources& o
    ) const
    {
      return enabled == o.enabled && std::memcmp(&elevation, &o.elevation, sizeof(float)) == 0 &&
        std::memcmp(&deep, &o.deep, sizeof(float)) == 0 && std::memcmp(&abyss, &o.abyss, sizeof(float)) == 0 &&
        planes == o.planes;
    }
  };

  struct Capture
  {
    bool valid = false;
    const void* terrain = nullptr; // the terrain the records belong to: a new session has another
    MapFileRecords records;
    std::string source;
    std::uint64_t heights = 0;
    WaterSources water;
    std::uint64_t maskRevision = 0;
  };

  Capture& TheCapture()
  {
    static Capture capture;
    return capture;
  }

  [[nodiscard]] std::uint64_t Fnv(
    const void* const data,
    const std::size_t size,
    std::uint64_t hash = 14695981039346656037ULL
  )
  {
    const auto* const bytes = static_cast<const unsigned char*>(data);
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
      std::uint64_t word = 0;
      std::memcpy(&word, bytes + i, sizeof(word));
      hash = (hash ^ word) * 1099511628211ULL;
      hash ^= hash >> 29;
    }
    for (; i < size; ++i) {
      hash = (hash ^ bytes[i]) * 1099511628211ULL;
    }
    return hash;
  }

  [[nodiscard]] moho::CWldMap* ActiveMap()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    return session != nullptr ? session->mWldMap : nullptr;
  }

  [[nodiscard]] moho::CWldTerrainRes* ActiveTerrain()
  {
    moho::CWldMap* const map = ActiveMap();
    return map != nullptr ? static_cast<moho::CWldTerrainRes*>(map->mTerrainRes) : nullptr;
  }

  [[nodiscard]] std::uint64_t HeightsHash(
    moho::CWldTerrainRes& terrain
  )
  {
    const moho::CHeightField* const field = terrain.GetHeightField();
    if (field == nullptr || field->data == nullptr) {
      return 0;
    }
    return Fnv(
      field->data,
      static_cast<std::size_t>(field->width) * static_cast<std::size_t>(field->height) * sizeof(field->data[0])
    );
  }

  [[nodiscard]] WaterSources WaterNow(
    moho::CWldTerrainRes& terrain
  )
  {
    WaterSources water;
    if (terrain.mMap != nullptr) {
      water.enabled = terrain.mMap->mWaterEnabled;
      water.elevation = terrain.mMap->mWaterElevation;
      water.deep = terrain.mMap->mWaterElevationDeep;
      water.abyss = terrain.mMap->mWaterElevationAbyss;
    }
    Wm3::Vector3f size{};
    if (terrain.mWaterMapTexture.get() != nullptr) {
      (void)terrain.mWaterMapTexture->GetDimensions(&size);
    }
    const std::size_t bytes = static_cast<std::size_t>(static_cast<std::int32_t>(size.x * size.y));
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::uint8_t* const plane :
         {terrain.GetWaterFoam(), terrain.GetWaterFlatness(), terrain.GetWaterDepthBias()}) {
      if (plane != nullptr && bytes != 0) {
        hash = Fnv(plane, bytes, hash);
      }
    }
    water.planes = hash;
    return water;
  }

  void ForgetCapture()
  {
    TheCapture() = Capture{};
  }

  // ---- ed_mapfile_*
  // ----------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_MapFileCapture(
    lua_State* const L
  )
  {
    const std::string source = up::ArgString(L, 1, "ed_mapfile_capture");
    moho::CWldMap* const map = ActiveMap();
    moho::CWldTerrainRes* const terrain = ActiveTerrain();
    ForgetCapture();
    if (map == nullptr || terrain == nullptr || map->mProps == nullptr) {
      return up::Fail(L, "no map is open");
    }
    Capture capture;
    capture.source = source;
    capture.records.preview = faf::editor::ReadPreviewRecord(source);
    if (capture.records.preview.empty()) {
      return up::Fail(L, "not a classic .scmap (no preview chunk): the engine's writer is used");
    }
    capture.records.images = faf::editor::ReadImageRecords(source, capture.records.preview.size());
    capture.records.props = faf::editor::ReadPropRecords(source, *map);
    capture.terrain = terrain;
    capture.heights = HeightsHash(*terrain);
    capture.water = WaterNow(*terrain);
    capture.maskRevision = moho::editor::StratumMaskRevision();
    capture.valid = capture.records.images.size() >= 3;
    TheCapture() = std::move(capture);
    const Capture& kept = TheCapture();
    lua_newtable(L);
    up::SetNumber(L, "images", static_cast<double>(kept.records.images.size()));
    up::SetNumber(L, "props", static_cast<double>(kept.records.props.size()));
    up::SetNumber(L, "preview", static_cast<double>(kept.records.preview.size()));
    up::SetBool(L, "valid", kept.valid);
    return 1;
  }

  struct Unchanged
  {
    bool heights = false;
    bool water = false;
    bool masks = false;
  };

  [[nodiscard]] Unchanged UnchangedNow(
    const Capture& capture,
    moho::CWldTerrainRes& terrain
  )
  {
    Unchanged u;
    u.heights = HeightsHash(terrain) == capture.heights;
    u.water = u.heights && WaterNow(terrain) == capture.water;
    u.masks = moho::editor::StratumMaskRevision() == capture.maskRevision;
    return u;
  }

  int __cdecl cfunc_MapFileState(
    lua_State* const L
  )
  {
    const Capture& capture = TheCapture();
    moho::CWldTerrainRes* const terrain = ActiveTerrain();
    lua_newtable(L);
    const bool usable = capture.valid && terrain != nullptr && capture.terrain == terrain;
    up::SetBool(L, "captured", usable);
    if (usable) {
      const Unchanged u = UnchangedNow(capture, *terrain);
      up::SetBool(L, "heights", u.heights);
      up::SetBool(L, "water", u.water);
      up::SetBool(L, "masks", u.masks);
      up::SetNumber(L, "images", static_cast<double>(capture.records.images.size()));
      up::SetNumber(L, "props", static_cast<double>(capture.records.props.size()));
    }
    return 1;
  }

  /** The images in the file's order: the normal maps, the two stratum masks, the water map; each kept while what it is
   * made from is as loaded. */
  [[nodiscard]] std::vector<bool> KeepImages(
    const Capture& capture,
    moho::CWldTerrainRes& terrain
  )
  {
    const std::size_t count = capture.records.images.size();
    std::vector<bool> keep(count, false);
    if (count < 3) {
      return keep;
    }
    const Unchanged u = UnchangedNow(capture, terrain);
    for (std::size_t i = 0; i < count; ++i) {
      keep[i] = i + 1 == count ? u.water : i + 3 >= count ? u.masks : u.heights;
    }
    return keep;
  }

  /** The exact writer on the open map: false with `error`. `kept` is how many of the file's images went back as they
   * were. */
  [[nodiscard]] bool SaveExact(
    const fs::path& path,
    std::size_t& kept,
    std::string& error
  )
  {
    moho::CWldMap* const map = ActiveMap();
    moho::CWldTerrainRes* const terrain = ActiveTerrain();
    Capture& capture = TheCapture();
    if (map == nullptr || terrain == nullptr) {
      error = "no map is open";
      return false;
    }
    // Until the terrain is ready for drawing (Finalize builds its normal map, which the file carries) the writer would
    // write leftover memory.
    if (!terrain->GetBool()) {
      error = "the map is not ready yet (its terrain is still being prepared)";
      return false;
    }
    capture.records.keepImages = KeepImages(capture, *terrain);
    kept =
      static_cast<std::size_t>(std::count(capture.records.keepImages.begin(), capture.records.keepImages.end(), true));
    return faf::editor::WriteMapFile(*map, capture.records, path, error);
  }

  int __cdecl cfunc_MapFileSave(
    lua_State* const L
  )
  {
    const std::string path = up::ArgString(L, 1, "ed_mapfile_save");
    const bool forceEngine = up::FieldBool(L, 2, "engine", false);
    moho::CWldMap* const map = ActiveMap();
    if (map == nullptr) {
      return up::Fail(L, "no map is open");
    }
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) {
      fs::create_directories(target.parent_path(), ec);
    }
    const Capture& capture = TheCapture();
    const bool usable = !forceEngine && capture.valid && capture.terrain == ActiveTerrain();
    lua_newtable(L);
    if (usable) {
      std::size_t kept = 0;
      std::string error;
      if (!SaveExact(target, kept, error)) {
        lua_settop(L, -2);
        return up::Fail(L, error);
      }
      up::SetString(L, "writer", "exact");
      up::SetNumber(L, "kept", static_cast<double>(kept));
      up::SetNumber(L, "images", static_cast<double>(capture.records.images.size()));
      return 1;
    }
    if (!map->MapSaveToDiskPath(path.c_str())) {
      lua_settop(L, -2);
      return up::Fail(L, "the engine could not write " + path);
    }
    up::SetString(L, "writer", "engine");
    return 1;
  }

  /** A whole game file by its virtual path (`/maps/x/x_save.lua`, from a folder or an archive): the text a save is made
   * against. */
  int __cdecl cfunc_GameFileRead(
    lua_State* const L
  )
  {
    std::string bytes;
    if (!faf::editor::ReadGameFile(up::ArgString(L, 1, "ed_game_file_read"), bytes)) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushlstring(L, bytes.data(), bytes.size());
    return 1;
  }

  // ---- the round trip check
  // ----------------------------------------------------------------------------------------------------------------------------

  [[nodiscard]] std::vector<char> FileBytes(
    const fs::path& path
  )
  {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  void Note(
    std::string& differences,
    const std::string& what
  )
  {
    if (!differences.empty()) {
      differences += "; ";
    }
    differences += what;
  }

  /** What a second map read from the saved file holds, against the open one (the part of upstream's EdRoundTrip that
   * needs no document). */
  void CompareRead(
    moho::CWldMap& a,
    moho::CWldMap& b,
    std::string& differences
  )
  {
    const moho::CHeightField* const fa = a.mTerrainRes->GetHeightField();
    const moho::CHeightField* const fb = b.mTerrainRes->GetHeightField();
    if (fa == nullptr || fb == nullptr || fa->width != fb->width || fa->height != fb->height) {
      Note(differences, "height field size");
    } else if (
      std::memcmp(
        fa->data, fb->data, static_cast<std::size_t>(fa->width) * static_cast<std::size_t>(fa->height) * 2u
      ) != 0
    ) {
      Note(differences, "heights");
    }
    // (a map without water is written with the -10000 sentinel for its levels: only the flag counts then)
    if (
      a.mTerrainRes->IsWaterEnabled() != b.mTerrainRes->IsWaterEnabled() ||
      (a.mTerrainRes->IsWaterEnabled() && a.mTerrainRes->GetWaterElevation() != b.mTerrainRes->GetWaterElevation())
    ) {
      Note(differences, "water");
    }
    const auto& pa = a.mProps->mEntries;
    const auto& pb = b.mProps->mEntries;
    if (pa.size() != pb.size()) {
      Note(differences, "prop count");
    } else {
      for (std::size_t i = 0; i < pa.size(); ++i) {
        const bool path = std::strcmp(pa[i].mBlueprintPath.c_str(), pb[i].mBlueprintPath.c_str()) != 0;
        const bool position =
          std::memcmp(&pa[i].mTransform.pos_, &pb[i].mTransform.pos_, sizeof(pa[i].mTransform.pos_)) != 0;
        // (a rotation that went through the file as a matrix may come back a few bits apart; a changed rotation is far
        // more)
        const Wm3::Quatf& qa = pa[i].mTransform.orient_;
        const Wm3::Quatf& qb = pb[i].mTransform.orient_;
        const bool orientation = std::fabs(qa.w - qb.w) > 1e-5f || std::fabs(qa.x - qb.x) > 1e-5f ||
          std::fabs(qa.y - qb.y) > 1e-5f || std::fabs(qa.z - qb.z) > 1e-5f;
        const bool scale = std::memcmp(&pa[i].mScale, &pb[i].mScale, sizeof(pa[i].mScale)) != 0;
        if (path || position || orientation || scale) {
          Note(
            differences,
            "prop " + std::to_string(i) + (path ? " path" : "") + (position ? " position" : "") +
              (orientation ? " rotation" : "") + (scale ? " scale" : "")
          );
          break;
        }
      }
    }
  }

  int __cdecl cfunc_MapFileRoundTrip(
    lua_State* const L
  )
  {
    const std::string folder = up::ArgString(L, 1, "ed_mapfile_roundtrip");
    const bool deep =
      up::FieldBool(L, 2, "deep", false) || (lua_type(L, 2) == LUA_TBOOLEAN && lua_toboolean(L, 2) != 0);
    moho::CWldMap* const map = ActiveMap();
    const Capture& capture = TheCapture();
    if (map == nullptr || !capture.valid || capture.terrain != ActiveTerrain()) {
      return up::Fail(L, "no captured map (ed_mapfile_capture first)");
    }
    std::error_code ignored;
    fs::create_directories(fs::path(folder), ignored);
    const fs::path first = fs::path(folder) / "roundtrip.1.scmap";
    const fs::path second = fs::path(folder) / "roundtrip.2.scmap";
    std::string differences;
    bool stable = false;
    bool sameAsOriginal = false;
    std::size_t kept = 0;
    std::string error;
    if (!SaveExact(first, kept, error)) {
      return up::Fail(L, "save: " + error);
    }
    if (deep) {
      MapFileRecords reread;
      std::unique_ptr<moho::CWldMap> other =
        faf::editor::ReadMapFile(first, LuaPlus::LuaState::CastState(L), reread, error);
      if (other == nullptr) {
        Note(differences, "read back: " + error);
      } else {
        CompareRead(*map, *other, differences);
      }
    }
    if (!SaveExact(second, kept, error)) {
      Note(differences, "second save: " + error);
    } else {
      const std::vector<char> firstBytes = FileBytes(first);
      stable = !firstBytes.empty() && firstBytes == FileBytes(second);
      if (!stable) {
        Note(differences, "saving it again gave other bytes");
      }
      const fs::path original = faf::editor::DiskFileOf(capture.source);
      if (!original.empty()) {
        sameAsOriginal = FileBytes(original) == firstBytes;
      }
      fs::remove(second, ignored);
    }
    lua_newtable(L);
    up::SetBool(L, "ok", differences.empty());
    up::SetBool(L, "stable", stable);
    up::SetBool(L, "sameAsOriginal", sameAsOriginal);
    up::SetString(L, "differences", differences);
    up::SetString(L, "file", first.string());
    up::SetNumber(L, "kept", static_cast<double>(kept));
    return 1;
  }

  // ---- _save.lua
  // --------------------------------------------------------------------------------------------------------------------------------------

  void PushEntries(
    lua_State* const L,
    const char* const key,
    const std::vector<faf::editor::SaveEntry>& entries
  )
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "name", entries[i].name);
      std::vector<std::string> keys;
      std::vector<std::string> values;
      for (const auto& property : entries[i].properties) {
        keys.push_back(property.first);
        values.push_back(property.second);
      }
      up::SetStringList(L, "keys", keys);
      up::SetStringList(L, "values", values);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  int __cdecl cfunc_SaveFileEntries(
    lua_State* const L
  )
  {
    faf::editor::SaveFile file;
    std::string error;
    if (!file.Parse(up::ArgString(L, 1, "ed_savefile_entries"), error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    PushEntries(L, "markers", file.Markers());
    PushEntries(L, "areas", file.Areas());
    PushEntries(L, "armies", file.Armies());
    lua_pushstring(L, "chains");
    lua_newtable(L);
    for (std::size_t i = 0; i < file.Chains().size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "name", file.Chains()[i].name);
      up::SetStringList(L, "markers", file.Chains()[i].markers);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
    return 1;
  }

  /** The entries of t[key] (an array of { name, keys, values }); false when t[key] is no table (the block is then left
   * as read). */
  [[nodiscard]] bool ReadEntries(
    lua_State* const L,
    const int table,
    const char* const key,
    std::vector<faf::editor::SaveEntry>& out
  )
  {
    if (up::PushField(L, table, key) != LUA_TTABLE) {
      lua_settop(L, -2);
      return false;
    }
    const int list = lua_gettop(L);
    for (int i = 1;; ++i) {
      lua_rawgeti(L, list, i);
      if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, -2);
        break;
      }
      const int entry = lua_gettop(L);
      faf::editor::SaveEntry item;
      item.name = up::FieldString(L, entry, "name");
      const std::vector<std::string> keys = up::FieldStringList(L, entry, "keys");
      const std::vector<std::string> values = up::FieldStringList(L, entry, "values");
      for (std::size_t k = 0; k < keys.size() && k < values.size(); ++k) {
        item.properties.emplace_back(keys[k], values[k]);
      }
      out.push_back(std::move(item));
      lua_settop(L, -2);
    }
    lua_settop(L, -2);
    return true;
  }

  int __cdecl cfunc_SaveFileWrite(
    lua_State* const L
  )
  {
    faf::editor::SaveFile file;
    std::string error;
    if (!file.Parse(up::ArgString(L, 1, "ed_savefile_write"), error)) {
      return up::Fail(L, error);
    }
    std::vector<faf::editor::SaveEntry> entries;
    if (ReadEntries(L, 2, "markers", entries)) {
      file.Markers() = std::move(entries);
    }
    entries.clear();
    if (ReadEntries(L, 2, "areas", entries)) {
      file.Areas() = std::move(entries);
    }
    entries.clear();
    if (ReadEntries(L, 2, "armies", entries)) {
      file.Armies() = std::move(entries);
    }
    if (up::PushField(L, 2, "chains") == LUA_TTABLE) {
      std::vector<faf::editor::MarkerChain> chains;
      const int list = lua_gettop(L);
      for (int i = 1;; ++i) {
        lua_rawgeti(L, list, i);
        if (lua_type(L, -1) != LUA_TTABLE) {
          lua_settop(L, -2);
          break;
        }
        faf::editor::MarkerChain chain;
        chain.name = up::FieldString(L, lua_gettop(L), "name");
        chain.markers = up::FieldStringList(L, lua_gettop(L), "markers");
        chains.push_back(std::move(chain));
        lua_settop(L, -2);
      }
      file.Chains() = std::move(chains);
    }
    lua_settop(L, -2);
    const std::string text = file.Text();
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  // ---- Lua text
  // ---------------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_LuaTextFields(
    lua_State* const L
  )
  {
    const std::string text = up::ArgString(L, 1, "ed_luatext_fields");
    const std::string name = up::ArgString(L, 2, "ed_luatext_fields");
    faf::editor::LuaText value;
    std::string error;
    if (!faf::editor::ReadLuaAssignment(text, name, value, error)) {
      return up::Fail(L, error.empty() ? name + " is not assigned" : error);
    }
    lua_newtable(L);
    int row = 0;
    for (const auto& field : value.fields) {
      lua_newtable(L);
      up::SetString(L, "key", field.first);
      up::SetString(L, "text", text.substr(field.second.start, field.second.end - field.second.start));
      up::SetNumber(L, "from", static_cast<double>(field.second.start) + 1.0);
      up::SetNumber(L, "to", static_cast<double>(field.second.end));
      lua_rawseti(L, -2, ++row);
    }
    // The assignment's own value (the table's braces): where a new field goes in.
    lua_newtable(L);
    up::SetNumber(L, "from", static_cast<double>(value.start) + 1.0);
    up::SetNumber(L, "to", static_cast<double>(value.end));
    return 2;
  }

  // ---- package
  // ------------------------------------------------------------------------------------------------------------------------------------------

  [[nodiscard]] std::string Lower(
    std::string s
  )
  {
    std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  }

  [[nodiscard]] bool EndsWith(
    const std::string& s,
    const char* const tail
  )
  {
    const std::size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
  }

  /** The files of a map folder that go to the vault: the editor's sidecar (`*_editor.lua`, FAF Studio's) is not one of
   * them, as pack_map.py left it out. */
  [[nodiscard]] std::vector<std::string> FolderFiles(
    const fs::path& folder
  )
  {
    std::vector<std::string> out;
    for (const std::string& file : faf::editor::FilesUnder(folder)) {
      if (!EndsWith(Lower(file), "_editor.lua")) {
        out.push_back(file);
      }
    }
    return out;
  }

  [[nodiscard]] std::string TopScenario(
    const std::vector<std::string>& files
  )
  {
    for (const std::string& file : files) {
      if (file.find('/') == std::string::npos && EndsWith(Lower(file), "_scenario.lua")) {
        return file;
      }
    }
    return {};
  }

  [[nodiscard]] std::string ReadAll(
    const fs::path& path
  )
  {
    const std::vector<char> bytes = FileBytes(path);
    return std::string(bytes.begin(), bytes.end());
  }

  [[nodiscard]] faf::editor::PackagePlan PlanFolder(
    const fs::path& folder,
    std::vector<std::string>& files,
    std::string& scenarioFile
  )
  {
    files = FolderFiles(folder);
    scenarioFile = TopScenario(files);
    const std::string scenario = scenarioFile.empty() ? std::string() : ReadAll(folder / scenarioFile);
    return faf::editor::PlanPackage(folder.filename().string(), scenario, files);
  }

  void PushPlan(
    lua_State* const L,
    const faf::editor::PackagePlan& plan
  )
  {
    up::SetString(L, "folder", plan.folder);
    up::SetNumber(L, "version", plan.version);
    up::SetNumber(L, "files", static_cast<double>(plan.files.size()));
    up::SetStringList(L, "errors", plan.errors);
    up::SetStringList(L, "warnings", plan.warnings);
  }

  int __cdecl cfunc_PackagePlan(
    lua_State* const L
  )
  {
    const fs::path folder = fs::path(up::ArgString(L, 1, "ed_package_plan")).lexically_normal();
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
      return up::Fail(L, "no such folder: " + folder.string());
    }
    std::vector<std::string> files;
    std::string scenarioFile;
    const fs::path trimmed = folder.has_filename() ? folder : folder.parent_path();
    const faf::editor::PackagePlan plan = PlanFolder(trimmed, files, scenarioFile);
    lua_newtable(L);
    PushPlan(L, plan);
    return 1;
  }

  struct PackageResult
  {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    faf::editor::PackagePlan plan;
    std::uint64_t bytes = 0;
  };

  up::JobSet<PackageResult>& PackageJobs()
  {
    static up::JobSet<PackageResult> jobs;
    return jobs;
  }

  PackageResult MakePackage(
    const fs::path& folder,
    const fs::path& zipPath,
    up::JobContext& job
  )
  {
    PackageResult result;
    std::vector<std::string> files;
    std::string scenarioFile;
    result.plan = PlanFolder(folder, files, scenarioFile);
    if (!result.plan.errors.empty()) {
      result.error = "the map folder does not pass the vault's checks";
      return result;
    }
    std::vector<faf::editor::ZipEntry> entries;
    for (std::size_t i = 0; i < result.plan.files.size(); ++i) {
      if (job.Cancelled()) {
        result.cancelled = true;
        return result;
      }
      const std::string& file = result.plan.files[i];
      faf::editor::ZipEntry entry;
      entry.name = result.plan.folder + "/" + file;
      entry.data = ReadAll(folder / file);
      if (file == scenarioFile) {
        entry.data = faf::editor::WithMapVersion(entry.data, result.plan.version);
      }
      entries.push_back(std::move(entry));
      job.Report(0.8f * static_cast<float>(i + 1) / static_cast<float>(result.plan.files.size()));
    }
    const std::string zip = faf::editor::WriteZip(entries);
    std::error_code ec;
    if (zipPath.has_parent_path()) {
      fs::create_directories(zipPath.parent_path(), ec);
    }
    std::ofstream out(zipPath, std::ios::binary);
    out.write(zip.data(), static_cast<std::streamsize>(zip.size()));
    out.close();
    if (!out) {
      result.error = "can't write " + zipPath.string();
      return result;
    }
    result.bytes = zip.size();
    result.ok = true;
    return result;
  }

  int __cdecl cfunc_PackageStart(
    lua_State* const L
  )
  {
    fs::path folder = fs::path(up::ArgString(L, 1, "ed_package_start")).lexically_normal();
    if (!folder.has_filename()) {
      folder = folder.parent_path();
    }
    const fs::path zipPath(up::ArgString(L, 2, "ed_package_start"));
    const int id = PackageJobs().Start([folder, zipPath](up::JobContext& job) {
      return MakePackage(folder, zipPath, job);
    });
    lua_pushnumber(L, id);
    return 1;
  }

  int __cdecl cfunc_PackagePoll(
    lua_State* const L
  )
  {
    const int id = static_cast<int>(luaL_checknumber(L, 1));
    const up::JobStatus status = PackageJobs().Poll(id);
    if (!status.known) {
      return up::Fail(L, "no such job");
    }
    up::PushJobStatus(L, status);
    if (status.done) {
      if (const auto result = PackageJobs().Get(id)) {
        up::SetBool(L, "ok", result->ok);
        up::SetBool(L, "cancelled", result->cancelled);
        up::SetString(L, "error", result->error);
        PushPlan(L, result->plan);
        up::SetNumber(L, "bytes", static_cast<double>(result->bytes));
      }
    }
    return 1;
  }

  int __cdecl cfunc_PackageCancel(
    lua_State* const L
  )
  {
    PackageJobs().Cancel(static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
  }

  int __cdecl cfunc_PackageFree(
    lua_State* const L
  )
  {
    PackageJobs().Free(static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
  }

  struct EdUpMapFileBootstrap
  {
    EdUpMapFileBootstrap()
    {
      moho::editor::OnSessionEnd([] {
        ForgetCapture();
      });
    }
  };

  const EdUpMapFileBootstrap gBootstrap{};

  const up::Natives gEdUpMapFileNatives{{
    {"ed_mapfile_capture",
     &cfunc_MapFileCapture,
     "ed_mapfile_capture(vfsMapPath) -> { images, props, preview, valid } | nil, message"},
    {"ed_mapfile_state",
     &cfunc_MapFileState,
     "ed_mapfile_state() -> { captured, heights, water, masks, images, props }"},
    {"ed_mapfile_save",
     &cfunc_MapFileSave,
     "ed_mapfile_save(diskPath[, { engine = bool }]) -> { writer = 'exact' | 'engine', kept, images } | nil, message"},
    {"ed_mapfile_roundtrip",
     &cfunc_MapFileRoundTrip,
     "ed_mapfile_roundtrip(folder[, deep]) -> { ok, stable, sameAsOriginal, differences, file } | nil, message"},
    {"ed_game_file_read",
     &cfunc_GameFileRead,
     "ed_game_file_read(vfsPath) -> string or nil (a game file from a folder or an archive)"},
    {"ed_savefile_entries",
     &cfunc_SaveFileEntries,
     "ed_savefile_entries(text) -> { markers, areas, armies, chains } | nil, message"},
    {"ed_savefile_write", &cfunc_SaveFileWrite, "ed_savefile_write(text, entries) -> text | nil, message"},
    {"ed_luatext_fields",
     &cfunc_LuaTextFields,
     "ed_luatext_fields(text, name) -> { {key, text, from, to}, ... } | nil, message"},
    {"ed_package_plan",
     &cfunc_PackagePlan,
     "ed_package_plan(diskFolder) -> { folder, version, files, errors, warnings } | nil, message"},
    {"ed_package_start", &cfunc_PackageStart, "ed_package_start(diskFolder, zipPath) -> job"},
    {"ed_package_poll",
     &cfunc_PackagePoll,
     "ed_package_poll(job) -> { done, progress, ok, error, errors, warnings, bytes } | nil, message"},
    {"ed_package_cancel", &cfunc_PackageCancel, "ed_package_cancel(job)"},
    {"ed_package_free", &cfunc_PackageFree, "ed_package_free(job)"},
  }};
} // namespace
