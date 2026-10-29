AB_PRESETS = {
    side = { 38, 0.5, 0, -12, 0 },
    back = { 38, 0.5, 3.14, -12, 0 },
    front = { 38, 0.5, 1.57, -12, 0 },
}
AB_STEPS = {
    { 'base', { 'ren_DynLights 0', 'ren_AmbientOcclusion 0', 'ren_SoftParticles 0', 'ren_ParticleLowRes 0', 'ren_ShadowFilter 0' }, 'side' },
    { 'lights', { 'ren_DynLights 4' }, 'side' },
    { 'soft', { 'ren_DynLights 0', 'ren_SoftParticles 1' }, 'side' },
    { 'lowres', { 'ren_SoftParticles 0', 'ren_ParticleLowRes 1' }, 'side' },
    { 'ao', { 'ren_ParticleLowRes 0', 'ren_AmbientOcclusion 3' }, 'side' },
    { 'bk_base', { 'ren_AmbientOcclusion 0' }, 'back' },
    { 'bk_all', { 'ren_DynLights 4', 'ren_AmbientOcclusion 3', 'ren_SoftParticles 1', 'ren_ParticleLowRes 1' }, 'back' },
    { 'fr_base', { 'ren_DynLights 0', 'ren_AmbientOcclusion 0', 'ren_SoftParticles 0', 'ren_ParticleLowRes 0' }, 'front' },
    { 'fr_all', { 'ren_DynLights 4', 'ren_AmbientOcclusion 3', 'ren_SoftParticles 1', 'ren_ParticleLowRes 1' }, 'front' },
}
