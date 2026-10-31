# Lighting V2: lights without caps, shadows from every light, cost independent of the light count

Status: **design, 2026-10-31.** Replaces the per-pixel light loops of `docs/DYNAMIC_LIGHTS.md` (grid + shader loops) once it beats them in the 100v100 scene. Until then the old path stays as the fallback.

## Why

Measured (`FEATAB_COUNT=100 tests/featab -Scene battle`, D3D12, 1600x900, about 2,500 lights on screen, GPU ms per frame): lights off 4.6; lights on without shadows 14.2; with shadows 38.5 before the steam-re port, 132.8 after it. Each pixel loops over every light of its cell and, per light, over the shadow casters: cost = pixels x lights x casters, and it is paid again in the ground pass, every decal/splat layer and the second draw of every mesh. A cap (fewer lights, fewer casters) only trades looks for speed. The cost has to stop depending on the light count.

## Architecture: light pre-pass with irradiance texture

```
1  depth + normal pre-pass      terrain + meshes, depth-only techniques, into depth target + RGBA16F normal target
2  light buffer (GPU)           every light of the frame in one structured buffer, no CPU grid, no per-cell cap
3  tiled culling (compute)      screen tile x depth slice clusters, exact lists from an atomic pool
4  irradiance pass (full-screen pixel program or compute, half or full resolution)
                                per pixel: depth -> world position, normal -> irradiance of all lights of the pixel's cluster,
                                written as RGB irradiance + a dominant direction (first-order directional irradiance)
5  shadows                      stochastic light selection per pixel (a fixed number K of lights, weighted by contribution)
                                + K shadow rays against terrain + casters; temporal + spatial reuse, denoise
6  forward passes (unchanged materials)
                                terrain, decals, meshes read the irradiance texture once (a single tex2D at the pixel) and multiply
                                with their own albedo, normal map (against the dominant direction) and specular
```

- **Why a pre-pass and not a full G-buffer:** the material shaders keep every detail they have; they only stop looping over lights. No MRT in the lit shaders, no change to how albedo, normal maps and specular are made. With lights off the image is identical.
- **Why the cost is bounded:** step 4 costs pixels x (lights in a cluster after intensity culling), step 5 costs pixels x K, whatever the number of lights and casters; the forward passes cost one texture read each.
- **Particles, shields, water** stay forward and read the same irradiance texture at their pixel (they have depth in the same buffer).

## Stages (each ships only if it beats the previous path in the 100v100 scene and the old path stays selectable until the end)

| # | What | Done when |
|---|---|---|
| 0 | Baseline (done): the table above | recorded |
| 1 | Depth + normal pre-pass for terrain and meshes into targets of ours | the pre-pass costs under 1 ms in the 100v100 and the forward passes still draw the same image |
| 2 | Light buffer + irradiance pass without shadows; forward shaders sample it instead of looping; old grid path removed from the shaders when the option is on | 100v100 lights-on GPU time within 2 ms of lights-off; no squares, no cap |
| 3 | Shadows: stochastic light sampling + shadow rays (terrain heightfield march + caster list first, DXR 1.1 ray queries on D3D12 after), temporal reuse | 100v100 with full shadows within 3 ms of lights-off |
| 4 | Directional irradiance for normal-mapped surfaces, specular from the dominant direction, denoiser quality, D3D11 and Vulkan parity (ray queries where the API has them, shadow-mapped strongest lights elsewhere) | per-API A/B pictures |

## Open questions (answered by the code survey before stage 1 starts)

- Where in the frame the pre-pass can be inserted and whether its depth can be reused by the forward passes (early-z) or must be a copy.
- How the translated effect shaders get the screen position to sample the irradiance texture.
- Whether a pixel program can write the normal target together with depth (MRT) on all three APIs.
- Whether the mesh submission can be replayed with the depth-only techniques that the sun shadow pass or the `RenderDepth` gate already use.
