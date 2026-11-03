AB_PRESETS = { z0 = { 1010, 1.3, 0.0, 0, 0 }, z1 = { 1000, 1.3, 0.0, 0, 0 }, z2 = { 1002, 1.3, 0.0, 0, 0 }, z3 = { 1004, 1.3, 0.0, 0, 0 }, z4 = { 1006, 1.3, 0.0, 0, 0 }, z5 = { 1008, 1.3, 0.0, 0, 0 } }
AB_STEPS = {
  { 'init', { 'ren_WaterSim 1', 'ren_WaterQuality 3', 'ren_WaterMaskDebug 1', 'ren_ShoreTessCell 0', 'ren_ShoreTessCoeff 1', 'ren_ShoreErrorCoeff 0.4' }, 'z0' },
  { 'm_retail', { 'ren_ShoreTessCell 0.002' }, 'z1' },
  { 'm_cell0.002', { 'ren_ShoreTessCell 0.0005' }, 'z2' },
  { 'm_cell0.0005', { 'ren_ShoreTessCell 0', 'ren_ShoreTessCoeff 0.1', 'ren_ShoreErrorCoeff 0.1' }, 'z3' },
  { 'm_coef0.1', { 'ren_ShoreTessCoeff 0.001', 'ren_ShoreErrorCoeff 0.001' }, 'z4' },
  { 'm_coef0.001', {}, 'z5' },
}
