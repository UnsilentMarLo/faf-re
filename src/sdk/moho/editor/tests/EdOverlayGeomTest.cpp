// Stand-alone test of the overlay decal geometry (EdOverlayGeom.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdOverlayGeomTest.cpp -o EdOverlayGeomTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdOverlayGeomTest.cpp
#include <cmath>
#include <cstdio>
#include <vector>

#include "moho/editor/EdOverlayGeom.h"

namespace og = moho::editor::overlaygeom;

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

static bool Near(const float a, const float b, const float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

int main()
{
  const int W = 64, D = 64;
  // a plane: 0.3 per unit in x, 0.1 per unit in z, offset 5
  auto plane = [](const int x, const int z) { return 5.0f + 0.3f * static_cast<float>(x) + 0.1f * static_cast<float>(z); };
  auto planeAt = [](const float x, const float z) { return 5.0f + 0.3f * x + 0.1f * z; };

  // GroundAt: exact on the samples, bilinear (exact for a plane) between them, clamped outside
  CHECK(Near(og::GroundAt(plane, W, D, 10.0f, 20.0f), planeAt(10, 20)), "sample");
  CHECK(Near(og::GroundAt(plane, W, D, 10.37f, 20.81f), planeAt(10.37f, 20.81f)), "between samples on a plane");
  CHECK(Near(og::GroundAt(plane, W, D, 63.0f, 63.0f), planeAt(63, 63)), "last sample");
  CHECK(Near(og::GroundAt(plane, W, D, 63.0f, 10.5f), planeAt(63, 10.5f)), "last column");
  CHECK(Near(og::GroundAt(plane, W, D, -5.0f, -5.0f), planeAt(0, 0)), "outside is the edge sample (low)");
  CHECK(Near(og::GroundAt(plane, W, D, 99.0f, 99.0f), planeAt(63, 63)), "outside is the edge sample (high)");
  // a bump: the four-sample blend, not the nearest sample
  auto bump = [](const int x, const int z) { return (x == 10 && z == 10) ? 4.0f : 0.0f; };
  CHECK(Near(og::GroundAt(bump, W, D, 10.5f, 10.0f), 2.0f), "halfway from a bump");
  CHECK(Near(og::GroundAt(bump, W, D, 10.5f, 10.5f), 1.0f), "centre of a cell with one high corner");
  // degenerate field sizes do not read outside
  auto one = [](const int x, const int z) { CHECK(x == 0 && z == 0, "1 x 1 field read at %d,%d", x, z); return 3.0f; };
  CHECK(Near(og::GroundAt(one, 1, 1, 0.7f, 0.2f), 3.0f), "1 x 1 field");

  // DecalCells: fine near, coarse far
  CHECK(og::DecalCells(2.0f, 0.0f) == 4, "mass splat, unknown screen size: %d", og::DecalCells(2.0f, 0.0f));
  CHECK(og::DecalCells(6.0f, 0.0f) == 12, "hydro splat, unknown screen size: %d", og::DecalCells(6.0f, 0.0f));
  CHECK(og::DecalCells(40.0f, 0.0f) == 16, "big decal, unknown screen size is capped: %d", og::DecalCells(40.0f, 0.0f));
  CHECK(og::DecalCells(6.0f, 20.0f) == 2, "far away: %d", og::DecalCells(6.0f, 20.0f));
  CHECK(og::DecalCells(6.0f, 4.0f) == 1, "very far away: %d", og::DecalCells(6.0f, 4.0f));
  CHECK(og::DecalCells(6.0f, 2000.0f) == 12, "close: bounded by the terrain: %d", og::DecalCells(6.0f, 2000.0f));
  CHECK(og::DecalCells(100.0f, 5000.0f) == 48, "huge close decal is capped: %d", og::DecalCells(100.0f, 5000.0f));
  CHECK(og::DecalCells(0.1f, 0.0f) == 2, "tiny decal: %d", og::DecalCells(0.1f, 0.0f));

  // BuildDecal on the plane: every point on the plane + the lift of the centre
  std::vector<og::DecalPoint> pts;
  const float cx = 30.25f, cz = 31.5f, lift = 0.05f;
  og::BuildDecal(plane, W, D, cx, planeAt(cx, cz) + lift, cz, 6.0f, 0.0f, 12, pts);
  CHECK(pts.size() == 13u * 13u, "point count %zu", pts.size());
  bool onPlane = true;
  for (const og::DecalPoint& p : pts) {
    if (!Near(p.y, planeAt(p.x, p.z) + og::kGroundLift)) onPlane = false;
  }
  CHECK(onPlane, "all points stand a little above the plane");
  // a height read from the nearest sample (a marker on a slope) or a bit off the ground still lies on the ground
  og::BuildDecal(plane, W, D, cx, planeAt(cx, cz) - 0.4f, cz, 6.0f, 0.0f, 4, pts);
  bool lying = true;
  for (const og::DecalPoint& p : pts) {
    if (!Near(p.y, planeAt(p.x, p.z) + og::kGroundLift)) lying = false;
  }
  CHECK(lying, "a centre height 0.4 below the ground still lies on it");
  og::BuildDecal(plane, W, D, cx, planeAt(cx, cz) + lift, cz, 6.0f, 0.0f, 12, pts);
  CHECK(Near(pts[0].x, cx - 3.0f) && Near(pts[0].z, cz - 3.0f) && Near(pts[0].u, 0.0f) && Near(pts[0].v, 0.0f), "corner 0 (-h,-h) uv 0,0");
  CHECK(Near(pts[12].x, cx + 3.0f) && Near(pts[12].z, cz - 3.0f) && Near(pts[12].u, 1.0f) && Near(pts[12].v, 0.0f), "corner 1 (h,-h) uv 1,0");
  CHECK(Near(pts[168].x, cx + 3.0f) && Near(pts[168].z, cz + 3.0f) && Near(pts[168].u, 1.0f) && Near(pts[168].v, 1.0f), "corner 2 (h,h) uv 1,1");
  CHECK(Near(pts[156].x, cx - 3.0f) && Near(pts[156].z, cz + 3.0f) && Near(pts[156].u, 0.0f) && Near(pts[156].v, 1.0f), "corner 3 (-h,h) uv 0,1");
  CHECK(Near(pts[6 * 13 + 6].x, cx) && Near(pts[6 * 13 + 6].z, cz) && Near(pts[6 * 13 + 6].y, planeAt(cx, cz) + og::kGroundLift), "centre point");

  // the same rotated a quarter turn: the same corner order as the flat quad (x' = x cos - z sin, z' = x sin + z cos)
  og::BuildDecal(plane, W, D, cx, planeAt(cx, cz), cz, 6.0f, 1.5707963f, 2, pts);
  CHECK(pts.size() == 9u, "2 x 2 cells: %zu points", pts.size());
  CHECK(Near(pts[0].x, cx + 3.0f) && Near(pts[0].z, cz - 3.0f), "rotated corner 0 at %.2f, %.2f", pts[0].x, pts[0].z);
  CHECK(Near(pts[2].x, cx + 3.0f) && Near(pts[2].z, cz + 3.0f), "rotated corner 1 at %.2f, %.2f", pts[2].x, pts[2].z);

  // a decal given at a mesh surface 20 above the field keeps that offset over a slope
  og::BuildDecal(plane, W, D, cx, planeAt(cx, cz) + 20.0f, cz, 4.0f, 0.0f, 4, pts);
  bool offset = true;
  for (const og::DecalPoint& p : pts) {
    if (!Near(p.y, planeAt(p.x, p.z) + 20.0f)) offset = false;
  }
  CHECK(offset, "an offset of 20 is kept");

  // at the map edge nothing reads outside the field
  int reads = 0;
  auto guarded = [&](const int x, const int z) { ++reads; CHECK(x >= 0 && z >= 0 && x < W && z < D, "read outside the field: %d,%d", x, z); return 1.0f; };
  og::BuildDecal(guarded, W, D, 0.5f, 1.0f, 0.5f, 6.0f, 0.3f, 12, pts);
  og::BuildDecal(guarded, W, D, 63.5f, 1.0f, 63.5f, 6.0f, 0.3f, 12, pts);
  CHECK(reads > 0, "the sampler was used");

  // cells < 1 is one cell
  og::BuildDecal(plane, W, D, cx, 10.0f, cz, 2.0f, 0.0f, 0, pts);
  CHECK(pts.size() == 4u, "zero cells makes one: %zu", pts.size());

  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
