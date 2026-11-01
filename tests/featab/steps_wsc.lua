AB_PRESETS = {
    a = { 30, 0.5, 1.57, 20, 0 },
    c = { 14, 0.6, 1.57, 12, 0 },
}
AB_STEPS = {
    { 'off', { 'ren_WaterCaustics 0', 'ren_WaterUnitShadows 0' }, 'a' },
    { 'caus', { 'ren_WaterCaustics 100' }, 'a' },
    { 'caus3', { 'ren_WaterCaustics 300' }, 'a' },
    { 'shad', { 'ren_WaterCaustics 0', 'ren_WaterUnitShadows 80' }, 'a' },
    { 'offc', { 'ren_WaterCaustics 0', 'ren_WaterUnitShadows 0' }, 'c' },
    { 'causc', { 'ren_WaterCaustics 150' }, 'c' },
    { 'shadc', { 'ren_WaterCaustics 0', 'ren_WaterUnitShadows 80' }, 'c' },
}
