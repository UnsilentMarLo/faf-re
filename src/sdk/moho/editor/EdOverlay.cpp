#include "moho/editor/EdOverlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "boost/shared_ptr.h"
#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/d3d/CD3DPrimBatcher.h"
#include "moho/render/textures/CD3DBatchTexture.h"

namespace
{
  struct Polyline
  {
    std::vector<float> pts;     // x, y, z triples
    std::uint32_t argb = 0xFFFFFFFFu;
    float worldWidth = 0.6f;    // ribbon width on the ground in world units
    float minPx = 2.0f;         // never thinner than this on screen
    float maxPx = 14.0f;        // never thicker than this on screen
    bool closed = false;
    bool halo = true;           // dark underlay for contrast on any terrain
  };

  std::map<std::string, Polyline>& Lines()
  {
    static std::map<std::string, Polyline> lines;
    return lines;
  }

  // A textured square of constant size on screen, anchored to a world point (marker icons).
  struct Icon
  {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    std::uint32_t argb = 0xFFFFFFFFu;
    float px = 24.0f;
    std::string texture;
  };

  std::map<std::string, Icon>& Icons()
  {
    static std::map<std::string, Icon> icons;
    return icons;
  }

  float gCam[16] = {};  // camera->viewport rows, for ed_overlay_stats
  float gDbg[8] = {};   // ed_overlay_stats: first vertex of the last drawn line (screen x, y, depth, half), viewport w, h, segments drawn, culled

  struct Pt
  {
    float x, y;
    float depth;
  };

  void Emit(moho::CD3DPrimBatcher& b, float x0, float y0, float h0, float x1, float y1, float h1, std::uint32_t color)
  {
    float dx = x1 - x0;
    float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) {
      return;
    }
    dx /= len;
    dy /= len;
    const float nx = -dy;
    const float ny = dx;
    using V = moho::CD3DPrimBatcher::Vertex;
    const V a{x0 + nx * h0, y0 + ny * h0, 0.0f, color, 0.0f, 0.0f};
    const V c{x0 - nx * h0, y0 - ny * h0, 0.0f, color, 0.0f, 1.0f};
    const V d{x1 - nx * h1, y1 - ny * h1, 0.0f, color, 1.0f, 1.0f};
    const V e{x1 + nx * h1, y1 + ny * h1, 0.0f, color, 1.0f, 0.0f};
    b.DrawQuad(a, c, d, e);
  }

  void Cap(moho::CD3DPrimBatcher& b, float x, float y, float h, std::uint32_t color)
  {
    using V = moho::CD3DPrimBatcher::Vertex;
    b.DrawQuad(V{x - h, y - h, 0.0f, color, 0.0f, 0.0f}, V{x + h, y - h, 0.0f, color, 1.0f, 0.0f},
               V{x + h, y + h, 0.0f, color, 1.0f, 1.0f}, V{x - h, y + h, 0.0f, color, 0.0f, 1.0f});
  }

  void DrawLine(moho::GeomCamera3& cam, moho::CD3DPrimBatcher& b, const Polyline& pl, float vw, float vh, bool halo)
  {
    const std::size_t n = pl.pts.size() / 3u;
    if (n < 2u) {
      return;
    }
    const std::size_t segs = pl.closed ? n : n - 1u;

    // screen position, depth and on-screen half width of every vertex
    std::vector<Pt> sp(n);
    std::vector<float> half(n);
    for (std::size_t i = 0; i < n; ++i) {
      const Wm3::Vector3f p{pl.pts[i * 3], pl.pts[i * 3 + 1], pl.pts[i * 3 + 2]};
      const Wm3::Vector2f s = cam.Project(p, 0.0f, vw, vh, 0.0f);
      sp[i] = {s.x, s.y, cam.viewport.ProjectViewportDepthRow1(p)};

      // width of a horizontal ribbon of `worldWidth` across this vertex, measured on screen
      const std::size_t a = (i == 0u) ? (pl.closed ? n - 1u : 0u) : i - 1u;
      const std::size_t c = (i + 1u < n) ? i + 1u : (pl.closed ? 0u : n - 1u);
      const float dx = pl.pts[c * 3] - pl.pts[a * 3];
      const float dz = pl.pts[c * 3 + 2] - pl.pts[a * 3 + 2];
      const float l = std::sqrt(dx * dx + dz * dz);
      float px = 0.0f;
      if (l > 0.0001f) {
        const float hw = pl.worldWidth * 0.5f;
        const float ox = dz / l * hw;
        const float oz = -dx / l * hw;
        const Wm3::Vector2f s1 = cam.Project(Wm3::Vector3f{p.x + ox, p.y, p.z + oz}, 0.0f, vw, vh, 0.0f);
        const Wm3::Vector2f s2 = cam.Project(Wm3::Vector3f{p.x - ox, p.y, p.z - oz}, 0.0f, vw, vh, 0.0f);
        px = 0.5f * std::sqrt((s1.x - s2.x) * (s1.x - s2.x) + (s1.y - s2.y) * (s1.y - s2.y));
      }
      half[i] = std::min(pl.maxPx * 0.5f, std::max(pl.minPx * 0.5f, px)) + (halo ? 1.0f : 0.0f);
    }

    if (!halo) {
      gDbg[0] = sp[0].x; gDbg[1] = sp[0].y; gDbg[2] = sp[0].depth; gDbg[3] = half[0]; gDbg[4] = vw; gDbg[5] = vh;
      gDbg[6] = 0.0f; gDbg[7] = 0.0f;
    }
    const std::uint32_t color = halo ? 0xB0000000u : pl.argb;
    for (std::size_t s = 0; s < segs; ++s) {
      const std::size_t i0 = s;
      const std::size_t i1 = (s + 1u) % n;
      const Pt& a = sp[i0];
      const Pt& c = sp[i1];
      if (a.depth < 1.0f || c.depth < 1.0f) {
        if (!halo) gDbg[7] += 1.0f;
        continue;
      }
      if ((a.x < -64.0f && c.x < -64.0f) || (a.x > vw + 64.0f && c.x > vw + 64.0f) ||
          (a.y < -64.0f && c.y < -64.0f) || (a.y > vh + 64.0f && c.y > vh + 64.0f)) {
        continue;
      }
      if (!halo) gDbg[6] += 1.0f;
      Emit(b, a.x, a.y, half[i0], c.x, c.y, half[i1], color);
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (sp[i].depth >= 1.0f) {
        Cap(b, sp[i].x, sp[i].y, half[i], color);   // joins the ribbons at the vertices
      }
    }
  }

  // ed_overlay_set(id, points, color [, worldWidth, minPx, maxPx, closed, halo])
  //   points: flat array { x, y, z, x, y, z, ... } in world coordinates; color: 'RRGGBB' or 'AARRGGBB' hex string
  int __cdecl cfunc_EdOverlaySet(lua_State* const L)
  {
    if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TTABLE) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_set(id, points, color [, worldWidth, minPx, maxPx, closed, halo])");
      return 2;
    }
    Polyline pl;
    const int count = lua_getn(L, 2);
    pl.pts.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i + 2 <= count; i += 3) {
      for (int k = 0; k < 3; ++k) {
        lua_rawgeti(L, 2, i + k);
        pl.pts.push_back(static_cast<float>(lua_tonumber(L, -1)));
        lua_settop(L, -2);
      }
    }
    if (const char* const text = lua_tostring(L, 3)) {
      const std::uint32_t v = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16));
      pl.argb = (std::strlen(text) <= 6u) ? (0xFF000000u | v) : v;
    }
    if (lua_type(L, 4) == LUA_TNUMBER) pl.worldWidth = static_cast<float>(lua_tonumber(L, 4));
    if (lua_type(L, 5) == LUA_TNUMBER) pl.minPx = static_cast<float>(lua_tonumber(L, 5));
    if (lua_type(L, 6) == LUA_TNUMBER) pl.maxPx = static_cast<float>(lua_tonumber(L, 6));
    pl.closed = lua_toboolean(L, 7) != 0;
    if (lua_type(L, 8) != LUA_TNIL && lua_type(L, 8) != LUA_TNONE) pl.halo = lua_toboolean(L, 8) != 0;
    Lines()[lua_tostring(L, 1)] = std::move(pl);
    lua_pushboolean(L, 1);
    return 1;
  }

  // ed_overlay_icon(id, x, y, z, texturePath, sizePx [, color])
  int __cdecl cfunc_EdOverlayIcon(lua_State* const L)
  {
    if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 5) != LUA_TSTRING) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_icon(id, x, y, z, texturePath, sizePx [, color])");
      return 2;
    }
    Icon icon;
    icon.x = static_cast<float>(lua_tonumber(L, 2));
    icon.y = static_cast<float>(lua_tonumber(L, 3));
    icon.z = static_cast<float>(lua_tonumber(L, 4));
    icon.texture = lua_tostring(L, 5);
    if (lua_type(L, 6) == LUA_TNUMBER) icon.px = static_cast<float>(lua_tonumber(L, 6));
    if (const char* const text = lua_tostring(L, 7)) {
      const std::uint32_t v = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16));
      icon.argb = (std::strlen(text) <= 6u) ? (0xFF000000u | v) : v;
    }
    Icons()[lua_tostring(L, 1)] = std::move(icon);
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_EdOverlayRemove(lua_State* const L)
  {
    if (const char* const id = lua_tostring(L, 1)) {
      Lines().erase(id);
      Icons().erase(id);
    }
    return 0;
  }

  int __cdecl cfunc_EdOverlayClear(lua_State* const)
  {
    Lines().clear();
    Icons().clear();
    return 0;
  }

  int __cdecl cfunc_EdOverlayStats(lua_State* const L)
  {
    for (int i = 0; i < 8; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(gDbg[i]));
    }
    for (int i = 0; i < 16; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(gCam[i]));
    }
    return 24;
  }

  int __cdecl cfunc_EdOverlayCount(lua_State* const L)
  {
    lua_pushnumber(L, static_cast<lua_Number>(Lines().size() + Icons().size()));
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

  struct EdOverlayLuaBootstrap
  {
    EdOverlayLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_overlay_set", &cfunc_EdOverlaySet, nullptr, "<global>",
                                    "ed_overlay_set(id, points, color [, worldWidth, minPx, maxPx, closed, halo]) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_icon", &cfunc_EdOverlayIcon, nullptr, "<global>",
                                    "ed_overlay_icon(id, x, y, z, texturePath, sizePx [, color]) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_remove", &cfunc_EdOverlayRemove, nullptr, "<global>", "ed_overlay_remove(id)");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_clear", &cfunc_EdOverlayClear, nullptr, "<global>", "ed_overlay_clear()");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_stats", &cfunc_EdOverlayStats, nullptr, "<global>", "ed_overlay_stats() -> x, y, depth, half, vw, vh, drawn, culled");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_count", &cfunc_EdOverlayCount, nullptr, "<global>", "ed_overlay_count() -> number of lines");
    }
  };

  const EdOverlayLuaBootstrap gEdOverlayLuaBootstrap{};
} // namespace

namespace moho
{
  namespace edoverlay
  {
    void Render(GeomCamera3* const camera, CD3DPrimBatcher* const batcher)
    {
      if ((Lines().empty() && Icons().empty()) || camera == nullptr || batcher == nullptr) {
        return;
      }
      for (int r = 0; r < 4; ++r) {
        gCam[r * 4 + 0] = camera->viewport.r[r].x;
        gCam[r * 4 + 1] = camera->viewport.r[r].y;
        gCam[r * 4 + 2] = camera->viewport.r[r].z;
        gCam[r * 4 + 3] = camera->viewport.r[r].w;
      }
      const float vw = static_cast<float>(static_cast<std::int32_t>(camera->viewport.r[3].z));
      const float vh = static_cast<float>(static_cast<std::int32_t>(camera->viewport.r[3].w));
      if (vw < 1.0f || vh < 1.0f) {
        return;
      }

      batcher->SetProjectionMatrix(MakeViewportPixelProjection(*camera));
      batcher->SetViewMatrix(VMatrix4::Identity());
      (void)batcher->Setup("TAlphaBlendLinearSampleNoDepth");
      batcher->SetTexture(CD3DBatchTexture::FromSolidColor(0xFFFFFFFFu));

      // dark halos first, then every line on top of all of them
      for (const auto& entry : Lines()) {
        if (entry.second.halo) {
          DrawLine(*camera, *batcher, entry.second, vw, vh, true);
        }
      }
      for (const auto& entry : Lines()) {
        DrawLine(*camera, *batcher, entry.second, vw, vh, false);
      }
      batcher->Flush();

      // icons, one texture at a time
      std::map<std::string, std::vector<const Icon*>> byTexture;
      for (const auto& entry : Icons()) {
        byTexture[entry.second.texture].push_back(&entry.second);
      }
      for (const auto& group : byTexture) {
        const boost::shared_ptr<CD3DBatchTexture> texture = CD3DBatchTexture::FromFile(group.first.c_str(), 0u);
        if (!texture) {
          continue;
        }
        batcher->SetTexture(texture);
        for (const Icon* const icon : group.second) {
          const Wm3::Vector3f p{icon->x, icon->y, icon->z};
          if (camera->viewport.ProjectViewportDepthRow1(p) < 1.0f) {
            continue;
          }
          const Wm3::Vector2f s = camera->Project(p, 0.0f, vw, vh, 0.0f);
          if (s.x < -64.0f || s.y < -64.0f || s.x > vw + 64.0f || s.y > vh + 64.0f) {
            continue;
          }
          const float h = icon->px * 0.5f;
          const float cx = std::floor(s.x) + 0.5f;
          const float cy = std::floor(s.y) + 0.5f;
          using V = CD3DPrimBatcher::Vertex;
          batcher->DrawQuad(V{cx - h, cy - h, 0.0f, icon->argb, 0.0f, 0.0f}, V{cx + h, cy - h, 0.0f, icon->argb, 1.0f, 0.0f},
                            V{cx + h, cy + h, 0.0f, icon->argb, 1.0f, 1.0f}, V{cx - h, cy + h, 0.0f, icon->argb, 0.0f, 1.0f});
        }
        batcher->Flush();
      }
    }
  } // namespace edoverlay
} // namespace moho
