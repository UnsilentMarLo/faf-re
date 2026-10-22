// EDITOR: Lua natives of the layer converter's texture work (faf_studio editor/runtime/lua/editor/texconv.lua; maths in EdTexConv.h, tests in tests/EdTexConvTest.cpp). Whole textures are
// processed here, because a 1024 x 1024 picture is four million numbers as a Lua table.
//
//   ed_tex_convert(src, dst, spec)   -> bytes, width, height, lossless | nil, message
//        Reads the DDS `src` (a disk path or a VFS path: entries inside the game's archives work), builds a new picture from its channels and writes it as a DXT5 DDS to `dst`.
//        spec (all optional): size (a square) or width + height (default: the source's size; the source is resampled), mips (levels, default the whole chain),
//        r / g / b / a = a channel table { from, value, scale, bias, invert, center, lo, hi }:
//          from  'keep' (default: the channel as it is), 'r' 'g' 'b' 'a', 'lum', 'const' (value), 'atlas' (atlas = { src, tile, ch }: channel ch of tile 1..8 of a roughness / height atlas)
//          v = source value; invert: 1 - v; center: (v - mean) * scale + 0.5 + bias; otherwise v * scale + bias; then clamped to lo .. hi (defaults 0 .. 1; scale 1, bias 0)
//        When the source is DXT5 at the result's size and only the alpha changes (keep / r-less: 'a' or 'const', no centring), the alpha blocks of every mip level are rewritten and the colour is
//        copied bit for bit (lossless = true).
//   ed_tex_atlas(dst, spec)          -> bytes, report | nil, message
//        Builds the roughness / height atlas of the PBR terrain shaders (EdTexConv.h: 2 x 2 tiles, eight layers, R G / B A) and writes it as a DXT5 DDS with mip maps.
//        spec: size (multiple of 8, default 1024), rough / height (the value of a tile with no picture: 0.65 / 0.5), mips, tiles = { [1] = { src, rough = chan, height = chan }, ... [8] }
//        (tile 1 = base layer, 2..8 = layers 1..7; the channels read channels of that tile's own texture). report[k] = 'ok' | 'no texture' | 'cannot read: ...' per tile 1..8.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/editor/EdIcon.h"
#include "moho/editor/EdTexConv.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace
{
  namespace tc = moho::editor::texconv;
  namespace dds = moho::editor::dds;

  int Fail(lua_State* const L, const std::string& message)
  {
    lua_pushnil(L);
    lua_pushstring(L, message.c_str());
    return 2;
  }

  // ---- reading tables (Lua 5.0: absolute indices) ---------------------------------------------------------------------------------------

  /** Pushes t[key] and returns its Lua type (the caller pops with lua_settop(L, -2)). */
  int Push(lua_State* const L, const int table, const char* const key)
  {
    lua_pushstring(L, key);
    lua_gettable(L, table);
    return lua_type(L, -1);
  }

  double Num(lua_State* const L, const int table, const char* const key, const double fallback)
  {
    double v = fallback;
    if (Push(L, table, key) == LUA_TNUMBER) {
      v = lua_tonumber(L, -1);
    }
    lua_settop(L, -2);
    return v;
  }

  std::string Str(lua_State* const L, const int table, const char* const key)
  {
    std::string v;
    if (Push(L, table, key) == LUA_TSTRING) {
      v = lua_tostring(L, -1);
    }
    lua_settop(L, -2);
    return v;
  }

  bool Flag(lua_State* const L, const int table, const char* const key)
  {
    const int type = Push(L, table, key);
    const bool on = type == LUA_TBOOLEAN && lua_toboolean(L, -1) != 0;
    lua_settop(L, -2);
    return on;
  }

  // ---- channels ---------------------------------------------------------------------------------------------------------------------------

  struct AtlasRef
  {
    std::string path;
    int tile = 1;
    int channel = 0;
  };

  /** Reads a channel table at stack index `table` (0 = none: keep). `atlas` gets the atlas reference of from = 'atlas' (c.from stays Keep until the caller has the plane). */
  tc::Chan ReadChan(lua_State* const L, const int table, bool& wantsAtlas, AtlasRef& atlas)
  {
    tc::Chan c;
    wantsAtlas = false;
    if (table == 0 || lua_type(L, table) != LUA_TTABLE) {
      return c;
    }
    const std::string from = Str(L, table, "from");
    if (from == "r") c.from = tc::From::R;
    else if (from == "g") c.from = tc::From::G;
    else if (from == "b") c.from = tc::From::B;
    else if (from == "a") c.from = tc::From::A;
    else if (from == "lum") c.from = tc::From::Lum;
    else if (from == "const") c.from = tc::From::Const;
    else if (from == "atlas") {
      wantsAtlas = true;
      if (Push(L, table, "atlas") == LUA_TTABLE) {
        const int at = lua_gettop(L);
        atlas.path = Str(L, at, "src");
        atlas.tile = static_cast<int>(Num(L, at, "tile", 1));
        const std::string ch = Str(L, at, "ch");
        atlas.channel = ch == "g" ? 1 : ch == "b" ? 2 : ch == "a" ? 3 : 0;
      }
      lua_settop(L, -2);
    }
    c.value = static_cast<float>(Num(L, table, "value", 0.0));
    c.scale = static_cast<float>(Num(L, table, "scale", 1.0));
    c.bias = static_cast<float>(Num(L, table, "bias", 0.0));
    c.lo = static_cast<float>(Num(L, table, "lo", 0.0));
    c.hi = static_cast<float>(Num(L, table, "hi", 1.0));
    c.invert = Flag(L, table, "invert");
    c.center = Flag(L, table, "center");
    return c;
  }

  // ---- files ------------------------------------------------------------------------------------------------------------------------------

  /** Decodes level `minSide`-picked of an already parsed DDS (`minSide` > 0 picks the smallest mip level whose shorter side is at least that: a big texture reduced to a small tile does not need
   *  its base level). */
  bool DecodeData(const std::string& data, const dds::Header& header, const int minSide, dds::Image& out, const std::string& path, std::string& why)
  {
    const int level = minSide > 0 ? dds::PickLevel(header, minSide) : 0;
    if (!dds::DecodeLevel(data, header, level, out.px, out.w, out.h) && (level == 0 || !dds::DecodeLevel(data, header, 0, out.px, out.w, out.h))) {
      why = "the DDS is damaged: " + path;
      return false;
    }
    return true;
  }

  /** Reads and decodes a DDS file. */
  bool LoadDds(const std::string& path, const int minSide, dds::Image& out, std::string& why)
  {
    std::string data;
    dds::Header header;
    if (!moho::edicon::ReadAllBytes(path, data)) {
      why = "cannot read " + path;
      return false;
    }
    if (!dds::ParseHeader(data, header)) {
      why = "not a DDS this editor can read (DXT1 / DXT3 / DXT5 / 32 bit): " + path;
      return false;
    }
    return DecodeData(data, header, minSide, out, path, why);
  }

  void CreateParents(const std::string& path)
  {
    for (std::size_t i = 0; i < path.size(); ++i) {
      if ((path[i] == '/' || path[i] == '\\') && i > 2) {
        CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
      }
    }
  }

  bool WriteBytes(const std::string& path, const std::string& bytes, std::string& why)
  {
    CreateParents(path);
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
      why = "cannot create " + path;
      return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != bytes.size()) {
      why = "writing failed: " + path;
      return false;
    }
    return true;
  }

  // ---- ed_tex_convert ---------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_TexConvert(lua_State* const L)
  {
    const char* const srcPath = lua_tostring(L, 1);
    const char* const dstPath = lua_tostring(L, 2);
    if (srcPath == nullptr || dstPath == nullptr) {
      return Fail(L, "ed_tex_convert(src, dst, spec): bad arguments");
    }
    const bool hasSpec = lua_type(L, 3) == LUA_TTABLE;
    const int spec = hasSpec ? 3 : 0;

    dds::Image src;
    std::string data;
    dds::Header header;
    std::string why;
    // the colour and size of the result decide whether the file can be edited block by block: read the channels first
    tc::Chan chans[4];
    bool wantsAtlas[4] = {false, false, false, false};
    AtlasRef atlasRef[4];
    static const char* const names[4] = {"r", "g", "b", "a"};
    for (int c = 0; c < 4; ++c) {
      if (hasSpec && Push(L, spec, names[c]) == LUA_TTABLE) {
        chans[c] = ReadChan(L, lua_gettop(L), wantsAtlas[c], atlasRef[c]);
      }
      if (hasSpec) {
        lua_settop(L, -2);
      }
    }
    int width = 0;
    int height = 0;
    int levels = 32;
    if (hasSpec) {
      const int side = static_cast<int>(Num(L, spec, "size", 0));
      width = static_cast<int>(Num(L, spec, "width", side));
      height = static_cast<int>(Num(L, spec, "height", side));
      levels = static_cast<int>(Num(L, spec, "mips", 32));
    }

    if (!moho::edicon::ReadAllBytes(srcPath, data) || !dds::ParseHeader(data, header)) {
      return Fail(L, std::string("cannot read the DDS ") + srcPath);
    }
    if (width < 1 || height < 1) {
      width = header.w;
      height = header.h;
    }
    if (width > 4096 || height > 4096) {
      return Fail(L, "the size is at most 4096");
    }

    // alpha only: the colour blocks stay as they are
    std::uint8_t lut[256];
    const bool colourKept = chans[0].from == tc::From::Keep && chans[1].from == tc::From::Keep && chans[2].from == tc::From::Keep;
    if (header.format == dds::Format::Dxt5 && width == header.w && height == header.h && colourKept && !wantsAtlas[3] && tc::AlphaTable(chans[3], lut) && levels >= dds::Levels(header)) {
      std::string out = data;
      if (dds::Dxt5MapAlpha(out, lut)) {
        if (!WriteBytes(dstPath, out, why)) {
          return Fail(L, why);
        }
        lua_pushnumber(L, static_cast<lua_Number>(out.size()));
        lua_pushnumber(L, width);
        lua_pushnumber(L, height);
        lua_pushboolean(L, 1);
        return 4;
      }
    }

    if (!DecodeData(data, header, 0, src, srcPath, why)) {
      return Fail(L, why);
    }
    // planes from atlases
    std::vector<tc::Plane> external;
    external.reserve(4);
    for (int c = 0; c < 4; ++c) {
      if (!wantsAtlas[c]) {
        continue;
      }
      dds::Image atlas;
      if (!LoadDds(atlasRef[c].path, 0, atlas, why)) {
        return Fail(L, why);
      }
      external.push_back(tc::AtlasTilePlane(atlas, std::max(1, std::min(8, atlasRef[c].tile)) - 1, atlasRef[c].channel, width, height));
      chans[c].from = tc::From::Ext;
      chans[c].ext = &external.back();
    }
    const dds::Image out = tc::Convert(src, width, height, chans);
    const std::string bytes = dds::EncodeDxt5(out, std::max(1, levels));
    if (!WriteBytes(dstPath, bytes, why)) {
      return Fail(L, why);
    }
    lua_pushnumber(L, static_cast<lua_Number>(bytes.size()));
    lua_pushnumber(L, width);
    lua_pushnumber(L, height);
    lua_pushboolean(L, 0);
    return 4;
  }

  // ---- ed_tex_atlas -----------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_TexAtlas(lua_State* const L)
  {
    const char* const dstPath = lua_tostring(L, 1);
    if (dstPath == nullptr || lua_type(L, 2) != LUA_TTABLE) {
      return Fail(L, "ed_tex_atlas(dst, spec): bad arguments");
    }
    const int spec = 2;
    int size = static_cast<int>(Num(L, spec, "size", 1024));
    size = std::max(64, std::min(4096, size)) / 8 * 8;
    const float roughDefault = static_cast<float>(Num(L, spec, "rough", 0.65));
    const float heightDefault = static_cast<float>(Num(L, spec, "height", 0.5));
    const int levels = static_cast<int>(Num(L, spec, "mips", 32));
    const int D = size / 4;

    std::vector<tc::Plane> rough(tc::kAtlasTiles);
    std::vector<tc::Plane> height(tc::kAtlasTiles);
    std::vector<std::string> report(tc::kAtlasTiles, "no texture");
    if (Push(L, spec, "tiles") == LUA_TTABLE) {
      const int tiles = lua_gettop(L);
      for (int k = 0; k < tc::kAtlasTiles; ++k) {
        lua_rawgeti(L, tiles, k + 1);
        if (lua_type(L, -1) == LUA_TTABLE) {
          const int tile = lua_gettop(L);
          const std::string path = Str(L, tile, "src");
          bool unusedAtlas = false;
          AtlasRef unusedRef;
          tc::Chan chanRough;
          tc::Chan chanHeight;
          if (Push(L, tile, "rough") == LUA_TTABLE) chanRough = ReadChan(L, lua_gettop(L), unusedAtlas, unusedRef);
          lua_settop(L, -2);
          if (Push(L, tile, "height") == LUA_TTABLE) chanHeight = ReadChan(L, lua_gettop(L), unusedAtlas, unusedRef);
          lua_settop(L, -2);
          dds::Image image;
          std::string why;
          const bool loaded = !path.empty() && LoadDds(path, D * 2, image, why);
          if (loaded) {
            report[k] = "ok";
          } else if (!path.empty()) {
            report[k] = "cannot read: " + why;
          }
          // a channel that reads the texture needs it; a constant does not (and a tile whose texture is missing keeps the defaults for the rest)
          tc::Plane p;
          if ((loaded || chanRough.from == tc::From::Const) && tc::MakePlane(chanRough, image.px, image.w, image.h, D, D, p)) {
            rough[k] = p;
          }
          if ((loaded || chanHeight.from == tc::From::Const) && tc::MakePlane(chanHeight, image.px, image.w, image.h, D, D, p)) {
            height[k] = p;
          }
        }
        lua_settop(L, -2);
      }
    }
    lua_settop(L, -2);

    const dds::Image atlas = tc::BuildAtlas(size, rough.data(), height.data(), roughDefault, heightDefault);
    const std::string bytes = dds::EncodeDxt5(atlas, std::max(1, levels));
    std::string why;
    if (!WriteBytes(dstPath, bytes, why)) {
      return Fail(L, why);
    }
    lua_pushnumber(L, static_cast<lua_Number>(bytes.size()));
    lua_newtable(L);
    for (int k = 0; k < tc::kAtlasTiles; ++k) {
      lua_pushnumber(L, static_cast<lua_Number>(k + 1));
      lua_pushstring(L, report[static_cast<std::size_t>(k)].c_str());
      lua_settable(L, -3);
    }
    return 2;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& UserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  struct EdTexConvLuaBootstrap
  {
    EdTexConvLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_tex_convert", &cfunc_TexConvert, nullptr, "<global>", "ed_tex_convert(src, dst, spec) -> bytes, width, height, lossless | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_tex_atlas", &cfunc_TexAtlas, nullptr, "<global>", "ed_tex_atlas(dst, spec) -> bytes, report | nil, message");
    }
  };

  const EdTexConvLuaBootstrap gEdTexConvLuaBootstrap{};
} // namespace
