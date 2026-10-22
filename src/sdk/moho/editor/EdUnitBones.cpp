// EDITOR: the bones of a unit as the renderer draws them right now (docs/projects/MOD_UNIT_EDITOR_DESIGN.md, unit viewer).
//
//   ed_camera_info() -> fovRad, eyeDistance, focusX, focusY, focusZ, heading, pitch, targetZoom, viewportX, viewportY, viewportW, viewportH
//   ed_lod_info([entityId]) -> table: eye position, heading, pitch, zoom, fov, lodScale, the LOD plane (viewport.r[1]), viewport, and for an entity its LOD depth, cutoffs, selected LOD
//   ed_unit_bones(entityId) -> { { name, parent, x, y, z, fx, fy, fz, ux, uy, uz }, ... } | nil, message
//
// One entry per bone of the entity's mesh instance (the pose is already in world space), in bone order (parent = index of the parent bone, -1 for the root; Lua counts from 1 for the list, the indices
// are the engine's bone indices counting from 0). (x, y, z) is the world position of the bone, (fx, fy, fz) the world direction of the bone's +z axis (the
// way it points), (ux, uy, uz) its +y axis. The pose is the interpolated pose of the current frame (MeshInstance::ComputeDebugPose), so a call per frame follows the animation
// exactly, unlike the sim, which only knows the pose of the last tick. Works for units (static-pose mesh instances); props and projectiles answer nil.
// The entity id is the sim's (Entity:GetEntityId()); the viewer's sim callback EdUnitInfo writes it for the unit it spawned.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "boost/shared_ptr.h"
#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/animation/CAniPose.h"
#include "moho/animation/CAniSkel.h"
#include "moho/entity/UserEntity.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/mesh/Mesh.h"
#include "moho/mesh/MeshLodDepth.h"
#include "moho/render/RCamManager.h"
#include "moho/render/camera/CameraImpl.h"
#include "moho/sim/CWldSession.h"

namespace moho
{
  extern float ren_MeshDissolve; // Mesh.cpp: the fade range past a LOD cutoff
}

namespace
{
  // v rotated by the unit quaternion q (w, x, y, z lanes of Wm3's storage: m_afTuple[0] = w)
  void Rotate(const Wm3::Quatf& q, const float x, const float y, const float z, float* const out)
  {
    const float w = q.m_afTuple[0];
    const float qx = q.m_afTuple[1];
    const float qy = q.m_afTuple[2];
    const float qz = q.m_afTuple[3];
    // t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
    const float tx = 2.0f * (qy * z - qz * y);
    const float ty = 2.0f * (qz * x - qx * z);
    const float tz = 2.0f * (qx * y - qy * x);
    out[0] = x + w * tx + (qy * tz - qz * ty);
    out[1] = y + w * ty + (qz * tx - qx * tz);
    out[2] = z + w * tz + (qx * ty - qy * tx);
  }

  int __cdecl cfunc_EdUnitBones(lua_State* const L)
  {
    const auto fail = [&](const char* const message) {
      lua_pushnil(L);
      lua_pushstring(L, message);
      return 2;
    };
    if (lua_type(L, 1) != LUA_TNUMBER) {
      return fail("ed_unit_bones(entityId)");
    }
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr) {
      return fail("no session");
    }
    moho::UserEntity* const entity = session->LookupEntityId(static_cast<moho::EntId>(lua_tonumber(L, 1)));
    if (entity == nullptr || entity->mMeshInstance == nullptr) {
      return fail("no such entity or no mesh");
    }
    moho::MeshInstance* const mesh = entity->mMeshInstance;
    const boost::shared_ptr<moho::CAniPose> pose = mesh->ComputeDebugPose();
    if (!pose) {
      return fail("the entity has no animated pose");
    }
    moho::CAniPoseBoneArray& bones = pose->mBones;
    const std::size_t count = static_cast<std::size_t>(bones.end() - bones.begin());

    lua_newtable(L);
    for (std::size_t i = 0; i < count; ++i) {
      const moho::CAniPoseBone& bone = bones[i];
      const moho::VTransform& t = bone.GetCompositeTransform();
      // the pose of a mesh instance is already in the world: the composite transform of a bone carries the entity's position and orientation
      float f[3];
      float u[3];
      Rotate(t.orient_, 0.0f, 0.0f, 1.0f, f);
      Rotate(t.orient_, 0.0f, 1.0f, 0.0f, u);

      lua_pushnumber(L, static_cast<lua_Number>(i + 1));
      lua_newtable(L);
      const char* const name = bone.GetBoneName();
      lua_pushstring(L, "name");
      lua_pushstring(L, name != nullptr ? name : "");
      lua_settable(L, -3);
      lua_pushstring(L, "index");
      lua_pushnumber(L, static_cast<lua_Number>(i));
      lua_settable(L, -3);
      lua_pushstring(L, "parent");
      lua_pushnumber(L, bone.mParent != nullptr ? static_cast<lua_Number>(bone.mParent->mIdx) : -1.0);
      lua_settable(L, -3);
      const char* const keys[9] = {"x", "y", "z", "fx", "fy", "fz", "ux", "uy", "uz"};
      const float values[9] = {t.pos_.x, t.pos_.y, t.pos_.z, f[0], f[1], f[2], u[0], u[1], u[2]};
      for (int k = 0; k < 9; ++k) {
        lua_pushstring(L, keys[k]);
        lua_pushnumber(L, values[k]);
        lua_settable(L, -3);
      }
      lua_settable(L, -3);
    }
    return 1;
  }

  // ed_camera_info() -> fovRad, eyeDistance, focusX, focusY, focusZ, heading, pitch, targetZoom, viewportX, viewportY, viewportW, viewportH | nil
  // The world camera as it is now: the field of view the engine derives from the zoom, the real distance of the eye from the focus point (what mesh LODs measure), the focus
  // point, the heading / pitch and the viewport the projection uses (the editor draws the world through the whole window and clips it to the viewport panel).
  int __cdecl cfunc_EdCameraInfo(lua_State* const L)
  {
    moho::CameraImpl* const camera = moho::CAM_GetManager()->GetCamera("WorldCamera");
    if (camera == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    Wm3::Vector2f origin;
    Wm3::Vector2f size;
    camera->CameraGetViewport(origin, size);
    const lua_Number values[12] = {camera->mFarFov, camera->mZoom, camera->mOffset.x, camera->mOffset.y, camera->mOffset.z, camera->mHeading, camera->mFarPitch,
                                   camera->mTargetZoom, origin.x, origin.y, size.x, size.y};
    for (const lua_Number v : values) {
      lua_pushnumber(L, v);
    }
    return 12;
  }

  void SetNumberField(lua_State* const L, const char* const key, const lua_Number value)
  {
    lua_pushstring(L, key);
    lua_pushnumber(L, value);
    lua_settable(L, -3);
  }

  // ed_lod_info([entityId]) -> { camera numbers..., [entity numbers...] } | nil
  // The numbers a mesh LOD decision is made from (camera diagnostics; the user sends them when units or props vanish):
  //   camera:  eyeX eyeY eyeZ (the real eye position of the last frame), heading pitch zoom (the target zoom) eyeDistance fov, lodScale, lodRowA..D (viewport.r[1]: the plane the LOD depth is
  //            measured against), viewportX/Y/W/H, ren_MeshDissolve
  //   entity:  x y z (interpolated position of the mesh instance), depth (what MeshRenderer::Batch compares with the cutoffs), distance (straight line eye to instance), lod (selected LOD,
  //            0 based, -1 = none, culled), lodCount, cutoff1..cutoffN, maxCutoff, dissolve, hidden, quatDepth (the retired quaternion based value, for comparison: must not be what is used now)
  int __cdecl cfunc_EdLodInfo(lua_State* const L)
  {
    moho::CameraImpl* const camera = moho::CAM_GetManager()->GetCamera("WorldCamera");
    if (camera == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    const moho::GeomCamera3& cam = camera->CameraGetView();
    Wm3::Vector2f origin;
    Wm3::Vector2f size;
    camera->CameraGetViewport(origin, size);
    lua_newtable(L);
    SetNumberField(L, "eyeX", cam.tranform.pos_.x);
    SetNumberField(L, "eyeY", cam.tranform.pos_.y);
    SetNumberField(L, "eyeZ", cam.tranform.pos_.z);
    SetNumberField(L, "heading", camera->mHeading);
    SetNumberField(L, "pitch", camera->mFarPitch);
    SetNumberField(L, "zoom", camera->mTargetZoom);
    SetNumberField(L, "eyeDistance", camera->mZoom);
    SetNumberField(L, "fov", camera->mFarFov);
    SetNumberField(L, "lodScale", cam.lodScale);
    SetNumberField(L, "lodRowA", cam.viewport.r[1].x);
    SetNumberField(L, "lodRowB", cam.viewport.r[1].y);
    SetNumberField(L, "lodRowC", cam.viewport.r[1].z);
    SetNumberField(L, "lodRowD", cam.viewport.r[1].w);
    SetNumberField(L, "viewportX", origin.x);
    SetNumberField(L, "viewportY", origin.y);
    SetNumberField(L, "viewportW", size.x);
    SetNumberField(L, "viewportH", size.y);
    SetNumberField(L, "dissolveRange", moho::ren_MeshDissolve);

    if (lua_type(L, 1) == LUA_TNUMBER) {
      moho::CWldSession* const session = moho::WLD_GetActiveSession();
      moho::UserEntity* const entity =
        session != nullptr ? session->LookupEntityId(static_cast<moho::EntId>(lua_tonumber(L, 1))) : nullptr;
      moho::MeshInstance* const instance = entity != nullptr ? entity->mMeshInstance : nullptr;
      if (instance != nullptr) {
        const Wm3::Vec3f& p = instance->interpolatedPosition;
        const float depth = moho::MeshLodDepth(cam.viewport.r[1].x, cam.viewport.r[1].y, cam.viewport.r[1].z, cam.viewport.r[1].w, p.x, p.y, p.z);
        const Wm3::Quatf& q = cam.tranform.orient_;
        const float dx = p.x - cam.tranform.pos_.x;
        const float dy = p.y - cam.tranform.pos_.y;
        const float dz = p.z - cam.tranform.pos_.z;
        SetNumberField(L, "x", p.x);
        SetNumberField(L, "y", p.y);
        SetNumberField(L, "z", p.z);
        SetNumberField(L, "depth", depth);
        SetNumberField(L, "distance", std::sqrt(dx * dx + dy * dy + dz * dz));
        SetNumberField(L, "quatDepth", moho::MeshLodDepth(q.m_afTuple[0], q.m_afTuple[1], q.m_afTuple[2], q.m_afTuple[3], p.x, p.y, p.z));
        SetNumberField(L, "dissolve", instance->dissolve);
        SetNumberField(L, "hidden", instance->isHidden != 0u ? 1.0 : 0.0);
        const boost::shared_ptr<moho::Mesh> mesh = instance->GetMesh();
        if (mesh) {
          const moho::MeshLOD* const chosen = mesh->ComputeLOD(depth);
          int index = -1;
          int count = 0;
          char key[16];
          for (moho::MeshLOD* const* it = mesh->lods.begin(); it != mesh->lods.end(); ++it, ++count) {
            if (*it == chosen && chosen != nullptr) {
              index = count;
            }
            std::snprintf(key, sizeof(key), "cutoff%d", count + 1);
            SetNumberField(L, key, *it != nullptr ? (*it)->cutoff : 0.0f);
          }
          SetNumberField(L, "lod", index);
          SetNumberField(L, "lodCount", count);
          SetNumberField(L, "maxCutoff", mesh->GetMaxCutoff());
        }
      }
    }
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

  struct EdUnitBonesLuaBootstrap
  {
    EdUnitBonesLuaBootstrap()
    {
      (void)new moho::CScrLuaBinder(UserLuaInitSet(), "ed_camera_info", &cfunc_EdCameraInfo, nullptr, "<global>", "ed_camera_info() -> fov, eye, focus x y z, heading, pitch, targetZoom, viewport x y w h");
      (void)new moho::CScrLuaBinder(UserLuaInitSet(), "ed_lod_info", &cfunc_EdLodInfo, nullptr, "<global>", "ed_lod_info([entityId]) -> table of the camera and LOD numbers (eye, heading, pitch, zoom, fov, LOD row, viewport; with an entity id its depth, cutoffs and selected LOD) | nil");
      (void)new moho::CScrLuaBinder(UserLuaInitSet(), "ed_unit_bones", &cfunc_EdUnitBones, nullptr, "<global>", "ed_unit_bones(entityId) -> list of bones | nil, message");
    }
  };

  const EdUnitBonesLuaBootstrap gEdUnitBonesLuaBootstrap{};
} // namespace
