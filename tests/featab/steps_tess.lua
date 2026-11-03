AB_PRESETS = { z300 = { 300, 0.9, 0.0, 0, 0 }, z600 = { 600, 0.8, 0.0, 0, 0 } }
AB_STEPS = {
  { 'off', { 'ren_WaterSim 1', 'ren_WaterQuality 3', 'ren_WaterMaskDebug 2', 'ren_ShoreTessCoeff 1' }, 'z300' },
  { 'o1', {}, 'z300' }, { 'o2', {}, 'z300' }, { 'o3', {}, 'z300' },
  { 'o4', {}, 'z600' }, { 'o5', {}, 'z600' }, { 'o6', {}, 'z600' },
  { 'on', { 'ren_ShoreTessCoeff 0.1' }, 'z300' },
  { 'n1', {}, 'z300' }, { 'n2', {}, 'z300' }, { 'n3', {}, 'z300' },
  { 'n4', {}, 'z600' }, { 'n5', {}, 'z600' }, { 'n6', {}, 'z600' },
}
