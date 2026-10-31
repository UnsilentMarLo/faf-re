# Lighting V2: lights without caps, shadows from every light, cost independent of the light count

Status: **design, revised 2026-10-31 after the render-path survey.** Replaces the per-pixel light loops of `docs/DYNAMIC_LIGHTS.md` (grid + shader loops) once it beats them in the 100v100 scene. Until then the old path stays as the fallback.

## Why

Measured (`FEATAB_COUNT=100 tests/featab -Scene battle`, D3D12, 1600x900, about 2,500 lights on screen, GPU ms per frame): lights off 4.6; lights on without shadows 14.2; with shadows 38.5 before the steam-re port, 132.8 after it. Each pixel loops over every light of its cell and, per light, over the shadow casters: cost = pixels x lights x casters, and it is paid again in the ground pass, every decal/splat layer and the second draw of every mesh. A cap (fewer lights, fewer casters) only trades looks for speed. The cost has to stop depending on the light count.

## Architecture: a world-space irradiance field, sampled once by the material shaders

The first draft (depth + normal pre-pass, screen-space irradiance) was dropped after the survey of the frame: the renderer has **one colour target per pass (no MRT), no float render-target formats, mesh depth techniques that write colour-encoded depth and no normals, and the main mesh batch runs after the terrain**; a pre-pass would add a second mesh batch and a screen-space normal reconstruction, and its result would shimmer with the camera.

```
1  light buffer (GPU)       every light of the frame in one structured buffer (GpuBuffer), written by the CPU collector; no grid, no cap
2  tile culling (compute)   coarse world tiles (for example 8 x 8 field cells): exact light list per tile from an atomic pool
3  field pass (pixel program, one draw per layer set)
                            a world-fixed field over the visible ground: cells of 0.5 to 1 world unit in XZ, a few layers of height above
                            the terrain (0, 2, 5, 10, 20, 40). Per cell: irradiance RGB + dominant light direction from every light of
                            the cell's tile (RGBA16F atlas render target, first-order directional irradiance)
4  shadows                  per cell: K lights picked by contribution (stochastic, weights 1/p), one shadow ray each against the terrain
                            heightfield and the caster list of the tile (analytic ellipsoid / box), later DXR 1.1 ray queries;
                            temporal accumulation in world space (cells do not move with the camera: no reprojection, no disocclusion)
5  material shaders         terrain, decals, meshes (mesh.fx, terrain.fx) sample the field once at their world position
                            (bilinear + a lerp between two height layers) and combine it with their own albedo, normal map and
                            specular; no light loops, no second mesh pass
```

- **Cost is bounded by the field, not by the lights, casters or passes:** cells (about 160,000 to 640,000 x 6 layers) x lights of the cell's tile for step 3, cells x K for step 4, one texture read per pixel and pass in step 5. It is the same few hundred thousand cell evaluations whether 25 or 25,000 lights are on, and the old per-pixel loops (pixels x lights x casters, three or more passes) disappear.
- **World-fixed, not screen-fixed:** stable under camera motion, no depth buffer, no normal buffer, no MRT, no extra geometry pass; temporal accumulation is a plain blend of the cell with its previous value.
- **Honest limits:** detail below the cell size (0.5 to 1 unit) is blurred, so light shapes are soft and the shadow of a small unit is a soft blob; the field covers the visible area plus a margin (the clip region follows the camera in whole-cell steps); a fast flying unit samples the nearest height layers. Chosen cell size is a measured trade-off, not a fixed promise.
- **Particles, shields, water** read the same field at their world position.

## Stages (each ships only if it beats the previous path in the 100v100 scene; the old path stays selectable until the end)

| # | What | Done when |
|---|---|---|
| 0 | Baseline (done): the table above | recorded |
| 1 | Backend groundwork: RGBA16F render target format on D3D11, D3D12, Vulkan (the survey: one table row in `DxgiFormats.cpp` plus the D3D11 and Vulkan format maps); a test that creates, draws and reads it | the target exists on all three backends |
| 2 | Light buffer + tile culling + field pass without shadows; `terrain.fx` / `mesh.fx` patched to sample the field instead of looping (cvar `ren_DynLightsMode 1`) | 100v100 lights-on GPU time within 2 ms of lights-off; no squares, no cap |
| 3 | Shadows: stochastic selection + rays against terrain heightfield and casters, temporal accumulation; DXR ray queries afterwards | 100v100 with full shadows within 3 ms of lights-off |
| 4 | Directional irradiance for normal maps and specular, cell size and layer tuning, D3D11 / Vulkan parity (ray queries where the API has them), old path removed | per-API A/B pictures |

## Survey facts the stages rely on

- Frame order (`WRenViewport::D3DWindowOnDeviceRender`, `moho/app/WxRuntimeTypes.cpp`): SkyDome, TerrainContext, TerrainNormals, Shadows (sun shadow map), **Terrain (4086)**, Ranges, MeshBatch, FeedMeshes (4139), Reflections, Meshes 0x14, Effects(true), Meshes 0x18, Water, AmbientOcclusion (4175), WorldUi, Meshes 0x24, Effects(false), ... `lights::BeginFrame` / `SetView` run at 4081, before the terrain. The field passes belong right there.
- Render targets: single colour + depth per pass; formats of `kRenderTargetFormats` (`gpg/gal/backends/dxgi/DxgiFormats.cpp:55`) are R10G10B10A2, BGRA8, BGRX8, 5551, 565, R16G16_UNORM; float formats exist for textures only.
- A GPU program (`CreateGpuProgram`, `DrawGpuProgram`) reads up to four structured buffers (t0..t3), two textures or depth or render targets (t4, t5) and one constant block; compute writes only `GpuBuffer`s (u0..u1): compute output reaches a texture through a pixel-program draw that reads the buffer.
- Effects bind a render target with `ShaderVar::SetRenderTargetTexture` (water does); translated pixel shaders get the pixel position as `SV_Position` for both `POSITION` and `VPOS`.
