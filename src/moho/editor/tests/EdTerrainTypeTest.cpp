// Stand-alone test of the terrain-type grid math (EdTerrainTypeMath.h; no engine needed).
//   g++ -std=c++20 -O2 -ffp-contract=off -I src/sdk src/sdk/moho/editor/tests/EdTerrainTypeTest.cpp -o EdTerrainTypeTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdTerrainTypeTest.cpp
//
// References: terraintype.lua Dab (a disc of the cells whose centre is within the radius) and NodeCodes (the cell under a node), transliterated with every Lua operation rounded to a float.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "moho/editor/EdTerrainTypeMath.h"

namespace tt = moho::editor::ttype;

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

static double F(const double v) { return static_cast<double>(static_cast<float>(v)); }

static std::uint32_t gSeed = 777u;
static std::uint32_t Rand()
{
  gSeed = gSeed * 1664525u + 1013904223u;
  return gSeed >> 8;
}

// the Lua Dab: x0 = max(0, floor(cx - r)) ... ; dx = (x + 0.5) - cx; if dx * dx + dz * dz <= r * r
static int LuaDisc(std::vector<std::uint8_t>& grid, const int w, const int h, double cx, double cz, double r, const int code, int bbox[4])
{
  cx = F(cx); cz = F(cz); r = F(r);
  const int x0 = std::max(0, static_cast<int>(std::floor(F(cx - r))));
  const int z0 = std::max(0, static_cast<int>(std::floor(F(cz - r))));
  const int x1 = std::min(w - 1, static_cast<int>(std::ceil(F(cx + r))));
  const int z1 = std::min(h - 1, static_cast<int>(std::ceil(F(cz + r))));
  int changed = 0;
  bbox[0] = 1 << 30; bbox[1] = 1 << 30; bbox[2] = -1; bbox[3] = -1;
  if (x1 < x0 || z1 < z0) return 0;
  for (int z = z0; z <= z1; ++z) {
    for (int x = x0; x <= x1; ++x) {
      const double dx = F(F(x + 0.5) - cx), dz = F(F(z + 0.5) - cz);
      if (F(F(dx * dx) + F(dz * dz)) <= F(r * r)) {
        std::uint8_t& v = grid[static_cast<std::size_t>(z) * w + x];
        if (v != code) {
          v = static_cast<std::uint8_t>(code);
          ++changed;
          bbox[0] = std::min(bbox[0], x); bbox[1] = std::min(bbox[1], z); bbox[2] = std::max(bbox[2], x); bbox[3] = std::max(bbox[3], z);
        }
      }
    }
  }
  return changed;
}

static void TestDisc()
{
  const int w = 97, h = 61;
  for (int it = 0; it < 3000; ++it) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * h);
    for (auto& v : a) v = static_cast<std::uint8_t>(Rand() % 4 == 0 ? 7 : 1);
    std::vector<std::uint8_t> b = a;
    const double cx = (static_cast<int>(Rand() % 1400) - 200) / 10.0, cz = (static_cast<int>(Rand() % 1000) - 200) / 10.0, r = (Rand() % 400) / 10.0;
    const int code = 1 + static_cast<int>(Rand() % 250);
    int bb[4];
    const int refChanged = LuaDisc(b, w, h, cx, cz, r, code, bb);
    const tt::DiscResult res = tt::PaintDisc(a.data(), w, h, static_cast<float>(F(cx)), static_cast<float>(F(cz)), static_cast<float>(F(r)), static_cast<std::uint8_t>(code));
    CHECK(a == b, "disc (%g,%g) r %g code %d: the grids differ", cx, cz, r, code);
    CHECK(res.changed == refChanged, "changed %d vs %d", res.changed, refChanged);
    if (refChanged > 0) {
      CHECK(res.x0 == bb[0] && res.z0 == bb[1] && res.x1 == bb[2] && res.z1 == bb[3], "bbox %d,%d..%d,%d vs %d,%d..%d,%d", res.x0, res.z0, res.x1, res.z1, bb[0], bb[1], bb[2], bb[3]);
    }
  }
  // a disc on the code it already holds changes nothing; off the map nothing; radius 0 nothing
  std::vector<std::uint8_t> g(static_cast<std::size_t>(w) * h, 5);
  CHECK(tt::PaintDisc(g.data(), w, h, 20, 20, 6, 5).changed == 0, "same code: no change");
  CHECK(tt::PaintDisc(g.data(), w, h, -50, -50, 6, 9).changed == 0, "off the map");
  CHECK(tt::PaintDisc(g.data(), w, h, 20, 20, 0, 9).changed == 0, "radius 0");
  const tt::DiscResult d = tt::PaintDisc(g.data(), w, h, 20, 20, 3, 9);
  CHECK(d.changed > 20 && d.changed < 40, "a disc of radius 3 has about 28 cells (%d)", d.changed);
  CHECK(d.x0 >= 17 && d.x1 <= 22 && d.z0 >= 17 && d.z1 <= 22, "bbox of the radius 3 disc");
  // a second dab on the same disc changes nothing
  CHECK(tt::PaintDisc(g.data(), w, h, 20, 20, 3, 9).changed == 0, "painting twice changes nothing the second time");
  // clamped at the map edge
  const tt::DiscResult e = tt::PaintDisc(g.data(), w, h, 0, 0, 4, 11);
  CHECK(e.changed > 0 && e.x0 == 0 && e.z0 == 0, "a disc at the corner");
}

static void TestNodes()
{
  const int w = 128, h = 128;
  std::vector<std::uint8_t> g(static_cast<std::size_t>(w) * h);
  for (int z = 0; z < h; ++z) for (int x = 0; x < w; ++x) g[static_cast<std::size_t>(z) * w + x] = static_cast<std::uint8_t>((x * 3 + z * 7) % 251);
  const float sizes[] = {1.0f, 2.0f, 0.5f, 1.5f, 4.0f};
  for (const float cs : sizes) {
    for (int it = 0; it < 200; ++it) {
      const int x0 = static_cast<int>(Rand() % 400) - 40, z0 = static_cast<int>(Rand() % 400) - 40;
      const int nw = 2 + static_cast<int>(Rand() % 60), nh = 2 + static_cast<int>(Rand() % 60);
      const int step = 1 << (Rand() % 4);
      std::vector<std::uint8_t> out(static_cast<std::size_t>(nw) * nh);
      tt::SampleNodes(g.data(), w, h, x0, z0, nw, nh, step, cs, cs, out.data());
      int bad = 0;
      for (int j = 0; j < nh; ++j) {
        // terraintype.lua NodeCodes: cz = max(0, min(gh - 1, floor((z0 + j * step) / csz)))
        const int cz = std::max(0, std::min(h - 1, static_cast<int>(std::floor(F(F(z0 + j * step) / F(cs))))));
        for (int i = 0; i < nw; ++i) {
          const int cx = std::max(0, std::min(w - 1, static_cast<int>(std::floor(F(F(x0 + i * step) / F(cs))))));
          if (out[static_cast<std::size_t>(j) * nw + i] != g[static_cast<std::size_t>(cz) * w + cx]) ++bad;
        }
      }
      CHECK(bad == 0, "nodes cs %g window %d,%d %dx%d step %d: %d wrong", static_cast<double>(cs), x0, z0, nw, nh, step, bad);
    }
  }
}

static void Bench()
{
  const int w = 2048, h = 2048;
  std::vector<std::uint8_t> g(static_cast<std::size_t>(w) * h, 1);
  const auto t0 = std::chrono::steady_clock::now();
  long changed = 0;
  for (int i = 0; i < 1000; ++i) changed += tt::PaintDisc(g.data(), w, h, 100.0f + i, 500.0f + i * 0.5f, 40.0f, static_cast<std::uint8_t>(2 + i % 3)).changed;
  const auto t1 = std::chrono::steady_clock::now();
  std::vector<std::uint8_t> out(160 * 160);
  for (int i = 0; i < 100; ++i) tt::SampleNodes(g.data(), w, h, 0, 0, 160, 160, 8, 1.0f, 1.0f, out.data());
  const auto t2 = std::chrono::steady_clock::now();
  std::printf("bench: disc r=40 %.4f ms, 160x160 node sample %.4f ms (%ld cells changed)\n", std::chrono::duration<double, std::milli>(t1 - t0).count() / 1000,
              std::chrono::duration<double, std::milli>(t2 - t1).count() / 100, changed);
}

int main(int argc, char** /*argv*/)
{
  TestDisc();
  TestNodes();
  if (argc > 1) Bench();
  std::printf("%s: %d checks, %d failures\n", gFailures == 0 ? "PASS" : "FAIL", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
