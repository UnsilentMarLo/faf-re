// Editor natives for the map's lighting, fog and water settings (the numbers the .scmap carries besides heights and strata):
//
//   ed_world_get()          -> table name -> number
//   ed_world_set(t[, refresh]) -> true | nil   applies the numbers that are present in `t`; `refresh` also rebuilds the water map
//                                              (needed after the water elevations or the water switch changed)
//
// Names (every value is a float, vectors are split into _x _y _z / _r _g _b lanes):
//   light_mult, sun_x/y/z (direction), ambient_r/g/b, sun_r/g/b (colour), fill_r/g/b (shadow fill), spec_r/g/b/a, bloom,
//   fog_r/g/b (colour), fog_start, fog_end,
//   water_on (0 / 1), water_elev, water_deep, water_abyss,
//   w_color_r/g/b, w_lerp_0/1, w_refraction, w_fresnel_bias, w_fresnel_power, w_unit_refl, w_sky_refl,
//   w_repeat_0..3, w_shininess, w_strength, w_sundir_x/y/z, w_suncol_r/g/b, w_sun_refl, w_glow
//
// The engine setters are plain stores; the renderers read the values every frame, so a change shows at once. The writer (CWldTerrainRes::Save)
// writes the same fields, so what is set here is what the saved map carries.

#include <cstdint>
#include <string>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"
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
    const char* name;
    float* ptr;
  };

  void Lanes(std::vector<Field>& out, const char* const* names, float* base, const int count)
  {
    for (int i = 0; i < count; ++i) {
      out.push_back({names[i], base + i});
    }
  }

  // Every float the panels edit, by name.
  std::vector<Field> Fields(moho::CWldTerrainRes& t)
  {
    std::vector<Field> f;
    f.push_back({"light_mult", &t.mLightingMultiplier});
    static const char* const sun[3] = {"sun_x", "sun_y", "sun_z"};
    static const char* const amb[3] = {"ambient_r", "ambient_g", "ambient_b"};
    static const char* const col[3] = {"sun_r", "sun_g", "sun_b"};
    static const char* const fill[3] = {"fill_r", "fill_g", "fill_b"};
    static const char* const spec[4] = {"spec_r", "spec_g", "spec_b", "spec_a"};
    Lanes(f, sun, reinterpret_cast<float*>(&t.mSunDirection), 3);
    Lanes(f, amb, reinterpret_cast<float*>(&t.mSunAmbience), 3);
    Lanes(f, col, reinterpret_cast<float*>(&t.mSunColor), 3);
    Lanes(f, fill, reinterpret_cast<float*>(&t.mShadowFillColor), 3);
    Lanes(f, spec, reinterpret_cast<float*>(&t.mSpecularColor), 4);
    f.push_back({"bloom", &t.mBloom});
    // The header names these lanes after a distance / clamp / curve model, but the values of a stock map are 0.8 0.8 0.8 1000 2000: the .scmap's
    // fog colour (r, g, b), fog start and fog end.
    f.push_back({"fog_r", &t.mFogStartDistance});
    f.push_back({"fog_g", &t.mFogCutoffDistance});
    f.push_back({"fog_b", &t.mFogMinClamp});
    f.push_back({"fog_start", &t.mFogMaxClamp});
    f.push_back({"fog_end", &t.mFogCurveExponent});
    if (t.mMap != nullptr) {
      f.push_back({"water_elev", &t.mMap->mWaterElevation});
      f.push_back({"water_deep", &t.mMap->mWaterElevationDeep});
      f.push_back({"water_abyss", &t.mMap->mWaterElevationAbyss});
    }
    moho::WaterShaderNumericState& w = t.mWaterShaderProperties.mNumericState;
    static const char* const wcol[3] = {"w_color_r", "w_color_g", "w_color_b"};
    static const char* const wlerp[2] = {"w_lerp_0", "w_lerp_1"};
    static const char* const wrep[4] = {"w_repeat_0", "w_repeat_1", "w_repeat_2", "w_repeat_3"};
    static const char* const wsd[3] = {"w_sundir_x", "w_sundir_y", "w_sundir_z"};
    static const char* const wsc[3] = {"w_suncol_r", "w_suncol_g", "w_suncol_b"};
    Lanes(f, wcol, w.mWaterColor, 3);
    Lanes(f, wlerp, w.mWaterLerp, 2);
    f.push_back({"w_refraction", &w.mRefractionScale});
    f.push_back({"w_fresnel_bias", &w.mFresnelBias});
    f.push_back({"w_fresnel_power", &w.mFresnelPower});
    f.push_back({"w_unit_refl", &w.mUnitReflectionAmount});
    f.push_back({"w_sky_refl", &w.mSkyReflectionAmount});
    Lanes(f, wrep, w.mNormalRepeatRate, 4);
    f.push_back({"w_shininess", &w.mSunShininess});
    f.push_back({"w_strength", &w.mSunStrength});
    Lanes(f, wsd, w.mSunDirection, 3);
    Lanes(f, wsc, w.mSunColor, 3);
    f.push_back({"w_sun_refl", &w.mSunReflectionAmount});
    f.push_back({"w_glow", &w.mSunGlow});
    return f;
  }

  void Put(lua_State* const L, const char* const key, const double value)
  {
    lua_pushstring(L, key);
    lua_pushnumber(L, static_cast<lua_Number>(value));
    lua_settable(L, -3);
  }

  int __cdecl cfunc_EdWorldGet(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    if (t == nullptr || t->mMap == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    lua_newtable(L);
    for (const Field& field : Fields(*t)) {
      Put(L, field.name, *field.ptr);
    }
    Put(L, "water_on", t->mMap->mWaterEnabled != 0 ? 1.0 : 0.0);
    return 1;
  }

  int __cdecl cfunc_EdWorldSet(lua_State* const L)
  {
    moho::CWldTerrainRes* const t = Terrain();
    luaL_checktype(L, 1, LUA_TTABLE);
    if (t == nullptr || t->mMap == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    for (const Field& field : Fields(*t)) {
      lua_pushstring(L, field.name);
      lua_gettable(L, 1);
      if (lua_type(L, -1) == LUA_TNUMBER) {
        *field.ptr = static_cast<float>(lua_tonumber(L, -1));
      }
      lua_settop(L, -2);
    }
    lua_pushstring(L, "water_on");
    lua_gettable(L, 1);
    if (lua_type(L, -1) == LUA_TNUMBER) {
      t->mMap->mWaterEnabled = lua_tonumber(L, -1) != 0.0 ? 1u : 0u;
    }
    lua_settop(L, -2);
    if (lua_toboolean(L, 2) != 0) {
      t->UpdateWaterMap();
    }
    lua_pushboolean(L, 1);
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

  struct EdWorldLuaBootstrap
  {
    EdWorldLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_world_get", &cfunc_EdWorldGet, nullptr, "<global>", "ed_world_get() -> table of lighting / fog / water numbers");
      (void)new moho::CScrLuaBinder(set, "ed_world_set", &cfunc_EdWorldSet, nullptr, "<global>", "ed_world_set(table[, refreshWater]) -> true");
    }
  };

  const EdWorldLuaBootstrap gEdWorldLuaBootstrap{};
} // namespace
