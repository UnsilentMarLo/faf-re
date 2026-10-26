// Stand-alone test of the node grid cell runs (EdOverlayNodes.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdOverlayNodesTest.cpp -o EdOverlayNodesTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdOverlayNodesTest.cpp
#include <cstdio>
#include <vector>

#include "moho/editor/EdOverlayNodes.h"

namespace on = moho::editor::overlaynodes;

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

int main()
{
  std::vector<on::Run> runs;
  {
    const std::vector<std::uint8_t> zero(6 * 5, 0);
    on::CollectRuns(zero.data(), 6, 5, 120, runs);
    CHECK(runs.empty(), "an empty grid draws nothing");
  }
  {
    std::vector<std::uint8_t> v(6 * 5, 255);
    on::CollectRuns(v.data(), 6, 5, 120, runs);
    // 5 x 4 cells, 4 rows of 5 cells: one run of 5 per row
    CHECK(runs.size() == 4, "a solid 6 x 5 grid is four runs (%zu)", runs.size());
    bool ok = true;
    int cells = 0;
    for (const auto& r : runs) {
      ok = ok && r.len == 5 && r.alpha == 120u;
      cells += r.len;
    }
    CHECK(ok && cells == 20, "runs cover all 20 cells");
  }
  {
    std::vector<std::uint8_t> v(20 * 2, 255);   // 19 cells in one row: runs of at most 8
    on::CollectRuns(v.data(), 20, 2, 255, runs);
    CHECK(runs.size() == 3 && runs[0].len == 8 && runs[1].len == 8 && runs[2].len == 3, "long rows are cut (%zu runs)", runs.size());
  }
  {
    // one node set: the four cells around it are drawn (their corners differ), the rest is not
    std::vector<std::uint8_t> v(5 * 5, 0);
    v[2 * 5 + 2] = 255;
    on::CollectRuns(v.data(), 5, 5, 200, runs);
    CHECK(runs.size() == 4, "a single node touches four cells (%zu)", runs.size());
    bool ok = true;
    for (const auto& r : runs) ok = ok && r.len == 1 && r.i >= 1 && r.i <= 2 && r.j >= 1 && r.j <= 2;
    CHECK(ok, "and those are the cells around it");
  }
  {
    // an alpha that rounds to zero draws nothing
    std::vector<std::uint8_t> v(4 * 4, 1);
    on::CollectRuns(v.data(), 4, 4, 100, runs);   // 1 * 100 / 255 = 0
    CHECK(runs.empty(), "alpha 0 after scaling is skipped");
    on::CollectRuns(v.data(), 4, 4, 0, runs);
    CHECK(runs.empty(), "max alpha 0 draws nothing");
  }
  {
    // a fade (different corner alphas) is never merged
    std::vector<std::uint8_t> v(4 * 2);
    for (int i = 0; i < 4; ++i) { v[i] = static_cast<std::uint8_t>(i * 60); v[4 + i] = static_cast<std::uint8_t>(i * 60); }
    on::CollectRuns(v.data(), 4, 2, 255, runs);
    bool ok = !runs.empty();
    for (const auto& r : runs) ok = ok && r.len == 1;
    CHECK(ok, "a gradient keeps its cells");
  }
  std::printf("EdOverlayNodesTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
