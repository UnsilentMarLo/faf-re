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

    if AB_SCENE == 'none' then
        Say(string.format('ground height %.2f', GetTerrainHeight(x0, z0)))
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
    if AB_SCENE == 'shield' then
        -- a shield generator with structures and units under its bubble (west), the same group without a shield (east) as the control
        WaitUntil(4)
        local function Group(cx, cz, withShield)
            if withShield then
                CreateUnitHPR('ueb4202', brainA.Name, cx, GetTerrainHeight(cx, cz), cz, 0, 0, 0)
            end
            local ids = { 'ueb1101', 'ueb2101', 'ueb0101', 'ueb2301', 'ueb1103', 'ueb3101' }
            for i = 1, 6 do
                local a = i * math.pi / 3
                local ux, uz = cx + 11 * math.cos(a), cz + 11 * math.sin(a)
                if __blueprints[ids[i]] then
                    CreateUnitHPR(ids[i], brainA.Name, ux, GetTerrainHeight(ux, uz), uz, 0, 0, 0)
                end
            end
            for i = 1, 3 do
                local ux, uz = cx - 6 + i * 6, cz + 3
                CreateUnitHPR('uel0303', brainA.Name, ux, GetTerrainHeight(ux, uz), uz, 0, 0.5 * i, 0)
            end
        end
        Group(x0 - 25, z0, true)
        Group(x0 + 25, z0, false)
        Say(string.format('shield scene created, ground height %.2f', GetTerrainHeight(x0, z0)))
        return
    end
    if AB_SCENE == 'fire' then
        -- weapons of all four factions (tanks, bots, point defence, artillery, the commanders) shoot at tanks of the other army that keep coming back
        local shooters = { 'l0201', 'l0202', 'l0303', 'l0103', 'l0001', 'b2101', 'b2301', 'b2303', 'l0203', 'l0106' }
        local n = 0
        for fi, prefix in FACTIONS do
            for si, sid in shooters do
                local id = prefix .. sid
                if __blueprints[id] then
                    local ux, uz = x0 - 28 + (si - 1) * 4.5, z0 - 24 + (fi - 1) * 9
                    CreateUnitHPR(id, brainA.Name, ux, GetTerrainHeight(ux, uz), uz, 0, math.pi * 0.5, 0)
                    n = n + 1
                end
            end
        end
        Say(string.format('%d shooters created, ground height %.2f', n, GetTerrainHeight(x0, z0)))
        local targets = {}
        while GetGameTimeSeconds() < 120 do
            local alive = 0
            for _, t in targets do
                if t and not t.Dead then alive = alive + 1 end
            end
            if alive < 8 then
                for i = 1, 8 do
                    local ux, uz = x0 + 16 + math.mod(i, 3) * 4, z0 - 24 + i * 6
                    local u = CreateUnitHPR('uel0201', brainB.Name, ux, GetTerrainHeight(ux, uz), uz, 0, -math.pi * 0.5, 0)
                    if u then table.insert(targets, u) end
                end
            end
            local dead = 0
            for _, t in targets do
                if not t or t.Dead then dead = dead + 1 end
            end
            Say(string.format('targets alive %d, dead %d, total %d', alive, dead, table.getn(targets)))
            WaitSeconds(3)
        end
        return
    end
    if AB_SCENE == 'beam1' then
        -- one spider bot firing at one immortal tank: a single beam and its impact, nothing else
        local shooter = CreateUnitHPR('url0402', brainA.Name, x0, GetTerrainHeight(x0, z0), z0, 0, math.pi * 0.5, 0)
        local ux, uz = x0 + 28, z0
        local target = CreateUnitHPR('uel0201', brainB.Name, ux, GetTerrainHeight(ux, uz), uz, 0, -math.pi * 0.5, 0)
        if target then target:SetCanTakeDamage(false) end
        Say(string.format('beam scene: shooter %s target %s ground %.2f', tostring(shooter and shooter:GetBlueprint().BlueprintId), tostring(target ~= nil), GetTerrainHeight(x0, z0)))
        return
    end
    if AB_SCENE == 'ring' then
        -- twelve spider bots around one immortal tank, all firing at it: the beams meet in one impact
        local ux, uz = x0, z0
        local target = CreateUnitHPR('uel0201', brainB.Name, ux, GetTerrainHeight(ux, uz), uz, 0, 0, 0)
        if target then target:SetCanTakeDamage(false) end
        for i = 1, 12 do
            local a = i * math.pi / 6
            local sx, sz = ux + 26 * math.cos(a), uz + 26 * math.sin(a)
            CreateUnitHPR('url0402', brainA.Name, sx, GetTerrainHeight(sx, sz), sz, 0, a + math.pi, 0)
        end
        Say('ring scene created')
        return
    end
    if AB_SCENE == 'lightdisc' then
        -- long-lived light particles of different sizes at known places; the footprint of each should be a smooth disc
        WaitUntil(4)
        local sizes = AB_SIZES or { 6, 14, 30 }
        local anchors = {}
        for i = 1, table.getn(sizes) do
            local ux, uz = x0 - 50 + (i - 1) * 24, z0
            local u = CreateUnitHPR('ueb1101', brainA.Name, ux, GetTerrainHeight(ux, uz), uz, 0, 0, 0)
            table.insert(anchors, u)
        end
        for round = 1, 20 do
            for i = 1, table.getn(sizes) do
                if anchors[i] and not anchors[i].Dead then
                    CreateLightParticle(anchors[i], -1, brainA:GetArmyIndex(), sizes[i], 150, 'glow_03', 'ramp_flare_02')
                end
            end
            WaitSeconds(14)
        end
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
