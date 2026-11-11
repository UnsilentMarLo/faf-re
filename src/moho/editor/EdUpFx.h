// EDITOR: what the particle editor's two binding units share (EdUpFxLua.cpp: the blueprint logic as natives; EdFxView.cpp: the preview).
#pragma once

#include "editor/doc/EdEmitter.h"
#include "lua/LuaRuntimeTypes.h"

namespace moho::editor::up
{
  /** The emitter a Lua table describes (a field it lacks keeps retail's starting value), see EdUpFxLua.cpp. */
  [[nodiscard]] faf::editor::EmitterBlueprint FxReadEmitter(lua_State* L, int index);
  /** The beam / trail a Lua table describes (`kind`, `fields`). */
  [[nodiscard]] faf::editor::FlatBlueprint FxReadFlat(lua_State* L, int index);
} // namespace moho::editor::up
