AB_PRESETS = {
    mid = { 11, 1.2, 0, 28, 0 },
}
AB_STEPS = {
    { 'all', { 'ren_DynLights 4', 'ren_ShadowFilter 4', 'ren_AmbientOcclusion 3', 'ren_AntiAliasing 1', 'ren_SharpenQuality 1', 'ren_Anisotropy 4', 'ren_WaterQuality 3', 'ren_ParticleLowRes 1', 'ren_SoftParticles 1' }, 'mid' },
    { 'no_shadow', { 'ren_ShadowFilter 0' }, 'mid' },
    { 'no_ao', { 'ren_ShadowFilter 4', 'ren_AmbientOcclusion 0' }, 'mid' },
    { 'no_lowres', { 'ren_AmbientOcclusion 3', 'ren_ParticleLowRes 0', 'ren_SoftParticles 0' }, 'mid' },
    { 'no_post', { 'ren_ParticleLowRes 1', 'ren_SoftParticles 1', 'ren_AntiAliasing 0', 'ren_SharpenQuality 0' }, 'mid' },
}
