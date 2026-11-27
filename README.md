Before / after frames for the render stack PRs (Draiget/faf-re#492 to #500). Not part of any PR: images and numbers only.

Each file `frames/prNN/<group>_<api>_<scene>.png` is one scene on one API: top row the full frame, bottom row a crop of the yellow box;
left to right master, the PR with defaults, the PR with its own switches off (where that differs from the defaults), the PR with its default-off features switched on (PR 03, 04, 05).
`stats.json` next to them holds the pixel shares: percent of pixels with a channel difference above 8/255, [full frame, crop, full frame after a 25 px box blur].
`waterdetail0_*` (PR 02): master and PR 02 with `ren_WaterDetail 0`.
Scenes: `sr` = sea scene (replay 28041625, tick 9000), `astro` = lake map (replay 28013263, tick 12000, lake in view, land in the crop), `levels` and `rivers` = map scenes of the PR 08 / 09 work.
Replay paused at a fixed tick, UI hidden, 1920x1080, D3D11 / D3D12 / Vulkan, RX 7900 XTX. Water animates on the wall clock while paused, so water pixels are not repeatable between runs of one build (see the repeat columns).
