AB_PRESETS = {
    top = { 40, 1.1, 0, 0, 0 },
    low = { 16, 0.28, 0, 0, 0 },
}
AB_STEPS = {
    { 'warm', { 'ren_WaterSim 1', 'ren_WaterSimTest 100', 'ren_DynLights 4' }, 'top' },
    { 'g_top', { 'ren_WaterGrid 1' }, 'top' },
    { 'g_top2', { 'ren_WaterGrid 1' }, 'top' },
    { 'g_low', { 'ren_WaterGrid 1' }, 'low' },
    { 'g_low2', { 'ren_WaterGrid 1' }, 'low' },
    { 'q_low', { 'ren_WaterGrid 0' }, 'low' },
    { 'q_low2', { 'ren_WaterGrid 0' }, 'low' },
}
