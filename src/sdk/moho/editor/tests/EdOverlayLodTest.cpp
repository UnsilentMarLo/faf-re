// Stand-alone test of the distance adaptive overlay detail (EdOverlayLod.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdOverlayLodTest.cpp -o EdOverlayLodTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdOverlayLodTest.cpp
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "moho/editor/EdOverlayLod.h"

namespace lod = moho::editor::overlaylod;

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    ++gChecks;                                         \
    if (!(cond)) {                                     \
      ++gFailures;                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

// A pinhole camera: eye, looking at a target, 60 degrees vertical field of view, 1280 x 720 pixels; w = distance along the view direction.
struct Cam
{
  float ex, ey, ez, fx, fy, fz, rx, ry, rz, ux, uy, uz, focal;
  Cam(const float ex_, const float ey_, const float ez_, const float tx, const float ty, const float tz) : ex(ex_), ey(ey_), ez(ez_)
  {
    float dx = tx - ex, dy = ty - ey, dz = tz - ez;
    const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
    fx = dx / l; fy = dy / l; fz = dz / l;
    rx = -fz; ry = 0.0f; rz = fx;
    const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    rx /= rl; rz /= rl;
    ux = ry * fz - rz * fy; uy = rz * fx - rx * fz; uz = rx * fy - ry * fx;
    focal = 360.0f / std::tan(30.0f * 3.14159265f / 180.0f);
  }
  lod::GroundNode Project(const float x, const float y, const float z) const
  {
    const float dx = x - ex, dy = y - ey, dz = z - ez;
    const float w = dx * fx + dy * fy + dz * fz;
    lod::GroundNode n;
    n.w = w;
    if (w > 0.05f) {
      const float sx = dx * rx + dy * ry + dz * rz, sy = dx * ux + dy * uy + dz * uz;
      n.x = 640.0f + focal * sx / w;
      n.y = 360.0f - focal * sy / w;
      n.front = true;
    }
    return n;
  }
};

// The probe the engine uses (EdOverlay.cpp ProbeBlock) over flat ground, for a block key.
static lod::NodeInfo Probe(const Cam& cam, const lod::Key& k, const int mapSize)
{
  const int size = 1 << k.level;
  const int x0 = k.bx << k.level, z0 = k.bz << k.level;
  if (x0 >= mapSize || z0 >= mapSize) return {};
  const int x1 = std::min(x0 + size, mapSize), z1 = std::min(z0 + size, mapSize);
  const lod::GroundNode n[4] = {cam.Project(static_cast<float>(x0), 0, static_cast<float>(z0)), cam.Project(static_cast<float>(x1), 0, static_cast<float>(z0)),
                                cam.Project(static_cast<float>(x1), 0, static_cast<float>(z1)), cam.Project(static_cast<float>(x0), 0, static_cast<float>(z1))};
  int front = 0;
  float wSum = 0;
  for (const auto& g : n) { front += g.front ? 1 : 0; wSum += std::max(0.0f, g.w); }
  if (front == 0) return {};
  if (front < 4) return lod::NodeInfo{true, 1.0e9f, 0.0f};
  const float sx[4] = {n[0].x, n[1].x, n[2].x, n[3].x}, sy[4] = {n[0].y, n[1].y, n[2].y, n[3].y};
  const float minX = std::min(std::min(sx[0], sx[1]), std::min(sx[2], sx[3])), maxX = std::max(std::max(sx[0], sx[1]), std::max(sx[2], sx[3]));
  const float minY = std::min(std::min(sy[0], sy[1]), std::min(sy[2], sy[3])), maxY = std::max(std::max(sy[0], sy[1]), std::max(sy[2], sy[3]));
  const float margin = 0.5f * std::max(maxX - minX, maxY - minY) + 16.0f;
  if (maxX < -margin || maxY < -margin || minX > 1280 + margin || minY > 720 + margin) return {};
  return lod::NodeInfo{true, lod::ProjectedSize(sx, sy), 0.25f * wSum};
}

static void Build(const Cam& cam, const int mapSize, lod::RefineState& st, const unsigned frame, const lod::TreeParams& p, lod::Tree& tree)
{
  std::vector<lod::Candidate> roots;
  const int rootsPerSide = (mapSize + 63) / 64;
  for (int bz = 0; bz < rootsPerSide; ++bz) {
    for (int bx = 0; bx < rootsPerSide; ++bx) {
      const lod::Key k{lod::kRootLevel, bx, bz};
      const lod::NodeInfo info = Probe(cam, k, mapSize);
      if (info.visible) roots.push_back(lod::Candidate{k, info});
    }
  }
  lod::BuildTree(roots, [&](const lod::Key& k) { return Probe(cam, k, mapSize); }, st, frame, p, tree);
}

int main()
{
  // ---- projected size, thresholds ----
  {
    const float sx[4] = {0, 100, 100, 0}, sy[4] = {0, 0, 100, 100};
    CHECK(std::fabs(lod::ProjectedSize(sx, sy) - 100.0f) < 1e-3f, "a square 100 px");
    const float tx[4] = {0, 100, 100, 0}, ty[4] = {0, 0, 2, 2};
    CHECK(lod::ProjectedSize(tx, ty) < 12.0f, "a sliver 100 x 2 px is not 100 px (%f)", lod::ProjectedSize(tx, ty));
    const lod::Thresholds th{14.0f, 10.0f};
    CHECK(!lod::ShouldRefine(12.0f, false, th) && lod::ShouldRefine(12.0f, true, th), "between the thresholds the previous state wins");
    CHECK(lod::ShouldRefine(15.0f, false, th) && !lod::ShouldRefine(9.0f, true, th), "above / below both");
  }

  // ---- the tree over a flat map seen from 120 units up, tilted: near blocks are fine, far blocks are coarse ----
  {
    const int map = 1024;
    const Cam cam(512, 120, 300, 512, 0, 520);   // looking at the ground ahead and down
    lod::RefineState st;
    lod::Tree tree;
    lod::TreeParams p;
    p.th = lod::Thresholds{lod::kSlopeRefinePx, lod::kSlopeMergePx};
    p.maxLeaves = lod::kSlopeMaxLeaves;
    Build(cam, map, st, 1, p, tree);
    int minLevel = 99, maxLevel = -1;
    double area = 0;
    for (const auto& l : tree.leaves) {
      minLevel = std::min(minLevel, l.key.level);
      maxLevel = std::max(maxLevel, l.key.level);
      area += std::pow(4.0, l.key.level);
    }
    CHECK(minLevel == 0, "single cells are reached near the camera (finest level %d)", minLevel);
    CHECK(maxLevel >= 3, "the far ground is coarse (coarsest level %d)", maxLevel);
    CHECK(static_cast<int>(tree.leaves.size()) <= p.maxLeaves, "within the leaf budget (%zu)", tree.leaves.size());
    // the nearest leaf is finer than the farthest
    float nearW = 1e9f, farW = 0;
    int nearLevel = 0, farLevel = 0;
    for (const auto& l : tree.leaves) {
      const lod::NodeInfo i = Probe(cam, l.key, map);
      if (i.dist < nearW) { nearW = i.dist; nearLevel = l.key.level; }
      if (i.dist > farW) { farW = i.dist; farLevel = l.key.level; }
    }
    CHECK(nearLevel < farLevel, "near leaf level %d < far leaf level %d", nearLevel, farLevel);
    // every leaf is a proper block of a cut parent chain: no two leaves overlap
    std::set<std::uint64_t> seen;
    bool overlap = false;
    for (const auto& l : tree.leaves) {
      lod::Key k = l.key;
      for (;;) {
        if (k.level != l.key.level && seen.count(lod::Pack(k))) overlap = true;
        if (k.level >= lod::kRootLevel) break;
        k = lod::Key{k.level + 1, k.bx >> 1, k.bz >> 1};
      }
      seen.insert(lod::Pack(l.key));
    }
    CHECK(!overlap, "no leaf lies inside another");
    CHECK(area > 0, "something is drawn");

    // a second frame with the same camera gives the same leaves (the hysteresis is a fixed point)
    lod::Tree again;
    Build(cam, map, st, 2, p, again);
    CHECK(again.leaves.size() == tree.leaves.size(), "same camera, same leaves (%zu vs %zu)", again.leaves.size(), tree.leaves.size());
    CHECK(st.Size() == static_cast<std::size_t>(again.refined), "the state holds exactly the blocks that were cut (%zu vs %d)", st.Size(), again.refined);
  }

  // ---- budget: nearest first ----
  {
    const int map = 1024;
    const Cam cam(512, 120, 300, 512, 0, 520);
    lod::RefineState st;
    lod::Tree small, big;
    lod::TreeParams p;
    p.th = lod::Thresholds{lod::kSlopeRefinePx, lod::kSlopeMergePx};
    p.maxLeaves = 400;
    Build(cam, map, st, 1, p, small);
    CHECK(static_cast<int>(small.leaves.size()) <= 400, "budget respected (%zu)", small.leaves.size());
    int nearLevel = 99;
    float nearW = 1e9f;
    for (const auto& l : small.leaves) {
      const lod::NodeInfo i = Probe(cam, l.key, map);
      if (i.dist < nearW) { nearW = i.dist; nearLevel = l.key.level; }
    }
    lod::RefineState st2;
    p.maxLeaves = 40000;
    Build(cam, map, st2, 1, p, big);
    int bigNearLevel = 99;
    float bigNearW = 1e9f;
    for (const auto& l : big.leaves) {
      const lod::NodeInfo i = Probe(cam, l.key, map);
      if (i.dist < bigNearW) { bigNearW = i.dist; bigNearLevel = l.key.level; }
    }
    CHECK(nearLevel <= bigNearLevel + 1, "even a tiny budget refines the ground under the camera first (level %d vs %d)", nearLevel, bigNearLevel);
    CHECK(small.leaves.size() < big.leaves.size(), "a bigger budget gives more leaves");
  }

  // ---- hysteresis while zooming: fewer flips than without ----
  {
    const int map = 1024;
    lod::TreeParams hys, plain;
    hys.th = lod::Thresholds{14.0f, 10.0f};
    plain.th = lod::Thresholds{14.0f, 14.0f};
    lod::RefineState sh, sp;
    lod::Tree th_, tp;
    std::set<std::uint64_t> prevH, prevP;
    long flipsH = 0, flipsP = 0;
    unsigned frame = 0;
    // the camera height jitters by +-1.5 % around 300 for 200 frames
    for (int i = 0; i < 200; ++i) {
      const float h = 300.0f * (1.0f + 0.015f * std::sin(i * 0.9f));
      const Cam cam(512, h, 300, 512, 0, 520);
      ++frame;
      Build(cam, map, sh, frame, hys, th_);
      Build(cam, map, sp, frame, plain, tp);
      std::set<std::uint64_t> curH, curP;
      for (const auto& l : th_.leaves) curH.insert(lod::Pack(l.key));
      for (const auto& l : tp.leaves) curP.insert(lod::Pack(l.key));
      if (i > 0) {
        for (auto k : curH) flipsH += prevH.count(k) ? 0 : 1;
        for (auto k : curP) flipsP += prevP.count(k) ? 0 : 1;
      }
      prevH = curH;
      prevP = curP;
    }
    CHECK(flipsH < flipsP, "hysteresis: %ld new leaves over the jitter against %ld without", flipsH, flipsP);
  }

  // ---- state is dropped when unused ----
  {
    lod::RefineState st;
    st.Mark(lod::Key{3, 1, 2}, 5);
    st.Mark(lod::Key{4, 0, 0}, 6);
    st.EndFrame(6);
    CHECK(!st.WasRefined(lod::Key{3, 1, 2}) && st.WasRefined(lod::Key{4, 0, 0}) && st.Size() == 1u, "an entry that a frame did not touch is dropped");
  }

  // ---- grid: every line once ----
  {
    const int map = 512;
    const Cam cam(256, 100, 150, 256, 0, 300);
    lod::RefineState st;
    lod::Tree tree;
    lod::TreeParams p;
    p.th = lod::Thresholds{lod::kGridRefinePx, lod::kGridMergePx};
    p.maxLeaves = lod::kGridMaxLeaves;
    Build(cam, map, st, 1, p, tree);
    // unit edges drawn: key = (orientation, x, z)
    std::map<std::tuple<int, int, int>, int> drawn;
    std::set<std::tuple<int, int, int>> wanted;
    for (const auto& l : tree.leaves) {
      const int size = 1 << l.key.level;
      const int x0 = l.key.bx << l.key.level, z0 = l.key.bz << l.key.level, x1 = std::min(x0 + size, map), z1 = std::min(z0 + size, map);
      const int sides = lod::GridSides(tree.all, l.key);
      for (int z = z0; z < z1; ++z) {
        wanted.insert({1, x0, z});
        wanted.insert({1, x1, z});
        if ((sides & lod::kLeft) != 0) ++drawn[{1, x0, z}];
        if ((sides & lod::kRight) != 0 && x0 + size <= map) ++drawn[{1, x1, z}];
      }
      for (int x = x0; x < x1; ++x) {
        wanted.insert({0, x, z0});
        wanted.insert({0, x, z1});
        if ((sides & lod::kTop) != 0) ++drawn[{0, x, z0}];
        if ((sides & lod::kBottom) != 0 && z0 + size <= map) ++drawn[{0, x, z1}];
      }
    }
    int twice = 0, missing = 0;
    for (const auto& kv : drawn) twice += kv.second > 1 ? 1 : 0;
    for (const auto& k : wanted) {
      // the map's far edge is not a line
      if (std::get<0>(k) == 1 && std::get<1>(k) >= map) continue;
      if (std::get<0>(k) == 0 && std::get<2>(k) >= map) continue;
      if (drawn.count(k)) continue;
      // (an edge next to a block that was cut and whose children are off screen is not drawn: only edges that are on screen count)
      const float ex = static_cast<float>(std::get<1>(k)) + (std::get<0>(k) == 0 ? 0.5f : 0.0f), ez = static_cast<float>(std::get<2>(k)) + (std::get<0>(k) == 1 ? 0.5f : 0.0f);
      const lod::GroundNode g = cam.Project(ex, 0, ez);
      if (g.front && g.x > 0 && g.x < 1280 && g.y > 0 && g.y < 720) ++missing;
    }
    CHECK(twice == 0, "no grid line segment is drawn twice (%d)", twice);
    CHECK(missing == 0, "every leaf edge is drawn (%d missing)", missing);
    CHECK(!tree.leaves.empty(), "leaves");
    // lines every cell near the camera, fading far away
    int fine = 0;
    for (const auto& l : tree.leaves) fine += l.key.level == 0 ? 1 : 0;
    CHECK(fine > 0, "a cell grid exists near the camera (%d level 0 leaves)", fine);
    CHECK(lod::GridAlpha(30.0f, false) > lod::GridAlpha(8.0f, false), "the lines fade with the block size");
    CHECK(lod::GridAlpha(30.0f, true) > lod::GridAlpha(30.0f, false), "major lines are brighter");
    CHECK(lod::GridAlpha(1000.0f, true, 1.0f) <= 255, "alpha in range");
    CHECK(lod::IsMajor(0, 0) && !lod::IsMajor(1, 0) && lod::IsMajor(4, 0) && lod::IsMajor(32, 3) && !lod::IsMajor(8, 3), "major lines every fourth block");
  }

  // ---- neighbours that were cut: the leaf grows over that edge ----
  {
    std::unordered_set<std::uint64_t> internal;
    internal.insert(lod::Pack(lod::Key{2, 3, 5}));
    const int g = lod::GrowSides(internal, lod::Key{2, 2, 5});
    CHECK(g == lod::kRight, "the cut neighbour on the right (%d)", g);
    CHECK(lod::GrowSides(internal, lod::Key{2, 3, 4}) == lod::kBottom, "the cut neighbour below");
    CHECK(lod::GrowSides(internal, lod::Key{2, 7, 7}) == 0, "no cut neighbour");
  }

  // ---- node table ----
  {
    lod::NodeTable t;
    int created = 0;
    int calls = 0;
    const auto make = [&](const int x, const int z) { ++calls; lod::GroundNode n; n.x = static_cast<float>(x); n.y = static_cast<float>(z); n.front = true; return n; };
    for (int z = 0; z < 100; ++z) {
      for (int x = 0; x < 100; ++x) t.Get(x, z, make, created);
    }
    CHECK(created == 10000 && calls == 10000 && t.Size() == 10000u, "10000 nodes created (%d)", created);
    const lod::GroundNode n = t.Get(37, 81, make, created);
    CHECK(created == 10000 && n.x == 37.0f && n.y == 81.0f, "a known node is not projected again");
    t.Clear();
    CHECK(t.Size() == 0u, "cleared");
    t.Get(37, 81, make, created);
    CHECK(created == 10001, "after Clear the node is projected again");
  }

  // ---- depth ----
  {
    // identity view, D3D projection: clip z = f / (f - n) * (vz - n), clip w = vz
    const float n = 10.0f, f = 10000.0f;
    const float a = f / (f - n);
    const lod::DepthFit fit = lod::FitDepth(0, 0, a, -a * n, 0, 0, 1, 0);
    CHECK(fit.ok && std::fabs(fit.a - a) < 1e-4f && std::fabs(fit.b - (-a * n)) < 1e-2f, "fit a %f b %f", fit.a, fit.b);
    for (const float w : {20.0f, 100.0f, 500.0f, 3000.0f}) {
      const float clipZ = a * (w - n);
      const float plain = clipZ / w;
      const float biased = lod::BiasedDepth(clipZ, w, fit);
      const float s = lod::DepthBiasWorld(w);
      const float exact = a * (w - s - n) / (w - s);
      CHECK(biased < plain, "w %.0f: moved towards the camera", w);
      CHECK(std::fabs(biased - std::min(1.0f, std::max(0.0f, exact))) < 1e-5f, "w %.0f: the bias is exactly s = %.2f units towards the camera (%f vs %f)", w, s, biased, exact);
    }
    CHECK(lod::DepthBiasWorld(500.0f) > lod::DepthBiasWorld(50.0f), "the bias grows with the distance");
    CHECK(lod::DepthBiasWorld(1.0f) <= 0.4f, "never more than 40 %% of the distance");
    // the depth the pixel projection gives a vertex z (EdOverlay.cpp: depth = 0.5 - 0.5 z)
    for (const float d : {0.0f, 0.3f, 0.99f, 1.0f}) {
      CHECK(std::fabs(0.5f - 0.5f * lod::VertexZ(d) - d) < 1e-6f, "VertexZ round trip %f", d);
    }
    // a rotated, scaled camera: the same fit from tilted columns
    const float wx = 0.3f, wy = -0.8f, wz = 0.5f, w0 = 7.0f, b = -3.0f, a2 = 1.002f;
    const lod::DepthFit fit2 = lod::FitDepth(a2 * wx, a2 * wy, a2 * wz, a2 * w0 + b, wx, wy, wz, w0);
    CHECK(fit2.ok && std::fabs(fit2.a - a2) < 1e-5f && std::fabs(fit2.b - b) < 1e-4f, "tilted columns: a %f b %f", fit2.a, fit2.b);
    CHECK(!lod::FitDepth(1, 1, 1, 1, 0, 0, 0, 1).ok, "a matrix without a w column cannot be fitted");
    // occlusion: a hill nearer than the overlay hides it, the terrain mesh sagging under the overlay does not
    const float hillW = 100.0f, behindW = 140.0f;
    const float hill = a * (hillW - n) / hillW;
    CHECK(lod::BiasedDepth(a * (behindW - n), behindW, fit) > hill, "ground behind a hill stays hidden");
    const float meshW = 100.6f;   // the terrain mesh 0.6 units farther than the height field at 100 (coarse tessellation)
    CHECK(lod::BiasedDepth(a * (hillW - n), hillW, fit) < a * (meshW - n) / meshW, "the overlay wins over a mesh that sags 0.6 units");
    const float farMesh = 500.0f * 1.01f;   // 5 units at 500 away
    CHECK(lod::BiasedDepth(a * (500.0f - n), 500.0f, fit) < a * (farMesh - n) / farMesh, "and at 500 away");
  }

  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
