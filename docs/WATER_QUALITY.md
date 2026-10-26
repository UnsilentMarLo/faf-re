# Water quality: reflection and refraction

Render feature `water_quality` (Options > Video > Water Quality, console variable `ren_WaterQuality`). Render only: no game data, no sim state, no replay effect.

## How the shipped water works

The same code on every backend (`src/moho/app/WxRuntimeTypes.cpp`, `WRenViewport::Render`; the backends only create the targets and compile `water2.fx`):

| Step (frame order) | What happens |
|---|---|
| `RenderReflections` | Binds `mSecondaryTargetLocks[head]` as colour and the head's depth buffer, clears to 0 (alpha 0), and draws **every mesh bucket mirrored** (`MeshRenderer::Render(2, ...)`: the view matrix is reflected about the water plane, `MeshInstance::isReflected` is set on all instances). That is units, structures and props such as trees. **Not in it:** the terrain, particles and effects, the sky dome. The target is the **size of the head** (screen) and has no mip chain. |
| `RenderWaterMask` | `TWaterLayAlphaMask` and `TShoreline` write the land cut-out into the alpha of the frame. |
| `RenderCopyForRefraction` | Copies the frame so far (terrain, units, effects; alpha = land cut-out and units above the water) into `mPrimaryTargetLocks[head]`, the **refraction map**. Only at shader quality (`graphics_Fidelity`) 2. |
| `RenderWater` | `HighFidelityWater::RenderWaterSurface` selects `water2.fx` `TWater`. Fidelity 2 is `Water_HighFidelity` (`HighFidelityPS`); fidelity 0 and 1 are `LowFidelityWater` (a flat tint; it never reads the reflection). `ren_Water` hides the water, `ren_Reflection` skips the mirrored pass (the target is still cleared). |

`HighFidelityPS` (FAF's `water2.fx`): four normal map layers give the normal `N`; the sky is a cube map lookup with the reflected ray; **units come from the reflection map and the sea floor from the refraction map, both looked up with one bilinear tap at `screenPos - sqrt(depth) * refractionScale * N.xz`** (a screen-space approximation, the shader cannot know where the reflected ray hits); Schlick Fresnel mixes them; the sun specular and the wave crests are added; the alpha is `1 - land mask` and the pass is alpha tested (`NotEqual 0`), **not blended**, so the shoreline is a staircase of whole pixels.

Consequences that are visible: the finest normal layer repeats every 2 world units, so neighbouring pixels sample the reflection several texels apart and the reflected hull is speckled and stair-stepped; the reflection target has the same size as the screen, so there was nothing to gain from a bigger one *except* as supersampling for a filtered lookup; the terrain does not reflect.

## Levels

| Level | Reflection target | Reflection lookup | Refraction lookup | Shoreline |
|---|---|---|---|---|
| 0 Off (shipped) | screen size | 1 tap | 1 tap | binary |
| 1 Low | **half** screen size | 4 taps | 1 tap | binary |
| 2 Medium (default) | screen size | 4 taps | 4 taps | soft |
| 3 High | **1.5x** screen size | 8 taps | 4 taps | soft |

- **Footprint filter** (`src/moho/terrain/water/WaterEffectPatch.cpp`, a source patch of `water2.fx` applied when the effect is read, like the dynamic lights patch): the taps are spread over the screen-space derivative of the lookup position (`ddx`/`ddy` of the displaced uv, limited to 0.02), which is what a mip level or an anisotropic filter would do for a render target that cannot have mips. The reflection taps are **alpha weighted** (`sum(rgb * a) / sum(a)`), so a reflected edge fades into the sky instead of into the black clear colour. In the refraction taps a texel that holds a unit above the water or the land cut-out is replaced by the unrefracted background first, as the shipped shader does for its single tap.
- **Soft shoreline** (`FafShoreline`): a water pixel takes the colour of the land by the share of its four neighbours that are land (their alpha in the refraction map). The staircase becomes a one pixel gradient. The land pixels are not drawn, so the land is untouched.
- **Reflection target size** (`src/moho/terrain/water/WaterQuality.cpp`, hooks in `RenderReflections` / `RenderWater` / `D3DWindowOnDeviceExit`): the targets are created on demand with their own depth buffer and the viewport rectangle is scaled; the water samples the same normalised screen position, so the shader needs no change. Back to scale 1.0 releases them. 1.5x at 3840x2160 is about 150 MB (colour + depth).
- The parameters reach the effect as `faf_WaterFilter` (float3: reflection taps, refraction taps, shoreline blend), set each frame by `water::BindShaderParams`. Level changes apply at once (no restart); the effect is patched at load on the Direct3D 10 effect dialect backends whatever the level, and level 0 takes the shipped paths in the shader (one uniform branch).
- A source the patch does not recognise (FAF changed `water2.fx`) is compiled unpatched and a line says why (`Water: water2 not patched: ...`); a patched source the backend compiler rejects falls back to the unpatched effect.

## Support per API

| | D3D9 | D3D10 | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|---|
| Entry in Options > Video | yes | no (does not start) | yes | yes | yes |
| Reflection target size | yes (runs; not captured) | - | yes | yes | yes |
| Filtered lookups, soft shoreline | no: the shipped water is `ps_2_a`, the D3D9 effect compiler has no room for the taps and the patch is not applied | - | yes | yes | yes |
| Default | Off (Medium is the same picture there: scale 1.0) | - | Medium | Medium | Medium |

## Measured (2026-10-27, RX 7900 XTX, 1600x900 windowed, Seton's Clutch, `tests/water/ingame/`)

Scene: a fleet on the water and a few structures on the shore, a low free camera (the shipped camera cannot look flat enough for the reflection to show), game paused, so every level renders the same frame (the water animation is frozen). The level is switched live; a coloured square at the left edge of the dumps says which level a frame shows. **Pictures:** `docs/images/water_quality_*.png` (D3D12 and D3D11 reflection crops at 3x, Vulkan scene, D3D12 shoreline at 4x).

What the pictures show: the reflected hull is speckled and stair-stepped with the shipped single tap and smooth with the filter (mean absolute pixel gradient in the reflected hull region, lower = smoother: D3D12 6.78 / 6.20 / 6.36 / 6.58 for levels 0..3, D3D11 7.06 / 6.62 / 6.68 / 6.77, Vulkan 7.10 / 6.41 / 6.66 / 6.80). **The difference is real but subtle**: the reflection of a few ships is a faint ghost under the hull at any level. High keeps more detail than Low (finer target, 8 taps), so it is a little sharper, not smoother.

GPU cost per frame (`FAF_GPU_PROFILE=100`, the passes `Reflections` and `Water`, median of 25..50 profile blocks per level; other sessions were running games on the same GPU, so the noise is about +-0.03 ms and levels 0 and 2, which do the same reflection work, show it):

| Level | D3D12 Reflections | D3D12 Water | Vulkan Reflections | Vulkan Water |
|---|---|---|---|---|
| 0 Off | 0.153 / 0.156 | 0.148 / 0.161 | 0.074 | 0.102 |
| 1 Low | 0.084 / 0.108 | 0.134 / 0.150 | 0.068 | 0.108 |
| 2 Medium | 0.113 / 0.099 | 0.117 / 0.127 | 0.081 | 0.120 |
| 3 High | 0.159 / 0.104 | 0.126 / 0.144 | 0.097 | 0.124 |

(D3D12: two runs.) So the filter costs at most about 0.02 ms in the Water pass at this resolution and the 1.5x target is not measurable here: the pass is limited by the mirrored mesh draws (19 draws in this scene), not by pixels. In a large battle the mirrored pass costs what the vertex work of every visible unit costs, which the target size does not change; Low saves the pixel part only. D3D11 GPU timestamps are not usable (the profiler falls back to wall time), so there is no D3D11 number.

## Not done, and why

- **Terrain and sky in the reflection / screen-space reflections.** The mirrored pass draws meshes only, so land, hills and the shore never reflect. A mirrored terrain pass is a second terrain render (normals, splats, decals, shadows) per frame. A screen-space fallback needs the scene depth in the water shader: the water pass writes no depth but tests against the buffer it samples, which the GAL cannot bind read-only on all three backends without new plumbing (the particle branch added `EffectVariable::SetDepthStencil` and `Device::CopyDepthStencil` for its own reason; a water pass on top of it is the way to go, then ray-march the refraction map). Not attempted here.
- **Direct3D 9 shader filtering** (`ps_2_a`).
- **Refraction depth edges beyond the shoreline:** units in front of the water (above the surface) are already replaced by the unrefracted background by the shipped alpha trick; only its filtered form is new.

## Reproduce

```
copy tests\water\ingame\water_scene.lua   <exe folder>\fafengine\lua\test\dynlights_test.lua
copy tests\water\ingame\water_camera.lua  <exe folder>\fafengine\lua\test\dynlights_camera.lua
main_x64.exe /init init_faf.lua /d3d12 /windowed 1600 900 /nomovie /nosound /seed 7 /faction 1 ^
  /map /maps/setons_clutch_-_faf_version.v0004/setons_clutch_-_faf_version_scenario.lua
  with FAF_GAL_DUMP_DIR=<dir> FAF_GAL_DUMP_SECONDS=1 FAF_GPU_PROFILE=100
```

The scene file logs the spot it found (`WATER-TEST: spot X, Z ...`; the user interface has no terrain height functions, so the camera script has it as a constant: edit it for another map). The log lines `WATER-TEST: pose P mode N` and the profiler blocks are in order, which attributes every block to its level. Host test of the shader patch: `cmake --build --preset clang-x64-release --target faf_water_tests` and `FAF_STOCK_FX_DIR=<folder with FAF's water2.fx> ctest -R water_effect_patch`.
