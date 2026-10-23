// Stand-alone test of the slope view's ratings and cache (EdOverlaySlope.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdOverlaySlopeTest.cpp -o EdOverlaySlopeTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdOverlaySlopeTest.cpp
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "moho/editor/EdOverlaySlope.h"

namespace os = moho::editor::overlayslope;

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

namespace
{
  struct Field
  {
    int w, d;
    std::vector<int> v;
    Field(const int width, const int depth, unsigned seed, const unsigned range = 3000u, const bool cliffs = true)
        : w(width), d(depth), v(static_cast<std::size_t>(width) * depth)
    {
      for (auto& x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<int>((seed >> 8) % range) + 2000;
        if (cliffs && (seed >> 4) % 11u == 0u) x += 9000;   // some cliffs
      }
    }
    int At(int x, int z) const
    {
      x = std::min(std::max(x, 0), w - 1);
      z = std::min(std::max(z, 0), d - 1);
      return v[static_cast<std::size_t>(z) * w + x];
    }
  };
}

int main()
{
  // ---- sliding extremes against brute force ----
  {
    unsigned seed = 7;
    for (int trial = 0; trial < 200; ++trial) {
      seed = seed * 1664525u + 1013904223u;
      const int count = 1 + static_cast<int>((seed >> 8) % 90u);
      const int w = 1 + static_cast<int>((seed >> 16) % 20u);
      std::vector<int> in(count), out(count, -12345), scratch(count * 2);
      for (auto& x : in) {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<int>((seed >> 9) % 1000u);
      }
      os::SlidingExtreme<true>(in.data(), 1, count, w, out.data(), 1, scratch.data(), count);
      std::vector<int> lo(count, -12345);
      os::SlidingExtreme<false>(in.data(), 1, count, w, lo.data(), 1, scratch.data(), count);
      bool ok = true;
      for (int i = 0; i + w <= count; ++i) {
        int mx = -1, mn = 1 << 30;
        for (int k = i; k < i + w; ++k) {
          mx = std::max(mx, in[k]);
          mn = std::min(mn, in[k]);
        }
        ok = ok && out[i] == mx && lo[i] == mn;
      }
      CHECK(ok, "sliding extreme count %d window %d", count, w);
      // strided input and output, limited output count
      if (count >= w) {
        std::vector<int> strided(count * 3, 0), res(count * 2, -777);
        for (int i = 0; i < count; ++i) strided[i * 3] = in[i];
        const int want = std::max(1, (count - w + 1) / 2);
        os::SlidingExtreme<true>(strided.data(), 3, count, w, res.data(), 2, scratch.data(), want);
        bool ok2 = true;
        for (int i = 0; i < want; ++i) {
          int mx = -1;
          for (int k = i; k < i + w; ++k) mx = std::max(mx, in[k]);
          ok2 = ok2 && res[i * 2] == mx;
        }
        ok2 = ok2 && res[want * 2] == -777;   // nothing written past the requested count
        CHECK(ok2, "strided sliding extreme count %d window %d", count, w);
      }
    }
  }

  // ---- tile ratings against the rule, every window, both kinds ----
  {
    const Field f(300, 220, 1);
    const auto h = [&f](const int x, const int z) { return f.At(x, z); };
    for (int kind = 0; kind < 3; ++kind) {
      for (int n : {1, 2, 3, 5, 8, 12, 15, 16}) {
        for (const int origin : {0, 64, 192, 256}) {
          std::vector<std::uint16_t> out(os::kTile * os::kTile);
          os::RateTile(kind, n, origin, 64, h, out.data());
          int bad = 0;
          for (int j = 0; j < os::kTile; ++j) {
            for (int i = 0; i < os::kTile; ++i) {
              const int want = std::min(65535, os::CellRating(kind, n, origin + i, 64 + j, h));
              if (out[static_cast<std::size_t>(j) * os::kTile + i] != want) ++bad;
            }
          }
          CHECK(bad == 0, "tile kind %d window %d origin %d: %d cells differ from the rule", kind, n, origin, bad);
        }
      }
    }
  }

  // ---- the ring rule (kind 2) against a direct transliteration of the engine's OCCUPY_CheckEdgeFlatness, in float world units as the engine computes them ----
  {
    // STIMap.cpp: SampleClampedElevation (word * 1/128 as a float, indices clamped) and OCCUPY_CheckEdgeFlatness, rect = { x0, x1, z0, z1 } (inclusive sample indices)
    struct EngineField
    {
      const Field& f;
      float Sample(const int x, const int z) const { return static_cast<float>(f.At(x, z)) * (1.0f / 128.0f); }
    };
    const auto engineDeviation = [](const EngineField& field, const int x0, const int x1, const int z0, const int z1) {
      float minHeight = std::numeric_limits<float>::max();
      float maxHeight = -std::numeric_limits<float>::max();
      float lastSample = 0.0f;
      for (int cellX = x0 - 1; cellX <= x1 + 1; ++cellX) {
        const float topSample = field.Sample(cellX, z0 - 1);
        lastSample = field.Sample(cellX, z1 + 1);
        minHeight = std::min({minHeight, topSample, lastSample});
        maxHeight = std::max({maxHeight, topSample, lastSample});
      }
      for (int cellZ = z0; cellZ <= z1; ++cellZ) {
        const float leftSample = field.Sample(x0 - 1, cellZ);
        lastSample = field.Sample(x1 + 1, cellZ);
        minHeight = std::min({minHeight, leftSample, lastSample});
        maxHeight = std::max({maxHeight, leftSample, lastSample});
      }
      const float pivot = std::ceil(lastSample);
      return std::max(maxHeight - pivot, pivot - minHeight);
    };
    unsigned seed = 99;
    int cases = 0, passes = 0, fails = 0;
    for (int variant = 0; variant < 3; ++variant) {
      // rough terrain (cliffs), gentle terrain (a few world units of noise) and nearly flat terrain (about one unit): the verdicts must vary
      const Field f(140, 110, 20u + static_cast<unsigned>(variant), variant == 0 ? 3000u : (variant == 1 ? 700u : 150u), variant == 0);
      const EngineField ef{f};
      const auto h = [&f](const int x, const int z) { return f.At(x, z); };
      for (int n = 1; n <= os::kMaxWindow; ++n) {
        for (int trial = 0; trial < 120; ++trial) {
          seed = seed * 1664525u + 1013904223u;
          const int cx = static_cast<int>((seed >> 8) % 150u) - 5;   // some cells hang over the edge of the field (clamped samples)
          seed = seed * 1664525u + 1013904223u;
          const int cz = static_cast<int>((seed >> 8) % 120u) - 5;
          const float want = engineDeviation(ef, cx, cx + n, cz, cz + n);
          const int got = os::CellRating(os::kKindRing, n, cx, cz, h);
          CHECK(static_cast<float>(got) / 128.0f == want, "ring n %d at (%d, %d): %d words vs the engine's %.7f", n, cx, cz, got, want);
          CHECK(got >= 0, "ring rating is never negative (%d)", got);
          for (const float limit : {1.0f, 1.1f, 1.2f}) {
            const bool engineFits = limit >= want;                                   // blueprint.Physics.MaxGroundVariation >= deviation
            const bool viewFits = !(static_cast<float>(got) / 128.0f > limit);       // what the slope view draws as not blocked
            CHECK(engineFits == viewFits, "ring verdict n %d at (%d, %d) limit %.1f", n, cx, cz, limit);
            ++cases;
            (engineFits ? passes : fails)++;
          }
        }
      }
    }
    CHECK(passes > 200 && fails > 200, "the random terrains give both verdicts (%d fit, %d do not of %d)", passes, fails, cases);

    // hand checks: a flat field at 3.3 units: the pivot is 4, the ring sits 0.7 below it
    {
      Field flat(40, 40, 1, 1u, false);
      for (auto& x : flat.v) x = 422;                // 422 / 128 = 3.296875
      const auto h = [&flat](const int x, const int z) { return flat.At(x, z); };
      CHECK(os::CellRating(os::kKindRing, 4, 10, 10, h) == 512 - 422, "flat ground below an integer height: rating = ceil - h (%d)", os::CellRating(os::kKindRing, 4, 10, 10, h));
      CHECK(os::CellRating(os::kKindBuild, 4, 10, 10, h) == 0, "the area rule calls the same ground flat");
      flat.v.assign(flat.v.size(), 512);             // exactly 4.0: the ceil keeps it
      CHECK(os::CellRating(os::kKindRing, 4, 10, 10, h) == 0, "flat ground at an integer height: 0");
      // only the sample the engine reads last sets the pivot: (x1 + 1, z1) = (cx + n + 1, cz + n)
      const int cx = 10, cz = 12, n = 5;
      flat.v[static_cast<std::size_t>(cz + n) * flat.w + (cx + n + 1)] = 900;    // 7.03125: ceil 8
      const int dev = os::CellRating(os::kKindRing, n, cx, cz, h);
      // ring min 512, max 900, pivot 1024: max(900 - 1024, 1024 - 512) = 512
      CHECK(dev == 512, "pivot from the last sample (x1 + 1, z1): %d", dev);
      // the sample next to it (x1 + 1, z1 + 1) is the end of the bottom row, not the last read: it does not move the pivot
      flat.v[static_cast<std::size_t>(cz + n) * flat.w + (cx + n + 1)] = 512;
      flat.v[static_cast<std::size_t>(cz + n + 1) * flat.w + (cx + n + 1)] = 900;
      CHECK(os::CellRating(os::kKindRing, n, cx, cz, h) == 900 - 512, "the corner sample is in the ring but is not the pivot (%d)", os::CellRating(os::kKindRing, n, cx, cz, h));
      // a sample inside the skirt is not looked at at all
      flat.v[static_cast<std::size_t>(cz + n) * flat.w + (cx + 2)] = 5000;
      flat.v[static_cast<std::size_t>(cz + 2) * flat.w + (cx + 2)] = 5000;
      CHECK(os::CellRating(os::kKindRing, n, cx, cz, h) == 900 - 512, "the skirt's own samples are not part of the ring rule");
      CHECK(os::CellRating(os::kKindBuild, n, cx, cz, h) > 900 - 512, "but the area rule sees them");
    }
    CHECK(os::CeilToUnit(0) == 0 && os::CeilToUnit(1) == 128 && os::CeilToUnit(128) == 128 && os::CeilToUnit(129) == 256 && os::CeilToUnit(65535) == 65536, "ceil to a world unit");
    CHECK(os::KindFromName("path") == os::kKindPath && os::KindFromName("build") == os::kKindBuild && os::KindFromName("ring") == os::kKindRing &&
              os::KindFromName("bogus") == os::kKindPath && os::KindFromName(nullptr) == os::kKindPath,
          "kind names");
  }

  // ---- levels: a block holds the maximum of its cells ----
  {
    std::vector<std::uint16_t> data(os::kTileWords, 0);
    unsigned seed = 3;
    for (int k = 0; k < os::kTile * os::kTile; ++k) {
      seed = seed * 1664525u + 1013904223u;
      data[k] = static_cast<std::uint16_t>((seed >> 12) % 1000u);
    }
    os::BuildLevels(data.data());
    bool ok = true;
    for (int l = 1; l < os::kLevels; ++l) {
      const int side = os::kTile >> l;
      for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
          int mx = 0;
          for (int dz = 0; dz < (1 << l); ++dz)
            for (int dx = 0; dx < (1 << l); ++dx) mx = std::max<int>(mx, data[static_cast<std::size_t>((j << l) + dz) * os::kTile + (i << l) + dx]);
          ok = ok && data[os::LevelOffset(l) + static_cast<std::size_t>(j) * side + i] == mx;
        }
      }
    }
    CHECK(ok, "levels are maxima of the cells below");
    CHECK(os::kTileWords == 64u * 64u + 32u * 32u + 16u * 16u + 8u * 8u + 4u * 4u + 2u * 2u + 1u, "tile size %zu", os::kTileWords);
  }

  // ---- store: lazy rating, blocks, invalidation, key change ----
  {
    Field f(200, 150, 5);
    const auto h = [&f](const int x, const int z) { return f.At(x, z); };
    os::Store s;
    s.Reset(f.w, f.d);
    CHECK(s.TilesX() == 4 && s.TilesZ() == 3, "tiles %d x %d", s.TilesX(), s.TilesZ());
    CHECK(s.SetKey(0, 2), "first key");
    CHECK(!s.SetKey(0, 2), "same key keeps the tiles");
    const unsigned versionBefore = s.Version();
    CHECK(s.NeedsRating(1, 1), "unrated tile needs rating");
    CHECK(s.Block(0, 70, 70) == 0, "unrated tile reads 0");
    s.Rate(1, 1, h, 1);
    CHECK(!s.NeedsRating(1, 1), "rated tile is up to date");
    CHECK(s.Built() == 1, "one tile built");
    CHECK(s.Version() != versionBefore, "rating a tile moves the version (drawings built from the ratings are stale)");
    CHECK(s.Block(0, 70, 70) == std::min(65535, os::CellRating(0, 2, 70, 70, h)), "block level 0 equals the rule");
    int mx = 0;
    for (int z = 64; z < 64 + 8; ++z)
      for (int x = 64; x < 64 + 8; ++x) mx = std::max(mx, os::CellRating(0, 2, x, z, h));
    CHECK(s.Block(3, 8, 8) == mx, "block level 3 is the maximum of its 8 x 8 cells (%d vs %d)", s.Block(3, 8, 8), mx);

    // a height edit far from the tile leaves it; an edit near its left / top edge (cells before it look at the edited sample) dirties it
    s.Invalidate(150, 140, 160, 149);
    CHECK(!s.NeedsRating(1, 1), "edit elsewhere keeps the tile");
    s.Invalidate(60, 100, 63, 100);   // sample x = 63 is looked at by cells 47 .. 63 of tile 0, not tile 1 ... but 64 - 17 = 47 reaches back
    CHECK(!s.NeedsRating(1, 1), "edit left of the margin keeps the tile");
    s.Invalidate(80, 80, 81, 81);
    CHECK(s.NeedsRating(1, 1), "edit inside dirties it");
    const int before = s.Block(0, 70, 70);
    f.v[static_cast<std::size_t>(70) * f.w + 71] += 5000;
    s.Rate(1, 1, h, 2);
    CHECK(s.Block(0, 70, 70) != before || before == std::min(65535, os::CellRating(0, 2, 70, 70, h)), "re-rated after the edit");
    CHECK(s.Block(0, 70, 70) == std::min(65535, os::CellRating(0, 2, 70, 70, h)), "re-rated tile equals the rule");

    // an edit just right of the tile's start: the cells before it (in the previous tile) must be re-rated: sample 64 + 3 changes cells 64 + 3 - n ... in tile 1,
    // sample 64 + 0 changes cells 64 - n .. 64: tile 0 too
    s.Rate(0, 1, h, 3);
    s.Invalidate(64, 70, 64, 70);
    CHECK(s.NeedsRating(0, 1), "an edit on the first column dirties the tile before it");

    CHECK(s.SetKey(1, 2), "another kind drops the ratings");
    CHECK(s.NeedsRating(1, 1) && s.Built() == 0, "dropped");
    s.Reset(100, 100);
    CHECK(s.TilesX() == 2 && s.Built() == 0, "reset");
  }

  // ---- store with the ring rule: the cell after a changed sample is stale too (it reads from one sample before its own) ----
  {
    Field f(200, 150, 6, 800u, false);
    const auto h = [&f](const int x, const int z) { return f.At(x, z); };
    os::Store s;
    s.Reset(f.w, f.d);
    s.SetKey(os::kKindRing, 3);
    s.Rate(1, 1, h, 1);
    s.Rate(0, 0, h, 1);
    CHECK(s.Block(0, 70, 70) == os::CellRating(os::kKindRing, 3, 70, 70, h), "ring tile cell equals the rule");
    s.Invalidate(60, 100, 63, 100);   // sample 63 is read by cell 64 (its ring starts at 63): the tile 1 is stale
    CHECK(s.NeedsRating(1, 1), "a ring cell reads one sample before it: an edit of sample 63 dirties tile 1");
    s.Rate(1, 1, h, 2);
    s.Invalidate(100, 60, 100, 62);   // row 62 is read by cells of row 63 at the earliest (z0 - 1 = 62 for cz = 63): tile row 0, not 1
    CHECK(!s.NeedsRating(1, 1), "an edit two rows above the tile keeps it");
    s.Invalidate(100, 63, 100, 63);
    CHECK(s.NeedsRating(1, 1), "row 63 is looked at by cells of tile row 1 (their ring starts one row above)");
  }

  // ---- trim ----
  {
    Field f(130, 130, 9);
    const auto h = [&f](const int x, const int z) { return f.At(x, z); };
    os::Store s;
    s.Reset(f.w, f.d);
    s.SetKey(1, 1);
    s.Rate(0, 0, h, 1);
    s.Rate(1, 1, h, 100);
    s.Trim(120, 50);
    CHECK(s.NeedsRating(0, 0) && !s.NeedsRating(1, 1), "trim frees old tiles only");
  }

  // ---- levels and colour steps ----
  {
    CHECK(os::LevelForView(100, 160) == 0, "small view full resolution");
    CHECK(os::LevelForView(4096, 160) == 5, "an 80 km view uses blocks of 32 (%d)", os::LevelForView(4096, 160));
    CHECK((4096 + 31) / 32 <= 160, "and fits");
    CHECK(os::LevelForView(320, 160) == 1, "320 cells -> blocks of 2");
    // hysteresis: 160 blocks per side; level 0 holds up to 200 cells, level 1 from 128 cells (0.8 x 160) down
    CHECK(os::LevelWithHysteresis(100, 160, -1) == 0 && os::LevelWithHysteresis(4096, 160, -1) == 5, "no previous level: the plain choice");
    CHECK(os::LevelWithHysteresis(170, 160, 0) == 0, "170 cells at level 0 (160 is the plain limit): kept up to 1.25 x");
    CHECK(os::LevelWithHysteresis(201, 160, 0) == 1, "201 cells: coarser");
    CHECK(os::LevelWithHysteresis(300, 160, 1) == 1, "at level 1 up to 400 cells");
    CHECK(os::LevelWithHysteresis(180, 160, 1) == 1, "180 cells at level 1: the plain choice is 1 as well");
    CHECK(os::LevelWithHysteresis(150, 160, 1) == 1, "150 cells at level 1 would fit at level 0 but not within 0.8 x: stays");
    CHECK(os::LevelWithHysteresis(128, 160, 1) == 0, "128 cells: finer");
    {
      // zooming in and out across the threshold flips at most once each way
      int level = -1, flips = 0, last = -1;
      for (int cells = 100; cells <= 400; cells += 3) {
        level = os::LevelWithHysteresis(cells, 160, level);
        if (last >= 0 && level != last) ++flips;
        last = level;
      }
      for (int cells = 400; cells >= 100; cells -= 3) {
        level = os::LevelWithHysteresis(cells, 160, level);
        if (level != last) ++flips;
        last = level;
      }
      CHECK(flips == 2, "a zoom from 100 to 400 cells and back flips the level once each way (0>1, 1>0; the plain choice flips at 160 and 320: 4 times): %d", flips);
      // jitter around the plain threshold (160) never flips
      level = os::LevelForView(160, 160);
      int jitterFlips = 0;
      for (int k = 0; k < 200; ++k) {
        const int cells = 160 + ((k * 7) % 21) - 10;
        const int now = os::LevelWithHysteresis(cells, 160, level);
        if (now != level) ++jitterFlips;
        level = now;
      }
      CHECK(jitterFlips == 0, "a view that jitters by +-10 cells around the threshold never flips (%d)", jitterFlips);
    }
    CHECK(os::ColorStep(0.0f, 0.75f) == 0 && os::ColorStep(0.02f, 0.75f) == 0, "flat is not drawn");
    CHECK(os::ColorStep(0.75f, 0.75f) == os::kSteps, "at the limit is the last step");
    CHECK(os::ColorStep(0.76f, 0.75f) == os::kSteps + 1, "beyond the limit");
    CHECK(os::ColorStep(0.375f, 0.75f) == os::kSteps / 2, "half way");
  }

  // ---- speed of a full 4096 x 4096 map (information, not a pass / fail) ----
  {
    const int W = 4096;
    std::vector<std::uint16_t> map(static_cast<std::size_t>(W) * W);
    unsigned seed = 11;
    for (auto& x : map) {
      seed = seed * 1664525u + 1013904223u;
      x = static_cast<std::uint16_t>(3000 + ((seed >> 14) % 400u));
    }
    const auto h = [&map, W](const int x, const int z) {
      return static_cast<int>(map[static_cast<std::size_t>(std::min(std::max(z, 0), W - 1)) * W + std::min(std::max(x, 0), W - 1)]);
    };
    os::Store s;
    s.Reset(W, W);
    s.SetKey(0, 3);
    const auto t0 = std::chrono::steady_clock::now();
    for (int tz = 0; tz < s.TilesZ(); ++tz)
      for (int tx = 0; tx < s.TilesX(); ++tx) s.Rate(tx, tz, h, 1);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("rating all %zu tiles of a 4096 x 4096 map (window 3): %.0f ms (%.2f ms per tile)\n", s.Built(), ms, ms / static_cast<double>(s.Built()));
  }

  std::printf("EdOverlaySlopeTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
