-- Feature on/off capture scene. Not part of the engine: copy as <exe folder>/fafengine/lua/test/dynlights_test.lua with a header that
-- defines AB_SCENE ('scan' | 'static' | 'nuke') and AB_TX, AB_TZ (the nuke target / water position for 'scan' output).
--   scan   : logs the start position and the nearest water ("FEATAB: water at x z")
--   static : the census structures of four factions plus idle units, nothing moves (so unpaused frames of different settings are comparable)
--   nuke   : the static scene is skipped; a strategic missile is launched at t = 10 s at (AB_TX, AB_TZ)

local function Say(text)
    LOG(string.format('FEATAB: [%.1f s] %s', GetGameTimeSeconds(), text))
end

local function WaitUntil(seconds)
    while GetGameTimeSeconds() < seconds do
        WaitTicks(5)
    end
end

local FACTIONS = { 'ue', 'ua', 'ur', 'xs' }
local STRUCTURES = { '0101', '0102', '1101', '1102', '1103', '1104', '1201', '1301', '3101', '3201', '2101', '2104', '2301', '2302', '2305' }

ForkThread(function()
    local brainA, brainB = ArmyBrains[1], ArmyBrains[2]
    local x0, z0 = brainA:GetArmyStartPos()
    SetAlliance(1, 2, 'Enemy')
    SetAlliance(2, 1, 'Enemy')
    Say(string.format('start position (%.1f, %.1f)', x0, z0))

    if AB_SCENE == 'scan' then
        for r = 10, 250, 4 do
            for i = 0, 23 do
                local a = i * math.pi / 12
                local x, z = x0 + r * math.cos(a), z0 + r * math.sin(a)
                if GetSurfaceHeight(x, z) > GetTerrainHeight(x, z) + 1.5 then
                    Say(string.format('water at %.1f %.1f (ground %.2f, surface %.2f, radius %d)', x, z, GetTerrainHeight(x, z), GetSurfaceHeight(x, z), r))
                    return
                end
            end
        end
        Say('no water found')
        return
    end

    brainA:GiveResource('Energy', 100000)
    brainA:GiveResource('Mass', 100000)
    if AB_SCENE == 'static' then
        WaitUntil(4)
        for fi, prefix in FACTIONS do
            for si, number in STRUCTURES do
                local id = prefix .. 'b' .. number
                local ux, uz = x0 - 40 + (si - 1) * 5, z0 - 30 + (fi - 1) * 12
                if __blueprints[id] then
                    CreateUnitHPR(id, brainA.Name, ux, GetTerrainHeight(ux, uz), uz, 0, 0, 0)
                end
            end
        end
        local ids = { 'uel0401', 'url0402', 'ual0401', 'xsl0401', 'xel0305', 'url0303', 'ual0303', 'uel0303' }
        for i = 1, 8 do
            local ux, uz = x0 - 35 + i * 9, z0 + 14
            if __blueprints[ids[i]] then
                CreateUnitHPR(ids[i], brainA.Name, ux, GetTerrainHeight(ux, uz), uz, 0, math.pi * 0.25 * i, 0)
            end
        end
        Say('static scene created')
        return
    end
    if AB_SCENE == 'water' then
        -- hover tanks and a few structures on and around the lake at (AB_TX, AB_TZ): the mirrored pass draws what stands on the water
        WaitUntil(4)
        local ids = { 'uel0203', 'url0203', 'ual0203', 'xsl0203', 'uel0203', 'url0203', 'ual0203', 'xsl0203' }
        for i = 1, 8 do
            local a = i * math.pi / 4
            local r = 6 + 4 * math.mod(i, 2)
            local ux, uz = AB_TX + r * math.cos(a), AB_TZ + r * math.sin(a)
            if __blueprints[ids[i]] then
                CreateUnitHPR(ids[i], brainA.Name, ux, GetSurfaceHeight(ux, uz), uz, 0, a, 0)
            end
        end
        for i = 1, 6 do
            local ux, uz = AB_TX - 30 + i * 10, AB_TZ + 24
            local id = ({ 'ueb2101', 'uab2101', 'urb2101', 'xsb2101', 'ueb1101', 'uab1101' })[i]
            if __blueprints[id] then
                CreateUnitHPR(id, brainA.Name, ux, GetTerrainHeight(ux, uz), uz, 0, 0, 0)
            end
        end
        Say('water scene created')
        return
    end
    if AB_SCENE == 'nuke' then
        WaitUntil(10)
        local launcher = CreateUnitHPR('xsb2305', brainB.Name, x0 + 60, GetTerrainHeight(x0 + 60, z0), z0, 0, 0, 0)
        if launcher then
            launcher:GiveNukeSiloAmmo(1)
            IssueNuke({ launcher }, { AB_TX, GetSurfaceHeight(AB_TX, AB_TZ), AB_TZ })
            Say(string.format('nuke launched at %.1f %.1f', AB_TX, AB_TZ))
        end
    end
end)
