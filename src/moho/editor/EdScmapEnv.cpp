// Editor native that reads the look of another map straight from its .scmap file on disk (no map is opened): lighting, water, strata, sky. It is what "import from a map" in the environment
// panels uses, and what the shipped preset / theme libraries are made from (editor/tools/gen_env_library.py reads the same blocks with the Python reader).
//
//   ed_scmap_env(path) -> table | nil, message       path of a .scmap file on disk (version 2, minor 53 - 60)
//
// The table uses the names of ed_world_get / ed_env_get (EdWorld.cpp, EdEnv.cpp) so that it can be applied as it is:
//   minor, width, height, water_on
//   light     light_mult, sun_x/y/z, ambient_r/g/b, sun_r/g/b, fill_r/g/b, spec_r/g/b/a, bloom, fog_r/g/b, fog_start, fog_end
//   water     water_elev, water_deep, water_abyss, w_color_r/g/b, w_lerp_0/1, w_refraction, w_fresnel_bias, w_fresnel_power, w_unit_refl, w_sky_refl, w_shininess, w_strength, w_sundir_x/y/z,
//             w_suncol_r/g/b, w_sun_refl, w_glow, w_repeat_0..3, w_move1..4_x / _z
//   wtex      water_cubemap, water_ramp, water_wave1..4
//   strata    list of 10 { albedo, size } (Base, strata 1..8, Upper) and normals: list of 9 { path, size } (Base, strata 1..8)
//   sky       (minor 60 only) the ed_env_get names; skytex (paths), billboards (flat list of 10 numbers each)
//   lookups   flat list key, path ...;  skycube, background

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace
{
  struct Reader
  {
    FILE* f = nullptr;
    bool ok = true;

    template <class T>
    T Take()
    {
      T v{};
      if (!ok || std::fread(&v, sizeof(T), 1, f) != 1) {
        ok = false;
        return T{};
      }
      return v;
    }

    float F() { return Take<float>(); }
    std::int32_t I() { return Take<std::int32_t>(); }
    std::uint8_t B() { return Take<std::uint8_t>(); }

    std::string S()
    {
      std::string s;
      while (ok) {
        const int c = std::fgetc(f);
        if (c == EOF) {
          ok = false;
          break;
        }
        if (c == 0) {
          break;
        }
        s.push_back(static_cast<char>(c));
        if (s.size() > 4096) {
          ok = false;
          break;
        }
      }
      return s;
    }

    void Skip(const long long n)
    {
      if (n < 0) {
        ok = false;
        return;
      }
      if (ok && _fseeki64(f, n, SEEK_CUR) != 0) {
        ok = false;
      }
    }

    void Blob()
    {
      const std::int32_t n = I();
      Skip(n);
    }
  };

  struct Out
  {
    lua_State* L;

    void Num(const std::string& key, const double v)
    {
      lua_pushstring(L, key.c_str());
      lua_pushnumber(L, static_cast<lua_Number>(v));
      lua_settable(L, -3);
    }

    void Str(const std::string& key, const std::string& v)
    {
      lua_pushstring(L, key.c_str());
      lua_pushstring(L, v.c_str());
      lua_settable(L, -3);
    }
  };

  std::string N(const char* const a, const int i, const char* const b)
  {
    return std::string(a) + std::to_string(i) + b;
  }

  void Triple(Out& o, const char* const a, const char* const b, const char* const c, Reader& r)
  {
    o.Num(a, r.F());
    o.Num(b, r.F());
    o.Num(c, r.F());
  }

  void Push(lua_State* const L, const std::vector<std::string>& list)
  {
    lua_newtable(L);
    for (std::size_t i = 0; i < list.size(); ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(i + 1));
      lua_pushstring(L, list[i].c_str());
      lua_settable(L, -3);
    }
  }

  int __cdecl cfunc_ScmapEnv(lua_State* const L)
  {
    const char* const path = luaL_checkstring(L, 1);
    FILE* const file = std::fopen(path, "rb");
    if (file == nullptr) {
      lua_pushnil(L);
      lua_pushstring(L, "cannot open the file");
      return 2;
    }
    Reader r;
    r.f = file;
    const auto fail = [&](const char* const why) {
      std::fclose(file);
      lua_pushnil(L);
      lua_pushstring(L, why);
      return 2;
    };

    const std::int32_t sig = r.I();
    const std::int32_t major = r.I();
    if (!r.ok || sig != 0x1A70614D || major != 2) {
      return fail("not a version 2 .scmap");
    }
    r.Skip(4 + 4 + 4 + 4 + 4 + 2);      // magic, 2, width, height, 0, 0 (i32 i32 f32 f32 i32 i16 after the two checked ones)
    r.Blob();                            // preview
    const std::int32_t minor = r.I();
    const std::int32_t width = r.I();
    const std::int32_t height = r.I();
    r.Skip(4);                           // height scale
    if (!r.ok || minor < 53 || minor > 60) {
      return fail("unsupported .scmap version");
    }
    r.Skip(static_cast<long long>(width + 1) * (height + 1) * 2);
    if (minor >= 56) {
      r.Skip(1);
    }
    r.S();                               // shader
    const std::string background = r.S();
    const std::string skycube = r.S();
    std::vector<std::string> lookups;
    if (minor >= 56) {
      const std::int32_t n = r.I();
      for (std::int32_t i = 0; i < n && r.ok && n < 1000; ++i) {
        std::string key = r.S();
        std::string value = r.S();
        lookups.push_back(key);
        lookups.push_back(value);
      }
    } else {
      lookups.push_back("<default>");
      lookups.push_back(r.S());
    }
    if (!r.ok) {
      return fail("the file ends in the cube map block");
    }

    lua_newtable(L);
    Out o{L};
    o.Num("minor", minor);
    o.Num("width", width);
    o.Num("height", height);
    o.Str("skycube", skycube);
    o.Str("background", background);
    lua_pushstring(L, "lookups");
    Push(L, lookups);
    lua_settable(L, -3);

    // lighting
    lua_pushstring(L, "light");
    lua_newtable(L);
    {
      Out l{L};
      l.Num("light_mult", r.F());
      Triple(l, "sun_x", "sun_y", "sun_z", r);
      Triple(l, "ambient_r", "ambient_g", "ambient_b", r);
      Triple(l, "sun_r", "sun_g", "sun_b", r);
      Triple(l, "fill_r", "fill_g", "fill_b", r);
      l.Num("spec_r", r.F());
      l.Num("spec_g", r.F());
      l.Num("spec_b", r.F());
      l.Num("spec_a", r.F());
      l.Num("bloom", r.F());
      Triple(l, "fog_r", "fog_g", "fog_b", r);
      l.Num("fog_start", r.F());
      l.Num("fog_end", r.F());
    }
    lua_settable(L, -3);

    // water
    const bool waterOn = r.B() != 0;
    lua_pushstring(L, "water");
    lua_newtable(L);
    {
      Out w{L};
      w.Num("water_on", waterOn ? 1.0 : 0.0);
      w.Num("water_elev", r.F());
      w.Num("water_deep", r.F());
      w.Num("water_abyss", r.F());
      Triple(w, "w_color_r", "w_color_g", "w_color_b", r);
      w.Num("w_lerp_0", r.F());
      w.Num("w_lerp_1", r.F());
      w.Num("w_refraction", r.F());
      w.Num("w_fresnel_bias", r.F());
      w.Num("w_fresnel_power", r.F());
      w.Num("w_unit_refl", r.F());
      w.Num("w_sky_refl", r.F());
      w.Num("w_shininess", r.F());
      w.Num("w_strength", r.F());
      Triple(w, "w_sundir_x", "w_sundir_y", "w_sundir_z", r);
      Triple(w, "w_suncol_r", "w_suncol_g", "w_suncol_b", r);
      w.Num("w_sun_refl", r.F());
      w.Num("w_glow", r.F());
      const std::string cubemap = r.S();
      const std::string ramp = r.S();
      float repeats[4];
      for (float& x : repeats) {
        x = r.F();
      }
      std::string waves[4];
      for (int i = 0; i < 4; ++i) {
        w.Num(N("w_repeat_", i, ""), repeats[i]);
        w.Num(N("w_move", i + 1, "_x"), r.F());
        w.Num(N("w_move", i + 1, "_z"), r.F());
        waves[i] = r.S();
      }
      lua_pushstring(L, "texts");
      lua_newtable(L);
      Out t{L};
      t.Str("water_cubemap", cubemap);
      t.Str("water_ramp", ramp);
      for (int i = 0; i < 4; ++i) {
        t.Str(N("water_wave", i + 1, ""), waves[i]);
      }
      lua_settable(L, -3);
    }
    lua_settable(L, -3);
    if (!r.ok) {
      std::fclose(file);
      lua_settop(L, 0);
      lua_pushnil(L);
      lua_pushstring(L, "the file ends in the lighting / water block");
      return 2;
    }

    // wave generators (skipped): texture, ramp, 17 floats each
    {
      const std::int32_t n = r.I();
      for (std::int32_t i = 0; i < n && r.ok && n < 100000; ++i) {
        r.S();
        r.S();
        r.Skip(17 * 4);
      }
    }
    // cartographic colours
    if (minor >= 56) {
      r.Skip(6 * 4);
      if (minor > 56) {
        r.Skip(4);
      }
    }

    // strata
    std::vector<std::pair<std::string, float>> albedo(10, {"", 4.0f});
    std::vector<std::pair<std::string, float>> normals(9, {"", 4.0f});
    if (minor >= 56) {
      for (auto& a : albedo) {
        a.first = r.S();
        a.second = r.F();
      }
      for (auto& n : normals) {
        n.first = r.S();
        n.second = r.F();
      }
    } else {
      r.S();                           // tileset
      const std::int32_t count = r.I();
      static const int slots[6] = {0, 1, 2, 3, 4, 9};
      for (int i = 0; i < count && i < 6 && r.ok; ++i) {
        const std::string a = r.S();
        const std::string n = r.S();
        const float sa = r.F();
        const float sn = r.F();
        albedo[static_cast<std::size_t>(slots[i])] = {a, sa};
        if (slots[i] < 9) {
          normals[static_cast<std::size_t>(slots[i])] = {n, sn};
        }
      }
    }
    lua_pushstring(L, "strata");
    lua_newtable(L);
    for (std::size_t i = 0; i < albedo.size(); ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(i + 1));
      lua_newtable(L);
      Out s{L};
      s.Str("albedo", albedo[i].first);
      s.Num("size", albedo[i].second);
      if (i < normals.size()) {
        s.Str("normal", normals[i].first);
        s.Num("nsize", normals[i].second);
      }
      lua_settable(L, -3);
    }
    lua_settable(L, -3);
    if (!r.ok) {
      std::fclose(file);
      lua_settop(L, 0);
      lua_pushnil(L);
      lua_pushstring(L, "the file ends in the strata block");
      return 2;
    }

    // the sky block lies behind decals, normal map, masks, water maps and the terrain-type grid
    if (minor >= 60) {
      r.Skip(8);                       // two unknown ints
      const std::int32_t decals = r.I();
      for (std::int32_t i = 0; i < decals && r.ok && decals < 10000000; ++i) {
        r.Skip(8);                     // id, type
        const std::int32_t nTex = r.I();
        for (std::int32_t k = 0; k < nTex && r.ok && nTex < 64; ++k) {
          const std::int32_t len = r.I();
          r.Skip(len);
        }
        r.Skip(9 * 4 + 4 + 4 + 4);
      }
      const std::int32_t groups = r.I();
      for (std::int32_t i = 0; i < groups && r.ok && groups < 10000000; ++i) {
        r.Skip(4);
        r.S();
        const std::int32_t cnt = r.I();
        r.Skip(static_cast<long long>(cnt) * 4);
      }
      r.Skip(8);                       // second width / height
      const std::int32_t normalMaps = r.I();
      for (std::int32_t i = 0; i < normalMaps && r.ok && normalMaps < 64; ++i) {
        r.Blob();
      }
      r.Blob();                        // strata masks 1-4
      r.Blob();                        // strata masks 5-8
      r.Skip(4);                       // water map count
      r.Blob();
      const long long half = static_cast<long long>(width / 2) * (height / 2);
      r.Skip(3 * half);
      r.Skip(static_cast<long long>(width) * height);
      if (r.ok) {
        lua_pushstring(L, "sky");
        lua_newtable(L);
        Out s{L};
        Triple(s, "dome_ox", "dome_oy", "dome_oz", r);
        s.Num("dome_height", r.F());
        s.Num("dome_radius", r.F());
        s.Num("dome_start", r.F());
        s.Num("dome_w", r.I());
        s.Num("dome_h", r.I());
        s.Num("horizon_size", r.F());
        Triple(s, "horizon_r", "horizon_g", "horizon_b", r);
        Triple(s, "sky_r", "sky_g", "sky_b", r);
        s.Num("horizon_blend", r.F());
        const std::string atlas = r.S();
        const std::string glow = r.S();
        const std::int32_t count = r.I();
        std::vector<float> billboards;
        for (std::int32_t i = 0; i < count && r.ok && count <= 1024; ++i) {
          for (int k = 0; k < 10; ++k) {
            billboards.push_back(r.F());
          }
        }
        const std::string cumulus[3] = {r.S(), r.S(), r.S()};
        s.Num("cirrus_mult", r.F());
        Triple(s, "cirrus_r", "cirrus_g", "cirrus_b", r);
        const std::string cirrus = r.S();
        const std::int32_t layerCount = r.I();
        for (std::int32_t i = 0; i < layerCount && r.ok && layerCount < 64; ++i) {
          const float v[5] = {r.F(), r.F(), r.F(), r.F(), r.F()};
          if (i < 4) {
            s.Num(N("cirrus", i + 1, "_fx"), v[0]);
            s.Num(N("cirrus", i + 1, "_fz"), v[1]);
            s.Num(N("cirrus", i + 1, "_speed"), v[2]);
            s.Num(N("cirrus", i + 1, "_dx"), v[3]);
            s.Num(N("cirrus", i + 1, "_dz"), v[4]);
          }
        }
        lua_pushstring(L, "texts");
        lua_newtable(L);
        Out t{L};
        t.Str("sky_billboards", atlas);
        t.Str("sky_glow", glow);
        t.Str("sky_cumulus1", cumulus[0]);
        t.Str("sky_cumulus2", cumulus[1]);
        t.Str("sky_cumulus3", cumulus[2]);
        t.Str("sky_cirrus", cirrus);
        lua_settable(L, -3);
        lua_pushstring(L, "billboards");
        lua_newtable(L);
        for (std::size_t i = 0; i < billboards.size(); ++i) {
          lua_pushnumber(L, static_cast<lua_Number>(i + 1));
          lua_pushnumber(L, static_cast<lua_Number>(billboards[i]));
          lua_settable(L, -3);
        }
        lua_settable(L, -3);
        if (r.ok) {
          lua_settable(L, -3);        // sky
        } else {
          lua_settop(L, -3);          // drop the key and the half read table
        }
      }
    }
    std::fclose(file);
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

  struct EdScmapEnvLuaBootstrap
  {
    EdScmapEnvLuaBootstrap()
    {
      (void)new moho::CScrLuaBinder(UserLuaInitSet(), "ed_scmap_env", &cfunc_ScmapEnv, nullptr, "<global>", "ed_scmap_env(path) -> table of lighting / water / strata / sky | nil, message");
    }
  };

  const EdScmapEnvLuaBootstrap gEdScmapEnvLuaBootstrap{};
} // namespace
