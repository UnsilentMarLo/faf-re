// Stand-alone test of the render-side optimisation logic (no engine needed):
//   EdShadowCache.h   the hash, the cache decision, the depth technique classes
//   EdUiCull.h        what counts as completely outside the window, which text runs can go
//   EdActivity.h      the change tracker, the tick arithmetic, the pacing proposal
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdRenderOptTest.cpp -o EdRenderOptTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdRenderOptTest.cpp
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <set>
#include <vector>

#include "moho/editor/EdActivity.h"
#include "moho/editor/EdShadowCache.h"
#include "moho/editor/EdUiCull.h"

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

namespace sc = moho::editor::shadowcache;
namespace uc = moho::editor::uicull;
namespace ac = moho::editor::activity;

// ---------------------------------------------------------------- shadow cache

static sc::Key MakeKey(const std::uint64_t scene)
{
  sc::Key k;
  k.scene = scene;
  for (int i = 0; i < 16; ++i) {
    k.lightViewProjection[i] = (i % 5 == 0) ? 1.0f : 0.25f * static_cast<float>(i);
  }
  k.size = 2048;
  k.fidelity = 3;
  k.blur = true;
  static int a, b, c, d;
  k.owner = &a;
  k.targets[0] = &b;
  k.targets[1] = &c;
  k.targets[2] = &d;
  return k;
}

static void TestHasher()
{
  sc::Hasher h1, h2;
  h1.AddFloat(1.0f);
  h1.Add(7);
  h2.AddFloat(1.0f);
  h2.Add(7);
  CHECK(h1.Value() == h2.Value(), "same input, same hash");

  // one changed float in a block of 200 changes the hash (also the sign of zero: more misses are safe, false hits are not)
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
  std::vector<float> data(200);
  for (float& f : data) {
    f = dist(rng);
  }
  const auto hashOf = [&](const std::vector<float>& v) {
    sc::Hasher h;
    h.AddBytes(v.data(), v.size() * sizeof(float));
    return h.Value();
  };
  const std::uint64_t base = hashOf(data);
  int same = 0;
  for (std::size_t i = 0; i < data.size(); ++i) {
    std::vector<float> changed = data;
    changed[i] = std::nextafter(changed[i], 1000.0f);   // one ulp
    if (hashOf(changed) == base) {
      ++same;
    }
  }
  CHECK(same == 0, "a one ulp change is always seen (%d of 200 not)", same);

  // 100000 random vectors: no collision among 64 bit values
  std::set<std::uint64_t> seen;
  int collisions = 0;
  for (int i = 0; i < 100000; ++i) {
    sc::Hasher h;
    h.Add(static_cast<std::uint32_t>(rng()));
    h.Add(static_cast<std::uint32_t>(rng()));
    h.Add(static_cast<std::uint32_t>(i));
    if (!seen.insert(h.Value()).second) {
      ++collisions;
    }
  }
  CHECK(collisions == 0, "no collisions in 100000 hashes (%d)", collisions);

  // byte tails: lengths 1, 2, 3 differ from each other and from the shorter prefix
  const unsigned char bytes[4] = {1, 2, 3, 4};
  std::set<std::uint64_t> tails;
  for (std::size_t n = 0; n <= 4; ++n) {
    sc::Hasher h;
    h.AddBytes(bytes, n);
    tails.insert(h.Value());
  }
  CHECK(tails.size() == 5, "byte lengths 0..4 give 5 different hashes (%zu)", tails.size());

  // order independence is the caller's: a sum of per instance hashes
  std::uint64_t sumA = 0, sumB = 0;
  std::vector<std::uint64_t> parts;
  for (int i = 0; i < 50; ++i) {
    sc::Hasher h;
    h.Add(static_cast<std::uint32_t>(i * 77));
    parts.push_back(h.Value());
  }
  for (std::size_t i = 0; i < parts.size(); ++i) {
    sumA += parts[i];
    sumB += parts[parts.size() - 1 - i];
  }
  CHECK(sumA == sumB, "the sum does not depend on the order");
}

static void TestDecide()
{
  sc::State st;
  const sc::Key k = MakeKey(42);

  CHECK(sc::Decide(st, k, true, false, 300) == sc::Decision::Render, "an empty cache renders");
  sc::Store(st, k, 0);
  CHECK(sc::Decide(st, k, true, false, 300) == sc::Decision::Reuse, "same key reuses");
  CHECK(st.hits == 1 && st.misses == 1, "counters %u/%u", st.hits, st.misses);

  // disabled: always renders, and the held map stays
  CHECK(sc::Decide(st, k, false, false, 300) == sc::Decision::Render, "cvar off renders");

  // every part of the key matters
  {
    sc::Key o = k;
    o.scene = 43;
    CHECK(sc::Decide(st, o, true, false, 300) == sc::Decision::Render, "scene");
    o = k;
    o.lightViewProjection[7] += 1e-6f;
    CHECK(sc::Decide(st, o, true, false, 300) == sc::Decision::Render, "light matrix");
    o = k;
    o.size = 1024;
    CHECK(sc::Decide(st, o, true, false, 300) == sc::Decision::Render, "size");
    o = k;
    o.fidelity = 2;
    CHECK(sc::Decide(st, o, true, false, 300) == sc::Decision::Render, "fidelity");
    o = k;
    o.blur = false;
    CHECK(sc::Decide(st, o, true, false, 300) == sc::Decision::Render, "blur");
    static int other;
    o = k;
    o.owner = &other;
    CHECK(sc::Decide(st, o, true, false, 300) == sc::Decision::Render, "owner");
    for (int t = 0; t < 3; ++t) {
      o = k;
      o.targets[t] = &other;
      CHECK(sc::Decide(st, o, true, false, 300) == sc::Decision::Render, "target %d", t);
    }
  }
  CHECK(sc::Decide(st, k, true, false, 300) == sc::Decision::Reuse, "the held key is still reusable after misses on other keys");

  // invalidate
  sc::Invalidate(st);
  CHECK(sc::Decide(st, k, true, false, 300) == sc::Decision::Render, "invalidated renders");
  sc::Store(st, k, 0);

  // maximum age: reuse for maxAge frames, then one render
  sc::State aged;
  sc::Store(aged, k, 0);
  int reuses = 0;
  for (int i = 0; i < 10; ++i) {
    if (sc::Decide(aged, k, true, false, 5) == sc::Decision::Reuse) {
      ++reuses;
    } else {
      break;
    }
  }
  CHECK(reuses == 5, "reused for exactly maxAge frames (%d)", reuses);
  CHECK(sc::Decide(aged, k, true, false, 5) == sc::Decision::Render, "then drawn again");
  sc::Store(aged, k, 0);
  CHECK(sc::Decide(aged, k, true, false, 5) == sc::Decision::Reuse, "and held again after the store");
  sc::State unlimited;
  sc::Store(unlimited, k, 0);
  bool allReuse = true;
  for (int i = 0; i < 100000; ++i) {
    allReuse = allReuse && sc::Decide(unlimited, k, true, false, 0) == sc::Decision::Reuse;
  }
  CHECK(allReuse, "maxAge 0 never expires");

  // time dependent maps
  sc::State wind;
  sc::Store(wind, k, sc::kTimeWind);
  CHECK(sc::Decide(wind, k, true, false, 300) == sc::Decision::Render, "a map with swaying trees is drawn every frame");
  CHECK(sc::Decide(wind, k, true, true, 300) == sc::Decision::Reuse, "unless ren_ShadowCacheTrees says the sway may be held");
  sc::State other;
  sc::Store(other, k, sc::kTimeOther);
  CHECK(sc::Decide(other, k, true, true, 300) == sc::Decision::Render, "an unknown technique is never held");
  sc::Store(other, k, static_cast<std::uint8_t>(sc::kTimeWind | sc::kTimeOther));
  CHECK(sc::Decide(other, k, true, true, 300) == sc::Decision::Render, "wind and unknown: never held");

  // classes of the techniques of mesh.fx
  CHECK(sc::ClassifyDepthTechnique("Depth") == 0, "Depth");
  CHECK(sc::ClassifyDepthTechnique("DepthClip") == 0, "DepthClip");
  CHECK(sc::ClassifyDepthTechnique("SeraphimBuildDepth") == 0, "SeraphimBuildDepth");
  CHECK(sc::ClassifyDepthTechnique("UndulatingDepthClip") == sc::kTimeWind, "UndulatingDepthClip");
  CHECK(sc::ClassifyDepthTechnique("depth") == sc::kTimeOther, "names are case sensitive, an unknown one is conservative");
  CHECK(sc::ClassifyDepthTechnique("") == sc::kTimeOther, "empty name");
  CHECK(sc::ClassifyDepthTechnique(nullptr) == sc::kTimeOther, "null name");
}

// A model of the pass over several frames: what the render target holds, what the scene was when it was drawn.
// The cache must never show a map that differs from what a fresh draw of the current scene would give.
static void TestStaleness()
{
  struct Scene
  {
    std::uint64_t id;       // content of the scene
    int camera;             // light camera
    bool wind;              // has swaying trees
    int clock;              // the time the wind depends on
  };
  const auto render = [](const Scene& s) {
    return std::uint64_t(s.id) * 1000003u + std::uint64_t(s.camera) * 7919u + (s.wind ? std::uint64_t(s.clock) * 104729u : 0u);
  };

  std::mt19937 rng(99);
  sc::State st;
  std::uint64_t targetContent = 0;
  Scene scene{1, 1, false, 0};
  long drawn = 0, reused = 0;
  for (int frame = 0; frame < 20000; ++frame) {
    // random events: the scene changes now and then, the camera moves now and then, the trees appear / disappear
    if (rng() % 50 == 0) scene.id = rng() % 5;
    if (rng() % 70 == 0) scene.camera = static_cast<int>(rng() % 4);
    if (rng() % 400 == 0) scene.wind = !scene.wind;
    scene.clock = frame;

    // the trees are instances of the scene: they change its signature when they appear or go
    sc::Key key = MakeKey(scene.id * 31 + 7 + (scene.wind ? 1000u : 0u));
    key.lightViewProjection[3] = static_cast<float>(scene.camera);
    if (sc::Decide(st, key, true, false, 300) == sc::Decision::Reuse) {
      ++reused;
    } else {
      targetContent = render(scene);
      sc::Store(st, key, scene.wind ? sc::kTimeWind : static_cast<std::uint8_t>(0));
      ++drawn;
    }
    if (targetContent != render(scene)) {
      CHECK(false, "stale map at frame %d", frame);
      return;
    }
  }
  CHECK(reused > 5000 && drawn > 100, "the model both reuses (%ld) and draws (%ld)", reused, drawn);
}

// ---------------------------------------------------------------- ui culling

static void TestUiCull()
{
  const float W = 1920.0f, H = 1080.0f;
  CHECK(!uc::QuadOutside(0, 0, 100, 100, W, H), "inside");
  CHECK(!uc::QuadOutside(-50, -50, 50, 50, W, H), "overlapping the corner");
  CHECK(!uc::QuadOutside(W - 1, H - 1, W + 100, H + 100, W, H), "touching the bottom right");
  CHECK(!uc::QuadOutside(-10, 10, 0, 20, W, H), "right edge exactly on 0 is not outside (a zero width rectangle draws nothing anyway)");
  CHECK(uc::QuadOutside(-100, 10, -1, 20, W, H), "left of the window");
  CHECK(uc::QuadOutside(W + 1, 10, W + 50, 20, W, H), "right of the window");
  CHECK(uc::QuadOutside(10, -100, 50, -0.5f, W, H), "above");
  CHECK(uc::QuadOutside(10, H + 1, 50, H + 40, W, H), "below");
  CHECK(uc::QuadOutside(-20000, -20000, -19900, -19976, W, H), "a parked tab holder child");
  CHECK(uc::QuadOutside(-4000, 100, -3850, 124, W, H), "a parked row at x = -4000");
  CHECK(!uc::QuadOutside(0, 0, W, H, W, H), "the whole window");
  CHECK(!uc::QuadOutside(-1e9f, -1e9f, 1e9f, 1e9f, W, H), "a huge rectangle covering the window");
  CHECK(!uc::QuadOutside(-1e9f, 0, -1e8f + 2e9f, 10, W, H), "wide");
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK(!uc::QuadOutside(nan, 0, 10, 10, W, H), "NaN is never culled");
  CHECK(!uc::QuadOutside(0, 0, 10, nan, W, H), "NaN is never culled (2)");
  CHECK(!uc::QuadOutside(-1000, 0, -900, 10, 0.0f, H), "an unknown viewport culls nothing");
  CHECK(!uc::QuadOutside(-1000, 0, -900, 10, W, -1.0f), "an unknown viewport culls nothing (2)");

  // exhaustive property: a rectangle that is "outside" has no point inside the window (checked on a grid of rectangles)
  int wrongCulls = 0;
  for (int l = -300; l <= 300; l += 25) {
    for (int t = -300; t <= 300; t += 25) {
      for (int w = 0; w <= 200; w += 20) {
        for (int h = 0; h <= 200; h += 20) {
          const float fl = static_cast<float>(l), ft = static_cast<float>(t), fr = fl + static_cast<float>(w), fb = ft + static_cast<float>(h);
          const bool culled = uc::QuadOutside(fl, ft, fr, fb, 200.0f, 150.0f);
          // does the rectangle share a point with [0,200] x [0,150]?
          const bool intersects = !(fr < 0.0f || fb < 0.0f || fl > 200.0f || ft > 150.0f);
          if (culled && intersects) {
            ++wrongCulls;
          }
          if (!culled && !intersects) {
            ++wrongCulls;
          }
        }
      }
    }
  }
  CHECK(wrongCulls == 0, "QuadOutside agrees with rectangle intersection (%d)", wrongCulls);

  // text
  const float pad = 256.0f;
  CHECK(!uc::TextOutside(10, 20, -1.0f, W, H, pad), "a label in the window");
  CHECK(uc::TextOutside(W + 300, 20, -1.0f, W, H, pad), "starts right of the window");
  CHECK(!uc::TextOutside(W + 100, 20, -1.0f, W, H, pad), "starts just right of the window: inside the margin");
  CHECK(uc::TextOutside(10, -20000, -1.0f, W, H, pad), "parked high");
  CHECK(uc::TextOutside(10, H + 5000, -1.0f, W, H, pad), "parked low");
  CHECK(!uc::TextOutside(10, -100, -1.0f, W, H, pad), "baseline a little above: the glyphs hang into the window");
  CHECK(uc::TextOutside(-4000, 40, 300.0f, W, H, pad), "parked left: ends at -3700");
  CHECK(!uc::TextOutside(-100, 40, 300.0f, W, H, pad), "starts left, ends inside");
  CHECK(!uc::TextOutside(-4000, 40, 5000.0f, W, H, pad), "a very long string that reaches into the window");
  CHECK(!uc::TextOutside(-4000, 40, -1.0f, W, H, pad), "width unknown: the horizontal left test is not applied");
  CHECK(!uc::TextOutside(-4000, 40, 300.0f, 0.0f, 0.0f, pad), "unknown viewport");

  CHECK(uc::ColorTransparent(0x00FFFFFFu), "alpha 0");
  CHECK(!uc::ColorTransparent(0x01000000u), "alpha 1");
  CHECK(!uc::ColorTransparent(0xFFFFFFFFu), "opaque");
}

// ---------------------------------------------------------------- activity

static void TestActivity()
{
  ac::Tracker t;
  CHECK(t.Epoch() == 0 && t.Take(false) == 0, "starts clean");
  t.Touch(0);
  CHECK(t.Epoch() == 0, "touching nothing changes nothing");
  t.Touch(ac::kCamera);
  t.Touch(ac::kOverlay);
  CHECK(t.Epoch() == 2, "two touches, two epochs");
  CHECK(t.Take(false) == (ac::kCamera | ac::kOverlay), "reasons add up");
  CHECK(t.Take(true) == (ac::kCamera | ac::kOverlay), "consuming returns them");
  CHECK(t.Take(true) == 0, "and clears them");
  CHECK(t.Epoch() == 2, "the epoch stays");

  CHECK(ac::ElapsedMs(1000u, 400u) == 600u, "plain");
  CHECK(ac::ElapsedMs(50u, 0xFFFFFFF0u) == 66u, "across the wrap of GetTickCount");
  CHECK(ac::ElapsedMs(7u, 7u) == 0u, "zero");

  const std::uint32_t hold = 2000u;
  CHECK(ac::RecommendedWaitMs(100, 100, 0, false, hold) == 0u, "just used: full rate");
  CHECK(ac::RecommendedWaitMs(5000, 100, 0, false, hold) == 0u, "no input but the scene changed a moment ago: full rate");
  CHECK(ac::RecommendedWaitMs(100, 5000, 0, false, hold) == 0u, "input a moment ago: full rate");
  CHECK(ac::RecommendedWaitMs(5000, 5000, 0, true, hold) == 0u, "a change is pending: full rate");
  CHECK(ac::RecommendedWaitMs(5000, 5000, ac::kAmbientSim, false, hold) == 0u, "a running simulation: full rate");
  CHECK(ac::RecommendedWaitMs(5000, 5000, 0, false, hold) == 100u, "still, nothing moves: 10 Hz");
  CHECK(ac::RecommendedWaitMs(5000, 5000, ac::kAmbientWater, false, hold) == 33u, "still with water: 30 Hz");
  CHECK(ac::RecommendedWaitMs(5000, 5000, ac::kAmbientWater | ac::kAmbientSky, false, hold) == 33u, "water and sky: 30 Hz");
  CHECK(ac::RecommendedWaitMs(2000, 2000, 0, false, hold) == 100u, "exactly at the hold time");
  CHECK(ac::RecommendedWaitMs(1999, 5000, 0, false, hold) == 0u, "one ms short");
}

int main()
{
  TestHasher();
  TestDecide();
  TestStaleness();
  TestUiCull();
  TestActivity();
  std::printf("EdRenderOptTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
