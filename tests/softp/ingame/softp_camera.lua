-- Soft particles test camera: zoom (and look at SOFT_TX, SOFT_TZ), pause at AB_PAUSE_AT, then cycle ren_SoftParticles through AB_MODES while
-- paused (AB_HOLD seconds each). Header defines AB_PAUSE_AT, AB_MODES, AB_HOLD, AB_ZOOM, SOFT_TX, SOFT_TY, SOFT_TZ and optionally AB_HEADING / AB_PITCH.
ForkThread(function()
    WaitSeconds(8)
    local camera = GetCamera('WorldCamera')
    if camera then
        if SOFT_TX ~= 0 then
            camera:SnapTo({ SOFT_TX, SOFT_TY, SOFT_TZ }, { AB_HEADING or 0, AB_PITCH or 0.55, 0 }, AB_ZOOM or 45)
        else
            camera:SetZoom(AB_ZOOM or 45, 1.0)
        end
        LOG('SOFT-TEST: camera set, zoom ' .. tostring(AB_ZOOM or 45))
    end
    if AB_PAUSE_AT > 0 then
        while GetGameTimeSeconds() < AB_PAUSE_AT do WaitSeconds(0.1) end
        SessionRequestPause()
        LOG('SOFT-TEST: paused at ' .. GetGameTimeSeconds())
        WaitSeconds(6)
        for i, mode in AB_MODES do
            if type(mode) == 'string' then ConExecute(mode) else ConExecute('ren_SoftParticles ' .. mode) end
            LOG('SOFT-TEST: mode ' .. mode .. ' wall ' .. GetSystemTimeSeconds())
            WaitSeconds(AB_HOLD)
        end
        LOG('SOFT-TEST: ab done')
    end
end)
