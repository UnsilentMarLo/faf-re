# Dynamic lights

Branch `dynamic-lights`. Status: **stages 1 and 2 done: light pool, TLight feed, light textures and their binding exist; nothing draws the lights yet** (snapshot 26/10/2026).

Dynamic lighting for explosions, muzzle flashes, beams, trails, projectiles and glowing units, built into the engine's renderer instead of patched into the shipped exe. It replaces the binary patch [FA-DynamicLights](https://github.com/UnsilentMarLo/FA-DynamicLights) (v0.1.0) on the Direct3D 11, 12 and Vulkan backends. Direct3D 9 keeps using the patch. The lights are **render only**: they read effect data and never touch the simulation, so replay parity is unaffected.

## What the patch could not do, and why the engine can

| Patch limit | Cause | In the engine |
|---|---|---|
| 16 lights on units, 28 on terrain | `ps_2_a` has 32 constant registers | The FAF effect layer compiles `ps_6_0`; its limits are the API's (64 KB `$Globals`, 64 SRV slots), not 32 registers |
| Mesh drawn 1+3 times, terrain 1+4 | One light array per additive pass | One pass: the shader reads a light texture and a light grid |
| Colour inferred from pixels of ramps and textures | The patch only sees the draw calls | The engine has the particle, beam and trail records and the effect blueprints |
| Unit lights from a table in the exe | No access to the unit's data | The collector can read mesh blueprints and the emissive masks when a mesh loads |
| Hooks into vtables by address | Binary patching | Plain code in `WRenViewport::Render` and the renderers |
| Lights go through walls | One sun shadow map | The terrain height map is a texture the terrain shader already owns: a light ray can be marched against it (stage 6) |

## Facts the design rests on (verified in source, 26/10/2026)

**Sources of lights**

- `CEffectManagerImpl::CreateLightParticle` (`moho/effects/rendering/CEffectManagerImpl.cpp`) emits a particle tagged `"TLight"`, blend mode 3, constant size, no velocity. This is the one place where "this is a light" is explicit. `RenderParticleBucket(..., onlyTLight)` already filters on the tag.
- `SWorldParticle` (`mPos`, `mInterop` age, `mLifetime`, `mBeginSize`, `mEndSize`, `mRampSelection`, `mTexture`, `mRampTexture`), `SWorldTrail` (`mStartPos`, `mEndPos`, ages, `mSize`) and `SWorldBeam` (`mStart`, `mEnd`, `mWidth`, `mStartColor`, `mEndColor`) are filled by `CWorldParticles::AddParticles` (once per sync beat, called from `CWldSession`). Only beams carry a CPU colour; particle colour is a ramp texture sampled on the GPU.
- `MeshRenderer::Batch` (`moho/mesh/Mesh.cpp`) rebuilds the frustum-culled mesh instances every frame (`MeshInstance`: `interpolatedPosition`, `sphere`, `scale`, `color`, blueprint). Emissive parts are not flagged; they come from blueprint-keyed profiles.

**Frame order** (`WRenViewport::Render`, `moho/app/WxRuntimeTypes.cpp`): sky, terrain normals, shadows, **terrain composite**, `MeshRenderer::Batch`, reflections, meshes, effects, water, UI, bloom. The terrain is composited before the meshes are batched, so **lights must be collected and uploaded before `RenderCompositeTerrain`**.

**Render targets**: colour is A8R8G8B8 (LDR, alpha is the glow mask), depth is D24S8 and **cannot be sampled** (no depth setter on `EffectVariable`, no sampled-depth flag at creation). There is no G-buffer. So this is a **forward** design; world position comes from the vertex stage (meshes) and the terrain's own world coordinates, as in the patch.

**Binding**: a shader parameter is a `ShaderVar` (`RegisterShaderVar`, set per pass in `MeshRenderer::ConfigureShader` and `*FidelityTerrain::LoadTerrainLighting`). `EffectVariable` has `SetTexture`, `SetVector`, `SetVectorArray`, `SetValue` and matrices but **no buffer setter**. A structured buffer would need a new root parameter, a new `EffectVariable` method and Vulkan changes. A dynamic `Float4` texture needs none of that, and `BonePaletteTexture` (`moho/mesh/HardwareMeshBatch.cpp`) is the precedent (Lock Discard, write, Unlock, bound with `RegisterShaderVar`). D3D12 has 64 SRV slots, so a free slot exists.

**Shader injection**: the game's `.fx` files are user data and are never copied into the repo. They already pass through `D3DPreprocess` and the shared `Translator::Run()` (`gpg/gal/backends/d3d11/EffectSourceD3D11.cpp`) on D3D11, D3D12 and Vulkan. A new translator step next to `WrapAlphaTest` can prepend the light declarations and function and wrap the lit pixel shaders. Bump `kTranslatorVersion` when it changes; the D3D12/Vulkan shader cache key hashes the translated source.

## Architecture

```
src/moho/render/lights/
  LightModel.h        importance classes and quality gate (from the patch's LightModel.h)
  LightCollector.*    feeds -> Emission records -> pool/envelopes -> selection (CPU, render thread)
  LightFeeds.*        TLight particles, glow sprites, beams, trails, projectile and unit meshes
  LightProfiles.*     blueprint-keyed emissive data (compiled defaults + data/fafengine/lights/*.lua)
  LightGrid.*         builds the two textures below
  LightBinding.*      owns the textures, binds them to the mesh and terrain ShaderVars
src/gpg/gal/backends/d3d11/EffectLightPass.cpp   translator step that injects the HLSL
data/fafengine/lua/options/options.lua           Options > Video > Dynamic Lights
```

### Textures (per frame, dynamic `Float4`)

1. **Light data**, 2 texels per light: `(pos.xyz, radius)` and `(rgb * intensity, kind | length)`. A beam or trail is a **capsule** (second end point in a third texel when `kind` says so), so a beam lights along its length instead of as a point. 1024 lights = 3072 texels, one row of a 4096 x 1 texture.
2. **Light grid**: a world-space XZ grid covering the camera's view of the ground (about 64 x 64 cells), each cell two texels = 8 light indices (`Float4`s carry exact integers). The collector bins each light into the cells its radius touches, strongest first. The shader reads one cell and loops over at most 8 lights. This is a clustered/tiled forward scheme with no depth needed, and the light count is limited by the pool size, not by registers.

One light pass per surface replaces the patch's 3 + 4 extra passes. With zero lights the shader branch is skipped (a constant flag), so "Off" and "no light on screen" cost the stock game's price.

### Shader side

`DynLight(worldPos, normal, albedo, spec params)` returns the added radiance. Per light: smooth-windowed inverse-square falloff to the radius, wrapped Lambert diffuse, GGX specular with the material's roughness where the lit technique has one, soft limit (and white bleed on over-exposure) against the LDR target. Terrain uses the normal map the terrain shaders already evaluate. The function is added to the lit pixel shaders by the translator wrapper; the wrapper needs the world position and normal the shader already computes, so the first job of stage 3 is to read the real `mesh.fx` / `terrain.fx` pixel structs and decide which techniques to wrap (the patch's generated `content/effects/*.fx` list the techniques it modified).

### Quality levels

Off / Low / Medium / High / Ultra with the patch's importance classes (power = intensity x radius^2: huge >= 8000, large >= 500, normal >= 6, minor), option `dynamic_lights` (default Ultra) in Options > Video, and the cvar `ren_DynLights` (0-4) for tests. Low and Medium skip the beam, trail and mesh feeds.

## Stages

| # | What | Done when |
|---|---|---|
| 0 | Branch, toolchain, x64 Release build | **done 26/10/2026** (`main_x64.exe` links) |
| 1 | `LightModel.h` and `LightCollector` with the `TLight` feed only, a host test (no GPU) for classes, pool, selection | **done 26/10/2026**: `faf_light_tests` passes (4240 checks); `ren_DynLights` (0-4, default 0) and `ren_DynLightsLog N` |
| 2 | `LightBinding` + the two textures, filled and bound on D3D12; a debug cvar draws the grid; check `Float4` `Lock`/`Unlock` on D3D11/D3D12/Vulkan (never exercised by `BonePaletteTexture`) | **built 26/10/2026**: CPU image tested (37078 checks); both textures create on D3D12 without error; flashes, upload and the debug view not yet seen in game (see below) |
| 3 | Translator step: inject `DynLight` into `mesh.fx` and `terrain.fx` lit techniques | explosions light units and ground on D3D12 |
| 4 | Option, cvar, `docs/SWITCHES.md` entries, `data/fafengine` hook | the option works after a restart |
| 5 | Remaining feeds: glow sprites, beams (capsules), trails, projectile and unit meshes, profiles for mods | parity with the patch's feeds |
| 6 | Improvements beyond the patch: terrain height-map occlusion, glow written into the alpha mask for bloom, flicker-free envelopes without ghost lights | lights stop at ridges |
| 7 | D3D11 and Vulkan verified; D3D9 left to the patch | all three modern backends pass the same screenshot A/B |

## Open questions

- **Colour of ramp particles.** The CPU holds texture handles only. Options: read the ramp texel from the decode cache when the texture loads (cheap, once per texture), or read the colour from a per-effect table. To check in stage 1.
- **Thread that owns `AddWorldParticle`.** Not verified; the collector must run on the render thread and copy what it needs.
- **Float4 dynamic textures on D3D11/12/Vulkan.** Implemented in the backends but never exercised by a real consumer.
- **Wrapping rule for pixel shaders.** The pixel input structs differ per technique; the translator step must find world position and normal per technique, or the shaders must be wrapped by name from a table.
- **Time of the first frame after a shader change.** The shader cache is hashed per source, so lit techniques recompile once per change.

## Stage 1 as built

- `src/moho/render/lights/LightModel.h`: importance classes, quality gate and the flash / glow / trail / weaken formulas of the patch, as `constexpr` functions in `moho::lights::model`. No engine includes.
- `LightCollector.{h,cpp}`: `Add(Emission)` validates and gates by quality, `Update(nowTick, maxLights, out)` prunes, applies the fade-in/out envelope (0.08 s / 0.30 s, scaled by `SetSecondsPerTick`), moves the light with the particle shader's rule (`pos + dir*t + 0.5*accel*t^2`), follows the size ramp, and returns the strongest `maxLights` (ties by birth, then arrival). A light's importance class is fixed at `Add` from its peak size, so it does not change while it fades. Pool of 4096. No engine includes.
- `LightFeeds.{h,cpp}`: `FeedParticleBuffer` is called from `CWldSession` right after `sWorldParticles.AddParticles` and picks the `"TLight"` particles (birth = beat tick + fractional `mInterop`); `BeginFrame(tick, alpha)` is called from `WRenViewport::Render` before the terrain composite. Colour is the patch's warm default (1.0, 0.84, 0.63) until the ramp texture can be read on the CPU (stage 5).
- Tests: `cmake --build --preset clang-x64-release --target faf_light_tests`, then `build/clang-x64-release/tests/faf_light_tests.exe`; also `ctest` (`dynamic_lights`).
- Trying it: start the game with the console command `ren_DynLights 4` and `ren_DynLightsLog 60`; the log shows `DynLights: ... lights=N` once per 60 frames. Nothing is drawn yet. Not yet run in game.

## Stage 2 as built

- `LightGrid.{h,cpp}` (pure, tested): `GridLayout::ForMap` (whole map, at most 64 x 64 cells of at least 16 units), `BuildLightGrid` (data texture: 3 RGBA32F texels per light; grid texture: 2 texels = 8 `index + 1` entries per cell, strongest first, `0` = empty), `LightGridImage::CellLights` (the lookup the shader will do).
- `LightBinding.{h,cpp}`: two dynamic `Float4` textures (`Lock(Discard)` / `Unlock`, as `BonePaletteTexture`), re-created when the map size changes, released with the mesh batch's shared buffers on a device reset. Binds `dynLightData`, `dynLightGrid`, `dynLightParams`, `dynLightGridSize`, `dynLightDataSize` to the `mesh` and `terrain` effects when they declare them (they do not yet).
- `LightDebugView.{h,cpp}`: `ren_DynLightsView 1` draws every lit grid cell on the ground (green 1 light .. red 8) and a marker per light, through the viewport prim batcher before the UI. The "in front of the camera" test uses the viewport matrix' depth row; unverified.
- `ren_DynLightsLog N` now also reports grid cells, overflow and texture uploads / failures / bound variables.

### Verification so far (D3D12, RX 7900 XTX, `ren_DynLights=4`)

- Both textures are created (`light data texture 1024x1`, `light grid texture 64x32`) with no error.
- **Open:** every run with `/d3d12 /windowed 1280 720` hits a D3D12 error at session start, with the lights **off** as well (so it is not this code): the debug layer reports `CopyTextureRegion: the destination region extends past ... SrcBox width 1280, height 721`, the command list then stays closed and the log fills with `DeviceD3D12.cpp(2424) HRESULT 0x80004005`. It looks like an off-by-one between the window's client height and the back buffer for that `/windowed` size; try another size or the profile's window mode before judging stage 2 in game.
- Not yet seen in game: flashes in the pool (`stored > 0`), a texture upload with lights, the debug view.
