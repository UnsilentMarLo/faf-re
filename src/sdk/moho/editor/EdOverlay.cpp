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
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"

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
    bool caps = true;           // squares at the vertices that join the ribbons (off for thin grid lines)
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

  // A textured square lying on the ground (a splat): `size` world units across, centred on (x, y, z), projected like everything else.
  struct Decal
  {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float size = 2.0f;
    float yaw = 0.0f;   // radians
    std::uint32_t argb = 0xFFFFFFFFu;
    std::string texture;
  };

  std::map<std::string, Decal>& Decals()
  {
    static std::map<std::string, Decal> decals;
    return decals;
  }

  float gCam[16] = {};  // camera->viewport rows, for ed_overlay_stats
  float gDbg[8] = {};   // ed_overlay_stats: first vertex of the last drawn line (screen x, y, depth, half), viewport w, h, segments drawn, culled

  // ---- slope view: the engine's own terrain ratings as a colour gradient over the ground ------------------------------------------------
  //
  // kind 0 "path": the rule a mobile footprint of `window` x `window` cells is checked with (STIMap::OccupancyCapsOfFootprintAt /
  //   OCCUPY_MobileCheck): the largest height difference between neighbouring samples inside the window, in world units per cell,
  //   against the footprint's MaxSlope (0.75 for every land, amphibious and hover footprint, lua/footprints.lua).
  // kind 1 "build": the rule a structure is checked with (OCCUPY_CheckAreaFlatness): highest minus lowest sample over the skirt
  //   (window x window cells, window + 1 samples per side), against the blueprint's Physics.MaxGroundVariation (1.1 for most).
  struct SlopeLayer
  {
    bool on = false;
    int kind = 0;
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;   // cell rectangle [x0, x1) x [z0, z1)
    int window = 1;
    float limit = 0.75f;
  };

  SlopeLayer gSlope;

  moho::CHeightField* ViewHeightField()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr || session->mWldMap->mTerrainRes == nullptr) {
      return nullptr;
    }
    return session->mWldMap->mTerrainRes->GetHeightField();
  }

  // The rating of one cell from a sampler h(x, z) returning the height word of a sample.
  template <class Sampler>
  float SlopeMetric(const int kind, const int n, const int cx, const int cz, Sampler&& h)
  {
    if (kind == 0) {
      int maxDelta = 0;
      for (int z = cz; z <= cz + n; ++z) {
        for (int x = cx; x < cx + n; ++x) {
          maxDelta = std::max(maxDelta, std::abs(h(x + 1, z) - h(x, z)));
        }
      }
      for (int x = cx; x <= cx + n; ++x) {
        for (int z = cz; z < cz + n; ++z) {
          maxDelta = std::max(maxDelta, std::abs(h(x, z + 1) - h(x, z)));
        }
      }
      return static_cast<float>(maxDelta) / 128.0f;
    }
    int lo = 1 << 30;
    int hi = -(1 << 30);
    for (int z = cz; z <= cz + n; ++z) {
      for (int x = cx; x <= cx + n; ++x) {
        const int v = h(x, z);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
    }
    return static_cast<float>(hi - lo) / 128.0f;
  }

  // Ratings of a whole region, computed with the same rules in separable passes (cost ~ cells x window), then reduced to blocks of
  // `stride` x `stride` cells (the steepest cell of a block wins, so a thin cliff never disappears when zoomed far out).
  struct SlopeCache
  {
    bool valid = false;
    int kind = -1, x0 = 0, z0 = 0, x1 = 0, z1 = 0, window = 0;
    unsigned builtAt = 0;
    int stride = 1, bw = 0, bh = 0;
    std::vector<float> block;   // bw x bh, row major
  };

  SlopeCache gSlopeCache;
  unsigned gSlopeFrame = 0;

  void RebuildSlopeCache(moho::CHeightField& field, const SlopeLayer& L, const int x0, const int z0, const int x1, const int z1)
  {
    SlopeCache& c = gSlopeCache;
    const int n = L.window;
    const int cw = x1 - x0;
    const int ch = z1 - z0;
    const int sw = cw + n + 1;
    const int sh = ch + n + 1;
    std::vector<int> hs(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh));
    for (int j = 0; j < sh; ++j) {
      for (int i = 0; i < sw; ++i) {
        hs[static_cast<std::size_t>(j) * sw + i] = static_cast<int>(field.GetHeightAt(x0 + i, z0 + j));
      }
    }
    auto H = [&](const int i, const int j) { return hs[static_cast<std::size_t>(j) * sw + i]; };

    std::vector<float> val(static_cast<std::size_t>(cw) * ch, 0.0f);
    if (L.kind == 0) {
      // neighbouring-sample deltas inside the window: rows z in [j, j+n] of horizontal deltas over x in [i, i+n-1], and the same for columns
      std::vector<int> a(static_cast<std::size_t>(cw) * sh, 0);        // a[z][i] = max over x in [i, i+n-1] of |h(x+1,z) - h(x,z)|
      for (int z = 0; z < sh; ++z) {
        for (int i = 0; i < cw; ++i) {
          int m = 0;
          for (int x = i; x < i + n; ++x) m = std::max(m, std::abs(H(x + 1, z) - H(x, z)));
          a[static_cast<std::size_t>(z) * cw + i] = m;
        }
      }
      std::vector<int> b(static_cast<std::size_t>(sw) * ch, 0);        // b[j][x] = max over z in [j, j+n-1] of |h(x,z+1) - h(x,z)|
      for (int j = 0; j < ch; ++j) {
        for (int x = 0; x < sw; ++x) {
          int m = 0;
          for (int z = j; z < j + n; ++z) m = std::max(m, std::abs(H(x, z + 1) - H(x, z)));
          b[static_cast<std::size_t>(j) * sw + x] = m;
        }
      }
      for (int j = 0; j < ch; ++j) {
        for (int i = 0; i < cw; ++i) {
          int m = 0;
          for (int z = j; z <= j + n; ++z) m = std::max(m, a[static_cast<std::size_t>(z) * cw + i]);
          for (int x = i; x <= i + n; ++x) m = std::max(m, b[static_cast<std::size_t>(j) * sw + x]);
          val[static_cast<std::size_t>(j) * cw + i] = static_cast<float>(m) / 128.0f;
        }
      }
    } else {
      std::vector<int> rmax(static_cast<std::size_t>(cw) * sh), rmin(static_cast<std::size_t>(cw) * sh);
      for (int z = 0; z < sh; ++z) {
        for (int i = 0; i < cw; ++i) {
          int hi = -(1 << 30), lo = 1 << 30;
          for (int x = i; x <= i + n; ++x) {
            hi = std::max(hi, H(x, z));
            lo = std::min(lo, H(x, z));
          }
          rmax[static_cast<std::size_t>(z) * cw + i] = hi;
          rmin[static_cast<std::size_t>(z) * cw + i] = lo;
        }
      }
      for (int j = 0; j < ch; ++j) {
        for (int i = 0; i < cw; ++i) {
          int hi = -(1 << 30), lo = 1 << 30;
          for (int z = j; z <= j + n; ++z) {
            hi = std::max(hi, rmax[static_cast<std::size_t>(z) * cw + i]);
            lo = std::min(lo, rmin[static_cast<std::size_t>(z) * cw + i]);
          }
          val[static_cast<std::size_t>(j) * cw + i] = static_cast<float>(hi - lo) / 128.0f;
        }
      }
    }

    c.stride = std::max(1, (std::max(cw, ch) + 189) / 190);
    c.bw = (cw + c.stride - 1) / c.stride;
    c.bh = (ch + c.stride - 1) / c.stride;
    c.block.assign(static_cast<std::size_t>(c.bw) * c.bh, 0.0f);
    for (int j = 0; j < ch; ++j) {
      for (int i = 0; i < cw; ++i) {
        float& blockValue = c.block[static_cast<std::size_t>(j / c.stride) * c.bw + (i / c.stride)];
        blockValue = std::max(blockValue, val[static_cast<std::size_t>(j) * cw + i]);
      }
    }
    c.kind = L.kind;
    c.x0 = x0; c.z0 = z0; c.x1 = x1; c.z1 = z1;
    c.window = n;
    c.builtAt = gSlopeFrame;
    c.valid = true;
  }

  // green (gentle) -> yellow -> red (at the limit), purple beyond it; flat ground stays clear so the terrain shows through
  std::uint32_t SlopeColor(const float value, const float limit)
  {
    const float t = value / limit;
    if (t > 1.0f) {
      return 0xB8C018D8u;
    }
    if (t < 0.04f) {
      return 0u;
    }
    float r, g, b;
    if (t < 0.5f) {
      const float f = t / 0.5f;
      r = 60.0f + (240.0f - 60.0f) * f;
      g = 200.0f + (220.0f - 200.0f) * f;
      b = 70.0f + (40.0f - 70.0f) * f;
    } else {
      const float f = (t - 0.5f) / 0.5f;
      r = 240.0f;
      g = 220.0f + (50.0f - 220.0f) * f;
      b = 40.0f;
    }
    const std::uint32_t a = 0x58u + static_cast<std::uint32_t>(0x40 * std::min(1.0f, t));
    return (a << 24) | (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
  }

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
    if (pl.caps) {
      for (std::size_t i = 0; i < n; ++i) {
        if (sp[i].depth >= 1.0f) {
          Cap(b, sp[i].x, sp[i].y, half[i], color);   // joins the ribbons at the vertices
        }
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
    if (lua_type(L, 9) != LUA_TNIL && lua_type(L, 9) != LUA_TNONE) pl.caps = lua_toboolean(L, 9) != 0;
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

  // ed_overlay_decal(id, x, y, z, texturePath, size [, color, yawRadians])
  int __cdecl cfunc_EdOverlayDecal(lua_State* const L)
  {
    if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 5) != LUA_TSTRING) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_decal(id, x, y, z, texturePath, size [, color, yaw])");
      return 2;
    }
    Decal decal;
    decal.x = static_cast<float>(lua_tonumber(L, 2));
    decal.y = static_cast<float>(lua_tonumber(L, 3));
    decal.z = static_cast<float>(lua_tonumber(L, 4));
    decal.texture = lua_tostring(L, 5);
    if (lua_type(L, 6) == LUA_TNUMBER) decal.size = static_cast<float>(lua_tonumber(L, 6));
    if (const char* const text = lua_tostring(L, 7)) {
      const std::uint32_t v = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16));
      decal.argb = (std::strlen(text) <= 6u) ? (0xFF000000u | v) : v;
    }
    if (lua_type(L, 8) == LUA_TNUMBER) decal.yaw = static_cast<float>(lua_tonumber(L, 8));
    Decals()[lua_tostring(L, 1)] = std::move(decal);
    lua_pushboolean(L, 1);
    return 1;
  }

  // ed_overlay_slope(kind, x0, z0, x1, z1, window, limit): show the slope gradient over the cell rectangle [x0, x1) x [z0, z1);
  // kind 'path' (mobile footprint rule) or 'build' (structure ground variation).
  int __cdecl cfunc_EdOverlaySlope(lua_State* const L)
  {
    const char* const kind = lua_tostring(L, 1);
    if (kind == nullptr) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_slope(kind, x0, z0, x1, z1, window, limit)");
      return 2;
    }
    gSlope.kind = (std::strcmp(kind, "build") == 0) ? 1 : 0;
    gSlope.x0 = static_cast<int>(lua_tonumber(L, 2));
    gSlope.z0 = static_cast<int>(lua_tonumber(L, 3));
    gSlope.x1 = static_cast<int>(lua_tonumber(L, 4));
    gSlope.z1 = static_cast<int>(lua_tonumber(L, 5));
    gSlope.window = std::max(1, std::min(16, static_cast<int>(lua_tonumber(L, 6))));
    gSlope.limit = std::max(0.01f, static_cast<float>(lua_tonumber(L, 7)));
    gSlope.on = true;
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_EdOverlaySlopeOff(lua_State* const)
  {
    gSlope.on = false;
    return 0;
  }

  // ed_slope_at(kind, x, z, window) -> the engine's rating of the cell containing (x, z), or nil
  int __cdecl cfunc_EdSlopeAt(lua_State* const L)
  {
    moho::CHeightField* const field = ViewHeightField();
    const char* const kind = lua_tostring(L, 1);
    if (field == nullptr || kind == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    const int cx = static_cast<int>(std::floor(lua_tonumber(L, 2)));
    const int cz = static_cast<int>(std::floor(lua_tonumber(L, 3)));
    const int n = std::max(1, std::min(16, static_cast<int>(lua_tonumber(L, 4))));
    const float v = SlopeMetric((std::strcmp(kind, "build") == 0) ? 1 : 0, n, cx, cz,
                                [field](int x, int z) { return static_cast<int>(field->GetHeightAt(x, z)); });
    lua_pushnumber(L, static_cast<lua_Number>(v));
    return 1;
  }

  int __cdecl cfunc_EdOverlayRemove(lua_State* const L)
  {
    if (const char* const id = lua_tostring(L, 1)) {
      Lines().erase(id);
      Icons().erase(id);
      Decals().erase(id);
    }
    return 0;
  }

  int __cdecl cfunc_EdOverlayClear(lua_State* const)
  {
    Lines().clear();
    Icons().clear();
    Decals().clear();
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
    lua_pushnumber(L, static_cast<lua_Number>(Lines().size() + Icons().size() + Decals().size()));
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
      (void)new moho::CScrLuaBinder(set, "ed_overlay_slope", &cfunc_EdOverlaySlope, nullptr, "<global>",
                                    "ed_overlay_slope(kind, x0, z0, x1, z1, window, limit) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_slope_off", &cfunc_EdOverlaySlopeOff, nullptr, "<global>", "ed_overlay_slope_off()");
      (void)new moho::CScrLuaBinder(set, "ed_slope_at", &cfunc_EdSlopeAt, nullptr, "<global>",
                                    "ed_slope_at(kind, x, z, window) -> rating of the cell, in the engine's units");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_decal", &cfunc_EdOverlayDecal, nullptr, "<global>",
                                    "ed_overlay_decal(id, x, y, z, texturePath, size [, color, yaw]) -> true | nil, message");
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
      if ((Lines().empty() && Icons().empty() && Decals().empty() && !gSlope.on) || camera == nullptr || batcher == nullptr) {
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

      // ground decals first (splats under everything else), one texture at a time
      {
        std::map<std::string, std::vector<const Decal*>> decalsByTexture;
        for (const auto& entry : Decals()) {
          decalsByTexture[entry.second.texture].push_back(&entry.second);
        }
        for (const auto& group : decalsByTexture) {
          const boost::shared_ptr<CD3DBatchTexture> texture = CD3DBatchTexture::FromFile(group.first.c_str(), 0u);
          if (!texture) {
            continue;
          }
          batcher->SetTexture(texture);
          for (const Decal* const d : group.second) {
            const float h = d->size * 0.5f;
            const float cs = std::cos(d->yaw);
            const float sn = std::sin(d->yaw);
            const float cornersX[4] = {-h, h, h, -h};
            const float cornersZ[4] = {-h, -h, h, h};
            Wm3::Vector2f s[4];
            bool visible = true;
            for (int i = 0; i < 4; ++i) {
              const Wm3::Vector3f p{d->x + cornersX[i] * cs - cornersZ[i] * sn, d->y, d->z + cornersX[i] * sn + cornersZ[i] * cs};
              if (camera->viewport.ProjectViewportDepthRow1(p) < 1.0f) {
                visible = false;
                break;
              }
              s[i] = camera->Project(p, 0.0f, vw, vh, 0.0f);
            }
            if (!visible) {
              continue;
            }
            using V = CD3DPrimBatcher::Vertex;
            batcher->DrawQuad(V{s[0].x, s[0].y, 0.0f, d->argb, 0.0f, 0.0f}, V{s[1].x, s[1].y, 0.0f, d->argb, 1.0f, 0.0f},
                              V{s[2].x, s[2].y, 0.0f, d->argb, 1.0f, 1.0f}, V{s[3].x, s[3].y, 0.0f, d->argb, 0.0f, 1.0f});
          }
          batcher->Flush();
        }
        (void)batcher->Setup("TAlphaBlendLinearSampleNoDepth");
        batcher->SetTexture(CD3DBatchTexture::FromSolidColor(0xFFFFFFFFu));
      }

      // slope gradient over the ground (under every line and icon)
      if (gSlope.on) {
        ++gSlopeFrame;
        if (moho::CHeightField* const field = ViewHeightField(); field != nullptr) {
          const int mapW = field->Width();
          const int mapH = field->Height();
          const int x0 = std::max(0, gSlope.x0);
          const int z0 = std::max(0, gSlope.z0);
          const int x1 = std::min(mapW - 1, std::min(gSlope.x1, x0 + 1100));
          const int z1 = std::min(mapH - 1, std::min(gSlope.z1, z0 + 1100));
          if (x1 > x0 && z1 > z0) {
            SlopeCache& c = gSlopeCache;
            // re-rate when something changed, and now and then so terrain edits show
            if (!c.valid || c.kind != gSlope.kind || c.x0 != x0 || c.z0 != z0 || c.x1 != x1 || c.z1 != z1 || c.window != gSlope.window ||
                gSlopeFrame - c.builtAt > 20u) {
              RebuildSlopeCache(*field, gSlope, x0, z0, x1, z1);
            }

            const int stride = c.stride;
            struct Node { float x, y; bool ok; };
            std::vector<Node> nodes(static_cast<std::size_t>(c.bw + 1) * static_cast<std::size_t>(c.bh + 1));
            for (int j = 0; j <= c.bh; ++j) {
              for (int i = 0; i <= c.bw; ++i) {
                const int wx = std::min(x1, x0 + i * stride);
                const int wz = std::min(z1, z0 + j * stride);
                const Wm3::Vector3f p{static_cast<float>(wx), static_cast<float>(field->GetHeightAt(wx, wz)) / 128.0f + 0.12f, static_cast<float>(wz)};
                Node& nd = nodes[static_cast<std::size_t>(j) * (c.bw + 1) + i];
                nd.ok = camera->viewport.ProjectViewportDepthRow1(p) >= 1.0f;
                if (nd.ok) {
                  const Wm3::Vector2f sp = camera->Project(p, 0.0f, vw, vh, 0.0f);
                  nd.x = sp.x;
                  nd.y = sp.y;
                }
              }
            }

            batcher->SetTexture(CD3DBatchTexture::FromSolidColor(0xFFFFFFFFu));
            using V = CD3DPrimBatcher::Vertex;
            for (int j = 0; j < c.bh; ++j) {
              for (int i = 0; i < c.bw; ++i) {
                const std::uint32_t color = SlopeColor(c.block[static_cast<std::size_t>(j) * c.bw + i], gSlope.limit);
                if ((color >> 24) == 0u) {
                  continue;
                }
                const Node& a = nodes[static_cast<std::size_t>(j) * (c.bw + 1) + i];
                const Node& b = nodes[static_cast<std::size_t>(j) * (c.bw + 1) + i + 1];
                const Node& cc = nodes[static_cast<std::size_t>(j + 1) * (c.bw + 1) + i + 1];
                const Node& d = nodes[static_cast<std::size_t>(j + 1) * (c.bw + 1) + i];
                if (!(a.ok && b.ok && cc.ok && d.ok)) {
                  continue;
                }
                const float minX = std::min(std::min(a.x, b.x), std::min(cc.x, d.x));
                const float maxX = std::max(std::max(a.x, b.x), std::max(cc.x, d.x));
                const float minY = std::min(std::min(a.y, b.y), std::min(cc.y, d.y));
                const float maxY = std::max(std::max(a.y, b.y), std::max(cc.y, d.y));
                if (maxX < 0.0f || maxY < 0.0f || minX > vw || minY > vh) {
                  continue;
                }
                batcher->DrawQuad(V{a.x, a.y, 0.0f, color, 0.0f, 0.0f}, V{b.x, b.y, 0.0f, color, 1.0f, 0.0f},
                                  V{cc.x, cc.y, 0.0f, color, 1.0f, 1.0f}, V{d.x, d.y, 0.0f, color, 0.0f, 1.0f});
              }
            }
            batcher->Flush();
          }
        }
      }

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
