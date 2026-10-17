// Editor addition: ed_sim_clear_deposits(), a global of the simulation's Lua state.
//
// The game creates a map's resource deposits once, at load (ScenarioUtilities.CreateResources -> CreateResourceDeposit). In the
// editor the markers move, appear and disappear, and the mass extractor / hydrocarbon plant refuse to be built anywhere without a
// deposit, so the editor's marker validation (lua/editor/validate.lua, simcallbacks.lua EdValidateBuild) clears the deposits and
// creates them again from the map document's resource markers before it asks the sim's own build check.

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/resource/CSimResources.h"
#include "moho/sim/Sim.h"

namespace
{
  // ed_sim_clear_deposits()
  int __cdecl cfunc_EdSimClearDeposits(lua_State* const state)
  {
    moho::Sim* const sim = (state != nullptr && state->l_G != nullptr) ? state->l_G->globalUserData : nullptr;
    if (sim != nullptr && sim->mSimResources.px != nullptr) {
      sim->mSimResources.px->EditorClearDeposits();
    }
    return 0;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& SimLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("Sim"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("Sim");
    return fallbackSet;
  }

  struct EdSimResourcesBootstrap
  {
    EdSimResourcesBootstrap()
    {
      (void)new moho::CScrLuaBinder(SimLuaInitSet(), "ed_sim_clear_deposits", &cfunc_EdSimClearDeposits, nullptr, "<global>",
                                    "ed_sim_clear_deposits()");
    }
  };

  const EdSimResourcesBootstrap gEdSimResourcesBootstrap{};
} // namespace
