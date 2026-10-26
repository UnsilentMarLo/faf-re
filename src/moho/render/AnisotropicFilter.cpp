// Forced anisotropic texture filtering: the render feature `aniso_quality` and its console variable. The sampler rewrite itself lives in the
// backends (gpg/gal/ForcedAnisotropy.h, docs/RENDERER_BACKENDS.md "Anisotropic filtering").

#include "moho/render/RenderFeatures.h"

#include "gpg/gal/ForcedAnisotropy.h"
#include "moho/console/CConCommand.h"

namespace moho::render
{
  namespace
  {
    // 0 off, 1 = 2x, 2 = 4x, 3 = 8x, 4 = 16x (the option defaults to 4x). The variable is the gal's own level, so a change reaches the devices at once:
    // each rebuilds its sampler states for the new level the next time it uses an effect (the epoch in ForcedAnisotropy.h).
    TConVar<int> gTConVar_ren_Anisotropy(
      "ren_Anisotropy",
      "Forced anisotropic texture filtering for the 3D world: 0 off (the effects' own filters), 1 2x, 2 4x, 3 8x, 4 16x. "
      "Normally set by the Anisotropic Filtering option (Options, Video). Point-sampled, shadow, lookup-table and interface samplers are never touched.",
      &gpg::gal::gForcedAnisotropyLevel
    );

    // The Options > Video entry (render features registry). Applies live on every API that can: Direct3D 9 sets the sampler states through its
    // state manager, Direct3D 11 swaps the effect's sampler states (Effects 11 SetSampler), Direct3D 12 and Vulkan build the sampler tables
    // with the level. Direct3D 10 does not start.
    const bool gRegisteredFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "aniso_quality";
      feature.title = "Anisotropic Filtering";
      feature.tip = "Keeps ground, roads, decals and units sharp where they are seen at a shallow angle, at a small cost in graphics memory bandwidth. "
                    "Higher is sharper. Interface, shadows and lookup textures are not touched. Applies at once.";
      feature.console = "ren_Anisotropy";
      feature.states = {{"Off", "off"}, {"2x", "x2"}, {"4x", "x4"}, {"8x", "x8"}, {"16x", "x16"}};
      feature.defaultLevel = 2; // 4x: sharp ground at a grazing angle for about 0.4 ms of GPU time at 1600x900 (docs/RENDERER_BACKENDS.md)
      feature.needsRestart = false;
      feature.apis[static_cast<int>(Api::D3D9)].supported = true;
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) {
        // The profile's level, unless a console command or the command line already set one.
        if (level > 0 && gpg::gal::gForcedAnisotropyLevel == 0) {
          gpg::gal::gForcedAnisotropyLevel = level;
        }
      };
      render::RegisterFeature(feature);
      return true;
    }();
  } // namespace
} // namespace moho::render
