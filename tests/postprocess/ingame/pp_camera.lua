-- Private SMAA / sharpening test camera: zoom, pause, then cycle through SEQUENCE (each entry: label, ren_AntiAliasing, ren_SharpenQuality) while
-- paused, HOLD seconds each, drawing the label on screen (in the interface, so the label itself is not filtered). The values below are edited
-- by the launcher with sed.
local ZOOM = 30
local HOLD = 6
local PAUSE_AT = 8 -- 0: never pause (with RESIZE)
local RESIZE = false -- true: run SC_PrimaryAdapter / windowed changes while the modes of SEQUENCE are on
local SEQUENCE = {
    { 'OFF', 0, 0 }, { 'FXAA', 1, 0 }, { 'SMAA', 2, 0 }, { 'SMAA+SHARPEN LOW', 2, 1 }, { 'SMAA+SHARPEN HIGH', 2, 2 }, { 'SHARPEN HIGH', 0, 2 }, { 'OFF', 0, 0 },
}

local label = nil
local function ShowMode(text)
    local UIUtil = import('/lua/ui/uiutil.lua')
    local LayoutHelpers = import('/lua/maui/layouthelpers.lua')
    if not label then
        label = UIUtil.CreateText(GetFrame(0), '', 30, UIUtil.bodyFont)
        LayoutHelpers.AtLeftTopIn(label, GetFrame(0), 380, 150)
        label.Depth:Set(10000)
    end
    label:SetText('PP MODE ' .. text)
end

ForkThread(function()
    WaitSeconds(6)
    local camera = GetCamera('WorldCamera')
    if camera then
        camera:SetZoom(ZOOM, 1.0)
        LOG('PP-TEST: camera zoom ' .. ZOOM)
    end
    if PAUSE_AT > 0 then
        while GetGameTimeSeconds() < PAUSE_AT do WaitSeconds(0.1) end
        SessionRequestPause()
        LOG('PP-TEST: paused at ' .. GetGameTimeSeconds())
        WaitSeconds(4)
    end
    for i, entry in SEQUENCE do
        ConExecute('ren_AntiAliasing ' .. entry[2])
        ConExecute('ren_SharpenQuality ' .. entry[3])
        ShowMode(entry[1])
        LOG('PP-TEST: mode ' .. entry[1] .. ' aa ' .. entry[2] .. ' sharpen ' .. entry[3] .. ' wall ' .. GetSystemTimeSeconds())
        WaitSeconds(HOLD)
    end
    LOG('PP-TEST: ab done')
end)

if RESIZE then
    ForkThread(function()
        WaitSeconds(14)
        for i, cmd in { 'SC_PrimaryAdapter 1280,720,60', 'SC_PrimaryAdapter 1920,1080,60', 'windowed', 'SC_PrimaryAdapter 1280,720,60', 'windowed' } do
            ConExecute(cmd)
            LOG('PP-TEST: ran ' .. cmd)
            WaitSeconds(5)
        end
        LOG('PP-TEST: resize done')
    end)
end
