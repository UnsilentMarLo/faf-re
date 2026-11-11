// Stand-alone test of the Terrace and Plane brush modes of the native dab (EdBrush.h, work package WP7 of the upstream port) against upstream's height brush
// (src/editor/tools/EdHeightBrush.cpp, built into the library faf_editor_logic): no engine needed.
//   python scripts/ed_native_tests.py          (EdBrushUpTest links editor/tools/EdHeightBrush.cpp and moho/editor/MapDocument.cpp)
//   MSVC: cl /std:c++20 /EHsc /O2 /I src EdBrushUpTest.cpp editor/tools/EdHeightBrush.cpp moho/editor/MapDocument.cpp
//
//   1. The expectations of upstream's HeightBrushTests "SetHeightTerraceAndPlaneReachTheirTargets" (tests/editor/HeightBrushTests.cpp), run on upstream's ApplyHeightBrush
//      and on our dab: 1000 words terraced by 640 -> 1280; the plane through (0,0,1000) (32,0,1640) (0,32,1320) -> 1000 + 20 x + 10 z; a line of points has no plane.
//   2. Random fields: at full pull (k = 1) and at a partial pull our dab and ApplyHeightBrush give the same words at every sample whose brush weight is 1 in both profiles.
//   3. Outside the brush nothing changes; an unset plane changes nothing; the strength 0 changes nothing.
//   4. The document: a stroke of terrace dabs and plane dabs is one undo step and undo restores the words (the Lua side compares the ed_cmd_undo checksum).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "editor/tools/EdHeightBrush.h"
#include "moho/editor/EdBrush.h"
#include "moho/editor/MapDocument.h"

namespace br = moho::editor::brush;
using moho::editor::CellRect;
using moho::editor::MapDocument;

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
    int w;
    int h;
    std::vector<std::uint16_t> words;
    Field(const int width, const int height, const std::uint16_t fill) : w(width), h(height), words(static_cast<std::size_t>(width) * height, fill) {}
    [[nodiscard]] std::uint16_t At(const int x, const int z) const { return words[static_cast<std::size_t>(z) * w + x]; }
  };

  std::uint32_t gRng = 12345u;
  std::uint32_t Next()
  {
    gRng = gRng * 1664525u + 1013904223u;
    return gRng >> 8;
  }

  /** Our dab on a 33 x 33 field at (cx, cz): radius r, hard edge (falloff 0: weight 1 up to r - 1), strength s. */
  void OurDab(Field& f, br::Dab dab)
  {
    br::Window win;
    std::vector<std::uint16_t> after;
    const br::Result res = br::ComputeDab(f.words.data(), f.w, f.h, dab, win, after);
    (void)res;
    for (int z = 0; z < win.Height(); ++z) {
      for (int x = 0; x < win.Width(); ++x) {
        f.words[static_cast<std::size_t>(win.z0 + z) * f.w + win.x0 + x] = after[static_cast<std::size_t>(z) * win.Width() + x];
      }
    }
  }

  /** Upstream's step on the same field; seconds / strength make the blend the same as ours (blend = strength * 8 * seconds). */
  void UpDab(Field& f, faf::editor::HeightBrush brush, const float cx, const float cz, const float seconds)
  {
    (void)faf::editor::ApplyHeightBrush(f.words.data(), f.w, f.h, cx, cz, brush, seconds);
  }

  void TestUpstreamExpectations()
  {
    // upstream: Stepped(brush, 1000): strength 1, falloff 0, radius 6, 1 second at (16, 16) of a 33 x 33 field
    faf::editor::HeightBrush up;
    up.mode = faf::editor::HeightBrushMode::Terrace;
    up.terraceStep = 640.0f;
    up.strength = 1.0f;
    up.falloff = 0.0f;
    up.radius = 6.0f;
    Field a(33, 33, 1000);
    UpDab(a, up, 16.0f, 16.0f, 1.0f);
    CHECK(a.At(16, 16) == 1280, "upstream terrace %d", a.At(16, 16));

    // ours: strength 5 pulls the whole way (k = strength * 0.2 * weight = 1)
    br::Dab dab;
    dab.mode = br::Mode::Terrace;
    dab.cx = 16.0f;
    dab.cz = 16.0f;
    dab.radius = 6.0f;
    dab.falloff = 0.0f;
    dab.strength = 5.0f;
    dab.step = 5.0f; // 640 words
    Field b(33, 33, 1000);
    OurDab(b, dab);
    CHECK(b.At(16, 16) == 1280, "our terrace %d", b.At(16, 16));
    CHECK(b.At(2, 2) == 1000, "outside the brush stays (%d)", b.At(2, 2));
    // the whole flat part agrees with upstream
    int differ = 0;
    for (int z = 0; z < 33; ++z) {
      for (int x = 0; x < 33; ++x) {
        const float d = std::hypot(static_cast<float>(x - 16), static_cast<float>(z - 16));
        if (d <= 4.5f && a.At(x, z) != b.At(x, z)) ++differ;
      }
    }
    CHECK(differ == 0, "terrace flat part differs from upstream at %d samples", differ);

    // the plane
    double px[3] = {0.0, 32.0, 0.0};
    double ph[3] = {1000.0, 1640.0, 1320.0};
    double pz[3] = {0.0, 0.0, 32.0};
    faf::editor::HeightBrush upPlane = up;
    upPlane.mode = faf::editor::HeightBrushMode::Plane;
    CHECK(faf::editor::PlaneThrough(px, ph, pz, upPlane.planeX, upPlane.planeZ, upPlane.planeC), "plane through three points");
    upPlane.planeSet = true;
    CHECK(std::fabs(upPlane.planeX - 20.0) < 1e-9 && std::fabs(upPlane.planeZ - 10.0) < 1e-9, "plane slopes %g %g", upPlane.planeX, upPlane.planeZ);
    Field pa(33, 33, 0);
    UpDab(pa, upPlane, 16.0f, 16.0f, 1.0f);
    CHECK(pa.At(16, 16) == 1000 + 20 * 16 + 10 * 16, "upstream plane %d", pa.At(16, 16));
    CHECK(pa.At(18, 14) == 1000 + 20 * 18 + 10 * 14, "upstream plane at (18, 14): %d", pa.At(18, 14));

    br::Dab pd = dab;
    pd.mode = br::Mode::Plane;
    pd.planeSet = true;
    pd.planeX = upPlane.planeX;
    pd.planeZ = upPlane.planeZ;
    pd.planeC = upPlane.planeC;
    Field pb(33, 33, 0);
    OurDab(pb, pd);
    CHECK(pb.At(16, 16) == 1000 + 20 * 16 + 10 * 16, "our plane %d", pb.At(16, 16));
    CHECK(pb.At(18, 14) == 1000 + 20 * 18 + 10 * 14, "our plane at (18, 14): %d", pb.At(18, 14));
    differ = 0;
    for (int z = 0; z < 33; ++z) {
      for (int x = 0; x < 33; ++x) {
        const float d = std::hypot(static_cast<float>(x - 16), static_cast<float>(z - 16));
        if (d <= 4.5f && pa.At(x, z) != pb.At(x, z)) ++differ;
      }
    }
    CHECK(differ == 0, "plane flat part differs from upstream at %d samples", differ);

    double line[3] = {0.0, 1.0, 2.0};
    double a1 = 0, a2 = 0, a3 = 0;
    CHECK(!faf::editor::PlaneThrough(line, ph, line, a1, a2, a3), "three points on a line have no plane");

    // an unset plane and strength 0 change nothing
    Field unset(33, 33, 777);
    br::Dab nd = pd;
    nd.planeSet = false;
    OurDab(unset, nd);
    CHECK(unset.words == Field(33, 33, 777).words, "an unset plane changes nothing");
    Field zero(33, 33, 777);
    br::Dab zd = dab;
    zd.strength = 0.0f;
    OurDab(zero, zd);
    CHECK(zero.words == Field(33, 33, 777).words, "strength 0 changes nothing");
  }

  void TestRandomEquality()
  {
    int compared = 0;
    int worst = 0;
    int exact = 0;
    for (int round = 0; round < 40; ++round) {
      Field start(41, 41, 0);
      for (auto& v : start.words) v = static_cast<std::uint16_t>(500 + (Next() % 6000));
      const bool plane = (round % 2) == 1;
      const float stepUnits = 1.0f + static_cast<float>(Next() % 12);
      const float strength = (round % 4 < 2) ? 5.0f : 1.0f + static_cast<float>(Next() % 30) * 0.1f; // 5 = the whole way; below: a partial pull
      const float cx = 18.0f + static_cast<float>(Next() % 5);
      const float cz = 18.0f + static_cast<float>(Next() % 5);
      const float radius = 8.0f + static_cast<float>(Next() % 5);

      faf::editor::HeightBrush up;
      up.mode = plane ? faf::editor::HeightBrushMode::Plane : faf::editor::HeightBrushMode::Terrace;
      up.terraceStep = stepUnits * 128.0f;
      up.strength = 1.0f; // upstream blend = min(1, strength * 8 * seconds): ours k = min(1, strength * 0.2) at weight 1
      up.falloff = 0.0f;
      up.radius = radius;
      br::Dab dab;
      dab.mode = plane ? br::Mode::Plane : br::Mode::Terrace;
      dab.cx = cx;
      dab.cz = cz;
      dab.radius = radius;
      dab.falloff = 0.0f;
      dab.strength = strength;
      dab.step = stepUnits;
      if (plane) {
        double px[3] = {static_cast<double>(Next() % 40), static_cast<double>(Next() % 40), static_cast<double>(Next() % 40)};
        double pz[3] = {static_cast<double>(Next() % 40), static_cast<double>(Next() % 40), static_cast<double>(Next() % 40)};
        double ph[3] = {1000.0 + Next() % 4000, 1000.0 + Next() % 4000, 1000.0 + Next() % 4000};
        if (!faf::editor::PlaneThrough(px, ph, pz, up.planeX, up.planeZ, up.planeC)) continue;
        up.planeSet = true;
        dab.planeSet = true;
        dab.planeX = up.planeX;
        dab.planeZ = up.planeZ;
        dab.planeC = up.planeC;
      }
      const float k = std::min(1.0f, strength * 0.2f);
      Field a = start;
      Field b = start;
      UpDab(a, up, cx, cz, k / 8.0f); // blend = 1 * 8 * seconds = k
      OurDab(b, dab);
      for (int z = 0; z < 41; ++z) {
        for (int x = 0; x < 41; ++x) {
          const float d = std::hypot(static_cast<float>(x) - cx, static_cast<float>(z) - cz);
          if (d > radius - 1.5f) {
            if (d >= radius + 1.0f) CHECK(b.At(x, z) == start.At(x, z), "outside the brush changed at (%d, %d)", x, z);
            continue;
          }
          const int diff = std::abs(static_cast<int>(a.At(x, z)) - static_cast<int>(b.At(x, z)));
          ++compared;
          if (diff == 0) ++exact;
          if (diff > worst) worst = diff;
          if (k >= 1.0f) CHECK(diff == 0, "full pull differs at (%d, %d): upstream %d ours %d (%s, round %d)", x, z, a.At(x, z), b.At(x, z), plane ? "plane" : "terrace", round);
        }
      }
    }
    CHECK(compared > 5000, "compared %d samples", compared);
    CHECK(worst <= 1, "partial pulls differ by %d words at most (float rounding of the blend)", worst);
    std::printf("random fields: %d samples compared, %d identical, worst %d word(s)\n", compared, exact, worst);
  }

  void TestDocument()
  {
    const int w = 70, h = 60;
    std::vector<std::uint16_t> start(static_cast<std::size_t>(w) * h);
    for (auto& v : start) v = static_cast<std::uint16_t>(2000 + (Next() % 3000));
    MapDocument doc;
    doc.Reset(w, h, start, [](const CellRect&, const std::uint16_t*) {});
    doc.BeginStroke("Terrain terrace and plane");
    std::size_t total = 0;
    for (int i = 0; i < 10; ++i) {
      br::Dab dab;
      dab.cx = 8.0f + i * 5.0f;
      dab.cz = 25.0f + 2.0f * static_cast<float>(i % 3);
      dab.radius = 9.0f;
      dab.strength = 5.0f;
      dab.falloff = 0.7f;
      dab.step = 4.0f;
      if (i % 2) {
        dab.mode = br::Mode::Plane;
        dab.planeSet = true;
        dab.planeX = 3.0;
        dab.planeZ = -2.0;
        dab.planeC = 2500.0;
      } else {
        dab.mode = br::Mode::Terrace;
      }
      const br::Window win = br::WindowOf(dab, doc.Width(), doc.Height());
      CellRect rect;
      rect.x0 = win.x0; rect.z0 = win.z0; rect.x1 = win.x1; rect.z1 = win.z1;
      std::vector<std::uint16_t> before;
      doc.ReadRect(rect, before);
      std::vector<std::uint16_t> after(before.size());
      const br::Result res = br::ApplyWindow(dab, win, before.data(), after.data());
      if (res.changed > 0) total += doc.EditHeights(rect, after.data());
    }
    doc.EndStroke();
    CHECK(total > 200, "the stroke changed %zu samples", total);
    CHECK(doc.UndoDepth() == 1, "one undo step (%zu)", doc.UndoDepth());
    CellRect all;
    all.x0 = 0; all.z0 = 0; all.x1 = w - 1; all.z1 = h - 1;
    std::vector<std::uint16_t> read;
    doc.ReadRect(all, read);
    CHECK(read != start, "the stroke changed the field");
    const moho::editor::HistoryResult u = doc.Undo();
    CHECK(u.ok && u.verified, "undo ok and verified");
    doc.ReadRect(all, read);
    CHECK(read == start, "undo restores every word");
  }
} // namespace

int main()
{
  TestUpstreamExpectations();
  TestRandomEquality();
  TestDocument();
  std::printf("EdBrushUpTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
