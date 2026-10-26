-- Camera and A/B control for the water quality tests (docs/WATER_QUALITY.md); copy as <exe folder>/fafengine/lua/test/dynlights_camera.lua.
-- The user interface has no GetTerrainHeight / GetSurfaceHeight, so the spot comes from the sim scan of water_scene.lua (its log line
-- "WATER-TEST: spot X, Z (land D units east) surface S"): edit SPOT and SURFACE for another map. The camera is a low, free camera (cam_Free) so
-- that the water is seen at the grazing angles where the reflection is visible. The game is paused, so every mode renders the same frame; the
-- quality level is switched live and a coloured square at the left edge (0 red, 1 green, 2 blue, 3 yellow) says which level a dumped frame shows.
-- The log lines "mode N" and the profiler blocks (FAF_GPU_PROFILE) are in the order they happened, which attributes every block to its level.
--   POSES: {x, z, zoom, pitch (radians), heading (radians)}; MODES: ren_WaterQuality values to cycle through; HOLD: seconds per mode.
local SPOT = {528, 408, 64}
local SURFACE = 17.5
local POSES = {{505, 408, 28, 0.25, 0}, {505, 408, 28, 0.25, 1.5708}}
local MODES = {0, 1, 2, 3}
local COLOURS = {[0] = 'ffff0000', [1] = 'ff00ff00', [2] = 'ff0000ff', [3] = 'ffffff00'}
local HOLD = 7

local Bitmap = import('/lua/maui/bitmap.lua').Bitmap
local LayoutHelpers = import('/lua/maui/layouthelpers.lua')

ForkThread(function()
    WaitSeconds(6)
    local camera = GetCamera('WorldCamera')
    if not camera then LOG('WATER-TEST: no camera') return end
    local marker = Bitmap(GetFrame(0))
    LayoutHelpers.AtLeftTopIn(marker, GetFrame(0), 2, 300)
    LayoutHelpers.SetDimensions(marker, 24, 24)
    marker.Depth:Set(100000)
    marker:SetSolidColor('ff808080')
    ConExecute('cam_Free true')
    local pose = POSES[1]
    camera:SnapTo(Vector(pose[1], SURFACE, pose[2]), Vector(pose[5], pose[4], 0), pose[3])
    WaitSeconds(14)
    SessionRequestPause()
    LOG('WATER-TEST: paused at ' .. GetGameTimeSeconds())
    WaitSeconds(3)
    for i, pose in POSES do
        camera:SnapTo(Vector(pose[1], SURFACE, pose[2]), Vector(pose[5], pose[4], 0), pose[3])
        LOG('WATER-TEST: pose ' .. i)
        WaitSeconds(3)
        for _, mode in MODES do
            ConExecute('ren_WaterQuality ' .. mode)
            marker:SetSolidColor(COLOURS[mode])
            LOG('WATER-TEST: pose ' .. i .. ' mode ' .. mode)
            WaitSeconds(HOLD)
        end
    end
    LOG('WATER-TEST: ab done')
end)
