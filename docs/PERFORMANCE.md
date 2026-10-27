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

What would cut it is fewer pixels, not a different D3D12 path: drawing the large soft particles (quads of more than some 100,000 pixels) into a half-resolution target and compositing them, or capping a particle's screen size. Both change the picture, so neither is done.

D3D11's GPU timestamps are not usable for this comparison: a region shows 14 ms for 12 draws, more than the frame's wall time (cause not investigated). D3D12's agree with the frame time.

## Shadow filtering

The filtered shadow lookup (Options, Video, Shadow Filtering) is roughly cost-neutral at 3840x2160 on an RX 7900 XTX: the ground lookups cost 0.2 to 0.8 ms per frame, the blur passes it makes unnecessary cost about as much, so the three shadow passes sum to between -0.2 and +0.35 ms of the Off value on Direct3D 12 and Vulkan (tables, method and noise in [Shadow filtering](SHADOW_FILTERING.md#measured)).

## Next

1. Pose and animation on sim workers (about 14 % of sim time).
2. D3D12 cost in replays at full speed.
3. Memory use.
4. GPU-driven draws (branch `gpu-driven`, not merged).
