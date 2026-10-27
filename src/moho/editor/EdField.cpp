// EDITOR: Lua natives of the field engine (EdFieldCore.h; docs/projects/editor/TERRAIN_GEN_DESIGN.md stage T1).
//
// A field is a float32 grid held in native memory and named in Lua by a number (a handle). Lua never sees the samples except through small reads, previews
// and the stats. Ops are pure: an op makes a new field from its inputs. Heights are world units (the document's words / 128), masks 0..1.
//
//   ed_field_height([step, x0, z0, x1, z1])   -> f      the map's heights (the document's truth); step keeps every step-th sample (cell = step); the rectangle is in map samples
//   ed_field_stratum(stratum[, raw])           -> f      paint mask of engine stratum 0..7 as 0..1 (the effective value the shader uses) or raw bytes 0..255;
//                                                       one sample per mask cell, cell = world size of a mask cell
//   ed_field_tt()                              -> f      the terrain-type grid as TypeCodes (stage T2); ed_field_commit_tt(f[, {mask}]) -> before, changed writes codes back
//   ed_field_wmask(kind)                       -> f      a water mask (0 foam, 1 flatness, 2 depth bias) as 0..1 (bytes / 255), (map / 2)^2 samples; ed_field_commit_wmask(f, kind[, {mask, strength}]) -> before, changed
//   ed_field_new(w, h, value[, cell, ox, oz])  -> f      constant field
//   ed_field_from_table(w, h, values[, cell, ox, oz]) -> f
//   ed_field_image(path[, {as='unit'|'height', width, height, cell}]) -> f   16-bit greyscale image (PNG, TIFF, raw); 'unit' = 0..1 (default), 'height' = words / 128
//   ed_field_op(name, params)                  -> f | nil, message     params: numbers, strings, booleans, number lists, and the input fields a, b, mask, dx, dz, like
//   ed_field_ops()                             -> list of op names
//   ed_field_info(f)                           -> w, h, cell, ox, oz
//   ed_field_stats(f)                          -> min, max, mean, sd
//   ed_field_checksum(f)                       -> hex string (FNV-1a of the float bits, bit identical for any thread count)
//   ed_field_get(f, x, z)                      -> value of one sample (indices)
//   ed_field_sample(f, wx, wz)                 -> bilinear value at a world position
//   ed_field_read(f, x0, z0, x1, z1)           -> list of values, row major (at most 65536 samples)
//   ed_field_preview(f, pw, ph[, {lo, hi, ramp='grey'|'terrain'|'heat'}]) -> rgba list (pw * ph * 4, 0..255), lo, hi    box-average thumbnail
//   ed_field_commit_height(f[, {label, strength, mask, x0, z0, x1, z1}]) -> changed, x0, z0, x1, z1    writes the field into the map heights as ONE undo step of the document
//                                              (history.lua's terrain entry and the re-snap are the Lua side's, fields.lua)
//   ed_field_commit_mask(f, stratum[, {raw, mode='set'|'max', strength, mask}]) -> before, changed    writes the field into a paint mask; `before` is a raw field of the
//                                              old bytes (ed_field_commit_mask(before, stratum, {raw = true}) restores them: the undo)
//   ed_field_copy(f)                           -> f2    a second handle on the same samples
//   ed_field_free(f) / ed_field_free_all()     frees
//   ed_field_count()                           -> number of fields, bytes
//   ed_field_threads([n])                      -> threads in use (sets them first when n is given; results never depend on it)
//   ed_field_clock()                           -> seconds (steady clock, for timings)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/editor/EdFieldCore.h"
#include "moho/editor/EdStrata.h"
#include "moho/editor/EdEnv.h"
#include "moho/editor/EdTerrainType.h"
#include "moho/editor/EditorDocument.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace moho::edfield
{
  bool ReadImage16(const std::string& utf8Path, std::vector<std::uint16_t>& out, int& w, int& h);   // EdImageIO.cpp
}

namespace
{
  using moho::editor::field::Grid;
  using moho::editor::field::GridPtr;
  namespace fld = moho::editor::field;

  // ---- handles -------------------------------------------------------------------------------------------------------------------------------

  std::map<int, GridPtr> gGrids;
  int gNextHandle = 1;

  int Store(Grid&& g)
  {
    const int id = gNextHandle++;
    gGrids[id] = std::make_shared<const Grid>(std::move(g));
    return id;
  }

  GridPtr Find(const int id)
  {
    const auto it = gGrids.find(id);
    return it != gGrids.end() ? it->second : GridPtr();
  }

  GridPtr Arg(lua_State* const L, const int index)
  {
    GridPtr g = Find(static_cast<int>(luaL_checknumber(L, index)));
    if (!g) {
      luaL_error(L, "bad field handle");
    }
    return g;
  }

  int Fail(lua_State* const L, const std::string& message)
  {
    lua_pushnil(L);
    lua_pushstring(L, message.c_str());
    return 2;
  }

  [[nodiscard]] std::uint16_t ClampWord(const float value)
  {
    const float v = std::floor(value + 0.5f);
    return static_cast<std::uint16_t>(v < 0.0f ? 0.0f : (v > 65535.0f ? 65535.0f : v));
  }

  // ---- table reading -------------------------------------------------------------------------------------------------------------------------

  void Flatten(lua_State* const L, const int index, std::vector<double>& out, const int depth)
  {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, index, i);
      const int t = lua_type(L, -1);
      if (t == LUA_TNIL) {
        lua_settop(L, -2);
        break;
      }
      if (t == LUA_TNUMBER) {
        out.push_back(lua_tonumber(L, -1));
      } else if (t == LUA_TTABLE && depth < 2) {
        Flatten(L, lua_gettop(L), out, depth + 1);
      }
      lua_settop(L, -2);
    }
  }

  bool IsInputKey(const std::string& k)
  {
    return k == "a" || k == "b" || k == "mask" || k == "dx" || k == "dz" || k == "like";
  }

  /** Reads the table at stack index `index` into op arguments (input keys become fields). */
  bool ReadArgs(lua_State* const L, const int index, fld::OpArgs& args, std::string& err)
  {
    if (lua_type(L, index) != LUA_TTABLE) {
      return true;
    }
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING) {
        const std::string key = lua_tostring(L, -2);
        const int t = lua_type(L, -1);
        if (IsInputKey(key) && t == LUA_TNUMBER) {
          const GridPtr g = Find(static_cast<int>(lua_tonumber(L, -1)));
          if (!g) {
            err = "input '" + key + "' is not a field";
            lua_settop(L, -3);
            return false;
          }
          args.in[key] = g;
        } else if (t == LUA_TNUMBER) {
          args.p.num[key] = lua_tonumber(L, -1);
        } else if (t == LUA_TBOOLEAN) {
          args.p.num[key] = lua_toboolean(L, -1) != 0 ? 1.0 : 0.0;
        } else if (t == LUA_TSTRING) {
          args.p.str[key] = lua_tostring(L, -1);
        } else if (t == LUA_TTABLE) {
          std::vector<double> flat;
          Flatten(L, lua_gettop(L), flat, 0);
          args.p.arr[key] = std::move(flat);
        }
      }
      lua_settop(L, -2);
    }
    return true;
  }

  double OptNum(lua_State* const L, const int index, const double fallback)
  {
    return lua_type(L, index) == LUA_TNUMBER ? lua_tonumber(L, index) : fallback;
  }

  double OptField(lua_State* const L, const int index, const char* const key, const double fallback)
  {
    if (lua_type(L, index) != LUA_TTABLE) {
      return fallback;
    }
    lua_pushstring(L, key);
    lua_gettable(L, index);
    double v = fallback;
    if (lua_type(L, -1) == LUA_TNUMBER) {
      v = lua_tonumber(L, -1);
    } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
      v = lua_toboolean(L, -1) != 0 ? 1.0 : 0.0;
    }
    lua_settop(L, -2);
    return v;
  }

  std::string OptString(lua_State* const L, const int index, const char* const key, const char* const fallback)
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

  // ---- constructors ---------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_Height(lua_State* const L)
  {
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    const moho::editor::MapDocument& doc = moho::editor::Document();
    const int step = std::max(1, std::min(64, static_cast<int>(OptNum(L, 1, 1))));
    moho::editor::CellRect rect;
    rect.x0 = static_cast<int>(OptNum(L, 2, 0));
    rect.z0 = static_cast<int>(OptNum(L, 3, 0));
    rect.x1 = static_cast<int>(OptNum(L, 4, doc.Width() - 1));
    rect.z1 = static_cast<int>(OptNum(L, 5, doc.Height() - 1));
    rect = doc.Clamp(rect);
    if (rect.Empty()) {
      return Fail(L, "empty rectangle");
    }
    std::vector<std::uint16_t> words;
    doc.ReadRect(rect, words);
    const int rw = rect.Width();
    const int w = (rw - 1) / step + 1;
    const int h = (rect.Height() - 1) / step + 1;
    Grid g(w, h, static_cast<float>(step), static_cast<float>(rect.x0), static_cast<float>(rect.z0));
    for (int z = 0; z < h; ++z) {
      for (int x = 0; x < w; ++x) {
        g.At(x, z) = static_cast<float>(words[static_cast<std::size_t>(z * step) * static_cast<std::size_t>(rw) + static_cast<std::size_t>(x * step)]) / 128.0f;
      }
    }
    lua_pushnumber(L, Store(std::move(g)));
    return 1;
  }

  float DecodeMask(const std::uint8_t m)
  {
    const float e = 2.0f * static_cast<float>(m) / 255.0f - 1.0f;
    return e < 0.0f ? 0.0f : e;
  }

  std::uint8_t EncodeMask(float e)
  {
    if (e <= 0.0f) return 0;
    if (e > 1.0f) e = 1.0f;
    return static_cast<std::uint8_t>(std::floor((e + 1.0f) / 2.0f * 255.0f + 0.5f));
  }

  int __cdecl cfunc_Stratum(lua_State* const L)
  {
    const int stratum = static_cast<int>(luaL_checknumber(L, 1));
    const bool raw = lua_toboolean(L, 2) != 0;
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    std::vector<std::uint8_t> bytes;
    int mw = 0;
    int mh = 0;
    if (!moho::editor::StratumMaskRead(stratum, bytes, mw, mh)) {
      return Fail(L, "no paint mask");
    }
    const float ps = static_cast<float>(moho::editor::Document().Width() - 1) / static_cast<float>(mw);
    Grid g(mw, mh, ps, 0.5f * ps, 0.5f * ps);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      g.v[i] = raw ? static_cast<float>(bytes[i]) : DecodeMask(bytes[i]);
    }
    lua_pushnumber(L, Store(std::move(g)));
    return 1;
  }

  /** The terrain-type grid as a field of TypeCodes (one sample per grid cell, cell = world size of a grid cell). */
  int __cdecl cfunc_TerrainType(lua_State* const L)
  {
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    std::vector<std::uint8_t> bytes;
    int tw = 0;
    int th = 0;
    if (!moho::editor::TerrainTypeRead(bytes, tw, th)) {
      return Fail(L, "no terrain type grid");
    }
    const float ps = static_cast<float>(moho::editor::Document().Width() - 1) / static_cast<float>(tw);
    Grid g(tw, th, ps, 0.5f * ps, 0.5f * ps);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      g.v[i] = static_cast<float>(bytes[i]);
    }
    lua_pushnumber(L, Store(std::move(g)));
    return 1;
  }

  int __cdecl cfunc_New(lua_State* const L)
  {
    const int w = static_cast<int>(luaL_checknumber(L, 1));
    const int h = static_cast<int>(luaL_checknumber(L, 2));
    if (w < 1 || h < 1 || static_cast<long long>(w) * h > (1LL << 27)) {
      return Fail(L, "bad size");
    }
    Grid g(w, h, static_cast<float>(OptNum(L, 4, 1)), static_cast<float>(OptNum(L, 5, 0)), static_cast<float>(OptNum(L, 6, 0)), static_cast<float>(luaL_checknumber(L, 3)));
    lua_pushnumber(L, Store(std::move(g)));
    return 1;
  }

  int __cdecl cfunc_FromTable(lua_State* const L)
  {
    const int w = static_cast<int>(luaL_checknumber(L, 1));
    const int h = static_cast<int>(luaL_checknumber(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    if (w < 1 || h < 1 || static_cast<long long>(w) * h > (1LL << 27)) {
      return Fail(L, "bad size");
    }
    Grid g(w, h, static_cast<float>(OptNum(L, 4, 1)), static_cast<float>(OptNum(L, 5, 0)), static_cast<float>(OptNum(L, 6, 0)));
    for (std::size_t i = 0; i < g.v.size(); ++i) {
      lua_rawgeti(L, 3, static_cast<int>(i) + 1);
      g.v[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_settop(L, -2);
    }
    lua_pushnumber(L, Store(std::move(g)));
    return 1;
  }

  int __cdecl cfunc_Image(lua_State* const L)
  {
    const char* const path = lua_tostring(L, 1);
    if (path == nullptr) {
      return Fail(L, "bad path");
    }
    std::vector<std::uint16_t> px;
    int iw = 0;
    int ih = 0;
    if (!moho::edfield::ReadImage16(path, px, iw, ih) || iw < 1 || ih < 1) {
      return Fail(L, "cannot read the image");
    }
    const bool height = OptString(L, 2, "as", "unit") == "height";
    const float scale = height ? 1.0f / 128.0f : 1.0f / 65535.0f;
    const int tw = static_cast<int>(OptField(L, 2, "width", iw));
    const int th = static_cast<int>(OptField(L, 2, "height", ih));
    if (tw < 2 || th < 2 || static_cast<long long>(tw) * th > (1LL << 27)) {
      return Fail(L, "bad size");
    }
    Grid g(tw, th, static_cast<float>(OptField(L, 2, "cell", 1)), 0.0f, 0.0f);
    for (int z = 0; z < th; ++z) {
      for (int x = 0; x < tw; ++x) {
        if (tw == iw && th == ih) {
          g.At(x, z) = static_cast<float>(px[static_cast<std::size_t>(z) * static_cast<std::size_t>(iw) + static_cast<std::size_t>(x)]) * scale;
          continue;
        }
        const double fx = static_cast<double>(x) * (iw - 1) / (tw - 1);
        const double fz = static_cast<double>(z) * (ih - 1) / (th - 1);
        const int x0 = std::min(iw - 2, static_cast<int>(fx));
        const int z0 = std::min(ih - 2, static_cast<int>(fz));
        const float tx = static_cast<float>(fx - x0);
        const float tz = static_cast<float>(fz - z0);
        const auto at = [&](const int xx, const int zz) { return static_cast<float>(px[static_cast<std::size_t>(zz) * static_cast<std::size_t>(iw) + static_cast<std::size_t>(xx)]); };
        const float top = at(x0, z0) * (1.0f - tx) + at(x0 + 1, z0) * tx;
        const float bottom = at(x0, z0 + 1) * (1.0f - tx) + at(x0 + 1, z0 + 1) * tx;
        g.At(x, z) = (top * (1.0f - tz) + bottom * tz) * scale;
      }
    }
    lua_pushnumber(L, Store(std::move(g)));
    return 1;
  }

  // ---- ops -----------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_Op(lua_State* const L)
  {
    const char* const name = lua_tostring(L, 1);
    if (name == nullptr) {
      return Fail(L, "bad op name");
    }
    fld::OpArgs args;
    std::string err;
    if (!ReadArgs(L, 2, args, err)) {
      return Fail(L, err);
    }
    // a generator without a shape takes the map's
    if (!args.in.count("like") && !args.in.count("a") && !args.p.Has("width") && moho::editor::AttachDocument(false)) {
      args.p.num["width"] = moho::editor::Document().Width();
      args.p.num["height"] = moho::editor::Document().Height();
    }
    Grid out;
    if (!fld::RunOp(name, args, out, err)) {
      return Fail(L, std::string(name) + ": " + err);
    }
    lua_pushnumber(L, Store(std::move(out)));
    return 1;
  }

  /** ed_field_op_multi(name, params) -> main field, { name = field, ... } : like ed_field_op, plus the other results of ops that have several (erosion: wear, deposit, flow ...). */
  int __cdecl cfunc_OpMulti(lua_State* const L)
  {
    const char* const name = lua_tostring(L, 1);
    if (name == nullptr) {
      return Fail(L, "bad op name");
    }
    fld::OpArgs args;
    std::string err;
    if (!ReadArgs(L, 2, args, err)) {
      return Fail(L, err);
    }
    if (!args.in.count("like") && !args.in.count("a") && !args.p.Has("width") && moho::editor::AttachDocument(false)) {
      args.p.num["width"] = moho::editor::Document().Width();
      args.p.num["height"] = moho::editor::Document().Height();
    }
    std::map<std::string, Grid> extra;
    args.extra = &extra;
    Grid out;
    if (!fld::RunOp(name, args, out, err)) {
      return Fail(L, std::string(name) + ": " + err);
    }
    lua_pushnumber(L, Store(std::move(out)));
    lua_newtable(L);
    for (auto& kv : extra) {
      lua_pushstring(L, kv.first.c_str());
      lua_pushnumber(L, Store(std::move(kv.second)));
      lua_settable(L, -3);
    }
    return 2;
  }

  int __cdecl cfunc_Ops(lua_State* const L)
  {
    lua_newtable(L);
    int i = 1;
    for (const std::string& n : fld::OpNames()) {
      lua_pushnumber(L, i++);
      lua_pushstring(L, n.c_str());
      lua_settable(L, -3);
    }
    return 1;
  }

  // ---- queries -------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_Info(lua_State* const L)
  {
    const GridPtr g = Arg(L, 1);
    lua_pushnumber(L, g->w);
    lua_pushnumber(L, g->h);
    lua_pushnumber(L, g->cell);
    lua_pushnumber(L, g->ox);
    lua_pushnumber(L, g->oz);
    return 5;
  }

  int __cdecl cfunc_Stats(lua_State* const L)
  {
    const fld::Stats s = fld::Measure(*Arg(L, 1));
    lua_pushnumber(L, s.min);
    lua_pushnumber(L, s.max);
    lua_pushnumber(L, s.mean);
    lua_pushnumber(L, s.sd);
    return 4;
  }

  int __cdecl cfunc_Checksum(lua_State* const L)
  {
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(fld::Checksum(*Arg(L, 1))));
    lua_pushstring(L, hex);
    return 1;
  }

  int __cdecl cfunc_Get(lua_State* const L)
  {
    const GridPtr g = Arg(L, 1);
    const int x = static_cast<int>(luaL_checknumber(L, 2));
    const int z = static_cast<int>(luaL_checknumber(L, 3));
    if (x < 0 || z < 0 || x >= g->w || z >= g->h) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushnumber(L, g->At(x, z));
    return 1;
  }

  int __cdecl cfunc_Sample(lua_State* const L)
  {
    const GridPtr g = Arg(L, 1);
    lua_pushnumber(L, fld::SampleWorld(*g, luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
  }

  int __cdecl cfunc_Read(lua_State* const L)
  {
    const GridPtr g = Arg(L, 1);
    int x0 = std::max(0, static_cast<int>(luaL_checknumber(L, 2)));
    int z0 = std::max(0, static_cast<int>(luaL_checknumber(L, 3)));
    int x1 = std::min(g->w - 1, static_cast<int>(luaL_checknumber(L, 4)));
    int z1 = std::min(g->h - 1, static_cast<int>(luaL_checknumber(L, 5)));
    if (x0 > x1 || z0 > z1 || static_cast<long long>(x1 - x0 + 1) * (z1 - z0 + 1) > 65536) {
      return Fail(L, "empty or too large (65536 samples at most)");
    }
    lua_newtable(L);
    int index = 1;
    for (int z = z0; z <= z1; ++z) {
      for (int x = x0; x <= x1; ++x, ++index) {
        lua_pushnumber(L, index);
        lua_pushnumber(L, g->At(x, z));
        lua_settable(L, -3);
      }
    }
    return 1;
  }

  struct Stop
  {
    float at;
    float r, g, b;
  };

  void RampColor(const std::string& ramp, const float t, std::uint8_t out[3])
  {
    static const Stop terrain[] = {{0.00f, 20, 40, 110}, {0.25f, 40, 110, 170}, {0.30f, 200, 190, 130}, {0.40f, 70, 140, 60},
                                   {0.65f, 130, 110, 80}, {0.85f, 170, 165, 160}, {1.00f, 250, 250, 250}};
    static const Stop heat[] = {{0.00f, 10, 10, 40}, {0.35f, 120, 20, 120}, {0.65f, 230, 90, 30}, {1.00f, 255, 240, 120}};
    static const Stop grey[] = {{0.0f, 0, 0, 0}, {1.0f, 255, 255, 255}};
    const Stop* stops = grey;
    int n = 2;
    if (ramp == "terrain") {
      stops = terrain;
      n = 7;
    } else if (ramp == "heat") {
      stops = heat;
      n = 4;
    }
    const float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    for (int i = 0; i + 1 < n; ++i) {
      if (c <= stops[i + 1].at) {
        const float u = (c - stops[i].at) / (stops[i + 1].at - stops[i].at);
        out[0] = static_cast<std::uint8_t>(stops[i].r + (stops[i + 1].r - stops[i].r) * u + 0.5f);
        out[1] = static_cast<std::uint8_t>(stops[i].g + (stops[i + 1].g - stops[i].g) * u + 0.5f);
        out[2] = static_cast<std::uint8_t>(stops[i].b + (stops[i + 1].b - stops[i].b) * u + 0.5f);
        return;
      }
    }
    out[0] = static_cast<std::uint8_t>(stops[n - 1].r);
    out[1] = static_cast<std::uint8_t>(stops[n - 1].g);
    out[2] = static_cast<std::uint8_t>(stops[n - 1].b);
  }

  int __cdecl cfunc_Preview(lua_State* const L)
  {
    const GridPtr g = Arg(L, 1);
    const int pw = static_cast<int>(luaL_checknumber(L, 2));
    const int ph = static_cast<int>(luaL_checknumber(L, 3));
    if (pw < 1 || ph < 1 || pw > 512 || ph > 512) {
      return Fail(L, "preview size 1..512");
    }
    std::vector<float> px;
    fld::Downsample(*g, pw, ph, px);
    float lo = 0.0f;
    float hi = 1.0f;
    if (lua_type(L, 4) == LUA_TTABLE && (OptField(L, 4, "lo", 1e30) < 1e29) && (OptField(L, 4, "hi", 1e30) < 1e29)) {
      lo = static_cast<float>(OptField(L, 4, "lo", 0));
      hi = static_cast<float>(OptField(L, 4, "hi", 1));
    } else {
      const fld::Stats s = fld::Measure(*g);
      lo = s.min;
      hi = s.max;
    }
    const std::string ramp = OptString(L, 4, "ramp", "grey");
    const float span = hi > lo ? hi - lo : 1.0f;
    lua_newtable(L);
    int index = 1;
    for (const float v : px) {
      std::uint8_t c[3];
      RampColor(ramp, (v - lo) / span, c);
      for (int k = 0; k < 4; ++k, ++index) {
        lua_pushnumber(L, index);
        lua_pushnumber(L, k < 3 ? c[k] : 255);
        lua_settable(L, -3);
      }
    }
    lua_pushnumber(L, lo);
    lua_pushnumber(L, hi);
    return 3;
  }

  // ---- commits -------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_CommitHeight(lua_State* const L)
  {
    const GridPtr f = Arg(L, 1);
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    moho::editor::MapDocument& doc = moho::editor::Document();
    const std::string label = OptString(L, 2, "label", "Field to terrain");
    const float strength = static_cast<float>(OptField(L, 2, "strength", 1.0));
    GridPtr mask;
    if (lua_type(L, 2) == LUA_TTABLE) {
      lua_pushstring(L, "mask");
      lua_gettable(L, 2);
      if (lua_type(L, -1) == LUA_TNUMBER) {
        mask = Find(static_cast<int>(lua_tonumber(L, -1)));
      }
      lua_settop(L, -2);
    }
    moho::editor::CellRect rect;
    rect.x0 = std::max(0, static_cast<int>(std::ceil(static_cast<double>(f->ox) - 1e-4)));
    rect.z0 = std::max(0, static_cast<int>(std::ceil(static_cast<double>(f->oz) - 1e-4)));
    rect.x1 = std::min(doc.Width() - 1, static_cast<int>(std::floor(static_cast<double>(f->ox) + (f->w - 1) * static_cast<double>(f->cell) + 1e-4)));
    rect.z1 = std::min(doc.Height() - 1, static_cast<int>(std::floor(static_cast<double>(f->oz) + (f->h - 1) * static_cast<double>(f->cell) + 1e-4)));
    rect.x0 = std::max(rect.x0, static_cast<int>(OptField(L, 2, "x0", rect.x0)));
    rect.z0 = std::max(rect.z0, static_cast<int>(OptField(L, 2, "z0", rect.z0)));
    rect.x1 = std::min(rect.x1, static_cast<int>(OptField(L, 2, "x1", rect.x1)));
    rect.z1 = std::min(rect.z1, static_cast<int>(OptField(L, 2, "z1", rect.z1)));
    if (rect.Empty()) {
      return Fail(L, "the field does not overlap the map");
    }
    const bool exact = f->cell == 1.0f && f->ox == std::floor(f->ox) && f->oz == std::floor(f->oz);
    const int rw = rect.Width();
    std::vector<std::uint16_t> words(rect.Count());
    for (int z = rect.z0; z <= rect.z1; ++z) {
      for (int x = rect.x0; x <= rect.x1; ++x) {
        float v = exact ? f->At(x - static_cast<int>(f->ox), z - static_cast<int>(f->oz))
                        : fld::SampleWorld(*f, static_cast<double>(x), static_cast<double>(z));
        float m = strength;
        if (mask) {
          m *= std::max(0.0f, std::min(1.0f, fld::SampleWorld(*mask, static_cast<double>(x), static_cast<double>(z))));
        }
        if (m < 1.0f) {
          const float old = static_cast<float>(doc.HeightAt(x, z)) / 128.0f;
          v = old + (v - old) * m;
        }
        words[static_cast<std::size_t>(z - rect.z0) * static_cast<std::size_t>(rw) + static_cast<std::size_t>(x - rect.x0)] = ClampWord(v * 128.0f);
      }
    }
    // a stroke that was left open (a brush stroke whose end never came) would swallow this commit into its undo step: BeginStroke does nothing while one is open and the
    // commit would be undone together with the brush; close it first so a field commit is always a step of its own
    while (doc.StrokeOpen()) {
      doc.EndStroke();
    }
    doc.BeginStroke(label);
    const std::size_t changed = doc.EditHeights(rect, words.data());
    doc.EndStroke();
    lua_pushnumber(L, static_cast<lua_Number>(changed));
    lua_pushnumber(L, rect.x0);
    lua_pushnumber(L, rect.z0);
    lua_pushnumber(L, rect.x1);
    lua_pushnumber(L, rect.z1);
    return 5;
  }

  int __cdecl cfunc_CommitMask(lua_State* const L)
  {
    const GridPtr f = Arg(L, 1);
    const int stratum = static_cast<int>(luaL_checknumber(L, 2));
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    std::vector<std::uint8_t> bytes;
    int mw = 0;
    int mh = 0;
    if (!moho::editor::StratumMaskRead(stratum, bytes, mw, mh)) {
      return Fail(L, "no paint mask");
    }
    const bool raw = OptField(L, 3, "raw", 0) != 0;
    const bool maxMode = OptString(L, 3, "mode", "set") == "max";
    const float strength = static_cast<float>(OptField(L, 3, "strength", 1.0));
    GridPtr mask;
    if (lua_type(L, 3) == LUA_TTABLE) {
      lua_pushstring(L, "mask");
      lua_gettable(L, 3);
      if (lua_type(L, -1) == LUA_TNUMBER) {
        mask = Find(static_cast<int>(lua_tonumber(L, -1)));
      }
      lua_settop(L, -2);
    }
    const float ps = static_cast<float>(moho::editor::Document().Width() - 1) / static_cast<float>(mw);
    Grid before(mw, mh, ps, 0.5f * ps, 0.5f * ps);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      before.v[i] = static_cast<float>(bytes[i]);
    }
    const bool same = f->w == mw && f->h == mh;
    std::vector<std::uint8_t> next(bytes.size());
    std::size_t changed = 0;
    for (int z = 0; z < mh; ++z) {
      for (int x = 0; x < mw; ++x) {
        const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(mw) + static_cast<std::size_t>(x);
        const double wx = (x + 0.5) * ps;
        const double wz = (z + 0.5) * ps;
        const float v = same ? f->At(x, z) : fld::SampleWorld(*f, wx, wz);
        float nb = raw ? std::floor(std::max(0.0f, std::min(255.0f, v)) + 0.5f) : static_cast<float>(EncodeMask(v));
        const float old = static_cast<float>(bytes[i]);
        if (maxMode) nb = std::max(nb, old);
        float m = strength;
        if (mask) m *= std::max(0.0f, std::min(1.0f, fld::SampleWorld(*mask, wx, wz)));
        if (m < 1.0f) nb = old + (nb - old) * m;
        next[i] = static_cast<std::uint8_t>(std::floor(std::max(0.0f, std::min(255.0f, nb)) + 0.5f));
        if (next[i] != bytes[i]) ++changed;
      }
    }
    if (changed > 0 && !moho::editor::StratumMaskWrite(stratum, next.data(), mw, mh)) {
      return Fail(L, "cannot write the paint mask");
    }
    lua_pushnumber(L, Store(std::move(before)));
    lua_pushnumber(L, static_cast<lua_Number>(changed));
    return 2;
  }

  /**
   * ed_field_commit_tt(f[, {mask}]) -> before, changed: writes the field (TypeCodes, rounded and clamped to 0..255) into the terrain-type grid. A field of another
   * shape is read at the cell centres in world space; cells where `mask` (0..1, optional) is not above 0.5 keep their code. `before` is a field of the old codes:
   * committing it again restores them (the undo).
   */
  int __cdecl cfunc_CommitTerrainType(lua_State* const L)
  {
    const GridPtr f = Arg(L, 1);
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    std::vector<std::uint8_t> bytes;
    int tw = 0;
    int th = 0;
    if (!moho::editor::TerrainTypeRead(bytes, tw, th)) {
      return Fail(L, "no terrain type grid");
    }
    GridPtr mask;
    if (lua_type(L, 2) == LUA_TTABLE) {
      lua_pushstring(L, "mask");
      lua_gettable(L, 2);
      if (lua_type(L, -1) == LUA_TNUMBER) {
        mask = Find(static_cast<int>(lua_tonumber(L, -1)));
      }
      lua_settop(L, -2);
    }
    const float ps = static_cast<float>(moho::editor::Document().Width() - 1) / static_cast<float>(tw);
    Grid before(tw, th, ps, 0.5f * ps, 0.5f * ps);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      before.v[i] = static_cast<float>(bytes[i]);
    }
    const bool same = f->w == tw && f->h == th;
    std::vector<std::uint8_t> next(bytes);
    std::size_t changed = 0;
    for (int z = 0; z < th; ++z) {
      for (int x = 0; x < tw; ++x) {
        const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(tw) + static_cast<std::size_t>(x);
        const double wx = (x + 0.5) * ps;
        const double wz = (z + 0.5) * ps;
        if (mask && fld::SampleWorld(*mask, wx, wz) <= 0.5f) {
          continue;
        }
        const float v = same ? f->At(x, z) : f->v[static_cast<std::size_t>(std::max(0, std::min(f->h - 1, static_cast<int>(std::floor((wz - f->oz) / f->cell + 0.5))))) *
                                                       static_cast<std::size_t>(f->w) +
                                                     static_cast<std::size_t>(std::max(0, std::min(f->w - 1, static_cast<int>(std::floor((wx - f->ox) / f->cell + 0.5)))))];
        next[i] = static_cast<std::uint8_t>(std::floor(std::max(0.0f, std::min(255.0f, v)) + 0.5f));
        if (next[i] != bytes[i]) ++changed;
      }
    }
    if (changed > 0 && !moho::editor::TerrainTypeWrite(next.data(), tw, th)) {
      return Fail(L, "cannot write the terrain type grid");
    }
    lua_pushnumber(L, Store(std::move(before)));
    lua_pushnumber(L, static_cast<lua_Number>(changed));
    return 2;
  }

  /** A water mask as a field (0..255 bytes scaled to 0..1; one sample per mask cell, cell = 2 world units). */
  int __cdecl cfunc_WaterMask(lua_State* const L)
  {
    const int kind = static_cast<int>(luaL_checknumber(L, 1));
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    std::vector<std::uint8_t> bytes;
    int mw = 0;
    int mh = 0;
    if (!moho::editor::WaterMaskRead(kind, bytes, mw, mh)) {
      return Fail(L, "no water mask");
    }
    const float ps = static_cast<float>(moho::editor::Document().Width() - 1) / static_cast<float>(mw);
    Grid g(mw, mh, ps, 0.5f * ps, 0.5f * ps);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      g.v[i] = static_cast<float>(bytes[i]) / 255.0f;
    }
    lua_pushnumber(L, Store(std::move(g)));
    return 1;
  }

  /** ed_field_commit_wmask(f, kind[, {mask, strength}]) -> before, changed: writes a field (0..1) into a water mask; `before` is a field of the old values (commit it again to undo). */
  int __cdecl cfunc_CommitWaterMask(lua_State* const L)
  {
    const GridPtr f = Arg(L, 1);
    const int kind = static_cast<int>(luaL_checknumber(L, 2));
    if (!moho::editor::AttachDocument(false)) {
      return Fail(L, "no terrain");
    }
    std::vector<std::uint8_t> bytes;
    int mw = 0;
    int mh = 0;
    if (!moho::editor::WaterMaskRead(kind, bytes, mw, mh)) {
      return Fail(L, "no water mask");
    }
    GridPtr mask;
    if (lua_type(L, 3) == LUA_TTABLE) {
      lua_pushstring(L, "mask");
      lua_gettable(L, 3);
      if (lua_type(L, -1) == LUA_TNUMBER) {
        mask = Find(static_cast<int>(lua_tonumber(L, -1)));
      }
      lua_settop(L, -2);
    }
    const float strength = static_cast<float>(OptField(L, 3, "strength", 1.0));
    const float ps = static_cast<float>(moho::editor::Document().Width() - 1) / static_cast<float>(mw);
    Grid before(mw, mh, ps, 0.5f * ps, 0.5f * ps);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      before.v[i] = static_cast<float>(bytes[i]) / 255.0f;
    }
    const bool same = f->w == mw && f->h == mh;
    std::vector<std::uint8_t> next(bytes.size());
    std::size_t changed = 0;
    for (int z = 0; z < mh; ++z) {
      for (int x = 0; x < mw; ++x) {
        const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(mw) + static_cast<std::size_t>(x);
        const double wx = (x + 0.5) * ps;
        const double wz = (z + 0.5) * ps;
        float v = same ? f->At(x, z) : fld::SampleWorld(*f, wx, wz);
        const float old = static_cast<float>(bytes[i]) / 255.0f;
        float m = strength;
        if (mask) m *= std::max(0.0f, std::min(1.0f, fld::SampleWorld(*mask, wx, wz)));
        if (m < 1.0f) v = old + (v - old) * m;
        next[i] = static_cast<std::uint8_t>(std::floor(std::max(0.0f, std::min(1.0f, v)) * 255.0f + 0.5f));
        if (next[i] != bytes[i]) ++changed;
      }
    }
    if (changed > 0 && !moho::editor::WaterMaskWrite(kind, next.data(), mw, mh)) {
      return Fail(L, "cannot write the water mask");
    }
    lua_pushnumber(L, Store(std::move(before)));
    lua_pushnumber(L, static_cast<lua_Number>(changed));
    return 2;
  }

  // ---- housekeeping --------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_Copy(lua_State* const L)
  {
    const GridPtr g = Arg(L, 1);
    const int id = gNextHandle++;
    gGrids[id] = g;
    lua_pushnumber(L, id);
    return 1;
  }

  int __cdecl cfunc_Free(lua_State* const L)
  {
    gGrids.erase(static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
  }

  int __cdecl cfunc_FreeAll(lua_State* const)
  {
    gGrids.clear();
    return 0;
  }

  int __cdecl cfunc_Count(lua_State* const L)
  {
    double bytes = 0.0;
    for (const auto& kv : gGrids) {
      bytes += static_cast<double>(kv.second->Count()) * 4.0;
    }
    lua_pushnumber(L, static_cast<lua_Number>(gGrids.size()));
    lua_pushnumber(L, bytes);
    return 2;
  }

  int __cdecl cfunc_Threads(lua_State* const L)
  {
    if (lua_type(L, 1) == LUA_TNUMBER) {
      fld::SetThreads(static_cast<int>(lua_tonumber(L, 1)));
    }
    lua_pushnumber(L, fld::Threads());
    return 1;
  }

  int __cdecl cfunc_Clock(lua_State* const L)
  {
    static const auto origin = std::chrono::steady_clock::now();
    lua_pushnumber(L, std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count());
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

  struct EdFieldLuaBootstrap
  {
    EdFieldLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      const auto bind = [&](const char* name, int (__cdecl* fn)(lua_State*), const char* doc) { (void)new moho::CScrLuaBinder(set, name, fn, nullptr, "<global>", doc); };
      bind("ed_field_height", &cfunc_Height, "ed_field_height([step, x0, z0, x1, z1]) -> field of the map heights");
      bind("ed_field_stratum", &cfunc_Stratum, "ed_field_stratum(stratum[, raw]) -> field of a paint mask");
      bind("ed_field_new", &cfunc_New, "ed_field_new(w, h, value[, cell, ox, oz]) -> field");
      bind("ed_field_from_table", &cfunc_FromTable, "ed_field_from_table(w, h, values[, cell, ox, oz]) -> field");
      bind("ed_field_image", &cfunc_Image, "ed_field_image(path[, {as, width, height, cell}]) -> field");
      bind("ed_field_op", &cfunc_Op, "ed_field_op(name, params) -> field | nil, message");
      bind("ed_field_op_multi", &cfunc_OpMulti, "ed_field_op_multi(name, params) -> field, { name = field }");
      bind("ed_field_ops", &cfunc_Ops, "ed_field_ops() -> list of op names");
      bind("ed_field_info", &cfunc_Info, "ed_field_info(f) -> w, h, cell, ox, oz");
      bind("ed_field_stats", &cfunc_Stats, "ed_field_stats(f) -> min, max, mean, sd");
      bind("ed_field_checksum", &cfunc_Checksum, "ed_field_checksum(f) -> hex");
      bind("ed_field_get", &cfunc_Get, "ed_field_get(f, x, z) -> value");
      bind("ed_field_sample", &cfunc_Sample, "ed_field_sample(f, wx, wz) -> value");
      bind("ed_field_read", &cfunc_Read, "ed_field_read(f, x0, z0, x1, z1) -> values");
      bind("ed_field_preview", &cfunc_Preview, "ed_field_preview(f, pw, ph[, {lo, hi, ramp}]) -> rgba, lo, hi");
      bind("ed_field_commit_height", &cfunc_CommitHeight, "ed_field_commit_height(f[, opts]) -> changed, x0, z0, x1, z1");
      bind("ed_field_commit_mask", &cfunc_CommitMask, "ed_field_commit_mask(f, stratum[, opts]) -> before, changed");
      bind("ed_field_tt", &cfunc_TerrainType, "ed_field_tt() -> field of the terrain-type grid (TypeCodes)");
      bind("ed_field_commit_tt", &cfunc_CommitTerrainType, "ed_field_commit_tt(f[, {mask}]) -> before, changed");
      bind("ed_field_wmask", &cfunc_WaterMask, "ed_field_wmask(kind) -> field of a water mask");
      bind("ed_field_commit_wmask", &cfunc_CommitWaterMask, "ed_field_commit_wmask(f, kind[, opts]) -> before, changed");
      bind("ed_field_copy", &cfunc_Copy, "ed_field_copy(f) -> f2");
      bind("ed_field_free", &cfunc_Free, "ed_field_free(f)");
      bind("ed_field_free_all", &cfunc_FreeAll, "ed_field_free_all()");
      bind("ed_field_count", &cfunc_Count, "ed_field_count() -> n, bytes");
      bind("ed_field_threads", &cfunc_Threads, "ed_field_threads([n]) -> threads");
      bind("ed_field_clock", &cfunc_Clock, "ed_field_clock() -> seconds");
    }
  };

  const EdFieldLuaBootstrap gEdFieldLuaBootstrap{};
} // namespace

namespace moho::editor::field
{
  // handle access for the other native files of the field engine (EdErosionLua.cpp)
  int StoreGrid(Grid&& g) { return Store(std::move(g)); }
  GridPtr FindGrid(const int id) { return Find(id); }
} // namespace moho::editor::field
