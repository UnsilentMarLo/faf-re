// EDITOR: the "up" natives of adaptive maps (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP5): upstream's `_tables.lua` / `_options.lua` text
// model, the lobby-option rules read from a map's script and what becomes of a unit, bound to Lua over EdUpCommon.h. The logic is upstream's
// (src/editor/doc/EdAdaptiveTables, EdAdaptiveModel, EdGroupOptions, EdUnitFate, unchanged, in faf_editor_logic); this unit is table <-> struct glue, plus
// `ed_wreck_value`, which replaces upstream's EdUnitWrecks (that one needs the EdDocument) by a lookup of the rules' blueprint.
//
// Conventions. Text in, text out: a native that changes a file takes the file's text and returns the new text, the file is the caller's (adaptive.lua reads and
// writes `_tables.lua` / `_options.lua`). Lua tables are plain: lists are arrays, structs are tables with the field names below. Every index into a list of
// values / options / groups that crosses the boundary is 1-BASED and 0 means "none / not offered" (upstream's 0-based and -1).
//
//   Tables (`_tables.lua`)
//     ed_adaptive_known_tables()                -> { { name, label, hydro, optionKey }, ... }     the option tables the script reads, in its order
//     ed_adaptive_tables_parse(text)            -> tables | nil, message
//          tables = { maxPlayers, mexArmy = { {markerName, ...}, ... }, hydroArmy = {...}, options = { { name, groups = { {markerName, ...}, ... } }, ... },
//                     forwardCrazyrush = {...}, crazyrushOne = {...} }       (marker names, "Mass 07")
//     ed_adaptive_tables_write(text, tables)    -> text      the file with the tables the script uses written in (the rest, its comments, kept); text '' = a new file
//     ed_adaptive_tables_path(script)           -> '/maps/x/x_tables.lua' | ''     the file the map's script imports ('' = not an adaptive script)
//     ed_adaptive_scenario(scenarioText)        -> text      `_scenario.lua` with AdaptiveMap = true
//     ed_adaptive_entry(markerName, hydro)      -> '7' | "'7'" | ''      the entry the script reads for a marker ('' = not that type)
//     ed_adaptive_marker(entry, hydro)          -> markerName
//     ed_adaptive_option_key(tableName)         -> 'middle_mexes'
//   Lobby options (`_options.lua`)
//     ed_adaptive_options_parse(text)           -> { { key, label, help, defaultIndex, values = { { key, number, numberKey, text, help }, ... } }, ... }
//     ed_adaptive_option_find(text, key)        -> { found, values, defaultKey }
//     ed_adaptive_option_put(text, option)      -> text      WithOption: the option as the table has it, its keys kept
//     ed_adaptive_option_for(text, key, label, hydro, groupSizes) -> text      WithLobbyOption: values 1 (none) .. groups + 1 with the groups' resource counts
//     ed_adaptive_option_values(text, key, label, help, values) -> text        values = { { text, help }, ... } (keys 1, 2, ...)
//     ed_adaptive_option_write(text, key, label, help, defaultIndex, values) -> text     values = { { key, text, help }, ... }, key as a Lua literal
//     ed_adaptive_option_remove(text, key)      -> text
//     ed_adaptive_next_level(option, text)      -> value      the next counted value of a level option, as { key, number, numberKey, text, help }
//   The setup by what the player picks
//     ed_adaptive_options_of(script, optionsText, tables) -> { { kind = 'mexes' | 'hydros' | 'levels' | 'other', key, lobby (1-based, 0 = not in the lobby),
//                                                                table, tableLabel, loop = {..}, title, scriptReads }, ... }
//          `tables` is a parsed tables table (or the text of `_tables.lua`).
//     ed_adaptive_table_group(value)            -> n         the table group a choice adds (1-based), 0 for "none"
//     ed_adaptive_level_group(loop, value)      -> name      the group a level choice adds, or ''
//     ed_adaptive_groups_made(script, optionsText, key, valueIndex) -> { { group, wreck, unit, maybe }, ... }
//     ed_adaptive_loops(script)                 -> { { prefix, army, key, from, wreck }, ... }
//     ed_adaptive_creations(script)             -> { { army, name, prefix, wreck }, ... }
//   Units
//     ed_adaptive_rule(script, optionsText, group) -> { scriptNames, never, keys, optionIndex, scriptDefault, cases = { { picks, made, asWreck, asUnit } },
//                                                       lines = { text, ... }, when }
//     ed_adaptive_spawn_targets(script, optionsText, army, groupPaths) -> { { path, exists, rule, when, wreck, unit, optionsWith, optionsChange }, ... }
//     ed_adaptive_fate(path, civilian, script)  -> { kind, group, text }       path = { 'INITIAL', 'Base' }; kind = unit | wreck | scriptUnit | scriptWreck |
//                                                                              scriptEither | never
//     ed_wreck_value(bpId, underwater)          -> { known, exists, mass, energy, buildMass, buildEnergy } | nil, message     the rules' blueprint
//     ed_wreck_value_of(bpText, buildMass, buildEnergy, underwater) -> { exists, mass, energy }

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "editor/doc/EdAdaptiveModel.h"
#include "editor/doc/EdAdaptiveTables.h"
#include "editor/doc/EdGroupOptions.h"
#include "editor/doc/EdUnitFate.h"
#include "gpg/core/streams/Stream.h"
#include "legacy/containers/String.h"
#include "moho/editor/EdUpCommon.h"
#include "moho/misc/CVirtualFileSystem.h"
#include "moho/misc/FileWaitHandleSet.h"
#include "moho/resource/RResId.h"
#include "moho/resource/blueprints/RUnitBlueprint.h"
#include "moho/sim/RRuleGameRules.h"

namespace
{
  namespace up = moho::editor::up;
  using namespace faf::editor;

  // ---- table <-> struct helpers ---------------------------------------------------------------------------------------------------------------------

  int Absolute(lua_State* const L, const int index)
  {
    return index < 0 && index > LUA_REGISTRYINDEX ? lua_gettop(L) + index + 1 : index;
  }

  /** Calls `fn(absoluteIndexOfElement)` for t[1], t[2], ... of the table at `table` (an empty loop for a non-table). */
  template <class F>
  void EachElement(lua_State* const L, const int table, F fn)
  {
    const int abs = Absolute(L, table);
    if (lua_type(L, abs) != LUA_TTABLE) {
      return;
    }
    for (int i = 1;; ++i) {
      lua_rawgeti(L, abs, i);
      if (lua_type(L, -1) == LUA_TNIL) {
        lua_settop(L, -2);
        return;
      }
      fn(lua_gettop(L));
      lua_settop(L, -2);
    }
  }

  std::string StringAt(lua_State* const L, const int index)
  {
    return lua_type(L, index) == LUA_TSTRING ? std::string(lua_tostring(L, index), lua_strlen(L, index)) : std::string();
  }

  std::vector<std::string> StringListAt(lua_State* const L, const int index)
  {
    std::vector<std::string> out;
    EachElement(L, index, [&](const int item) {
      if (lua_type(L, item) == LUA_TSTRING) {
        out.push_back(StringAt(L, item));
      }
    });
    return out;
  }

  /** The array of string arrays held by the field t[key]. */
  std::vector<std::vector<std::string>> NestedField(lua_State* const L, const int table, const char* const key)
  {
    std::vector<std::vector<std::string>> out;
    if (up::PushField(L, table, key) == LUA_TTABLE) {
      EachElement(L, lua_gettop(L), [&](const int item) { out.push_back(StringListAt(L, item)); });
    }
    lua_settop(L, -2);
    return out;
  }

  void SetNested(lua_State* const L, const char* const key, const std::vector<std::vector<std::string>>& lists)
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (std::size_t i = 0; i < lists.size(); ++i) {
      lua_newtable(L);
      for (std::size_t j = 0; j < lists[i].size(); ++j) {
        lua_pushlstring(L, lists[i][j].data(), lists[i][j].size());
        lua_rawseti(L, -2, static_cast<int>(j) + 1);
      }
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  void SetIntList(lua_State* const L, const char* const key, const std::vector<int>& values)
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (std::size_t i = 0; i < values.size(); ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(values[i]));
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  // ---- structs ------------------------------------------------------------------------------------------------------------------------------------

  AdaptiveTables ReadTables(lua_State* const L, const int index)
  {
    AdaptiveTables tables;
    tables.maxPlayers = up::FieldInt(L, index, "maxPlayers", tables.maxPlayers);
    tables.mexArmy = NestedField(L, index, "mexArmy");
    tables.hydroArmy = NestedField(L, index, "hydroArmy");
    if (up::PushField(L, index, "options") == LUA_TTABLE) {
      EachElement(L, lua_gettop(L), [&](const int item) {
        tables.options.emplace_back(up::FieldString(L, item, "name"), NestedField(L, item, "groups"));
      });
    }
    lua_settop(L, -2);
    tables.forwardCrazyrush = up::FieldStringList(L, index, "forwardCrazyrush");
    tables.crazyrushOne = up::FieldStringList(L, index, "crazyrushOne");
    return tables;
  }

  void PushTables(lua_State* const L, const AdaptiveTables& tables)
  {
    lua_newtable(L);
    up::SetNumber(L, "maxPlayers", tables.maxPlayers);
    SetNested(L, "mexArmy", tables.mexArmy);
    SetNested(L, "hydroArmy", tables.hydroArmy);
    lua_pushstring(L, "options");
    lua_newtable(L);
    for (std::size_t i = 0; i < tables.options.size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "name", tables.options[i].first);
      SetNested(L, "groups", tables.options[i].second);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
    up::SetStringList(L, "forwardCrazyrush", tables.forwardCrazyrush);
    up::SetStringList(L, "crazyrushOne", tables.crazyrushOne);
  }

  LobbyOptionInfo::Value ReadValue(lua_State* const L, const int index)
  {
    LobbyOptionInfo::Value value;
    value.key = up::FieldString(L, index, "key");
    value.number = up::FieldBool(L, index, "number");
    value.numberKey = up::FieldNumber(L, index, "numberKey");
    value.text = up::FieldString(L, index, "text");
    value.help = up::FieldString(L, index, "help");
    return value;
  }

  void PushValue(lua_State* const L, const LobbyOptionInfo::Value& value)
  {
    lua_newtable(L);
    up::SetString(L, "key", value.key);
    up::SetBool(L, "number", value.number);
    up::SetNumber(L, "numberKey", value.numberKey);
    up::SetString(L, "text", value.text);
    up::SetString(L, "help", value.help);
  }

  LobbyOptionInfo ReadOption(lua_State* const L, const int index)
  {
    LobbyOptionInfo option;
    option.key = up::FieldString(L, index, "key");
    option.label = up::FieldString(L, index, "label");
    option.help = up::FieldString(L, index, "help");
    option.defaultIndex = up::FieldInt(L, index, "defaultIndex");
    if (up::PushField(L, index, "values") == LUA_TTABLE) {
      EachElement(L, lua_gettop(L), [&](const int item) { option.values.push_back(ReadValue(L, item)); });
    }
    lua_settop(L, -2);
    return option;
  }

  void PushOption(lua_State* const L, const LobbyOptionInfo& option)
  {
    lua_newtable(L);
    up::SetString(L, "key", option.key);
    up::SetString(L, "label", option.label);
    up::SetString(L, "help", option.help);
    up::SetNumber(L, "defaultIndex", option.defaultIndex);
    lua_pushstring(L, "values");
    lua_newtable(L);
    for (std::size_t i = 0; i < option.values.size(); ++i) {
      PushValue(L, option.values[i]);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  ScriptGroupLoop ReadLoop(lua_State* const L, const int index)
  {
    ScriptGroupLoop loop;
    loop.prefix = up::FieldString(L, index, "prefix");
    loop.army = up::FieldString(L, index, "army");
    loop.key = up::FieldString(L, index, "key");
    loop.from = up::FieldInt(L, index, "from", 2);
    loop.wreck = up::FieldBool(L, index, "wreck");
    return loop;
  }

  void PushLoop(lua_State* const L, const ScriptGroupLoop& loop)
  {
    lua_newtable(L);
    up::SetString(L, "prefix", loop.prefix);
    up::SetString(L, "army", loop.army);
    up::SetString(L, "key", loop.key);
    up::SetNumber(L, "from", loop.from);
    up::SetBool(L, "wreck", loop.wreck);
  }

  const char* MadeName(const GroupLobbyRule::Made made)
  {
    return made == GroupLobbyRule::Made::Yes ? "yes" : made == GroupLobbyRule::Made::Maybe ? "maybe" : "no";
  }

  /** The rule as a table (indices 1-based, 0 = not offered); `lines` and `when` as the Properties panel words them. */
  void PushRule(lua_State* const L, const GroupLobbyRule& rule, const std::vector<LobbyOptionInfo>& options)
  {
    lua_newtable(L);
    up::SetBool(L, "scriptNames", rule.scriptNames);
    up::SetBool(L, "never", rule.Never());
    up::SetStringList(L, "keys", rule.keys);
    std::vector<int> optionIndex;
    for (const int i : rule.optionIndex) {
      optionIndex.push_back(i + 1);
    }
    SetIntList(L, "optionIndex", optionIndex);
    up::SetStringList(L, "scriptDefault", rule.scriptDefault);
    lua_pushstring(L, "cases");
    lua_newtable(L);
    for (std::size_t i = 0; i < rule.cases.size(); ++i) {
      lua_newtable(L);
      std::vector<int> picks;
      for (const int p : rule.cases[i].picks) {
        picks.push_back(p + 1);
      }
      SetIntList(L, "picks", picks);
      up::SetString(L, "made", MadeName(rule.cases[i].made));
      up::SetBool(L, "asWreck", rule.cases[i].asWreck);
      up::SetBool(L, "asUnit", rule.cases[i].asUnit);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
    up::SetStringList(L, "lines", DescribeLobbyRule(rule, options));
    up::SetString(L, "when", LobbyRuleWhen(rule, options));
  }

  const char* KindName(const AdaptiveOption::Kind kind)
  {
    switch (kind) {
    case AdaptiveOption::Kind::Mexes:
      return "mexes";
    case AdaptiveOption::Kind::Hydros:
      return "hydros";
    case AdaptiveOption::Kind::UnitLevels:
      return "levels";
    default:
      return "other";
    }
  }

  const char* FateName(const UnitFate::Kind kind)
  {
    switch (kind) {
    case UnitFate::Kind::Unit:
      return "unit";
    case UnitFate::Kind::Wreck:
      return "wreck";
    case UnitFate::Kind::ScriptUnit:
      return "scriptUnit";
    case UnitFate::Kind::ScriptWreck:
      return "scriptWreck";
    case UnitFate::Kind::ScriptEither:
      return "scriptEither";
    default:
      return "never";
    }
  }

  // ---- tables -------------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_KnownTables(lua_State* const L)
  {
    lua_newtable(L);
    int n = 0;
    for (const OptionTable& table : KnownOptionTables()) {
      lua_newtable(L);
      up::SetString(L, "name", table.name);
      up::SetString(L, "label", table.label);
      up::SetBool(L, "hydro", table.hydro);
      up::SetString(L, "optionKey", AdaptiveOptionKey(table.name));
      lua_rawseti(L, -2, ++n);
    }
    return 1;
  }

  int __cdecl cfunc_TablesParse(lua_State* const L)
  {
    const std::string text = up::ArgString(L, 1, "ed_adaptive_tables_parse");
    AdaptiveTables tables;
    std::string error;
    if (!ParseAdaptiveTables(text, tables, error)) {
      return up::Fail(L, error);
    }
    PushTables(L, tables);
    return 1;
  }

  int __cdecl cfunc_TablesWrite(lua_State* const L)
  {
    const std::string text = up::ArgString(L, 1, "ed_adaptive_tables_write");
    const std::string out = WithAdaptiveTables(text, ReadTables(L, 2));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
  }

  int __cdecl cfunc_TablesPath(lua_State* const L)
  {
    const std::string path = AdaptiveTablesPath(up::ArgString(L, 1, "ed_adaptive_tables_path"));
    lua_pushlstring(L, path.data(), path.size());
    return 1;
  }

  int __cdecl cfunc_Scenario(lua_State* const L)
  {
    const std::string out = WithScenarioAdaptive(up::ArgString(L, 1, "ed_adaptive_scenario"));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
  }

  int __cdecl cfunc_Entry(lua_State* const L)
  {
    const std::string out = AdaptiveEntry(up::ArgString(L, 1, "ed_adaptive_entry"), lua_toboolean(L, 2) != 0);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
  }

  int __cdecl cfunc_Marker(lua_State* const L)
  {
    const std::string out = AdaptiveMarkerName(up::ArgString(L, 1, "ed_adaptive_marker"), lua_toboolean(L, 2) != 0);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
  }

  int __cdecl cfunc_OptionKey(lua_State* const L)
  {
    const std::string out = AdaptiveOptionKey(up::ArgString(L, 1, "ed_adaptive_option_key"));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
  }

  // ---- lobby options ------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_OptionsParse(lua_State* const L)
  {
    const std::vector<LobbyOptionInfo> options = ParseLobbyOptions(up::ArgString(L, 1, "ed_adaptive_options_parse"));
    lua_newtable(L);
    for (std::size_t i = 0; i < options.size(); ++i) {
      PushOption(L, options[i]);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
  }

  int __cdecl cfunc_OptionFind(lua_State* const L)
  {
    const LobbyOption found = FindLobbyOption(up::ArgString(L, 1, "ed_adaptive_option_find"), up::ArgString(L, 2, "ed_adaptive_option_find"));
    lua_newtable(L);
    up::SetBool(L, "found", found.found);
    up::SetNumber(L, "values", found.values);
    up::SetNumber(L, "defaultKey", found.defaultKey);
    return 1;
  }

  int PushText(lua_State* const L, const std::string& text)
  {
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  int __cdecl cfunc_OptionPut(lua_State* const L)
  {
    return PushText(L, WithOption(up::ArgString(L, 1, "ed_adaptive_option_put"), ReadOption(L, 2)));
  }

  int __cdecl cfunc_OptionFor(lua_State* const L)
  {
    std::vector<int> sizes; // the array itself (argument 5)
    EachElement(L, 5, [&](const int item) { sizes.push_back(lua_type(L, item) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, item)) : 0); });
    return PushText(L, WithLobbyOption(up::ArgString(L, 1, "ed_adaptive_option_for"), up::ArgString(L, 2, "ed_adaptive_option_for"), StringAt(L, 3),
                                       lua_toboolean(L, 4) != 0, sizes));
  }

  int __cdecl cfunc_OptionValues(lua_State* const L)
  {
    std::vector<std::pair<std::string, std::string>> values;
    EachElement(L, 5, [&](const int item) { values.emplace_back(up::FieldString(L, item, "text"), up::FieldString(L, item, "help")); });
    return PushText(L, WithLobbyOptionValues(up::ArgString(L, 1, "ed_adaptive_option_values"), up::ArgString(L, 2, "ed_adaptive_option_values"),
                                             StringAt(L, 3), StringAt(L, 4), values));
  }

  int __cdecl cfunc_OptionWrite(lua_State* const L)
  {
    std::vector<LobbyValueText> values;
    EachElement(L, 6, [&](const int item) {
      values.push_back({up::FieldString(L, item, "key"), up::FieldString(L, item, "text"), up::FieldString(L, item, "help")});
    });
    const int defaultIndex = lua_type(L, 5) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, 5)) : 0;
    return PushText(L, WithLobbyOptionWritten(up::ArgString(L, 1, "ed_adaptive_option_write"), up::ArgString(L, 2, "ed_adaptive_option_write"),
                                              StringAt(L, 3), StringAt(L, 4), defaultIndex, values));
  }

  int __cdecl cfunc_OptionRemove(lua_State* const L)
  {
    return PushText(L, WithoutLobbyOption(up::ArgString(L, 1, "ed_adaptive_option_remove"), up::ArgString(L, 2, "ed_adaptive_option_remove")));
  }

  int __cdecl cfunc_NextLevel(lua_State* const L)
  {
    PushValue(L, NextLevelValue(ReadOption(L, 1), StringAt(L, 2)));
    return 1;
  }

  // ---- the setup by what the player picks ------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_OptionsOf(lua_State* const L)
  {
    const std::string script = up::ArgString(L, 1, "ed_adaptive_options_of");
    const std::string optionsText = StringAt(L, 2);
    AdaptiveTables tables;
    if (lua_type(L, 3) == LUA_TSTRING) {
      std::string error;
      if (!ParseAdaptiveTables(StringAt(L, 3), tables, error)) {
        tables = AdaptiveTables();
      }
    } else {
      tables = ReadTables(L, 3);
    }
    const std::vector<LobbyOptionInfo> lobby = ParseLobbyOptions(optionsText);
    const bool adaptiveScript = !AdaptiveTablesPath(script).empty();
    const std::vector<AdaptiveOption> options = AdaptiveOptionsOf(script, lobby, tables, adaptiveScript);
    lua_newtable(L);
    for (std::size_t i = 0; i < options.size(); ++i) {
      const AdaptiveOption& option = options[i];
      lua_newtable(L);
      up::SetString(L, "kind", KindName(option.kind));
      up::SetString(L, "key", option.key);
      up::SetNumber(L, "lobby", option.lobby + 1);
      up::SetString(L, "table", option.table);
      up::SetString(L, "tableLabel", option.tableLabel);
      lua_pushstring(L, "loop");
      PushLoop(L, option.loop);
      lua_settable(L, -3);
      up::SetString(L, "title", option.title);
      up::SetBool(L, "scriptReads", option.scriptReads);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
  }

  int __cdecl cfunc_TableGroup(lua_State* const L)
  {
    lua_pushnumber(L, TableGroupOfChoice(ReadValue(L, 1)) + 1);
    return 1;
  }

  int __cdecl cfunc_LevelGroup(lua_State* const L)
  {
    return PushText(L, LoopGroupOfChoice(ReadLoop(L, 1), ReadValue(L, 2)));
  }

  int __cdecl cfunc_GroupsMade(lua_State* const L)
  {
    const std::string script = up::ArgString(L, 1, "ed_adaptive_groups_made");
    const std::vector<LobbyOptionInfo> lobby = ParseLobbyOptions(StringAt(L, 2));
    const std::string key = up::ArgString(L, 3, "ed_adaptive_groups_made");
    const int valueIndex = static_cast<int>(luaL_checknumber(L, 4)) - 1;
    const std::vector<MadeGroup> made = GroupsMadeByChoice(script, lobby, key, valueIndex);
    lua_newtable(L);
    for (std::size_t i = 0; i < made.size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "group", made[i].group);
      up::SetBool(L, "wreck", made[i].wreck);
      up::SetBool(L, "unit", made[i].unit);
      up::SetBool(L, "maybe", made[i].maybe);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
  }

  int __cdecl cfunc_Loops(lua_State* const L)
  {
    const std::vector<ScriptGroupLoop> loops = ScriptGroupLoops(up::ArgString(L, 1, "ed_adaptive_loops"));
    lua_newtable(L);
    for (std::size_t i = 0; i < loops.size(); ++i) {
      PushLoop(L, loops[i]);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
  }

  int __cdecl cfunc_Creations(lua_State* const L)
  {
    const std::vector<ScriptGroupCreation> creations = ScriptGroupCreations(up::ArgString(L, 1, "ed_adaptive_creations"));
    lua_newtable(L);
    for (std::size_t i = 0; i < creations.size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "army", creations[i].army);
      up::SetString(L, "name", creations[i].name);
      up::SetBool(L, "prefix", creations[i].prefix);
      up::SetBool(L, "wreck", creations[i].wreck);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
  }

  // ---- units --------------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_Rule(lua_State* const L)
  {
    const std::string script = up::ArgString(L, 1, "ed_adaptive_rule");
    const std::vector<LobbyOptionInfo> options = ParseLobbyOptions(StringAt(L, 2));
    PushRule(L, LobbyRuleOfGroup(script, options, up::ArgString(L, 3, "ed_adaptive_rule")), options);
    return 1;
  }

  int __cdecl cfunc_SpawnTargets(lua_State* const L)
  {
    const std::string script = up::ArgString(L, 1, "ed_adaptive_spawn_targets");
    const std::string optionsText = StringAt(L, 2);
    const std::string army = up::ArgString(L, 3, "ed_adaptive_spawn_targets");
    std::vector<std::vector<std::string>> paths;
    EachElement(L, 4, [&](const int item) { paths.push_back(StringListAt(L, item)); });
    const std::vector<SpawnTarget> targets = SpawnTargets(script, optionsText, army, paths);
    const std::vector<LobbyOptionInfo> current = ParseLobbyOptions(optionsText);
    lua_newtable(L);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      const SpawnTarget& target = targets[i];
      lua_newtable(L);
      up::SetStringList(L, "path", target.path);
      up::SetBool(L, "exists", target.exists);
      lua_pushstring(L, "rule");
      PushRule(L, target.rule, target.optionsWith.empty() ? current : ParseLobbyOptions(target.optionsWith));
      lua_settable(L, -3);
      up::SetString(L, "when", target.when);
      up::SetBool(L, "wreck", target.wreck);
      up::SetBool(L, "unit", target.unit);
      up::SetString(L, "optionsWith", target.optionsWith);
      up::SetString(L, "optionsChange", target.optionsChange);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
  }

  int __cdecl cfunc_Fate(lua_State* const L)
  {
    const std::vector<std::string> path = StringListAt(L, 1);
    const UnitFate fate = FateOfUnit(path, lua_toboolean(L, 2) != 0, StringAt(L, 3));
    lua_newtable(L);
    up::SetString(L, "kind", FateName(fate.kind));
    up::SetString(L, "group", fate.group);
    up::SetString(L, "text", fate.text);
    return 1;
  }

  // ---- wrecks -------------------------------------------------------------------------------------------------------------------------------------

  /** A game file's text (a folder or an archive; the path as MapLoad resolves it), false when it is not there. */
  bool ReadGameText(const std::string& virtualPath, std::string& out)
  {
    out.clear();
    std::string path = virtualPath;
    if (moho::FWaitHandleSet* const files = moho::FILE_GetWaitHandleSet(); files != nullptr && files->mHandle != nullptr) {
      fa::string resolved;
      (void)files->mHandle->FindFile(&resolved, virtualPath.c_str(), nullptr);
      if (!resolved.empty()) {
        path = std::string(resolved.c_str());
      }
    }
    try {
      fa::auto_ptr<gpg::Stream> stream = moho::DISK_OpenFileRead(path.c_str());
      if (stream.get() == nullptr) {
        return false;
      }
      char buffer[65536];
      for (std::size_t got = stream->Read(buffer, sizeof(buffer)); got != 0; got = stream->Read(buffer, sizeof(buffer))) {
        out.append(buffer, got);
      }
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  struct BlueprintFacts
  {
    bool found = false;
    double mass = 0.0;
    double energy = 0.0;
    std::string text;
  };

  /**
   * A blueprint's build costs and `.bp` text, read once per id: from the kept rules when they are built (upstream's editor builds them), else from the Lua state's `__blueprints`
   * (the editor session's blueprint table: Economy.BuildCostMass / BuildCostEnergy and Source, the file the blueprint came from). Null when neither knows the unit (and nothing is cached).
   */
  const BlueprintFacts* FactsOf(lua_State* const L, const std::string& id)
  {
    static std::map<std::string, BlueprintFacts> cache;
    if (const auto found = cache.find(id); found != cache.end()) {
      return &found->second;
    }
    BlueprintFacts facts;
    bool known = false;
    if (moho::RRuleGameRules* const rules = moho::RULE_PeekKept(); rules != nullptr) {
      moho::RResId resId{};
      resId.name.assign(id.c_str());
      if (const moho::RUnitBlueprint* const blueprint = rules->GetUnitBlueprint(resId); blueprint != nullptr) {
        facts.mass = blueprint->Economy.BuildCostMass;
        facts.energy = blueprint->Economy.BuildCostEnergy;
        facts.found = ReadGameText(blueprint->mSource.c_str(), facts.text);
        known = true;
      }
    }
    if (!known) {
      lua_pushstring(L, "__blueprints");
      lua_gettable(L, LUA_GLOBALSINDEX);
      if (lua_type(L, -1) == LUA_TTABLE) {
        const std::string lowerText = [&] { std::string t = id; std::transform(t.begin(), t.end(), t.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); }); return t; }();
        for (const std::string* const name : {&id, &lowerText}) {
          if (known) {
            break;
          }
          if (up::PushField(L, -1, name->c_str()) == LUA_TTABLE) {
            const std::string source = up::FieldString(L, -1, "Source");
            if (up::PushField(L, -1, "Economy") == LUA_TTABLE) {
              facts.mass = up::FieldNumber(L, -1, "BuildCostMass");
              facts.energy = up::FieldNumber(L, -1, "BuildCostEnergy");
            }
            lua_settop(L, -2); // Economy
            facts.found = !source.empty() && ReadGameText(source, facts.text);
            known = facts.found;
          }
          lua_settop(L, -2); // the blueprint
        }
      }
      lua_settop(L, -2);
    }
    if (!known && moho::RULE_PeekKept() == nullptr) {
      return nullptr;
    }
    return &cache.emplace(id, std::move(facts)).first->second;
  }

  int __cdecl cfunc_WreckValue(lua_State* const L)
  {
    const std::string id = up::ArgString(L, 1, "ed_wreck_value");
    const bool underwater = lua_toboolean(L, 2) != 0;
    const BlueprintFacts* const facts = FactsOf(L, id);
    if (facts == nullptr) {
      return up::Fail(L, "no blueprint " + id + " (the rules are not built and the Lua state has none)");
    }
    lua_newtable(L);
    up::SetBool(L, "known", facts->found);
    if (facts->found) {
      const WreckValue wreck = WreckOfUnit(facts->text, facts->mass, facts->energy, underwater);
      up::SetBool(L, "exists", wreck.exists);
      up::SetNumber(L, "mass", wreck.mass);
      up::SetNumber(L, "energy", wreck.energy);
      up::SetNumber(L, "buildMass", facts->mass);
      up::SetNumber(L, "buildEnergy", facts->energy);
    }
    return 1;
  }

  int __cdecl cfunc_WreckValueOf(lua_State* const L)
  {
    const WreckValue wreck = WreckOfUnit(up::ArgString(L, 1, "ed_wreck_value_of"), luaL_checknumber(L, 2), luaL_checknumber(L, 3), lua_toboolean(L, 4) != 0);
    lua_newtable(L);
    up::SetBool(L, "exists", wreck.exists);
    up::SetNumber(L, "mass", wreck.mass);
    up::SetNumber(L, "energy", wreck.energy);
    return 1;
  }

  const up::Natives gEdUpAdaptiveNatives{{
    {"ed_adaptive_known_tables", &cfunc_KnownTables, "ed_adaptive_known_tables() -> { { name, label, hydro, optionKey }, ... }"},
    {"ed_adaptive_tables_parse", &cfunc_TablesParse, "ed_adaptive_tables_parse(text) -> tables | nil, message"},
    {"ed_adaptive_tables_write", &cfunc_TablesWrite, "ed_adaptive_tables_write(text, tables) -> text"},
    {"ed_adaptive_tables_path", &cfunc_TablesPath, "ed_adaptive_tables_path(script) -> vfs path | ''"},
    {"ed_adaptive_scenario", &cfunc_Scenario, "ed_adaptive_scenario(scenarioText) -> text with AdaptiveMap = true"},
    {"ed_adaptive_entry", &cfunc_Entry, "ed_adaptive_entry(markerName, hydro) -> entry | ''"},
    {"ed_adaptive_marker", &cfunc_Marker, "ed_adaptive_marker(entry, hydro) -> markerName"},
    {"ed_adaptive_option_key", &cfunc_OptionKey, "ed_adaptive_option_key(tableName) -> lobby option key"},
    {"ed_adaptive_options_parse", &cfunc_OptionsParse, "ed_adaptive_options_parse(text) -> { option, ... }"},
    {"ed_adaptive_option_find", &cfunc_OptionFind, "ed_adaptive_option_find(text, key) -> { found, values, defaultKey }"},
    {"ed_adaptive_option_put", &cfunc_OptionPut, "ed_adaptive_option_put(text, option) -> text"},
    {"ed_adaptive_option_for", &cfunc_OptionFor, "ed_adaptive_option_for(text, key, label, hydro, groupSizes) -> text"},
    {"ed_adaptive_option_values", &cfunc_OptionValues, "ed_adaptive_option_values(text, key, label, help, values) -> text"},
    {"ed_adaptive_option_write", &cfunc_OptionWrite, "ed_adaptive_option_write(text, key, label, help, defaultIndex, values) -> text"},
    {"ed_adaptive_option_remove", &cfunc_OptionRemove, "ed_adaptive_option_remove(text, key) -> text"},
    {"ed_adaptive_next_level", &cfunc_NextLevel, "ed_adaptive_next_level(option, text) -> value"},
    {"ed_adaptive_options_of", &cfunc_OptionsOf, "ed_adaptive_options_of(script, optionsText, tables) -> { option, ... }"},
    {"ed_adaptive_table_group", &cfunc_TableGroup, "ed_adaptive_table_group(value) -> group (1-based, 0 none)"},
    {"ed_adaptive_level_group", &cfunc_LevelGroup, "ed_adaptive_level_group(loop, value) -> group name | ''"},
    {"ed_adaptive_groups_made", &cfunc_GroupsMade, "ed_adaptive_groups_made(script, optionsText, key, valueIndex) -> { { group, wreck, unit, maybe } }"},
    {"ed_adaptive_loops", &cfunc_Loops, "ed_adaptive_loops(script) -> { loop, ... }"},
    {"ed_adaptive_creations", &cfunc_Creations, "ed_adaptive_creations(script) -> { { army, name, prefix, wreck } }"},
    {"ed_adaptive_rule", &cfunc_Rule, "ed_adaptive_rule(script, optionsText, group) -> rule"},
    {"ed_adaptive_spawn_targets", &cfunc_SpawnTargets, "ed_adaptive_spawn_targets(script, optionsText, army, groupPaths) -> { target, ... }"},
    {"ed_adaptive_fate", &cfunc_Fate, "ed_adaptive_fate(path, civilian, script) -> { kind, group, text }"},
    {"ed_wreck_value", &cfunc_WreckValue, "ed_wreck_value(bpId, underwater) -> { known, exists, mass, energy, buildMass, buildEnergy } | nil, message"},
    {"ed_wreck_value_of", &cfunc_WreckValueOf, "ed_wreck_value_of(bpText, buildMass, buildEnergy, underwater) -> { exists, mass, energy }"},
  }};
} // namespace
