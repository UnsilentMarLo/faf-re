AB_PRESETS = {
    top = { 45, 1.2, 0, 0, 0 },
    low = { 22, 0.5, 0, 0, 0 },
}
AB_STEPS = {
    { 'warm', { 'ren_WaterSim 1', 'ren_DynLights 4' }, 'top' },
    { 'w_top', { 'ren_WaterSimStrength 100' }, 'top' },
    { 'w_top2', { 'ren_WaterSimStrength 100' }, 'top' },
    { 'w_low', { 'ren_WaterSimStrength 100' }, 'low' },
    { 'w_low2', { 'ren_WaterSimStrength 100' }, 'low' },
    { 'w_off', { 'ren_WaterSimStrength 0' }, 'low' },
}
