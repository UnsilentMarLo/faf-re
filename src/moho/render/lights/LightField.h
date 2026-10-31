#pragma once

#include <cstdint>

#include "boost/shared_ptr.h"
#include "moho/render/lights/LightGrid.h"

namespace gpg::gal
{
  class RenderTarget;
}

namespace moho
{
  struct GeomCamera3;
}

// Lighting V2, stage 2 (docs/LIGHTING_V2.md): a world-fixed irradiance field of the visible ground. A compute shader evaluates every light of
// the frame for each cell of the field (cells of 0.5 to 16 world units, a few layers of height above the terrain); the result goes into an
// RGBA16F atlas that the terrain and mesh shaders sample once at their world position instead of looping over the lights of a grid cell.
// The cost depends on the size of the field, not on the number of lights or of the passes that read it.
//
// Atlas layout (width = cells * layers, height = 2 * cells): layer l, cell (x, y) is at pixel (l * cells + x, y) for the first half and
// (l * cells + x, cells + y) for the second:
//   first half   rgb = sum of colour x attenuation of the lights (irradiance without a normal), a = terrain height at the cell
//   second half  xyz = sum of luminance x attenuation x direction to the light, w = sum of luminance x attenuation
// A surface with normal n then receives E * (1/3) + E * (2/3) * dot(n, D) / W, which is the wrapped diffuse term the grid path sums per light,
// exact for lights in the linear range of the wrap.
//
// Render thread only.
namespace moho::lights
{
  // 0 (the default while the field is tested): the light grid and the shader loops of docs/DYNAMIC_LIGHTS.md; 1: the irradiance field.
  extern int ren_DynLightsMode;

  // Elevation of the terrain at x, z; `context` is whatever the caller needs (the height field).
  using ElevationFunction = float (*)(const void* context, float x, float z);

  struct FieldFrame
  {
    boost::shared_ptr<gpg::gal::RenderTarget> atlas;
    float params[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // originX, originZ, 1 / cellSize, cells
    float params2[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // mode (1: sample the field, 0: use the grid), layers, 1 / atlas width, 1 / atlas height
    float layerHeights[4] = {0.5f, 3.0f, 10.0f, 30.0f}; // metres above the terrain, one per layer
    bool valid = false;
  };

  // Fits the field to the camera's view of the ground, uploads the lights of `image` and runs the compute and the copy into the atlas.
  // False (with `CurrentField().valid` false) when the field is off, unavailable on this API or has no light: the shaders then use the grid.
  bool UpdateField(const GeomCamera3& camera, const LightGridImage& image, const void* terrain, ElevationFunction elevation);

  const FieldFrame& CurrentField();

  // Frees the buffers, programs and the atlas (a device reset, shutdown).
  void ReleaseField();

  struct FieldStats
  {
    std::uint32_t frames = 0;
    float cellSize = 0.0f;
    std::uint32_t cells = 0;
    std::uint32_t lights = 0;
  };
  const FieldStats& GetFieldStats();
} // namespace moho::lights
