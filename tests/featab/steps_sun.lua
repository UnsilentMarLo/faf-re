AB_PRESETS = {
    wide = { 110, 1.0, 0, 0, 0 },
    mid = { 45, 0.8, 0, 0, 0 },
}
AB_STEPS = {
    { 'on_wide', { 'ren_DynLights 4', 'ren_DynLightsMode 1', 'ren_DynLightsSunShadows 1' }, 'wide' },
    { 'on_mid', { 'ren_DynLightsSunShadows 1' }, 'mid' },
    { 'off_wide', { 'ren_DynLightsSunShadows 0' }, 'wide' },
    { 'off_mid', { 'ren_DynLightsSunShadows 0' }, 'mid' },
    { 'on_wide_b', { 'ren_DynLightsSunShadows 1' }, 'wide' },
}
