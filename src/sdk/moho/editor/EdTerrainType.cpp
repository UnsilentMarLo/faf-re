// Editor natives for the terrain-type grid (one byte per map cell, the TypeCode of a /lua/TerrainTypes.lua entry; it drives effects, sounds, the
// blocking flag and the unit style):
//
//   ed_tt_info()                              -> width, height   (cells; nil without a grid)
//   ed_tt_read(x0, z0, x1, z1)                -> values, x0, z0, x1, z1   (clamped, row major)
//   ed_tt_write(x0, z0, x1, z1, values)       -> number of cells written
//
// The grid exists twice in a running editor: in the map the world session shows and in the simulation's own copy. Both are written so the sim
// (blocking cells, effects) sees the edit at once; the map writer saves the session's grid.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/ISTIDriver.h"
#include "moho/sim/STIMap.h"
#include "moho/sim/Sim.h"
#include "moho/sim/SimDriver.h"

namespace
{
  moho::STIMap* SessionMap()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr || session->mWldMap->mTerrainRes == nullptr) {
      return nullptr;
    }
    return session->mWldMap->mTerrainRes->mMap;
  }

  moho::STIMap* SimMap()
  {
    if (moho::ISTIDriver* const driver = moho::WLD_GetDriver(); driver != nullptr) {
      if (auto* const simDriver = dynamic_cast<moho::CSimDriver*>(driver); simDriver != nullptr) {
        if (moho::Sim* const sim = simDriver->GetSimForEditor(); sim != nullptr) {
          return sim->mMapData;
        }
      }
    }
    return nullptr;
  }

  bool HasGrid(const moho::STIMap* const m)
  {
    return m != nullptr && m->mTerrainType.data != nullptr && m->mTerrainType.width > 0 && m->mTerrainType.height > 0;
  }

  int __cdecl cfunc_EdTtInfo(lua_State* const L)
  {
    const moho::STIMap* const m = SessionMap();
    if (!HasGrid(m)) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(m->mTerrainType.width));
    lua_pushnumber(L, static_cast<lua_Number>(m->mTerrainType.height));
    return 2;
  }

  int __cdecl cfunc_EdTtRead(lua_State* const L)
  {
    const moho::STIMap* const m = SessionMap();
    int x0 = static_cast<int>(luaL_checknumber(L, 1));
    int z0 = static_cast<int>(luaL_checknumber(L, 2));
    int x1 = static_cast<int>(luaL_checknumber(L, 3));
    int z1 = static_cast<int>(luaL_checknumber(L, 4));
    if (!HasGrid(m)) {
      lua_pushnil(L);
      return 1;
    }
    const int w = m->mTerrainType.width;
    const int h = m->mTerrainType.height;
    x0 = std::max(0, x0);
    z0 = std::max(0, z0);
    x1 = std::min(w - 1, x1);
    z1 = std::min(h - 1, z1);
    lua_newtable(L);
    if (x0 > x1 || z0 > z1) {
      lua_pushnumber(L, static_cast<lua_Number>(x0));
      lua_pushnumber(L, static_cast<lua_Number>(z0));
      lua_pushnumber(L, static_cast<lua_Number>(x1));
      lua_pushnumber(L, static_cast<lua_Number>(z1));
      return 5;
    }
    int index = 1;
    for (int z = z0; z <= z1; ++z) {
      for (int x = x0; x <= x1; ++x, ++index) {
        lua_pushnumber(L, static_cast<lua_Number>(index));
        lua_pushnumber(L, static_cast<lua_Number>(m->mTerrainType.data[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]));
        lua_settable(L, -3);
      }
    }
    lua_pushnumber(L, static_cast<lua_Number>(x0));
    lua_pushnumber(L, static_cast<lua_Number>(z0));
    lua_pushnumber(L, static_cast<lua_Number>(x1));
    lua_pushnumber(L, static_cast<lua_Number>(z1));
    return 5;
  }

  void WriteGrid(moho::STIMap* const m, const int x0, const int z0, const int x1, const int z1, const std::vector<std::uint8_t>& values)
  {
    if (!HasGrid(m)) {
      return;
    }
    const int w = m->mTerrainType.width;
    const int h = m->mTerrainType.height;
    int i = 0;
    for (int z = z0; z <= z1; ++z) {
      for (int x = x0; x <= x1; ++x, ++i) {
        if (x >= 0 && z >= 0 && x < w && z < h) {
          m->mTerrainType.data[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = values[static_cast<std::size_t>(i)];
        }
      }
    }
  }

  int __cdecl cfunc_EdTtWrite(lua_State* const L)
  {
    moho::STIMap* const m = SessionMap();
    const int x0 = static_cast<int>(luaL_checknumber(L, 1));
    const int z0 = static_cast<int>(luaL_checknumber(L, 2));
    const int x1 = static_cast<int>(luaL_checknumber(L, 3));
    const int z1 = static_cast<int>(luaL_checknumber(L, 4));
    luaL_checktype(L, 5, LUA_TTABLE);
    if (!HasGrid(m) || x0 > x1 || z0 > z1 || x0 < 0 || z0 < 0 || x1 >= m->mTerrainType.width || z1 >= m->mTerrainType.height) {
      lua_pushnumber(L, 0);
      return 1;
    }
    const std::size_t count = static_cast<std::size_t>(x1 - x0 + 1) * static_cast<std::size_t>(z1 - z0 + 1);
    std::vector<std::uint8_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
      lua_rawgeti(L, 5, static_cast<int>(i + 1));
      const double v = lua_tonumber(L, -1);
      lua_settop(L, -2);
      values[i] = static_cast<std::uint8_t>(std::max(0.0, std::min(255.0, v)));
    }
    WriteGrid(m, x0, z0, x1, z1, values);
    moho::STIMap* const sim = SimMap();
    if (sim != nullptr && sim != m) {
      WriteGrid(sim, x0, z0, x1, z1, values);
    }
    lua_pushnumber(L, static_cast<lua_Number>(count));
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

  struct EdTerrainTypeLuaBootstrap
  {
    EdTerrainTypeLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_tt_info", &cfunc_EdTtInfo, nullptr, "<global>", "ed_tt_info() -> width, height");
      (void)new moho::CScrLuaBinder(set, "ed_tt_read", &cfunc_EdTtRead, nullptr, "<global>", "ed_tt_read(x0, z0, x1, z1) -> values, x0, z0, x1, z1");
      (void)new moho::CScrLuaBinder(set, "ed_tt_write", &cfunc_EdTtWrite, nullptr, "<global>", "ed_tt_write(x0, z0, x1, z1, values) -> cells written");
    }
  };

  const EdTerrainTypeLuaBootstrap gEdTerrainTypeLuaBootstrap{};
} // namespace
