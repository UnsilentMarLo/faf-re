// EDITOR: which cells of a node grid (EdOverlay.cpp, ed_overlay_nodes: brush weights, terrain type layers) need drawing, engine independent so it can be
// tested on its own (moho/editor/tests/EdOverlayNodesTest.cpp).
//
// A grid node (i, j) carries a value 0..255 whose alpha is value * maxAlpha / 255; the cell between four nodes is drawn with the four alphas blended. A cell whose four
// alphas are all zero draws nothing and is left out. A horizontal run of cells whose corners all carry the same alpha looks the same as one wide quad, so such runs
// (at most kMaxRun cells) are drawn as one: a big solid area costs a few quads per row, not one per cell.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace moho::editor::overlaynodes
{
  constexpr int kMaxRun = 8;

  struct Run
  {
    std::uint16_t i, j;   // first cell (its top left node)
    std::uint16_t len;    // cells in the run, 1 .. kMaxRun
    std::uint32_t alpha;  // the common alpha when len > 1 (and all four corners are equal), else unused
  };

  [[nodiscard]] inline std::uint32_t NodeAlpha(const std::uint8_t value, const std::uint32_t maxAlpha)
  {
    return (static_cast<std::uint32_t>(value) * maxAlpha) / 255u;
  }

  /** The runs of drawn cells of an nw x nh grid of node values (row major). */
  inline void CollectRuns(const std::uint8_t* const v, const int nw, const int nh, const std::uint32_t maxAlpha, std::vector<Run>& out)
  {
    out.clear();
    if (nw < 2 || nh < 2 || maxAlpha == 0u) {
      return;
    }
    const auto at = [&](const int i, const int j) { return NodeAlpha(v[static_cast<std::size_t>(j) * nw + i], maxAlpha); };
    for (int j = 0; j + 1 < nh; ++j) {
      int i = 0;
      while (i + 1 < nw) {
        const std::uint32_t a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
        if ((a | b | c | d) == 0u) {
          ++i;
          continue;
        }
        const bool flat = a == b && b == c && c == d;
        int len = 1;
        if (flat) {
          while (len < kMaxRun && i + len + 1 < nw && at(i + len, j) == a && at(i + len + 1, j) == a && at(i + len, j + 1) == a && at(i + len + 1, j + 1) == a) {
            ++len;
          }
        }
        out.push_back(Run{static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(j), static_cast<std::uint16_t>(len), flat ? a : 0u});
        i += len;
      }
    }
  }
} // namespace moho::editor::overlaynodes
