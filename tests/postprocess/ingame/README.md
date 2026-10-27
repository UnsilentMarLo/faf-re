# In-game SMAA / sharpening A/B

Not part of the engine: copy `pp_scene.lua` as `<exe folder>/fafengine/lua/test/dynlights_test.lua` (the sim hook runs it) and `pp_camera.lua` as `.../dynlights_camera.lua` (the UI hook runs it; edit `HOLD`, `PAUSE_AT`, `RESIZE`, `SEQUENCE` at its top), then start a skirmish
(`main_x64_<name>.exe /init C:\ProgramData\FAForever\bin\init_faf.lua /nobugreport /d3d12 /windowed 1600 900 /nomovie /nosound /seed 7 /faction 1 /map /maps/adaptive_acid_lakes.v0004/adaptive_acid_lakes_scenario.lua /log <file>`, working directory `C:\ProgramData\FAForever\bin`; `/init` must be an absolute path because it is resolved against the exe folder; `FAF_GAL_DUMP_DIR` / `FAF_GAL_DUMP_SECONDS=1` for frames, `FAF_GPU_PROFILE=120` for GPU times).

| File | What |
|---|---|
| `pp_scene.lua` | 60 structures of four factions, 48 land units in clusters, two long walls (one straight, one slanted: thin lines) and three air units in front of the start position |
| `pp_camera.lua` | zoom 30, pause at 8 s, cycle `ren_AntiAliasing` / `ren_SharpenQuality` through `SEQUENCE` (Off, FXAA, SMAA, SMAA + Sharpen Low / High, Sharpen High, Off; `HOLD` seconds each) and draw "PP MODE <name>" on screen; `RESIZE = true` with `PAUSE_AT = 0` runs the `SC_PrimaryAdapter` / windowed changes instead |
| `pick_frames.py <dumpdir> <out>` | groups the dumped frames by the label and saves the last frame of each group as `<out>_<k>.png` |
| `crops.py <out prefix> <sheet.png> x y w h scale k...` | the same crop of frames k side by side, enlarged without smoothing |
| `perf.py <log> [--merge]` | GPU ms per mode of the `AntiAliasing`, `Sharpen` and main passes from the profile blocks of the log |

Host test of the shaders (no game): `cmake --build --preset clang-x64-release --target faf_postprocess_tests`, then `build/clang-x64-release/tests/faf_postprocess_tests.exe`.
