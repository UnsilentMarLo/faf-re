#include "moho/render/shadow/ShadowFilter.h"

#include <algorithm>
#include <atomic>

#include "moho/console/CConCommand.h"
#include "moho/render/RenderFeatures.h"
#include "moho/render/d3d/ShaderVar.h"
#include "moho/terrain/TerrainShaderVars.h"

namespace moho::shadowfilter
{
  int ren_ShadowFilter = 0;
  static float ren_ShadowFilterBias = 0.5f;
  static float ren_ShadowFilterSlope = 1.0f;

  static TConVar<int> gTConVar_ren_ShadowFilter(
    "ren_ShadowFilter",
    "Shadow filtering (docs/SHADOW_FILTERING.md): 0 Off (the shipped 1024 map, 5-tap lookup and blurred caster mask), 1 Low, 2 Medium, 3 High, 4 Ultra. "
    "A level sets the smallest shadow map size (ren_ShadowSize can ask for more), the number of taps of the rotated-disk lookup, and (High, Ultra) contact-hardening soft shadows. "
    "Normally set by the Shadow Filtering option (Options, Video); applies at once.",
    &ren_ShadowFilter
  );

  static TConVar<float> gTConVar_ren_ShadowFilterBias(
    "ren_ShadowFilterBias", "Scale of the constant depth bias of the filtered shadow lookup (1 = the shipped bias); default 0.5.", &ren_ShadowFilterBias
  );

  static TConVar<float> gTConVar_ren_ShadowFilterSlope(
    "ren_ShadowFilterSlope", "Scale of the receiver-plane (slope) bias of the filtered shadow lookup; 0 = none, default 1.", &ren_ShadowFilterSlope
  );

  namespace
  {
    // Direct3D 9 and Direct3D 10 (does not start) are not served; see the feature registration below.
    constexpr LevelParams kLevels[] = {
      {1024, 8, 1.5f, 1.5f, 0.0f},    // Low: the shipped map size, 8 taps
      {2048, 12, 2.0f, 2.0f, 0.0f},   // Medium
      {2048, 16, 1.5f, 6.0f, 0.04f},  // High: contact hardening
      {4096, 24, 2.0f, 10.0f, 0.04f}, // Ultra
    };

    std::atomic<bool> gMeshPatched{false};
    std::atomic<bool> gTerrainPatched{false};

    float gLightWidth = 0.0f;
    float gLightDepthRange = 0.0f;

    struct FilterVars
    {
      ShaderVar meshFilter{};
      ShaderVar meshFilter2{};
      ShaderVar terrainFilter{};
      ShaderVar terrainFilter2{};
      FilterVars()
      {
        RegisterShaderVar("shadowFilter", &meshFilter, "mesh");
        RegisterShaderVar("shadowFilter2", &meshFilter2, "mesh");
        RegisterShaderVar("ShadowFilter", &terrainFilter, "terrain");
        RegisterShaderVar("ShadowFilter2", &terrainFilter2, "terrain");
      }
    };

    FilterVars& Vars()
    {
      static FilterVars vars;
      return vars;
    }

    // Direct3D 11, 12 and Vulkan: the Direct3D 10 effect dialect, where the tap loop runs on a shader parameter and the level switches live.
    bool UsesDynamicDialect(const gpg::gal::DeviceApi api)
    {
      return gpg::gal::UsesD3D10EffectDialect(api);
    }

    const bool gRegisteredFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "shadow_filter";
      feature.title = "Shadow Filtering";
      feature.tip = "Soft, filtered shadows: a larger shadow map, a rotated-disk filter instead of the 5-tap lookup, and from High on shadows that soften "
                    "with the distance to the object that casts them. Off keeps the shipped shadows. Render only, never affects the game; applies at once.";
      feature.console = "ren_ShadowFilter";
      feature.states = {{"Off (original)", "off"}, {"Low", "low"}, {"Medium", "medium"}, {"High (soft shadows)", "high"}, {"Ultra (4096 map)", "ultra"}};
      feature.defaultLevel = 0;
      feature.needsRestart = false;
      feature.apis[static_cast<int>(Api::D3D9)].reason =
        "Direct3D 9 compiles the shipped shaders as ps_2_a with 32 constant registers, which the lit pixel shaders already use (a patched lookup with "
        "taps compiled in fails with X5589), and has no dynamic loops";
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) { ren_ShadowFilter = level; };
      render::RegisterFeature(feature);
      return true;
    }();
  } // namespace

  const LevelParams& ParamsFor(const int level)
  {
    return kLevels[std::clamp(level, 1, 4) - 1];
  }

  bool Supported(const gpg::gal::DeviceApi api)
  {
    return UsesDynamicDialect(api);
  }

  void NoteEffectPatched(const bool terrain, const bool patched)
  {
    (terrain ? gTerrainPatched : gMeshPatched).store(patched, std::memory_order_relaxed);
  }

  bool Active()
  {
    return ren_ShadowFilter > 0 && gMeshPatched.load(std::memory_order_relaxed) && gTerrainPatched.load(std::memory_order_relaxed);
  }

  int WantedMapSize(const int userSize)
  {
    // the level sets the smallest map; the Shadow Resolution slider (ren_ShadowSize) can ask for more
    return Active() ? std::max(userSize, ParamsFor(ren_ShadowFilter).mapSize) : userSize;
  }

  void SetLightVolume(const float width, const float depthRange)
  {
    gLightWidth = width;
    gLightDepthRange = depthRange;
  }

  void UpdateShaderVars(const int mapSize)
  {
    float filter[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float filter2[4] = {ren_ShadowFilterBias, ren_ShadowFilterSlope, 0.0f, 0.0f};
    if (Active()) {
      const LevelParams& p = ParamsFor(ren_ShadowFilter);
      filter[0] = static_cast<float>(p.taps);
      filter[1] = p.minRadius;
      filter[2] = p.maxRadius;
      // uv per unit of depth difference: a depth unit spans the light volume's depth range in world units, a uv unit its width
      if (p.penumbraTan > 0.0f && gLightWidth > 0.0f && gLightDepthRange > 0.0f) {
        filter[3] = p.penumbraTan * gLightDepthRange / gLightWidth;
      }
    }
    FilterVars& vars = Vars();
    SetShaderVarMem(vars.meshFilter, 4U, filter);
    SetShaderVarMem(vars.meshFilter2, 4U, filter2);
    SetShaderVarMem(vars.terrainFilter, 4U, filter);
    SetShaderVarMem(vars.terrainFilter2, 4U, filter2);
    if (Active()) {
      // the blur passes set the terrain's ShadowSize; with the filter on they do not run
      GetTerrainShaderVars().shadowSize.SetFloat(static_cast<float>(mapSize));
    }
  }
} // namespace moho::shadowfilter
