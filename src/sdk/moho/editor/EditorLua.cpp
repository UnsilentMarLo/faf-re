// Editor native layer, Lua side (see docs/projects/MAP_EDITOR.md, "Native API for Lua").
//
// Everything the editor needs that the game's own Lua API does not give is a plain C function bound as a
// global of the user Lua state (the UI), so the editor's behaviour stays in Lua data under editor/ and the
// binary only provides capabilities. First batch: file I/O outside the virtual file system.
//
//   ed_file_exists(path)        -> bool
//   ed_file_stamp(path)         -> string "<size>:<mtime>" or nil   (cheap change detection)
//   ed_read_file(path)          -> string or nil
//   ed_write_file(path, data)   -> bool                              (creates parent folders)
//
// Terrain (the live world; samples are 1 world unit apart, heights are world units = word / 128):
//   ed_terrain_info()                    -> samplesX, samplesZ
//   ed_terrain_get(x, z)                 -> world height at one sample
//   ed_terrain_raise(cx, cz, r, amount)  -> samples changed  (cosine hill, `amount` world units at the centre)
//   ed_terrain_set_rect(x0,z0,x1,z1, h)  -> samples changed  (flatten an inclusive sample rectangle to h)
//   ed_terrain_read_rect(x0,z0,x1,z1)    -> array of height words, row major (x fastest), rect clamped
//                                           returns the clamped x0,z0,x1,z1 after the array
//   ed_terrain_write_rect(x0,z0,x1,z1, words) -> samples written   (words as returned by read_rect)
//
// Document and command stack (M1; moho/editor/MapDocument.h). The document owns the heights; edits are commands:
//   ed_doc_attach()                      -> samplesX, samplesZ   (binds the document to the open session; a new
//                                           session always calls it, see control.lua)
//   ed_doc_height_write(x0,z0,x1,z1, words) -> samples that changed (undoable; words as read_rect returns them;
//                                           unchanged samples are not recorded, 0 = no command)
//   ed_cmd_begin(label) / ed_cmd_end()   edits in between merge into one undo step
//   ed_cmd_undo() / ed_cmd_redo()        -> ok, x0, z0, x1, z1, label, verified   (rectangle that changed)
//   ed_cmd_depth()                       -> undo steps, redo steps, dirty, revision
//   ed_cmd_labels()                      -> undo label, redo label
//   ed_cmd_clear()                       drops the history (the document keeps its state and dirty flag)
//   ed_doc_mark_clean()                  the document equals what was saved
//   ed_doc_checksum(x0,z0,x1,z1)         -> hex string, FNV-1a of the document's words over the rectangle
//   (sim state) ed_sim_apply_pending()   -> n   applies queued height edits to the sim's field now (also done at
//                                           the start of every beat)
//   (sim state) ed_sim_checksum(x0,z0,x1,z1) -> hex string of the SIM's own field, comparable with ed_doc_checksum
//
// Map file:
//   ed_map_save(path)                    -> bool   writes the live map as a .scmap (engine writer: terrain,
//                                                  texturing, lighting, water, decals, props) to a disk path
//
// Simulation caches (sim Lua state only; call from a sim callback so it runs on the sim thread):
//   ed_sim_terrain_changed(x0, z0, x1, z1)  tells the sim that the terrain heights in this cell rectangle changed:
//                                           the pathfinder's cluster maps for that area (and a margin) are marked
//                                           dirty and rebuilt on demand. Occupancy / buildability caps are computed
//                                           from the sim's height field at query time and need nothing.
//
// Virtual file system:
//   ed_vfs_to_disk(vfsPath)              -> disk path of a mounted file, or nil
//   ed_vfs_mount(diskDir, mountPoint)    -> bool   mounts a directory at runtime (a freshly saved map folder)
//
// Props of the map file (the list the .scmap save writes; the simulation's own prop entities are created by
// the sim callbacks in editor/runtime/schook/lua/SimCallbacks.lua):
//   ed_prop_count()                      -> n
//   ed_prop_get(i)                       -> blueprint, x, y, z, qw, qx, qy, qz, sx, sy, sz   (i = 1..n)
//   ed_prop_add(blueprint, x, y, z, heading [, sx [, sy, sz]]) -> new index
//                                           (heading in radians about the up axis; one scale value = uniform;
//                                            the scale is kept in the map file's per-prop scale lanes)
//   ed_prop_scale(i, sx [, sy, sz])      -> bool   (map data only; the sim entity is rescaled by the caller)
//   ed_prop_move(i, x, y, z)             -> bool
//   ed_prop_remove(i)                    -> bool
//
// Decals of the map file (terrain decal manager):
//   ed_decal_count()                     -> n
//   ed_decal_get(i)                      -> name1, name2, type, x,y,z, sx,sy,sz, rx,ry,rz, cutoff
//   ed_decal_add(name, type, x,y,z, rx,ry,rz, sx,sy,sz, cutoff) -> new index   (type 1 albedo, 2 normals ...)
//   ed_decal_remove(i)                   -> bool
//
// Both height fields are written (the sim field and the view field), the tessellation error is refreshed on
// both and the terrain resource is told to rebuild normals, so the change shows on the next frame.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <algorithm>
#include <cmath>
#include <vector>

#include "lua/LuaRuntimeTypes.h"
#include "moho/editor/EditorDocument.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/path/PathTables.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "lua/LuaPrimitives.h"
#include "gpg/core/containers/String.h"
#include "moho/misc/CVirtualFileSystem.h"
#include "moho/misc/FileWaitHandleSet.h"
#include "moho/render/CWldTerrainDecal.h"
#include "moho/sim/CVFSImpl.h"
#include "moho/sim/CWldMap.h"
#include "moho/terrain/splat/CWldSplat.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/ISTIDriver.h"
#include "moho/sim/STIMap.h"
#include "moho/sim/Sim.h"
#include "moho/sim/SimDriver.h"

namespace
{
  namespace fs = std::filesystem;

  [[nodiscard]] moho::CScrLuaInitFormSet& EditorUserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }

    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& EditorSimLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("Sim"); set != nullptr) {
      return *set;
    }

    static moho::CScrLuaInitFormSet fallbackSet("Sim");
    return fallbackSet;
  }

  [[nodiscard]] const char* PathArg(lua_State* const state, const char* const functionName)
  {
    const char* const path = lua_tostring(state, 1);
    if (path == nullptr) {
      luaL_error(state, "%s: argument 1 must be a string path", functionName);
    }
    return path;
  }

  int __cdecl cfunc_EdFileExists(lua_State* const state)
  {
    std::error_code ec;
    lua_pushboolean(state, fs::is_regular_file(fs::path(PathArg(state, "ed_file_exists")), ec) ? 1 : 0);
    return 1;
  }

  int __cdecl cfunc_EdFileStamp(lua_State* const state)
  {
    const fs::path path(PathArg(state, "ed_file_stamp"));
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
      lua_pushnil(state);
      return 1;
    }
    const auto time = fs::last_write_time(path, ec);
    if (ec) {
      lua_pushnil(state);
      return 1;
    }
    char text[64];
    std::snprintf(
      text, sizeof(text), "%llu:%lld", static_cast<unsigned long long>(size),
      static_cast<long long>(time.time_since_epoch().count())
    );
    lua_pushstring(state, text);
    return 1;
  }

  int __cdecl cfunc_EdReadFile(lua_State* const state)
  {
    std::ifstream in(fs::path(PathArg(state, "ed_read_file")), std::ios::binary);
    if (!in) {
      lua_pushnil(state);
      return 1;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    lua_pushlstring(state, data.data(), data.size());
    return 1;
  }

  int __cdecl cfunc_EdWriteFile(lua_State* const state)
  {
    const fs::path path(PathArg(state, "ed_write_file"));
    const char* const data = lua_tostring(state, 2);
    if (data == nullptr) {
      luaL_error(state, "ed_write_file: argument 2 must be a string");
    }
    const std::size_t size = lua_strlen(state, 2);

    std::error_code ec;
    if (path.has_parent_path()) {
      fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      lua_pushboolean(state, 0);
      return 1;
    }
    out.write(data, static_cast<std::streamsize>(size));
    lua_pushboolean(state, out.good() ? 1 : 0);
    return 1;
  }


  // ---- terrain -------------------------------------------------------------------------------------------

  struct TerrainTargets
  {
    moho::IWldTerrainRes* res = nullptr;
    moho::CHeightField* view = nullptr;   // what the renderer reads
    moho::CHeightField* sim = nullptr;    // what the simulation reads (null outside a live sim)
  };

  [[nodiscard]] TerrainTargets ResolveTerrain()
  {
    TerrainTargets out;
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr || session->mWldMap->mTerrainRes == nullptr) {
      return out;
    }
    out.res = session->mWldMap->mTerrainRes;
    out.view = out.res->GetHeightField();

    if (moho::ISTIDriver* const driver = moho::WLD_GetDriver(); driver != nullptr) {
      if (auto* const simDriver = dynamic_cast<moho::CSimDriver*>(driver); simDriver != nullptr) {
        if (moho::Sim* const sim = simDriver->GetSimForEditor(); sim != nullptr && sim->mMapData != nullptr) {
          out.sim = sim->mMapData->mHeightField.get();
          if (out.sim == out.view) {
            out.sim = nullptr;
          }
        }
      }
    }
    return out;
  }

  [[nodiscard]] std::uint16_t ClampWord(const float value)
  {
    const float v = std::floor(value + 0.5f);
    return static_cast<std::uint16_t>(v < 0.0f ? 0.0f : (v > 65535.0f ? 65535.0f : v));
  }

  // A write that is not an undoable command (tests, import): it goes through the document, which stays the truth and
  // projects into the view field at once and into the sim's field between beats; the history is cleared (see
  // MapDocument::WriteExternal). Undoable edits use ed_doc_height_write.
  void ApplyTerrainRect(const TerrainTargets& t, int x0, int z0, int x1, int z1, const std::vector<std::uint16_t>& words)
  {
    (void)t;
    if (!moho::editor::AttachDocument(false)) {
      return;
    }
    moho::editor::CellRect rect;
    rect.x0 = x0;
    rect.z0 = z0;
    rect.x1 = x1;
    rect.z1 = z1;
    moho::editor::Document().WriteExternal(rect, words.data());
  }

  int __cdecl cfunc_EdTerrainInfo(lua_State* const state)
  {
    const TerrainTargets t = ResolveTerrain();
    if (t.view == nullptr) {
      lua_pushnil(state);
      return 1;
    }
    lua_pushnumber(state, static_cast<lua_Number>(t.view->Width()));
    lua_pushnumber(state, static_cast<lua_Number>(t.view->Height()));
    return 2;
  }

  int __cdecl cfunc_EdTerrainGet(lua_State* const state)
  {
    const TerrainTargets t = ResolveTerrain();
    const int x = static_cast<int>(luaL_checknumber(state, 1));
    const int z = static_cast<int>(luaL_checknumber(state, 2));
    if (t.view == nullptr || x < 0 || z < 0 || x >= t.view->Width() || z >= t.view->Height()) {
      lua_pushnil(state);
      return 1;
    }
    lua_pushnumber(state, static_cast<lua_Number>(t.view->GetHeightAt(x, z)) / 128.0f);
    return 1;
  }

  int __cdecl cfunc_EdTerrainRaise(lua_State* const state)
  {
    const TerrainTargets t = ResolveTerrain();
    const float cx = static_cast<float>(luaL_checknumber(state, 1));
    const float cz = static_cast<float>(luaL_checknumber(state, 2));
    const float radius = static_cast<float>(luaL_checknumber(state, 3));
    const float amount = static_cast<float>(luaL_checknumber(state, 4));
    if (t.view == nullptr || radius <= 0.0f) {
      lua_pushnumber(state, 0);
      return 1;
    }
    const int w = t.view->Width();
    const int h = t.view->Height();
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int z0 = std::max(0, static_cast<int>(std::floor(cz - radius)));
    const int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + radius)));
    const int z1 = std::min(h - 1, static_cast<int>(std::ceil(cz + radius)));
    if (x0 > x1 || z0 > z1) {
      lua_pushnumber(state, 0);
      return 1;
    }
    std::vector<std::uint16_t> words(static_cast<std::size_t>(x1 - x0 + 1) * static_cast<std::size_t>(z1 - z0 + 1));
    int changed = 0;
    std::size_t i = 0;
    for (int z = z0; z <= z1; ++z) {
      for (int x = x0; x <= x1; ++x, ++i) {
        const float dx = static_cast<float>(x) - cx;
        const float dz = static_cast<float>(z) - cz;
        const float d = std::sqrt(dx * dx + dz * dz);
        float word = static_cast<float>(t.view->GetHeightAt(x, z));
        if (d < radius) {
          const float weight = 0.5f * (1.0f + std::cos(3.14159265f * d / radius));
          word += amount * weight * 128.0f;
          ++changed;
        }
        words[i] = ClampWord(word);
      }
    }
    ApplyTerrainRect(t, x0, z0, x1, z1, words);
    lua_pushnumber(state, static_cast<lua_Number>(changed));
    return 1;
  }

  int __cdecl cfunc_EdTerrainSetRect(lua_State* const state)
  {
    const TerrainTargets t = ResolveTerrain();
    int x0 = static_cast<int>(luaL_checknumber(state, 1));
    int z0 = static_cast<int>(luaL_checknumber(state, 2));
    int x1 = static_cast<int>(luaL_checknumber(state, 3));
    int z1 = static_cast<int>(luaL_checknumber(state, 4));
    const float height = static_cast<float>(luaL_checknumber(state, 5));
    if (t.view == nullptr) {
      lua_pushnumber(state, 0);
      return 1;
    }
    x0 = std::max(0, x0);
    z0 = std::max(0, z0);
    x1 = std::min(t.view->Width() - 1, x1);
    z1 = std::min(t.view->Height() - 1, z1);
    if (x0 > x1 || z0 > z1) {
      lua_pushnumber(state, 0);
      return 1;
    }
    const std::uint16_t word = ClampWord(height * 128.0f);
    const std::size_t count = static_cast<std::size_t>(x1 - x0 + 1) * static_cast<std::size_t>(z1 - z0 + 1);
    const std::vector<std::uint16_t> words(count, word);
    ApplyTerrainRect(t, x0, z0, x1, z1, words);
    lua_pushnumber(state, static_cast<lua_Number>(count));
    return 1;
  }


  void ClampRect(const moho::CHeightField* const f, int& x0, int& z0, int& x1, int& z1)
  {
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);
    x0 = std::max(0, x0);
    z0 = std::max(0, z0);
    x1 = std::min(f->Width() - 1, x1);
    z1 = std::min(f->Height() - 1, z1);
  }

  int __cdecl cfunc_EdTerrainReadRect(lua_State* const state)
  {
    const TerrainTargets t = ResolveTerrain();
    int x0 = static_cast<int>(luaL_checknumber(state, 1));
    int z0 = static_cast<int>(luaL_checknumber(state, 2));
    int x1 = static_cast<int>(luaL_checknumber(state, 3));
    int z1 = static_cast<int>(luaL_checknumber(state, 4));
    if (t.view == nullptr) {
      lua_pushnil(state);
      return 1;
    }
    ClampRect(t.view, x0, z0, x1, z1);
    lua_newtable(state);
    if (x0 > x1 || z0 > z1) {
      return 1;
    }
    int index = 1;
    for (int z = z0; z <= z1; ++z) {
      for (int x = x0; x <= x1; ++x, ++index) {
        lua_pushnumber(state, static_cast<lua_Number>(index));
        lua_pushnumber(state, static_cast<lua_Number>(t.view->GetHeightAt(x, z)));
        lua_settable(state, -3);
      }
    }
    lua_pushnumber(state, static_cast<lua_Number>(x0));
    lua_pushnumber(state, static_cast<lua_Number>(z0));
    lua_pushnumber(state, static_cast<lua_Number>(x1));
    lua_pushnumber(state, static_cast<lua_Number>(z1));
    return 5;
  }

  int __cdecl cfunc_EdTerrainWriteRect(lua_State* const state)
  {
    const TerrainTargets t = ResolveTerrain();
    int x0 = static_cast<int>(luaL_checknumber(state, 1));
    int z0 = static_cast<int>(luaL_checknumber(state, 2));
    int x1 = static_cast<int>(luaL_checknumber(state, 3));
    int z1 = static_cast<int>(luaL_checknumber(state, 4));
    luaL_checktype(state, 5, LUA_TTABLE);
    if (t.view == nullptr) {
      lua_pushnumber(state, 0);
      return 1;
    }
    // the table was read for exactly this rectangle: it must already be inside the field
    if (x0 > x1 || z0 > z1 || x0 < 0 || z0 < 0 || x1 >= t.view->Width() || z1 >= t.view->Height()) {
      lua_pushnumber(state, 0);
      return 1;
    }
    const std::size_t count = static_cast<std::size_t>(x1 - x0 + 1) * static_cast<std::size_t>(z1 - z0 + 1);
    std::vector<std::uint16_t> words(count);
    for (std::size_t i = 0; i < count; ++i) {
      lua_pushnumber(state, static_cast<lua_Number>(i + 1));
      lua_gettable(state, 5);
      words[i] = ClampWord(static_cast<float>(lua_tonumber(state, -1)));
      lua_pop(state, 1);
    }
    ApplyTerrainRect(t, x0, z0, x1, z1, words);
    lua_pushnumber(state, static_cast<lua_Number>(count));
    return 1;
  }


  // ---- document and command stack -------------------------------------------------------------------------

  [[nodiscard]] moho::editor::CellRect RectArgs(lua_State* const state, const int first)
  {
    moho::editor::CellRect r;
    r.x0 = static_cast<int>(luaL_checknumber(state, first));
    r.z0 = static_cast<int>(luaL_checknumber(state, first + 1));
    r.x1 = static_cast<int>(luaL_checknumber(state, first + 2));
    r.z1 = static_cast<int>(luaL_checknumber(state, first + 3));
    return r;
  }

  int __cdecl cfunc_EdDocAttach(lua_State* const state)
  {
    if (!moho::editor::AttachDocument(true)) {
      lua_pushnil(state);
      return 1;
    }
    lua_pushnumber(state, static_cast<lua_Number>(moho::editor::Document().Width()));
    lua_pushnumber(state, static_cast<lua_Number>(moho::editor::Document().Height()));
    return 2;
  }

  int __cdecl cfunc_EdDocHeightWrite(lua_State* const state)
  {
    const moho::editor::CellRect rect = RectArgs(state, 1);
    luaL_checktype(state, 5, LUA_TTABLE);
    if (!moho::editor::AttachDocument(false)) {
      lua_pushnumber(state, 0);
      return 1;
    }
    moho::editor::MapDocument& doc = moho::editor::Document();
    if (rect.Empty() || rect.x0 < 0 || rect.z0 < 0 || rect.x1 >= doc.Width() || rect.z1 >= doc.Height()) {
      lua_pushnumber(state, 0);
      return 1;
    }
    const std::size_t count = rect.Count();
    std::vector<std::uint16_t> words(count);
    for (std::size_t i = 0; i < count; ++i) {
      lua_pushnumber(state, static_cast<lua_Number>(i + 1));
      lua_gettable(state, 5);
      words[i] = ClampWord(static_cast<float>(lua_tonumber(state, -1)));
      lua_pop(state, 1);
    }
    lua_pushnumber(state, static_cast<lua_Number>(doc.EditHeights(rect, words.data())));
    return 1;
  }

  int __cdecl cfunc_EdCmdBegin(lua_State* const state)
  {
    if (moho::editor::AttachDocument(false)) {
      const char* const label = lua_tostring(state, 1);
      moho::editor::Document().BeginStroke(label != nullptr ? label : "Edit");
    }
    return 0;
  }

  int __cdecl cfunc_EdCmdEnd(lua_State* const state)
  {
    (void)state;
    if (moho::editor::Document().Valid()) {
      moho::editor::Document().EndStroke();
    }
    return 0;
  }

  int HistoryStep(lua_State* const state, const bool undo)
  {
    if (!moho::editor::AttachDocument(false)) {
      lua_pushboolean(state, 0);
      return 1;
    }
    const moho::editor::HistoryResult r = undo ? moho::editor::Document().Undo() : moho::editor::Document().Redo();
    lua_pushboolean(state, r.ok ? 1 : 0);
    lua_pushnumber(state, r.bounds.x0);
    lua_pushnumber(state, r.bounds.z0);
    lua_pushnumber(state, r.bounds.x1);
    lua_pushnumber(state, r.bounds.z1);
    lua_pushstring(state, r.label.c_str());
    lua_pushboolean(state, r.verified ? 1 : 0);
    return 7;
  }

  int __cdecl cfunc_EdCmdUndo(lua_State* const state)
  {
    return HistoryStep(state, true);
  }

  int __cdecl cfunc_EdCmdRedo(lua_State* const state)
  {
    return HistoryStep(state, false);
  }

  int __cdecl cfunc_EdCmdDepth(lua_State* const state)
  {
    const moho::editor::MapDocument& doc = moho::editor::Document();
    lua_pushnumber(state, static_cast<lua_Number>(doc.UndoDepth()));
    lua_pushnumber(state, static_cast<lua_Number>(doc.RedoDepth()));
    lua_pushboolean(state, doc.Dirty() ? 1 : 0);
    lua_pushnumber(state, static_cast<lua_Number>(doc.Revision()));
    return 4;
  }

  int __cdecl cfunc_EdCmdLabels(lua_State* const state)
  {
    const moho::editor::MapDocument& doc = moho::editor::Document();
    lua_pushstring(state, doc.UndoLabel().c_str());
    lua_pushstring(state, doc.RedoLabel().c_str());
    return 2;
  }

  int __cdecl cfunc_EdCmdClear(lua_State* const state)
  {
    (void)state;
    moho::editor::Document().ClearHistory();
    return 0;
  }

  int __cdecl cfunc_EdDocMarkClean(lua_State* const state)
  {
    (void)state;
    moho::editor::Document().MarkClean();
    return 0;
  }

  int __cdecl cfunc_EdDocChecksum(lua_State* const state)
  {
    const moho::editor::CellRect rect = RectArgs(state, 1);
    if (!moho::editor::AttachDocument(false)) {
      lua_pushnil(state);
      return 1;
    }
    const moho::editor::MapDocument& doc = moho::editor::Document();
    lua_pushstring(state, moho::editor::HexChecksum(doc.Checksum(doc.Clamp(rect))).c_str());
    return 1;
  }

  // sim state: lua_State -> Sim through the global user data, like ed_sim_terrain_changed
  int __cdecl cfunc_EdSimApplyPending(lua_State* const state)
  {
    moho::Sim* const sim = (state != nullptr && state->l_G != nullptr) ? state->l_G->globalUserData : nullptr;
    lua_pushnumber(state, static_cast<lua_Number>(moho::editor::ApplyPendingHeightEdits(sim)));
    return 1;
  }

  int __cdecl cfunc_EdSimChecksum(lua_State* const state)
  {
    moho::Sim* const sim = (state != nullptr && state->l_G != nullptr) ? state->l_G->globalUserData : nullptr;
    const moho::editor::CellRect rect = RectArgs(state, 1);
    lua_pushstring(state, moho::editor::HexChecksum(moho::editor::SimFieldChecksum(sim, rect)).c_str());
    return 1;
  }

  int __cdecl cfunc_EdMapSave(lua_State* const state)
  {
    const char* const path = PathArg(state, "ed_map_save");
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    bool ok = false;
    if (session != nullptr && session->mWldMap != nullptr) {
      std::error_code ec;
      const fs::path target(path);
      if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
      }
      ok = session->mWldMap->MapSaveToDiskPath(path);
    }
    lua_pushboolean(state, ok ? 1 : 0);
    return 1;
  }


  // ---- virtual file system -------------------------------------------------------------------------------

  [[nodiscard]] moho::CVirtualFileSystem* ActiveVfs()
  {
    moho::FWaitHandleSet* const set = moho::FILE_GetWaitHandleSet();
    return set != nullptr ? set->mHandle : nullptr;
  }

  int __cdecl cfunc_EdVfsToDisk(lua_State* const state)
  {
    const char* const path = PathArg(state, "ed_vfs_to_disk");
    moho::CVirtualFileSystem* const vfs = ActiveVfs();
    if (vfs == nullptr) {
      lua_pushnil(state);
      return 1;
    }
    msvc8::string out;
    (void)vfs->FindFile(&out, path, nullptr);
    if (out.empty()) {
      lua_pushnil(state);
    } else {
      lua_pushstring(state, out.c_str());
    }
    return 1;
  }

  int __cdecl cfunc_EdVfsMount(lua_State* const state)
  {
    const char* const dir = PathArg(state, "ed_vfs_mount");
    const char* const mountpoint = lua_tostring(state, 2);
    if (mountpoint == nullptr) {
      luaL_error(state, "ed_vfs_mount: argument 2 must be a mount point string");
    }
    auto* const impl = dynamic_cast<moho::CVFSImpl*>(ActiveVfs());
    std::error_code ec;
    if (impl == nullptr || !fs::is_directory(fs::path(dir), ec)) {
      lua_pushboolean(state, 0);
      return 1;
    }
    msvc8::string canonical;
    gpg::STR_CanonizeFilename(&canonical, dir);
    moho::SVFSMountPoint mount;
    mount.mDir = gpg::STR_ToLower(canonical.c_str());
    mount.mMountpoint = gpg::STR_ToLower(mountpoint);
    if (mount.mMountpoint.empty() || mount.mMountpoint[mount.mMountpoint.size() - 1] != '/') {
      (void)mount.mMountpoint.push_back('/');
    }
    impl->mMountpoints.push_back(std::move(mount));
    lua_pushboolean(state, 1);
    return 1;
  }

  // ---- props ---------------------------------------------------------------------------------------------

  [[nodiscard]] moho::CWldProps* LiveProps()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr) {
      return nullptr;
    }
    return session->mWldMap->mProps;
  }

  int __cdecl cfunc_EdPropCount(lua_State* const state)
  {
    moho::CWldProps* const props = LiveProps();
    lua_pushnumber(state, props != nullptr ? static_cast<lua_Number>(props->mEntries.size()) : 0);
    return 1;
  }

  int __cdecl cfunc_EdPropGet(lua_State* const state)
  {
    moho::CWldProps* const props = LiveProps();
    const int index = static_cast<int>(luaL_checknumber(state, 1)) - 1;
    if (props == nullptr || index < 0 || static_cast<std::size_t>(index) >= props->mEntries.size()) {
      lua_pushnil(state);
      return 1;
    }
    const moho::CWldPropEntry& e = props->mEntries[static_cast<std::size_t>(index)];
    lua_pushstring(state, e.mBlueprintPath.c_str());
    lua_pushnumber(state, e.mTransform.pos_.x);
    lua_pushnumber(state, e.mTransform.pos_.y);
    lua_pushnumber(state, e.mTransform.pos_.z);
    lua_pushnumber(state, e.mTransform.orient_.w);
    lua_pushnumber(state, e.mTransform.orient_.x);
    lua_pushnumber(state, e.mTransform.orient_.y);
    lua_pushnumber(state, e.mTransform.orient_.z);
    lua_pushnumber(state, e.mScale.x);
    lua_pushnumber(state, e.mScale.y);
    lua_pushnumber(state, e.mScale.z);
    return 11;
  }

  // Scale arguments starting at `first`: none = (1,1,1), one = uniform, three = per axis.
  [[nodiscard]] Wm3::Vec3f ScaleArgs(lua_State* const state, const int first)
  {
    if (lua_gettop(state) < first) {
      return Wm3::Vec3f(1.0f, 1.0f, 1.0f);
    }
    const float sx = static_cast<float>(luaL_checknumber(state, first));
    if (lua_gettop(state) < first + 2) {
      return Wm3::Vec3f(sx, sx, sx);
    }
    return Wm3::Vec3f(
      sx, static_cast<float>(luaL_checknumber(state, first + 1)), static_cast<float>(luaL_checknumber(state, first + 2)));
  }

  int __cdecl cfunc_EdPropAdd(lua_State* const state)
  {
    moho::CWldProps* const props = LiveProps();
    const char* const blueprint = lua_tostring(state, 1);
    if (props == nullptr || blueprint == nullptr) {
      lua_pushnil(state);
      return 1;
    }
    const float x = static_cast<float>(luaL_checknumber(state, 2));
    const float y = static_cast<float>(luaL_checknumber(state, 3));
    const float z = static_cast<float>(luaL_checknumber(state, 4));
    const float heading = static_cast<float>(luaL_optnumber(state, 5, 0.0));
    moho::CWldPropEntry entry;
    entry.mBlueprintPath = msvc8::string(blueprint);
    entry.mTransform.pos_ = Wm3::Vec3f(x, y, z);
    entry.mTransform.orient_ = Wm3::Quatf(std::cos(heading * 0.5f), 0.0f, std::sin(heading * 0.5f), 0.0f);
    entry.mScale = ScaleArgs(state, 6);
    props->mEntries.push_back(entry);
    lua_pushnumber(state, static_cast<lua_Number>(props->mEntries.size()));
    return 1;
  }

  int __cdecl cfunc_EdPropMove(lua_State* const state)
  {
    moho::CWldProps* const props = LiveProps();
    const int index = static_cast<int>(luaL_checknumber(state, 1)) - 1;
    if (props == nullptr || index < 0 || static_cast<std::size_t>(index) >= props->mEntries.size()) {
      lua_pushboolean(state, 0);
      return 1;
    }
    moho::CWldPropEntry& e = props->mEntries[static_cast<std::size_t>(index)];
    e.mTransform.pos_ = Wm3::Vec3f(
      static_cast<float>(luaL_checknumber(state, 2)),
      static_cast<float>(luaL_checknumber(state, 3)),
      static_cast<float>(luaL_checknumber(state, 4)));
    lua_pushboolean(state, 1);
    return 1;
  }

  int __cdecl cfunc_EdPropScale(lua_State* const state)
  {
    moho::CWldProps* const props = LiveProps();
    const int index = static_cast<int>(luaL_checknumber(state, 1)) - 1;
    if (props == nullptr || index < 0 || static_cast<std::size_t>(index) >= props->mEntries.size()) {
      lua_pushboolean(state, 0);
      return 1;
    }
    props->mEntries[static_cast<std::size_t>(index)].mScale = ScaleArgs(state, 2);
    lua_pushboolean(state, 1);
    return 1;
  }

  int __cdecl cfunc_EdPropRemove(lua_State* const state)
  {
    moho::CWldProps* const props = LiveProps();
    const int index = static_cast<int>(luaL_checknumber(state, 1)) - 1;
    if (props == nullptr || index < 0 || static_cast<std::size_t>(index) >= props->mEntries.size()) {
      lua_pushboolean(state, 0);
      return 1;
    }
    // msvc8::vector in this tree has no erase(): rebuild without the removed entry
    std::vector<moho::CWldPropEntry> keep;
    keep.reserve(props->mEntries.size());
    for (std::size_t i = 0; i < props->mEntries.size(); ++i) {
      if (i != static_cast<std::size_t>(index)) {
        keep.push_back(props->mEntries[i]);
      }
    }
    props->mEntries.clear();
    for (const moho::CWldPropEntry& e : keep) {
      props->mEntries.push_back(e);
    }
    lua_pushboolean(state, 1);
    return 1;
  }

  // ---- decals --------------------------------------------------------------------------------------------

  [[nodiscard]] moho::IDecalManager* LiveDecals()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr || session->mWldMap->mTerrainRes == nullptr) {
      return nullptr;
    }
    return session->mWldMap->mTerrainRes->GetDecalManager();
  }

  // map decals only: simulation-created decals (mArmy >= 0) are runtime effects and are skipped everywhere
  [[nodiscard]] moho::CWldTerrainDecal* NthMapDecal(moho::IDecalManager* const manager, const int ordinal)
  {
    int seen = 0;
    const int count = manager->GetDecalCount();
    for (int i = 0; i < count; ++i) {
      moho::CWldTerrainDecal* const decal = manager->GetDecal(i);
      if (decal == nullptr || decal->mArmy >= 0 || decal->mUnknownA0 != 0u) {
        continue;
      }
      if (seen == ordinal) {
        return decal;
      }
      ++seen;
    }
    return nullptr;
  }

  int __cdecl cfunc_EdDecalCount(lua_State* const state)
  {
    moho::IDecalManager* const manager = LiveDecals();
    int n = 0;
    if (manager != nullptr) {
      while (NthMapDecal(manager, n) != nullptr) {
        ++n;
      }
    }
    lua_pushnumber(state, static_cast<lua_Number>(n));
    return 1;
  }

  int __cdecl cfunc_EdDecalGet(lua_State* const state)
  {
    moho::IDecalManager* const manager = LiveDecals();
    const int ordinal = static_cast<int>(luaL_checknumber(state, 1)) - 1;
    const moho::CWldTerrainDecal* const d = (manager != nullptr && ordinal >= 0) ? NthMapDecal(manager, ordinal) : nullptr;
    if (d == nullptr) {
      lua_pushnil(state);
      return 1;
    }
    lua_pushstring(state, d->mNames[0].c_str());
    lua_pushstring(state, d->mNames[1].c_str());
    lua_pushnumber(state, static_cast<lua_Number>(static_cast<int>(d->mType)));
    lua_pushnumber(state, d->mPosition.x);
    lua_pushnumber(state, d->mPosition.y);
    lua_pushnumber(state, d->mPosition.z);
    lua_pushnumber(state, d->mScale.x);
    lua_pushnumber(state, d->mScale.y);
    lua_pushnumber(state, d->mScale.z);
    lua_pushnumber(state, d->mOrientation.x);
    lua_pushnumber(state, d->mOrientation.y);
    lua_pushnumber(state, d->mOrientation.z);
    lua_pushnumber(state, d->mCutoffLOD);
    return 13;
  }

  int __cdecl cfunc_EdDecalAdd(lua_State* const state)
  {
    moho::IDecalManager* const manager = LiveDecals();
    const char* const name = lua_tostring(state, 1);
    if (manager == nullptr || name == nullptr) {
      lua_pushnil(state);
      return 1;
    }
    const int type = static_cast<int>(luaL_checknumber(state, 2));
    const Wm3::Vec3f position(static_cast<float>(luaL_checknumber(state, 3)),
                              static_cast<float>(luaL_checknumber(state, 4)),
                              static_cast<float>(luaL_checknumber(state, 5)));
    const Wm3::Vec3f rotation(static_cast<float>(luaL_optnumber(state, 6, 0.0)),
                              static_cast<float>(luaL_optnumber(state, 7, 0.0)),
                              static_cast<float>(luaL_optnumber(state, 8, 0.0)));
    const Wm3::Vec3f scale(static_cast<float>(luaL_optnumber(state, 9, 10.0)),
                           static_cast<float>(luaL_optnumber(state, 10, 10.0)),
                           static_cast<float>(luaL_optnumber(state, 11, 10.0)));
    const float cutoff = static_cast<float>(luaL_optnumber(state, 12, 1000.0));

    // a free decal index: one past the highest in use (map decals address each other by index in groups)
    int nextIndex = 0;
    const int total = manager->GetDecalCount();
    for (int i = 0; i < total; ++i) {
      if (const moho::CWldTerrainDecal* const d = manager->GetDecal(i); d != nullptr && d->mIndex >= nextIndex) {
        nextIndex = d->mIndex + 1;
      }
    }

    moho::CWldTerrainDecal* const decal = manager->NewDecal(nextIndex);
    if (decal == nullptr) {
      lua_pushnil(state);
      return 1;
    }
    decal->mType = static_cast<moho::EWldTerrainDecalType>(type);
    decal->SetName(msvc8::string(name), 0);
    decal->SetParameters(rotation, position, scale);
    decal->SetCutoffLOD(cutoff);
    decal->mArmy = -1;
    decal->Update();
    manager->MarkPendingChanges();

    int n = 0;
    while (NthMapDecal(manager, n) != nullptr) {
      ++n;
    }
    lua_pushnumber(state, static_cast<lua_Number>(n));
    return 1;
  }

  int __cdecl cfunc_EdDecalRemove(lua_State* const state)
  {
    moho::IDecalManager* const manager = LiveDecals();
    const int ordinal = static_cast<int>(luaL_checknumber(state, 1)) - 1;
    moho::CWldTerrainDecal* const decal = (manager != nullptr && ordinal >= 0) ? NthMapDecal(manager, ordinal) : nullptr;
    if (decal == nullptr) {
      lua_pushboolean(state, 0);
      return 1;
    }
    manager->DestroyDecal(decal);
    manager->MarkPendingChanges();
    lua_pushboolean(state, 1);
    return 1;
  }


  // ---- simulation caches ---------------------------------------------------------------------------------

  int __cdecl cfunc_EdSimTerrainChanged(lua_State* const state)
  {
    moho::Sim* const sim = (state != nullptr && state->l_G != nullptr) ? state->l_G->globalUserData : nullptr;
    int x0 = static_cast<int>(luaL_checknumber(state, 1));
    int z0 = static_cast<int>(luaL_checknumber(state, 2));
    int x1 = static_cast<int>(luaL_checknumber(state, 3));
    int z1 = static_cast<int>(luaL_checknumber(state, 4));
    if (sim == nullptr || sim->mPathTables == nullptr || sim->mMapData == nullptr || sim->mMapData->mHeightField.get() == nullptr) {
      lua_pushboolean(state, 0);
      return 1;
    }
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);
    // A path cell reads the heights of the cell and its neighbours, and a unit footprint spans several cells:
    // dirty a margin around the edit so no cluster that could see the change keeps its old answer.
    constexpr int kMargin = 10;
    const moho::CHeightField* const field = sim->mMapData->mHeightField.get();
    gpg::Rect2i rect{};
    rect.x0 = std::max(0, x0 - kMargin);
    rect.z0 = std::max(0, z0 - kMargin);
    rect.x1 = std::min(field->Width() - 1, x1 + kMargin);
    rect.z1 = std::min(field->Height() - 1, z1 + kMargin);
    sim->mPathTables->DirtyClusters(rect);
    lua_pushboolean(state, 1);
    return 1;
  }

  moho::CScrLuaInitForm* BindGlobal(const char* const name, int(__cdecl* const function)(lua_State*), const char* const help)
  {
    // one binder per call site, kept alive for the process (forms are linked into the set by construction)
    return new moho::CScrLuaBinder(EditorUserLuaInitSet(), name, function, nullptr, "<global>", help);
  }

  // The same function also in the simulation's Lua state (sim callbacks use the file functions to report).
  moho::CScrLuaInitForm* BindSimGlobal(const char* const name, int(__cdecl* const function)(lua_State*), const char* const help)
  {
    return new moho::CScrLuaBinder(EditorSimLuaInitSet(), name, function, nullptr, "<global>", help);
  }

  // Runs before main (like every other startup binder in this tree); see UnitLuaFuncDefBootstrap in Unit.cpp.
  struct EditorLuaBootstrap
  {
    EditorLuaBootstrap()
    {
      (void)BindGlobal("ed_file_exists", &cfunc_EdFileExists, "ed_file_exists(path) -> bool");
      (void)BindSimGlobal("ed_file_exists", &cfunc_EdFileExists, "ed_file_exists(path) -> bool");
      (void)BindSimGlobal("ed_read_file", &cfunc_EdReadFile, "ed_read_file(path) -> string or nil");
      (void)BindSimGlobal("ed_write_file", &cfunc_EdWriteFile, "ed_write_file(path, data) -> bool");
      (void)BindSimGlobal("ed_sim_terrain_changed", &cfunc_EdSimTerrainChanged, "ed_sim_terrain_changed(x0, z0, x1, z1) -> bool");
      (void)BindGlobal("ed_file_stamp", &cfunc_EdFileStamp, "ed_file_stamp(path) -> \"size:mtime\" or nil");
      (void)BindGlobal("ed_read_file", &cfunc_EdReadFile, "ed_read_file(path) -> string or nil");
      (void)BindGlobal("ed_write_file", &cfunc_EdWriteFile, "ed_write_file(path, data) -> bool");
      (void)BindGlobal("ed_terrain_info", &cfunc_EdTerrainInfo, "ed_terrain_info() -> samplesX, samplesZ");
      (void)BindGlobal("ed_terrain_get", &cfunc_EdTerrainGet, "ed_terrain_get(x, z) -> world height");
      (void)BindGlobal("ed_terrain_raise", &cfunc_EdTerrainRaise, "ed_terrain_raise(cx, cz, radius, amount) -> samples");
      (void)BindGlobal("ed_terrain_set_rect", &cfunc_EdTerrainSetRect, "ed_terrain_set_rect(x0, z0, x1, z1, height) -> samples");
      (void)BindGlobal("ed_terrain_read_rect", &cfunc_EdTerrainReadRect, "ed_terrain_read_rect(x0, z0, x1, z1) -> words, x0, z0, x1, z1");
      (void)BindGlobal("ed_terrain_write_rect", &cfunc_EdTerrainWriteRect, "ed_terrain_write_rect(x0, z0, x1, z1, words) -> samples");
      (void)BindGlobal("ed_map_save", &cfunc_EdMapSave, "ed_map_save(path) -> bool");
      (void)BindGlobal("ed_vfs_to_disk", &cfunc_EdVfsToDisk, "ed_vfs_to_disk(...)");
      (void)BindGlobal("ed_vfs_mount", &cfunc_EdVfsMount, "ed_vfs_mount(...)");
      (void)BindGlobal("ed_prop_count", &cfunc_EdPropCount, "ed_prop_count(...)");
      (void)BindGlobal("ed_prop_get", &cfunc_EdPropGet, "ed_prop_get(...)");
      (void)BindGlobal("ed_prop_add", &cfunc_EdPropAdd, "ed_prop_add(...)");
      (void)BindGlobal("ed_prop_move", &cfunc_EdPropMove, "ed_prop_move(...)");
      (void)BindGlobal("ed_prop_remove", &cfunc_EdPropRemove, "ed_prop_remove(...)");
      (void)BindGlobal("ed_prop_scale", &cfunc_EdPropScale, "ed_prop_scale(...)");
      (void)BindGlobal("ed_doc_attach", &cfunc_EdDocAttach, "ed_doc_attach() -> samplesX, samplesZ");
      (void)BindGlobal("ed_doc_height_write", &cfunc_EdDocHeightWrite, "ed_doc_height_write(x0, z0, x1, z1, words) -> changed");
      (void)BindGlobal("ed_cmd_begin", &cfunc_EdCmdBegin, "ed_cmd_begin(label)");
      (void)BindGlobal("ed_cmd_end", &cfunc_EdCmdEnd, "ed_cmd_end()");
      (void)BindGlobal("ed_cmd_undo", &cfunc_EdCmdUndo, "ed_cmd_undo() -> ok, x0, z0, x1, z1, label, verified");
      (void)BindGlobal("ed_cmd_redo", &cfunc_EdCmdRedo, "ed_cmd_redo() -> ok, x0, z0, x1, z1, label, verified");
      (void)BindGlobal("ed_cmd_depth", &cfunc_EdCmdDepth, "ed_cmd_depth() -> undo, redo, dirty, revision");
      (void)BindGlobal("ed_cmd_labels", &cfunc_EdCmdLabels, "ed_cmd_labels() -> undoLabel, redoLabel");
      (void)BindGlobal("ed_cmd_clear", &cfunc_EdCmdClear, "ed_cmd_clear()");
      (void)BindGlobal("ed_doc_mark_clean", &cfunc_EdDocMarkClean, "ed_doc_mark_clean()");
      (void)BindGlobal("ed_doc_checksum", &cfunc_EdDocChecksum, "ed_doc_checksum(x0, z0, x1, z1) -> hex");
      (void)BindSimGlobal("ed_sim_apply_pending", &cfunc_EdSimApplyPending, "ed_sim_apply_pending() -> n");
      (void)BindSimGlobal("ed_sim_checksum", &cfunc_EdSimChecksum, "ed_sim_checksum(x0, z0, x1, z1) -> hex");
      (void)BindGlobal("ed_decal_count", &cfunc_EdDecalCount, "ed_decal_count(...)");
      (void)BindGlobal("ed_decal_get", &cfunc_EdDecalGet, "ed_decal_get(...)");
      (void)BindGlobal("ed_decal_add", &cfunc_EdDecalAdd, "ed_decal_add(...)");
      (void)BindGlobal("ed_decal_remove", &cfunc_EdDecalRemove, "ed_decal_remove(...)");
    }
  };

  const EditorLuaBootstrap gEditorLuaBootstrap{};
} // namespace
