// EDITOR: the ratings behind the slope view (EdOverlay.cpp, ed_overlay_slope), engine independent so they can be tested on their own
// (moho/editor/tests/EdOverlaySlopeTest.cpp).
//
//   kind 0 "path": the rule a mobile footprint of `n` x `n` cells is checked with (STIMap::OccupancyCapsOfFootprintAt / OCCUPY_MobileCheck): the largest
//          height difference between neighbouring samples inside the window (x in [cx, cx + n], z in [cz, cz + n]), in height words (128 per world unit).
//   kind 1 "build": the rule a structure is checked with (OCCUPY_CheckAreaFlatness): highest minus lowest sample over the same window.
//
// The ratings live in a cache of 64 x 64 cell tiles kept until the heights under a tile change (Invalidate) or the rule changes (SetKey): nothing is
// rated twice, a brush stroke re-rates only the tiles it touched, and a tile is rated in O(cells) with sliding window maxima (not O(cells * window^2)).
// Every tile also keeps its ratings reduced to blocks of 2, 4, ... 64 cells (the steepest cell of a block wins, so a thin cliff never disappears when the
// view is wide), so drawing a view costs what is on screen and not what is in the area.
//
// Heights are given by a callback `int h(int x, int z)` (the height word of a sample; positions outside the field must answer with the edge sample).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

namespace moho::editor::overlayslope
{
  constexpr int kTile = 64;      // cells per tile side
  constexpr int kLevels = 7;     // block sizes 1, 2, 4 ... 64 cells
  constexpr int kMaxWindow = 16;

  /** The rating of one cell computed straight from the rule (slow; the reference for the tests and the readout under the pointer). */
  template <class Sampler>
  [[nodiscard]] inline int CellRating(const int kind, const int n, const int cx, const int cz, Sampler&& h)
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
      return maxDelta;
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
    return hi - lo;
  }

  /**
   * Sliding window extreme in O(count) (van Herk / Gil-Werman): out[i * outStride] = max (or min) of in[(i .. i + w - 1) * inStride] for the first
   * `outCount` (at most count - w + 1) positions i. `scratch` is two arrays of `count` ints.
   */
  template <bool Max>
  inline void SlidingExtreme(const int* const in, const int inStride, const int count, const int w, int* const out, const int outStride, int* const scratch,
                             const int outCount)
  {
    if (w < 1 || count < w) {
      return;
    }
    const int produced = std::min(outCount, count - w + 1);
    const auto pick = [](const int a, const int b) { return Max ? std::max(a, b) : std::min(a, b); };
    if (w == 1) {
      for (int i = 0; i < produced; ++i) {
        out[i * outStride] = in[i * inStride];
      }
      return;
    }
    int* const g = scratch;           // running extreme from the start of the block
    int* const hh = scratch + count;  // running extreme back from the end of the block
    for (int i = 0; i < count; ++i) {
      const int v = in[i * inStride];
      g[i] = (i % w == 0) ? v : pick(g[i - 1], v);
    }
    for (int i = count - 1; i >= 0; --i) {
      const int v = in[i * inStride];
      hh[i] = (i == count - 1 || (i + 1) % w == 0) ? v : pick(hh[i + 1], v);
    }
    for (int i = 0; i < produced; ++i) {
      out[i * outStride] = pick(hh[i], g[i + w - 1]);
    }
  }

  /**
   * The ratings of the kTile x kTile cells whose first cell is (cx0, cz0), written to `out` (row major, x fastest), with the separable sliding window passes:
   * cost ~ cells * a few operations whatever the window. Needs the samples [cx0, cx0 + kTile + n] x [cz0, cz0 + kTile + n].
   */
  template <class Sampler>
  inline void RateTile(const int kind, const int n, const int cx0, const int cz0, Sampler&& h, std::uint16_t* const out)
  {
    const int sw = kTile + n + 1;
    std::vector<int> H(static_cast<std::size_t>(sw) * sw);
    for (int j = 0; j < sw; ++j) {
      for (int i = 0; i < sw; ++i) {
        H[static_cast<std::size_t>(j) * sw + i] = h(cx0 + i, cz0 + j);
      }
    }
    std::vector<int> scratch(static_cast<std::size_t>(sw) * 2);
    std::vector<int> t1(static_cast<std::size_t>(sw) * sw), t2(static_cast<std::size_t>(sw) * sw);
    std::vector<int> r1(static_cast<std::size_t>(kTile) * kTile), r2(static_cast<std::size_t>(kTile) * kTile);
    const auto clampWord = [](const int v) { return static_cast<std::uint16_t>(std::min(65535, std::max(0, v))); };
    if (kind == 0) {
      // A[z][i] = max over x in [i, i + n - 1] of |h(x + 1, z) - h(x, z)|, then max over z in [j, j + n] (window n + 1)
      for (int z = 0; z < sw; ++z) {
        for (int x = 0; x + 1 < sw; ++x) {
          t1[static_cast<std::size_t>(z) * sw + x] = std::abs(H[static_cast<std::size_t>(z) * sw + x + 1] - H[static_cast<std::size_t>(z) * sw + x]);
        }
        SlidingExtreme<true>(&t1[static_cast<std::size_t>(z) * sw], 1, sw - 1, n, &t2[static_cast<std::size_t>(z) * sw], 1, scratch.data(), kTile);
      }
      for (int i = 0; i < kTile; ++i) {
        SlidingExtreme<true>(&t2[i], sw, sw, n + 1, &r1[i], kTile, scratch.data(), kTile);
      }
      // B[j][x] = max over z in [j, j + n - 1] of |h(x, z + 1) - h(x, z)|, then max over x in [i, i + n]
      for (int z = 0; z + 1 < sw; ++z) {
        for (int x = 0; x < sw; ++x) {
          t1[static_cast<std::size_t>(z) * sw + x] = std::abs(H[static_cast<std::size_t>(z + 1) * sw + x] - H[static_cast<std::size_t>(z) * sw + x]);
        }
      }
      for (int x = 0; x < sw; ++x) {
        SlidingExtreme<true>(&t1[x], sw, sw - 1, n, &t2[x], sw, scratch.data(), kTile);
      }
      for (int j = 0; j < kTile; ++j) {
        SlidingExtreme<true>(&t2[static_cast<std::size_t>(j) * sw], 1, sw, n + 1, &r2[static_cast<std::size_t>(j) * kTile], 1, scratch.data(), kTile);
      }
      for (std::size_t k = 0; k < r1.size(); ++k) {
        out[k] = clampWord(std::max(r1[k], r2[k]));
      }
    } else {
      // hi / lo over x in [i, i + n] per row, then over z in [j, j + n]
      std::vector<int> lo2(static_cast<std::size_t>(sw) * sw);
      for (int z = 0; z < sw; ++z) {
        SlidingExtreme<true>(&H[static_cast<std::size_t>(z) * sw], 1, sw, n + 1, &t2[static_cast<std::size_t>(z) * sw], 1, scratch.data(), kTile);
        SlidingExtreme<false>(&H[static_cast<std::size_t>(z) * sw], 1, sw, n + 1, &lo2[static_cast<std::size_t>(z) * sw], 1, scratch.data(), kTile);
      }
      for (int i = 0; i < kTile; ++i) {
        SlidingExtreme<true>(&t2[i], sw, sw, n + 1, &r1[i], kTile, scratch.data(), kTile);
        SlidingExtreme<false>(&lo2[i], sw, sw, n + 1, &r2[i], kTile, scratch.data(), kTile);
      }
      for (std::size_t k = 0; k < r1.size(); ++k) {
        out[k] = clampWord(r1[k] - r2[k]);
      }
    }
  }

  /** Offset of block level `level` (cells per block 1 << level) in a tile's data: level 0 first (64 x 64), then 32 x 32 ... down to 1 x 1. */
  [[nodiscard]] constexpr std::size_t LevelOffset(const int level)
  {
    std::size_t off = 0;
    for (int l = 0; l < level; ++l) {
      const std::size_t side = static_cast<std::size_t>(kTile >> l);
      off += side * side;
    }
    return off;
  }

  constexpr std::size_t kTileWords = LevelOffset(kLevels);

  /** Fills the levels above 0 of a tile's data with the maximum of the four blocks below. */
  inline void BuildLevels(std::uint16_t* const data)
  {
    for (int l = 1; l < kLevels; ++l) {
      const int side = kTile >> l;
      const std::uint16_t* const src = data + LevelOffset(l - 1);
      std::uint16_t* const dst = data + LevelOffset(l);
      const int srcSide = side * 2;
      for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
          const std::uint16_t* const p = src + static_cast<std::size_t>(j * 2) * srcSide + i * 2;
          dst[static_cast<std::size_t>(j) * side + i] = std::max(std::max(p[0], p[1]), std::max(p[srcSide], p[srcSide + 1]));
        }
      }
    }
  }

  struct Tile
  {
    std::vector<std::uint16_t> data;   // empty until rated; kTileWords words
    bool dirty = true;                 // heights changed since it was rated (the old ratings stay drawn until it is rated again)
    unsigned usedAt = 0;
  };

  /** The tiles of one map for one rule. */
  class Store
  {
  public:
    /** A new map size or an unusable store: forget everything. */
    void Reset(const int width, const int height)
    {
      tilesX_ = (width + kTile - 1) / kTile;
      tilesZ_ = (height + kTile - 1) / kTile;
      width_ = width;
      height_ = height;
      tiles_.clear();
      tiles_.resize(static_cast<std::size_t>(std::max(0, tilesX_)) * static_cast<std::size_t>(std::max(0, tilesZ_)));
      built_ = 0;
      kind_ = -1;
    }

    [[nodiscard]] int Width() const noexcept { return width_; }
    [[nodiscard]] int Height() const noexcept { return height_; }
    [[nodiscard]] int TilesX() const noexcept { return tilesX_; }
    [[nodiscard]] int TilesZ() const noexcept { return tilesZ_; }
    [[nodiscard]] int Kind() const noexcept { return kind_; }
    [[nodiscard]] int Window() const noexcept { return window_; }
    [[nodiscard]] std::size_t Built() const noexcept { return built_; }

    /** Another rule or window: every rating is wrong, drop them. Returns true when something was dropped. */
    bool SetKey(const int kind, const int window)
    {
      if (kind == kind_ && window == window_) {
        return false;
      }
      for (auto& t : tiles_) {
        t.reset();
      }
      built_ = 0;
      kind_ = kind;
      window_ = window;
      return true;
    }

    /** The samples [x0, x1] x [z0, z1] changed: the cells that look at them (up to kMaxWindow + 1 cells before them) must be rated again. */
    void Invalidate(const int x0, const int z0, const int x1, const int z1)
    {
      if (tiles_.empty()) {
        return;
      }
      const int tx0 = std::max(0, (x0 - kMaxWindow - 1) / kTile);   // (a negative quotient truncates towards zero: still tile 0 or less)
      const int tz0 = std::max(0, (z0 - kMaxWindow - 1) / kTile);
      const int tx1 = std::min(tilesX_ - 1, std::max(0, x1) / kTile);
      const int tz1 = std::min(tilesZ_ - 1, std::max(0, z1) / kTile);
      for (int tz = tz0; tz <= tz1; ++tz) {
        for (int tx = tx0; tx <= tx1; ++tx) {
          if (std::unique_ptr<Tile>& t = tiles_[static_cast<std::size_t>(tz) * tilesX_ + tx]; t) {
            t->dirty = true;
          }
        }
      }
    }

    void InvalidateAll()
    {
      for (auto& t : tiles_) {
        if (t) {
          t->dirty = true;
        }
      }
    }

    /** The tile, or null when it was never asked for. */
    [[nodiscard]] Tile* Find(const int tx, const int tz) const
    {
      if (tx < 0 || tz < 0 || tx >= tilesX_ || tz >= tilesZ_) {
        return nullptr;
      }
      return tiles_[static_cast<std::size_t>(tz) * tilesX_ + tx].get();
    }

    /** True when the tile has no ratings yet or its heights changed. */
    [[nodiscard]] bool NeedsRating(const int tx, const int tz) const
    {
      const Tile* const t = Find(tx, tz);
      return t == nullptr || t->data.empty() || t->dirty;
    }

    /** Rates one tile now. */
    template <class Sampler>
    void Rate(const int tx, const int tz, Sampler&& h, const unsigned frame)
    {
      if (tx < 0 || tz < 0 || tx >= tilesX_ || tz >= tilesZ_) {
        return;
      }
      std::unique_ptr<Tile>& slot = tiles_[static_cast<std::size_t>(tz) * tilesX_ + tx];
      if (!slot) {
        slot = std::make_unique<Tile>();
      }
      Tile& t = *slot;
      if (t.data.empty()) {
        t.data.assign(kTileWords, 0);
        ++built_;
      }
      RateTile(kind_, window_, tx * kTile, tz * kTile, h, t.data.data());
      BuildLevels(t.data.data());
      t.dirty = false;
      t.usedAt = frame;
    }

    /** The rating (height words) of block (bx, bz) of `level` (the block covers cells bx << level ...); 0 where no tile is rated yet. */
    [[nodiscard]] int Block(const int level, const int bx, const int bz) const
    {
      const int cx = bx << level;
      const int cz = bz << level;
      const Tile* const t = Find(cx / kTile, cz / kTile);
      if (t == nullptr || t->data.empty()) {
        return 0;
      }
      const int side = kTile >> level;
      const int lx = (cx % kTile) >> level;
      const int lz = (cz % kTile) >> level;
      return t->data[LevelOffset(level) + static_cast<std::size_t>(lz) * side + lx];
    }

    /** Frees the ratings of tiles that were not used for `age` frames (keeps the memory of a map explored at length bounded). */
    void Trim(const unsigned frame, const unsigned age)
    {
      for (auto& t : tiles_) {
        if (t && frame - t->usedAt > age) {
          t.reset();
          if (built_ > 0) {
            --built_;
          }
        }
      }
    }

    void Touch(const int tx, const int tz, const unsigned frame)
    {
      if (Tile* const t = Find(tx, tz); t != nullptr) {
        t->usedAt = frame;
      }
    }

  private:
    int width_ = 0, height_ = 0, tilesX_ = 0, tilesZ_ = 0;
    int kind_ = -1, window_ = 0;
    std::size_t built_ = 0;
    std::vector<std::unique_ptr<Tile>> tiles_;
  };

  /** The block size for a view `cells` wide when at most `maxBlocks` blocks per side are wanted: the smallest power of two (as a level) that fits. */
  [[nodiscard]] inline int LevelForView(const int cells, const int maxBlocks)
  {
    int level = 0;
    while (level < kLevels - 1 && (cells + (1 << level) - 1) / (1 << level) > maxBlocks) {
      ++level;
    }
    return level;
  }

  /**
   * Colour step of a rating `value` against `limit` (both in the same unit): 0 = flat (not drawn), 1 .. kSteps = green to red up to the limit, kSteps + 1 = beyond
   * the limit. Neighbouring blocks with the same step get the same colour, so a run of them is drawn as one quad.
   */
  constexpr int kSteps = 24;
  [[nodiscard]] inline int ColorStep(const float value, const float limit)
  {
    const float t = value / limit;
    if (t > 1.0f) {
      return kSteps + 1;
    }
    const int step = static_cast<int>(t * static_cast<float>(kSteps));
    return std::min(kSteps, std::max(0, step));
  }
} // namespace moho::editor::overlayslope
