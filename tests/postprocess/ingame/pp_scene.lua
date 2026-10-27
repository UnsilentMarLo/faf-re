-- Private SMAA / sharpening test scene: structures of four factions in a grid, clusters of land units, a long wall (thin lines), and air units,
-- in front of the first army's start position. The UI script (dynlights_camera.lua) pauses the game and cycles ren_AntiAliasing / ren_SharpenQuality.
local function Say(text) LOG(string.format('PP-TEST: [%.1f s] %s', GetGameTimeSeconds(), text)) end
local function WaitUntil(seconds) while GetGameTimeSeconds() < seconds do WaitTicks(5) end end

local FACTIONS = { 'ue', 'ua', 'ur', 'xs' }
local STRUCTURES = { '0101', '0102', '1101', '1102', '1103', '1104', '1201', '1301', '3101', '3201', '4202', '4301', '2101', '2104', '2301' }
local UNITS = { 'uel0201', 'ual0201', 'url0107', 'xsl0201', 'uel0106', 'ual0106' }

ForkThread(function()
    local brain = ArmyBrains[1]
    local x0, z0 = brain:GetArmyStartPos()
    WaitUntil(4)
    local created = 0
    local function make(id, x, z, heading, air)
        if not __blueprints[id] then return nil end
        local y = GetTerrainHeight(x, z)
        if air then y = y + air end
        local unit = CreateUnitHPR(id, brain.Name, x, y, z, 0, heading or 0, 0)
        if unit then created = created + 1 end
        return unit
    end
    for fi, prefix in FACTIONS do
        for si, number in STRUCTURES do
            make(prefix .. 'b' .. number, x0 - 36 + (si - 1) * 5, z0 - 24 + (fi - 1) * 11)
        end
    end
    for ci = 1, 4 do
        for k = 1, 12 do
            make(UNITS[math.mod(k, 6) + 1], x0 - 30 + (ci - 1) * 18 + math.mod(k, 4) * 1.6, z0 + 24 + math.floor(k / 4) * 1.6)
        end
    end
    -- thin lines: two long walls, one straight, one slanted
    for i = 0, 39 do
        make('ueb5101', x0 - 36 + i * 1.0, z0 + 14, 0)
        make('uab5101', x0 - 36 + i * 1.0, z0 + 16 + i * 0.3, 0)
    end
    make('uea0305', x0 - 8, z0 + 6, 0, 6)
    make('uea0203', x0 - 3, z0 + 6, 0, 9)
    make('uaa0310', x0 + 12, z0 + 5, 0, 20)
    Say(string.format('%d units created around (%.1f, %.1f)', created, x0, z0))
end)
