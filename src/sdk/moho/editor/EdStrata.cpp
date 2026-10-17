// Editor natives for the terrain strata (texture layers and their paint masks):
//
//   ed_stratum_info()                          -> maskWidth, maskHeight   (the two RGBA mask textures; 8 lanes = strata 0..7)
//   ed_stratum_layer(i)                        -> path, tileSize, tileCount   i = 0..9 albedo (lower, strata 0..7, upper), 10..19 normals
//   ed_stratum_set_layer(i, path, tileSize)    -> true | nil, message   loads the texture and applies the size against the map
//   ed_stratum_read(stratum, x0, z0, x1, z1)   -> values, x0, z0, x1, z1   mask bytes of the rectangle (clamped), row major
//   ed_stratum_write(stratum, x0, z0, x1, z1, values) -> number of cells written
//
// The masks are the engine's own dynamic textures. They are locked with D3DLOCK_DISCARD, so the engine's partial UpdateStratumMask / GetStratumMask
// cannot be used on a live texture: a copy of the eight masks is kept here (MaskShadow) and a change rewrites the whole mask texture it belongs
// to in one lock. The terrain renderer and the map writer read the same textures. Undo of mask edits is the Lua side's (editor/strata.lua).
//
//   ed_stratum_shader([name])                  -> terrain technique name; 'TTerrain' blends strata 0..3 only, 'TTerrainXP' all eight

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/render/textures/CD3DDynamicTextureSheet.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"
#include "moho/terrain/StratumMaterial.h"

namespace
{
  moho::IWldTerrainRes* Terrain()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr) {
      return nullptr;
    }
    return session->mWldMap->mTerrainRes;
  }

  moho::CStratumMaterial* LayerOf(moho::StratumMaterial& m, const int i)
  {
    moho::CStratumMaterial* const table[20] = {
      &m.mLowerAlbedoTexture,   &m.mStratum0AlbedoTexture, &m.mStratum1AlbedoTexture, &m.mStratum2AlbedoTexture,
      &m.mStratum3AlbedoTexture, &m.mStratum4AlbedoTexture, &m.mStratum5AlbedoTexture, &m.mStratum6AlbedoTexture,
      &m.mStratum7AlbedoTexture, &m.mUpperAlbedoTexture,   &m.mLowerNormalTexture,    &m.mStratum0NormalTexture,
      &m.mStratum1NormalTexture, &m.mStratum2NormalTexture, &m.mStratum3NormalTexture, &m.mStratum4NormalTexture,
      &m.mStratum5NormalTexture, &m.mStratum6NormalTexture, &m.mStratum7NormalTexture, &m.mUpperNormalTexture,
    };
    return (i >= 0 && i < 20) ? table[i] : nullptr;
  }

  int __cdecl cfunc_EdStratumInfo(lua_State* const L)
  {
    moho::IWldTerrainRes* const res = Terrain();
    if (res == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    const moho::StratumMaterial& m = res->GetStratumMaterial();
    lua_pushnumber(L, static_cast<lua_Number>(m.mStratumMaskWidth));
    lua_pushnumber(L, static_cast<lua_Number>(m.mStratumMaskHeight));
    return 2;
  }

  int __cdecl cfunc_EdStratumLayer(lua_State* const L)
  {
    moho::IWldTerrainRes* const res = Terrain();
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (res == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    const moho::CStratumMaterial* const layer = LayerOf(res->GetStratumMaterial(), i);
    if (layer == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushstring(L, layer->mPath.c_str());
    lua_pushnumber(L, static_cast<lua_Number>(layer->mSize));
    lua_pushnumber(L, static_cast<lua_Number>(layer->mScaleX));
    return 3;
  }

  int __cdecl cfunc_EdStratumSetLayer(lua_State* const L)
  {
    moho::IWldTerrainRes* const res = Terrain();
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    const char* const path = lua_tostring(L, 2);
    const float size = static_cast<float>(luaL_checknumber(L, 3));
    if (res == nullptr || path == nullptr || res->mMap == nullptr || res->mMap->mHeightField.get() == nullptr) {
      lua_pushnil(L);
      lua_pushstring(L, "no terrain");
      return 2;
    }
    moho::CStratumMaterial* const layer = LayerOf(res->GetStratumMaterial(), i);
    if (layer == nullptr || size <= 0.0f) {
      lua_pushnil(L);
      lua_pushstring(L, "bad layer or size");
      return 2;
    }
    layer->mPath.assign(path);
    layer->mSize = size;
    layer->mTextureSheet = {};      // load the new texture
    const Wm3::Vector2f maxSize{static_cast<float>(res->mMap->mHeightField->width - 1),
                                     static_cast<float>(res->mMap->mHeightField->height - 1)};
    moho::CStratumMaterial::SetSize(maxSize, *layer);
    lua_pushboolean(L, layer->mTextureSheet.px != nullptr ? 1 : 0);
    return 1;
  }

  // ed_stratum_shader([name]) -> the terrain technique name; with a name it is changed. 'TTerrain' blends strata 0..3 only (painted layers 1..4),
  // 'TTerrainXP' all eight.
  int __cdecl cfunc_EdStratumShader(lua_State* const L)
  {
    moho::IWldTerrainRes* const res = Terrain();
    if (res == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    moho::StratumMaterial& m = res->GetStratumMaterial();
    if (const char* const name = lua_tostring(L, 1); name != nullptr && name[0] != '\0') {
      m.mShaderName.assign(name);
    }
    lua_pushstring(L, m.mShaderName.c_str());
    return 1;
  }

  // The mask textures are locked with D3DLOCK_DISCARD (CD3DDynamicTextureSheet::Lock on a dynamic texture), so a partial update through
  // UpdateStratumMask leaves everything outside the rectangle undefined (it showed as diagonal stripes), and reading a lane back is not
  // reliable either. The masks are therefore kept here, one byte array per stratum, read from the texture once, and a change always writes
  // a whole mask texture (four lanes, one lock).
  struct MaskShadow
  {
    const void* terrain = nullptr;
    const void* sheet[2] = {nullptr, nullptr};
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> lane[8];
    bool loaded[2] = {false, false};
  };

  MaskShadow gShadow;

  moho::CD3DDynamicTextureSheet* MaskSheet(moho::StratumMaterial& m, const int which)
  {
    return reinterpret_cast<moho::CD3DDynamicTextureSheet*>(which != 0 ? m.mStratumMask1.px : m.mStratumMask0.px);
  }

  // The texture the engine holds must be exactly w x h (a map loaded from a file brings its own mask textures): a larger mask size than the texture
  // would write past the locked memory.
  bool SheetFits(moho::CD3DDynamicTextureSheet* const sheet, const int w, const int h)
  {
    Wm3::Vector3f dim{};
    if (sheet->GetDimensions(&dim) == nullptr) {
      return false;
    }
    return static_cast<int>(dim.x) == w && static_cast<int>(dim.y) == h;
  }

  // Makes the shadow of mask texture `which` (0: strata 0..3, 1: 4..7) valid. false when there is no mask.
  bool EnsureShadow(moho::IWldTerrainRes* const res, const int which)
  {
    moho::StratumMaterial& m = res->GetStratumMaterial();
    const int w = static_cast<int>(m.mStratumMaskWidth);
    const int h = static_cast<int>(m.mStratumMaskHeight);
    moho::CD3DDynamicTextureSheet* const sheet = MaskSheet(m, which);
    if (sheet == nullptr || w <= 0 || h <= 0 || !SheetFits(sheet, w, h)) {
      return false;
    }
    if (gShadow.terrain != res || gShadow.w != w || gShadow.h != h || gShadow.sheet[which] != sheet) {
      // another map, another size, or the engine replaced the texture (edit mode clones): the copy of this mask is stale
      if (gShadow.terrain != res || gShadow.w != w || gShadow.h != h) {
        gShadow = MaskShadow{};
        gShadow.terrain = res;
        gShadow.w = w;
        gShadow.h = h;
      }
      gShadow.loaded[which] = false;
      gShadow.sheet[which] = sheet;
    }
    if (gShadow.loaded[which]) {
      return true;
    }
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::uint32_t pitch = 0;
    void* bits = nullptr;
    if (!sheet->Lock(&pitch, &bits) || bits == nullptr) {
      return false;
    }
    if (pitch < static_cast<std::uint32_t>(w) * 4u) {
      (void)sheet->Unlock();
      return false;
    }
    for (int i = 0; i < 4; ++i) {
      gShadow.lane[which * 4 + i].assign(count, 0);
    }
    for (int y = 0; y < h; ++y) {
      const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(bits) + static_cast<std::size_t>(y) * pitch);
      for (int x = 0; x < w; ++x) {
        const std::uint32_t px = row[x];
        const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        gShadow.lane[which * 4 + 0][at] = static_cast<std::uint8_t>((px >> 16) & 0xFFu);
        gShadow.lane[which * 4 + 1][at] = static_cast<std::uint8_t>((px >> 8) & 0xFFu);
        gShadow.lane[which * 4 + 2][at] = static_cast<std::uint8_t>(px & 0xFFu);
        gShadow.lane[which * 4 + 3][at] = static_cast<std::uint8_t>((px >> 24) & 0xFFu);
      }
    }
    // the lock may have discarded the contents: write back exactly what was read
    for (int y = 0; y < h; ++y) {
      auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(bits) + static_cast<std::size_t>(y) * pitch);
      for (int x = 0; x < w; ++x) {
        const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        row[x] = (static_cast<std::uint32_t>(gShadow.lane[which * 4 + 3][at]) << 24) | (static_cast<std::uint32_t>(gShadow.lane[which * 4 + 0][at]) << 16) |
                 (static_cast<std::uint32_t>(gShadow.lane[which * 4 + 1][at]) << 8) | static_cast<std::uint32_t>(gShadow.lane[which * 4 + 2][at]);
      }
    }
    (void)sheet->Unlock();
    gShadow.loaded[which] = true;
    return true;
  }

  // Writes the whole mask texture `which` from the shadow (one lock, four lanes).
  void UploadShadow(moho::IWldTerrainRes* const res, const int which)
  {
    moho::StratumMaterial& m = res->GetStratumMaterial();
    moho::CD3DDynamicTextureSheet* const sheet = MaskSheet(m, which);
    const int w = gShadow.w;
    const int h = gShadow.h;
    std::uint32_t pitch = 0;
    void* bits = nullptr;
    if (sheet == nullptr || !SheetFits(sheet, w, h) || !sheet->Lock(&pitch, &bits) || bits == nullptr) {
      return;
    }
    if (pitch < static_cast<std::uint32_t>(w) * 4u) {
      (void)sheet->Unlock();
      return;
    }
    for (int y = 0; y < h; ++y) {
      auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(bits) + static_cast<std::size_t>(y) * pitch);
      for (int x = 0; x < w; ++x) {
        const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        row[x] = (static_cast<std::uint32_t>(gShadow.lane[which * 4 + 3][at]) << 24) | (static_cast<std::uint32_t>(gShadow.lane[which * 4 + 0][at]) << 16) |
                 (static_cast<std::uint32_t>(gShadow.lane[which * 4 + 1][at]) << 8) | static_cast<std::uint32_t>(gShadow.lane[which * 4 + 2][at]);
      }
    }
    (void)sheet->Unlock();
  }

  void ClampMask(const moho::StratumMaterial& m, int& x0, int& z0, int& x1, int& z1)
  {
    x0 = std::max(0, x0);
    z0 = std::max(0, z0);
    x1 = std::min(static_cast<int>(m.mStratumMaskWidth) - 1, x1);
    z1 = std::min(static_cast<int>(m.mStratumMaskHeight) - 1, z1);
  }

  int __cdecl cfunc_EdStratumRead(lua_State* const L)
  {
    moho::IWldTerrainRes* const res = Terrain();
    const int stratum = static_cast<int>(luaL_checknumber(L, 1));
    int x0 = static_cast<int>(luaL_checknumber(L, 2));
    int z0 = static_cast<int>(luaL_checknumber(L, 3));
    int x1 = static_cast<int>(luaL_checknumber(L, 4));
    int z1 = static_cast<int>(luaL_checknumber(L, 5));
    if (res == nullptr || stratum < 0 || stratum > 7) {
      lua_pushnil(L);
      return 1;
    }
    const moho::StratumMaterial& m = res->GetStratumMaterial();
    const int w = static_cast<int>(m.mStratumMaskWidth);
    const int h = static_cast<int>(m.mStratumMaskHeight);
    if (w <= 0 || h <= 0) {
      lua_pushnil(L);
      return 1;
    }
    ClampMask(m, x0, z0, x1, z1);
    lua_newtable(L);
    if (x0 > x1 || z0 > z1) {
      return 1;
    }
    if (!EnsureShadow(res, stratum / 4)) {
      lua_pushnil(L);
      return 1;
    }
    const std::vector<std::uint8_t>& full = gShadow.lane[stratum];
    int index = 1;
    for (int z = z0; z <= z1; ++z) {
      for (int x = x0; x <= x1; ++x, ++index) {
        lua_pushnumber(L, static_cast<lua_Number>(index));
        lua_pushnumber(L, static_cast<lua_Number>(full[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]));
        lua_settable(L, -3);
      }
    }
    lua_pushnumber(L, static_cast<lua_Number>(x0));
    lua_pushnumber(L, static_cast<lua_Number>(z0));
    lua_pushnumber(L, static_cast<lua_Number>(x1));
    lua_pushnumber(L, static_cast<lua_Number>(z1));
    return 5;
  }

  int __cdecl cfunc_EdStratumWrite(lua_State* const L)
  {
    moho::IWldTerrainRes* const res = Terrain();
    const int stratum = static_cast<int>(luaL_checknumber(L, 1));
    int x0 = static_cast<int>(luaL_checknumber(L, 2));
    int z0 = static_cast<int>(luaL_checknumber(L, 3));
    int x1 = static_cast<int>(luaL_checknumber(L, 4));
    int z1 = static_cast<int>(luaL_checknumber(L, 5));
    luaL_checktype(L, 6, LUA_TTABLE);
    if (res == nullptr || stratum < 0 || stratum > 7) {
      lua_pushnumber(L, 0);
      return 1;
    }
    const moho::StratumMaterial& m = res->GetStratumMaterial();
    const int w = static_cast<int>(m.mStratumMaskWidth);
    const int h = static_cast<int>(m.mStratumMaskHeight);
    // the table was read for exactly this rectangle: it must be inside the mask
    if (w <= 0 || h <= 0 || x0 > x1 || z0 > z1 || x0 < 0 || z0 < 0 || x1 >= w || z1 >= h) {
      lua_pushnumber(L, 0);
      return 1;
    }
    if (!EnsureShadow(res, stratum / 4)) {
      lua_pushnumber(L, 0);
      return 1;
    }
    std::vector<std::uint8_t>& full = gShadow.lane[stratum];
    int index = 1;
    for (int z = z0; z <= z1; ++z) {
      for (int x = x0; x <= x1; ++x, ++index) {
        lua_rawgeti(L, 6, index);
        const double value = lua_tonumber(L, -1);
        lua_settop(L, -2);
        full[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
          static_cast<std::uint8_t>(std::max(0.0, std::min(255.0, value)));
      }
    }
    UploadShadow(res, stratum / 4);
    lua_pushnumber(L, static_cast<lua_Number>(index - 1));
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

  struct EdStrataLuaBootstrap
  {
    EdStrataLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_stratum_info", &cfunc_EdStratumInfo, nullptr, "<global>", "ed_stratum_info() -> maskWidth, maskHeight");
      (void)new moho::CScrLuaBinder(set, "ed_stratum_layer", &cfunc_EdStratumLayer, nullptr, "<global>",
                                    "ed_stratum_layer(i) -> path, tileSize, tileCount (0..9 albedo, 10..19 normal)");
      (void)new moho::CScrLuaBinder(set, "ed_stratum_set_layer", &cfunc_EdStratumSetLayer, nullptr, "<global>",
                                    "ed_stratum_set_layer(i, path, tileSize) -> loaded | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_stratum_shader", &cfunc_EdStratumShader, nullptr, "<global>",
                                    "ed_stratum_shader([name]) -> terrain technique name (TTerrain: strata 0..3, TTerrainXP: all eight)");
      (void)new moho::CScrLuaBinder(set, "ed_stratum_read", &cfunc_EdStratumRead, nullptr, "<global>",
                                    "ed_stratum_read(stratum, x0, z0, x1, z1) -> values, x0, z0, x1, z1");
      (void)new moho::CScrLuaBinder(set, "ed_stratum_write", &cfunc_EdStratumWrite, nullptr, "<global>",
                                    "ed_stratum_write(stratum, x0, z0, x1, z1, values) -> cells written");
    }
  };

  const EdStrataLuaBootstrap gEdStrataLuaBootstrap{};
} // namespace
