AB_PRESETS = {
    far = { 70, 0.6, 0, 0, 0 },
    near = { 14, 0.6, 0, 0, 0 },
    nearer = { 8, 0.55, 0, 0, 0 },
}
AB_STEPS = {
    { 'far_off', { 'ren_AmbientOcclusion 0', 'ren_ShadowFilter 0' }, 'far' },
    { 'far_ao', { 'ren_AmbientOcclusion 3' }, 'far' },
    { 'far_sh', { 'ren_AmbientOcclusion 0', 'ren_ShadowFilter 4' }, 'far' },
    { 'near_off', { 'ren_AmbientOcclusion 0', 'ren_ShadowFilter 0' }, 'near' },
    { 'near_ao', { 'ren_AmbientOcclusion 3' }, 'near' },
    { 'near_sh', { 'ren_AmbientOcclusion 0', 'ren_ShadowFilter 4' }, 'near' },
    { 'nearer_off', { 'ren_AmbientOcclusion 0', 'ren_ShadowFilter 0' }, 'nearer' },
    { 'nearer_ao', { 'ren_AmbientOcclusion 3' }, 'nearer' },
    { 'nearer_sh', { 'ren_AmbientOcclusion 0', 'ren_ShadowFilter 4' }, 'nearer' },
}
