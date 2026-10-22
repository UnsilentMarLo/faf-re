// EDITOR: the native terrain brush dab (docs/projects/MAP_EDITOR.md, "Native brush dab"; math in EdBrush.h, caller editor/runtime/lua/editor/terrain.lua DabOne).
//
//   ed_terrain_brush(cx, cz, radius, mode, strength, dir, falloff, level, seed [, noiseScale, noiseOctaves, noiseRoughness]) -> changed, x0, z0, x1, z1, level
//       One dab of the round brush at sample position (cx, cz) inside the open stroke of the document (ed_cmd_begin). mode = 'raise' | 'lower' |
//       'plateau' | 'flatten' | 'smooth' | 'noise' (anything else: returns nil and does nothing, the caller keeps its Lua path). strength, falloff and the
//       weight profile are the ones of terrain.lua; dir = -1 inverts; level (world units, flatten / plateau) nil or negative = the ground under the
//       dab centre; seed = the stroke id (noise). It reads the document's heights, computes the new window and writes it through
//       MapDocument::EditHeights, the path ed_doc_height_write takes (undo stroke, view field, sim queue). Returns the number of changed samples,
//       the inclusive window it worked on (the rectangle to re-snap units in; clamped to the map) and, for plateau / flatten, the level it used
//       (nil for the other modes) so the caller can keep it for the rest of the stroke. nil without a terrain.
//       Noise mode: smooth value noise at the sample coordinates (so it is continuous across the dabs of a stroke, seed = the stroke id); noiseScale = the
//       lattice cell size in samples (default 8), noiseOctaves 1..4 (default 2), noiseRoughness = amplitude share of each further octave (default 0.5).
#include <cstdint>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/editor/EdBrush.h"
#include "moho/editor/EditorDocument.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace
{
  namespace br = moho::editor::brush;

  double NumberOr(lua_State* const L, const int index, const double fallback)
  {
    if (lua_type(L, index) == LUA_TNUMBER) {
      return lua_tonumber(L, index);
    }
    return fallback;
  }

  int __cdecl cfunc_EdTerrainBrush(lua_State* const L)
  {
    br::Dab dab;
    dab.cx = static_cast<float>(luaL_checknumber(L, 1));
    dab.cz = static_cast<float>(luaL_checknumber(L, 2));
    dab.radius = static_cast<float>(luaL_checknumber(L, 3));
    const char* const modeName = lua_tostring(L, 4);
    if (!br::ModeFromName(modeName, dab.mode)) {
      lua_pushnil(L);
      return 1;
    }
    dab.strength = static_cast<float>(luaL_checknumber(L, 5));
    dab.dir = NumberOr(L, 6, 1.0) < 0.0 ? -1 : 1;
    dab.falloff = static_cast<float>(NumberOr(L, 7, 1.0));
    dab.level = static_cast<float>(NumberOr(L, 8, -1.0));
    dab.seed = static_cast<int>(NumberOr(L, 9, 0.0));
    // optional trailing arguments (the noise mode); an exe without them ignores the extras
    dab.noiseScale = static_cast<float>(NumberOr(L, 10, 8.0));
    dab.noiseOctaves = static_cast<int>(NumberOr(L, 11, 2.0));
    dab.noiseRoughness = static_cast<float>(NumberOr(L, 12, 0.5));

    if (!moho::editor::AttachDocument(false)) {
      lua_pushnil(L);
      return 1;
    }
    moho::editor::MapDocument& doc = moho::editor::Document();
    const br::Window win = br::WindowOf(dab, doc.Width(), doc.Height());
    if (win.Empty() || !(dab.radius > 0.0f)) {
      lua_pushnumber(L, 0);
      return 1;
    }
    moho::editor::CellRect rect;
    rect.x0 = win.x0;
    rect.z0 = win.z0;
    rect.x1 = win.x1;
    rect.z1 = win.z1;
    std::vector<std::uint16_t> before;
    doc.ReadRect(rect, before);
    if (before.size() != win.Count()) {
      lua_pushnumber(L, 0);
      return 1;
    }
    std::vector<std::uint16_t> after(before.size());
    const br::Result result = br::ApplyWindow(dab, win, before.data(), after.data());
    std::size_t changed = 0;
    if (result.changed > 0) {
      changed = doc.EditHeights(rect, after.data());
    }
    lua_pushnumber(L, static_cast<lua_Number>(changed));
    lua_pushnumber(L, static_cast<lua_Number>(win.x0));
    lua_pushnumber(L, static_cast<lua_Number>(win.z0));
    lua_pushnumber(L, static_cast<lua_Number>(win.x1));
    lua_pushnumber(L, static_cast<lua_Number>(win.z1));
    if (result.usedLevel) {
      lua_pushnumber(L, static_cast<lua_Number>(result.level));
    } else {
      lua_pushnil(L);
    }
    return 6;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& UserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  struct EdBrushLuaBootstrap
  {
    EdBrushLuaBootstrap()
    {
      (void)new moho::CScrLuaBinder(
        UserLuaInitSet(), "ed_terrain_brush", &cfunc_EdTerrainBrush, nullptr, "<global>",
        "ed_terrain_brush(cx, cz, radius, mode, strength, dir, falloff, level, seed [, noiseScale, noiseOctaves, noiseRoughness]) -> changed, x0, z0, x1, z1, level"
      );
    }
  };

  const EdBrushLuaBootstrap gEdBrushLuaBootstrap{};
} // namespace
