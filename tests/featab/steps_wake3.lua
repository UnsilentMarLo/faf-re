AB_PRESETS = {
    top = { 30, 1.1, 0, 0, 0 },
    low = { 18, 0.5, 0, 0, 0 },
}
AB_STEPS = {
    { 'warm', { 'ren_WaterSim 1', 'ren_DynLights 4', 'ren_WaterSimStrength 100' }, 'top' },
    { 'w_top', { 'ren_WaterSimStrength 100' }, 'top' },
    { 'w_top2', { 'ren_WaterSimStrength 100' }, 'top' },
    { 'w_low', { 'ren_WaterSimStrength 100' }, 'low' },
    { 'w_low2', { 'ren_WaterSimStrength 100' }, 'low' },
    { 'w_off', { 'ren_WaterSimStrength 0' }, 'low' },
}
