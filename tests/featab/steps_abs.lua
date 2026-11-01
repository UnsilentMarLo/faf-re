AB_PRESETS = {
    a = { 30, 0.5, 1.57, 20, 0 },
    b = { 55, 0.8, 1.57, 20, 0 },
}
AB_STEPS = {
    { 'off', { 'ren_WaterAbsorb 0' }, 'a' },
    { 'real', { 'ren_WaterAbsorb 1' }, 'a' },
    { 'real2', { 'ren_WaterAbsorb 2' }, 'a' },
    { 'murky', { 'ren_WaterAbsorb 2', 'ren_WaterAbsorbDepth 3' }, 'a' },
    { 'offb', { 'ren_WaterAbsorb 0', 'ren_WaterAbsorbDepth 10' }, 'b' },
    { 'realb', { 'ren_WaterAbsorb 1' }, 'b' },
}
