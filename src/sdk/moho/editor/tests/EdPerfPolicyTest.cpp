// Stand-alone test of the frame pacing policy of the editor's main loop (EdPerfPolicy.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdPerfPolicyTest.cpp -o EdPerfPolicyTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdPerfPolicyTest.cpp
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "moho/editor/EdPerfPolicy.h"

using namespace moho::editor::perf;

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

static constexpr std::int64_t kMs = 1000000;

static void TestCaps()
{
  Caps caps;     // defaults: active 300, loading 60, background 60, idle off
  Situation s;

  CHECK(EffectiveCap(caps, s) == 300.0f, "active default cap is 300, got %f", EffectiveCap(caps, s));

  s.loading = true;
  CHECK(EffectiveCap(caps, s) == 60.0f, "loading caps to 60");
  s.loading = false;

  s.foreground = false;
  CHECK(EffectiveCap(caps, s) == 60.0f, "background caps to 60");
  s.foreground = true;

  // idle is off by default, however long the user is away
  s.idleSeconds = 3600.0;
  CHECK(EffectiveCap(caps, s) == 300.0f, "idle cap is off by default");

  caps.idle = 30.0f;
  s.idleSeconds = 9.9;
  CHECK(EffectiveCap(caps, s) == 300.0f, "not idle long enough");
  s.idleSeconds = 10.0;
  CHECK(EffectiveCap(caps, s) == 30.0f, "idle cap applies after the delay");

  // the smallest cap wins, in any combination
  s.foreground = false;
  s.loading = true;
  CHECK(EffectiveCap(caps, s) == 30.0f, "smallest of idle 30 / background 60 / loading 60");
  caps.idle = 120.0f;
  CHECK(EffectiveCap(caps, s) == 60.0f, "smallest of idle 120 / background 60 / loading 60");

  // 0 switches a cap off
  caps = Caps{};
  caps.active = 0.0f;
  s = Situation{};
  CHECK(EffectiveCap(caps, s) == 0.0f, "active cap off: uncapped while active");
  s.foreground = false;
  CHECK(EffectiveCap(caps, s) == 60.0f, "the background cap still applies when the active cap is off");
  caps.background = 0.0f;
  CHECK(EffectiveCap(caps, s) == 0.0f, "every cap off: uncapped");

  // minimized: never capped here (the loop's wake-up timer paces it)
  caps = Caps{};
  s = Situation{};
  s.minimized = true;
  s.foreground = false;
  s.loading = true;
  CHECK(EffectiveCap(caps, s) == 0.0f, "minimized has no cap");

  // a silly cap is raised to the minimum, never a frozen editor
  caps = Caps{};
  caps.active = 1.0f;
  s = Situation{};
  CHECK(EffectiveCap(caps, s) == kMinCap, "cap below the minimum is raised to %f", kMinCap);
}

static void TestStateName()
{
  Situation s;
  CHECK(std::string(StateName(s)) == "active", "active");
  s.foreground = false;
  CHECK(std::string(StateName(s)) == "background", "background");
  s.loading = true;
  CHECK(std::string(StateName(s)) == "loading", "loading wins over background");
  s.minimized = true;
  CHECK(std::string(StateName(s)) == "minimized", "minimized wins");
}

// A model of the loop: each frame takes `work` of CPU, then waits for the pacer.
static std::vector<std::int64_t> Simulate(const float cap, const std::int64_t work, const int frames, const std::int64_t stallAt = -1, const std::int64_t stall = 0)
{
  Pacer pacer;
  std::int64_t now = 1000 * kMs;
  std::vector<std::int64_t> starts;
  for (int i = 0; i < frames; ++i) {
    now += pacer.Remaining(now, cap);          // the wait
    pacer.FrameStarted(now, cap);
    starts.push_back(now);
    now += work + (i == stallAt ? stall : 0);   // the frame
  }
  return starts;
}

static void TestPacer()
{
  // a fast frame (1 ms) at a 100 fps cap: one frame every 10 ms exactly, no drift over 1000 frames
  {
    const auto s = Simulate(100.0f, 1 * kMs, 1000);
    for (std::size_t i = 1; i < s.size(); ++i) {
      CHECK(s[i] - s[i - 1] == 10 * kMs, "frame %zu: interval %lld ns", i, static_cast<long long>(s[i] - s[i - 1]));
    }
    CHECK(s.back() - s.front() == 999 * 10 * kMs, "no drift");
  }
  // a frame slower than the period is never delayed (the limiter adds no latency to a frame that already takes long)
  {
    const auto s = Simulate(100.0f, 25 * kMs, 50);
    for (std::size_t i = 1; i < s.size(); ++i) {
      CHECK(s[i] - s[i - 1] == 25 * kMs, "slow frame %zu: interval %lld ns", i, static_cast<long long>(s[i] - s[i - 1]));
    }
  }
  // a stall (a 500 ms hitch) is not made up for with a burst of frames
  {
    const auto s = Simulate(100.0f, 1 * kMs, 40, 10, 500 * kMs);
    int burst = 0;
    for (std::size_t i = 12; i < s.size(); ++i) {
      if (s[i] - s[i - 1] < 10 * kMs) {
        ++burst;
      }
    }
    CHECK(burst == 0, "no catch-up burst after a stall, %d short intervals", burst);
    CHECK(s[12] - s[11] == 10 * kMs, "cadence resumes at once after the stall");
  }
  // cap 0 never waits
  {
    Pacer p;
    p.FrameStarted(5 * kMs, 0.0f);
    CHECK(p.Remaining(5 * kMs, 0.0f) == 0, "cap 0: no wait");
  }
  // the first frame never waits, whatever the cap
  {
    Pacer p;
    CHECK(p.Remaining(123, 30.0f) == 0, "first frame: no wait");
  }
  // an interrupted wait (input arrived) starts a frame early; the grid restarts from there, it does not stack waits
  {
    Pacer p;
    p.FrameStarted(0, 60.0f);
    const std::int64_t early = 4 * kMs;
    CHECK(p.Remaining(early, 60.0f) > 0, "still waiting after 4 ms");
    p.FrameStarted(early, 60.0f);   // ran early
    CHECK(p.Remaining(early, 60.0f) == Pacer::Period(60.0f), "next frame one period after the early one");
  }
  // switching the cap on the fly (focus lost): the new period applies to the next frame
  {
    Pacer p;
    p.FrameStarted(0, 300.0f);
    p.FrameStarted(Pacer::Period(300.0f), 300.0f);
    const std::int64_t t = Pacer::Period(300.0f);
    CHECK(p.Remaining(t + kMs, 60.0f) == Pacer::Period(60.0f) - kMs, "new period from the last start");
  }
  // period arithmetic
  CHECK(Pacer::Period(100.0f) == 10 * kMs, "period of 100 fps");
  CHECK(Pacer::Period(60.0f) == 16666666, "period of 60 fps, %lld", static_cast<long long>(Pacer::Period(60.0f)));
  CHECK(Pacer::Period(1.0f) == Pacer::Period(kMinCap), "period is clamped to the minimum cap");
}

int main()
{
  TestCaps();
  TestStateName();
  TestPacer();
  std::printf("EdPerfPolicyTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
