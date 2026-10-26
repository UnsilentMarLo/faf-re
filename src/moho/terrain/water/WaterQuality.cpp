#include "moho/terrain/water/WaterQuality.h"

#include <algorithm>
#include <cstddef>

#include "gpg/core/utils/Logging.h"
#include "moho/console/CConCommand.h"
#include "moho/misc/ID3DDeviceResources.h"
#include "moho/render/RenderFeatures.h"
#include "moho/render/d3d/CD3DDepthStencil.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/CD3DRenderTarget.h"
#include "moho/terrain/TerrainShaderVars.h"
#include "moho/terrain/water/WaterShaderVars.h"

namespace moho::water
{
  int ren_WaterQuality = -1;

  static TConVar<int> gTConVar_ren_WaterQuality(
    "ren_WaterQuality",
    "Water reflection quality: 0 the shipped renderer, 1 low (reflection target at half the screen size, filtered lookups), 2 medium (screen "
    "size, 4-tap filtered reflection and refraction lookups), 3 high (1.5 times the screen size, 8-tap reflection). -1 (the default) takes the "
    "Water Quality option (Options, Video). Render only (docs/WATER_QUALITY.md).",
    &ren_WaterQuality
  );

  namespace
  {
    constexpr int kMaxLevel = 3;
    constexpr int kDefaultLevel = 2;

    // The Options > Video entry (render features registry). The shader filter lives in the patched water2.fx and needs the Direct3D 10 effect
    // dialect backends (11, 12, Vulkan); the size of the reflection target is plain device resource creation and works on every backend that
    // draws the water, Direct3D 9 included.
    const bool gRegisteredFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "water_quality";
      feature.title = "Water Quality";
      feature.tip = "Reflection and refraction of the water surface. Low draws the mirrored units at half the screen size, Medium filters the water's "
                    "lookups so reflections no longer shimmer and stair-step, High also draws them at 1.5 times the screen size. Render only, never "
                    "affects the game. Off is the shipped look.";
      feature.console = "ren_WaterQuality";
      feature.states = {{"Off (shipped)", "off"}, {"Low (half size)", "low"}, {"Medium", "medium"}, {"High (1.5x, 8 taps)", "high"}};
      feature.defaultLevel = kDefaultLevel;
      feature.needsRestart = false;
      feature.apis[static_cast<int>(Api::D3D9)].supported = true;
      feature.apis[static_cast<int>(Api::D3D9)].reason =
        "Direct3D 9 compiles the shipped water shader as ps_2_a, which has no room for the filter: only the size of the reflection target changes";
      feature.apis[static_cast<int>(Api::D3D9)].defaultLevel = 0;
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) {
        if (ren_WaterQuality < 0) { // not set on the command line (/con ren_WaterQuality N)
          ren_WaterQuality = level;
        }
      };
      render::RegisterFeature(feature);
      return true;
    }();

    render::Api ActiveRenderApi()
    {
      return render::ActiveApi();
    }

    struct HeadTargets
    {
      boost::shared_ptr<CD3DRenderTarget> color;
      boost::shared_ptr<CD3DDepthStencil> depth;
      int width = 0;
      int height = 0;
    };
    HeadTargets gHeads[2];

    // The format codes WRenViewport::D3DWindowOnDeviceInit creates the head targets with (kViewportHeadTargetFormat, ...DepthStencilFormat).
    constexpr int kTargetFormat = 2;
    constexpr int kDepthStencilFormat = 3;
  } // namespace

  QualityParams ParamsForLevel(const int level) noexcept
  {
    switch (std::clamp(level, 0, kMaxLevel)) {
    case 1:
      return {0.5f, 4.0f, 0.0f, 0.0f};
    case 2:
      return {1.0f, 4.0f, 4.0f, 1.0f};
    case 3:
      return {1.5f, 8.0f, 4.0f, 1.0f};
    default:
      return {1.0f, 0.0f, 0.0f, 0.0f};
    }
  }

  int CurrentLevel()
  {
    if (ren_WaterQuality >= 0) {
      return std::min(ren_WaterQuality, kMaxLevel);
    }
    if (!render::ActiveApiKnown()) {
      return 0;
    }
    // Not set by the startup pass (yet): the option, else the API's default. Cached per API because the console variable is the live control.
    static int cachedFor = -1;
    static int level = 0;
    if (cachedFor != static_cast<int>(ActiveRenderApi())) {
      cachedFor = static_cast<int>(ActiveRenderApi());
      level = std::max(render::FeatureLevel("water_quality", ActiveRenderApi()), 0);
    }
    return std::min(level, kMaxLevel);
  }

  bool EffectPatchWanted(const gpg::gal::DeviceApi api)
  {
    return gpg::gal::UsesD3D10EffectDialect(api) && render::FeatureLevel("water_quality", render::ApiFromDevice(api), false) >= 0;
  }

  void BindShaderParams()
  {
    ShaderVar& filter = GetWater2WaterFilterShaderVar();
    if (!filter.Exists()) {
      return; // the effect was not patched (Direct3D 9, or the source was not recognised)
    }
    const QualityParams params = ParamsForLevel(CurrentLevel());
    const float values[3] = {params.reflectionTaps, params.refractionTaps, params.shorelineBlend};
    SetShaderVarMem(filter, 3U, values);
  }

  ReflectionTarget AcquireReflectionTarget(const int head, const int headWidth, const int headHeight)
  {
    ReflectionTarget result;
    if (head < 0 || head > 1) {
      return result;
    }
    const float scale = ParamsForLevel(CurrentLevel()).reflectionScale;
    if (scale == 1.0f || headWidth <= 0 || headHeight <= 0) {
      if (gHeads[head].color) {
        gHeads[head] = HeadTargets{}; // back to the shipped target: free the extra memory
      }
      return result;
    }

    const int width = std::max(1, static_cast<int>(static_cast<float>(headWidth) * scale));
    const int height = std::max(1, static_cast<int>(static_cast<float>(headHeight) * scale));
    HeadTargets& targets = gHeads[head];
    if (!targets.color || !targets.depth || targets.width != width || targets.height != height) {
      targets = HeadTargets{};
      ID3DDeviceResources* const resources = D3D_GetDevice()->GetResources();
      resources->CreateRenderTarget(targets.color, width, height, kTargetFormat);
      resources->CreateDepthStencil(targets.depth, width, height, kDepthStencilFormat);
      if (!targets.color || !targets.depth) {
        gpg::Warnf("water quality: cannot create a %dx%d reflection target, using the screen-sized one", width, height);
        targets = HeadTargets{};
        return result;
      }
      targets.width = width;
      targets.height = height;
      gpg::Logf("water quality: reflection target %dx%d (screen %dx%d, scale %.2f)", width, height, headWidth, headHeight, scale);
    }
    result.color = targets.color;
    result.depth = targets.depth;
    result.scale = scale;
    result.width = targets.width;
    result.height = targets.height;
    return result;
  }

  void ReleaseReflectionTargets()
  {
    gHeads[0] = HeadTargets{};
    gHeads[1] = HeadTargets{};
  }
} // namespace moho::water
