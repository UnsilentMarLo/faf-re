// Stand-alone test of the dirty rectangle of a height edit (EdDirtyRect.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdDirtyRectTest.cpp -o EdDirtyRectTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdDirtyRectTest.cpp
//
// Besides the pure function it carries MODELS of what the engine does with the rectangle (the loops of CWldTerrainRes::UpdateNormalMap and CHeightField::UpdateErrorKernel,
// copied in structure from src/sdk/moho/sim/CWldMap.cpp / STIMap.cpp) and proves, on random edits, that a TIGHT rectangle leaves stale texels / error cells on its border
// and that PadDirtyRect(rect, kDerivedDataPad, ...) leaves none.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "moho/editor/EdDirtyRect.h"

using moho::editor::DirtyRect;
using moho::editor::kDerivedDataPad;
using moho::editor::PadDirtyRect;

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

static bool Same(const DirtyRect& a, const DirtyRect& b)
{
  return (a.Empty() && b.Empty()) || (a.x0 == b.x0 && a.z0 == b.z0 && a.x1 == b.x1 && a.z1 == b.z1);
}

// ---- the pure function ------------------------------------------------------------------------------------------------------------------------

static void TestPad()
{
  // plain growth
  CHECK(Same(PadDirtyRect({10, 20, 12, 24}, 2, 100, 100, 1), {8, 18, 14, 26}), "plain growth");
  // clamped to the field on every side
  CHECK(Same(PadDirtyRect({0, 0, 3, 3}, 2, 100, 100, 1), {0, 0, 5, 5}), "clamped low");
  CHECK(Same(PadDirtyRect({97, 98, 99, 99}, 2, 100, 100, 1), {95, 96, 99, 99}), "clamped high");
  CHECK(Same(PadDirtyRect({0, 0, 99, 99}, 2, 100, 100, 1), {0, 0, 99, 99}), "whole field stays");
  // a rectangle already past the field is clamped, not wrapped (the field is 100 wide)
  CHECK(Same(PadDirtyRect({-5, -5, 3, 3}, 1, 100, 100, 1), {0, 0, 4, 4}), "input below zero");
  // one sample
  CHECK(Same(PadDirtyRect({50, 50, 50, 50}, 1, 100, 100, 1), {49, 49, 51, 51}), "single sample");
  CHECK(Same(PadDirtyRect({0, 0, 0, 0}, 0, 1, 1, 1), {0, 0, 0, 0}), "1 x 1 field, no pad");
  // pad 0 is the identity (inside the field)
  CHECK(Same(PadDirtyRect({4, 5, 6, 7}, 0, 100, 100, 1), {4, 5, 6, 7}), "pad 0");
  // empty / invalid inputs give an empty result
  CHECK(PadDirtyRect({5, 5, 4, 9}, 2, 100, 100, 1).Empty(), "empty input");
  CHECK(PadDirtyRect({1, 1, 2, 2}, -1, 100, 100, 1).Empty(), "negative pad");
  CHECK(PadDirtyRect({1, 1, 2, 2}, 2, 0, 100, 1).Empty(), "no field");
  CHECK(PadDirtyRect({200, 200, 210, 210}, 2, 100, 100, 1).Empty(), "entirely outside the field");
  // tile rounding: out to the tile grid (x0 down, x1 up to the last sample of its tile), then clamped to the field
  CHECK(Same(PadDirtyRect({10, 10, 12, 12}, 1, 100, 100, 16), {0, 0, 15, 15}), "tile 16 inside one tile");
  CHECK(Same(PadDirtyRect({15, 15, 16, 16}, 1, 100, 100, 16), {0, 0, 31, 31}), "tile 16 across a border");
  CHECK(Same(PadDirtyRect({40, 40, 50, 50}, 2, 100, 100, 16), {32, 32, 63, 63}), "tile 16 two tiles");
  CHECK(Same(PadDirtyRect({90, 90, 99, 99}, 2, 100, 100, 16), {80, 80, 99, 99}), "tile 16 last partial tile is clamped to the field");
  CHECK(Same(PadDirtyRect({10, 10, 12, 12}, 1, 100, 100, 0), {9, 9, 13, 13}), "tile 0 = no rounding");
  // a randomized property: the result contains the input grown by pad (inside the field), stays in the field, and tile rounding only grows it
  std::mt19937 rng(7);
  for (int n = 0; n < 5000; ++n) {
    const int w = 5 + static_cast<int>(rng() % 200), h = 5 + static_cast<int>(rng() % 200);
    const int x0 = static_cast<int>(rng() % w), z0 = static_cast<int>(rng() % h);
    const int x1 = std::min(w - 1, x0 + static_cast<int>(rng() % 40)), z1 = std::min(h - 1, z0 + static_cast<int>(rng() % 40));
    const int pad = static_cast<int>(rng() % 4);
    const int tile = static_cast<int>(rng() % 3) == 0 ? 1 << (1 + static_cast<int>(rng() % 5)) : 1;
    const DirtyRect in{x0, z0, x1, z1};
    const DirtyRect out = PadDirtyRect(in, pad, w, h, tile);
    const DirtyRect base = PadDirtyRect(in, pad, w, h, 1);
    CHECK(!out.Empty() && out.x0 >= 0 && out.z0 >= 0 && out.x1 <= w - 1 && out.z1 <= h - 1, "in the field");
    CHECK(out.x0 <= base.x0 && out.z0 <= base.z0 && out.x1 >= base.x1 && out.z1 >= base.z1, "tile rounding only grows");
    CHECK(base.x0 == std::max(0, x0 - pad) && base.x1 == std::min(w - 1, x1 + pad) && base.z0 == std::max(0, z0 - pad) && base.z1 == std::min(h - 1, z1 + pad), "exact growth");
    if (tile > 1) {
      CHECK(out.x0 % tile == 0 && out.z0 % tile == 0 && (((out.x1 + 1) % tile) == 0 || out.x1 == w - 1) && (((out.z1 + 1) % tile) == 0 || out.z1 == h - 1), "on the tile grid");
    }
  }
}

// ---- a model of the normal map update ---------------------------------------------------------------------------------------------------------

struct Field
{
  int w = 0, h = 0;
  std::vector<std::uint16_t> d;
  [[nodiscard]] int Cl(const int v, const int hi) const { return std::max(0, std::min(hi, v)); }
  // CHeightField::GetElevation: bilinear
  [[nodiscard]] float Elev(const float x, const float z) const
  {
    const int fx = static_cast<int>(std::floor(x)), fz = static_cast<int>(std::floor(z));
    const float ax = x - static_cast<float>(fx), az = z - static_cast<float>(fz);
    const int x0 = Cl(fx, w - 1), x1 = Cl(fx + 1, w - 1), z0 = Cl(fz, h - 1), z1 = Cl(fz + 1, h - 1);
    const float h00 = d[x0 + z0 * w] / 128.0f, h01 = d[x0 + z1 * w] / 128.0f, h10 = d[x1 + z0 * w] / 128.0f, h11 = d[x1 + z1 * w] / 128.0f;
    const float r0 = h00 + (h01 - h00) * az, r1 = h10 + (h11 - h10) * az;
    return r0 + (r1 - r0) * ax;
  }
  // CHeightField::GetNormal, the two lanes the normal map keeps
  [[nodiscard]] std::pair<float, float> Normal(const float x, const float z) const
  {
    const float top = Elev(x, z - 0.5f), left = Elev(x - 0.5f, z), right = Elev(x + 0.5f, z), bottom = Elev(x, z + 0.5f);
    const float nx = left - right, nz = top - bottom;
    const float l = std::sqrt(nx * nx + 1.0f + nz * nz);
    return {nx / l, nz / l};
  }
};

using Texels = std::vector<std::pair<float, float>>;   // (w - 1) x (h - 1)

static Texels FullNormals(const Field& f)
{
  Texels t(static_cast<std::size_t>(f.w - 1) * static_cast<std::size_t>(f.h - 1));
  for (int j = 0; j < f.h - 1; ++j)
    for (int i = 0; i < f.w - 1; ++i) t[static_cast<std::size_t>(j) * static_cast<std::size_t>(f.w - 1) + static_cast<std::size_t>(i)] = f.Normal(i + 0.5f, j + 0.5f);
  return t;
}

// the loops of CWldTerrainRes::UpdateNormalMap(rect): tiles of tileW x tileH texels, the rectangle's end is EXCLUSIVE, the lock is rounded out to 4 x 4 blocks of the tile
static void UpdateNormalMapModel(const Field& f, Texels& t, const int tileW, const int tileH, const DirtyRect rect)
{
  const auto down4 = [](const int v) { return v & ~3; };
  const auto up4 = [](const int v) { return (v + 3) & ~3; };
  for (int tz = 0; tz < f.h - 1; tz += tileH) {
    for (int tx = 0; tx < f.w - 1; tx += tileW) {
      const int cx0 = std::max(tx, rect.x0), cx1 = std::min(tx + tileW, rect.x1);
      const int cz0 = std::max(tz, rect.z0), cz1 = std::min(tz + tileH, rect.z1);
      if (cx0 >= cx1 || cz0 >= cz1) continue;
      const int lx0 = down4(cx0 - tx), lz0 = down4(cz0 - tz), lx1 = up4(cx1 - tx), lz1 = up4(cz1 - tz);
      for (int z = tz + lz0; z < tz + lz1; ++z)
        for (int x = tx + lx0; x < tx + lx1; ++x)
          if (x < f.w - 1 && z < f.h - 1) t[static_cast<std::size_t>(z) * static_cast<std::size_t>(f.w - 1) + static_cast<std::size_t>(x)] = f.Normal(x + 0.5f, z + 0.5f);
    }
  }
}

static int StaleTexels(const Texels& have, const Texels& want)
{
  int n = 0;
  for (std::size_t i = 0; i < have.size(); ++i)
    if (std::fabs(have[i].first - want[i].first) > 1e-6f || std::fabs(have[i].second - want[i].second) > 1e-6f) ++n;
  return n;
}

static void TestNormalMap()
{
  std::mt19937 rng(11);
  int tightStale = 0, tightRuns = 0, paddedStale = 0, paddedOneStale = 0;
  for (int run = 0; run < 400; ++run) {
    Field f;
    f.w = 40 + static_cast<int>(rng() % 30);
    f.h = 40 + static_cast<int>(rng() % 30);
    f.d.resize(static_cast<std::size_t>(f.w) * static_cast<std::size_t>(f.h));
    for (auto& v : f.d) v = static_cast<std::uint16_t>(2000 + rng() % 400);
    // several tile layouts: one tile for the whole map (the engine's choice up to 2048 samples) and smaller ones
    const int tileW = run % 3 == 0 ? f.w - 1 : 16, tileH = run % 3 == 0 ? f.h - 1 : 16;
    Texels shown = FullNormals(f);
    Texels shownTight = shown, shownOne = shown;
    // an edit: a random rectangle of changed samples, touching the map edge sometimes
    const int x0 = static_cast<int>(rng() % f.w), z0 = static_cast<int>(rng() % f.h);
    const DirtyRect changed{x0, z0, std::min(f.w - 1, x0 + 1 + static_cast<int>(rng() % 12)), std::min(f.h - 1, z0 + 1 + static_cast<int>(rng() % 12))};
    for (int z = changed.z0; z <= changed.z1; ++z)
      for (int x = changed.x0; x <= changed.x1; ++x) f.d[static_cast<std::size_t>(z) * static_cast<std::size_t>(f.w) + static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(2000 + rng() % 400);
    const Texels want = FullNormals(f);
    // (a) the tight rectangle, as handed to the engine
    UpdateNormalMapModel(f, shownTight, tileW, tileH, changed);
    // (b) the old sink: one sample more on every side
    UpdateNormalMapModel(f, shownOne, tileW, tileH, PadDirtyRect(changed, 1, f.w, f.h, 1));
    // (c) the new sink
    UpdateNormalMapModel(f, shown, tileW, tileH, PadDirtyRect(changed, kDerivedDataPad, f.w, f.h, 1));
    ++tightRuns;
    if (StaleTexels(shownTight, want) > 0) ++tightStale;
    paddedOneStale += StaleTexels(shownOne, want);
    const int stale = StaleTexels(shown, want);
    paddedStale += stale;
    CHECK(stale == 0, "run %d: %d stale texels after a padded update of [%d,%d]-[%d,%d] (tile %d)", run, stale, changed.x0, changed.z0, changed.x1, changed.z1, tileW);
  }
  // the model is only evidence if the tight rectangle really fails
  CHECK(tightStale > tightRuns / 2, "the tight rectangle must leave stale texels in most runs (%d of %d)", tightStale, tightRuns);
  // the old sink (+1 and the exclusive end) is enough for the normal map: it is the error grids that needed the pad
  CHECK(paddedOneStale == 0, "pad 1 already covers every texel (%d stale)", paddedOneStale);
  CHECK(paddedStale == 0, "pad %d leaves no stale texel", kDerivedDataPad);
  std::printf("normal map: tight rectangle stale in %d of %d runs, pad 1 stale texels %d, pad %d stale texels %d\n", tightStale, tightRuns, paddedOneStale, kDerivedDataPad, paddedStale);
}

// ---- a model of the error grid update ---------------------------------------------------------------------------------------------------------

// UpdateErrorKernel visits, per tier t = 1 .. tiers, the cells [minX >> 1 ...) where minX / maxX are shifted once per tier: minX >>= 1, maxX = (maxX + 1) >> 1 (exclusive end)
static std::set<std::pair<int, std::pair<int, int>>> VisitedCells(int minX, int minZ, int maxX, int maxZ, const int tiers)
{
  std::set<std::pair<int, std::pair<int, int>>> out;
  for (int tier = 1; tier <= tiers; ++tier) {
    minX >>= 1;
    maxX = (maxX + 1) >> 1;
    minZ >>= 1;
    maxZ = (maxZ + 1) >> 1;
    for (int z = minZ; z < maxZ; ++z)
      for (int x = minX; x < maxX; ++x) out.insert({tier, {x, z}});
  }
  return out;
}

// every cell of every tier whose INCLUSIVE sample span [g << t, (g + 1) << t] contains the sample (x, z)
static std::set<std::pair<int, std::pair<int, int>>> CellsContaining(const int x, const int z, const int tiers, const int w, const int h)
{
  std::set<std::pair<int, std::pair<int, int>>> out;
  for (int tier = 1; tier <= tiers; ++tier) {
    const int size = 1 << tier;
    const int cellsX = (w - 1 + size - 1) / size, cellsZ = (h - 1 + size - 1) / size;     // the tier's grid covers the field's cells
    for (int gz = 0; gz < cellsZ; ++gz)
      for (int gx = 0; gx < cellsX; ++gx)
        if (x >= (gx << tier) && x <= ((gx + 1) << tier) && z >= (gz << tier) && z <= ((gz + 1) << tier)) out.insert({tier, {gx, gz}});
  }
  return out;
}

static void TestErrorGrid()
{
  std::mt19937 rng(23);
  int tightMisses = 0, runs = 0, paddedMisses = 0, oneMisses = 0;
  for (int run = 0; run < 3000; ++run) {
    const int w = 33 + static_cast<int>(rng() % 200), h = 33 + static_cast<int>(rng() % 200);
    const int tiers = 6;
    const int x0 = static_cast<int>(rng() % w), z0 = static_cast<int>(rng() % h);
    const DirtyRect changed{x0, z0, std::min(w - 1, x0 + static_cast<int>(rng() % 20)), std::min(h - 1, z0 + static_cast<int>(rng() % 20))};
    // all the cells that contain a changed sample
    std::set<std::pair<int, std::pair<int, int>>> needed;
    for (int z = changed.z0; z <= changed.z1; ++z)
      for (int x = changed.x0; x <= changed.x1; ++x) {
        const auto c = CellsContaining(x, z, tiers, w, h);
        needed.insert(c.begin(), c.end());
      }
    const auto missing = [&](const DirtyRect& r) {
      // CHeightField::UpdateError clamps the end to width - 1 / height - 1 first
      const auto seen = VisitedCells(std::max(0, r.x0), std::max(0, r.z0), std::min(w - 1, r.x1), std::min(h - 1, r.z1), tiers);
      int n = 0;
      for (const auto& c : needed)
        if (seen.find(c) == seen.end()) ++n;
      return n;
    };
    ++runs;
    if (missing(changed) > 0) ++tightMisses;
    oneMisses += missing(PadDirtyRect(changed, 1, w, h, 1));
    const int m = missing(PadDirtyRect(changed, kDerivedDataPad, w, h, 1));
    paddedMisses += m;
    CHECK(m == 0, "run %d: %d error cells not visited for [%d,%d]-[%d,%d] on %d x %d", run, m, changed.x0, changed.z0, changed.x1, changed.z1, w, h);
  }
  CHECK(tightMisses > runs / 4, "the tight rectangle must miss cells in many runs (%d of %d)", tightMisses, runs);
  CHECK(oneMisses == 0, "pad 1 reaches every cell that holds a changed sample (%d missed)", oneMisses);
  std::printf("error grids: tight rectangle misses cells in %d of %d runs, pad 1 missed %d, pad %d missed %d\n", tightMisses, runs, oneMisses, kDerivedDataPad, paddedMisses);
}

int main()
{
  TestPad();
  TestNormalMap();
  TestErrorGrid();
  std::printf("%s: %d checks, %d failures\n", gFailures == 0 ? "PASS" : "FAIL", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
