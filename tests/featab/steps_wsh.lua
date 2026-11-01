AB_PRESETS = {
    top = { 40, 1.2, 0, 0, 0 },
    mid = { 26, 0.8, 0, 6, 0 },
}
AB_STEPS = {
    { 'off', { 'ren_WaterCaustics 0', 'ren_WaterUnitShadows 0' }, 'top' },
    { 'shad', { 'ren_WaterUnitShadows 80' }, 'top' },
    { 'dbg', { 'ren_WaterSSRDebug 2' }, 'top' },
    { 'dbgm', { 'ren_WaterSSRDebug 2' }, 'mid' },
    { 'normm', { 'ren_WaterSSRDebug 0', 'ren_WaterUnitShadows 80' }, 'mid' },
    { 'offm', { 'ren_WaterUnitShadows 0' }, 'mid' },
    { 'caus', { 'ren_WaterCaustics 100' }, 'mid' },
}
