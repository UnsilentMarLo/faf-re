// Editor natives for the mesh terrain (EdMeshTerrain.h, EdMeshData.h):
//
//   ed_mesh_load(manifestVfsPath)       -> true | nil, why          reads the manifest and its patches (any mounted folder or archive)
//   ed_mesh_unload()
//   ed_mesh_mode([mode])                -> mode                     0 height field, 1 mesh, 2 both; a number sets it
//   ed_mesh_info()                      -> table | nil              loaded map: counts, size, playable, water, heights, last draw time
//   ed_mesh_height(x, z [, ceiling])    -> y | nil                  highest mesh surface under the point at or below `ceiling`
//   ed_mesh_ray(ox, oy, oz, dx, dy, dz [, maxDistance]) -> t, x, y, z, nx, ny, nz, patch, material | nil     (1 based patch and material numbers)
//   ed_mesh_pick(px, py, vw, vh)        -> ray hit under a pixel of the world view (see below)
//   ed_mesh_range([margin])             -> low, high | nil          lowest / highest vertex inside the playable rectangle grown by `margin` (default 32)

#include <cmath>
#include <cstdint>
#include <string>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/editor/EdMeshTerrain.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace
{
  void Put(lua_State* const L, const char* const key, const double value)
  {
    lua_pushstring(L, key);
    lua_pushnumber(L, static_cast<lua_Number>(value));
    lua_settable(L, -3);
  }

  void PutString(lua_State* const L, const char* const key, const std::string& value)
  {
    lua_pushstring(L, key);
    lua_pushstring(L, value.c_str());
    lua_settable(L, -3);
  }

  int __cdecl cfunc_EdMeshLoad(lua_State* const L)
  {
    const char* const path = lua_tostring(L, 1);
    if (path == nullptr) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_mesh_load(manifestVfsPath)");
      return 2;
    }
    moho::edmesh::ReleaseGpu();
    const bool ok = moho::edmesh::Map().Load(path);
    moho::edmesh::MarkChanged();
    if (!ok) {
      lua_pushnil(L);
      lua_pushstring(L, moho::edmesh::Map().error.c_str());
      return 2;
    }
    moho::edmesh::ApplyHeightBands();
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_EdMeshUnload(lua_State* const L)
  {
    (void)L;
    moho::edmesh::ReleaseGpu();
    moho::edmesh::Map().Clear();
    moho::edmesh::MarkChanged();
    return 0;
  }

  int __cdecl cfunc_EdMeshMode(lua_State* const L)
  {
    if (lua_type(L, 1) == LUA_TNUMBER) {
      const int mode = static_cast<int>(lua_tonumber(L, 1));
      moho::edmesh::SetMode(mode <= 0 ? moho::edmesh::Mode::HeightField : mode == 1 ? moho::edmesh::Mode::Mesh : moho::edmesh::Mode::Both);
    }
    lua_pushnumber(L, static_cast<lua_Number>(static_cast<int>(moho::edmesh::GetMode())));
    return 1;
  }

  int __cdecl cfunc_EdMeshInfo(lua_State* const L)
  {
    const moho::edmesh::MeshMap& m = moho::edmesh::Map();
    if (!m.Loaded()) {
      lua_pushnil(L);
      return 1;
    }
    lua_newtable(L);
    PutString(L, "manifest", m.manifestPath);
    Put(L, "mode", static_cast<int>(moho::edmesh::GetMode()));
    Put(L, "patches", static_cast<double>(m.patches.size()));
    Put(L, "materials", static_cast<double>(m.materials.size()));
    Put(L, "vertices", static_cast<double>(m.TotalVertices()));
    Put(L, "triangles", static_cast<double>(m.TotalTriangles()));
    int sky = 0;
    for (const moho::edmesh::Patch& p : m.patches) {
      sky += p.sky ? 1 : 0;
    }
    Put(L, "sky_patches", sky);
    const moho::edmesh::Settings& s = m.settings;
    Put(L, "size", s.size);
    Put(L, "playable_x0", s.playable[0]);
    Put(L, "playable_z0", s.playable[1]);
    Put(L, "playable_x1", s.playable[2]);
    Put(L, "playable_z1", s.playable[3]);
    Put(L, "water", s.water);
    Put(L, "water_abyss", s.waterAbyss);
    Put(L, "ground_low", s.groundLow);
    Put(L, "ground_high", s.groundHigh);
    Put(L, "mesh_high", s.meshHigh);
    Put(L, "draw_ms", moho::edmesh::LastDrawMs());
    Put(L, "generation", static_cast<double>(moho::edmesh::Generation()));
    return 1;
  }

  int __cdecl cfunc_EdMeshHeight(lua_State* const L)
  {
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float z = static_cast<float>(luaL_checknumber(L, 2));
    const float ceiling = lua_type(L, 3) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, 3)) : 1e30f;
    float y = 0.0f;
    if (moho::edmesh::Map().HeightAt(x, z, ceiling, y)) {
      lua_pushnumber(L, static_cast<lua_Number>(y));
    } else {
      lua_pushnil(L);
    }
    return 1;
  }

  int __cdecl cfunc_EdMeshRay(lua_State* const L)
  {
    float o[3], d[3];
    for (int i = 0; i < 3; ++i) {
      o[i] = static_cast<float>(luaL_checknumber(L, 1 + i));
      d[i] = static_cast<float>(luaL_checknumber(L, 4 + i));
    }
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-9f) {
      lua_pushnil(L);
      return 1;
    }
    for (float& v : d) {
      v /= len;
    }
    const float maxDistance = lua_type(L, 7) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, 7)) : 1e9f;
    moho::edmesh::RayHit hit;
    if (!moho::edmesh::Map().Raycast(o, d, maxDistance, hit)) {
      lua_pushnil(L);
      return 1;
    }
    const lua_Number values[9] = {hit.t,          hit.position[0], hit.position[1], hit.position[2], hit.normal[0],
                                  hit.normal[1],  hit.normal[2],   static_cast<lua_Number>(hit.patch + 1), static_cast<lua_Number>(hit.material + 1)};
    for (const lua_Number v : values) {
      lua_pushnumber(L, v);
    }
    return 9;
  }

  // ed_mesh_pick(px, py, vw, vh) -> the same as ed_mesh_ray for the ray under that pixel of the world view; the ray itself when the mesh is not hit:
  //   t, x, y, z, nx, ny, nz, patch, material | nil, ox, oy, oz, dx, dy, dz
  int __cdecl cfunc_EdMeshPick(lua_State* const L)
  {
    float o[3], d[3];
    if (!moho::edmesh::CameraRay(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                                 static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)), o, d)) {
      lua_pushnil(L);
      return 1;
    }
    moho::edmesh::RayHit hit;
    if (moho::edmesh::Map().Raycast(o, d, 1e9f, hit)) {
      const lua_Number values[9] = {hit.t,          hit.position[0], hit.position[1], hit.position[2], hit.normal[0],
                                    hit.normal[1],  hit.normal[2],   static_cast<lua_Number>(hit.patch + 1), static_cast<lua_Number>(hit.material + 1)};
      for (const lua_Number v : values) {
        lua_pushnumber(L, v);
      }
      return 9;
    }
    lua_pushnil(L);
    for (int i = 0; i < 3; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(o[i]));
    }
    for (int i = 0; i < 3; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(d[i]));
    }
    return 7;
  }

  int __cdecl cfunc_EdMeshRange(lua_State* const L)
  {
    const float margin = lua_type(L, 1) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, 1)) : 32.0f;
    float low = 0.0f, high = 0.0f;
    if (!moho::edmesh::Map().HeightRange(margin, low, high)) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(low));
    lua_pushnumber(L, static_cast<lua_Number>(high));
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

  struct EdMeshLuaBootstrap
  {
    EdMeshLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_mesh_load", &cfunc_EdMeshLoad, nullptr, "<global>", "ed_mesh_load(manifestVfsPath) -> true | nil, why");
      (void)new moho::CScrLuaBinder(set, "ed_mesh_unload", &cfunc_EdMeshUnload, nullptr, "<global>", "ed_mesh_unload()");
      (void)new moho::CScrLuaBinder(set, "ed_mesh_mode", &cfunc_EdMeshMode, nullptr, "<global>", "ed_mesh_mode([mode]) -> mode (0 height field, 1 mesh, 2 both)");
      (void)new moho::CScrLuaBinder(set, "ed_mesh_info", &cfunc_EdMeshInfo, nullptr, "<global>", "ed_mesh_info() -> table | nil");
      (void)new moho::CScrLuaBinder(set, "ed_mesh_height", &cfunc_EdMeshHeight, nullptr, "<global>", "ed_mesh_height(x, z [, ceiling]) -> y | nil");
      (void)new moho::CScrLuaBinder(set, "ed_mesh_ray", &cfunc_EdMeshRay, nullptr, "<global>",
                                    "ed_mesh_ray(ox, oy, oz, dx, dy, dz [, maxDistance]) -> t, x, y, z, nx, ny, nz, patch, material | nil");
      (void)new moho::CScrLuaBinder(set, "ed_mesh_pick", &cfunc_EdMeshPick, nullptr, "<global>",
                                    "ed_mesh_pick(px, py, vw, vh) -> t, x, y, z, nx, ny, nz, patch, material | nil, ox, oy, oz, dx, dy, dz");
      (void)new moho::CScrLuaBinder(set, "ed_mesh_range", &cfunc_EdMeshRange, nullptr, "<global>", "ed_mesh_range([margin]) -> low, high | nil");
    }
  };

  const EdMeshLuaBootstrap gEdMeshLuaBootstrap{};
} // namespace
