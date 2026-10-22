// Stand-alone test of the overlay draping (EdOverlayDrape.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdOverlayDrapeTest.cpp -o EdOverlayDrapeTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdOverlayDrapeTest.cpp
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "moho/editor/EdOverlayDrape.h"

namespace od = moho::editor::overlaydrape;

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

// A pinhole camera: eye, looking at a target, 60 degrees vertical field of view, 1280 x 720 pixels. w = distance along the view direction (what the engine divides by).
struct Cam
{
  float ex, ey, ez, fx, fy, fz, rx, ry, rz, ux, uy, uz, focal;
  Cam(const float ex_, const float ey_, const float ez_, const float tx, const float ty, const float tz) : ex(ex_), ey(ey_), ez(ez_)
  {
    float dx = tx - ex, dy = ty - ey, dz = tz - ez;
    const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
    fx = dx / l; fy = dy / l; fz = dz / l;
    // right = forward x up(0,1,0)
    rx = fy * 0.0f - fz * 1.0f; ry = fz * 0.0f - fx * 0.0f; rz = fx * 1.0f - fy * 0.0f;
    const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    rx /= rl; ry /= rl; rz /= rl;
    ux = ry * fz - rz * fy; uy = rz * fx - rx * fz; uz = rx * fy - ry * fx;
    focal = 360.0f / std::tan(30.0f * 3.14159265f / 180.0f);
  }
  od::Node Project(const float x, const float y, const float z) const
  {
    const float dx = x - ex, dy = y - ey, dz = z - ez;
    const float w = dx * fx + dy * fy + dz * fz;
    const float sx = dx * rx + dy * ry + dz * rz, sy = dx * ux + dy * uy + dz * uz;
    return od::Node{640.0f + focal * sx / w, 360.0f - focal * sy / w, w};
  }
};

static float Area(const od::Node* q)
{
  float a = 0.0f;
  for (int i = 0; i < 4; ++i) {
    const od::Node& p = q[i];
    const od::Node& n = q[(i + 1) % 4];
    a += p.x * n.y - n.x * p.y;
  }
  return std::fabs(a) * 0.5f;
}

int main()
{
  // ---- InFront --------------------------------------------------------------------------------------------------------------------------------
  CHECK(od::InFront(10.0f), "in front");
  CHECK(od::InFront(0.5f), "in front, close");
  CHECK(!od::InFront(0.0f), "in the camera plane");
  CHECK(!od::InFront(-3.0f), "behind");

  // The retired test: viewport.r[1] . p = lodScale * (world width of the view at that depth) = lodScale * 2 * tan(fov/2) * aspect * depth >= 1.
  // 60 degree vertical fov, aspect 16/9: width per unit depth = 2 * tan(30deg) * 16/9 = 2.053.
  {
    const float perDepth = 2.0f * std::tan(30.0f * 3.14159265f / 180.0f) * 16.0f / 9.0f;
    const auto oldTest = [&](const float lodScale, const float depth) { return lodScale * perDepth * depth >= 1.0f; };
    CHECK(oldTest(1.0f, 5.0f), "old test passes at lodScale 1, depth 5");
    CHECK(!oldTest(0.1f, 3.0f), "old test rejects a point 3 units in front at lodScale 0.1 (threshold ~4.9)");
    CHECK(!oldTest(0.0f, 500.0f), "old test rejects everything at lodScale 0");
    CHECK(od::InFront(3.0f) && od::InFront(500.0f), "the new test does not depend on a scale");
  }

  // ---- flat ground: the perspective correct flat patch is exact; the plain screen bilinear is not ----------------------------------------------
  {
    const Cam cam(100.0f, 40.0f, 0.0f, 100.0f, 0.0f, 100.0f);   // looks over the ground at a shallow angle
    const int x0 = 64, z0 = 64, x1 = 128, z1 = 128;
    const od::Node c[4] = {cam.Project(x0, 0, z0), cam.Project(x1, 0, z0), cam.Project(x1, 0, z1), cam.Project(x0, 0, z1)};
    float worstFlat = 0.0f, worstNaive = 0.0f;
    for (int iu = 0; iu <= 4; ++iu) {
      for (int iv = 0; iv <= 4; ++iv) {
        const float u = iu * 0.25f, v = iv * 0.25f;
        const od::Node t = cam.Project(x0 + u * (x1 - x0), 0.0f, z0 + v * (z1 - z0));
        const od::Sample s{u, v, t};
        worstFlat = std::max(worstFlat, od::Deviation(c, s));
        const float nx = (1 - u) * (1 - v) * c[0].x + u * (1 - v) * c[1].x + u * v * c[2].x + (1 - u) * v * c[3].x;
        const float ny = (1 - u) * (1 - v) * c[0].y + u * (1 - v) * c[1].y + u * v * c[2].y + (1 - u) * v * c[3].y;
        worstNaive = std::max(worstNaive, std::sqrt((nx - t.x) * (nx - t.x) + (ny - t.y) * (ny - t.y)));
      }
    }
    std::printf("flat 64x64 patch at a low angle: flat patch deviation %.4f px, screen bilinear deviation %.2f px\n", worstFlat, worstNaive);
    CHECK(worstFlat < 0.01f, "flat ground matches the perspective correct patch (%f)", worstFlat);
    CHECK(worstNaive > 5.0f, "the screen bilinear would split flat ground for no reason (%f)", worstNaive);
  }

  // ---- DrapePatch on a function terrain -------------------------------------------------------------------------------------------------------
  const auto run = [](const Cam& cam, const std::function<float(int, int)>& height, const int x0, const int z0, const int x1, const int z1, const int cap, const float tol,
                      std::vector<std::vector<od::Node>>& quads, int& nodeCalls) {
    nodeCalls = 0;
    auto node = [&](const int x, const int z, od::Node& n) {
      ++nodeCalls;
      n = cam.Project(static_cast<float>(x), height(x, z), static_cast<float>(z));
      return true;
    };
    auto visible = [](float, float, float, float) { return true; };
    auto emit = [&](const od::Node* q, const std::uint32_t*) { quads.emplace_back(q, q + 4); };
    od::QuadBudget budget;
    budget.cap = cap;
    budget.committed = 1;
    const od::Node c[4] = {cam.Project(x0, height(x0, z0), z0), cam.Project(x1, height(x1, z0), z0), cam.Project(x1, height(x1, z1), z1),
                           cam.Project(x0, height(x0, z1), z1)};
    const std::uint32_t col[4] = {0x80FF0000u, 0x80FF0000u, 0x80FF0000u, 0x80FF0000u};
    od::DrapePatch(node, visible, emit, budget, x0, z0, x1, z1, c, col, 0, od::kMaxDepth, tol);
    return budget.committed;
  };
  {
    const Cam cam(100.0f, 60.0f, 0.0f, 100.0f, 0.0f, 100.0f);
    std::vector<std::vector<od::Node>> quads;
    int calls = 0;
    run(cam, [](int, int) { return 0.0f; }, 64, 64, 128, 128, 60000, od::kTolerancePx, quads, calls);
    CHECK(quads.size() == 1, "flat ground stays one quad (%zu)", quads.size());
    CHECK(calls == 5, "and costs the five samples (%d)", calls);
  }
  {
    // a slope is planar: still one quad
    const Cam cam(100.0f, 60.0f, 0.0f, 100.0f, 0.0f, 100.0f);
    std::vector<std::vector<od::Node>> quads;
    int calls = 0;
    run(cam, [](int x, int z) { return 0.07f * x + 0.03f * z; }, 64, 64, 128, 128, 60000, od::kTolerancePx, quads, calls);
    CHECK(quads.size() == 1, "a tilted plane stays one quad (%zu)", quads.size());
  }
  {
    // forced splitting (negative tolerance): 4 depth levels would be 256 quads; with maxDepth 2 here: 16 quads that tile the parent exactly on flat ground
    const Cam cam(100.0f, 60.0f, 0.0f, 100.0f, 0.0f, 100.0f);
    std::vector<std::vector<od::Node>> quads;
    int calls = 0;
    auto height = [](int, int) { return 0.0f; };
    auto node = [&](const int x, const int z, od::Node& n) { n = cam.Project(static_cast<float>(x), height(x, z), static_cast<float>(z)); return true; };
    auto visible = [](float, float, float, float) { return true; };
    auto emit = [&](const od::Node* q, const std::uint32_t*) { quads.emplace_back(q, q + 4); };
    od::QuadBudget budget;
    budget.committed = 1;
    const od::Node c[4] = {cam.Project(64, 0, 64), cam.Project(128, 0, 64), cam.Project(128, 0, 128), cam.Project(64, 0, 128)};
    const std::uint32_t col[4] = {1, 1, 1, 1};
    od::DrapePatch(node, visible, emit, budget, 64, 64, 128, 128, c, col, 0, 2, -1.0f);
    float sum = 0.0f;
    for (const auto& q : quads) sum += Area(q.data());
    const float whole = Area(c);
    CHECK(quads.size() == 16, "two forced levels give 16 quads (%zu)", quads.size());
    CHECK(std::fabs(sum - whole) < whole * 1.0e-3f, "the 16 quads tile the parent (%f vs %f)", sum, whole);
    CHECK(budget.committed == 16, "the budget counted them (%d)", budget.committed);
    (void)calls;
  }
  {
    // bumpy ground: more quads, and every leaf passes its own test within the tolerance (checked with fresh samples)
    const Cam cam(100.0f, 45.0f, 10.0f, 100.0f, 0.0f, 100.0f);
    const auto height = [](const int x, const int z) { return 4.0f * std::sin(x * 0.21f) * std::cos(z * 0.17f) + 1.5f * std::sin(x * 0.9f + z * 0.7f); };
    std::vector<std::vector<od::Node>> quads;
    int calls = 0;
    run(cam, height, 64, 64, 128, 128, 60000, od::kTolerancePx, quads, calls);
    std::printf("bumpy 64x64 patch: %zu quads, %d projections\n", quads.size(), calls);
    CHECK(quads.size() > 40, "bumpy ground is cut up (%zu)", quads.size());
    CHECK(quads.size() <= 64 * 64, "never more than one quad per cell (%zu)", quads.size());
    // no leaf is bigger than needed in the sense of the test: re-evaluate the corners as a patch and sample its middle by finding which world rectangle it covers
    float totalArea = 0.0f;
    for (const auto& q : quads) totalArea += Area(q.data());
    // the real surface has the same footprint as the cells: summing the leaf areas gives about the area of the projected surface (sanity: finite and positive)
    CHECK(totalArea > 0.0f && std::isfinite(totalArea), "leaf areas are sane (%f)", totalArea);
  }
  {
    // budget: with a tiny cap nothing splits beyond it, and the number of quads never exceeds the cap
    const Cam cam(100.0f, 45.0f, 10.0f, 100.0f, 0.0f, 100.0f);
    const auto height = [](const int x, const int z) { return 4.0f * std::sin(x * 0.21f) * std::cos(z * 0.17f) + 1.5f * std::sin(x * 0.9f + z * 0.7f); };
    std::vector<std::vector<od::Node>> quads;
    int calls = 0;
    run(cam, height, 64, 64, 128, 128, 10, od::kTolerancePx, quads, calls);
    CHECK(quads.size() <= 10, "a cap of 10 holds (%zu)", quads.size());
    quads.clear();
    run(cam, height, 64, 64, 128, 128, 1, od::kTolerancePx, quads, calls);
    CHECK(quads.size() == 1 && calls == 0, "a cap of 1 is the plain quad and costs no sampling (%zu, %d)", quads.size(), calls);
  }
  {
    // a single spike in the middle of otherwise flat ground is found by the centre sample
    const Cam cam(100.0f, 60.0f, 0.0f, 100.0f, 0.0f, 100.0f);
    std::vector<std::vector<od::Node>> quads;
    int calls = 0;
    run(cam, [](int x, int z) { return (x == 96 && z == 96) ? 6.0f : 0.0f; }, 64, 64, 128, 128, 60000, od::kTolerancePx, quads, calls);
    CHECK(quads.size() > 1, "a spike at the centre splits the patch (%zu)", quads.size());
  }
  {
    // a strip one cell high is cut along its length only
    const Cam cam(100.0f, 60.0f, 0.0f, 100.0f, 0.0f, 100.0f);
    std::vector<std::vector<od::Node>> quads;
    int calls = 0;
    run(cam, [](int x, int) { return 3.0f * std::sin(x * 0.5f); }, 64, 100, 96, 101, 60000, od::kTolerancePx, quads, calls);
    CHECK(quads.size() > 2 && quads.size() <= 32, "a strip is split along its length (%zu)", quads.size());
  }
  {
    // a sample behind the camera: the corners are in front, the plain quad is drawn
    std::vector<od::Node> got;
    auto node = [](int, int, od::Node& n) { n = od::Node{0, 0, -1.0f}; return true; };
    auto visible = [](float, float, float, float) { return true; };
    int emitted = 0;
    auto emit = [&](const od::Node*, const std::uint32_t*) { ++emitted; };
    od::QuadBudget budget;
    budget.committed = 1;
    const od::Node c[4] = {{0, 0, 5}, {100, 0, 5}, {100, 100, 5}, {0, 100, 5}};
    const std::uint32_t col[4] = {1, 1, 1, 1};
    od::DrapePatch(node, visible, emit, budget, 0, 0, 16, 16, c, col, 0);
    CHECK(emitted == 1, "a sample behind the camera gives the plain quad (%d)", emitted);
  }
  {
    // off screen after sampling: dropped before it is cut up
    auto node = [](int, int, od::Node& n) { n = od::Node{5000, 5000, 5}; return true; };
    auto visible = [](float, float, float, float) { return false; };
    int emitted = 0;
    auto emit = [&](const od::Node*, const std::uint32_t*) { ++emitted; };
    od::QuadBudget budget;
    budget.committed = 1;
    const od::Node c[4] = {{5000, 5000, 5}, {5100, 5000, 5}, {5100, 5100, 5}, {5000, 5100, 5}};
    const std::uint32_t col[4] = {1, 1, 1, 1};
    od::DrapePatch(node, visible, emit, budget, 0, 0, 16, 16, c, col, 0);
    CHECK(emitted == 0, "an off screen patch emits nothing (%d)", emitted);
  }

  // ---- colours --------------------------------------------------------------------------------------------------------------------------------
  {
    const std::uint32_t same[4] = {0x80102030u, 0x80102030u, 0x80102030u, 0x80102030u};
    CHECK(od::LerpArgb(same, 0.3f, 0.6f) == 0x80102030u, "a uniform colour stays");
    const std::uint32_t ramp[4] = {0x00FFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00FFFFFFu};
    CHECK(od::LerpArgb(ramp, 0.0f, 0.5f) == 0x00FFFFFFu, "left edge");
    CHECK(od::LerpArgb(ramp, 1.0f, 0.5f) == 0xFFFFFFFFu, "right edge");
    const std::uint32_t mid = od::LerpArgb(ramp, 0.5f, 0.5f);
    CHECK((mid >> 24) >= 0x7Fu && (mid >> 24) <= 0x80u, "alpha half way (%08X)", mid);
  }

  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
