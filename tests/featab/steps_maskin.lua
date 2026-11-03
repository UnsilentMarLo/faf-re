AB_PRESETS = { z0 = { 1010, 1.3, 0.0, 0, 0 }, z1 = { 1000, 1.3, 0.0, 0, 0 } }
AB_STEPS = {
  { 'init', { 'ren_WaterSim 1', 'ren_WaterQuality 3', 'ren_ShoreTessCell 0', 'ren_ShoreTessCoeff 1', 'ren_ShoreErrorCoeff 0.4', 'ren_WaterMaskDebug 1' }, 'z0' },
  { 'mode1', {}, 'z1' },
  { 'mode3', { 'ren_WaterMaskDebug 3' }, 'z1' },
  { 'mode4', { 'ren_WaterMaskDebug 4' }, 'z1' },
  { 'mode0', { 'ren_WaterMaskDebug 0' }, 'z1' },
  { 'end', {}, 'z1' },
}
