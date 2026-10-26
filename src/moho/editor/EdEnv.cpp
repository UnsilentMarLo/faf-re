// Editor natives for the parts of the environment EdWorld.cpp does not cover: the sky dome, the wave textures and movement of the water, the sky cube / background / environment cube
// lookups and the strategic (cartographic) colours. All of it lives in the terrain resource and is written to the .scmap by the engine's own saver (CWldTerrainRes::Save: the sky block,
// the water block, the cube maps), so what is set here is what a saved map carries.
//
//   ed_env_get()                   -> table name -> number
//   ed_env_set(t[, reset])         -> true | nil     numbers present in `t`; `reset` (default true) also rebuilds the sky dome's GPU resources
//   ed_env_texts()                 -> table name -> path (strings)
//   ed_env_set_texts(t)            -> true | nil     texture paths present in `t`; the owners reload their textures
//   ed_env_sky_decals()            -> flat list, 10 numbers per sky billboard (sun / moon / planets): x, y, z, rotation, width, height, u, v, du, dv
//   ed_env_set_sky_decals(list)    -> number of billboards set (at most 1024: the engine's buffer)
//   ed_env_lookups()               -> flat list key, path, key, path ... (the environment cube maps meshes reflect: "<default>" and per environment)
//   ed_env_set_lookups(list)       -> number of pairs set
//
// Numbers (floats unless noted):
//   dome_ox / oy / oz (centre), dome_height, dome_radius, dome_start (start angle), dome_w, dome_h (tessellation), horizon_size, horizon_r/g/b, sky_r/g/b, horizon_blend (the glow of the
//   sky billboards), cirrus_mult, cirrus_r/g/b, cirrus1..4_fx, _fz (frequency), _speed, _dx, _dz (direction), sky_decal_count (read only),
//   w_move1..4_x, _z (wave movement of the four normal maps), topo_samples, imager_offset, hypso0..4_r/g/b/a (0..1)
// Texts:
//   sky_billboards (albedo atlas), sky_glow (glow atlas), sky_cumulus1..3, sky_cirrus, skycube, background, water_cubemap, water_ramp, water_wave1..4

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/editor/EdEnv.h"
#include "moho/render/SkyDome.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"
#include "moho/terrain/splat/CWldSplat.h"
#include "moho/terrain/water/CWaterShaderProperties.h"

namespace
{
  moho::CWldTerrainRes* Terrain()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr || session->mWldMap->mTerrainRes == nullptr) {
      return nullptr;
    }
    return static_cast<moho::CWldTerrainRes*>(session->mWldMap->mTerrainRes);
  }

  struct Field
  {
    std::string name;
    float* ptr;
  };

  void Lane(std::vector<Field>& out, const std::string& name, float* const ptr)
  {
    out.push_back({name, ptr});
  }

  std::string Numbered(const char* const stem, const int n, const char* const tail)
  {
    return std::string(stem) + std::to_string(n) + tail;
  }

  // The float lanes by name.
  std::vector<Field> Fields(moho::CWldTerrainRes& t)
  {
    std::vector<Field> f;
    moho::SkyDome& sky = t.mSkyDome;
    Lane(f, "dome_ox", &sky.mDomeOrigin.x);
    Lane(f, "dome_oy", &sky.mDomeOrigin.y);
    Lane(f, "dome_oz", &sky.mDomeOrigin.z);
    Lane(f, "dome_height", &sky.mDomeShapeParams.x);
    Lane(f, "dome_radius", &sky.mDomeShapeParams.y);
    Lane(f, "dome_start", &sky.mDomeShapeParams.z);
    Lane(f, "horizon_size", &sky.mHorizonSize);
    Lane(f, "horizon_r", &sky.mHorizonColor.x);
    Lane(f, "horizon_g", &sky.mHorizonColor.y);
    Lane(f, "horizon_b", &sky.mHorizonColor.z);
    Lane(f, "sky_r", &sky.mSkyColor.x);
    Lane(f, "sky_g", &sky.mSkyColor.y);
    Lane(f, "sky_b", &sky.mSkyColor.z);
    Lane(f, "horizon_blend", &sky.mHorizonBlend);
    Lane(f, "cirrus_mult", &sky.mCirrusMultiplier);
    Lane(f, "cirrus_r", &sky.mCirrusColor_R);
    Lane(f, "cirrus_g", &sky.mCirrusColor_G);
    Lane(f, "cirrus_b", &sky.mCirrusColor_B);
    float* const layers = reinterpret_cast<float*>(sky.mCirrusData);        // 4 records of 5 floats: frequency (2), speed, direction (2)
    static const char* const lane[5] = {"_fx", "_fz", "_speed", "_dx", "_dz"};
    for (int i = 0; i < 4; ++i) {
      for (int k = 0; k < 5; ++k) {
        Lane(f, Numbered("cirrus", i + 1, lane[k]), layers + i * 5 + k);
      }
    }
    moho::WaterShaderNumericState& w = t.mWaterShaderProperties.mNumericState;
    float* const moves[4] = {w.mNormal1Movement, w.mNormal2Movement, w.mNormal3Movement, w.mNormal4Movement};
    for (int i = 0; i < 4; ++i) {
      Lane(f, Numbered("w_move", i + 1, "_x"), moves[i]);
      Lane(f, Numbered("w_move", i + 1, "_z"), moves[i] + 1);
    }
    return f;
  }

  struct TextField
  {
    const char* name;
    msvc8::string* str;
    int owner;      // 0 sky, 1 water, 2 terrain (skycube / background: through their setters)
  };

  std::vector<TextField> Texts(moho::CWldTerrainRes& t)
  {
    std::vector<TextField> f;
    moho::SkyDome& sky = t.mSkyDome;
    f.push_back({"sky_billboards", &sky.mAtmosphereTexPath, 0});
    f.push_back({"sky_glow", &sky.mAtmosphereTexPath2, 0});
    f.push_back({"sky_cumulus1", &sky.mDecalTexPath1, 0});
    f.push_back({"sky_cumulus2", &sky.mDecalTexPath2, 0});
    f.push_back({"sky_cumulus3", &sky.mDecalTexPath3, 0});
    f.push_back({"sky_cirrus", &sky.mCirrusTexPath, 0});
    moho::CWaterShaderProperties& w = t.mWaterShaderProperties;
    f.push_back({"water_cubemap", &w.mWaterCubemap, 1});
    f.push_back({"water_ramp", &w.mWaterRamp, 1});
    static const char* const waves[4] = {"water_wave1", "water_wave2", "water_wave3", "water_wave4"};
    for (int i = 0; i < 4; ++i) {
      f.push_back({waves[i], &w.mShaderNames[i], 1});
    }
    return f;
  }

  void Put(lua_State* const L, const char* const key, const double value)
  {
    lua_pushstring(L, key);
    lua_pushnumber(L, static_cast<lua_Number>(value));
    lua_settable(L, -3);
  }

  void PutText(lua_State* const L, const char* const key, const std::string& value)
  {
    lua_pushstring(L, key);
    lua_pushstring(L, value.c_str());
    lua_settable(L, -3);
  }

  int __cdecl cfunc_Get(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    if (t == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    lua_newtable(L);
    for (const Field& f : Fields(*t)) {
      Put(L, f.name.c_str(), *f.ptr);
    }
    Put(L, "dome_w", t->mSkyDome.mWidth);
    Put(L, "dome_h", t->mSkyDome.mHeight);
    Put(L, "sky_decal_count", static_cast<double>(t->mSkyDome.mDecalUploads.size()));
    Put(L, "topo_samples", t->GetTopographicSamples());
    Put(L, "imager_offset", t->GetImagerElevationOffset());
    for (int i = 0; i < 5; ++i) {
      const std::uint32_t c = t->GetHypsometricColor(i);
      Put(L, Numbered("hypso", i, "_a").c_str(), static_cast<double>((c >> 24) & 255u) / 255.0);
      Put(L, Numbered("hypso", i, "_r").c_str(), static_cast<double>((c >> 16) & 255u) / 255.0);
      Put(L, Numbered("hypso", i, "_g").c_str(), static_cast<double>((c >> 8) & 255u) / 255.0);
      Put(L, Numbered("hypso", i, "_b").c_str(), static_cast<double>(c & 255u) / 255.0);
    }
    return 1;
  }

  bool Has(lua_State* const L, const int table, const char* const key, double& value)
  {
    lua_pushstring(L, key);
    lua_gettable(L, table);
    const bool ok = lua_type(L, -1) == LUA_TNUMBER;
    if (ok) {
      value = lua_tonumber(L, -1);
    }
    lua_settop(L, -2);
    return ok;
  }

  std::uint32_t Byte(const double v)
  {
    return static_cast<std::uint32_t>(std::max(0.0, std::min(255.0, v * 255.0 + 0.5)));
  }

  int __cdecl cfunc_Set(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    luaL_checktype(L, 1, LUA_TTABLE);
    if (t == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    double v = 0.0;
    for (const Field& f : Fields(*t)) {
      if (Has(L, 1, f.name.c_str(), v)) {
        *f.ptr = static_cast<float>(v);
      }
    }
    bool shape = false;
    if (Has(L, 1, "dome_w", v)) {
      t->mSkyDome.mWidth = std::max(3, std::min(128, static_cast<int>(v)));
      shape = true;
    }
    if (Has(L, 1, "dome_h", v)) {
      t->mSkyDome.mHeight = std::max(2, std::min(64, static_cast<int>(v)));
      shape = true;
    }
    if (Has(L, 1, "topo_samples", v)) {
      t->SetTopographicSamples(std::max(0, std::min(256, static_cast<int>(v))));
    }
    if (Has(L, 1, "imager_offset", v)) {
      t->SetImagerElevationOffset(static_cast<float>(v));
    }
    for (int i = 0; i < 5; ++i) {
      const std::uint32_t old = t->GetHypsometricColor(i);
      std::uint32_t a = (old >> 24) & 255u;
      std::uint32_t r = (old >> 16) & 255u;
      std::uint32_t g = (old >> 8) & 255u;
      std::uint32_t b = old & 255u;
      bool any = false;
      if (Has(L, 1, Numbered("hypso", i, "_a").c_str(), v)) { a = Byte(v); any = true; }
      if (Has(L, 1, Numbered("hypso", i, "_r").c_str(), v)) { r = Byte(v); any = true; }
      if (Has(L, 1, Numbered("hypso", i, "_g").c_str(), v)) { g = Byte(v); any = true; }
      if (Has(L, 1, Numbered("hypso", i, "_b").c_str(), v)) { b = Byte(v); any = true; }
      if (any) {
        t->SetHypsometricColor(i, (a << 24) | (r << 16) | (g << 8) | b);
      }
    }
    // the dome's vertex buffer is built from the shape numbers: a changed shape needs the GPU resources rebuilt (colours are read every frame)
    if (shape || (lua_type(L, 2) != LUA_TNIL && lua_toboolean(L, 2) != 0)) {
      t->mSkyDome.Reset();
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_Texts(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    if (t == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    lua_newtable(L);
    for (const TextField& f : Texts(*t)) {
      PutText(L, f.name, std::string(f.str->c_str()));
    }
    PutText(L, "skycube", std::string(t->GetSkycubeFile().c_str()));
    PutText(L, "background", std::string(t->GetBackgroundFile().c_str()));
    return 1;
  }

  int __cdecl cfunc_SetTexts(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    luaL_checktype(L, 1, LUA_TTABLE);
    if (t == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    bool sky = false;
    bool water = false;
    for (const TextField& f : Texts(*t)) {
      lua_pushstring(L, f.name);
      lua_gettable(L, 1);
      if (lua_type(L, -1) == LUA_TSTRING) {
        f.str->assign(lua_tostring(L, -1));
        (f.owner == 0 ? sky : water) = true;
      }
      lua_settop(L, -2);
    }
    lua_pushstring(L, "skycube");
    lua_gettable(L, 1);
    if (lua_type(L, -1) == LUA_TSTRING) {
      t->SetSkycube(msvc8::string(lua_tostring(L, -1)));
    }
    lua_settop(L, -2);
    lua_pushstring(L, "background");
    lua_gettable(L, 1);
    if (lua_type(L, -1) == LUA_TSTRING) {
      t->SetBackground(msvc8::string(lua_tostring(L, -1)));
    }
    lua_settop(L, -2);
    if (sky) {
      t->mSkyDome.Reset();           // releases every texture handle: the next frame loads the paths again
    }
    if (water) {
      t->mWaterShaderProperties.releaseTextures();
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_SkyDecals(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    if (t == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    lua_newtable(L);
    int index = 1;
    for (const moho::SkyDomeDecalVertices& d : t->mSkyDome.mDecalUploads) {
      float v[10];
      std::memcpy(v, d.mBytes, sizeof(v));
      for (int k = 0; k < 10; ++k, ++index) {
        lua_pushnumber(L, static_cast<lua_Number>(index));
        lua_pushnumber(L, static_cast<lua_Number>(v[k]));
        lua_settable(L, -3);
      }
    }
    return 1;
  }

  int __cdecl cfunc_SetSkyDecals(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    luaL_checktype(L, 1, LUA_TTABLE);
    if (t == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    std::vector<float> values;
    for (int i = 1; i <= 10 * 1024; ++i) {
      lua_rawgeti(L, 1, i);
      if (lua_type(L, -1) != LUA_TNUMBER) {
        lua_settop(L, -2);
        break;
      }
      values.push_back(static_cast<float>(lua_tonumber(L, -1)));
      lua_settop(L, -2);
    }
    t->mSkyDome.mDecalUploads.clear();
    int count = 0;
    for (std::size_t i = 0; i + 10 <= values.size(); i += 10) {
      moho::SkyDomeDecalVertices d{};
      std::memcpy(d.mBytes, &values[i], 40);
      t->mSkyDome.mDecalUploads.push_back(d);
      ++count;
    }
    t->mSkyDome.mNeedsRebuild = true;
    lua_pushnumber(L, count);
    return 1;
  }

  int __cdecl cfunc_Lookups(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    if (t == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    moho::TerrainEnvironmentLookupPairs pairs;
    t->EnumerateEnvLookup(pairs);
    lua_newtable(L);
    int index = 1;
    for (const auto& pair : pairs) {
      lua_pushnumber(L, static_cast<lua_Number>(index++));
      lua_pushstring(L, pair.first.c_str());
      lua_settable(L, -3);
      lua_pushnumber(L, static_cast<lua_Number>(index++));
      lua_pushstring(L, pair.second.c_str());
      lua_settable(L, -3);
    }
    return 1;
  }

  int __cdecl cfunc_SetLookups(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    luaL_checktype(L, 1, LUA_TTABLE);
    if (t == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    std::vector<std::string> items;
    for (int i = 1; i <= 512; ++i) {
      lua_rawgeti(L, 1, i);
      if (lua_type(L, -1) != LUA_TSTRING) {
        lua_settop(L, -2);
        break;
      }
      items.emplace_back(lua_tostring(L, -1));
      lua_settop(L, -2);
    }
    t->ClearEnvLookup();
    int count = 0;
    for (std::size_t i = 0; i + 1 < items.size(); i += 2) {
      t->AddEnvLookup(msvc8::string(items[i].c_str()), msvc8::string(items[i + 1].c_str()));
      ++count;
    }
    lua_pushnumber(L, count);
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

  struct EdEnvLuaBootstrap
  {
    EdEnvLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      const auto bind = [&](const char* name, int(__cdecl* fn)(lua_State*), const char* doc) { (void)new moho::CScrLuaBinder(set, name, fn, nullptr, "<global>", doc); };
      bind("ed_env_get", &cfunc_Get, "ed_env_get() -> table of sky / water motion / imager numbers");
      bind("ed_env_set", &cfunc_Set, "ed_env_set(table[, reset]) -> true");
      bind("ed_env_texts", &cfunc_Texts, "ed_env_texts() -> table of sky / water / sky cube texture paths");
      bind("ed_env_set_texts", &cfunc_SetTexts, "ed_env_set_texts(table) -> true");
      bind("ed_env_sky_decals", &cfunc_SkyDecals, "ed_env_sky_decals() -> flat list, 10 numbers per sky billboard");
      bind("ed_env_set_sky_decals", &cfunc_SetSkyDecals, "ed_env_set_sky_decals(list) -> count");
      bind("ed_env_lookups", &cfunc_Lookups, "ed_env_lookups() -> flat list key, path ...");
      bind("ed_env_set_lookups", &cfunc_SetLookups, "ed_env_set_lookups(list) -> count");
    }
  };

  const EdEnvLuaBootstrap gEdEnvLuaBootstrap{};
} // namespace

namespace moho::editor
{
  namespace
  {
    std::uint8_t* MaskPtr(moho::CWldTerrainRes& t, const int kind)
    {
      return kind == 0 ? t.GetWaterFoam() : (kind == 1 ? t.GetWaterFlatness() : t.GetWaterDepthBias());
    }

    bool MaskSize(moho::CWldTerrainRes& t, int& w, int& h)
    {
      if (t.mMap == nullptr || t.mMap->mHeightField.get() == nullptr) {
        return false;
      }
      w = (t.mMap->mHeightField->width - 1) >> 1;
      h = (t.mMap->mHeightField->height - 1) >> 1;
      return w > 0 && h > 0;
    }
  } // namespace

  bool WaterMaskRead(const int kind, std::vector<std::uint8_t>& out, int& w, int& h)
  {
    moho::CWldTerrainRes* const t = Terrain();
    if (t == nullptr || kind < 0 || kind > 2 || !MaskSize(*t, w, h) || MaskPtr(*t, kind) == nullptr) {
      return false;
    }
    out.assign(MaskPtr(*t, kind), MaskPtr(*t, kind) + static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    return true;
  }

  bool WaterMaskWrite(const int kind, const std::uint8_t* const data, const int w, const int h)
  {
    moho::CWldTerrainRes* const t = Terrain();
    int mw = 0;
    int mh = 0;
    if (t == nullptr || kind < 0 || kind > 2 || !MaskSize(*t, mw, mh) || mw != w || mh != h || MaskPtr(*t, kind) == nullptr) {
      return false;
    }
    std::memcpy(MaskPtr(*t, kind), data, static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    t->UpdateWaterMap();
    if (moho::IDecalManager* const decals = t->GetDecalManager(); decals != nullptr) {
      decals->MarkPendingChanges();
    }
    return true;
  }
} // namespace moho::editor
