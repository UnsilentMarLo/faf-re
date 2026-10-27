In-game A/B of the soft particles (docs/RENDERER_BACKENDS.md, "Soft particles"). Copy softp_scene.lua as <exe folder>/fafengine/lua/test/dynlights_test.lua
and softp_camera.lua as dynlights_camera.lua there. Each file gets a header (Lua assignments) in front of it:
  scene:  SOFT_MODE = 'scan' | 'land' | 'water'; SOFT_TX, SOFT_TZ = the missile target
  camera: AB_PAUSE_AT (game seconds to pause at, 0 never), AB_MODES (a Lua list of ren_SoftParticles values to cycle while paused), AB_HOLD (seconds each),
          AB_ZOOM, SOFT_TX / SOFT_TY / SOFT_TZ (where the camera looks), AB_HEADING, AB_PITCH
Launch a skirmish on adaptive_acid_lakes (seed 7) with /windowed 1600 900, FAF_GAL_DUMP_DIR + FAF_GAL_DUMP_SECONDS=1 for the frames and FAF_GPU_PROFILE=300 for
the regions (EffParticles, SoftDepth). Run SOFT_MODE 'scan' first for the water position.
