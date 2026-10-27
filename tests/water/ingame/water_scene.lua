-- Scene for the water quality tests (docs/WATER_QUALITY.md). Not part of the engine: copy as <exe folder>/fafengine/lua/test/dynlights_test.lua
-- (the name the sim hook looks for) and water_camera.lua as .../test/dynlights_camera.lua (the UI hook), start a skirmish on a map with a
-- shoreline (Seton's Clutch), e.g. /map /maps/setons_clutch_-_faf_version.v0004/setons_clutch_-_faf_version_scenario.lua.
--
-- Finds a spot of deep water with land 20-40 units to the east (the same scan runs in the camera script), puts a fleet on the water and a few
-- structures and land units on the shore, so the picture holds reflected hulls, a shoreline, units on land and open water.

local function Say(text)
    LOG(string.format('WATER-TEST: [%.1f s] %s', GetGameTimeSeconds(), text))
end

-- scan identical to water_camera.lua
local function FindSpot()
    local size = ScenarioInfo.size
    local best, bestScore = nil, -1
    for x = 64, size[1] - 64, 8 do
        for z = 64, size[2] - 64, 8 do
            local ground = GetTerrainHeight(x, z)
            local surface = GetSurfaceHeight(x, z)
            if surface - ground > 6 then
                -- land to the east within 40 units and deep water for 40 units to the west, south and north
                local landAt = nil
                for d = 8, 64, 4 do
                    if GetSurfaceHeight(x + d, z) - GetTerrainHeight(x + d, z) < 0.2 then landAt = d break end
                end
                if landAt then
                    local deep = true
                    for _, o in {{-20, 0}, {0, -20}, {0, 20}, {-20, -20}, {-20, 20}} do
                        if GetSurfaceHeight(x + o[1], z + o[2]) - GetTerrainHeight(x + o[1], z + o[2]) < 4 then deep = false end
                    end
                    if deep then
                        local score = -math.abs(x - size[1] / 2) - math.abs(z - size[2] / 2) - landAt
                        if score > bestScore or not best then best, bestScore = {x, z, landAt}, score end
                    end
                end
            end
        end
    end
    return best
end

ForkThread(function()
    local brain = ArmyBrains[1]
    WaitSeconds(3)
    do
        local size = ScenarioInfo.size
        local n1, n3, n6, maxd = 0, 0, 0, 0
        for x = 16, size[1] - 16, 16 do
            for z = 16, size[2] - 16, 16 do
                local d = GetSurfaceHeight(x, z) - GetTerrainHeight(x, z)
                if d > 1 then n1 = n1 + 1 end
                if d > 3 then n3 = n3 + 1 end
                if d > 6 then n6 = n6 + 1 end
                if d > maxd then maxd = d end
            end
        end
        Say(string.format('water cells (16 grid): >1 %d, >3 %d, >6 %d, max depth %.1f', n1, n3, n6, maxd))
    end
    local spot = FindSpot()
    if not spot then Say(string.format('no shoreline spot found; map %d x %d, centre ground %.1f surface %.1f', ScenarioInfo.size[1], ScenarioInfo.size[2], GetTerrainHeight(ScenarioInfo.size[1] / 2, ScenarioInfo.size[2] / 2), GetSurfaceHeight(ScenarioInfo.size[1] / 2, ScenarioInfo.size[2] / 2))) return end
    local x, z, landAt = spot[1], spot[2], spot[3]
    Say(string.format('spot %d, %d (land %d units east) surface %.1f map %d x %d', x, z, landAt, GetSurfaceHeight(x, z), ScenarioInfo.size[1], ScenarioInfo.size[2]))

    local ships = {
        {'ues0201', -12, -16}, {'ues0103', -4, -8}, {'uas0201', -10, 6}, {'urs0201', -18, 14}, {'xss0201', -2, 18},
        {'ues0302', -26, 0}, {'uas0302', -20, -26}, {'urs0103', -6, 0}, {'uas0103', -14, 26}, {'ues0202', -30, -12},
    }
    local made = 0
    for _, s in ships do
        local ux, uz = x + s[2], z + s[3]
        if __blueprints[s[1]] then
            local unit = CreateUnitHPR(s[1], brain.Name, ux, GetSurfaceHeight(ux, uz), uz, 0, 1.2, 0)
            if unit then made = made + 1 end
        end
    end
    -- shore: structures and land units where the ground is above the water
    local shore = x + landAt + 6
    for i, id in {'ueb1101', 'ueb2101', 'uab1101', 'urb2101', 'ueb4201', 'uel0201', 'ual0201', 'url0107'} do
        local ux, uz = shore + math.mod(i, 2) * 8, z - 22 + i * 5
        if __blueprints[id] and GetSurfaceHeight(ux, uz) - GetTerrainHeight(ux, uz) < 0.2 then
            if CreateUnitHPR(id, brain.Name, ux, GetTerrainHeight(ux, uz), uz, 0, 0, 0) then made = made + 1 end
        end
    end
    Say(string.format('%d units created', made))
end)
