#include "moho/editor/EdOverlay.h"
#include "moho/editor/EdOverlayDrape.h"
#include "moho/editor/EdOverlayGeom.h"
#include "moho/editor/EdOverlayNodes.h"
#include "moho/editor/EdOverlaySlope.h"

#include <algorithm>
#include <array>
#include <chrono>
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

  // A grid of values on the height samples (brush weights): node (x0 + i * step, z0 + j * step) carries `v[j * nw + i]` (0..255) as the alpha of
  // `rgb`, the cells between the nodes are drawn with smoothly interpolated alpha, draped over the terrain.
  struct NodeGrid
  {
    int x0 = 0, z0 = 0, nw = 0, nh = 0;
    int step = 1;            // world units between neighbouring nodes (a coarse grid covers a big view)
    std::vector<std::uint8_t> v;
    std::uint32_t rgb = 0xFFFFFFu;
    std::uint32_t maxAlpha = 0xC0u;
    std::vector<moho::editor::overlaynodes::Run> runs;   // the cells that draw anything (EdOverlayNodes.h), rebuilt when the values change
    bool runsDirty = true;
  };

  std::map<std::string, NodeGrid>& NodeGrids()
  {
    static std::map<std::string, NodeGrid> grids;
    return grids;
  }

  // scratch buffers of the decal drawing (one decal at a time; kept so a frame with many splats does not allocate for each)
  std::vector<moho::editor::overlaygeom::DecalPoint> gDecalPoints;
  std::vector<Wm3::Vector2f> gDecalScreen;
  std::vector<char> gDecalVisible;

  float gCam[16] = {};  // camera->viewport rows, for ed_overlay_stats
  float gDbg[8] = {};   // ed_overlay_stats: first vertex of the last drawn line (screen x, y, depth, half), viewport w, h, segments drawn, culled

  // ---- slope view: the engine's own terrain ratings as a colour gradient over the ground ------------------------------------------------
  //
  // kind 0 "path": the rule a mobile footprint of `window` x `window` cells is checked with (STIMap::OccupancyCapsOfFootprintAt /
  //   OCCUPY_MobileCheck): the largest height difference between neighbouring samples inside the window, in world units per cell,
  //   against the footprint's MaxSlope (0.75 for every land, amphibious and hover footprint, lua/footprints.lua).
  // kind 1 "build": the rule a structure is checked with (OCCUPY_CheckAreaFlatness): highest minus lowest sample over the skirt
  //   (window x window cells, window + 1 samples per side), against the blueprint's Physics.MaxGroundVariation (1.1 for most).
  //
  // The ratings are kept in a tile cache (EdOverlaySlope.h) until the heights under a tile change (NotifyHeights, called by the document's height sink) or
  // the rule / window changes; a frame only rates tiles that were never rated or went stale, within a time budget, and draws at most kMaxBlocks blocks
  // per side whatever the zoom.
  struct SlopeLayer
  {
    bool on = false;
    int kind = 0;
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;   // cell rectangle [x0, x1) x [z0, z1)
    int window = 1;
    float limit = 0.75f;
  };

  SlopeLayer gSlope;

  unsigned gHeightEpoch = 1;                       // moves with every change of the height field (NotifyHeights)
  moho::editor::overlayslope::Store gSlopeStore;
  unsigned gSlopeFrame = 0;
  constexpr int kSlopeMaxBlocks = 160;             // blocks per side drawn at most
  constexpr double kSlopeBudgetMs = 5.0;           // rating time per frame

  // numbers for ed_overlay_perf (last frame)
  struct OverlayPerf
  {
    double slopeRateMs = 0.0;       // time spent rating tiles
    double slopeDrawMs = 0.0;       // time to project and draw the slope blocks (without the rating)
    int slopeTilesRated = 0;
    int slopePending = 0;           // tiles in view that still wait for their rating
    int slopeQuads = 0;
    int slopeLevel = 0;
    int slopeNodesProjected = 0;
    double gridsMs = 0.0;
    int gridQuads = 0;
    int gridNodesProjected = 0;
    int gridGroups = 0;
  };
  OverlayPerf gPerf;

  double NowMs()
  {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  moho::CHeightField* ViewHeightField()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr || session->mWldMap->mTerrainRes == nullptr) {
      return nullptr;
    }
    return session->mWldMap->mTerrainRes->GetHeightField();
  }

  // The w that GeomCamera3::Project divides by: > 0 in front of the camera (the distance along the view direction), <= 0 behind it. This, not viewport.r[1] (the LOD plane,
  // scaled by the camera's LOD scale), decides whether a point is in front (overlaydrape::InFront).
  float ClipW(const moho::GeomCamera3& camera, const Wm3::Vector3f& p)
  {
    const moho::VMatrix4& m = camera.viewProjection;
    return (m.r[0].w * p.x) + (m.r[1].w * p.y) + (m.r[2].w * p.z) + m.r[3].w;
  }

  // What the camera looked like when a projection was made: the rows of its viewport matrix and the size of the view. A cached projection is good while this is equal.
  struct CamStamp
  {
    float m[18] = {};
    bool operator==(const CamStamp& o) const { return std::memcmp(m, o.m, sizeof(m)) == 0; }
    bool operator!=(const CamStamp& o) const { return !(*this == o); }
  };

  CamStamp MakeStamp(const moho::GeomCamera3& camera, const float vw, const float vh)
  {
    CamStamp s;
    for (int r = 0; r < 4; ++r) {
      s.m[r * 4 + 0] = camera.viewport.r[r].x;
      s.m[r * 4 + 1] = camera.viewport.r[r].y;
      s.m[r * 4 + 2] = camera.viewport.r[r].z;
      s.m[r * 4 + 3] = camera.viewport.r[r].w;
    }
    s.m[16] = vw;
    s.m[17] = vh;
    return s;
  }

  // Screen positions of a grid of ground points (x0 + i * step, z0 + j * step), projected when first asked for and kept while the camera, the heights and the grid
  // stay the same: a static view costs no projection at all, and only the nodes that a drawn cell needs are ever projected.
  struct ProjGrid
  {
    int x0 = 0, z0 = 0, nw = 0, nh = 0, step = 1;
    float lift = 0.0f;
    unsigned heightEpoch = 0;
    CamStamp stamp;
    bool valid = false;
    std::vector<float> sx, sy, sw;     // screen position and clip w of a projected node
    std::vector<std::uint8_t> state;   // 0 not projected yet, 1 on screen side, 2 behind the camera
    unsigned usedFrame = 0;

    void Prepare(const int px0, const int pz0, const int pnw, const int pnh, const int pstep, const float plift, const CamStamp& cs)
    {
      if (valid && x0 == px0 && z0 == pz0 && nw == pnw && nh == pnh && step == pstep && lift == plift && heightEpoch == gHeightEpoch && stamp == cs) {
        return;
      }
      x0 = px0; z0 = pz0; nw = pnw; nh = pnh; step = pstep; lift = plift;
      heightEpoch = gHeightEpoch;
      stamp = cs;
      valid = true;
      const std::size_t n = static_cast<std::size_t>(nw) * static_cast<std::size_t>(nh);
      sx.resize(n);
      sy.resize(n);
      sw.resize(n);
      state.assign(n, 0);
    }

    // Node (i, j): true when it is in front of the camera; its screen position in (x, y).
    bool Get(moho::CHeightField& field, moho::GeomCamera3& camera, const float vw, const float vh, const int i, const int j, float& x, float& y, int& projected, float* const outW = nullptr)
    {
      const std::size_t k = static_cast<std::size_t>(j) * static_cast<std::size_t>(nw) + static_cast<std::size_t>(i);
      if (state[k] == 0) {
        const int wx = std::max(0, std::min(field.Width() - 1, x0 + i * step));
        const int wz = std::max(0, std::min(field.Height() - 1, z0 + j * step));
        const Wm3::Vector3f p{static_cast<float>(wx), static_cast<float>(field.GetHeightAt(wx, wz)) / 128.0f + lift, static_cast<float>(wz)};
        const float w = ClipW(camera, p);
        if (moho::editor::overlaydrape::InFront(w)) {
          const Wm3::Vector2f sp = camera.Project(p, 0.0f, vw, vh, 0.0f);
          sx[k] = sp.x;
          sy[k] = sp.y;
          sw[k] = w;
          state[k] = 1;
        } else {
          state[k] = 2;
        }
        ++projected;
      }
      x = sx[k];
      y = sy[k];
      if (outW != nullptr) {
        *outW = sw[k];
      }
      return state[k] == 1;
    }
  };

  ProjGrid gSlopeProj;
  std::map<std::array<int, 5>, ProjGrid> gGridProj;   // by window (x0, z0, nw, nh, step): shared by every grid that has it

  // A run of cells drawn as ONE straight screen quad only looks right while the nodes it skips lie on the straight line between its end nodes: over bumpy ground the chord
  // cuts under the terrain (which pokes through the draped colour as dark slivers) or floats above it. The longest length <= `len` whose skipped top and bottom edge nodes are
  // within half a pixel of the chord; 1 (a single cell, always exact) when none is.
  template <class Proj>
  int FitRun(Proj& pg, moho::CHeightField& field, moho::GeomCamera3& camera, const float vw, const float vh, const int i, const int j, int len, int& projected)
  {
    for (; len > 1; --len) {
      bool ok = true;
      for (int row = 0; row < 2 && ok; ++row) {
        float x0, y0, x1, y1;
        if (!pg.Get(field, camera, vw, vh, i, j + row, x0, y0, projected) || !pg.Get(field, camera, vw, vh, i + len, j + row, x1, y1, projected)) {
          ok = false;
          break;
        }
        const float dx = x1 - x0, dy = y1 - y0;
        const float chord = std::sqrt(dx * dx + dy * dy);
        for (int k = 1; k < len; ++k) {
          float x, y;
          if (!pg.Get(field, camera, vw, vh, i + k, j + row, x, y, projected)) {
            ok = false;
            break;
          }
          const float off = chord > 1.0e-3f ? std::fabs((x - x0) * dy - (y - y0) * dx) / chord : std::sqrt((x - x0) * (x - x0) + (y - y0) * (y - y0));
          if (off > 0.5f) {
            ok = false;
            break;
          }
        }
      }
      if (ok) {
        return len;
      }
    }
    return 1;
  }

  // ---- draping a patch over the real ground (EdOverlayDrape.h) ---------------------------------------------------------------------------
  //
  // A patch (a block of the slope view, a cell run of a coarse node grid) is ONE screen quad between its four corner nodes, which only follows the terrain when the ground
  // inside is as flat as the quad. DrapeRect cuts it 2 x 2 (recursively, DrapePatch) wherever the real ground at the edge midpoints / centre is more than 0.75 px away from
  // the flat patch, sampling the true heights of the sub-nodes.
  struct DrapeQuad
  {
    float x[4], y[4];
    std::uint32_t c[4];
  };

  struct DrapeScene
  {
    moho::CHeightField* field = nullptr;
    moho::GeomCamera3* camera = nullptr;
    float vw = 0.0f, vh = 0.0f, lift = 0.0f;
    int* projected = nullptr;                          // perf counter of projected nodes
    moho::editor::overlaydrape::QuadBudget budget;
    std::vector<DrapeQuad>* out = nullptr;
  };

  // Clips [x0, x1] x [z0, z1] to the map (what ProjGrid does with its nodes), cuts it up and appends the quads to sc.out. `pre` are the projected corners of the CLIPPED rectangle
  // in the order (x0, z0), (x1, z0), (x1, z1), (x0, z1); `col` the ARGB at the corners of the unclipped one.
  void DrapeRect(DrapeScene& sc, const int x0, const int z0, const int x1, const int z1, const std::uint32_t* const col, const moho::editor::overlaydrape::Node* const pre)
  {
    namespace od = moho::editor::overlaydrape;
    const int maxX = sc.field->Width() - 1, maxZ = sc.field->Height() - 1;
    const int cx0 = std::max(0, std::min(maxX, x0)), cx1 = std::max(0, std::min(maxX, x1));
    const int cz0 = std::max(0, std::min(maxZ, z0)), cz1 = std::max(0, std::min(maxZ, z1));
    if (cx1 <= cx0 || cz1 <= cz0 || x1 <= x0 || z1 <= z0) {
      return;
    }
    const float u0 = static_cast<float>(cx0 - x0) / static_cast<float>(x1 - x0), u1 = static_cast<float>(cx1 - x0) / static_cast<float>(x1 - x0);
    const float v0 = static_cast<float>(cz0 - z0) / static_cast<float>(z1 - z0), v1 = static_cast<float>(cz1 - z0) / static_cast<float>(z1 - z0);
    const std::uint32_t cc[4] = {od::LerpArgb(col, u0, v0), od::LerpArgb(col, u1, v0), od::LerpArgb(col, u1, v1), od::LerpArgb(col, u0, v1)};
    const auto node = [&sc](const int x, const int z, od::Node& n) {
      const Wm3::Vector3f p{static_cast<float>(x), static_cast<float>(sc.field->GetHeightAt(x, z)) / 128.0f + sc.lift, static_cast<float>(z)};
      const float w = ClipW(*sc.camera, p);
      ++*sc.projected;
      if (!od::InFront(w)) {
        return false;
      }
      const Wm3::Vector2f s = sc.camera->Project(p, 0.0f, sc.vw, sc.vh, 0.0f);
      n = od::Node{s.x, s.y, w};
      return true;
    };
    const auto visible = [&sc](const float minX, const float minY, const float maxXs, const float maxYs) {
      return !(maxXs < 0.0f || maxYs < 0.0f || minX > sc.vw || minY > sc.vh);
    };
    const auto emit = [&sc, &visible](const od::Node* const q, const std::uint32_t* const c) {
      const float minX = std::min(std::min(q[0].x, q[1].x), std::min(q[2].x, q[3].x));
      const float maxXs = std::max(std::max(q[0].x, q[1].x), std::max(q[2].x, q[3].x));
      const float minY = std::min(std::min(q[0].y, q[1].y), std::min(q[2].y, q[3].y));
      const float maxYs = std::max(std::max(q[0].y, q[1].y), std::max(q[2].y, q[3].y));
      if (!visible(minX, minY, maxXs, maxYs)) {
        return;
      }
      DrapeQuad d;
      for (int k = 0; k < 4; ++k) {
        d.x[k] = q[k].x;
        d.y[k] = q[k].y;
        d.c[k] = c[k];
      }
      sc.out->push_back(d);
    };
    od::DrapePatch(node, visible, emit, sc.budget, cx0, cz0, cx1, cz1, pre, cc, 0);
  }

  void DrawDrapeQuads(moho::CD3DPrimBatcher& batcher, const std::vector<DrapeQuad>& quads)
  {
    using V = moho::CD3DPrimBatcher::Vertex;
    for (const DrapeQuad& q : quads) {
      batcher.DrawQuad(V{q.x[0], q.y[0], 0.0f, q.c[0], 0.0f, 0.0f}, V{q.x[1], q.y[1], 0.0f, q.c[1], 1.0f, 0.0f}, V{q.x[2], q.y[2], 0.0f, q.c[2], 1.0f, 1.0f},
                       V{q.x[3], q.y[3], 0.0f, q.c[3], 0.0f, 1.0f});
    }
  }

  // The drawn slope blocks of the last frame, kept while nothing they depend on changes: a still view replays them without projecting or sampling anything.
  struct SlopeCache
  {
    bool valid = false;
    CamStamp stamp;
    unsigned heightEpoch = 0;
    int level = 0, bx0 = 0, bz0 = 0, bw = 0, bh = 0;
    std::vector<std::uint8_t> steps;
    std::vector<DrapeQuad> quads;
  };
  SlopeCache gSlopeCache;
  int gSlopeLevel = -1;                          // block level of the last frame (the hysteresis of LevelWithHysteresis)
  constexpr int kSlopeQuadCap = 60000;           // quads of the slope view per frame (a block that would be cut up beyond it stays one quad)
  constexpr int kGridQuadCap = 30000;            // the same for all node grids together

  // green (gentle) -> yellow -> red (at the limit), purple beyond it; flat ground stays clear so the terrain shows through. `step` is a ColorStep
  // (EdOverlaySlope.h): 0 flat, 1 .. kSteps up to the limit, kSteps + 1 beyond.
  std::uint32_t SlopeColor(const int step)
  {
    if (step > moho::editor::overlayslope::kSteps) {
      return 0xB8C018D8u;
    }
    if (step <= 0) {
      return 0u;
    }
    const float t = static_cast<float>(step) / static_cast<float>(moho::editor::overlayslope::kSteps);
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
      sp[i] = {s.x, s.y, ClipW(cam, p)};

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
      if (!moho::editor::overlaydrape::InFront(a.depth) || !moho::editor::overlaydrape::InFront(c.depth)) {
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
        if (moho::editor::overlaydrape::InFront(sp[i].depth)) {
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
    const int v = moho::editor::overlayslope::CellRating((std::strcmp(kind, "build") == 0) ? 1 : 0, n, cx, cz,
                                                         [field](int x, int z) { return static_cast<int>(field->GetHeightAt(x, z)); });
    lua_pushnumber(L, static_cast<lua_Number>(static_cast<float>(v) / 128.0f));
    return 1;
  }

  // ed_overlay_nodes(id, x0, z0, nw, nh, values, rgb [, maxAlpha [, step]])  values: nw * nh numbers 0..255, row major; node (i, j) lies at world (x0 + i * step, z0 + j * step)
  int __cdecl cfunc_EdOverlayNodes(lua_State* const L)
  {
    if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 6) != LUA_TTABLE) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_nodes(id, x0, z0, nw, nh, values, rgb [, maxAlpha])");
      return 2;
    }
    NodeGrid g;
    g.x0 = static_cast<int>(lua_tonumber(L, 2));
    g.z0 = static_cast<int>(lua_tonumber(L, 3));
    g.nw = static_cast<int>(lua_tonumber(L, 4));
    g.nh = static_cast<int>(lua_tonumber(L, 5));
    if (g.nw < 2 || g.nh < 2 || g.nw > 300 || g.nh > 300) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_nodes: size out of range");
      return 2;
    }
    const int count = g.nw * g.nh;
    g.v.resize(static_cast<std::size_t>(count), 0);
    for (int i = 0; i < count; ++i) {
      lua_rawgeti(L, 6, i + 1);
      const double value = lua_tonumber(L, -1);
      g.v[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::max(0.0, std::min(255.0, value)));
      lua_settop(L, -2);
    }
    if (const char* const text = lua_tostring(L, 7)) {
      g.rgb = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16)) & 0xFFFFFFu;
    }
    if (lua_type(L, 8) == LUA_TNUMBER) g.maxAlpha = static_cast<std::uint32_t>(std::max(0.0, std::min(255.0, lua_tonumber(L, 8))));
    if (lua_type(L, 9) == LUA_TNUMBER) g.step = std::max(1, std::min(64, static_cast<int>(lua_tonumber(L, 9))));
    // the same grid again (a caller that sends its layers every time): keep what is there, with its cell runs and projections
    const auto existing = NodeGrids().find(lua_tostring(L, 1));
    if (existing != NodeGrids().end()) {
      const NodeGrid& e = existing->second;
      if (e.x0 == g.x0 && e.z0 == g.z0 && e.nw == g.nw && e.nh == g.nh && e.step == g.step && e.rgb == g.rgb && e.maxAlpha == g.maxAlpha && e.v == g.v) {
        lua_pushboolean(L, 1);
        return 1;
      }
    }
    NodeGrids()[lua_tostring(L, 1)] = std::move(g);
    lua_pushboolean(L, 1);
    return 1;
  }

  // ed_overlay_nodes_rect(id, i0, j0, w, h, values): replaces the nodes [i0, i0 + w) x [j0, j0 + h) of a grid that is already there with `values` (w * h numbers
  // 0..255, row major): the cheap way to send a change of a few nodes instead of the whole grid. nil, message when there is no such grid or the rectangle is outside it.
  int __cdecl cfunc_EdOverlayNodesRect(lua_State* const L)
  {
    if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 6) != LUA_TTABLE) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_nodes_rect(id, i0, j0, w, h, values)");
      return 2;
    }
    const auto it = NodeGrids().find(lua_tostring(L, 1));
    if (it == NodeGrids().end()) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_nodes_rect: no such grid");
      return 2;
    }
    NodeGrid& g = it->second;
    const int i0 = static_cast<int>(lua_tonumber(L, 2));
    const int j0 = static_cast<int>(lua_tonumber(L, 3));
    const int w = static_cast<int>(lua_tonumber(L, 4));
    const int h = static_cast<int>(lua_tonumber(L, 5));
    if (i0 < 0 || j0 < 0 || w < 1 || h < 1 || i0 + w > g.nw || j0 + h > g.nh) {
      lua_pushnil(L);
      lua_pushstring(L, "ed_overlay_nodes_rect: rectangle outside the grid");
      return 2;
    }
    bool changed = false;
    int k = 1;
    for (int j = 0; j < h; ++j) {
      for (int i = 0; i < w; ++i, ++k) {
        lua_rawgeti(L, 6, k);
        const double value = lua_tonumber(L, -1);
        lua_settop(L, -2);
        const std::uint8_t b = static_cast<std::uint8_t>(std::max(0.0, std::min(255.0, value)));
        std::uint8_t& dst = g.v[static_cast<std::size_t>(j0 + j) * static_cast<std::size_t>(g.nw) + static_cast<std::size_t>(i0 + i)];
        if (dst != b) {
          dst = b;
          changed = true;
        }
      }
    }
    if (changed) {
      g.runsDirty = true;
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  // ed_overlay_perf() -> slopeRateMs, slopeDrawMs, slopeTilesRated, slopePending, slopeQuads, slopeBlockLevel, slopeNodesProjected, gridsMs, gridQuads,
  //   gridNodesProjected, gridGroups   (what the last frame spent on the slope view and the node grids)
  int __cdecl cfunc_EdOverlayPerf(lua_State* const L)
  {
    lua_pushnumber(L, gPerf.slopeRateMs);
    lua_pushnumber(L, gPerf.slopeDrawMs);
    lua_pushnumber(L, gPerf.slopeTilesRated);
    lua_pushnumber(L, gPerf.slopePending);
    lua_pushnumber(L, gPerf.slopeQuads);
    lua_pushnumber(L, gPerf.slopeLevel);
    lua_pushnumber(L, gPerf.slopeNodesProjected);
    lua_pushnumber(L, gPerf.gridsMs);
    lua_pushnumber(L, gPerf.gridQuads);
    lua_pushnumber(L, gPerf.gridNodesProjected);
    lua_pushnumber(L, gPerf.gridGroups);
    return 11;
  }

  int __cdecl cfunc_EdOverlayRemove(lua_State* const L)
  {
    if (const char* const id = lua_tostring(L, 1)) {
      Lines().erase(id);
      Icons().erase(id);
      Decals().erase(id);
      NodeGrids().erase(id);
    }
    return 0;
  }

  int __cdecl cfunc_EdOverlayClear(lua_State* const)
  {
    Lines().clear();
    Icons().clear();
    Decals().clear();
    NodeGrids().clear();
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
    lua_pushnumber(L, static_cast<lua_Number>(Lines().size() + Icons().size() + Decals().size() + NodeGrids().size()));
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
      (void)new moho::CScrLuaBinder(set, "ed_overlay_nodes", &cfunc_EdOverlayNodes, nullptr, "<global>",
                                    "ed_overlay_nodes(id, x0, z0, nw, nh, values, rgb [, maxAlpha [, step]]) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_nodes_rect", &cfunc_EdOverlayNodesRect, nullptr, "<global>",
                                    "ed_overlay_nodes_rect(id, i0, j0, w, h, values) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_overlay_perf", &cfunc_EdOverlayPerf, nullptr, "<global>",
                                    "ed_overlay_perf() -> slopeRateMs, slopeDrawMs, slopeTilesRated, slopePending, slopeQuads, slopeBlockLevel, slopeNodesProjected, gridsMs, gridQuads, gridNodesProjected, gridGroups");
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
    void NotifyHeights(const int x0, const int z0, const int x1, const int z1)
    {
      ++gHeightEpoch;
      gSlopeStore.Invalidate(x0, z0, x1, z1);
    }

    void NotifyHeightsAll()
    {
      ++gHeightEpoch;
      gSlopeStore.InvalidateAll();
    }

    void Render(GeomCamera3* const camera, CD3DPrimBatcher* const batcher)
    {
      if ((Lines().empty() && Icons().empty() && Decals().empty() && NodeGrids().empty() && !gSlope.on) || camera == nullptr || batcher == nullptr) {
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
            // The corners of the flat square: they decide whether the decal is on screen at all, and how big it is there (how finely it is cut up).
            const float h = d->size * 0.5f;
            const float cs = std::cos(d->yaw);
            const float sn = std::sin(d->yaw);
            const float cornersX[4] = {-h, h, h, -h};
            const float cornersZ[4] = {-h, -h, h, h};
            Wm3::Vector2f s[4];
            bool allVisible = true;
            for (int i = 0; i < 4; ++i) {
              const Wm3::Vector3f p{d->x + cornersX[i] * cs - cornersZ[i] * sn, d->y, d->z + cornersX[i] * sn + cornersZ[i] * cs};
              if (!moho::editor::overlaydrape::InFront(ClipW(*camera, p))) {
                allVisible = false;
                continue;
              }
              s[i] = camera->Project(p, 0.0f, vw, vh, 0.0f);
            }
            float screenPx = 0.0f;   // widest extent on screen; 0 = a corner is behind the camera (cut up by the terrain alone)
            if (allVisible) {
              if ((s[0].x < -64.0f && s[1].x < -64.0f && s[2].x < -64.0f && s[3].x < -64.0f) ||
                  (s[0].x > vw + 64.0f && s[1].x > vw + 64.0f && s[2].x > vw + 64.0f && s[3].x > vw + 64.0f) ||
                  (s[0].y < -64.0f && s[1].y < -64.0f && s[2].y < -64.0f && s[3].y < -64.0f) ||
                  (s[0].y > vh + 64.0f && s[1].y > vh + 64.0f && s[2].y > vh + 64.0f && s[3].y > vh + 64.0f)) {
                continue;
              }
              for (int i = 0; i < 4; ++i) {
                for (int k = i + 1; k < 4; ++k) {
                  screenPx = std::max(screenPx, std::max(std::fabs(s[i].x - s[k].x), std::fabs(s[i].y - s[k].y)));
                }
              }
              screenPx = std::max(screenPx, 1.0f);
            }

            // Laid onto the terrain in cells (moho/editor/EdOverlayGeom.h): it lies on the slope, and a decal seen from close by is not skewed by the linear
            // interpolation of its picture over one big quad. Without a height field (no terrain loaded) the flat square is drawn as one cell.
            moho::CHeightField* const field = ViewHeightField();
            const int fw = (field != nullptr) ? field->Width() : 0;
            const int fh = (field != nullptr) ? field->Height() : 0;
            std::vector<moho::editor::overlaygeom::DecalPoint>& pts = gDecalPoints;
            if (fw >= 2 && fh >= 2) {
              const auto height = [field](const int x, const int z) { return static_cast<float>(field->GetHeightAt(x, z)) / 128.0f; };
              moho::editor::overlaygeom::BuildDecal(height, fw, fh, d->x, d->y, d->z, d->size, d->yaw,
                                                    moho::editor::overlaygeom::DecalCells(d->size, screenPx), pts);
            } else {
              const auto flat = [](const int, const int) { return 0.0f; };
              moho::editor::overlaygeom::BuildDecal(flat, 2, 2, d->x, d->y, d->z, d->size, d->yaw, 1, pts);
            }
            const int n = static_cast<int>(std::lround(std::sqrt(static_cast<double>(pts.size())))) - 1;
            std::vector<Wm3::Vector2f>& screen = gDecalScreen;
            std::vector<char>& visible = gDecalVisible;
            screen.assign(pts.size(), Wm3::Vector2f{0.0f, 0.0f});
            visible.assign(pts.size(), 0);
            for (std::size_t i = 0; i < pts.size(); ++i) {
              const Wm3::Vector3f p{pts[i].x, pts[i].y, pts[i].z};
              if (moho::editor::overlaydrape::InFront(ClipW(*camera, p))) {
                screen[i] = camera->Project(p, 0.0f, vw, vh, 0.0f);
                visible[i] = 1;
              }
            }
            using V = CD3DPrimBatcher::Vertex;
            for (int j = 0; j < n; ++j) {
              for (int i = 0; i < n; ++i) {
                const std::size_t i00 = static_cast<std::size_t>(j) * (n + 1) + i;
                const std::size_t i10 = i00 + 1;
                const std::size_t i11 = i00 + (n + 1) + 1;
                const std::size_t i01 = i00 + (n + 1);
                if (!visible[i00] || !visible[i10] || !visible[i11] || !visible[i01]) {
                  continue;
                }
                batcher->DrawQuad(V{screen[i00].x, screen[i00].y, 0.0f, d->argb, pts[i00].u, pts[i00].v},
                                  V{screen[i10].x, screen[i10].y, 0.0f, d->argb, pts[i10].u, pts[i10].v},
                                  V{screen[i11].x, screen[i11].y, 0.0f, d->argb, pts[i11].u, pts[i11].v},
                                  V{screen[i01].x, screen[i01].y, 0.0f, d->argb, pts[i01].u, pts[i01].v});
              }
            }
          }
          batcher->Flush();
        }
        (void)batcher->Setup("TAlphaBlendLinearSampleNoDepth");
        batcher->SetTexture(CD3DBatchTexture::FromSolidColor(0xFFFFFFFFu));
      }

      // slope gradient over the ground (under every line and icon)
      gPerf.slopeRateMs = 0.0;
      gPerf.slopeDrawMs = 0.0;
      gPerf.slopeTilesRated = 0;
      gPerf.slopePending = 0;
      gPerf.slopeQuads = 0;
      gPerf.slopeNodesProjected = 0;
      gPerf.gridsMs = 0.0;
      gPerf.gridQuads = 0;
      gPerf.gridNodesProjected = 0;
      gPerf.gridGroups = 0;
      const CamStamp stamp = MakeStamp(*camera, vw, vh);
      {
        // a new map (another height field): every projection and rating is stale
        static const void* lastField = nullptr;
        static int lastW = 0, lastH = 0;
        moho::CHeightField* const f = ViewHeightField();
        const int fw = (f != nullptr) ? f->Width() : 0;
        const int fh = (f != nullptr) ? f->Height() : 0;
        if (f != lastField || fw != lastW || fh != lastH) {
          lastField = f;
          lastW = fw;
          lastH = fh;
          gSlopeLevel = -1;
          gSlopeCache.valid = false;
          ++gHeightEpoch;
          gSlopeStore.Reset(fw, fh);
        }
      }
      if (gSlope.on) {
        ++gSlopeFrame;
        if (moho::CHeightField* const field = ViewHeightField(); field != nullptr) {
          namespace sl = moho::editor::overlayslope;
          const int mapW = field->Width();
          const int mapH = field->Height();
          const int x0 = std::max(0, gSlope.x0);
          const int z0 = std::max(0, gSlope.z0);
          const int x1 = std::min(mapW - 1, gSlope.x1);
          const int z1 = std::min(mapH - 1, gSlope.z1);
          if (x1 > x0 && z1 > z0) {
            if (gSlopeStore.Width() != mapW || gSlopeStore.Height() != mapH) {
              gSlopeStore.Reset(mapW, mapH);
            }
            gSlopeStore.SetKey(gSlope.kind, gSlope.window);

            // the block size: a power of two of cells, so that at most kSlopeMaxBlocks blocks per side are drawn; blocks are aligned to multiples of their size, so
            // they do not shift while the camera pans
            // (with hysteresis: the size of the view jitters with the camera, and a flip of the level re-projects everything)
            const int level = sl::LevelWithHysteresis(std::max(x1 - x0, z1 - z0), kSlopeMaxBlocks, gSlopeLevel);
            gSlopeLevel = level;
            const int stride = 1 << level;
            const int bx0 = x0 >> level;
            const int bz0 = z0 >> level;
            const int bw = ((x1 + stride - 1) >> level) - bx0;
            const int bh = ((z1 + stride - 1) >> level) - bz0;

            // rate the tiles of the view that were never rated or went stale (the dirty ones first, then the ones nearest the middle of the view), within a time budget
            const double tRate = NowMs();
            {
              const int tx0 = (bx0 << level) / sl::kTile;
              const int tz0 = (bz0 << level) / sl::kTile;
              const int tx1 = std::min(gSlopeStore.TilesX() - 1, ((bx0 + bw) << level) / sl::kTile);
              const int tz1 = std::min(gSlopeStore.TilesZ() - 1, ((bz0 + bh) << level) / sl::kTile);
              struct Todo { double key; int tx, tz; };
              std::vector<Todo> todo;
              const double mx = 0.5 * (x0 + x1) / sl::kTile;
              const double mz = 0.5 * (z0 + z1) / sl::kTile;
              for (int tz = tz0; tz <= tz1; ++tz) {
                for (int tx = tx0; tx <= tx1; ++tx) {
                  gSlopeStore.Touch(tx, tz, gSlopeFrame);
                  if (gSlopeStore.NeedsRating(tx, tz)) {
                    const sl::Tile* const t = gSlopeStore.Find(tx, tz);
                    const bool stale = t != nullptr && !t->data.empty();
                    const double dx = tx + 0.5 - mx, dz = tz + 0.5 - mz;
                    todo.push_back(Todo{(stale ? -1.0e9 : 0.0) + dx * dx + dz * dz, tx, tz});
                  }
                }
              }
              std::sort(todo.begin(), todo.end(), [](const Todo& a, const Todo& b) { return a.key < b.key; });
              const auto sampler = [field](const int x, const int z) { return static_cast<int>(field->GetHeightAt(x, z)); };
              std::size_t done = 0;
              for (; done < todo.size(); ++done) {
                gSlopeStore.Rate(todo[done].tx, todo[done].tz, sampler, gSlopeFrame);
                ++gPerf.slopeTilesRated;
                if (NowMs() - tRate > kSlopeBudgetMs) {
                  ++done;
                  break;
                }
              }
              gPerf.slopePending = static_cast<int>(todo.size() - done);
              if (gSlopeStore.Built() > 2600u) {
                gSlopeStore.Trim(gSlopeFrame, 300u);   // an explored 80 km map keeps the tiles near the camera, not all 4096
              }
            }
            const double tDraw = NowMs();
            gPerf.slopeRateMs = tDraw - tRate;
            gPerf.slopeLevel = level;

            std::vector<std::uint8_t> steps(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh));
            for (int j = 0; j < bh; ++j) {
              for (int i = 0; i < bw; ++i) {
                const float value = static_cast<float>(gSlopeStore.Block(level, bx0 + i, bz0 + j)) / 128.0f;
                steps[static_cast<std::size_t>(j) * bw + i] = static_cast<std::uint8_t>(sl::ColorStep(value, gSlope.limit));
              }
            }

            batcher->SetTexture(CD3DBatchTexture::FromSolidColor(0xFFFFFFFFu));
            // Nothing changed since the last frame (camera, heights, view, colours): replay its quads.
            SlopeCache& cache = gSlopeCache;
            const bool same = cache.valid && cache.stamp == stamp && cache.heightEpoch == gHeightEpoch && cache.level == level && cache.bx0 == bx0 && cache.bz0 == bz0 &&
                              cache.bw == bw && cache.bh == bh && cache.steps == steps;
            if (!same) {
              cache.valid = false;
              cache.quads.clear();
              gSlopeProj.Prepare(bx0 << level, bz0 << level, bw + 1, bh + 1, stride, 0.12f, stamp);
              namespace od = moho::editor::overlaydrape;
              DrapeScene sc;
              sc.field = field;
              sc.camera = camera;
              sc.vw = vw;
              sc.vh = vh;
              sc.lift = 0.12f;
              sc.projected = &gPerf.slopeNodesProjected;
              sc.budget.cap = kSlopeQuadCap;
              // every block that is drawn counts one quad from the start, so the blocks drawn first cannot use up the quads the others need
              sc.budget.committed = static_cast<int>(steps.size() - static_cast<std::size_t>(std::count(steps.begin(), steps.end(), static_cast<std::uint8_t>(0))));
              sc.out = &cache.quads;
              constexpr int kMaxRunBlocks = 8;
              for (int j = 0; j < bh; ++j) {
                int i = 0;
                while (i < bw) {
                  const int st = steps[static_cast<std::size_t>(j) * bw + i];
                  float ax, ay, bxs, bys, cx, cy, dx, dy;
                  if (st == 0 || !gSlopeProj.Get(*field, *camera, vw, vh, i, j, ax, ay, gPerf.slopeNodesProjected) ||
                      !gSlopeProj.Get(*field, *camera, vw, vh, i, j + 1, dx, dy, gPerf.slopeNodesProjected)) {
                    ++i;
                    continue;
                  }
                  // a run of neighbouring blocks with the same colour is one patch
                  int len = 1;
                  bool ok = gSlopeProj.Get(*field, *camera, vw, vh, i + 1, j, bxs, bys, gPerf.slopeNodesProjected) &&
                            gSlopeProj.Get(*field, *camera, vw, vh, i + 1, j + 1, cx, cy, gPerf.slopeNodesProjected);
                  while (ok && len < kMaxRunBlocks && i + len < bw && steps[static_cast<std::size_t>(j) * bw + i + len] == st) {
                    float nbx, nby, ncx, ncy;
                    if (!gSlopeProj.Get(*field, *camera, vw, vh, i + len + 1, j, nbx, nby, gPerf.slopeNodesProjected) ||
                        !gSlopeProj.Get(*field, *camera, vw, vh, i + len + 1, j + 1, ncx, ncy, gPerf.slopeNodesProjected)) {
                      break;
                    }
                    bxs = nbx; bys = nby; cx = ncx; cy = ncy;
                    ++len;
                  }
                  if (ok && len > 1) {
                    len = FitRun(gSlopeProj, *field, *camera, vw, vh, i, j, len, gPerf.slopeNodesProjected);
                  }
                  // the corners of the run with their clip w, then the patch is draped over the ground between them
                  od::Node pre[4];
                  ok = ok && gSlopeProj.Get(*field, *camera, vw, vh, i, j, pre[0].x, pre[0].y, gPerf.slopeNodesProjected, &pre[0].w) &&
                       gSlopeProj.Get(*field, *camera, vw, vh, i + len, j, pre[1].x, pre[1].y, gPerf.slopeNodesProjected, &pre[1].w) &&
                       gSlopeProj.Get(*field, *camera, vw, vh, i + len, j + 1, pre[2].x, pre[2].y, gPerf.slopeNodesProjected, &pre[2].w) &&
                       gSlopeProj.Get(*field, *camera, vw, vh, i, j + 1, pre[3].x, pre[3].y, gPerf.slopeNodesProjected, &pre[3].w);
                  if (ok) {
                    const std::uint32_t color = SlopeColor(st);
                    const std::uint32_t col[4] = {color, color, color, color};
                    const int wx0 = (bx0 << level) + i * stride, wz0 = (bz0 << level) + j * stride;
                    DrapeRect(sc, wx0, wz0, wx0 + len * stride, wz0 + stride, col, pre);
                  }
                  i += len;
                }
              }
              cache.stamp = stamp;
              cache.heightEpoch = gHeightEpoch;
              cache.level = level;
              cache.bx0 = bx0;
              cache.bz0 = bz0;
              cache.bw = bw;
              cache.bh = bh;
              cache.steps = steps;
              cache.valid = true;
            }
            DrawDrapeQuads(*batcher, cache.quads);
            gPerf.slopeQuads = static_cast<int>(cache.quads.size());
            batcher->Flush();
            gPerf.slopeDrawMs = NowMs() - tDraw;
          }
        }
      }

      // value grids (brush weights, terrain type layers) draped over the ground
      static unsigned gridFrame = 0;
      if (NodeGrids().empty()) {
        gGridProj.clear();
      } else if (moho::CHeightField* const field = ViewHeightField(); field != nullptr) {
        ++gridFrame;
        const double tGrids = NowMs();
        batcher->SetTexture(CD3DBatchTexture::FromSolidColor(0xFFFFFFFFu));
        namespace od = moho::editor::overlaydrape;
        static std::vector<DrapeQuad> gridQuads;   // kept between frames so a frame does not allocate
        gridQuads.clear();
        DrapeScene sc;
        sc.field = field;
        sc.camera = camera;
        sc.vw = vw;
        sc.vh = vh;
        sc.lift = 0.14f;
        sc.projected = &gPerf.gridNodesProjected;
        sc.budget.cap = kGridQuadCap;
        sc.out = &gridQuads;
        for (auto& entry : NodeGrids()) {
          sc.budget.committed += static_cast<int>(entry.second.runs.size());
        }
        for (auto& entry : NodeGrids()) {
          NodeGrid& g = entry.second;
          if (g.runsDirty) {
            moho::editor::overlaynodes::CollectRuns(g.v.data(), g.nw, g.nh, g.maxAlpha, g.runs);
            g.runsDirty = false;
          }
          if (g.runs.empty()) {
            continue;
          }
          // grids with the same window share their projected nodes
          ProjGrid& pg = gGridProj[std::array<int, 5>{g.x0, g.z0, g.nw, g.nh, g.step}];
          pg.Prepare(g.x0, g.z0, g.nw, g.nh, g.step, 0.14f, stamp);
          pg.usedFrame = gridFrame;
          const auto colorAt = [&g](const int i, const int j) {
            return (moho::editor::overlaynodes::NodeAlpha(g.v[static_cast<std::size_t>(j) * g.nw + i], g.maxAlpha) << 24) | g.rgb;
          };
          for (const moho::editor::overlaynodes::Run& run : g.runs) {
            // a long run is split where the ground is not straight (FitRun)
            int pos = 0;
            while (pos < run.len) {
              const int i = run.i + pos, j = run.j;
              const int len = run.len > 1 ? FitRun(pg, *field, *camera, vw, vh, i, j, run.len - pos, gPerf.gridNodesProjected) : 1;
              pos += len;
              // the corners with their clip w; the patch (a run of cells of a coarse grid) is draped over the ground between them (DrapeRect)
              od::Node pre[4];
              if (!pg.Get(*field, *camera, vw, vh, i, j, pre[0].x, pre[0].y, gPerf.gridNodesProjected, &pre[0].w) ||
                  !pg.Get(*field, *camera, vw, vh, i + len, j, pre[1].x, pre[1].y, gPerf.gridNodesProjected, &pre[1].w) ||
                  !pg.Get(*field, *camera, vw, vh, i + len, j + 1, pre[2].x, pre[2].y, gPerf.gridNodesProjected, &pre[2].w) ||
                  !pg.Get(*field, *camera, vw, vh, i, j + 1, pre[3].x, pre[3].y, gPerf.gridNodesProjected, &pre[3].w)) {
                continue;
              }
              const std::uint32_t col[4] = {colorAt(i, j), colorAt(i + len, j), colorAt(i + len, j + 1), colorAt(i, j + 1)};
              DrapeRect(sc, g.x0 + i * g.step, g.z0 + j * g.step, g.x0 + (i + len) * g.step, g.z0 + (j + 1) * g.step, col, pre);
            }
          }
        }
        DrawDrapeQuads(*batcher, gridQuads);
        gPerf.gridQuads = static_cast<int>(gridQuads.size());
        batcher->Flush();
        for (auto it = gGridProj.begin(); it != gGridProj.end();) {
          if (it->second.usedFrame != gridFrame) {
            it = gGridProj.erase(it);
          } else {
            ++it;
          }
        }
        gPerf.gridGroups = static_cast<int>(gGridProj.size());
        gPerf.gridsMs = NowMs() - tGrids;
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
          if (!moho::editor::overlaydrape::InFront(ClipW(*camera, p))) {
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
