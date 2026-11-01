AB_PRESETS = { a = { 60, 1.2, 0.8, 10, -45 }, b = { 60, 1.2, 0.8, 10, -20 }, c = { 90, 1.2, 0.8, 30, -40 } }
AB_STEPS = {
  { 'on', { 'ren_ShadowFilter 4', 'ren_DynLights 4', 'ren_DynLightsMode 1', 'ren_WaterQuality 3', 'shadow_Fidelity 3', 'ren_ShadowSize 2048', 'ren_ShadowLOD 440' }, 'a' },
  { 'a', {}, 'a' }, { 'dbg', { 'ren_WaterSSRDebug 2' }, 'a' }, { 'b', {}, 'b' }, { 'c', {}, 'c' }, { 'off', { 'ren_WaterSSRDebug 0' }, 'c' },
}
