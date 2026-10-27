AB_PRESETS = {
    close = { 18, 0.5, 0, -20, 4 },
    closer = { 9, 0.5, 0, -20, 6 },
}
AB_STEPS = {
    { 'off_close', { 'ren_ShadowFilter 0' }, 'close' },
    { 'sh2_default', { 'ren_ShadowFilter 2' }, 'close' },
    { 'sh2_bias0', { 'ren_ShadowFilterBias 0', 'ren_ShadowFilterSlope 0' }, 'close' },
    { 'sh4_bias0', { 'ren_ShadowFilter 4' }, 'close' },
    { 'sh3_bias0', { 'ren_ShadowFilter 3' }, 'close' },
    { 'sh3_default', { 'ren_ShadowFilterBias 0.5', 'ren_ShadowFilterSlope 1' }, 'close' },
    { 'off_closer', { 'ren_ShadowFilter 0' }, 'closer' },
    { 'sh2_default_c', { 'ren_ShadowFilter 2' }, 'closer' },
    { 'sh2_bias0_c', { 'ren_ShadowFilterBias 0', 'ren_ShadowFilterSlope 0' }, 'closer' },
}
