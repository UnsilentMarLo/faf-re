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

## Stage 1 and 2 result (2026-10-31, no shadows yet)

Built: RGBA16F render target token 9 (`DxgiFormats.cpp`), `LightField.{h,cpp}` (compute: one 8x8x1 thread group per tile and layer, the lights culled against the tile in chunks of 1024 into group shared memory, so no list is ever cut; a fullscreen draw copies the field buffer into the atlas), `BindField` (`LightBinding.cpp`), `DynFieldSample` / `DynFieldDiffuse` / `DynFieldBRDF` in the injected HLSL (`LightEffectPatch.cpp`; the terrain composite, decal / splat layers and mesh light pass branch on `dynFieldParams2.x`). Switch: `ren_DynLightsMode 1` (default 0 = the old grid path).

Measured, same scene as the baseline (100v100, 2,500 lights, D3D12, 1600x900, `FAF_GPU_PROFILE=300`; lights off 4.6 ms; old path with shadows 132.8 ms; without shadows 14.2 ms):

| | GPU per frame | Terrain | Meshes | LightField |
|---|---|---|---|---|
| field (mode 1), no shadows | 6.3 ms | 0.29 ms | 0.21 ms | 0.27 ms |

The lights cost 1.7 ms over lights off, with every light of the frame in the sum (no cap, no cell borders). Pictures: the same fight, old path and field, light the ground alike. Open: shadows (stage 3), a light-only packer instead of the CPU grid build (`BuildLightGrid` still runs; its `image.data` feeds the compute), more than 4096 lights, D3D11 / Vulkan runs.

## Stage 3 result (2026-10-31): shadows in the field, default on

The compute pass also shades: per cell, four reservoirs pick lights with probability proportional to their light (weighted reservoir sampling over the chunked light stream, so no list is cut), each picked light is tested against the casters near the tile (also chunked), and the mean transmittance of the samples is the cell's visibility. Only the visibility is averaged over time (blend 0.25, history shifted with the window; the lights of the cell are exact every frame, so a flash is not delayed). Casters are the engine's boxes as ellipsoids at 0.8 of their half extents, the soft shape the steam-re session calibrated (60 % at most). Switch `ren_DynLightsFieldShadows` (default 1). `ren_DynLightsMode 1` is now the default; `0` brings back the grid path.

| 100v100, 2,500 lights, ~100 casters, 1600x900 | GPU per frame | LightField pass |
|---|---|---|
| lights off | 4.6 ms (D3D12) | |
| old path, shadows (before this work) | 132.8 ms | |
| field, no shadows, D3D12 | 6.3 ms | 0.27 ms |
| **field with shadows, D3D12** | **6.5 ms** | 0.34 ms |
| field with shadows, D3D11 | 4.1 ms total | 0.42 ms |
| field with shadows, Vulkan | 6.8 ms total | 0.38 ms |

Shadow scene (`steps_fshadow.lua`): shadows on vs off differ by 1.03 (mean pixel difference), two runs with shadows on differ by 0.009 (no flicker). Open: terrain height-field shadows (lights behind ridges), more than 4,096 lights and 1,024 casters (the CPU packer limits), real unit silhouettes (the casters are ellipsoids), directional specular quality, the cost of a zoomed-in view (cells of 0.5), a light-only packer without the CPU grid.

## Stage 4: terrain and water in the field (2026-11-01)

Survey facts (water2.fx, `WaterEffectPatch`, `STIMap`): the water is one quad at `WaterElevation` drawn by `HighFidelityPS` after the scene copy; its depth term is a baked half-resolution map (`UtilitySamplerC.g`); terrain and meshes tint by depth through `ApplyWaterColor` / the mesh equivalent; terrain is not in the sun shadow map; `CHeightField` gives raw uint16 samples at 1/128 unit; `STIMap` has `IsWaterEnabled()` / `GetWaterElevation()`.

Built:

- **Full-resolution terrain in the compute pass.** The map's raw height samples go to the GPU once (again when `HeightFieldRevision()` changes), two to a word; the pass reads them bilinear (it used a 129x129 lattice before). A log line checks the unit against `GetElevation` at start (19.000 against 19.000 on Adaptive Acid Lakes).
- **Ridge shadows for lights.** Each shadow sample marches its ray over the terrain (12 steps, soft penumbra widening along the ray) next to the caster test; a light behind a ridge no longer lights the ground behind it.
- **Water in the field.** Layers are now: 0 the terrain 0.5 above it (the sea bed under water), 1 to 4 the surface (water or terrain) 0.3, 3, 10, 30 above it; the alpha of layers 0 and 1 holds the terrain and the surface height. The shaders sample by height: a point under the surface blends the sea bed and surface layers, one above uses the layers above the surface.
- **Underwater lights and water depth.** Every light is weakened by the length of its path inside the water (`WaterTransmit`, Beer-Lambert, absorption 0.25 / 0.06 / 0.03 per unit for red / green / blue), for lights below the surface and for lights above it that reach a submerged point.
- **Lit water.** `lights::PatchWaterEffect` (chained with the water quality patch in `CD3DEffectTechnique.cpp`, cache tag `.waterl1`) adds `FafWaterLights` to `HighFidelityPS`: GGX glints of the combined light on the wave normal, and the light scattered back out of the water in the water colour. The field variables are bound to `water2` like to `mesh` and `terrain`.

Measured: 100v100, D3D12: GPU 6.47 ms, LightField pass 0.41 ms (0.34 before the terrain march). A lit lake (hover tanks with light particles, `-Scene waterlight`) lights the water, the sea bed and the shore; not yet compared against the old path by eye in a deep-water map (the test map's lake is 2.5 units deep).

Next: sun shadows of the terrain onto terrain, units and water (a part-2 plane in the atlas), per-pixel water depth from a full-resolution height texture, a height-field wave simulation feeding the water normals (units, projectiles and impacts as sources), then displaced water geometry.

## Stage 5: the terrain's shadow of the sun (2026-11-01)

The sun's light is marched over the same raw heights as the light rays: for each cell of layers 0 and 1 (the terrain and the surface), 40 samples along the direction to the sun with steps that grow with the distance (up to 500 units, long shadows of a low sun) and a penumbra that widens along the ray. The result is part 3 of the atlas (the atlas is now `3 * cells` high); `DynFieldSun(p)` reads it (a point under the surface blends the sea bed and surface values).

Consumers, all best effort and falling back to the unpatched text:

- **Terrain:** every read of the shadow map (`tex2D(ShadowSampler, X.mShadow.xy).g`, or `ShadowLookup(X.mShadow)` after the shadow filter patch) is multiplied by `DynFieldSun(X.mTexWT.xzy)`; `CalculateLighting`'s shadow too (decals and splats). The terrain knows its world position, so no matrix is needed. The terrain shaders only read the shadow map while `ShadowsEnabled` is set, which was `mShadowCameraValid`: it is now also set while the field carries the sun shadow (`lights::SunShadowActive()`), the map being cleared to lit when it has no casters.
- **Meshes:** `ComputeShadow` is wrapped (`ComputeShadowBase` keeps the old body); the pixel's shadow coordinates are mapped back to a clip position, and the inverse of the sun's shadow matrix (`dynShadowInv`, bound from `WRenViewport` after the shadow pass) gives the world position.
- **Water:** the sun reflection is multiplied by `DynFieldSun` at the water surface (a ridge or a shore wall cuts the glint).

Switch: `ren_DynLightsSunShadows` (default 1; 0 off; 2 a checker and 3 "no sun anywhere", both debug views to see that the term reaches the shaders). The field also runs when no light is on, for the sun shadow alone, while Dynamic Lights is not Off (the effect patches belong to that option).

Shader model limit: ps_4_1 allows 16 samplers and some terrain PBR techniques already use 15, so the extra field read does not compile on D3D11; the effect builder retries without the PBR reads (`PatchTerrainEffect(..., sunReads = false)`), keeping the sun shadow in `CalculateLighting` (decals, splats). D3D12 and Vulkan keep all seven terrain reads.

Measured (100v100, 2,500 lights): D3D12 GPU 6.69 ms (LightField 0.55 ms; it was 0.41 ms with ridge shadows for lights only), Vulkan 7.06 ms, D3D11 4.26 ms. Lake crater test (`steps_sun5.lua`): sun shadows on vs off differ by 0.66 (mean pixel difference), the difference is a shadow band from the crater rim and cut glints on the lake; the debug "no sun" view changes the image by 22.

## Stage 6: water that moves (2026-11-01)

`src/moho/terrain/water/WaterSimulation.{h,cpp}`, switch `ren_WaterSim` (default 1; patched into `water2.fx` at load, cache tag `.watersim1`).

- **Simulation.** A damped height-field wave equation on the GPU: a toroidal grid of 1024 x 1024 cells of one world unit that follows the point of the water the middle of the big world view looks at (the window moves in whole thread groups; cells it moves onto are started calm, the border absorbs). Fixed steps of 1/90 s (at most 6 a frame), real time, render only: nothing here touches the simulation of the game, so replays stay in step. Sources are collected over the frame in `SimAddSource`: units standing in the water (`FeedWaterSources` in `LightMeshFeed.cpp`: the bodies that cast shadows, with their speed from the start and end transform: a wake) and flashes or glow sprites at the water surface (`NoteWaterImpact` in `LightFeedSources.cpp`: an impact or an explosion throws a ring). The compute pass culls the sources against its tile in chunks, like the light field.
- **Normals.** The result (slope x, slope z, height) is one RGBA16F texture; `water2.fx` `HighFidelityPS` bends the wave normal with it before the reflection, the refraction offset, the sun glints and the light glints of the field read the normal, and raises the wave crest (foam) where the water is high. `ren_WaterSimStrength` (percent).
- **Geometry.** While the simulation is on, `RenderWaterSurface` draws a projected grid of 180 x 180 vertices instead of the 4-vertex quad (`ren_WaterGrid`, default 1): the vertex shader shoots each vertex along its screen ray (`waterGridInv`, the camera's inverse view projection) onto the water plane, limited to the map, and lifts it by the simulation height (`ren_WaterDisplace`, percent). The flat quad stays for the mask and shoreline passes and when the grid is off. Test: `ren_WaterSimTest N` throws a ring in the middle of the view every N frames; at `ren_WaterDisplace 400` the lifted water sheet is plainly visible against the shore.
- **Checked:** D3D12, D3D11 and Vulkan build and draw the patched water; the test ring appears in the normals, the foam and the geometry; the flat quad and the grid give the same picture without waves. **Not checked:** wakes of moving units (the hover tanks of the test scene never stayed on the lake; the unit filter was never seen to fire), impacts in a real fight, the cost at 100v100 on a map with a big sea, the strength of rings from many impacts.
- **Also:** the field (and so the lights and sun shadows) is now only built for the big world view (the minimap's render pass used to move it).

Next (after stages 7 to 9): wave-simulation caustics, wakes and impacts seen in a real fight.

## Stage 7: the water reflects the scene (2026-11-01)

`src/moho/terrain/water/WaterReflections.{h,cpp}`, switch `ren_WaterSSR` (0 off, 1 default = 24 march steps, 2 = 48; patched into `water2.fx` at load, cache tag `.waterssr1`, falls back to the unpatched effect if the backend rejects it). Direct3D 11, 12 and Vulkan; D3D9 keeps the shipped water.

- **What it does.** `HighFidelityPS` marches the reflected view ray (the wave-bent normal, so ripples distort the picture) through a copy of the scene depth, in screen space with perspective-correct 1/w interpolation, a per-pixel jitter and five binary-search steps at the hit. A hit takes the pixel of the scene colour the refraction already holds (the copy taken right before the water: terrain, buildings, units, effects, never the water itself). The result replaces the sky / mirrored-unit reflection by the hit confidence (screen-edge and end-of-ray fade) before the Fresnel mix, and the sun glint is cut where a hit is reflected. A hit counts only if the ray went behind the scene surface by less than about two steps, so foreground objects do not smear. The sea bed can never be hit by a ray above the water plane (the camera ray of that pixel meets the plane first).
- **Binding.** `BindWaterReflections` (called from `WRenViewport::RenderWater`) copies the head depth with `CopyDepthStencil` into a sampled target of its own (the depth buffer stays a depth output while the water draws; the same pattern as the soft particles), binds it and the march parameters (`waterSceneDepth`, `waterSsrParams`, `waterSsrParams2`), and binds "off" for an orthographic view (the minimap), a depth buffer that was not created sampled, or a backend that cannot copy depth.
- **Console.** `ren_WaterSSR`, `ren_WaterSSRStrength` (percent of the reflection the hits replace, default 100), `ren_WaterSSRDistance` (longest ray, default 800 units), `ren_WaterSSRDebug 1` (red = hit, blue = miss).
- **Checked** (Adaptive Acid Lakes, hover tanks on the lake, `tests/featab/steps_ssr.lua`): D3D12, D3D11 and Vulkan compile the fully patched water (lights + simulation + quality filter + reflections) and the debug view shows hits on the crater walls and on the unit in the water, misses where the ray escapes to the sky. Cost on D3D12 with the lake about a quarter of a 1600x900 screen: water pass 0.065 ms with the march off, 0.12-0.14 ms at level 1, 0.155 ms at level 2; the depth copy 0.018 ms. **Not checked:** a large sea with buildings on the shore, 100v100 on a sea map, split-screen / smaller view rectangles (the march clamps to the whole texture), the look at very grazing angles (the test lake is small and shallow, so the camera framing for those was poor).
- **Why the effect is subtle at steep angles.** The reflection weight is still the shipped Fresnel term (F0 0.08); a steep view keeps about a tenth of the reflection, a grazing view most of it. That is the physical behaviour and the shipped balance, so old maps keep their look; `ren_WaterSSRStrength` only scales the hit part.

## Stage 8: the water's thickness from the real scene depth (2026-11-01)

Same file and patch as stage 7 (`WaterReflections.cpp`; the depth copy is shared, the patch is applied when `ren_WaterSSR` or `ren_WaterAbsorb` is on; cache tag now `.waterssr2`). Switch `ren_WaterAbsorb` (0 shipped baked depth map, 1 default, 2 adds colour extinction), `ren_WaterAbsorbDepth` (world units, default 10).

- **What it does.** The baked half-resolution depth map (`UtilitySamplerC.g`, authored with the map and often wrong near units, bridges and after terrain edits) is replaced in `HighFidelityPS` by the distance the view ray really travels through the water: `(sceneZ - waterZ) * |eye| / waterZ`, where `sceneZ` is the sampled scene depth under the refracted lookup and `waterZ` is the water pixel's clip `w`. It is turned into the same 0..1 quantity the shader always used, `1 - exp(-path / ren_WaterAbsorbDepth)`, so the map's `waterLerp` clamp, `waterColor` and `refractionScale` keep their meaning and old maps stay in their authored range.
- **Refraction.** The first estimate sizes the wave offset; if the refracted lookup lands on something in front of the water plane (a unit, a bridge, the shore) the lookup falls back to the straight view, then the thickness is read where the final lookup lands. Level 2 also multiplies the refracted picture by `exp(-(1, 0.4, 0.15) * path / (4 * depth))`, red absorbed first.
- **Fallbacks.** With the depth copy unavailable (orthographic view, depth not sampled, backend cannot copy) the parameters bind "off" and the shipped depth map is used.
- **Checked** (Acid Lakes lake, `tests/featab/steps_abs.lua`): compiles and renders on D3D12, D3D11 and Vulkan; on the lake the sea bed shows through more in the shallows and the colour follows the real thickness (4 to 10 percent of the frame changes with the default and with a murky depth of 3). Cost on D3D12 (same lake view): the water pass reads 0.131-0.152 ms with the option off or on at either level, within the noise of the level-1 reflections of stage 7; it reuses the depth copy of stage 7 (0.018 ms). Results on D3D11 and Vulkan match D3D12 (the same 13.9 percent of the cropped lake changes at level 2). **Not checked:** a large sea, shorelines with buildings, split-screen, unit wakes under real depth.

## Stage 9: unit shadows on the water and caustics (2026-11-01)

Same file and patch again (`WaterReflections.cpp`, cache tag `.waterssr11`).

- **Unit sun shadows.** `ren_WaterUnitShadows` (percent, default 45, 0 off). `BindWaterReflections` now takes the viewport's `Shadow` renderer and binds the sun shadow map the terrain and the meshes already read (`waterShadowTexture`, `waterShadowMatrix = Shadow::mCamera.viewProjection`, bound only when `mShadowCameraValid`). `HighFidelityPS` reads it at the water pixel's world position (`FafWaterShadow`, linear filter, border = lit), darkens the transmitted light by the strength where it is in shade and cuts the sun glint fully. The terrain's ridge shadow stays with the light field (stage 5). `ren_WaterSSRDebug 2` shows the shadow value (white lit, black shade, yellow = no map bound; note the water's own blend alpha scales the picture, so shallow rims look grey).
- **Caustics.** `ren_WaterCaustics` (percent, default 50, 0 off; needs `ren_WaterAbsorb`). For the sea bed seen through the water (the bed point is rebuilt from the real thickness of stage 8) `FafCaustic` takes the slope of the two small wave normal layers at that world position (same maps and movement the surface uses, so the pattern moves with the waves), the divergence of that slope field by finite differences, and turns it into focused light: `0.6 * (1 / max(1 - depth * divergence * 1.5, 0.2))^1.6`, clamped 0.25..4, mean about 1. It is applied as `1 + strength * fade * baked sun shadow * (caustic - 1)` to the refracted picture before the tint, fading with `exp(-depth / (0.5 * ren_WaterAbsorbDepth))`, so deep water and shadowed bed get none.
- **Checked** (Acid Lakes, `tests/featab/steps_wsc.lua`, `steps_wsh.lua`, `steps_wsp.lua`): compiles and renders on D3D12, D3D11 and Vulkan (the same 23 percent of the lake view changes with everything on). The shadow binding follows the cvar and the shadow map (debug view; the hover tank's own shade shows black on the lake); caustics read as a moving dappled web on the lake floor. Cost on D3D12 (lake about a quarter of the screen): water pass about 0.135 ms with stages 8 and 9 off, 0.145-0.15 ms with everything on.
- **Not checked / open.** Unit shadows were only seen under a hover tank (small); a large unit or building next to the shore is the real test. Caustics are procedural from the normal maps (not from the wave simulation, so wakes and impact rings do not focus light yet), have no colour dispersion, and at 100 percent look busy; 50 is the default. Large sea, split-screen, 100v100, wakes under real depth.

Next: feed the wave simulation's slope into the caustics and the shadow, wakes and impacts in a real fight.

## Fixes after the first real game (2026-11-01)

The first game played with stages 5 to 9 (Seton's Clutch, D3D12, Dynamic Lights Ultra, shadow filter Ultra, water High) showed: water moving when things happened ashore, one small explosion rippling across the map, tree shadows on the water far from the trees, a GPU hang after about five minutes, no dynamic lights and bad map shadows. The harness (`tests/featab`) had forced the light and shadow options off, so none of it had been seen: **test with the user's options** (`-ExtraCon ';ren_ShadowFilter=4;ren_DynLights=4;ren_WaterQuality=3'`, and `run_ai_games.py --faf-bin C:/ProgramData/FAForever/bin_x64 --exe <name>.exe` plays a whole game with the copy of the user's `Game.prefs`).

- **Water simulation and land.** Causes: a source was accepted wherever the *height* matched the sea (a low beach, a unit ashore), the glow feed threw a new ring every frame while a fire burned, and the wave equation ran over land. Now: `SimAddSource` drops a source whose ground is not below the water level; impacts are one ring per place (4-unit cell) and 0.8 s, glows throw none; the map's raw heights go to the GPU once (`WaterSimHeights`, the same packing as the light field) and a land cell is a wall in `SimCS` (it holds no wave, so a wave dies at the beach and nothing ashore reaches the sea); damping 0.995 to 0.99 a step, impact strength 9 to 5 per size unit.
- **Tree shadows on the water.** The sun shadow map holds the depth of the nearest caster (R) and a blurred shade (G) without knowing the receiver; with G alone any tree whose silhouette lines up with a water pixel along the sun ray shades it, including trees on a hill that the ray passes far below. `FafWaterShadow` now measures the distance along the sun ray between receiver and caster (`(z - R) * world units per depth unit`, the unit from the shadow matrix) and counts only casters within 20 units fully, fading to nothing at 45. Cache tag `.waterssr12`. Seen and fixed on Seton's Clutch (`tests/featab/steps_tree2.lua`, `ren_WaterSSRDebug 2` shows the value).
- **Ambient occlusion under water.** The pass ran after the water, read the depth of the sea bed and darkened the already tinted and refracted colour: halos round submerged units and streaks on the bed. It now runs before the water (after the opaque meshes), so the water tints and refracts the darkened bed and the surface is no geometry for it. Not checked beyond the build and a lake frame (the test lake shows no submerged units).
- **Open: a GPU page fault in `terrain.fx:TTerrain` (D3D12 device hung).** Seen in two real games on Seton's Clutch (game time 5:36 by default, 1:04 with `FAF_GAL_UPLOAD_SUBMIT_KB=300` which forces mid-frame submits); both times the faulting command list was cut right after the first terrain draw, no live or freed allocation at the address. Not reproduced in five further games (water simulation off, its dispatches or its copy skipped, lights and simulation off, debug layer on): too few runs to name a cause. Next: longer runs with `/d3d12dred`, bisect lights vs water simulation with many more games.
- **Open: a dark region with a straight edge when the view is zoomed out in a game** (terrain only, sea unaffected). Not reproduced in static scenes; suspected: the sun term of the field when the shadow camera turns off above `ren_ShadowLOD`.


### GPU hang found: the water march loop (2026-11-01)

The device hang with a page fault in the water draw (reproduced in every real game and in 10 of 10 test games under `/d3d12gbv`, 36 to 100 s after the start) came from the dynamic `[loop]` with a `break` of the screen-space march in `FafWaterSSR`. Bisect (one 2 min AI game each, Seton's Clutch): lights off, simulation off, absorb off, caustics off, unit shadows off all still crashed; `ren_WaterSSR 0` (march skipped, patch otherwise on) and no patch at all ran for good. A temporary switch that left the loop but removed its depth reads crashed as well; exits before the loop (after the matrix setup, after the screen range) ran for good. So the loop itself, not what it reads. The march is now a fixed 32 step unrolled loop with a latched hit (`ren_WaterSSR` 1 and 2 are the same now); three 3 min games on that build ran through under GBV. The cause inside the driver (AMD, RX 7900 XTX) is not known.

Water unit/tree shadows: reach tightened to casters within 8 + 8 world units of the surface (was 20 + 25), so trees on hills no longer shade the water.

### Water shadow lookup, ridge shadows, hang notes (2026-11-01 night)

- The water's sun shadow lookup (`FafWaterShadow`) used the clip space coordinates of the shadow matrix as texture coordinates: no `(x + w) / 2`, `(w - y) / 2` mapping like the terrain's vertex shaders (`mShadow.x = (x + w) * 0.5; mShadow.y = (-y + w) * 0.5`). It read the wrong place of the shadow map (magnified, flipped): tree, prop and unit shapes on the water far from their owners. Fixed in `WaterReflections.cpp` (shader cache tag `.waterssr18`). Not shown in a before/after frame (no shadow falls on water in the test scenes), found by comparing with the terrain effect.
- Ridge shadows (the light field's `SunVis`): a first try (a shadow edge at least 0.6 of a cell wide) made the whole terrain dark when zoomed out (cells of 8 to 16 units: the clearance of every ray near the ground divided by a large edge): measured mean brightness 43 -> 29 over a zoom sweep. Replaced by `SunVisCell`: from cells of 2 units on the visibility is the mean of four points around the cell centre (mean brightness 48 to 58 at every zoom). The march has no early `break` any more (dynamic loops with a break hung the GPU in the water shader).
- The terrain's filtered shadow lookup is off while the shadow camera is invalid and counts ground outside the light volume as lit (the diagonal dark half with the light field's sun term).
- The sun's shadow on the sea bed fades with the water depth (`FafSunUnder`, `LightEffectPatch.cpp`).
- The height map re-uploads of the light field and the water land mask (every building that flattens ground bumps `HeightFieldRevision`) run at most every 90 frames.
- GPU hang on Seton's Clutch (2026-11-01 16:58, 100 s into a real game) after the unrolled SSR march: not reproduced by two 4 min AI games without the debug layer. `faf_dred.on` next to the exe switches DRED on for launches without a command line (the crash report's recent log then has the breadcrumbs).

### Water simulation: ship wakes (2026-11-01 night)

- Cause of the dragged bubble and the shaking sea: a moving hull was one positive disc (radius 1.1 r, strength up to 14 x (0.5 + 0.2 r), 35 for the Fatboy). A steady push on a wave field settles at about a R^2 / (4 c^2) (c^2 = 0.22): tens of units, held by the +-6 clamp. The same went for impacts. Now: the clamp is +-1.5 and the damping 0.985 a step; a hull is a pair of discs along its heading, bow +, stern - (no net volume: nothing piles up and follows the ship), sized for a peak height of 0.12 + 0.025 r (at most 0.45) at full speed; ren_WaterSimWake / ren_WaterSimImpact scale ships and splashes in percent.
- A body only moves water when its middle is not deeper than a quarter of its radius under the surface (a submarine, an amphibious unit on the sea bed, a submerged Fatboy do not).
- `ren_WaterSimLog N` logs the highest and lowest wave height every N frames (reads the state back: stalls the GPU, diagnostics only). Measured with the test frigate and the Fatboy sailing: -0.17 .. 0.16, 50 to 150 cells above 0.02 of 1,048,576.
- The wake particles of the shipped game (the effects of the unit blueprints) are not part of this and are not touched.
- Crash fixed (2026-11-01 18:23, after 20 game minutes): LoadTexture cleared its cache when it held 512 textures while callers held pointers (AnalyseMeshGlow read the albedo after the specular map had loaded). TrimTextureCache runs between frames; the glow map is trimmed in FeedMeshes.

### Water simulation rebuilt at hull scale (2026-11-01 night, second part)

User brief: the water should be what Red Alert 3 has: a V bow wake, a noisy propeller wash, wash-up on the shore, constant wind waves, more surface variation, a more solid sea, shells and explosions that rock the sea. What was wrong: the wave field travelled at 40 units a second (ten times a hull's waves), no foam, one disc per hull, clamps of +-6.
- Simulation: 0.5 unit cells (a 512 unit window follows the view), wave speed 2.5 units a second (term 0.0031), damping 0.998 a step, +-1.5 clamp, land reflects waves. State is float4 (height, last height, foam, land flag), sources are two float4 (push; foam).
- Foam channel: spreads a little, fades in a couple of seconds (0.994 a step), thrown by the sources (noisy: the hash of cell and frame) and by breaking crests (|h| above 0.15) and by the surf at the beach (cells next to land, driven by the wind sea height). Drawn over the water colour as a threshold with the normal map's alpha as breakup (white flecks, not a haze).
- Hull: bow push + foam, stern trough + wash foam, a third source behind the stern with foam only (the propeller wash); discs of 0.35 x radius, narrow. A body whose middle is under water moves nothing.
- Impacts: a hump (0.9 x size) and a burst of foam.
- Sea look: `ren_WaterSolid` (default 25) is the least share of the water colour over the sea bed; normals sharpened (1.45x); two slow low frequency reads of the shipped normal maps add large scale undulation (noise, no lattice). Mistakes made and removed on the way, kept here so they are not repeated: eight analytic sharp-crest wind waves lifted the mesh and bent the normals (the interference showed as a diamond lattice of facets and the short waves oscillated fast: a swell of short waves cannot be both small and slow); the foam of breaking crests at 0.03 a step covered the wake in a grey slab 150 units long.
- Open: the V is subtle, hull waves are +-0.2 units; a wash-up with real run-up on the beach (the surf is foam only); explosions at scale are untested; wind sea height is only used for the surf. Not tested in a real game by the author: judged from frames of one destroyer sailing a bay and the `ren_WaterSimLog` numbers.

### Water: sizes of wakes, shells and deaths (2026-11-01 night, third part)

User report: the wave sizes did not match the units: a tiny unit made huge waves, a battleship none; projectile impacts made no waves; deaths made waves that did not match the size.
- Cause of the hulls: the push strength fell with the disc size (a push `a` on a disc of R cells), so small hulls got the strongest pushes; measured (one unit sailing the bay, `ren_WaterSimLog`): hover tank (radius 0.6) 0.43, frigate (2.6) 0.77, destroyer (3.5) 0.71, cruiser (3.4) 0.54, battleship (8.1, slow) 0.15. Each unit is ONE mesh instance (no turret meshes): its radius is the size.
- Now: height = 0.45 x (radius / 8)^1.1 (at least 0.02, at most 0.7) x a speed factor (speed / 4.5, between 0.8 and 1.3), disc radius 0.4 x radius (0.8 to 7), the push from the measured response: peak = push x (2.5 + 0.8 x cells). Measured after: hover tank 0.017, frigate 0.15, destroyer 0.27, cruiser 0.19, battleship about 0.3 or more; the submerged amphibious experimental moves nothing (its middle is under the surface).
- Projectile impacts: `Projectile::Impact` calls `water::SimImpact` for impacts on water and under it (`IMPACT_Water`, `Underwater`, `ProjectileUnderwater`, `UnitUnderwater`; half strength below): the size is the cube root of the damage (`mDamage`, or the `DamageData.DamageAmount` of the Lua object), debris below 12 damage is ignored. Radius 0.7 x size (0.8 to 18), hump height 0.07 x size (0.06 to 1.2); the push of one frame is added to the height of a step and leaves the water with a velocity: a twentieth of the wanted hump is pushed (measured rings: 500 damage 0.56, 3,000 1.0, 20,000 1.26 units).
- Deaths: `Unit::Kill` calls `water::SimUnitDeath` with the position and the mass cost: size 0.14 x sqrt(cost) (cost 300: a shell of 15 damage, 2,250: 300, 10,000: 2,700). Airborne wrecks (more than 3 units above the surface) move nothing, submerged ones half. Measured rings: cost 280 to 320: 0.15 to 0.47 (the T2 hover tank is the odd one: its own death throws more), cost 2,250: 0.45, 10,000: 0.92.
- The glow and flash sprites of the shipped effects no longer throw rings (their size says nothing about what landed or sank).
- All of this is render only: nothing is read back into the sim. `ren_WaterSimLog` logs the impacts and deaths the water receives.
- Aircraft: the death in the air splashes nothing (`Unit::Kill` skips the air layer); the wreck splashes where it lands: `CUnitMotion` fires the unit's `OnImpact("Water")` when a dead unit's layer changes to the water, and calls `water::SimUnitDeath` there with the position and the cost. Tested over the bay: a gunship (cost 192) and a strategic bomber (cost 1750) splash at the surface; a fighter (`uea0102`) logged nothing (not understood).

### Water: pause, fog, weight, splashes (2026-11-01 night, fourth part)

- Pause: the water keeps game time, not wall time. `StepWaterSimulation` watches the game tick: no new tick for a third of a second is a pause (dt 0: no steps, the foam does not fade, the swell stands still), and the speed of the game (ticks over real time, smoothed) scales dt. The shipped water's `Time` is `tick + tickLerp` already. Tested: 270 log lines after a pause are identical; the frames are identical.
- Fog: `SimAddSource` drops a source at a place the local player cannot see (the focus army's vision circles from `session->mVisionDb.CollectVisibleCircles`; an observer sees all). Exempt (`shown`): meshes that are being drawn, and events of the player's own army or an ally's (`water::SimIsOwnSide`): a dying unit takes its vision with it, and the destroyer's own death splash was lost to the fog test before that. Enemy shells and deaths in the fog make no waves.
- Nothing in this feeds back: the hooks (`Projectile::Impact`, `Unit::Kill`, `CUnitMotion` OnImpact) only read the position, the damage (`mDamage`, or a raw read of `DamageData.DamageAmount` of the projectile's Lua table, identical on every client) and the cost, and call a render-side queue. Collision and the waterline of the sim stay the flat plane of the map (`mWaterElevation`); the simulated height only moves the drawn surface.
- Weight beats speed for hulls: height = 1.0 x (radius / 8)^1.8 (0.05 to 0.9) x (0.7 + 0.3 x min(speed / 6, 1.3)). Measured sailing the bay: hover tank 0.005, frigate 0.034, destroyer 0.088, cruiser 0.076, battleship 0.40.
- Splashes: viscosity (0.16 x the laplacian of the velocity) damps the ringing; a splash is 4 to 8 lumps of different size, place and strength (some pull down); hump 0.03 x size (at most 0.5); little foam. Ship foam raised (bow 0.005, stern 0.008, wash 0.007 per frame x speed x body). Measured: shells of 500, 3,000, 20,000 damage: 0.11, 0.72, 0.86; deaths: hover tank 0.13, frigate 0.01, destroyer 0.10, battleship 0.43.

### Water: the shape of a wake (2026-11-01 night, fifth part)

- A round hull (Aeon Tempest) had only a small pointed bow wave and foam: the wake was sized from the bounding sphere radius. Now `AddWaterSource` reads the oriented box of the mesh instance (`instance.box`, `boundsValid`) and takes the half length along the heading and the half beam across it. The bow wave is an arc of 1 to 5 lumps across the beam (one per 1.6 units of half beam), the stern trough as wide, the wash behind as wide as the hull, and foam along both sides (three discs a side).
- Straight walls: a steady push in a non dispersive wave field makes one sharp straight ridge at the edge of the wake. The push now alternates with the place along the track (a world fixed pattern, wavelength 1.1 x the half length of the hull, at least 2.5 units), so the water behind a moving ship carries a train of crests and troughs one hull length apart; each lump has a random strength (0.6 to 1.4 x) and size (0.8 to 1.2 x). No state per ship.
- Visibility: wake heights x2.4, `ren_WaterSimWake` default 200, the ripples bend the normal at 0.2 x `ren_WaterSimStrength` (percent, 100 default); viscosity eased to 0.10. At wake x4 and bend x3 over this the foam churn was too much; this is the middle. `kMaxSources` is 4096 (a broad hull throws up to 17 sources a frame).
- The red patches near the hulls in the test frames are the reflection of the (red) hull in the bent surface (`unitReflections`), not an artefact of the simulation.
- The thin pale circles around a selected ship in the frames are its range rings (UI), not the water.

### Dynamic lights: weak lights, build beams, the Titan (2026-11-01 night)

User report: a UEF Titan glows only up close, gunfire of a Fatboy is visible, weaker lights vanish, no build beams.
- Zoom: the cells of the light field grow with the zoom (0.5 units close up, 2 to 16 zoomed out). A light smaller than a cell fell between the cell centres and vanished. The compute pass now grows every light's footprint to at least 1.5 cells (also in the tile culling) and keeps the square root of the share of its radius as strength (`splat`): a small glow gets wider and dimmer instead of going out.
- Importance classes: `kPowerNormal` 9 -> 4. The build beams (intensity x radius^2 about 7), reclaim rays and weak unit glows were class Minor and only Ultra kept them. `kMinVisible` 0.035 -> 0.02. Beams: radius 4.5 + 10 x width (4.5 to 14), intensity 1.4 + 0.6 x width (1.4 to 1.9) before the colour of the texture. Mesh glows: intensity cap 0.85 (was 0.55), exponent 0.35 of the glow area (was 0.5; the assault bot with a few lamps got 0.046, at the edge of the drop limit, now 0.1), radius 2 x sphere + 2 (was 1.5 x + 1).
- The Titan (uel0303): its mesh material is `PhaseShield`, and a shader name with "shield" in it was taken for a shield bubble: no light at all. A unit with a shield-named shader and an albedo under `/units/` is a unit now (`GlowOf`). But the specular map of the Titan has no emissive channel in the rule of the UEF units (specular.b): its glow area is 0, so it still lights nothing itself; what the user saw up close is something else (its weapons, the unit's own emissive shading).
- Census (`ren_DynLightsCensus 240`, 4 factions building): the build beams are `ok:beam` (beam_white_02: 125 of 240 frames; beam_disruptor), so they were lights, but at power 7 below the Normal class. Measured light of the beams (difference of the frame with lights on and off, zooms 40 / 90 / 180, mean over the crop): old 0.057 / 0.016 / 0.033, then 0.051 / 0.028 / 0.023, 0.076 / 0.026 / 0.018 after the intensity change: the weak lights stay faint in absolute terms (the difference needs a tenfold amplification to be seen at zoom 90).
- `ren_DynLightsGain` (percent, default 100, 0 to 1000): scales the colour x intensity of every light of the frame after the collector (`BeginFrame`), so the grid and the field both see it. Measured on the build scene at zoom 40 (pixels that differ from the frame without lights by more than 2 levels): gain 0: 2,146 (the noise floor), 100: 2,995, 300: 4,228, 1000: 14,863; the mean difference saturates (0.25 / 0.26 / 0.32) because the light shaders soft-limit the sum. Console only; no entry in the options menu.

## Hull sources are per step, not per frame (2026-11-02, from a user screenshot)

The feeds run once per frame, the simulation steps 90 times a second, and the sources were applied at the first step of the frame (and, when a frame
had no step, stayed in the pending list until one came). So what a moving hull pushed per second was the frame rate: at 200 fps two frames of sources
landed in each step (foam of the three side sources and the wash added up to an opaque slab with straight edges, ridges along every lump track grew
thick), at 30 fps a wake was thin, and in a pause the list filled to 4096 and burst on the next frame. All calibration so far ran at 90 fps (one step
a frame: the featab log says "1 steps").

`water::SimSourceWeight()` = game time of the last frame / one step (0 when paused, 2 at game speed 2, 0.45 at 200 fps), clamped to 4. The hull feed
multiplies strength and foam by it and returns early at 0; shells and deaths are one-off events and are not weighed. Not yet seen in a game: built as
`wave19` (not deployed: the user's game was running). Check: the same ship at 60 and 240 fps (`FEATAB` with different `-Width`/vsync, or `ren_WaterSimLog`
peaks) must leave the same wake.
