#pragma once

#include <string>

namespace moho
{
  struct GeomCamera3;
}

// Water that moves when things move in it (docs/LIGHTING_V2.md, "Water simulation"): a damped height-field wave simulation on the GPU, a
// toroidal grid of 1024 x 1024 cells of one world unit that follows the camera over the water. Units standing in the water leave wakes, the
// impacts and explosions at the surface throw rings. The result (slope and height, one RGBA16F texture) bends the normal the water pixel
// shader uses, so the reflection, the refraction, the sun glints and the light glints of the field all see the waves, and pushes the wave
// crest colour (foam) where the water is high.
//
// Render only: the simulation never sees any of it (replays stay in step), and nothing here feeds back to the units. Cost: one compute
// dispatch of 1024 x 1024 cells per step (at most 6 steps a frame, 90 a second) and one copy draw.
namespace moho::water
{
  // 1 (the default): the simulation runs and the water shader reads it; 0: the shipped water.
  extern int ren_WaterSim;

  // A source of waves for the next steps: a disc of `radius` world units at (x, z) that pushes the water by `strength` (world units per second;
  // negative pulls it down). Collected over the frame, applied by the next step.
  // `foam` is how much foam it throws per frame (0 none; a wash of 0.005 to 0.01 is a steady band).
  void SimAddSource(float x, float z, float radius, float strength, float foam = 0.0f);

  // How strongly the wakes of ships and the splashes of impacts push the water, as factors (1 = the tuned default): ren_WaterSimWake and
  // ren_WaterSimImpact in percent.
  float SimWakeScale();
  float SimImpactScale();

  // The water of the active map (what the sources are compared with); false when the map has no water or the simulation is off.
  bool SimWaterLevel(float& elevation);

  // Once per frame before the water is drawn: steps the simulation, makes the texture and binds it to the water effect.
  void StepWaterSimulation(const GeomCamera3& camera);

  // The projected grid (docs/LIGHTING_V2.md): while the simulation is on, the water surface is drawn as a grid of vertices that the vertex shader
  // shoots along the camera rays onto the water plane and lifts by the height of the simulation, so the waves are geometry, not only shading.
  // Binds the grid's shader variables for this draw; false when the grid is off (the shipped quad is drawn).
  bool BindWaterGrid(const GeomCamera3& camera, float mapExtentX, float mapExtentZ);

  // Source patch of water2.fx: declares the variables and lets the ripples bend the normal and raise the wave crest. Text in, text out.
  struct SimPatchResult
  {
    bool patched = false;
    std::string error;
  };
  SimPatchResult PatchWaterSimulation(std::string& source);

  // Whether the water effect should be patched (the option is on).
  bool SimWanted();

  // Frees the GPU resources (a device reset, shutdown).
  void ReleaseWaterSimulation();
} // namespace moho::water
