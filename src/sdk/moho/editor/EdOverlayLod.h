// EDITOR: distance adaptive detail for the slope view and the build grid (EdOverlay.cpp), engine independent so it can be tested on its own
// (moho/editor/tests/EdOverlayLodTest.cpp).
//
// Both overlays are a quadtree of ground blocks. A block of level L covers (1 << L) x (1 << L) cells and is aligned to multiples of its size, so it does not shift
// while the camera moves. The roots are level kRootLevel (64 cells = one tile of the slope store) blocks over the visible ground rectangle; a block is cut into its four
// children while it is big on the SCREEN (its projected size, `ProjectedSize`), down to single cells. A block under the camera is therefore as fine as a block at the
// horizon is coarse: the detail does not depend on how far the visible rectangle reaches.
//   - `ShouldRefine` has hysteresis (a block that was cut last frame stays cut until it is clearly smaller than the refine threshold, `RefineState` keeps that between
//     frames), so a zoom does not make blocks pop at the threshold.
//   - `BuildTree` handles the blocks nearest the camera first and stops cutting when the leaf budget is spent, so a frame costs at most the budget and the far ground stays
//     coarse when the budget runs out, never the ground under the camera.
//   - `NodeTable` caches the projections of the ground nodes (integer ground positions): the corners shared by neighbouring blocks, and everything between frames while
//     the camera and the heights stay the same, are projected once; a level change does not project anything that is already known.
//   - The grid draws the outlines of the leaves (`GridSides`: every line once), the slope view fills them with their rating.
// Also here: the depth an overlay vertex is given so the terrain can hide it (`FitDepth`, `BiasedDepth`, `VertexZ`).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace moho::editor::overlaylod
{
  constexpr int kRootLevel = 6;   // 64 cells: one tile of the slope store (EdOverlaySlope.h kTile)

  // ---- refinement thresholds (pixels of the longest projected edge of a block, `ProjectedSize`) ----------------------------------------------------------------
  constexpr float kSlopeRefinePx = 16.0f;   // a block bigger than this is cut into four ...
  constexpr float kSlopeMergePx = 11.0f;    // ... and the four are merged again below this
  constexpr float kGridRefinePx = 20.0f;    // grid: lines every 2^L cells while a block of 2^L cells is at most 20 px: a cell is shown from ~10 px on
  constexpr float kGridMergePx = 14.0f;
  constexpr int kSlopeMaxLeaves = 40000;
  constexpr int kGridMaxLeaves = 20000;
  constexpr int kSlopeQuadCap = 120000;
  constexpr int kGridSegmentCap = 100000;

  struct Thresholds
  {
    float refinePx = kSlopeRefinePx;
    float mergePx = kSlopeMergePx;
  };

  /** Cut a block into four? A block that is already cut stays cut down to `mergePx`; a block that is not is cut from `refinePx` on. */
  [[nodiscard]] inline bool ShouldRefine(const float projectedPx, const bool wasRefined, const Thresholds& t) noexcept
  {
    return projectedPx > (wasRefined ? t.mergePx : t.refinePx);
  }

  /**
   * The size of a block on screen: its longest projected edge, but never more than kAniso times the shorter of the two axis lengths (+ kAnisoPad). A block seen at a
   * grazing angle is long and thin: cutting it up would only make thousands of slivers a pixel high. Corner order: (x0, z0), (x1, z0), (x1, z1), (x0, z1).
   */
  constexpr float kAniso = 4.0f;
  constexpr float kAnisoPad = 3.0f;

  [[nodiscard]] inline float ProjectedSize(const float* const sx, const float* const sy) noexcept
  {
    const auto len = [&](const int a, const int b) { return std::sqrt((sx[a] - sx[b]) * (sx[a] - sx[b]) + (sy[a] - sy[b]) * (sy[a] - sy[b])); };
    const float e0 = len(0, 1), e1 = len(1, 2), e2 = len(3, 2), e3 = len(0, 3);
    const float longest = std::max(std::max(e0, e1), std::max(e2, e3));
    const float shorter = std::min(0.5f * (e0 + e2), 0.5f * (e1 + e3));
    return std::min(longest, kAniso * shorter + kAnisoPad);
  }

  struct Key
  {
    int level = 0, bx = 0, bz = 0;   // block (bx, bz) of `level`: cells [bx << level, (bx + 1) << level) ...
  };

  [[nodiscard]] inline std::uint64_t Pack(const Key& k) noexcept
  {
    return (static_cast<std::uint64_t>(static_cast<unsigned>(k.level) & 0xFFu) << 56) | ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.bx)) & 0xFFFFFFFull) << 28) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.bz)) & 0xFFFFFFFull);
  }

  /** What the client says about a block: is any of it on screen, how big is it there, how far from the camera (the clip w; nearest first). */
  struct NodeInfo
  {
    bool visible = false;
    float px = 0.0f;
    float dist = 0.0f;
  };

  /** Blocks refined in the last frame, with the frame stamp: an entry that a frame did not touch is dropped (EndFrame). */
  class RefineState
  {
  public:
    [[nodiscard]] bool WasRefined(const Key& k) const { return m_.find(Pack(k)) != m_.end(); }
    void Mark(const Key& k, const unsigned frame) { m_[Pack(k)] = frame; }
    void EndFrame(const unsigned frame)
    {
      for (auto it = m_.begin(); it != m_.end();) {
        if (it->second != frame) {
          it = m_.erase(it);
        } else {
          ++it;
        }
      }
    }
    void Clear() { m_.clear(); }
    [[nodiscard]] std::size_t Size() const { return m_.size(); }

  private:
    std::unordered_map<std::uint64_t, unsigned> m_;
  };

  struct Candidate
  {
    Key key;
    NodeInfo info;
  };

  struct Leaf
  {
    Key key;
    float px = 0.0f;
  };

  struct Tree
  {
    std::vector<Leaf> leaves;
    std::unordered_set<std::uint64_t> internal;   // blocks that were cut
    std::unordered_set<std::uint64_t> all;        // every block that is a leaf or was cut
    int refined = 0;
  };

  struct TreeParams
  {
    Thresholds th;
    int maxLeaves = kSlopeMaxLeaves;
    int minLevel = 0;
  };

  /**
   * Builds the leaves from the visible root blocks. probe(const Key&) -> NodeInfo is asked for the four children of every block that is cut (visible == false drops the child:
   * off screen, behind the camera, or nothing to draw). The nearest block is handled first: when `maxLeaves` is reached the rest stays as it is, so the far ground stays coarse.
   */
  template <class Probe>
  void BuildTree(const std::vector<Candidate>& roots, Probe&& probe, RefineState& state, const unsigned frame, const TreeParams& p, Tree& out)
  {
    out.leaves.clear();
    out.internal.clear();
    out.all.clear();
    out.refined = 0;
    struct Farther
    {
      bool operator()(const Candidate& a, const Candidate& b) const { return a.info.dist > b.info.dist; }
    };
    std::priority_queue<Candidate, std::vector<Candidate>, Farther> q;
    for (const Candidate& c : roots) {
      q.push(c);
    }
    const std::size_t cap = static_cast<std::size_t>(std::max(1, p.maxLeaves));
    while (!q.empty()) {
      const Candidate c = q.top();
      q.pop();
      out.all.insert(Pack(c.key));
      const bool room = out.leaves.size() + q.size() + 4u <= cap;
      if (c.key.level > p.minLevel && room && ShouldRefine(c.info.px, state.WasRefined(c.key), p.th)) {
        state.Mark(c.key, frame);
        out.internal.insert(Pack(c.key));
        ++out.refined;
        for (int j = 0; j < 2; ++j) {
          for (int i = 0; i < 2; ++i) {
            const Key child{c.key.level - 1, c.key.bx * 2 + i, c.key.bz * 2 + j};
            const NodeInfo info = probe(child);
            if (info.visible) {
              q.push(Candidate{child, info});
            }
          }
        }
      } else {
        out.leaves.push_back(Leaf{c.key, c.info.px});
      }
    }
    state.EndFrame(frame);
  }

  // ---- neighbours -------------------------------------------------------------------------------------------------------------------------------------------------
  enum Side : int
  {
    kLeft = 1,     // the edge at x = x0
    kRight = 2,    // x = x1
    kTop = 4,      // z = z0
    kBottom = 8    // z = z1
  };

  [[nodiscard]] inline Key Neighbour(const Key& k, const int dx, const int dz) noexcept
  {
    return Key{k.level, k.bx + dx, k.bz + dz};
  }

  /** True when the block, or a coarser block that covers it, is in the tree (a leaf or cut). */
  [[nodiscard]] inline bool Present(const std::unordered_set<std::uint64_t>& all, Key k)
  {
    for (;;) {
      if (all.find(Pack(k)) != all.end()) {
        return true;
      }
      if (k.level >= kRootLevel || k.bx < 0 || k.bz < 0) {
        return false;
      }
      k = Key{k.level + 1, k.bx >> 1, k.bz >> 1};
    }
  }

  /**
   * The sides of a leaf where the neighbour of the same level was cut into finer blocks: the finer blocks have more nodes along the shared edge than the leaf, and over
   * bumpy ground their edge does not meet the leaf's straight one exactly. The slope view grows the leaf over that edge by a pixel (the overlay is translucent: a hairline
   * of overlap is better than a gap).
   */
  [[nodiscard]] inline int GrowSides(const std::unordered_set<std::uint64_t>& internal, const Key& k)
  {
    int s = 0;
    if (internal.find(Pack(Neighbour(k, -1, 0))) != internal.end()) s |= kLeft;
    if (internal.find(Pack(Neighbour(k, 1, 0))) != internal.end()) s |= kRight;
    if (internal.find(Pack(Neighbour(k, 0, -1))) != internal.end()) s |= kTop;
    if (internal.find(Pack(Neighbour(k, 0, 1))) != internal.end()) s |= kBottom;
    return s;
  }

  /**
   * The edges of a leaf the grid draws, so that every grid line is drawn once: the left and top edges always; the right (bottom) edge only when the neighbour across it is
   * not in the tree at all (nothing else draws that line): a neighbour that is a leaf draws its own left edge, a finer one draws its left edges, a coarser one covers the
   * line with its own left edge.
   */
  [[nodiscard]] inline int GridSides(const std::unordered_set<std::uint64_t>& all, const Key& k)
  {
    int s = kLeft | kTop;
    if (!Present(all, Neighbour(k, 1, 0))) s |= kRight;
    if (!Present(all, Neighbour(k, 0, 1))) s |= kBottom;
    return s;
  }

  /** A line at ground coordinate `coord` (a multiple of 1 << level) is a major line every fourth block (brighter), like the old Lua grid. */
  [[nodiscard]] inline bool IsMajor(const int coord, const int level) noexcept
  {
    return ((coord >> level) & 3) == 0;
  }

  /** Alpha (0 .. 255) of a grid line of a block `px` big: full from ~20 px, fading with the block size towards the horizon; major lines are brighter. */
  constexpr float kGridFadeFromPx = 6.0f;
  constexpr float kGridFadeToPx = 20.0f;
  constexpr float kGridMinFade = 0.45f;

  [[nodiscard]] inline int GridAlpha(const float px, const bool major, const float scale = 1.0f) noexcept
  {
    const float t = std::min(1.0f, std::max(0.0f, (px - kGridFadeFromPx) / (kGridFadeToPx - kGridFadeFromPx)));
    const float fade = kGridMinFade + (1.0f - kGridMinFade) * t;
    const float base = major ? 192.0f : 85.0f;
    return static_cast<int>(std::min(255.0f, std::max(0.0f, base * fade * scale)));
  }

  // ---- node cache -------------------------------------------------------------------------------------------------------------------------------------------------
  struct GroundNode
  {
    float x = 0.0f, y = 0.0f, w = 0.0f, z = 0.0f;   // screen position, clip w, the vertex z (VertexZ)
    bool front = false;                              // in front of the camera
  };

  /** Open addressing hash of ground nodes by integer position; Clear() is O(1) (a generation counter). */
  class NodeTable
  {
  public:
    void Clear()
    {
      ++gen_;
      used_ = 0;
      if (gen_ == 0) {
        for (Entry& e : e_) {
          e.gen = 0;
        }
        gen_ = 1;
      }
    }

    [[nodiscard]] std::size_t Size() const noexcept { return used_; }

    /** The node at (x, z); `make(x, z) -> GroundNode` projects it when it is not cached. `created` counts the projections made. */
    template <class Make>
    GroundNode Get(const int x, const int z, Make&& make, int& created)
    {
      if (e_.empty() || (used_ + 1u) * 2u > e_.size()) {
        Grow();
      }
      const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
      const std::size_t mask = e_.size() - 1u;
      std::size_t i = Hash(key) & mask;
      while (e_[i].gen == gen_) {
        if (e_[i].key == key) {
          return e_[i].n;
        }
        i = (i + 1u) & mask;
      }
      const GroundNode n = make(x, z);
      e_[i] = Entry{key, gen_, n};
      ++used_;
      ++created;
      return n;
    }

  private:
    struct Entry
    {
      std::uint64_t key = 0;
      std::uint32_t gen = 0;
      GroundNode n;
    };

    static std::size_t Hash(const std::uint64_t key) noexcept { return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 29); }

    void Grow()
    {
      std::vector<Entry> old;
      old.swap(e_);
      e_.assign(std::max<std::size_t>(1024u, old.empty() ? 0u : old.size() * 2u), Entry{});
      const std::size_t mask = e_.size() - 1u;
      for (const Entry& o : old) {
        if (o.gen != gen_) {
          continue;
        }
        std::size_t i = Hash(o.key) & mask;
        while (e_[i].gen == gen_) {
          i = (i + 1u) & mask;
        }
        e_[i] = o;
      }
    }

    std::vector<Entry> e_;
    std::uint32_t gen_ = 1;
    std::size_t used_ = 0;
  };

  // ---- depth ------------------------------------------------------------------------------------------------------------------------------------------------------
  //
  // The overlay is drawn with screen space vertices (EdOverlay.cpp: MakeViewportPixelProjection maps the vertex z to the depth 0.5 - 0.5 z). To let the terrain hide what is
  // behind it, every vertex gets the depth the camera's own view projection gives its ground point (clip z / clip w), moved a little towards the camera so the overlay wins
  // over the terrain mesh that is tessellated coarser than the height field away from the camera: the bias grows with the distance. Hills in front still hide the overlay.
  //
  // For a perspective projection the depth is  d(w) = a + b / w  (w = the distance along the view direction), so moving a point `s` units towards the camera is exact in
  // that form; a and b are fitted from the matrix columns (clip z = a * clip w + b).
  struct DepthFit
  {
    float a = 0.0f, b = 0.0f;
    bool ok = false;
  };

  /** zx..z0 and wx..w0: the z and w columns of the view projection (clip z = zx * x + zy * y + zz * z + z0, same for w). */
  [[nodiscard]] inline DepthFit FitDepth(const float zx, const float zy, const float zz, const float z0, const float wx, const float wy, const float wz, const float w0) noexcept
  {
    DepthFit f;
    const float ww = wx * wx + wy * wy + wz * wz;
    if (ww < 1.0e-12f) {
      return f;
    }
    f.a = (zx * wx + zy * wy + zz * wz) / ww;
    f.b = z0 - f.a * w0;
    f.ok = true;
    return f;
  }

  constexpr float kDepthBiasBase = 0.25f;      // world units towards the camera at the camera ...
  constexpr float kDepthBiasPerW = 0.008f;     // ... plus this per unit of distance (500 away: 4.25 units)

  [[nodiscard]] inline float DepthBiasWorld(const float w) noexcept
  {
    return std::min(kDepthBiasBase + kDepthBiasPerW * w, 0.4f * w);
  }

  /** Depth buffer value (0 near .. 1 far) of a point with clip z `clipZ` and clip w `w` (> 0), moved towards the camera by DepthBiasWorld(w). */
  [[nodiscard]] inline float BiasedDepth(const float clipZ, const float w, const DepthFit& fit) noexcept
  {
    float d = clipZ / w;
    if (fit.ok) {
      const float s = DepthBiasWorld(w);
      d += fit.b * s / (w * (w - s));
    }
    return std::min(1.0f, std::max(0.0f, d));
  }

  /** The z of a vertex under MakeViewportPixelProjection for the depth `d`: the projection gives depth = 0.5 - 0.5 * z. */
  [[nodiscard]] inline float VertexZ(const float d) noexcept
  {
    return 1.0f - 2.0f * d;
  }
} // namespace moho::editor::overlaylod
