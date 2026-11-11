// EDITOR: the "up" natives of the particle editor (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP8): upstream's emitter / beam / trail
// blueprint model, its curve sampling, the browser's grouping and the effect templates as node graphs, bound to Lua over EdUpCommon.h. The logic is
// upstream's (src/editor/doc/EdEmitter, src/editor/graph/EdEffectGraph, EdNodeGraph, in faf_editor_logic); this unit is the table <-> struct glue and nothing
// else. The engine side of the preview (particles, the offscreen view) is EdFxView.cpp.
//
// An emitter is a table (a field a table lacks keeps retail's starting value, see ed_fx_emitter_new):
//   { blueprintId, lifetime, repeatTime, textureFrameCount, blendMode, lodCutoff, textureStripCount, sortOrder, lowFidelity, medFidelity, highFidelity,
//     texture, rampTexture, flags = { [FlagName] = bool }, curves = { [1..21] = curve, [CurveName] = curve }, extra = { { key, text }, ... } }
//   curve = { name, xRange, keys = { { x, y, z }, ... } }     x the time (0..xRange), y the value, z the random spread around it
//
//   ed_fx_info()                          -> { curves = { { name, group, caption, viewMin, viewMax, initialValue, initialSpread }, ... in the file's order },
//                                              flags = { names }, blendModes = { names }, factions = { names }, kinds = { names } }
//   ed_fx_emitter_new()                   -> emitter      retail's editor's new emitter
//   ed_fx_emitter_parse(text)             -> emitter | nil, message       (message names the line)
//   ed_fx_emitter_write(emitter[, original]) -> text, patched     as retail's emitter editor writes it; with `original` (the file's text as it was opened) the file's own text is edited
//                                         instead: only the keys that changed are replaced, comments, layout and line endings stay (patched = false: the plain write was used)
//   ed_fx_emitter_roundtrip(text)         -> { ok, identical, preserved, error, written }     parse + write + parse + compare: what the retail audit runs over every file.
//                                         ok: reads back as the same emitter (upstream's Same); identical: the writer's text equals the file's (CRs aside); preserved: writing the
//                                         unedited model back into the file's text gives the file's bytes
//   ed_fx_emitter_type_tag(emitter)       -> string       the particle technique's type tag (TRamp, TRampFlat, TRampAlign, ...)
//   ed_fx_curve_samples(keys, xRange, n)  -> { mid = {n y}, lo = {n}, hi = {n} }     SEfxCurve::GetValue at n points from 0 to xRange (lo / hi: the spread's edges)
//   ed_fx_flat_info(kind)                 -> { { key, kind, initial, color = {r,g,b,a}, help, texture }, ... }       kind 'BeamBlueprint' | 'TrailEmitterBlueprint'
//   ed_fx_flat_parse(text)                -> flat | nil, message         flat = { kind, fields = { { key, kind = 'number'|'bool'|'text'|'color'|'raw', number, flag, text, color = {r,g,b,a} }, ... } }
//   ed_fx_flat_write(flat[, original])    -> text, patched          (as ed_fx_emitter_write)
//   ed_fx_flat_roundtrip(text)            -> { ok, identical, preserved, error }
//   ed_fx_effect_class(path)              -> factionIndex, kindIndex     the browser's grouping of a game file name (0 = "all", index into ed_fx_info().factions / kinds, 0-based)
//   ed_fx_list_files([roots])             -> { { path, name, faction, kind }, ... }     every /effects/**/*.bp of the game's file system and of the extra virtual folders `roots` names, sorted by name
//   ed_fx_templates(luaText)              -> { { name, paths = { ... } }, ... }          the templates of an EffectTemplates.lua that are plain lists of emitter paths
//   ed_fx_script_effects(script, on)      -> text         the map's _script.lua with the editor's MapEditorEffects function and its call in OnPopulate (on) or both taken out (off)
//   ed_fx_graph_node_types()              -> { { name, title, category, help, output, inputs = { { name, type } }, outputs = { ... },
//                                                params = { { name, kind, min, max, choices = {..}, initial = {a,b,c,d}, initialText, help } } }, ... }
//   ed_fx_graph_build(name, { { name, emitter }, ... }) -> graph    the Effect output fed by Emitter / Curve / Constant / Texture nodes
//   ed_fx_graph_emitters(graph)           -> { { name, emitter }, ... } | nil, message      the emitters the graph makes, in the output's pin order
//        graph = { nodes = { { id, type, x, y, params = { { v = {a,b,c,d}, text }, ... in the type's parameter order } }, ... }, links = { { fromNode, fromPin, toNode, toPin }, ... } }
//        (pin and parameter numbers are 0-based as upstream's, ids are the graph's own)

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "editor/doc/EdEmitter.h"
#include "editor/doc/EdLuaText.h"
#include "editor/doc/EdSaveFile.h"
#include "editor/graph/EdEffectGraph.h"
#include "editor/graph/EdNodeGraph.h"
#include "legacy/containers/String.h"
#include "legacy/containers/Vector.h"
#include "moho/editor/EdUpCommon.h"
#include "moho/editor/EdUpFx.h"
#include "moho/misc/CVirtualFileSystem.h"
#include "moho/misc/FileWaitHandleSet.h"

namespace
{
  namespace up = moho::editor::up;
  using namespace faf::editor;

  // ---- small helpers ------------------------------------------------------------------------------------------------------------------------------

  /** Calls `fn(elementIndex)` for each table element of the array t[key] (the element is on top of the stack during the call). */
  template <class Fn>
  void EachElement(lua_State* const L, const int index, const char* const key, Fn&& fn)
  {
    if (up::PushField(L, index, key) == LUA_TTABLE) {
      const int list = lua_gettop(L);
      for (int i = 1;; ++i) {
        lua_rawgeti(L, list, i);
        if (lua_type(L, -1) == LUA_TNIL) {
          lua_settop(L, -2);
          break;
        }
        if (lua_type(L, -1) == LUA_TTABLE) {
          fn(lua_gettop(L));
        }
        lua_settop(L, -2);
      }
    }
    lua_settop(L, -2);
  }

  /** Same for an array held at stack index `index` itself. */
  template <class Fn>
  void EachOf(lua_State* const L, const int index, Fn&& fn)
  {
    const int list = index < 0 ? lua_gettop(L) + index + 1 : index;
    if (lua_type(L, list) != LUA_TTABLE) {
      return;
    }
    for (int i = 1;; ++i) {
      lua_rawgeti(L, list, i);
      if (lua_type(L, -1) == LUA_TNIL) {
        lua_settop(L, -2);
        break;
      }
      if (lua_type(L, -1) == LUA_TTABLE) {
        fn(lua_gettop(L));
      }
      lua_settop(L, -2);
    }
  }

  [[nodiscard]] float FieldFloat(lua_State* const L, const int index, const char* const key, const float fallback)
  {
    return static_cast<float>(up::FieldNumber(L, index, key, static_cast<double>(fallback)));
  }

  /** t[key] when it is a table (left on the stack: pop it with lua_settop(L, -2)), else pushes nil and returns false. */
  [[nodiscard]] bool PushTable(lua_State* const L, const int index, const char* const key)
  {
    if (up::PushField(L, index, key) == LUA_TTABLE) {
      return true;
    }
    return false;
  }

  void SetFloatArray(lua_State* const L, const char* const key, const std::array<float, 4>& values)
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (int i = 0; i < 4; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(values[static_cast<std::size_t>(i)]));
      lua_rawseti(L, -2, i + 1);
    }
    lua_settable(L, -3);
  }

  [[nodiscard]] std::array<float, 4> ReadFloatArray(lua_State* const L, const int index, const char* const key, const std::array<float, 4>& fallback)
  {
    std::array<float, 4> out = fallback;
    if (up::PushField(L, index, key) == LUA_TTABLE) {
      const int list = lua_gettop(L);
      for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, list, i + 1);
        if (lua_type(L, -1) == LUA_TNUMBER) {
          out[static_cast<std::size_t>(i)] = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_settop(L, -2);
      }
    }
    lua_settop(L, -2);
    return out;
  }

  // ---- emitter <-> table --------------------------------------------------------------------------------------------------------------------------

  void PushCurve(lua_State* const L, const char* const name, const EmitterCurve& curve)
  {
    lua_newtable(L);
    up::SetString(L, "name", name);
    up::SetNumber(L, "xRange", curve.xRange);
    lua_pushstring(L, "keys");
    lua_newtable(L);
    for (std::size_t i = 0; i < curve.keys.size(); ++i) {
      lua_newtable(L);
      up::SetNumber(L, "x", curve.keys[i].x);
      up::SetNumber(L, "y", curve.keys[i].y);
      up::SetNumber(L, "z", curve.keys[i].z);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  void PushEmitter(lua_State* const L, const EmitterBlueprint& e)
  {
    lua_newtable(L);
    up::SetString(L, "blueprintId", e.blueprintId);
    up::SetNumber(L, "lifetime", e.lifetime);
    up::SetNumber(L, "repeatTime", e.repeatTime);
    up::SetNumber(L, "textureFrameCount", e.textureFrameCount);
    up::SetNumber(L, "blendMode", e.blendMode);
    up::SetNumber(L, "lodCutoff", e.lodCutoff);
    up::SetNumber(L, "textureStripCount", e.textureStripCount);
    up::SetNumber(L, "sortOrder", e.sortOrder);
    up::SetBool(L, "lowFidelity", e.lowFidelity);
    up::SetBool(L, "medFidelity", e.medFidelity);
    up::SetBool(L, "highFidelity", e.highFidelity);
    up::SetString(L, "texture", e.texture);
    up::SetString(L, "rampTexture", e.rampTexture);

    lua_pushstring(L, "flags");
    lua_newtable(L);
    for (int i = 0; i < kEmitterFlagCount; ++i) {
      up::SetBool(L, FlagName(i), e.flags[static_cast<std::size_t>(i)]);
    }
    lua_settable(L, -3);

    lua_pushstring(L, "curves");
    lua_newtable(L);
    for (int i = 0; i < kEmitterCurveCount; ++i) {
      const char* const name = CurveInfo(i).name;
      PushCurve(L, name, e.curves[static_cast<std::size_t>(i)]);
      lua_pushstring(L, name);
      lua_pushvalue(L, -2);
      lua_settable(L, -4); // curves[name] = the same table
      lua_rawseti(L, -2, i + 1);
    }
    lua_settable(L, -3);

    lua_pushstring(L, "extra");
    lua_newtable(L);
    for (std::size_t i = 0; i < e.extra.size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "key", e.extra[i].first);
      up::SetString(L, "text", e.extra[i].second);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  /** The emitter a table describes; a field it lacks keeps retail's starting value. */
  [[nodiscard]] EmitterBlueprint ReadEmitter(lua_State* const L, const int index)
  {
    EmitterBlueprint e = NewEmitter();
    e.blueprintId = up::FieldString(L, index, "blueprintId", e.blueprintId);
    e.lifetime = FieldFloat(L, index, "lifetime", e.lifetime);
    e.repeatTime = FieldFloat(L, index, "repeatTime", e.repeatTime);
    e.textureFrameCount = FieldFloat(L, index, "textureFrameCount", e.textureFrameCount);
    e.blendMode = FieldFloat(L, index, "blendMode", e.blendMode);
    e.lodCutoff = FieldFloat(L, index, "lodCutoff", e.lodCutoff);
    e.textureStripCount = FieldFloat(L, index, "textureStripCount", e.textureStripCount);
    e.sortOrder = FieldFloat(L, index, "sortOrder", e.sortOrder);
    e.lowFidelity = up::FieldBool(L, index, "lowFidelity", e.lowFidelity);
    e.medFidelity = up::FieldBool(L, index, "medFidelity", e.medFidelity);
    e.highFidelity = up::FieldBool(L, index, "highFidelity", e.highFidelity);
    e.texture = up::FieldString(L, index, "texture", e.texture);
    e.rampTexture = up::FieldString(L, index, "rampTexture", e.rampTexture);
    if (PushTable(L, index, "flags")) {
      const int flags = lua_gettop(L);
      for (int i = 0; i < kEmitterFlagCount; ++i) {
        e.flags[static_cast<std::size_t>(i)] = up::FieldBool(L, flags, FlagName(i), e.flags[static_cast<std::size_t>(i)]);
      }
    }
    lua_settop(L, -2);
    if (PushTable(L, index, "curves")) {
      const int curves = lua_gettop(L);
      for (int i = 0; i < kEmitterCurveCount; ++i) {
        // by name first, then by position
        int type = up::PushField(L, curves, CurveInfo(i).name);
        if (type != LUA_TTABLE) {
          lua_settop(L, -2);
          lua_rawgeti(L, curves, i + 1);
          type = lua_type(L, -1);
        }
        if (type == LUA_TTABLE) {
          const int curve = lua_gettop(L);
          EmitterCurve& c = e.curves[static_cast<std::size_t>(i)];
          c.xRange = FieldFloat(L, curve, "xRange", c.xRange);
          if (PushTable(L, curve, "keys")) {
            c.keys.clear();
            EachOf(L, -1, [&](const int key) {
              EmitterKey k;
              k.x = FieldFloat(L, key, "x", 0.0f);
              k.y = FieldFloat(L, key, "y", 0.0f);
              k.z = FieldFloat(L, key, "z", 0.0f);
              c.keys.push_back(k);
            });
          }
          lua_settop(L, -2);
        }
        lua_settop(L, -2);
      }
    }
    lua_settop(L, -2);
    e.extra.clear();
    EachElement(L, index, "extra", [&](const int item) {
      e.extra.emplace_back(up::FieldString(L, item, "key"), up::FieldString(L, item, "text"));
    });
    return e;
  }

  // ---- info ---------------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_Info(lua_State* const L)
  {
    lua_newtable(L);
    lua_pushstring(L, "curves");
    lua_newtable(L);
    for (int i = 0; i < kEmitterCurveCount; ++i) {
      const EmitterCurveInfo& info = CurveInfo(i);
      lua_newtable(L);
      up::SetString(L, "name", info.name);
      up::SetString(L, "group", info.group);
      up::SetString(L, "caption", info.caption);
      up::SetNumber(L, "viewMin", info.viewMin);
      up::SetNumber(L, "viewMax", info.viewMax);
      up::SetNumber(L, "initialValue", info.initialValue);
      up::SetNumber(L, "initialSpread", info.initialSpread);
      lua_rawseti(L, -2, i + 1);
    }
    lua_settable(L, -3);
    std::vector<std::string> list;
    for (int i = 0; i < kEmitterFlagCount; ++i) {
      list.emplace_back(FlagName(i));
    }
    up::SetStringList(L, "flags", list);
    list.clear();
    for (const char* const mode : kEmitterBlendModes) {
      list.emplace_back(mode);
    }
    up::SetStringList(L, "blendModes", list);
    list.assign(std::begin(kEffectFactions), std::end(kEffectFactions));
    up::SetStringList(L, "factions", list);
    list.assign(std::begin(kEffectKinds), std::end(kEffectKinds));
    up::SetStringList(L, "kinds", list);
    return 1;
  }

  // ---- emitters -----------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_EmitterNew(lua_State* const L)
  {
    PushEmitter(L, NewEmitter());
    return 1;
  }

  // ---- writing back into the file's own text ------------------------------------------------------------------------------------------------------
  //
  // WriteEmitter / WriteFlatBlueprint write the layout of retail's own editor. A file made by an older version of it, or by hand, differs in layout (a key missing, another order,
  // 'x' quotes, another number of decimals: 550 of the 2,437 emitters of the game's effects.scd), and a plain write would turn a file the user only opened into a different file.
  // So the file's text is edited instead of replaced: each key whose value the writer would write differently for the edited model than for the model read from the text is
  // replaced by the writer's text of that value (a key the file lacks is added before the closing brace); everything else, comments, layout, line endings, stays as it was.
  // The result is read back and must equal the edited model, else the plain write is used (reported as `patched = false`).

  [[nodiscard]] std::string WithoutCr(std::string text)
  {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
  }

  /**
   * The text with its `# comment` lines blanked (same length, so positions stay): the game's Lua takes `#` for a line comment, EdLuaText only knows `--`. Strings are left alone
   * (a quote or a long bracket opens one, `--` comments are skipped whole).
   */
  [[nodiscard]] std::string BlankHashComments(std::string text)
  {
    constexpr char kQuote = '\'';
    constexpr char kNewline = '\n';
    constexpr char kReturn = '\r';
    constexpr char kBackslash = static_cast<char>(92);
    std::size_t i = 0;
    while (i < text.size()) {
      const char c = text[i];
      if (c == kQuote || c == '"') {
        ++i;
        while (i < text.size() && text[i] != c && text[i] != kNewline) {
          i += text[i] == kBackslash ? 2 : 1;
        }
        ++i;
      } else if (c == '[' && i + 1 < text.size() && text[i + 1] == '[') {
        const std::size_t end = text.find("]]", i + 2);
        i = end == std::string::npos ? text.size() : end + 2;
      } else if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
        const std::size_t end = text.find(kNewline, i);
        i = end == std::string::npos ? text.size() : end;
      } else if (c == '#') {
        while (i < text.size() && text[i] != kNewline) {
          if (text[i] != kReturn) {
            text[i] = ' ';
          }
          ++i;
        }
      } else {
        ++i;
      }
    }
    return text;
  }

  /** The table of a blueprint file (`EmitterBlueprint { ... }`): its fields with the positions of their values. */
  [[nodiscard]] bool ReadBlueprintTable(const std::string& raw, LuaText& table)
  {
    const std::string text = BlankHashComments(raw);
    const std::size_t name = text.find("Blueprint");
    if (name == std::string::npos) {
      return false;
    }
    const std::size_t brace = text.find('{', name);
    std::string error;
    return brace != std::string::npos && ReadLuaValue(text, brace, table, error) && table.isTable;
  }

  [[nodiscard]] std::string ValueText(const std::string& text, const LuaText& value)
  {
    return text.substr(value.start, value.end - value.start);
  }

  /**
   * `original` with the keys the edit changed replaced. `w0` is the writer's text of the model read from `original`, `w1` its text of the edited model.
   * Empty when the files' tables cannot be read (the caller then writes `w1`).
   */
  [[nodiscard]] std::string PatchBlueprintText(const std::string& original, const std::string& w0, const std::string& w1)
  {
    LuaText orig, base, edit;
    if (!ReadBlueprintTable(original, orig) || !ReadBlueprintTable(w0, base) || !ReadBlueprintTable(w1, edit)) {
      return {};
    }
    const bool crlf = original.find("\r\n") != std::string::npos;
    const auto convert = [&](const std::string& value) {
      if (!crlf) {
        return value;
      }
      std::string out;
      for (const char c : value) {
        if (c == '\n') {
          out += '\r';
        }
        out += c;
      }
      return out;
    };
    struct Edit
    {
      std::size_t from;
      std::size_t to;
      std::string text;
    };
    std::vector<Edit> edits;
    std::string added;
    for (const auto& [key, value] : edit.fields) {
      const std::string now = ValueText(w1, value);
      const LuaText* const was = base.Field(key);
      if (was != nullptr && ValueText(w0, *was) == now) {
        continue; // the writer says the same of it: it stays as the file has it
      }
      if (const LuaText* const there = orig.Field(key); there != nullptr) {
        edits.push_back({there->start, there->end, convert(now)});
      } else {
        added += "\t" + key + " = " + convert(now) + (crlf ? ",\r\n" : ",\n");
      }
    }
    std::string out = original;
    if (!added.empty()) {
      // before the closing brace's line (or at the brace, on a line of its own)
      const std::size_t close = orig.end - 1;
      std::size_t lineStart = original.rfind('\n', close);
      lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
      bool onlySpace = true;
      for (std::size_t i = lineStart; i < close; ++i) {
        onlySpace = onlySpace && std::isspace(static_cast<unsigned char>(original[i])) != 0;
      }
      const std::size_t at = onlySpace ? lineStart : close;
      if (!onlySpace) {
        added += crlf ? "\r\n" : "\n";
      }
      // the last field needs its comma
      std::size_t last = at;
      while (last > 0 && std::isspace(static_cast<unsigned char>(original[last - 1])) != 0) {
        --last;
      }
      if (last > 0 && original[last - 1] != ',' && original[last - 1] != '{') {
        edits.push_back({last, last, ","});
      }
      edits.push_back({at, at, added});
    }
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.from > b.from; });
    for (const Edit& e : edits) {
      out.replace(e.from, e.to - e.from, e.text);
    }
    return out;
  }

  /** The upstream tests' notion of the same emitter: settings and keys as the file keeps its numbers (two decimals, keys three). */
  [[nodiscard]] bool SameEmitter(const EmitterBlueprint& a, const EmitterBlueprint& b)
  {
    if (a.blueprintId != b.blueprintId || a.texture != b.texture || a.rampTexture != b.rampTexture || a.flags != b.flags || a.lowFidelity != b.lowFidelity ||
        a.medFidelity != b.medFidelity || a.highFidelity != b.highFidelity || a.extra != b.extra) {
      return false;
    }
    const auto within = [](const float x, const float y, const float step) { return std::fabs(x - y) <= step * 0.51f; };
    const float values[2][7] = {{a.lifetime, a.repeatTime, a.textureFrameCount, a.blendMode, a.lodCutoff, a.textureStripCount, a.sortOrder},
                                {b.lifetime, b.repeatTime, b.textureFrameCount, b.blendMode, b.lodCutoff, b.textureStripCount, b.sortOrder}};
    for (int i = 0; i < 7; ++i) {
      if (!within(values[0][i], values[1][i], 0.01f)) {
        return false;
      }
    }
    for (int c = 0; c < kEmitterCurveCount; ++c) {
      const EmitterCurve& x = a.curves[static_cast<std::size_t>(c)];
      const EmitterCurve& y = b.curves[static_cast<std::size_t>(c)];
      if (!within(x.xRange, y.xRange, 0.01f) || x.keys.size() != y.keys.size()) {
        return false;
      }
      for (std::size_t k = 0; k < x.keys.size(); ++k) {
        if (!within(x.keys[k].x, y.keys[k].x, 0.001f) || !within(x.keys[k].y, y.keys[k].y, 0.001f) || !within(x.keys[k].z, y.keys[k].z, 0.001f)) {
          return false;
        }
      }
    }
    return true;
  }

  [[nodiscard]] bool SameFlat(const FlatBlueprint& a, const FlatBlueprint& b)
  {
    if (a.kind != b.kind || a.fields.size() != b.fields.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.fields.size(); ++i) {
      const FlatValue& x = a.fields[i].second;
      const FlatValue& y = b.fields[i].second;
      if (a.fields[i].first != b.fields[i].first || x.kind != y.kind || std::fabs(x.number - y.number) > 0.0051f || x.flag != y.flag || x.text != y.text) {
        return false;
      }
      for (std::size_t c = 0; c < 4; ++c) {
        if (std::fabs(x.color[c] - y.color[c]) > 0.0051f) {
          return false;
        }
      }
    }
    return true;
  }

  /** The text of the edited emitter written into `original` (when there is one); `patched` says whether the file's own text was kept. */
  [[nodiscard]] std::string WriteEmitterInto(const EmitterBlueprint& edited, const std::string& original, bool& patched)
  {
    patched = false;
    const std::string plain = WriteEmitter(edited);
    if (original.empty()) {
      return plain;
    }
    EmitterBlueprint base;
    std::string error;
    if (!ParseEmitter(WithoutCr(original), base, error)) {
      return plain;
    }
    const std::string text = PatchBlueprintText(original, WriteEmitter(base), plain);
    EmitterBlueprint check;
    if (text.empty() || !ParseEmitter(WithoutCr(text), check, error) || !SameEmitter(check, edited)) {
      return plain;
    }
    patched = true;
    return text;
  }

  [[nodiscard]] std::string WriteFlatInto(const FlatBlueprint& edited, const std::string& original, bool& patched)
  {
    patched = false;
    const std::string plain = WriteFlatBlueprint(edited);
    if (original.empty()) {
      return plain;
    }
    FlatBlueprint base;
    std::string error;
    if (!ParseFlatBlueprint(WithoutCr(original), base, error)) {
      return plain;
    }
    const std::string text = PatchBlueprintText(original, WriteFlatBlueprint(base), plain);
    FlatBlueprint check;
    if (text.empty() || !ParseFlatBlueprint(WithoutCr(text), check, error) || !SameFlat(check, edited)) {
      return plain;
    }
    patched = true;
    return text;
  }

  int __cdecl cfunc_EmitterParse(lua_State* const L)
  {
    EmitterBlueprint emitter;
    std::string error;
    if (!ParseEmitter(WithoutCr(up::ArgString(L, 1, "ed_fx_emitter_parse")), emitter, error)) {
      return up::Fail(L, error);
    }
    PushEmitter(L, emitter);
    return 1;
  }

  int __cdecl cfunc_EmitterWrite(lua_State* const L)
  {
    bool patched = false;
    const std::string original = lua_type(L, 2) == LUA_TSTRING ? std::string(lua_tostring(L, 2), lua_strlen(L, 2)) : std::string();
    const std::string text = WriteEmitterInto(ReadEmitter(L, 1), original, patched);
    lua_pushlstring(L, text.data(), text.size());
    lua_pushboolean(L, patched ? 1 : 0);
    return 2;
  }

  int __cdecl cfunc_EmitterRoundTrip(lua_State* const L)
  {
    const std::string raw = up::ArgString(L, 1, "ed_fx_emitter_roundtrip");
    const std::string text = WithoutCr(raw);
    EmitterBlueprint first;
    std::string error;
    lua_newtable(L);
    if (!ParseEmitter(text, first, error)) {
      up::SetBool(L, "ok", false);
      up::SetString(L, "error", error);
      return 1;
    }
    // the writer's own text, read back (ok: the same emitter within the file's precision; identical: also the same bytes as the file, CRs aside)
    const std::string written = WriteEmitter(first);
    EmitterBlueprint second;
    const bool again = ParseEmitter(written, second, error);
    up::SetBool(L, "ok", again && SameEmitter(first, second));
    up::SetBool(L, "identical", written == text);
    // the file written back through its own text: the bytes of the file, CRs and all
    bool patched = false;
    const std::string kept = WriteEmitterInto(first, raw, patched);
    up::SetBool(L, "preserved", patched && kept == raw);
    if (!again) {
      up::SetString(L, "error", error);
    }
    if (written != text) {
      up::SetString(L, "written", written);
    }
    return 1;
  }

  int __cdecl cfunc_EmitterTypeTag(lua_State* const L)
  {
    const std::string tag = EmitterTypeTag(ReadEmitter(L, 1));
    lua_pushlstring(L, tag.data(), tag.size());
    return 1;
  }

  int __cdecl cfunc_CurveSamples(lua_State* const L)
  {
    EmitterCurve curve;
    curve.xRange = static_cast<float>(lua_type(L, 2) == LUA_TNUMBER ? lua_tonumber(L, 2) : 1.0);
    if (lua_type(L, 1) == LUA_TTABLE) {
      EachOf(L, 1, [&](const int key) {
        EmitterKey k;
        k.x = FieldFloat(L, key, "x", 0.0f);
        k.y = FieldFloat(L, key, "y", 0.0f);
        k.z = FieldFloat(L, key, "z", 0.0f);
        curve.keys.push_back(k);
      });
    }
    const int count = std::clamp(lua_type(L, 3) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, 3)) : 64, 2, 4096);
    std::vector<double> mid(static_cast<std::size_t>(count)), lo(static_cast<std::size_t>(count)), hi(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      const float t = curve.xRange * static_cast<float>(i) / static_cast<float>(count - 1);
      mid[static_cast<std::size_t>(i)] = EmitterCurveValue(curve, t, 0.5f);
      lo[static_cast<std::size_t>(i)] = EmitterCurveValue(curve, t, 0.0f);
      hi[static_cast<std::size_t>(i)] = EmitterCurveValue(curve, t, 1.0f);
    }
    lua_newtable(L);
    up::SetNumberList(L, "mid", mid);
    up::SetNumberList(L, "lo", lo);
    up::SetNumberList(L, "hi", hi);
    return 1;
  }

  // ---- beams and trails ---------------------------------------------------------------------------------------------------------------------------

  const char* KindName(const FlatValue::Kind kind)
  {
    switch (kind) {
    case FlatValue::Kind::Number: return "number";
    case FlatValue::Kind::Bool: return "bool";
    case FlatValue::Kind::Text: return "text";
    case FlatValue::Kind::Color: return "color";
    default: return "raw";
    }
  }

  FlatValue::Kind KindOf(const std::string& name)
  {
    if (name == "number") {
      return FlatValue::Kind::Number;
    }
    if (name == "bool") {
      return FlatValue::Kind::Bool;
    }
    if (name == "text") {
      return FlatValue::Kind::Text;
    }
    if (name == "color") {
      return FlatValue::Kind::Color;
    }
    return FlatValue::Kind::Raw;
  }

  int __cdecl cfunc_FlatInfo(lua_State* const L)
  {
    const std::string kind = up::ArgString(L, 1, "ed_fx_flat_info");
    lua_newtable(L);
    int n = 0;
    for (const FlatField& field : FlatFields(kind)) {
      lua_newtable(L);
      up::SetString(L, "key", field.key);
      up::SetString(L, "kind", KindName(field.kind));
      up::SetNumber(L, "initial", field.initial);
      SetFloatArray(L, "color", field.initialColor);
      up::SetString(L, "help", field.help != nullptr ? field.help : "");
      up::SetBool(L, "texture", field.texture);
      lua_rawseti(L, -2, ++n);
    }
    return 1;
  }

  int __cdecl cfunc_FlatParse(lua_State* const L)
  {
    FlatBlueprint flat;
    std::string error;
    if (!ParseFlatBlueprint(WithoutCr(up::ArgString(L, 1, "ed_fx_flat_parse")), flat, error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    up::SetString(L, "kind", flat.kind);
    lua_pushstring(L, "fields");
    lua_newtable(L);
    int n = 0;
    for (const auto& [key, value] : flat.fields) {
      lua_newtable(L);
      up::SetString(L, "key", key);
      up::SetString(L, "kind", KindName(value.kind));
      up::SetNumber(L, "number", value.number);
      up::SetBool(L, "flag", value.flag);
      up::SetString(L, "text", value.text);
      SetFloatArray(L, "color", value.color);
      lua_rawseti(L, -2, ++n);
    }
    lua_settable(L, -3);
    return 1;
  }

  [[nodiscard]] FlatBlueprint ReadFlat(lua_State* const L, const int index)
  {
    FlatBlueprint flat;
    flat.kind = up::FieldString(L, index, "kind", "BeamBlueprint");
    EachElement(L, index, "fields", [&](const int item) {
      FlatValue value;
      value.kind = KindOf(up::FieldString(L, item, "kind", "number"));
      value.number = FieldFloat(L, item, "number", 0.0f);
      value.flag = up::FieldBool(L, item, "flag", false);
      value.text = up::FieldString(L, item, "text");
      value.color = ReadFloatArray(L, item, "color", {0.0f, 0.0f, 0.0f, 0.0f});
      flat.fields.emplace_back(up::FieldString(L, item, "key"), value);
    });
    return flat;
  }

  int __cdecl cfunc_FlatWrite(lua_State* const L)
  {
    bool patched = false;
    const std::string original = lua_type(L, 2) == LUA_TSTRING ? std::string(lua_tostring(L, 2), lua_strlen(L, 2)) : std::string();
    const std::string text = WriteFlatInto(ReadFlat(L, 1), original, patched);
    lua_pushlstring(L, text.data(), text.size());
    lua_pushboolean(L, patched ? 1 : 0);
    return 2;
  }

  int __cdecl cfunc_FlatRoundTrip(lua_State* const L)
  {
    const std::string raw = up::ArgString(L, 1, "ed_fx_flat_roundtrip");
    FlatBlueprint first;
    std::string error;
    lua_newtable(L);
    if (!ParseFlatBlueprint(WithoutCr(raw), first, error)) {
      up::SetBool(L, "ok", false);
      up::SetString(L, "error", error);
      return 1;
    }
    const std::string written = WriteFlatBlueprint(first);
    FlatBlueprint second;
    up::SetBool(L, "ok", ParseFlatBlueprint(written, second, error) && SameFlat(first, second));
    up::SetBool(L, "identical", written == WithoutCr(raw));
    bool patched = false;
    const std::string kept = WriteFlatInto(first, raw, patched);
    up::SetBool(L, "preserved", patched && kept == raw);
    return 1;
  }

  // ---- the browser --------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_EffectClass(lua_State* const L)
  {
    const std::string path = up::ArgString(L, 1, "ed_fx_effect_class");
    lua_pushnumber(L, static_cast<lua_Number>(EffectFaction(path)));
    lua_pushnumber(L, static_cast<lua_Number>(EffectKind(path)));
    return 2;
  }

  [[nodiscard]] std::string Lower(std::string text)
  {
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return text;
  }

  int __cdecl cfunc_ListFiles(lua_State* const L)
  {
    struct Entry
    {
      std::string path;
      std::string name;
      std::string sortKey;
    };
    std::vector<Entry> entries;
    // the game's /effects, and the folders the caller names (a mod project's `/mods/<name>/effects`, the map's own `effects`)
    std::vector<std::string> roots{"/effects"};
    if (lua_type(L, 1) == LUA_TTABLE) {
      for (int i = 1;; ++i) {
        lua_rawgeti(L, 1, i);
        const int type = lua_type(L, -1);
        if (type == LUA_TSTRING) {
          roots.emplace_back(lua_tostring(L, -1), lua_strlen(L, -1));
        }
        lua_settop(L, -2);
        if (type == LUA_TNIL) {
          break;
        }
      }
    }
    if (moho::CVirtualFileSystem* const vfs = moho::DISK_GetVFS(); vfs != nullptr) {
      for (const std::string& root : roots) {
        fa::vector<fa::string> found;
        vfs->EnumerateFiles(root.c_str(), "*.bp", true, &found);
        for (const fa::string& file : found) {
          Entry entry;
          entry.path = file.c_str();
          entry.name = entry.path.substr(entry.path.find_last_of('/') + 1);
          if (const std::size_t dot = entry.name.rfind(".bp"); dot != std::string::npos) {
            entry.name.resize(dot);
          }
          entry.sortKey = Lower(entry.name);
          entries.push_back(std::move(entry));
        }
      }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.sortKey < b.sortKey; });
    lua_newtable(L);
    int n = 0;
    for (const Entry& entry : entries) {
      lua_newtable(L);
      up::SetString(L, "path", entry.path);
      up::SetString(L, "name", entry.name);
      up::SetNumber(L, "faction", EffectFaction(entry.path));
      up::SetNumber(L, "kind", EffectKind(entry.path));
      lua_rawseti(L, -2, ++n);
    }
    return 1;
  }

  int __cdecl cfunc_Templates(lua_State* const L)
  {
    const std::vector<EffectTemplate> templates = ParseEffectTemplates(up::ArgString(L, 1, "ed_fx_templates"));
    lua_newtable(L);
    int n = 0;
    for (const EffectTemplate& t : templates) {
      lua_newtable(L);
      up::SetString(L, "name", t.name);
      up::SetStringList(L, "paths", t.paths);
      lua_rawseti(L, -2, ++n);
    }
    return 1;
  }

  // ---- the effect graph ---------------------------------------------------------------------------------------------------------------------------

  const char* PinTypeText(const PinType type)
  {
    return PinTypeName(type);
  }

  const char* ParamKindText(const ParamKind kind)
  {
    switch (kind) {
    case ParamKind::Float: return "float";
    case ParamKind::Int: return "int";
    case ParamKind::Bool: return "bool";
    case ParamKind::Color: return "color";
    case ParamKind::Choice: return "choice";
    default: return "text";
    }
  }

  void PushPins(lua_State* const L, const char* const key, const std::vector<PinDef>& pins)
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (std::size_t i = 0; i < pins.size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "name", pins[i].name);
      up::SetString(L, "type", PinTypeText(pins[i].type));
      SetFloatArray(L, "initial", pins[i].initial);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  int __cdecl cfunc_GraphNodeTypes(lua_State* const L)
  {
    lua_newtable(L);
    int n = 0;
    for (const auto& type : EffectNodeTypes().All()) {
      lua_newtable(L);
      up::SetString(L, "name", type->name);
      up::SetString(L, "title", type->title);
      up::SetString(L, "category", type->category);
      up::SetString(L, "help", type->help);
      up::SetBool(L, "output", type->isOutput);
      PushPins(L, "inputs", type->inputs);
      PushPins(L, "outputs", type->outputs);
      lua_pushstring(L, "params");
      lua_newtable(L);
      for (std::size_t i = 0; i < type->params.size(); ++i) {
        const ParamDef& p = type->params[i];
        lua_newtable(L);
        up::SetString(L, "name", p.name);
        up::SetString(L, "kind", ParamKindText(p.kind));
        up::SetNumber(L, "min", p.min);
        up::SetNumber(L, "max", p.max);
        up::SetStringList(L, "choices", p.choices);
        SetFloatArray(L, "initial", p.initial);
        up::SetString(L, "initialText", p.initialText);
        up::SetString(L, "help", p.help);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
      }
      lua_settable(L, -3);
      lua_rawseti(L, -2, ++n);
    }
    return 1;
  }

  void PushGraph(lua_State* const L, const NodeGraph& graph)
  {
    lua_newtable(L);
    lua_pushstring(L, "nodes");
    lua_newtable(L);
    int n = 0;
    for (const GraphNode& node : graph.Nodes()) {
      lua_newtable(L);
      up::SetNumber(L, "id", node.id);
      up::SetString(L, "type", node.type != nullptr ? node.type->name : "");
      up::SetNumber(L, "x", node.x);
      up::SetNumber(L, "y", node.y);
      lua_pushstring(L, "params");
      lua_newtable(L);
      for (std::size_t i = 0; i < node.params.size(); ++i) {
        lua_newtable(L);
        SetFloatArray(L, "v", node.params[i].value);
        up::SetString(L, "text", node.params[i].text);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
      }
      lua_settable(L, -3);
      lua_rawseti(L, -2, ++n);
    }
    lua_settable(L, -3);
    lua_pushstring(L, "links");
    lua_newtable(L);
    n = 0;
    for (const GraphLink& link : graph.Links()) {
      lua_newtable(L);
      up::SetNumber(L, "fromNode", link.fromNode);
      up::SetNumber(L, "fromPin", link.fromPin);
      up::SetNumber(L, "toNode", link.toNode);
      up::SetNumber(L, "toPin", link.toPin);
      lua_rawseti(L, -2, ++n);
    }
    lua_settable(L, -3);
  }

  int __cdecl cfunc_GraphBuild(lua_State* const L)
  {
    const std::string name = up::ArgString(L, 1, "ed_fx_graph_build");
    std::vector<std::pair<std::string, EmitterBlueprint>> emitters;
    EachOf(L, 2, [&](const int item) {
      const bool has = up::PushField(L, item, "emitter") == LUA_TTABLE;
      emitters.emplace_back(up::FieldString(L, item, "name"), has ? ReadEmitter(L, lua_gettop(L)) : NewEmitter());
      lua_settop(L, -2);
    });
    NodeGraph graph(EffectNodeTypes());
    BuildEffectGraph(graph, name, emitters);
    PushGraph(L, graph);
    return 1;
  }

  int __cdecl cfunc_GraphEmitters(lua_State* const L)
  {
    NodeGraph graph(EffectNodeTypes());
    std::map<int, int> ids; // the table's ids -> the graph's
    std::vector<std::pair<int, int>> nodes; // (table id, node)
    EachElement(L, 1, "nodes", [&](const int item) {
      const int old = up::FieldInt(L, item, "id", 0);
      const int id = graph.AddNode(up::FieldString(L, item, "type"), FieldFloat(L, item, "x", 0.0f), FieldFloat(L, item, "y", 0.0f));
      if (id == 0) {
        return;
      }
      ids[old] = id;
      GraphNode* const node = graph.Find(id);
      std::size_t index = 0;
      if (PushTable(L, item, "params")) {
        EachOf(L, -1, [&](const int param) {
          if (node != nullptr && index < node->params.size()) {
            ParamValue value = node->params[index];
            value.value = ReadFloatArray(L, param, "v", value.value);
            value.text = up::FieldString(L, param, "text", value.text);
            graph.SetParam(id, index, value);
          }
          ++index;
        });
      }
      lua_settop(L, -2);
    });
    EachElement(L, 1, "links", [&](const int item) {
      const auto from = ids.find(up::FieldInt(L, item, "fromNode", -1));
      const auto to = ids.find(up::FieldInt(L, item, "toNode", -1));
      if (from != ids.end() && to != ids.end()) {
        (void)graph.Link(from->second, up::FieldInt(L, item, "fromPin", 0), to->second, up::FieldInt(L, item, "toPin", 0));
      }
    });
    std::vector<std::pair<std::string, EmitterBlueprint>> emitters;
    std::string error;
    if (!GraphEmitters(graph, emitters, error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    int n = 0;
    for (const auto& [name, emitter] : emitters) {
      lua_newtable(L);
      up::SetString(L, "name", name);
      lua_pushstring(L, "emitter");
      PushEmitter(L, emitter);
      lua_settable(L, -3);
      lua_rawseti(L, -2, ++n);
    }
    return 1;
  }

  // ---- effect decorations in the map's script ---------------------------------------------------------------------------------------------------

  int __cdecl cfunc_ScriptEffects(lua_State* const L)
  {
    const std::string text = WithScriptEffects(up::ArgString(L, 1, "ed_fx_script_effects"), lua_toboolean(L, 2) != 0);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  const up::Natives gEdUpFxNatives{{
    {"ed_fx_info", &cfunc_Info, "ed_fx_info() -> { curves, flags, blendModes, factions, kinds }"},
    {"ed_fx_emitter_new", &cfunc_EmitterNew, "ed_fx_emitter_new() -> emitter"},
    {"ed_fx_emitter_parse", &cfunc_EmitterParse, "ed_fx_emitter_parse(text) -> emitter | nil, message"},
    {"ed_fx_emitter_write", &cfunc_EmitterWrite, "ed_fx_emitter_write(emitter[, original]) -> text, patched"},
    {"ed_fx_emitter_roundtrip", &cfunc_EmitterRoundTrip, "ed_fx_emitter_roundtrip(text) -> { ok, identical, preserved, error, written }"},
    {"ed_fx_emitter_type_tag", &cfunc_EmitterTypeTag, "ed_fx_emitter_type_tag(emitter) -> string"},
    {"ed_fx_curve_samples", &cfunc_CurveSamples, "ed_fx_curve_samples(keys, xRange, n) -> { mid, lo, hi }"},
    {"ed_fx_flat_info", &cfunc_FlatInfo, "ed_fx_flat_info(kind) -> { { key, kind, initial, color, help, texture }, ... }"},
    {"ed_fx_flat_parse", &cfunc_FlatParse, "ed_fx_flat_parse(text) -> flat | nil, message"},
    {"ed_fx_flat_write", &cfunc_FlatWrite, "ed_fx_flat_write(flat[, original]) -> text, patched"},
    {"ed_fx_flat_roundtrip", &cfunc_FlatRoundTrip, "ed_fx_flat_roundtrip(text) -> { ok, identical, preserved }"},
    {"ed_fx_effect_class", &cfunc_EffectClass, "ed_fx_effect_class(path) -> factionIndex, kindIndex"},
    {"ed_fx_list_files", &cfunc_ListFiles, "ed_fx_list_files([roots]) -> { { path, name, faction, kind }, ... }"},
    {"ed_fx_templates", &cfunc_Templates, "ed_fx_templates(luaText) -> { { name, paths }, ... }"},
    {"ed_fx_graph_node_types", &cfunc_GraphNodeTypes, "ed_fx_graph_node_types() -> { { name, title, category, inputs, outputs, params }, ... }"},
    {"ed_fx_graph_build", &cfunc_GraphBuild, "ed_fx_graph_build(name, { { name, emitter }, ... }) -> graph"},
    {"ed_fx_script_effects", &cfunc_ScriptEffects, "ed_fx_script_effects(scriptText, on) -> text   the map script with the MapEditorEffects function and its call (on) or without (off)"},
    {"ed_fx_graph_emitters", &cfunc_GraphEmitters, "ed_fx_graph_emitters(graph) -> { { name, emitter }, ... } | nil, message"},
  }};
} // namespace

namespace moho::editor::up
{
  faf::editor::EmitterBlueprint FxReadEmitter(lua_State* const L, const int index)
  {
    return ReadEmitter(L, index);
  }

  faf::editor::FlatBlueprint FxReadFlat(lua_State* const L, const int index)
  {
    return ReadFlat(L, index);
  }
} // namespace moho::editor::up
