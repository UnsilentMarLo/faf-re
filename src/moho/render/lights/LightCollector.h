#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "moho/render/lights/LightModel.h"

// The light pool: feeds offer Emissions (a thing that glows, with its birth time, life and motion), Update(now) turns the live ones into
// the Lights of this frame (position, radius, colour x intensity), ranked by power and capped.
//
// Pure data and arithmetic, no engine headers and no GPU: compiled on its own by the host test (tests/lights). Render only; nothing
// here reads or writes sim state. Single-threaded: the feeds and Update run on the thread that renders.
//
// Times are sim ticks as floats (tick + sub-tick fraction); the caller supplies the same clock the particle shaders use
// (REN_GetGameTick() + frame alpha).
namespace moho::lights
{
  enum class Kind : std::uint8_t
  {
    Flash = 0, // CreateLightParticle ("TLight" particle)
    Glow,      // additive glow sprite (later stage)
    Beam,      // later stage
    Trail,     // later stage
    Mesh,      // projectile or unit mesh (later stage)
    Count
  };

  // One glowing thing, as a feed measured it. Moves as pos + dir*t + 0.5*accel*t^2 with t in ticks since birth (the particle shader's rule).
  struct Emission
  {
    Kind kind = Kind::Flash;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float dir[3] = {0.0f, 0.0f, 0.0f};
    float accel[3] = {0.0f, 0.0f, 0.0f};
    float birth = 0.0f; // tick (with fraction)
    float life = 0.0f;  // ticks
    float size0 = 0.0f; // sprite size at birth and at the end of life, world units
    float size1 = 0.0f;
    float rgb[3] = {1.0f, 1.0f, 1.0f}; // 0..1
  };

  // One light for this frame.
  struct Light
  {
    float pos[3];
    float radius;
    float rgb[3]; // colour x intensity, linear
    Kind kind;
    float power; // intensity x radius^2 at this moment (the ranking key)
  };

  struct Stats
  {
    std::uint32_t offered = 0;  // Add() calls
    std::uint32_t invalid = 0;  // non-finite or absurd values
    std::uint32_t gated = 0;    // below the quality level's importance class
    std::uint32_t poolFull = 0; // no room even after pruning
    std::uint32_t stored = 0;   // accepted
    std::uint32_t expired = 0;  // pruned after their fade-out
    std::uint32_t active = 0;   // lights evaluated by the last Update (before the cap)
    std::uint32_t culledByCap = 0; // lights of the last Update that did not fit the cap
  };

  class LightCollector
  {
  public:
    static constexpr std::size_t kPoolCapacity = 4096;

    // Quality: model::kOff..model::kUltra. Lowering it also drops stored lights the new level no longer admits.
    void SetQuality(int quality);
    [[nodiscard]] int Quality() const { return quality_; }

    // Seconds per tick at the current game speed (0.1 at speed 1); sets the envelope length in ticks.
    void SetSecondsPerTick(float secondsPerTick);

    // Offers one emission. Returns true when it was stored.
    bool Add(const Emission& emission);

    // Evaluates the stored lights at `nowTick`, prunes the expired ones, and fills `out` with at most `maxLights`, strongest first
    // (ties broken by birth time then by order of arrival, so the result does not depend on the sort implementation).
    void Update(float nowTick, std::size_t maxLights, std::vector<Light>& out);

    void Clear();

    [[nodiscard]] std::size_t Stored() const { return pool_.size(); }
    [[nodiscard]] const Stats& GetStats() const { return stats_; }

  private:
    struct Entry
    {
      Emission e;
      float peakPower; // importance, from the peak size: fixed for the entry's life
      std::uint32_t serial;
    };

    [[nodiscard]] float FadeInTicks() const;
    [[nodiscard]] float FadeOutTicks() const;
    void PruneExpired(float nowTick);

    std::vector<Entry> pool_;
    Stats stats_;
    int quality_ = model::kOff;
    float secondsPerTick_ = 0.1f;
    std::uint32_t nextSerial_ = 0;
    float lastNow_ = 0.0f;
  };
} // namespace moho::lights
