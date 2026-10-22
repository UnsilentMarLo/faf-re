// Stand-alone test of the slope view's ratings and cache (EdOverlaySlope.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdOverlaySlopeTest.cpp -o EdOverlaySlopeTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdOverlaySlopeTest.cpp
#include <chrono>
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
    Field(const int width, const int depth, unsigned seed) : w(width), d(depth), v(static_cast<std::size_t>(width) * depth)
    {
      for (auto& x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<int>((seed >> 8) % 3000u) + 2000;
        if ((seed >> 4) % 11u == 0u) x += 9000;   // some cliffs
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
    for (int kind = 0; kind < 2; ++kind) {
      for (int n : {1, 2, 3, 5, 8, 16}) {
        for (const int origin : {0, 64, 192}) {
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
    CHECK(s.NeedsRating(1, 1), "unrated tile needs rating");
    CHECK(s.Block(0, 70, 70) == 0, "unrated tile reads 0");
    s.Rate(1, 1, h, 1);
    CHECK(!s.NeedsRating(1, 1), "rated tile is up to date");
    CHECK(s.Built() == 1, "one tile built");
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
