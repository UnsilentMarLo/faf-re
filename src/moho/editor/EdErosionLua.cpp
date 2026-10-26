// EDITOR: Lua natives of the local erosion brush (docs/projects/editor/TERRAIN_GEN_DESIGN.md, stage T3; the solver is EdErosion.cpp).
//
//   ed_erode_begin([params])                      -> ok | nil, message   starts a stroke session: copies the map heights as floats, water / sediment start empty.
//                                                 params (all optional numbers): rain evaporation gain capacity erosion deposition depth_max min_slope cut_limit talus thermal thermal_every max_speed vary vary_scale
//                                                 inertia conserve seed. The brush runs water droplets (EdErosion.h): rain scales how many are released per round, capacity / erosion / deposition / evaporation /
//                                                 min_slope / cut_limit / inertia are theirs, thermal / talus / thermal_every the slides after them; gain, depth_max and max_speed belong to the grid solver and are ignored
//   ed_erode_window(cx, cz, radius[, {mode, margin}])  -> x0, z0, x1, z1    the inclusive sample window a dab would use (for building shaped brush weights); the erode and deposit brushes
//                                                 never change a sample outside the radius, the rain brush up to 0.9 * margin past it
//   ed_erode_dab(cx, cz, radius[, opts])          -> changed, x0, z0, x1, z1   one dab inside the open stroke of the document (ed_cmd_begin): runs the kernels on the window and writes
//                                                 the new heights. opts: iterations strength falloff margin amount weights (list for the window, row major), mode = 'erode' | 'rain' | 'deposit',
//                                                 shape (handle of ed_brush_shape_set, EdBrush.h) + angle (degrees): a shaped brush, the weights are made natively from the mask (cut at the radius)
//   ed_erode_end([settle])                        -> changed, x0, z0, x1, z1   settles the remaining sediment (default) into the terrain, writes it and frees the session
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/editor/EdBrush.h"
#include "moho/editor/EdErosion.h"
#include "moho/editor/EdFieldCore.h"
#include "moho/editor/EditorDocument.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace
{
  namespace er = moho::editor::field::erosion;

  er::ErodeSession gSession;

  int Fail(lua_State* const L, const char* const message)
  {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
  }

  double OptNum(lua_State* const L, const int index, const char* const key, const double fallback)
  {
    if (lua_type(L, index) != LUA_TTABLE) {
      return fallback;
    }
    lua_pushstring(L, key);
    lua_gettable(L, index);
    double v = fallback;
    if (lua_type(L, -1) == LUA_TNUMBER) {
      v = lua_tonumber(L, -1);
    }
    lua_settop(L, -2);
    return v;
  }

  std::string OptStr(lua_State* const L, const int index, const char* const key, const char* const fallback)
  {
    if (lua_type(L, index) != LUA_TTABLE) {
      return fallback;
    }
    lua_pushstring(L, key);
    lua_gettable(L, index);
    std::string v = lua_type(L, -1) == LUA_TSTRING ? std::string(lua_tostring(L, -1)) : std::string(fallback);
    lua_settop(L, -2);
    return v;
  }

  er::HydroParams ParamsOf(lua_State* const L, const int index)
  {
    er::HydroParams p;
    p.rain = static_cast<float>(OptNum(L, index, "rain", p.rain));
    p.evaporation = static_cast<float>(OptNum(L, index, "evaporation", p.evaporation));
    p.gain = static_cast<float>(OptNum(L, index, "gain", p.gain));
    p.capacity = static_cast<float>(OptNum(L, index, "capacity", p.capacity));
    p.erosion = static_cast<float>(OptNum(L, index, "erosion", p.erosion));
    p.deposition = static_cast<float>(OptNum(L, index, "deposition", p.deposition));
    p.depth_max = static_cast<float>(OptNum(L, index, "depth_max", p.depth_max));
    p.min_slope = static_cast<float>(OptNum(L, index, "min_slope", p.min_slope));
    p.cut_limit = static_cast<float>(OptNum(L, index, "cut_limit", p.cut_limit));
    p.talus = static_cast<float>(OptNum(L, index, "talus", p.talus));
    p.thermal = static_cast<float>(OptNum(L, index, "thermal", p.thermal));
    p.thermal_every = static_cast<int>(OptNum(L, index, "thermal_every", p.thermal_every));
    p.max_speed = static_cast<float>(OptNum(L, index, "max_speed", p.max_speed));
    p.vary = std::max(0.0f, std::min(1.0f, static_cast<float>(OptNum(L, index, "vary", p.vary))));
    p.vary_scale = std::max(2.0f, static_cast<float>(OptNum(L, index, "vary_scale", p.vary_scale)));
    p.inertia = std::max(0.0f, std::min(0.95f, static_cast<float>(OptNum(L, index, "inertia", p.inertia))));
    p.conserve = std::max(0.0f, std::min(1.0f, static_cast<float>(OptNum(L, index, "conserve", p.conserve))));
    p.seed = static_cast<std::uint32_t>(OptNum(L, index, "seed", static_cast<double>(p.seed)));
    return p;
  }

  int ModeOf(const std::string& s) { return s == "rain" ? 1 : (s == "deposit" ? 2 : 0); }

  int __cdecl cfunc_Begin(lua_State* const L)
  {
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    moho::editor::MapDocument& doc = moho::editor::Document();
    moho::editor::CellRect all;
    all.x0 = 0;
    all.z0 = 0;
    all.x1 = doc.Width() - 1;
    all.z1 = doc.Height() - 1;
    std::vector<std::uint16_t> words;
    doc.ReadRect(all, words);
    gSession.Begin(doc.Width(), doc.Height(), words.data(), ParamsOf(L, 1));
    if (!gSession.Active()) {
      return Fail(L, "the map is too small");
    }
    lua_pushnumber(L, 1);
    return 1;
  }

  int __cdecl cfunc_Window(lua_State* const L)
  {
    // the window rule of ErodeSession::Dab, without running it
    const double cx = luaL_checknumber(L, 1), cz = luaL_checknumber(L, 2);
    const double radius = std::max(1.0, static_cast<double>(luaL_checknumber(L, 3)));
    er::BrushDab probe;
    probe.cx = cx;
    probe.cz = cz;
    probe.radius = radius;
    probe.mode = ModeOf(OptStr(L, 4, "mode", "erode"));
    probe.margin = OptNum(L, 4, "margin", -1.0);
    moho::editor::MapDocument& doc = moho::editor::Document();
    int x0, z0, x1, z1;
    er::BrushWindow(probe, doc.Width(), doc.Height(), x0, z0, x1, z1);
    lua_pushnumber(L, x0);
    lua_pushnumber(L, z0);
    lua_pushnumber(L, x1);
    lua_pushnumber(L, z1);
    return 4;
  }

  int WriteBack(lua_State* const L, const int x0, const int z0, const int x1, const int z1, const std::vector<std::uint16_t>& words)
  {
    moho::editor::MapDocument& doc = moho::editor::Document();
    moho::editor::CellRect rect;
    rect.x0 = x0;
    rect.z0 = z0;
    rect.x1 = x1;
    rect.z1 = z1;
    const std::size_t changed = doc.EditHeights(rect, words.data());
    lua_pushnumber(L, static_cast<lua_Number>(changed));
    lua_pushnumber(L, x0);
    lua_pushnumber(L, z0);
    lua_pushnumber(L, x1);
    lua_pushnumber(L, z1);
    return 5;
  }

  int __cdecl cfunc_Dab(lua_State* const L)
  {
    if (!gSession.Active() || !moho::editor::AttachDocument(false)) {
      return Fail(L, "no erosion session (ed_erode_begin)");
    }
    er::BrushDab dab;
    dab.cx = luaL_checknumber(L, 1);
    dab.cz = luaL_checknumber(L, 2);
    dab.radius = luaL_checknumber(L, 3);
    dab.iterations = static_cast<int>(OptNum(L, 4, "iterations", dab.iterations));
    dab.strength = OptNum(L, 4, "strength", dab.strength);
    dab.falloff = OptNum(L, 4, "falloff", dab.falloff);
    dab.margin = OptNum(L, 4, "margin", dab.margin);
    dab.amount = OptNum(L, 4, "amount", dab.amount);
    dab.mode = ModeOf(OptStr(L, 4, "mode", "erode"));
    if (lua_type(L, 4) == LUA_TTABLE) {
      lua_pushstring(L, "weights");
      lua_gettable(L, 4);
      if (lua_type(L, -1) == LUA_TTABLE) {
        const int t = lua_gettop(L);
        for (int i = 1;; ++i) {
          lua_rawgeti(L, t, i);
          if (lua_type(L, -1) != LUA_TNUMBER) {
            lua_settop(L, -2);
            break;
          }
          dab.weights.push_back(static_cast<float>(lua_tonumber(L, -1)));
          lua_settop(L, -2);
        }
      }
      lua_settop(L, -2);
    }
    // opts.shape (a handle of ed_brush_shape_set) and opts.angle (degrees): a shaped brush. The weights of the dab's window are made here from the stored mask, instead of by a Lua callback
    // per sample (terrain.lua used to hand a list built by Brush.Weight). Unknown handle: no weights, the round falloff.
    if (lua_type(L, 4) == LUA_TTABLE) {
      const int shapeHandle = static_cast<int>(OptNum(L, 4, "shape", 0.0));
      const moho::editor::brush::Shape* const shape = shapeHandle > 0 ? moho::editor::brush::FindShape(shapeHandle) : nullptr;
      if (shape != nullptr) {
        moho::editor::MapDocument& doc = moho::editor::Document();
        int wx0, wz0, wx1, wz1;
        er::BrushWindow(dab, doc.Width(), doc.Height(), wx0, wz0, wx1, wz1);       // the same rule as ErodeSession::Dab
        moho::editor::brush::FillShapeWeights(*shape, static_cast<float>(OptNum(L, 4, "angle", 0.0)), static_cast<float>(dab.cx), static_cast<float>(dab.cz),
                                              static_cast<float>(dab.radius), wx0, wz0, wx1, wz1, dab.weights);
      }
    }
    int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
    std::vector<std::uint16_t> words;
    if (!gSession.Dab(dab, x0, z0, x1, z1, words)) {
      lua_pushnumber(L, 0);
      return 1;
    }
    return WriteBack(L, x0, z0, x1, z1, words);
  }

  int __cdecl cfunc_End(lua_State* const L)
  {
    if (!gSession.Active()) {
      lua_pushnumber(L, 0);
      return 1;
    }
    const bool settle = !(lua_type(L, 1) == LUA_TBOOLEAN && lua_toboolean(L, 1) == 0);
    int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
    std::vector<std::uint16_t> words;
    int results = 1;
    if (settle && moho::editor::AttachDocument(false) && gSession.Finish(x0, z0, x1, z1, words)) {
      results = WriteBack(L, x0, z0, x1, z1, words);
    } else {
      lua_pushnumber(L, 0);
    }
    gSession.End();
    return results;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& UserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  struct EdErosionLuaBootstrap
  {
    EdErosionLuaBootstrap()
    {
      moho::editor::OnSessionEnd([] { gSession.End(); });   // the session's terrain is gone: drop the working copy
      auto& set = UserLuaInitSet();
      const auto bind = [&](const char* name, int (__cdecl* fn)(lua_State*), const char* doc) { (void)new moho::CScrLuaBinder(set, name, fn, nullptr, "<global>", doc); };
      bind("ed_erode_begin", &cfunc_Begin, "ed_erode_begin([params]) -> ok | nil, message");
      bind("ed_erode_window", &cfunc_Window, "ed_erode_window(cx, cz, radius[, {mode, margin}]) -> x0, z0, x1, z1");
      bind("ed_erode_dab", &cfunc_Dab, "ed_erode_dab(cx, cz, radius[, opts]) -> changed, x0, z0, x1, z1");
      bind("ed_erode_end", &cfunc_End, "ed_erode_end([settle]) -> changed, x0, z0, x1, z1");
    }
  };

  const EdErosionLuaBootstrap gEdErosionLuaBootstrap{};
} // namespace
