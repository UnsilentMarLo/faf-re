# Performance

The rebuilt x64 engine plays the same games as retail `ForgedAlliance.exe`, bit for bit, and about **a quarter faster**.

*Snapshot: 24/10/2026.*

## Against retail

Five FAF vault replays, played one at a time. Retail runs D3D9. Ours runs x64 with every switch at its default, 8 sim workers and D3D12. Time is wall-clock seconds from game 0:00 to the end of the replay. Every run matched every checksum the players recorded.

| Replay | Players | Game length | Retail | Ours | Faster by |
|---|---|---|---|---|---|
| 26919967 | 12 | 46 min | 1675 s | 1125 s | **33 %** |
| 26928802 | 8 | 39 min | 1123 s | 855 s | **24 %** |
| 26935800 | 12 | 41 min | 1244 s | 1032 s | **17 %** |
| 26997072 | 10 | 23 min | 180 s | 151 s | **16 %** |
| 26923388 | 2 | 45 min | 189 s | 181 s | **4 %** |

| | Time saved | Speed |
|---|---|---|
| Least (small game, both near the game's speed cap) | 4 % | 1.04Ã— |
| Most | 33 % | 1.49Ã— |
| Average | 19 % | 1.25Ã— |
| All five together (4411 s â†’ 3344 s) | 24 % | 1.32Ã— |

The bigger the battle, the bigger the gain. In the heaviest stretch of the three large games (ticks 20,000â€“22,000) we are 23â€“38 % faster.

A few more details:

- **Without sim workers** our x64 is about 9 % faster than retail (26935800, D3D9).
- **D3D9 is faster than D3D12 in a replay at full speed**: 924 s against 1032 s on 26935800.
- **Memory**: we use 0.4â€“0.9 GB more than retail, because of 64-bit pointers and the D3D12 backend.
- **Scale**: x86 runs out of address space at about 60,000 units. x64 was still running at 90,000.
- **Multiplayer**: retail and our x64 played 30 minutes in one lobby without a desync, with and without sim workers.

## Where the speed comes from

- **Sim work on worker threads**: recon blips, entity coordinate updates, sync packets and the Lua garbage collector's mark and sweep. The collector alone went from 35.9 s to 9.3 s per game.
- **Less repeated work in the sim**: intel grids, recon blip refreshes, blueprint lookups, area queries and checksum hashing no longer redo work whose result is already known.
- **Pacing**: replays and single player compute a few beats ahead of the screen, on the original rate schedule.
- **Main thread on workers**: strategic icons, view culling, fog of war and applying each beat's unit updates.

## How it stays exact

The sim checksum covers every bit of game state, so a speed-up is only kept if it changes nothing:

- A step runs in parallel only when the result is provably the same as running it serially. Anything order-sensitive (Lua, random numbers, creating entities) stays on the sim thread, in the original order.
- The benchmark replay produces the same checksum on every one of its 24,565 beats with 0 and with 8 workers.
- 75 vault replays match every recorded checksum on the final build.
- Every change has a switch that brings back the original code. See [Switches](SWITCHES.md).

## What did not help

- **Lua interpreter**: about 140 CPU cycles per instruction, spent waiting on memory for table lookups. Two faster dispatch paths changed nothing, so they were dropped.
- **Running Lua callbacks in parallel**: only 2â€“7 % of them are independent. The rest allocate, use the random stream or call into the engine.
- **Reusing a unit's old pose object, or keeping unchanged blip lists**: in real games the conditions almost never hold.

## GPU time of big explosions (particles)

*Measured 26/10/2026, RX 7900 XTX, D3D12, 1600x900 windowed, `adaptive_acid_lakes` seed 7, the staged battle of `tests/lights/ingame/dynlights_census.lua` (camera zoom 28, lights off), `FAF_GPU_PROFILE=600`.*

In the heaviest 600-frame blocks the `EffParticles` pass (inside `Effects`) takes **1.4–1.7 ms GPU per frame** (averaged over all blocks 0.43 ms) of a 2.2–2.6 ms frame, from only 90–100 draws and about **1,500 quads**. It is fill rate, not draw cost or shader cost, and the game's own overdraw rather than the D3D12 translation:

- **Pixels**: `FAF_GPU_PROFILE_PIXELS=EffParticles` (new on D3D12; `1` = outermost regions; D3D11 already had it for every region) counts 130–220 million pixel-shader invocations per frame in those blocks. That is 90–150 screens of overdraw at 1600x900, about 120,000 pixels (350x350) per quad. Direct3D 11 shows the same counts on the same scene (up to 200 Mpix), so both backends rasterise the same work. The rate is 90–140 Gpix/s in the peak blocks, about 60–70 Gpix/s averaged over every block above 20 Mpix.
- **Not the shaders or textures**: `particle.fx` has no depth reads and no soft particles; its pixel shaders are `ps_2_0` (one or two samples and a ramp). Running the pass with a solid-colour pixel shader (`FAF_GAL_SOLID_PS=1`) costs the same (1.10 ms against 1.08 ms per frame in the blocks above 20 Mpix), so what is left is blend and ROP bandwidth.
- **Not the translation**: the draws are the same instanced quads, the state is the effect's (`Depth_Enable_Less_Write_None`, blend per technique), the target is the same BGRA8 back buffer on both APIs (one shared format table), and the particle draws only bind their own textures and buffers (no copies or render-target switches in the pass).
- **Scales with the pixels** (extrapolated, not measured): at 2560x1440 the same blocks would cost about 2.5x, at 3840x2160 5.8x (about 4 and 10 ms), which is what a "100 % GPU" report at the nuke looks like on a larger screen or a frame limiter that leaves the GPU no idle time.

What cuts it is fewer pixels, not a different D3D12 path: the next section draws the large soft particles into a smaller target.

D3D11's GPU timestamps are not usable for this comparison: a region shows 14 ms for 12 draws, more than the frame's wall time (cause not investigated). D3D12's agree with the frame time.

## Large particles in a lower-resolution target

*Done 26/10/2026 on branch `particle-overdraw`. Measured on the RX 7900 XTX, D3D12, `adaptive_acid_lakes` seed 7, a staged scene of 67 structures and two strategic nukes (`tests/lights/ingame/dynlights_census_structures.lua`, nuke at game time 13 s, camera zoom 22, the game paused at 43 s inside the fireball so that every frame is the same one), `FAF_GPU_PROFILE=300`, the setting switched between Full, Half and Quarter in the running process.*

The quads of a big explosion are drawn over each other until a screen carries a hundred layers; the page above shows that this costs blend and ROP bandwidth. Those quads are soft clouds, so the engine now draws the ones of 320 pixels and more (side of a square, at full resolution; `ren_ParticleLowResSize`) into a half or quarter resolution target and blends that onto the scene. Everything smaller is drawn as before. The setting is **Options > Video > Large Particle Quality** (Full / Half / Quarter resolution, default Half; `ren_ParticleLowRes` 0 / 1 / 2) on Direct3D 11, Direct3D 12 and Vulkan.

How it works (`src/moho/particles/ParticleLowRes.cpp`, `ParticleEffectPatch.cpp`; no game file is changed):

- **The effect is patched as it loads** (like the dynamic lights): `particle.fx` gets the globals `LRParams` and friends, a size test in `WorldVS` / `WorldVSAlign` that collapses the quads of the other size class (the test needs only the instance data and the camera, so two draws of one bucket agree on which quad is which), an `_LR` twin of every ADD, ALPHABLEND and PREMODALPHA world-particle technique, and the downsample and composite passes. `/noparticlelowres` leaves the file alone; a source that does not match leaves it unpatched, and a patched effect that does not build falls back to the original.
- **Per `RenderEffects` call** the scene depth is copied once (`Device::CopyDepthStencil`, so the depth buffer itself never changes state between runs), and downsampled to the farthest depth of each low-resolution pixel's footprint, written as the depth of the low-resolution target.
- **Per run of buckets** (same sort scalar; the game sorts them by sort scalar, then blend mode, then texture pointer) whose large quads are estimated to cover at least 30 Mpix (`ren_ParticleLowResMinMpix`): the large quads are drawn into the cleared target with the `_LR` techniques, depth-tested by the hardware against the downsampled depth; the target is composited onto the scene; then the same instanced draws are issued again for the small quads. The estimate comes from a copy of each uploaded particle's vertex-shader inputs, evaluated at the frame's time, so a frame without a big explosion takes exactly the old path (the vertex shader still classifies every quad; a wrong estimate costs time, never pixels).
- **The blending is exact**, not an approximation of it: the target holds S (colour) and coverage A. An "over" quad (ALPHABLEND, PREMODALPHA) does `S = c*a + S*(1-a)`, `A = a + A*(1-a)`, an ADD quad does `S += c*a` and leaves A, and any mix of the two composes in drawing order: `scene = S + scene*(1-A)`. (The backends blend alpha with the colour factors, as D3D9 did, so the "over" quads are written premultiplied to get this.) What changes is the order of large against small quads inside one sort scalar (all large ones first, then the small ones) and the 8 bit clamping of the additive sum.
- **The composite is depth aware**: each pixel takes the four nearest low-resolution pixels weighted by how close their depth is to its own (bilinear on a smooth surface, the side of the edge it is on at a silhouette), which keeps units in front of a fireball free of halos.
- MODULATE, REFRACT and TLight buckets, beams and trails are not touched; a MODULATE bucket ends a run.

Frame GPU time (`GPU`) and the `EffParticles` pass per frame, Full / Half / Quarter, with the defaults of the engine otherwise (`ren_ParticleBatch` off on D3D12, 196 draws in the pass):

| Resolution | Full | Half | Quarter |
|---|---|---|---|
| 1600x900 | 3.32 ms (pass 2.69) | 3.11 ms (2.48) | 2.99 ms (2.37) |
| 2560x1440 | 4.59 ms (3.52) | 4.00 ms (2.87) | 3.75 ms (2.63) |
| 3840x2160 | 5.94 ms (4.44) | 4.73 ms (3.31) | 4.24 ms (2.81) |

So the gain grows with the resolution (6 % of the frame at 1600x900, 13 % at 1440p, 20 % at 4K for Half; 10 / 18 / 29 % for Quarter). At 1600x900 on this card it is small because the card draws 270 Mpix in 2.7 ms and the passes cost about 0.4 ms (the depth copy and downsample, one clear and composite per run, and the large quads' draws twice: draw cost, about 6 us each here, dominates before fill does). With `ren_ParticleBatch` forced on (36 draws) the pass is 1.60 / 1.43 / 1.31 ms and the frame GPU time stays at 2.25 / 2.25 / 2.23 ms at 1600x900. The run of the same scene on Direct3D 11 at 3840x2160 goes from 7.8 ms per frame to the 240 fps limiter (4.2 ms) with Half and with Quarter (its GPU timestamps are not usable, see below); Vulkan 3840x2160: 6.23 / 4.92 / 4.38 ms frame GPU time.

What was checked in game (D3D12, paused frames A/B in one process, Half against Full, `FAF_GAL_DUMP_DIR`; the difference image is the largest channel difference per pixel):

- The nuke fireball filling the screen (zoom 45, 1600x900), six paused frames, three Half and three Full: mean difference 0.18-0.23 of 255, 0.05-0.12 % of the pixels differ by more than 24, the 99.9th percentile is 18-28; two frames of the same setting differ by 0.002-0.05 on average. The difference image is nearly black, with thin lines on the fireball's silhouette.
- The staged battle with experimentals and shield hits (`dynlights_census.lua`, zoom 28), with the minimum raised to 0 so that the path runs whenever there is one large quad: mean 0.17, 0.08 % over 24; a crop of a shield bubble with an Aeon Czar at 2.5x shows no halo, no step pattern and no visible softening. An earlier hard "nearest texel" composite showed 2x2 steps there and was replaced by the weighted one.
- An odd size (1367x769, targets 684x385 and 342x193), zoom 14, Half and Quarter: as noisy as the same setting against itself.
- Direct3D 11 and Vulkan, the same nuke: mean 0.31-0.36 on both, 0.01-0.1 % over 24.
- Not checked: Quarter at the threshold by eye (it is visibly softer on fine detail and meant for slow cards), a resolution change in the running game, split screens.

Per API:

| API | Large Particle Quality | Why |
|---|---|---|
| Direct3D 9 | not offered | it runs the shipped effects, which this patch does not touch, and the shipped exe path has no render-target switch to hang it on; a smaller target also would not pay for itself at the pass costs above |
| Direct3D 10 | not offered | the renderer does not start |
| Direct3D 11 | yes | verified in game |
| Direct3D 12 | yes | verified in game |
| Vulkan | yes | verified in game (the same effect layer: sampled depth views, `CopyDepthStencil` as `vkCmdCopyImage`) |

Limits: the large quads lose about half a pixel of sharpness at Half (the textures are soft; a thin ring or a spark texture of 320 px or more would show it, Quarter shows it everywhere); the 30 Mpix minimum is a guess made on a fast card (a slower card would profit from less, `ren_ParticleLowResMinMpix`); the estimate is evaluated on the CPU once per frame for the buckets of runs (a few hundred particles); a quad exactly at the threshold changes class once as it grows, which is not visible because both classes draw the same picture.

Switches: `ren_ParticleLowRes` (0, 1, 2), `ren_ParticleLowResSize` (320), `ren_ParticleLowResMinMpix` (30), `/noparticlelowres` on the command line; for comparisons `FAF_PARTICLE_LOWRES`, `FAF_PARTICLE_LOWRES_SIZE`, `FAF_PARTICLE_LOWRES_MINMPIX` in the environment (they win over the console variables) and `FAF_PARTICLE_LOWRES_DEBUG=1` (a log line per run now and then; the passes show up as `LRDepth`, `LRAccum`, `LRComposite` in `FAF_GPU_PROFILE`). Host test of the effect patch: `faf_particle_tests` (`FAF_STOCK_FX_DIR` for the real `particle.fx`).

## Shadow filtering

The filtered shadow lookup (Options, Video, Shadow Filtering) is roughly cost-neutral at 3840x2160 on an RX 7900 XTX: the ground lookups cost 0.2 to 0.8 ms per frame, the blur passes it makes unnecessary cost about as much, so the three shadow passes sum to between -0.2 and +0.35 ms of the Off value on Direct3D 12 and Vulkan (tables, method and noise in [Shadow filtering](SHADOW_FILTERING.md#measured)).

## Next

1. Pose and animation on sim workers (about 14 % of sim time).
2. D3D12 cost in replays at full speed.
3. Memory use.
4. GPU-driven draws (branch `gpu-driven`, not merged).
