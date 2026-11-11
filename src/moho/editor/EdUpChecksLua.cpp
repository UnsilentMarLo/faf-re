// EDITOR: upstream's map checks as natives (EdUpCommon.h; docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP7). The logic is src/editor/tools/EdChecks.cpp of the library.
//
//   ed_up_check_props(props, symmetry [, turns, width, height, reach, listed]) -> issues
//       The props' symmetry: a prop with no prop of its blueprint within `reach` (default 2) of one of its symmetric copies is unmatched (its reclaim is not the same for every player).
//       props = array of { blueprint, x, z, mass } (positional: [1] = the blueprint path, [2] = x, [3] = z, [4] = reclaim mass, 0 when unknown); symmetry = our mode ('off', 'mirror_x',
//       'mirror_z', 'mirror_xz', 'rot2', 'rot4', 'diag', 'adiag', 'diag_both', 'rotn' with `turns`) or upstream's name ('none', 'left_right', 'top_bottom', 'diagonal_back', 'diagonal_forward',
//       'half_turn', 'both_axes', 'both_diagonals', 'quarter_turns', 'n_way'); width, height = the height field's size in samples (the copies mirror about (width - 1) / 2; for a symmetry
//       centre elsewhere pass 2 * centre + 1). issues = array of { level = 'error' | 'warning' | 'note', marker, text, x, z }: a summary first (a warning when unmatched props hold reclaim mass,
//       else a note), then the first `listed` (default 20) unmatched props, each at its place; empty when everything matches, the symmetry is off or the size is not usable.
//   ed_up_check_lua(text [, name]) -> { ok = true } | { ok = false, line = n | nil, message, raw }
//       Compiles a Lua text with the engine's own parser (luaL_loadbuffer: nothing is run, nothing raises) and splits the error into its line and message (faf::editor::ParseLuaError,
//       the "SplitLuaError" of the plan). `name` is the chunk name shown in the error (default "_script.lua").
//   ed_up_parse_lua_error(error) -> line, message | nil
//       The same split for an error text (`name(12): message`, `[string "x"]:12: message` or `x:12: message`); nil when it has no line.
//   ed_up_decal_path(points, length, width [, overlap]) -> decals
//       Upstream's decal path (EdDecalShape.cpp DecalsAlongPath): decals laid end to end along the Catmull-Rom curve through `points` (array of { x, z }), each `length` long along the curve and
//       `width` across, turned along it, the next starting length * (1 - overlap) further on (overlap 0..0.9, default 0). decals = array of { cx, cz (the middle), x, z (the corner the game
//       keeps), sizeX, sizeZ, yaw (radians) }; empty for fewer than two points.
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "editor/tools/EdChecks.h"
#include "editor/tools/EdDecalShape.h"
#include "editor/tools/EdHeightBrush.h"
#include "moho/editor/EdUpCommon.h"

namespace
{
  namespace up = moho::editor::up;
  using faf::editor::CheckIssue;
  using faf::editor::Symmetry;

  bool SymmetryFromName(const std::string& name, Symmetry& out)
  {
    struct Entry
    {
      const char* name;
      Symmetry symmetry;
    };
    static const Entry kNames[] = {
      {"off", Symmetry::None},
      {"none", Symmetry::None},
      {"mirror_x", Symmetry::LeftRight},
      {"left_right", Symmetry::LeftRight},
      {"mirror_z", Symmetry::TopBottom},
      {"top_bottom", Symmetry::TopBottom},
      {"diag", Symmetry::DiagonalBack},
      {"diagonal_back", Symmetry::DiagonalBack},
      {"adiag", Symmetry::DiagonalForward},
      {"diagonal_forward", Symmetry::DiagonalForward},
      {"rot2", Symmetry::HalfTurn},
      {"half_turn", Symmetry::HalfTurn},
      {"mirror_xz", Symmetry::BothAxes},
      {"both_axes", Symmetry::BothAxes},
      {"diag_both", Symmetry::BothDiagonals},
      {"both_diagonals", Symmetry::BothDiagonals},
      {"rot4", Symmetry::QuarterTurns},
      {"quarter_turns", Symmetry::QuarterTurns},
      {"rotn", Symmetry::NWay},
      {"n_way", Symmetry::NWay},
    };
    for (const Entry& e : kNames) {
      if (name == e.name) {
        out = e.symmetry;
        return true;
      }
    }
    return false;
  }

  /** Reads t[i] of the array at `list` (absolute index) as a number, or `fallback`. */
  double ElementNumber(lua_State* const L, const int list, const int i, const double fallback)
  {
    lua_rawgeti(L, list, i);
    const double v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<double>(lua_tonumber(L, -1)) : fallback;
    lua_settop(L, -2);
    return v;
  }

  int __cdecl cfunc_UpCheckProps(lua_State* const L)
  {
    if (lua_type(L, 1) != LUA_TTABLE) {
      return up::Fail(L, "ed_up_check_props: argument 1 must be a table of props");
    }
    Symmetry symmetry = Symmetry::None;
    if (lua_type(L, 2) != LUA_TSTRING || !SymmetryFromName(lua_tostring(L, 2), symmetry)) {
      return up::Fail(L, "ed_up_check_props: argument 2 must be a symmetry name");
    }
    const int turns = lua_type(L, 3) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, 3)) : 3;
    const int width = lua_type(L, 4) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, 4)) : 0;
    const int height = lua_type(L, 5) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, 5)) : width;
    const float reach = lua_type(L, 6) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, 6)) : 2.0f;
    const std::size_t listed =
      lua_type(L, 7) == LUA_TNUMBER ? static_cast<std::size_t>(std::max(0.0, static_cast<double>(lua_tonumber(L, 7)))) : static_cast<std::size_t>(20);

    std::vector<faf::editor::PropPoint> props;
    for (int i = 1;; ++i) {
      lua_rawgeti(L, 1, i);
      if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, -2);
        break;
      }
      const int entry = lua_gettop(L);
      faf::editor::PropPoint point;
      lua_rawgeti(L, entry, 1);
      if (lua_type(L, -1) == LUA_TSTRING) {
        point.blueprint.assign(lua_tostring(L, -1), lua_strlen(L, -1));
      }
      lua_settop(L, -2);
      point.x = static_cast<float>(ElementNumber(L, entry, 2, 0.0));
      point.z = static_cast<float>(ElementNumber(L, entry, 3, 0.0));
      point.mass = static_cast<float>(ElementNumber(L, entry, 4, 0.0));
      props.push_back(std::move(point));
      lua_settop(L, -2);
    }

    const std::vector<CheckIssue> issues = faf::editor::CheckPropSymmetry(props, symmetry, turns, width, height, reach, listed);
    lua_newtable(L);
    int n = 0;
    for (const CheckIssue& issue : issues) {
      lua_pushnumber(L, ++n);
      lua_newtable(L);
      up::SetString(L, "level", faf::editor::CheckLevelName(issue.level));
      up::SetString(L, "marker", issue.marker);
      up::SetString(L, "text", issue.text);
      up::SetNumber(L, "x", issue.x);
      up::SetNumber(L, "z", issue.z);
      lua_settable(L, -3);
    }
    return 1;
  }

  int __cdecl cfunc_UpCheckLua(lua_State* const L)
  {
    const std::string text = up::ArgString(L, 1, "ed_up_check_lua");
    const std::string name = lua_type(L, 2) == LUA_TSTRING ? std::string(lua_tostring(L, 2)) : std::string("_script.lua");
    const int top = lua_gettop(L);
    const std::string chunk = "=" + name;
    const bool failed = luaL_loadbuffer(L, text.data(), text.size(), chunk.c_str()) != 0;
    std::string raw;
    if (failed) {
      const char* const message = lua_tostring(L, -1);
      raw = message != nullptr ? message : "does not compile";
    }
    lua_settop(L, top);
    lua_newtable(L);
    up::SetBool(L, "ok", !failed);
    if (failed) {
      int line = 0;
      std::string message;
      if (faf::editor::ParseLuaError(raw, line, message)) {
        up::SetNumber(L, "line", line);
        up::SetString(L, "message", message);
      } else {
        up::SetString(L, "message", raw);
      }
      up::SetString(L, "raw", raw);
    }
    return 1;
  }

  int __cdecl cfunc_UpParseLuaError(lua_State* const L)
  {
    const std::string error = up::ArgString(L, 1, "ed_up_parse_lua_error");
    int line = 0;
    std::string message;
    if (!faf::editor::ParseLuaError(error, line, message)) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushnumber(L, line);
    lua_pushlstring(L, message.data(), message.size());
    return 2;
  }

  int __cdecl cfunc_UpDecalPath(lua_State* const L)
  {
    if (lua_type(L, 1) != LUA_TTABLE) {
      return up::Fail(L, "ed_up_decal_path: argument 1 must be an array of { x, z } points");
    }
    const float length = static_cast<float>(luaL_checknumber(L, 2));
    const float width = static_cast<float>(luaL_checknumber(L, 3));
    const float overlap = lua_type(L, 4) == LUA_TNUMBER ? std::clamp(static_cast<float>(lua_tonumber(L, 4)), 0.0f, 0.9f) : 0.0f;
    std::vector<std::pair<float, float>> points;
    for (int i = 1;; ++i) {
      lua_rawgeti(L, 1, i);
      if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, -2);
        break;
      }
      const int entry = lua_gettop(L);
      const float x = static_cast<float>(up::FieldNumber(L, entry, "x", ElementNumber(L, entry, 1, 0.0)));
      const float z = static_cast<float>(up::FieldNumber(L, entry, "z", ElementNumber(L, entry, 2, 0.0)));
      points.emplace_back(x, z);
      lua_settop(L, -2);
    }
    lua_newtable(L);
    if (points.size() < 2 || !(length > 0.0f) || !(width > 0.0f)) {
      return 1;
    }
    int n = 0;
    for (const faf::editor::DecalFootprint& d : faf::editor::DecalsAlongPath(points, length, width, overlap)) {
      float cx = 0.0f;
      float cz = 0.0f;
      faf::editor::DecalCentre(d, cx, cz);
      lua_pushnumber(L, ++n);
      lua_newtable(L);
      up::SetNumber(L, "cx", cx);
      up::SetNumber(L, "cz", cz);
      up::SetNumber(L, "x", d.x);
      up::SetNumber(L, "z", d.z);
      up::SetNumber(L, "sizeX", d.sizeX);
      up::SetNumber(L, "sizeZ", d.sizeZ);
      up::SetNumber(L, "yaw", d.yaw);
      lua_settable(L, -3);
      if (n >= 4000) {
        break;
      }
    }
    return 1;
  }

  const up::Natives gEdUpChecksNatives{{
    {"ed_up_check_props", &cfunc_UpCheckProps, "ed_up_check_props(props, symmetry [, turns, width, height, reach, listed]) -> issues"},
    {"ed_up_check_lua", &cfunc_UpCheckLua, "ed_up_check_lua(text [, name]) -> { ok, line, message, raw }"},
    {"ed_up_parse_lua_error", &cfunc_UpParseLuaError, "ed_up_parse_lua_error(error) -> line, message | nil"},
    {"ed_up_decal_path", &cfunc_UpDecalPath, "ed_up_decal_path(points, length, width [, overlap]) -> decals"},
  }};
} // namespace
