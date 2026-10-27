-- Soft particles test scene (docs/RENDERER_BACKENDS.md, "Soft particles"). Not part of the engine: copy this file as
-- <exe folder>/fafengine/lua/test/dynlights_test.lua and softp_camera.lua as dynlights_camera.lua next to it, each with a header (see README.txt).
--
--   SOFT_MODE 'scan'  : looks for the nearest water around the start position, logs it ("SOFT-TEST: water at x z"), nothing else
--   SOFT_MODE 'land'  : a strategic missile from the east hits (SOFT_TX, SOFT_TZ) on dry land with a few structures around it
--   SOFT_MODE 'water' : the same missile hits the water at (SOFT_TX, SOFT_TZ)
--   SOFT_MODE 'wreck' : eight big units are created around (SOFT_TX, SOFT_TZ) at t = 12 s and killed at t = 14 s (explosions, fire, smoke columns)
-- The missile is launched at t = 10 s and detonates at about t = 40 s.

local function Say(text)
    LOG(string.format('SOFT-TEST: [%.1f s] %s', GetGameTimeSeconds(), text))
end

local function WaitUntil(seconds)
    while GetGameTimeSeconds() < seconds do
        WaitTicks(5)
    end
end

ForkThread(function()
    local brainA, brainB = ArmyBrains[1], ArmyBrains[2]
    local x0, z0 = brainA:GetArmyStartPos()
    SetAlliance(1, 2, 'Enemy')
    SetAlliance(2, 1, 'Enemy')
    Say(string.format('start position (%.1f, %.1f)', x0, z0))

    if SOFT_MODE == 'scan' then
        local found = false
        for r = 10, 250, 4 do
            for i = 0, 23 do
                local a = i * math.pi / 12
                local x, z = x0 + r * math.cos(a), z0 + r * math.sin(a)
                if not found and GetSurfaceHeight(x, z) > GetTerrainHeight(x, z) + 1.5 then
                    found = true
                    Say(string.format('water at %.1f %.1f (ground %.2f, surface %.2f, radius %d)', x, z, GetTerrainHeight(x, z), GetSurfaceHeight(x, z), r))
                end
            end
        end
        if not found then Say('no water found') end
        return
    end

    local tx, tz = SOFT_TX, SOFT_TZ
    if SOFT_MODE == 'wreck' then
        WaitUntil(12)
        local ids = { 'uel0401', 'url0402', 'ual0401', 'xsl0401', 'xrl0403', 'xel0305', 'url0303', 'ual0303' }
        local units = {}
        for i = 1, 8 do
            local a = i * math.pi / 4
            local x, z = tx + 7 * math.cos(a), tz + 7 * math.sin(a)
            if __blueprints[ids[i]] then
                table.insert(units, CreateUnitHPR(ids[i], brainA.Name, x, GetSurfaceHeight(x, z), z, 0, a, 0))
            end
        end
        Say(string.format('%d units created', table.getn(units)))
        WaitUntil(14)
        for _, unit in units do
            if unit and not unit.Dead then unit:Kill() end
        end
        Say('units killed')
        WaitUntil(240)
        return
    end
    WaitUntil(3)
    brainA:GiveResource('Energy', 100000)
    brainA:GiveResource('Mass', 100000)
    if SOFT_MODE == 'land' then
        -- structures and units around the impact: particles intersect them
        local ids = { 'ueb1101', 'ueb1102', 'uab1101', 'urb1101', 'ueb2101', 'uab2101', 'ueb3101', 'urb3101' }
        for i = 1, 16 do
            local a = i * math.pi / 8
            local r = 14 + math.mod(i, 3) * 7
            local x, z = tx + r * math.cos(a), tz + r * math.sin(a)
            local id = ids[1 + math.mod(i, 8)]
            if __blueprints[id] then
                CreateUnitHPR(id, brainA.Name, x, GetTerrainHeight(x, z), z, 0, 0, 0)
            end
        end
        for i = 1, 10 do
            local x, z = tx - 30 + i * 4, tz + 34
            CreateUnitHPR(i < 6 and 'xel0305' or 'url0303', brainA.Name, x, GetTerrainHeight(x, z), z, 0, 0, 0)
        end
    end

    WaitUntil(10)
    local launcher = CreateUnitHPR('xsb2305', brainB.Name, x0 + 60, GetTerrainHeight(x0 + 60, z0), z0, 0, 0, 0)
    if launcher then
        launcher:GiveNukeSiloAmmo(1)
        IssueNuke({ launcher }, { tx, GetSurfaceHeight(tx, tz), tz })
        Say(string.format('nuke launched at %.1f %.1f', tx, tz))
    else
        Say('no launcher')
    end
    WaitUntil(240)
    Say('done')
end)
