// Stand-alone test of the Maui clipping math (EdUiClip.h, no engine needed):
//   g++ -std=c++20 -O2 -I src engine/faf-re/src/moho/editor/tests/EdUiClipTest.cpp -o EdUiClipTest
#include <cmath>
#include <cstdio>

#include "moho/editor/EdUiClip.h"

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

namespace uc = moho::editor::uiclip;

struct V
{
  float mX = 0.0f;
  float mY = 0.0f;
  float mU = 0.0f;
  float mV = 0.0f;
};

static V Lerp(const V& a, const V& b, const float t)
{
  return {a.mX + (b.mX - a.mX) * t, a.mY + (b.mY - a.mY) * t, a.mU + (b.mU - a.mU) * t, a.mV + (b.mV - a.mV) * t};
}

static bool Near(const float a, const float b)
{
  return std::fabs(a - b) < 1e-4f;
}

int main()
{
  const uc::Rect clip{10.0f, 20.0f, 110.0f, 70.0f};

  // rectangles
  CHECK(!uc::Empty(clip), "clip is not empty");
  CHECK(uc::Empty({5.0f, 5.0f, 5.0f, 9.0f}) && uc::Empty({5.0f, 9.0f, 8.0f, 9.0f}) && uc::Empty({8.0f, 8.0f, 2.0f, 9.0f}), "no area is empty");
  const uc::Rect cut = uc::Intersect(clip, {50.0f, 0.0f, 200.0f, 40.0f});
  CHECK(Near(cut.left, 50.0f) && Near(cut.top, 20.0f) && Near(cut.right, 110.0f) && Near(cut.bottom, 40.0f), "intersect");
  CHECK(uc::Empty(uc::Intersect(clip, {200.0f, 0.0f, 300.0f, 40.0f})), "disjoint rectangles have an empty intersection");
  CHECK(uc::Contains(clip, 10.0f, 20.0f) && !uc::Contains(clip, 110.0f, 20.0f) && !uc::Contains(clip, 9.9f, 30.0f), "contains: left / top edge in, right edge out");
  CHECK(uc::Inside(clip, 10.0f, 20.0f, 110.0f, 70.0f) && !uc::Inside(clip, 9.0f, 20.0f, 110.0f, 70.0f), "inside");
  CHECK(uc::Outside(clip, 110.0f, 0.0f, 150.0f, 100.0f) && uc::Outside(clip, 0.0f, 0.0f, 10.0f, 100.0f) && !uc::Outside(clip, 0.0f, 0.0f, 10.5f, 100.0f), "outside: touching is outside");

  // a textured quad cut on the right and at the bottom: positions and texture coordinates stay in step
  {
    const V quad[4] = {{50.0f, 40.0f, 0.0f, 0.0f}, {150.0f, 40.0f, 1.0f, 0.0f}, {150.0f, 100.0f, 1.0f, 1.0f}, {50.0f, 100.0f, 0.0f, 1.0f}};
    V out[8];
    const int n = uc::ClipPolygon(quad, 4, clip, out, Lerp);
    CHECK(n == 4, "a quad cut by two edges stays a quad (%d)", n);
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, maxU = -1e9f, maxV = -1e9f;
    for (int i = 0; i < n; ++i) {
      minX = std::fmin(minX, out[i].mX);
      maxX = std::fmax(maxX, out[i].mX);
      minY = std::fmin(minY, out[i].mY);
      maxY = std::fmax(maxY, out[i].mY);
      maxU = std::fmax(maxU, out[i].mU);
      maxV = std::fmax(maxV, out[i].mV);
    }
    CHECK(Near(minX, 50.0f) && Near(maxX, 110.0f) && Near(minY, 40.0f) && Near(maxY, 70.0f), "the clipped box is the intersection (%g %g %g %g)", minX, maxX, minY, maxY);
    CHECK(Near(maxU, 0.6f) && Near(maxV, 0.5f), "the texture coordinates follow the cut (%g %g)", maxU, maxV);
  }

  // a quad that covers the clip rectangle completely gives the clip rectangle
  {
    const V quad[4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {200.0f, 0.0f, 1.0f, 0.0f}, {200.0f, 200.0f, 1.0f, 1.0f}, {0.0f, 200.0f, 0.0f, 1.0f}};
    V out[8];
    const int n = uc::ClipPolygon(quad, 4, clip, out, Lerp);
    CHECK(n == 4, "a covering quad becomes the clip rectangle (%d)", n);
    float area = 0.0f;
    for (int i = 0; i < n; ++i) {
      const V& a = out[i];
      const V& b = out[(i + 1) % n];
      area += a.mX * b.mY - b.mX * a.mY;
    }
    CHECK(Near(std::fabs(area) * 0.5f, 100.0f * 50.0f), "area %g", std::fabs(area) * 0.5f);
  }

  // outside and degenerate
  {
    const V away[4] = {{200.0f, 0.0f, 0, 0}, {300.0f, 0.0f, 0, 0}, {300.0f, 50.0f, 0, 0}, {200.0f, 50.0f, 0, 0}};
    V out[8];
    CHECK(uc::ClipPolygon(away, 4, clip, out, Lerp) == 0, "a quad outside gives nothing");
    const V line[4] = {{0.0f, 30.0f, 0, 0}, {10.0f, 30.0f, 0, 0}, {10.0f, 60.0f, 0, 0}, {0.0f, 60.0f, 0, 0}};
    CHECK(uc::ClipPolygon(line, 4, clip, out, Lerp) == 0, "a quad touching the clip edge has no area inside");
    CHECK(uc::ClipPolygon(away, 2, clip, out, Lerp) == 0, "fewer than 3 vertices");
  }

  // a rotated quad (a diamond) cut at a corner: a polygon with more corners, inside the clip
  {
    const V diamond[4] = {{60.0f, -20.0f, 0, 0}, {120.0f, 45.0f, 0, 0}, {60.0f, 110.0f, 0, 0}, {0.0f, 45.0f, 0, 0}};
    V out[8];
    const int n = uc::ClipPolygon(diamond, 4, clip, out, Lerp);
    CHECK(n >= 5 && n <= 8, "a diamond cut on all sides has more corners (%d)", n);
    bool allInside = true;
    for (int i = 0; i < n; ++i) {
      if (out[i].mX < clip.left - 1e-3f || out[i].mX > clip.right + 1e-3f || out[i].mY < clip.top - 1e-3f || out[i].mY > clip.bottom + 1e-3f) {
        allInside = false;
      }
    }
    CHECK(allInside, "every corner is inside the clip rectangle");
  }

  std::printf("EdUiClipTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
