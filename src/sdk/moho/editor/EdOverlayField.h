// EDITOR: a float field (the field engine's grids, EdFieldCore.h) as a ground overlay: the pure parts, engine independent so they can be tested on their own
// (moho/editor/tests/EdOverlayFieldTest.cpp). The draw is in EdOverlay.cpp (ed_overlay_field).
//
// The overlay shows a field the way the old Lua preview did (a mask as coloured squares), but without Lua touching a single sample:
//   - `MapOpts` turns a field value into an alpha level 0..255:  t = v * scale + bias;  invert: t = 1 - t;  t <= cutoff -> 0;  level = round(min(1, t) * 255).
//   - `Pyramid` keeps that level for every field sample and, for every 2 x 2 x ... block of samples, the AVERAGE and the MAXIMUM level (a mip chain, built once per field
//     and mapping, a few ms for a 1025 x 1025 field). Reducing the mapped levels (not the raw values) means "alpha of the reduced value" and "reduced alpha" are the same
//     thing, whatever scale / bias / invert say.
//   - A node of the ground quadtree (EdOverlayLod.h) at block level L sits on a multiple of 2^L map units. Its value is the reduction (average for masks, maximum for thin
//     features) of the field over the 2^L x 2^L box CENTRED on the node, so the nodes of one level agree with each other (no seams between neighbouring leaves of a level)
//     and a far node stands for the whole ground around it. A leaf's four corners carry the four values as the alpha of its colour: a coarse leaf is a soft blob, a fine
//     leaf shows single samples.
//   - A block whose (grown) box holds no alpha at all is not drawn and not cut up.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace moho::editor::overlayfield
{
  // ---- refinement (the leaves of the quadtree) ---------------------------------------------------------------------------------------------------------------------
  constexpr float kRefinePx = 8.0f;     // a leaf bigger than this on screen is cut into four ... (the slope view uses 16: a mask has finer detail than a slope rating)
  constexpr float kMergePx = 5.5f;      // ... and merged again below this
  constexpr int kMaxLeaves = 24000;     // nearest blocks first: when it runs out the far ground stays coarse, never the ground under the camera
  constexpr int kQuadCap = 120000;
  constexpr float kMovingPxScale = 2.5f;   // while the camera moves the tree is rebuilt every frame: blocks may be this much bigger on screen, and a quarter of the leaves are allowed
  constexpr int kRootLevel = 6;         // as EdOverlayLod.h kRootLevel (a root block is 64 map units)

  /** The finest block level worth drawing for a field whose samples are `cell` map units apart: a leaf smaller than a field sample shows nothing new. */
  [[nodiscard]] inline int MinLevel(const float cell) noexcept
  {
    int level = 0;
    while (level < kRootLevel && static_cast<float>(1 << (level + 1)) <= cell) {
      ++level;
    }
    return level;
  }

  // ---- value -> alpha level --------------------------------------------------------------------------------------------------------------------------------------
  struct MapOpts
  {
    float scale = 1.0f;
    float bias = 0.0f;
    bool invert = false;
    float cutoff = 0.02f;   // levels of t at or below this draw nothing (a mask's faint noise floor)

    [[nodiscard]] bool operator==(const MapOpts& o) const noexcept { return scale == o.scale && bias == o.bias && invert == o.invert && cutoff == o.cutoff; }
    [[nodiscard]] bool operator!=(const MapOpts& o) const noexcept { return !(*this == o); }
  };

  /** The alpha level 0 .. 255 of a field value. NaN draws nothing. */
  [[nodiscard]] inline std::uint8_t MapValue(const float v, const MapOpts& o) noexcept
  {
    float t = v * o.scale + o.bias;
    if (o.invert) {
      t = 1.0f - t;
    }
    if (!(t > o.cutoff)) {
      return 0;
    }
    return static_cast<std::uint8_t>(std::min(1.0f, t) * 255.0f + 0.5f);
  }

  /** The alpha (0 .. maxAlpha) of a reduced level `q` (0 .. 255). */
  [[nodiscard]] inline std::uint32_t AlphaOf(const float q, const std::uint32_t maxAlpha) noexcept
  {
    const float a = q * static_cast<float>(maxAlpha) / 255.0f + 0.5f;
    return std::min(maxAlpha, static_cast<std::uint32_t>(std::max(0.0f, a)));
  }

  // ---- the mip chain of alpha levels ----------------------------------------------------------------------------------------------------------------------------
  //
  // Sample space: sample i occupies [i - 0.5, i + 0.5] (its centre at i), the field covers [-0.5, w - 0.5] x [-0.5, h - 0.5]. Level k holds one texel per 2^k x 2^k block of
  // samples (a block at the right / bottom edge holds the samples that exist); texel t covers [t * 2^k - 0.5, (t + 1) * 2^k - 0.5].
  class Pyramid
  {
  public:
    void Build(const float* const v, const int w, const int h, const MapOpts& o)
    {
      lv_.clear();
      if (v == nullptr || w < 1 || h < 1) {
        return;
      }
      Level base;
      base.w = w;
      base.h = h;
      base.avg.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
      for (std::size_t i = 0; i < base.avg.size(); ++i) {
        base.avg[i] = MapValue(v[i], o);
      }
      lv_.push_back(std::move(base));
      while ((lv_.back().w > 1 || lv_.back().h > 1) && lv_.size() < 16u) {
        const int k = static_cast<int>(lv_.size()) - 1;
        const Level& c = lv_[static_cast<std::size_t>(k)];
        Level p;
        p.w = (c.w + 1) / 2;
        p.h = (c.h + 1) / 2;
        p.avg.resize(static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h));
        p.max.resize(p.avg.size());
        for (int j = 0; j < p.h; ++j) {
          for (int i = 0; i < p.w; ++i) {
            double sum = 0.0, weight = 0.0;
            std::uint8_t mx = 0;
            for (int dz = 0; dz < 2; ++dz) {
              for (int dx = 0; dx < 2; ++dx) {
                const int ci = 2 * i + dx, cj = 2 * j + dz;
                if (ci >= c.w || cj >= c.h) {
                  continue;
                }
                const std::size_t ck = static_cast<std::size_t>(cj) * static_cast<std::size_t>(c.w) + static_cast<std::size_t>(ci);
                const double n = static_cast<double>(Count(k, ci, w)) * static_cast<double>(Count(k, cj, h));
                sum += n * c.avg[ck];
                weight += n;
                mx = std::max(mx, k == 0 ? c.avg[ck] : c.max[ck]);
              }
            }
            const std::size_t pk = static_cast<std::size_t>(j) * static_cast<std::size_t>(p.w) + static_cast<std::size_t>(i);
            p.avg[pk] = static_cast<std::uint8_t>(weight > 0.0 ? sum / weight + 0.5 : 0.0);
            p.max[pk] = mx;
          }
        }
        lv_.push_back(std::move(p));
      }
    }

    [[nodiscard]] bool Empty() const noexcept { return lv_.empty(); }
    [[nodiscard]] int Width() const noexcept { return lv_.empty() ? 0 : lv_[0].w; }
    [[nodiscard]] int Height() const noexcept { return lv_.empty() ? 0 : lv_[0].h; }
    [[nodiscard]] int Levels() const noexcept { return static_cast<int>(lv_.size()); }
    /** The alpha level of one sample. */
    [[nodiscard]] std::uint8_t Sample(const int i, const int j) const { return lv_[0].avg[static_cast<std::size_t>(j) * static_cast<std::size_t>(lv_[0].w) + static_cast<std::size_t>(i)]; }
    /** The average / maximum level of texel (i, j) of level k (level 0: the sample itself). */
    [[nodiscard]] std::uint8_t Texel(const int k, const int i, const int j, const bool useMax) const
    {
      const Level& l = lv_[static_cast<std::size_t>(k)];
      const std::size_t at = static_cast<std::size_t>(j) * static_cast<std::size_t>(l.w) + static_cast<std::size_t>(i);
      return (useMax && k > 0) ? l.max[at] : l.avg[at];
    }

    /** Bilinear level at a position in sample space; 0 beyond the field's extent. */
    [[nodiscard]] float Point(const double sx, const double sz) const
    {
      if (lv_.empty() || sx < -0.5 || sz < -0.5 || sx > Width() - 0.5 || sz > Height() - 0.5) {
        return 0.0f;
      }
      const double cx = std::min(std::max(sx, 0.0), static_cast<double>(Width() - 1));
      const double cz = std::min(std::max(sz, 0.0), static_cast<double>(Height() - 1));
      const int ix = std::min(Width() - 1, static_cast<int>(cx)), iz = std::min(Height() - 1, static_cast<int>(cz));
      const int ix1 = std::min(Width() - 1, ix + 1), iz1 = std::min(Height() - 1, iz + 1);
      const float tx = static_cast<float>(cx - ix), tz = static_cast<float>(cz - iz);
      const float top = Sample(ix, iz) + (static_cast<float>(Sample(ix1, iz)) - Sample(ix, iz)) * tx;
      const float bottom = Sample(ix, iz1) + (static_cast<float>(Sample(ix1, iz1)) - Sample(ix, iz1)) * tx;
      return top + (bottom - top) * tz;
    }

    /**
     * The average (or maximum) alpha level over the box [x0, x1] x [z0, z1] of sample space (the part inside the field; 0 when none is). The mip level is the one whose texels
     * are at most as big as the box's longer side, so the box covers a handful of texels (at most 3 x 3 for a square box); the average weighs each texel by the area of it inside
     * the box, the maximum takes every texel the box touches.
     */
    [[nodiscard]] float Reduce(double x0, double z0, double x1, double z1, const bool useMax) const
    {
      if (lv_.empty()) {
        return 0.0f;
      }
      x0 = std::max(x0, -0.5);
      z0 = std::max(z0, -0.5);
      x1 = std::min(x1, Width() - 0.5);
      z1 = std::min(z1, Height() - 0.5);
      if (!(x1 > x0) || !(z1 > z0)) {
        return 0.0f;
      }
      const double longer = std::max(x1 - x0, z1 - z0);
      int k = 0;
      while (k + 1 < Levels() && static_cast<double>(1 << (k + 1)) <= longer) {
        ++k;
      }
      const Level& l = lv_[static_cast<std::size_t>(k)];
      const double size = static_cast<double>(1 << k);
      const int tx0 = std::max(0, static_cast<int>(std::floor((x0 + 0.5) / size))), tx1 = std::min(l.w - 1, static_cast<int>(std::floor((x1 + 0.5) / size)));
      const int tz0 = std::max(0, static_cast<int>(std::floor((z0 + 0.5) / size))), tz1 = std::min(l.h - 1, static_cast<int>(std::floor((z1 + 0.5) / size)));
      double sum = 0.0, weight = 0.0;
      int mx = 0;
      for (int tz = tz0; tz <= tz1; ++tz) {
        const double oz = std::min(z1, (tz + 1) * size - 0.5) - std::max(z0, tz * size - 0.5);
        if (!(oz > 1.0e-9)) {
          continue;
        }
        for (int tx = tx0; tx <= tx1; ++tx) {
          const double ox = std::min(x1, (tx + 1) * size - 0.5) - std::max(x0, tx * size - 0.5);
          if (!(ox > 1.0e-9)) {
            continue;
          }
          const std::size_t at = static_cast<std::size_t>(tz) * static_cast<std::size_t>(l.w) + static_cast<std::size_t>(tx);
          sum += ox * oz * l.avg[at];
          weight += ox * oz;
          mx = std::max<int>(mx, (k > 0) ? l.max[at] : l.avg[at]);
        }
      }
      if (useMax) {
        return static_cast<float>(mx);
      }
      return weight > 0.0 ? static_cast<float>(sum / weight) : 0.0f;
    }

  private:
    struct Level
    {
      int w = 0, h = 0;
      std::vector<std::uint8_t> avg, max;   // max is empty at level 0 (the samples themselves)
    };

    /** Samples a texel of level k covers along one axis (the last one may be short). */
    static int Count(const int k, const int index, const int total) noexcept
    {
      const long long lo = static_cast<long long>(index) << k, hi = std::min<long long>(total, (static_cast<long long>(index) + 1) << k);
      return static_cast<int>(std::max<long long>(0, hi - lo));
    }

    std::vector<Level> lv_;
  };

  // ---- ground nodes and leaves -------------------------------------------------------------------------------------------------------------------------------------
  /** Where the field lies in map units: sample (i, j) at (ox + i * cell, oz + j * cell). */
  struct Placement
  {
    float ox = 0.0f, oz = 0.0f, cell = 1.0f;
    int w = 0, h = 0;
  };

  /** The world box a field covers: [ox - cell / 2, ox + (w - 0.5) * cell] (every sample owns a cell-sized square). */
  [[nodiscard]] inline double ExtentMinX(const Placement& p) noexcept { return static_cast<double>(p.ox) - 0.5 * p.cell; }
  [[nodiscard]] inline double ExtentMinZ(const Placement& p) noexcept { return static_cast<double>(p.oz) - 0.5 * p.cell; }
  [[nodiscard]] inline double ExtentMaxX(const Placement& p) noexcept { return static_cast<double>(p.ox) + (p.w - 0.5) * p.cell; }
  [[nodiscard]] inline double ExtentMaxZ(const Placement& p) noexcept { return static_cast<double>(p.oz) + (p.h - 0.5) * p.cell; }

  /**
   * The level (0 .. 255) of a ground node at world (wx, wz) of a quadtree block level whose cells are `blockSize` map units: the reduction of the field over the box of
   * blockSize x blockSize map units centred on the node. A box smaller than ~1.5 field samples is a plain bilinear lookup (a leaf finer than the field).
   */
  [[nodiscard]] inline float NodeLevel(const Pyramid& py, const Placement& p, const double wx, const double wz, const double blockSize, const bool useMax)
  {
    const double c = p.cell > 0.0f ? static_cast<double>(p.cell) : 1.0;
    const double sx = (wx - p.ox) / c, sz = (wz - p.oz) / c;
    const double half = 0.5 * blockSize / c;
    if (half <= 0.75) {
      return py.Point(sx, sz);
    }
    return py.Reduce(sx - half, sz - half, sx + half, sz + half, useMax);
  }

  /** The largest level anywhere in the block [x0, x0 + size] x [z0, z0 + size] grown by half a block on every side (what its corner boxes can see): 0 means draw nothing. */
  [[nodiscard]] inline float BlockMaxLevel(const Pyramid& py, const Placement& p, const int x0, const int z0, const int size)
  {
    const double c = p.cell > 0.0f ? static_cast<double>(p.cell) : 1.0;
    const double g = 0.5 * size;
    return py.Reduce((x0 - g - p.ox) / c, (z0 - g - p.oz) / c, (x0 + size + g - p.ox) / c, (z0 + size + g - p.oz) / c, true);
  }

  /** The four corner alphas of the leaf [x0, x0 + size] x [z0, z0 + size], in the order (x0, z0), (x1, z0), (x1, z1), (x0, z1). Returns true when any is above zero. */
  inline bool LeafAlphas(const Pyramid& py, const Placement& p, const int x0, const int z0, const int size, const std::uint32_t maxAlpha, const bool useMax, std::uint32_t* const out)
  {
    const double s = static_cast<double>(size);
    const double xs[4] = {static_cast<double>(x0), static_cast<double>(x0) + s, static_cast<double>(x0) + s, static_cast<double>(x0)};
    const double zs[4] = {static_cast<double>(z0), static_cast<double>(z0), static_cast<double>(z0) + s, static_cast<double>(z0) + s};
    std::uint32_t any = 0;
    for (int k = 0; k < 4; ++k) {
      out[k] = AlphaOf(NodeLevel(py, p, xs[k], zs[k], s, useMax), maxAlpha);
      any |= out[k];
    }
    return any != 0u;
  }
} // namespace moho::editor::overlayfield
