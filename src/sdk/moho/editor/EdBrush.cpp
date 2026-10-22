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
//       Shaped brush: arguments 13 and 14, shape (a handle of ed_brush_shape_set; nil / 0 = the round falloff profile) and angle (degrees the mask is turned: brushes.lua Angle
//       plus the stroke's random turn). The mask then replaces the falloff profile (the brush is still cut at the radius), the maths of every mode is unchanged. An unknown handle returns nil
//       and does nothing, so the Lua loop takes over. An exe without these ignores the two extra arguments and would draw a round brush: the Lua side only passes a shape after
//       finding ed_brush_shape_set.
//   ed_brush_shape_set(handle, n, values) -> true | false
//       Stores the shape mask `handle` (an integer >= 1; set again to replace it): n x n weights 0..1 (2 <= n <= 512) in a Lua array, row after row (z outer, x fastest), over the unit
//       square -1..1 with the samples on the corners (u = -1 + 2 i / (n - 1)). Done once per shape and resolution (brushes.lua NativeShape), not per dab.
//   ed_brush_shape_info(handle) -> n | nil
//       The size of a stored shape (nil: none), for tests and as the probe.
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
    const int shapeHandle = static_cast<int>(NumberOr(L, 13, 0.0));
    if (shapeHandle > 0) {
      dab.shape = br::FindShape(shapeHandle);
      if (dab.shape == nullptr) {
        lua_pushnil(L);       // unknown handle: the Lua loop does the dab
        return 1;
      }
      dab.shapeAngle = static_cast<float>(NumberOr(L, 14, 0.0));
    }

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

  int __cdecl cfunc_EdBrushShapeSet(lua_State* const L)
  {
    const int handle = static_cast<int>(luaL_checknumber(L, 1));
    const int n = static_cast<int>(luaL_checknumber(L, 2));
    if (handle <= 0 || n < 2 || n > br::kMaxShapeSize || lua_type(L, 3) != LUA_TTABLE) {
      lua_pushboolean(L, 0);
      return 1;
    }
    const std::size_t count = static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
    std::vector<float> values;
    values.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
      lua_rawgeti(L, 3, static_cast<int>(i));
      float v = 0.0f;
      if (lua_type(L, -1) == LUA_TNUMBER) {
        v = static_cast<float>(lua_tonumber(L, -1));
      }
      lua_settop(L, -2);
      values.push_back(v);
    }
    lua_pushboolean(L, br::SetShape(handle, n, std::move(values)) ? 1 : 0);
    return 1;
  }

  int __cdecl cfunc_EdBrushShapeInfo(lua_State* const L)
  {
    const br::Shape* const shape = br::FindShape(static_cast<int>(luaL_checknumber(L, 1)));
    if (shape == nullptr) {
      lua_pushnil(L);
    } else {
      lua_pushnumber(L, static_cast<lua_Number>(shape->n));
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

  struct EdBrushLuaBootstrap
  {
    EdBrushLuaBootstrap()
    {
      (void)new moho::CScrLuaBinder(
        UserLuaInitSet(), "ed_terrain_brush", &cfunc_EdTerrainBrush, nullptr, "<global>",
        "ed_terrain_brush(cx, cz, radius, mode, strength, dir, falloff, level, seed [, noiseScale, noiseOctaves, noiseRoughness, shape, angle]) -> changed, x0, z0, x1, z1, level"
      );
      (void)new moho::CScrLuaBinder(
        UserLuaInitSet(), "ed_brush_shape_set", &cfunc_EdBrushShapeSet, nullptr, "<global>", "ed_brush_shape_set(handle, n, values) -> true | false"
      );
      (void)new moho::CScrLuaBinder(
        UserLuaInitSet(), "ed_brush_shape_info", &cfunc_EdBrushShapeInfo, nullptr, "<global>", "ed_brush_shape_info(handle) -> n | nil"
      );
    }
  };

  const EdBrushLuaBootstrap gEdBrushLuaBootstrap{};
} // namespace
