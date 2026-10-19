// EDITOR: bulk prop removal (docs/projects/editor/MAP_EDITOR_DESIGN.md 19.28).
//
//   ed_prop_remove_many(indices) -> removed     indices: a Lua array of 1-based prop numbers (any order, duplicates and numbers out of range are ignored). One pass over the
//                                               map's prop list, instead of ed_prop_remove(i) once per prop: that rebuilds the whole list each time (msvc8::vector has no erase),
//                                               17 ms per call on a map with 40000 props, so removing half of them took minutes (mirror of a half map, replace by blueprint, delete of
//                                               a big selection). Returns how many props went. The numbers of the props behind a removed one shift down by one, as with ed_prop_remove.

#include <algorithm>
#include <cstddef>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"

namespace
{
  [[nodiscard]] moho::CWldProps* LiveProps()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr) {
      return nullptr;
    }
    return session->mWldMap->mProps;
  }

  int __cdecl cfunc_EdPropRemoveMany(lua_State* const L)
  {
    moho::CWldProps* const props = LiveProps();
    if (props == nullptr || lua_type(L, 1) != LUA_TTABLE) {
      lua_pushnumber(L, 0);
      return 1;
    }
    const std::size_t count = props->mEntries.size();
    std::vector<char> drop(count, 0);
    std::size_t removed = 0;
    for (int i = 1;; ++i) {
      lua_rawgeti(L, 1, i);
      if (lua_type(L, -1) != LUA_TNUMBER) {
        lua_settop(L, -2);
        break;
      }
      const int index = static_cast<int>(lua_tonumber(L, -1)) - 1;
      lua_settop(L, -2);
      if (index >= 0 && static_cast<std::size_t>(index) < count && drop[static_cast<std::size_t>(index)] == 0) {
        drop[static_cast<std::size_t>(index)] = 1;
        ++removed;
      }
    }
    if (removed > 0) {
      std::vector<moho::CWldPropEntry> keep;
      keep.reserve(count - removed);
      for (std::size_t i = 0; i < count; ++i) {
        if (drop[i] == 0) {
          keep.push_back(props->mEntries[i]);
        }
      }
      props->mEntries.clear();
      for (const moho::CWldPropEntry& e : keep) {
        props->mEntries.push_back(e);
      }
    }
    lua_pushnumber(L, static_cast<lua_Number>(removed));
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

  struct EdPropBulkLuaBootstrap
  {
    EdPropBulkLuaBootstrap()
    {
      moho::CScrLuaInitFormSet& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_prop_remove_many", &cfunc_EdPropRemoveMany, nullptr, "<global>", "ed_prop_remove_many(indices) -> removed");
    }
  };

  const EdPropBulkLuaBootstrap gEdPropBulkLuaBootstrap{};
} // namespace
