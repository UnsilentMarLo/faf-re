#pragma once

#include <cstddef>
#include <vector>

#include "moho/render/lights/LightCollector.h"
#include "moho/render/lights/LightGrid.h"

namespace moho
{
  struct SParticleBuffer;
  struct GeomCamera3;
}

// Engine side of the dynamic lights: reads the engine's effect records into the light pool (LightCollector) and evaluates the pool once
// per rendered frame. Render only; nothing here reads or writes sim state, so replay parity is unaffected.
//
// Stage 1 (docs/DYNAMIC_LIGHTS.md): the "TLight" particles of CreateLightParticle.
// Stage 2: BeginFrame also builds the light textures' CPU image (LightGrid) for the active map and uploads and binds it (LightBinding).
// Nothing draws the lights yet (the shaders do not declare the parameters); see ren_DynLightsView for a debug view of the grid.
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

  // Called after BeginFrame for each world view that is about to draw: tells the shaders where the camera is, and feeds the test light.
  void SetView(GeomCamera3& camera);

  // Console variable ren_DynLightsTest N: every N ticks a flash (size 12, 2 seconds) appears on the ground in the middle of the view.
  extern int ren_DynLightsTest;

  // The lights of the last BeginFrame, strongest first.
  const std::vector<Light>& FrameLights();

  // The texture images built from them (empty while no map is loaded); what LightBinding uploads and the debug view draws.
  const LightGridImage& FrameImage();

  constexpr std::size_t kMaxFrameLights = 256;
} // namespace moho::lights
