# Dynamic lights

Branch `dynamic-lights`. Status: **stages 1-5 built: light pool, textures, shader patch, colour from effect textures, beam / trail / glow / mesh / Lua feeds; lights are visible on D3D12 (see "Round 4" at the end for what is measured and what is not)** (snapshot 26/10/2026).

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
| 3 | Source patch: inject `DynLight` into `mesh.fx` and `terrain.fx` lit techniques (done at effect load, not in the translator, see below) | **done 26/10/2026**: both effects patch (19 + 33 techniques), the light shaders compile on D3D12, a test flash tints units and ground; strength not calibrated |
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

## Stage 3 as built

- **Where:** the patch runs on the effect text in `CD3DEffect::InitEffectFromFile` (`LightEffectPatch.{h,cpp}`), not inside the translator as first planned: it is backend-neutral text work on the game's own source, so one hook serves D3D11, D3D12 and Vulkan, and the generator it ports (`make_lab_shaders.py`) was written against that text. Only on the Direct3D 10 dialect backends and only with `/dynlights` on the command line; a source it does not recognise compiles unchanged (the log says why). The patched effect has its own cache name.
- **mesh.fx:** the view distance is stored in an unused interpolator by the vertex shaders of the lit techniques (`depth.y`, `texcoord0.z`), `WreckagePS` stops reading `depth.y`, and 19 single-pass techniques get `pass PDynLight` (additive, same vertex shader, the pixel shaders `DynLightsPS` / `DynLightsVertexNormalPS`). The world position is `dynLightCamera - viewDir * distance`; `LightBinding::SetCameraPosition` sets `dynLightCamera` per view.
- **terrain.fx:** 33 composite techniques get a last pass (dest * (1 + light)); `CalculateLighting` adds the lights to the sun term, so decals and splats are lit too.
- **Pass control:** the techniques carry `int dynLightPasses = 1;`; `ProgramEffectTechnique::BeginTechnique` (D3D12, Vulkan) drops that pass from the count while `gpg::gal::gDynLightPassActive` is false (no light this frame, or quality Off). D3D11 draws it always (an empty grid then adds nothing).
- **Host test:** `FAF_STOCK_FX_DIR=<folder with FAF's mesh.fx and terrain.fx>` (extract them from `gamedata/effects.nx2`) enables the stock-file checks (19 and 33 techniques, balanced braces, refused when changed or already patched).
- **Diagnostics:** `/dynlightsflood` paints the light passes magenta (shows they run: they do, on terrain and meshes); `/dynlightsflood2` paints the world position as a 20-unit checker (shows the terrain pass gets world coordinates). `ren_DynLightsTest N` drops a test flash (size 18, 6 units above the ground) in the middle of the view every N ticks. `FAF_GAL_DUMP_DIR` + `FAF_GAL_DUMP_SECONDS` write the back buffer as BMP for comparison.

### Verification (D3D12, RX 7900 XTX, 1600x900 windowed, `/dynlights`, adaptive_acid_lakes)

- Light passes run on terrain and meshes (magenta flood); lights off (`ren_DynLights=0`) restores the unpatched brightness (control frame).
- With the test flash the ground around the commander brightens by about 15-23 % with a warm tint (ratio to the lights-off frame: R 1.15-1.23, G 1.10-1.16, B 1.04). The grid and data textures read back correctly in the shader.
- Weak on purpose for now: the soft limit, the wrapped diffuse and the weakening factor of the patch's model are kept as they are; calibration against the patch is stage 5/6 work.
- **Window size trap:** `/windowed 1280 720` makes the D3D12 back-buffer copy fail by one row (the lights are not involved); use another size.
- **Not done:** D3D11 and Vulkan runs; the debug grid view (`ren_DynLightsView`) drew nothing in the zoomed-in test (its in-front test is unverified); real explosions on the screen (only the synthetic flash was seen).

## Round 4: sources, colour, Lua lights, limits (26/10/2026)

### Limits (the first patch had 16 lights on units and 28 on the ground)

| | now | where |
|---|---|---|
| lights per frame | 4096 (data texture 1024 x 12 RGBA32F, 3 texels a light) | `kMaxGridLights`, `LightGrid.h` |
| lights in the pool | 16384 | `LightCollector::kPoolCapacity` |
| lights one pixel adds up | 16 (the strongest 16 of its 16-unit grid cell) | `kLightsPerCell`; the grid texture is 4 texels per cell |
| kinds | Flash, Glow, Beam, Trail, Mesh, **Spot** | `Kind` |

Beams and trails are capsules (a segment with a radius) and are entered in every cell the segment reaches; a spotlight is a point with a direction and a cone (texel C holds the direction and the cosine of the outer half angle). Host test: 4096 lights in the data texture and the grid, 10000 lights in the pool, 40 lights in one frame in game.

### Where the colour comes from

The engine keeps no pixels of its textures, so `LightTexture.cpp` reads the DDS file itself (DXT1/3/5 and uncompressed 16/24/32 bit, the first mip that fits 256 x 256; `LightTextureCache.cpp` finds the file the way `InitEffectFromFile` does and caches the result per path). A light particle, a glow sprite, a trail or a beam takes the **hue of its texture** (the brightness-weighted mean colour of the texels, brightest channel = 1) times the **ramp texture at the particle age** (four points over the life, alpha included, so a ramp that fades also fades the light). Checked against an independent Pillow decode of `glow_03.dds` and five ramps (`ramp_blue_01`: (0.71 0.71 1.0) at a third of the life, (0.27 0.28 0.91) at two thirds; `ramp_flare_01` white; `ramp_disintegrator_01` black: no light).

| source | feed | colour | strength |
|---|---|---|---|
| `CreateLightParticle` flash | `FeedParticleBuffer` | texture hue x ramp curve | `FlashRadius/Intensity(size)` (the first patch's model) |
| additive / premodulated sprites (fire, plasma) | same, merged per 4-unit cell, tracked | texture hue x ramp | `GlowRadius/Energy(size)`, sprites of a cell add as energy; only textures with a glowing texel (alpha x luminance >= 0.3) |
| poly trails | same, tracked per head cell | texture hue x ramp head | `TrailRadius/Intensity(width)` |
| beams | same, tracked per start/end/texture | texture hue x mean vertex colour | `TrailRadius/Intensity(width)` |
| unit meshes | `FeedMeshes` after `MeshRenderer::Batch`, tracked per instance | glow colour of the albedo, army colour where the army colour glows | glow area of the albedo / specular maps by the pixel shader rule of the faction (`LightMeshEmission.h`) |
| Lua lights | `SWorldLight` lane of the sim particle buffer | what the script gave | what the script gave |

Not done: **Glow decals** (type `Glow`, the decal draws only into the glow alpha channel, it has no colour); emissive **projectile meshes** go through the same mesh feed but only if their shader is one of the unit shaders (the laser bolt meshes are not); **ramp rows** other than the first for the mesh-less effects (`rampSelection` is used as the row for particles, 0 for trails).

### Lua lights (`data/fafengine/lua/dynlights/light.lua`)

```lua
local Light = import('/fafengine/lua/dynlights/light.lua')
local lamp = Light.Create(unit, 'Turret', 1.0, 0.8, 0.5, 30, 1.0)   -- entity (nil: world position), bone (name, number, nil), r, g, b, radius, intensity
lamp:SetOffset(0, 1.5, 0); lamp:SetColour(r, g, b); lamp:SetRadius(50); lamp:SetIntensity(1.5)
lamp:SetSpot(0, -0.1, 1, 35)        -- spotlight along a bone-space direction, 35 degrees half angle
lamp:SetPosition(x, y, z)           -- a light without an entity
lamp:Destroy()
Light.Headlight(unit)               -- a warm 32 degree spotlight at the front
```

Engine side: the Lua globals `CreateLight`, `SetLight`, `SetLightOffset`, `SetLightPosition`, `SetLightSpot`, `DestroyLight` (in `EffectLuaStartupRegistrations.cpp`) call the registry `SimLights.cpp`; every sim tick (after the effects) it writes one `SWorldLight` record per live light into `SParticleBuffer::mLights` (the buffer grew from 0x30 to 0x40 bytes, a lane the shipped binary does not have); the renderer tracks the lights by handle (`LightCollector::Track`: fades in once, follows, fades out 3 ticks after the last record). **Determinism:** nothing reads the lights back, they are not in the checksum, and the functions always return a handle whatever the quality level or camera, so scripts take the same branches on every client. A light ends with its entity. Lights are **not saved** with a game. Note `CreateUnitHPR` takes (pitch, yaw, roll) in that order, whatever its name says.

### In-game tests (`tests/lights/ingame/`)

`dynlights_test.lua` (phases: static colours and an intensity ladder, light particles with five ramps, two beams, a bone light and a headlight on the commander, 40 lights at once) and `dynlights_test_units.lua` (five units with different emission rules, a tank with a headlight). Copy one as `<exe folder>/fafengine/lua/test/dynlights_test.lua`; the hook in `data/fafengine/lua/simInit.lua` runs it after the session began (on a normal install the file is absent and nothing happens). Launch with `/dynlights /d3d12 /windowed 1600 900 /map ... /con "ren_DynLights=4;ren_DynLightsLog=120"` and `FAF_GAL_DUMP_DIR` / `FAF_GAL_DUMP_SECONDS=3` for frames. `analyze.py` and `probe.py` compare a frame with quiet frames of the same scene: L = on / quiet - 1 per channel is the light the shader added, independent of the ground colour.

### Measured (D3D12, RX 7900 XTX, 1600 x 900)

- Static lights, radius 7, 4 units above the ground: red, green and blue give disk-mean L (0.268 0.020 0.029), (0.008 0.242 0.013) and (0.003 0.004 0.256): pure hues. A light of intensity 1 at that height should add 0.34 at its centre by the shader formula (`c / (1 + c)`, `c = colour x intensity x a x (0.35 + 0.65 a)`, `a = 1 - (h / r)^2`) and about 0.27 averaged over the central 4 units: measured 0.27.
- White ladder 0.25 / 0.5 / 1.0: mean L 0.10 / 0.19 / 0.235 (expected 0.09 / 0.165 / 0.27).
- 40 lights at once: smooth coloured patches over the whole view.
- Lua bone light on the commander (blue (0.3 0.6 1.0), radius 14): disk mean L (0.27 0.38 0.61), hue (0.44 0.63 1.0) with other lights present; it follows the commander and ends with `Destroy`.
- Headlight on a tank (warm (1.0 0.93 0.75), cone 32, radius 40): on the axis mean L (0.34 0.33 0.32) at 10 units, falling with distance; to the side (0.14-0.2); the hue on the axis is the light's (1.0 0.97 0.93). Other lights in the scene (a Seraphim power plant next to the tank) lit the area behind it, so the cone is not cleanly separated.
- Unit emission read from the textures: UEF T3 power plant glow area 0.056, hue (1.0 0.70 0.32) (the orange reactor), Aeon T3 power plant area 0.023, 80 % army colour, Seraphim T3 power area 0.189, Aeon Harbinger area 0.011, Cybran PD and the UEF tank no glow (their specular blue channel is empty), wreckage and props none. Five mesh lights per frame in the scene. The ground light of the units was **not** measured separately (the units stand in the frame, the quiet frames show the ground without them, and the units themselves differ).
- Not measured: the strength of beam lights, the light particles with pale ramps (their ramp alpha is 0.1-0.3, the lights are weak by design), emissive projectiles.

### Traps found

- `DISK_MemoryMapFile` maps a *resolved* name: resolve the virtual path with `FWaitHandleSet::FindFile` first, or every read returns 0 bytes.
- The shader divides by the data texture's real height (12 rows), not by the rows in use.
- `MeshBatchKey::mLod` is all bits set for an instance too far for every LOD, and some LODs carry a `lodBlueprintCopy` that is not a blueprint (shields, engine-made LODs): both are guarded, the names are read under structured exception handling.
- An imported Moho Lua module is its own global table: define `function Create` at module level, not a `local` table returned at the end.
- `/windowed 1280 720` makes the D3D12 back-buffer copy fail by a row (existing, not the lights).

## Round 5: frame-rate lights, colour, beams, cost (2026-10-26)

Review of the first patch's problems that the rewrite had inherited: lights stepped with the tick, strengths were uneven, beams lit nothing, colours were pale, constant sources blinked.

- **Frame rate.** A tracked light (beam, trail, mesh, Lua light) keeps the velocity of its last two refreshes (or the owner's, `Emission::hasVel`) and is carried along it between two refreshes, at most `kMaxCarryTicks` (2); a jump over 40 units counts as a teleport. Beams are worked out for every frame with the beam renderer's own interpolation (`EmitInterpolatedBeamQuadVertices(beam, frameAlpha)`, `FeedBeamsForFrame` from `BeginFrame`).
- **Beams were lit at (0,0,0).** The beat record keeps `SWorldBeam::mStart/mEnd` as *offsets* in the frame of the two end transforms; the world endpoints only exist after the renderer's interpolation. Reading the fields directly put every beam light at the origin (seen in the log). Fixed as above; a beam's strength comes from its own model (`BeamRadius`, `BeamIntensity`, floor 0.55 of the model), not from the poly-trail model.
- **Constant sources.** Glow sprites are lights of their own (born with the sprite, its motion, its ramp, dead with it) instead of one tracked light per cell with a hold of 3 ticks, which blinked for emitters that spawn less often than that. Sprites born in the same beat, in a 3 unit cell, with the same texture and life bucket are one light (energy adds as the root of the sum of squares). Beams and trails keep one identity over the ticks (`Follow`: same texture, the beam's start or the trail's old end within a few units) so a moving beam is one light, not a new one per cell.
- **Colour.** The colour of a light is the texture's average colour (alpha weighted over all texels) with the white taken out (`ReduceWhite`, 0.75 for the tint, 0.6 for tint x ramp colour), brightest channel 1. The ramp's alpha only sets the brightness (alpha^0.6). Unit glow colours lose half of their white.
- **Strength.** Glow sprite energy is `0.06 size + 0.04` (halved: sprites overlap now), scaled by the texture's mean brightness (`0.3 + 2 b`, 0.3..1). Lights of radius > 80 are capped at the strongest 8.
- **Cost.** Measured (RX 7900 XTX, D3D12, 1600x900, staged battle with an AI nuke, camera zoom 28, `FAF_GPU_PROFILE=600`): lights on 1.95 ms GPU per frame, off 1.94 ms; Terrain pass 0.30 against 0.26 ms. The first version of this round cost +0.4 ms (Terrain 0.46, peak 1.0). Changes: grid cells 8 units instead of 16 (up to 128 x 128), the per-pixel loop is written as unrolled quads that stop at the first empty one, the colour texel is fetched only by lights that reach the pixel (a spotlight is stored with a negative radius), decals and splats (`CalculateLighting`, run several times per pixel) look at the 8 strongest lights only. The nuke itself is expensive without any light: the Effects (particle) pass is 2.6 ms of 3.5 in the lights-off run.

### Strength tuning (second pass of round 5)

`ren_DynLightsLog=N` now also prints one line per light kind (count, mean and strongest colour x intensity at the centre, mean radius, flux = brightest channel x radius^2), see `scripts` in the scratchpad `kinds.py` idea: sum them over a battle. Staged battle, ultra quality, before the retune: flash peak mean 0.08 at radius 91 (an explosion flash was too wide and too faint to light its neighbours), glow 0.027 at radius 6 (a lone sprite invisible), beam 0.43-0.55, trail 0.14-0.27. Retune: flash radius `1.4 size` (max 420), intensity `0.55 + 0.004 size` (max 1.4), class threshold `kPowerLarge` 400; glow energy `0.09 size + 0.06`. After: flash peak mean 0.09 at radius 62, strongest 0.35; glow strongest 0.30; beams and trails unchanged. Strength is still a judgement against the frames, not a calibration against the effect brightness.

### Missed effects (third pass of round 5)

A census scene with structures of all four factions, engineers building, shield hits and a nuke (`tests/lights/ingame/dynlights_census_structures.lua`) plus the battle scene found:

- **The texture tint was not a colour.** `TextureColour::tint` kept the brightness of the average colour (a spark is a black quad with one bright spot: 0.03) instead of being brought to a brightest channel of 1, so every colour that used it (sparks, flares, electricity, beams, trails, ramps) came out 3-30 times too dark and 49 000 sprites were dropped as "ramp is dark over the whole life". `Summarise` now normalises it. This was most of "colours too weak" and "sparks give no light".
- **Alpha-blended flames.** Only additive and premultiplied sprites (blend 3 and 4) were lights. A flame or a nuke cloud drawn with alpha blending (blend 0) with a *hot* ramp (at some age bright, coloured, not transparent; smoke and dust ramps are grey or brown) is now a light at half strength; blends 1 and 2 (modulate inverse: they darken) stay out. `ok:hot alpha sprite` in the census.
- **Sprite strength.** Energy `0.05 + 0.12 sqrt(size)`, radius `2.4 size` up to 40 (a nuke's cloud sprites are tens of units across), gain `0.15 + 0.5 peak + 1.5 mean brightness` (a spark is high peak and low mean).
- **Trails were one round light at the head.** A trail record is one segment per tick; the light followed that segment only. A followed trail now keeps the path of its head and the light is three capsules over `0.7 x` the trail's drawn length (at most 20 ticks), at 1.0 / 0.7 / 0.45 strength from the head backwards; trail intensity `0.40 + 0.2 size` (max 0.7), radius `5 + 5 size`.
- **Unit and structure lights** follow the PBR shaders of `mesh.fx`: the glow of a unit is `specular.b` (the bloom mask), so a Cybran structure whose specular blue channel is empty (most of them) really has no glow; it is not a missing rule. Shields and adjacency nodes are effects, not lights.
- **Cost.** In the structure scene (67 structures, nuke, up to 3000 glow lights) lights on are 2.45 ms GPU against 2.28 off (it was 3.49 before: lights below colour x intensity 0.035 are dropped, and the water reflection pass no longer evaluates lights).

### Mesh lights and effect meshes (fourth pass of round 5, 2026-10-26)

- **Mesh lights were all off.** `MeshInstance::dissolve` is 1 for a solid mesh and 0 for one that has dissolved away; the feed used `1 - dissolve`, so every normal unit and structure got zero intensity (all 20 mesh lights of a frame were dropped as too dim; the per-kind log never listed `mesh`). Fixed: intensity x dissolve. Units and structures with a glow (specular blue channel, Seraphim bright albedo, Aeon team glow) now light their surroundings, at most 0.55 (was 0.8) with radius 1.5 x body radius + 1.
- **Effect meshes.** The fireball meshes of strategic missiles (`NukeUEF`, `NukeEMP`, `NukeQuantum`: procedural fire) burn as a whole in the colour of their albedo, up to 1.2 and a radius up to 150; the flash of a hit on a shield (`ShieldImpact`, `CybranShieldImpact`) is a half-strength light; a unit being built (shaders with "build" in the name) glows faintly in the faction's build colour (Aeon green, UEF blue, Cybran orange, Seraphim violet).
- **Cost.** 8 entries per 8-unit cell instead of 16 (a dense 67-structure base with a nuke cost 1.4 ms more GPU than with lights off at 1600x900, 4.59 against 3.20 ms; with 16 entries and the dissolve bug fixed it was about 2.5 ms more); the mesh pass is most of it because every mesh is drawn a second time with the light pass. Levels below High do not light meshes at all (their power is below the Normal class), so Low and Medium stay cheap.

### Muzzle flashes and cell-border steps (2026-10-28)

- **Muzzle flashes lit nothing.** A muzzle flash is a small additive sprite (size 0.3 to 1) that lives a tick or two. The glow model gave it radius 3 and an energy of about 0.1 (`GlowRadius` floor 3, `GlowEnergy(0.6) = 0.14`, times the texture gain), so a Cybran point defence fired in the dark with the ground around it unlit. Measured in a staged fight (`tests/featab`, `-Scene fire`, `ren_DynLightsLog 30`): the glow lights had radius 3.0 and a strongest colour x intensity of 0.34. A glow sprite of at most 5 ticks and size 0.25 or more is now a flash (`kFlashLifeTicks`, `kFlashMinSize`) and gets 4 times the energy; the glow radius is `2.4 x size + 2.5` with a floor of 5. Same fight after: radius 5.0, strongest 1.08. Not changed: blend mode 1 sprites (`flare_lens_add`, `sparkle_white_add` with a `premod` ramp) stay without light, the engine draws mode 1 as `_MODULATEINVERSE` (`Zero, InvSrcColor`), which darkens.
- **Squares and lines in the lit ground.** A grid cell lists at most 8 lights. The fight above has 120 lights on screen and drops 36 to 40 (light, cell) pairs per frame on average, 450 at most (trails are 3 capsules each, 90 trail lights at once). A light that is listed in one cell and dropped from the next ends at the cell border: an 8-unit step in the lit ground. The cell kept the 8 strongest lights by their power over the whole map, so a strong light that only grazed the cell pushed out a weak one right on top of it. A cell now keeps the 8 lights that add the most at its middle (the shader's windowed falloff at the closest point of the light's segment); the order inside the cell is still the ranking order. What is dropped is the least visible there, so the step is small. The capacity stays 8 (16 cost 1.2 ms in a dense base). Host test `TestCellKeepsWhatAddsMost`. Not verified in a picture: the steps themselves are hard to catch (the beat-driven lights need the game running, a paused frame shows no light difference), only the counts and the host test.

### Cell squares, second round (2026-10-28, evening)

- **The squares were still there** after the cell-ranking change. Screenshots from a game (a Cybran spider bot firing a beam, dynamic lights Ultra) show lit blocks with straight edges at the muzzle and around the beam impact, about one 8-unit cell wide. Reproduced with `tests/featab` (`-Scene beam1`, one `url0402` firing at one immortal tank, camera at the user's zoom, the whole profile on): the build with 8 entries per 8-unit cell shows a lighter block with straight vertical edges right at the muzzle; the same scene on the build below is smooth. At a much closer zoom (11) the old build showed nothing: the squares are as large as the screen there. The staged 37-unit fight above is not the right test for this (its overflow is a stack of trails on the same targets); a single beam is.
- **Now:** 16 entries per cell (`kLightsPerCell`), four texels per cell, cells down to 4 units (`kMinCellSize`: a 10 km map has 128 cells of 4, a 20 km map 128 of 8, larger maps cells of 16 and 32). The shader fetches a texel only when the one before it is full (`DynLightCell`), so a cell with a few lights still costs one fetch. The fight with 37 shooters still drops 35 (light, cell) pairs per frame on average (40 before) and 591 at most: it is a stress test, not a target. Cost with the new layout was not measured on a dense base yet (the 16 entry version cost 1.2 ms there once).
- **Wrong earlier:** "the capacity stays 8" and "the squares are fixed by keeping what adds most": the ranking was a good change but did not remove the squares.
