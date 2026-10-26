#pragma once

#include <cstddef>
#include <vector>

#include "moho/render/lights/LightCollector.h"

namespace moho
{
  struct SParticleBuffer;
}

// Engine side of the dynamic lights: reads the engine's effect records into the light pool (LightCollector) and evaluates the pool once
// per rendered frame. Render only; nothing here reads or writes sim state, so replay parity is unaffected.
//
// Stage 1 (docs/DYNAMIC_LIGHTS.md): the "TLight" particles of CreateLightParticle. Nothing draws the lights yet; the result is
// FrameLights() and the log line of ren_DynLightsLog.
namespace moho::lights
{
  // Quality level, model::kOff..model::kUltra (Options > Video > Dynamic Lights later; console variable ren_DynLights meanwhile).
  extern int ren_DynLights;

  // The one pool of the render thread.
  LightCollector& Collector();

  // Called when the user sync of a beat adds its particle buffer to the world particles (CWldSession); `beatTick` is the tick of that beat.
  void FeedParticleBuffer(const SParticleBuffer& batch, int beatTick);

  // Called once per rendered world view before the terrain is composited, with the clock the particle shaders use (tick + frame alpha).
  void BeginFrame(int tick, float frameAlpha);

  // The lights of the last BeginFrame, strongest first.
  const std::vector<Light>& FrameLights();

  constexpr std::size_t kMaxFrameLights = 256;
} // namespace moho::lights
