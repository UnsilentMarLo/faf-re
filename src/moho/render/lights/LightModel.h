#pragma once

// The light model: the one place that decides how big, how strong and how important a light is.
//
// Dynamic lights are INFERRED from what the engine is about to draw (explosion light particles, glow sprites, beams, trails, projectile
// meshes, emissive unit parts): nothing in unit or effect data carries light data. Each feed measures the visual (world size, peak
// brightness, colour) and asks this header for the light's radius, intensity and importance class. The constants live here so a modded
// effect gets the same treatment as a stock one and the quality levels of the options menu act on one number.
//
// Pure functions, no engine headers: the host test (tests/lights) compiles this header and LightCollector.cpp on their own.
// Ported from the FA-DynamicLights patch (src/section/LightModel.h); the hue-transfer constants of that file belong to the merging
// stage and are not here yet.
//
// Units: lengths in world units, times in sim ticks (a tick is 0.1 s at game speed 1), intensity unitless (1.0 = a fully lit surface at
// the light's centre).

namespace moho::lights::model
{
  // ------------------------------------------------------------------------------------------------ importance
  // Importance of a light = its POWER, intensity x radius^2 (how much light it puts into the world), taken at its PEAK (before the fade
  // envelope and before any level of detail), so a nuke flash stays "huge" while it fades and a light never changes class while it lives.
  // Calibration against FA's own CreateLightParticle calls (size is the sprite size in world units):
  //     nuke / Seraphim experimental flashes   size 60..300     power 26000 .. 275000     huge
  //     T3 / experimental explosions, jammers  size 10..40      power   700 ..  12000     large
  //     T1/T2 explosions, impacts, beams       size 3..8        power    26 ..    480     normal
  //     muzzle sparks, tracer trails, faint    size < 3         power     0 ..      6     minor
  enum Class
  {
    kMinor = 0,
    kNormal = 1,
    kLarge = 2,
    kHuge = 3,
    kClassCount = 4
  };
  constexpr float kPowerNormal = 6.0f;
  constexpr float kPowerLarge = 500.0f;
  constexpr float kPowerHuge = 8000.0f;

  // Quality levels (Options > Video > Dynamic Lights, cvar ren_DynLights).
  //   Off     no lights
  //   Low     only the very big ones: nukes and the biggest explosions
  //   Medium  also the smaller big ones (T3 / experimental artillery, large explosions)
  //   High    most lights: everything but tiny sparks and faint glows
  //   Ultra   all of them
  enum Quality
  {
    kOff = 0,
    kLow = 1,
    kMedium = 2,
    kHigh = 3,
    kUltra = 4
  };

  constexpr float Clamp(const float v, const float lo, const float hi)
  {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  constexpr float Power(const float intensity, const float radius)
  {
    return intensity * radius * radius;
  }

  constexpr int ClassOfPower(const float power)
  {
    if (power >= kPowerHuge) {
      return kHuge;
    }
    if (power >= kPowerLarge) {
      return kLarge;
    }
    if (power >= kPowerNormal) {
      return kNormal;
    }
    return kMinor;
  }

  // The lowest importance class a quality level lets through; kClassCount = nothing.
  constexpr int MinClassFor(const int quality)
  {
    switch (quality) {
    case kLow:
      return kHuge;
    case kMedium:
      return kLarge;
    case kHigh:
      return kNormal;
    case kUltra:
      return kMinor;
    default:
      return kClassCount;
    }
  }

  constexpr bool Admits(const int quality, const float power)
  {
    return ClassOfPower(power) >= MinClassFor(quality);
  }

  // ------------------------------------------------------------------------------------------------ explosion light particles
  // CreateLightParticle(entity, bone, army, size, lifetime, texture, ramp): a flash whose size the effect author chose (nukes 100..300).
  constexpr float kMaxSpriteSize = 1000.0f; // above this a particle is garbage, not a light
  constexpr float kMaxLifeTicks = 600.0f;   // a "light" that lives longer than a minute is not a flash

  constexpr float FlashRadius(const float size)
  {
    return Clamp(2.6f * size, 2.0f, 520.0f);
  }
  constexpr float FlashIntensity(const float size)
  {
    return Clamp(0.14f * size, 0.3f, 1.1f);
  }

  // ------------------------------------------------------------------------------------------------ glow sprites (fire, plasma, flares)
  constexpr float GlowRadius(const float size)
  {
    return Clamp(2.4f * size, 3.0f, 14.0f);
  }
  constexpr float GlowEnergy(const float size) // per sprite, before the ramp brightness
  {
    return 0.11f * size + 0.07f;
  }

  // ------------------------------------------------------------------------------------------------ trails and beams
  // Poly trails (projectile streaks) are thin: the Aeon laser bolt has size 0.035.
  constexpr float TrailRadius(const float size)
  {
    return Clamp(4.5f + 4.0f * size, 4.5f, 12.0f);
  }
  constexpr float TrailIntensity(const float size)
  {
    return Clamp(0.28f + 0.15f * size, 0.28f, 0.55f);
  }

  // ------------------------------------------------------------------------------------------------ level of detail
  // A very big light is weakened (the same intensity over a ten times larger area is ten times the light), but not below a floor that
  // grows with the radius: a nuke has to stay a nuke.
  constexpr float WeakenFloor(const float radius)
  {
    return radius >= 300.0f ? 0.55f : (radius >= 100.0f ? 0.45f : 0.30f);
  }
  constexpr float WeakenFactor(const float radius) // multiplies the intensity of a light of this radius
  {
    return Clamp(20.0f / radius, WeakenFloor(radius), 1.0f);
  }

  // ------------------------------------------------------------------------------------------------ envelope
  // A light fades in over kFadeInSeconds after its birth and out over kFadeOutSeconds after its life ends, so it never pops.
  constexpr float kFadeInSeconds = 0.08f;
  constexpr float kFadeOutSeconds = 0.30f;
  constexpr float kMinVisible = 0.015f; // colour x intensity below this adds nothing visible: the light is dropped
} // namespace moho::lights::model
