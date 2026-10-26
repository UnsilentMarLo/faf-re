#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "gpg/core/utils/Logging.h"
#include "moho/mesh/Mesh.h"
#include "moho/mesh/MeshBatchKey.h"
#include "moho/render/lights/LightCensus.h"
#include "moho/render/lights/LightFeeds.h"
#include "moho/render/lights/LightMeshEmission.h"
#include "moho/render/lights/LightTextureCache.h"
#include "moho/resource/blueprints/RMeshBlueprint.h"

// The unit and projectile meshes that glow (docs/DYNAMIC_LIGHTS.md, "Sources of light"): the glow of a mesh is read from its albedo and
// specular maps the way its pixel shader reads them (LightMeshEmission.h), once per mesh LOD; every visible instance of a glowing LOD is one
// light at the centre of the instance's bounding sphere, in the colour of the glow (the army colour where the army colour glows).
namespace moho::lights
{
  namespace
  {
    constexpr std::uint64_t kKeyMesh = 5ull << 40;
    constexpr float kHoldTicks = 3.0f;
    constexpr std::size_t kMaxMeshLights = 512; // per frame, the instances on screen are many more

    struct LodGlow
    {
      bool analysed = false;
      MeshGlow glow;
      float intensity = 0.0f;
    };

    std::unordered_map<std::string, LodGlow> gGlow; // keyed by albedo | specular | shader

    // Copies the three names out of a blueprint LOD. Not every LOD the renderer batches has a blueprint behind it (the engine makes some LODs
    // itself, for shields and the like) and such a record can hold anything, so the read is guarded: false when it faults. No C++ objects
    // with destructors in this function (structured exception handling).
    bool ReadNames(const RMeshBlueprintLOD& lod, char (&albedo)[260], char (&specular)[260], char (&shader)[64])
    {
      __try {
        std::strncpy(albedo, lod.mAlbedoName.c_str(), sizeof(albedo) - 1);
        std::strncpy(specular, lod.mSpecularName.c_str(), sizeof(specular) - 1);
        std::strncpy(shader, lod.mShaderName.c_str(), sizeof(shader) - 1);
        return true;
      } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
      }
    }

    const LodGlow& GlowOf(const RMeshBlueprintLOD& lod)
    {
      static const LodGlow kNone{};
      char albedoBuffer[260] = {};
      char specularBuffer[260] = {};
      char shaderBuffer[64] = {};
      if (!ReadNames(lod, albedoBuffer, specularBuffer, shaderBuffer)) {
        return kNone;
      }
      const std::string albedo(albedoBuffer);
      const std::string specular(specularBuffer);
      const std::string shader(shaderBuffer);
      const std::string key = albedo + "|" + specular + "|" + shader;
      LodGlow& entry = gGlow[key];
      if (entry.analysed) {
        return entry;
      }
      entry.analysed = true;
      const TexEntry* albedoTexture = LoadTexture(albedo);
      if (albedoTexture == nullptr) {
        return entry;
      }
      const TexEntry* specularTexture = specular.empty() ? nullptr : LoadTexture(specular);
      entry.glow = AnalyseMeshGlow(albedoTexture->image, specularTexture != nullptr ? &specularTexture->image : nullptr, shader);
      entry.intensity = MeshLightIntensity(entry.glow);
      static int sLogBudget = 150;
      if (sLogBudget > 0) {
        --sLogBudget;
        gpg::Logf(
          "DynLights: mesh LOD shader=%s albedo=%s%s specular=%s%s: glow area=%.3f team=%.2f hue=(%.2f %.2f %.2f) -> intensity %.3f",
          shader.c_str(),
          albedo.c_str(),
          albedoTexture != nullptr ? "" : " (unreadable)",
          specular.c_str(),
          specular.empty() ? " (none)" : (specularTexture != nullptr ? "" : " (unreadable)"),
          entry.glow.area,
          entry.glow.teamShare,
          entry.glow.hue[0],
          entry.glow.hue[1],
          entry.glow.hue[2],
          entry.intensity
        );
      }
      if (gGlow.size() > 4096) {
        gGlow.clear(); // the reference is still used by the caller this frame: it was copied into `entry` before the clear
      }
      return entry;
    }
  } // namespace

  void FeedMeshes(const float nowTick)
  {
    SyncQuality();
    LightCollector& collector = Collector();
    if (collector.Quality() == model::kOff) {
      return;
    }
    MeshRenderer* const renderer = MeshRenderer::GetInstance();
    if (renderer == nullptr) {
      return;
    }

    std::size_t emitted = 0;
    static int sFrame = 0;
    for (const MeshBatchBucket& bucket : renderer->meshes) {
      MeshLOD* const lod = bucket.first.mLod;
      // MeshRenderer::Batch stores Mesh::ComputeLOD's result in the key, and that is -1 (all bits set) for an instance that is too far for
      // every LOD: not a pointer, not drawn.
      const auto lodValue = reinterpret_cast<std::uintptr_t>(lod);
      if (lodValue < 0x10000u || lodValue == ~static_cast<std::uintptr_t>(0)) {
        continue;
      }
      // the LOD's blueprint copy can be a marker too (LODs the engine makes itself, shields): only a plausible address is used
      const RMeshBlueprintLOD* const blueprintLod = lod->lodBlueprintCopy.get();
      const auto blueprintValue = reinterpret_cast<std::uintptr_t>(blueprintLod);
      if (blueprintValue < 0x10000u || blueprintValue == ~static_cast<std::uintptr_t>(0)) {
        continue;
      }
      const LodGlow glowCopy = GlowOf(*blueprintLod);
      if (CensusEnabled()) {
        char albedo[260] = {};
        char specular[260] = {};
        char shader[64] = {};
        if (ReadNames(*blueprintLod, albedo, specular, shader)) {
          TextureColour c;
          c.valid = true;
          c.peak = glowCopy.glow.area;
          c.hue[0] = glowCopy.glow.hue[0];
          c.hue[1] = glowCopy.glow.hue[1];
          c.hue[2] = glowCopy.glow.hue[2];
          c.brightness = glowCopy.glow.teamShare;
          const std::string key = std::string("shader=") + shader + " albedo=" + albedo + " spec=" + specular;
          for (std::size_t i = 0; i < bucket.second.size(); ++i) {
            CensusNote("mesh", key, glowCopy.intensity > 0.0f ? "ok:glowing unit" : "no:no glow (area 0)", 0.0f, &c);
          }
        }
      }
      if (glowCopy.intensity <= 0.0f) {
        continue;
      }
      for (const MeshInstance* const instance : bucket.second) {
        if (instance == nullptr || instance->isHidden != 0 || emitted >= kMaxMeshLights) {
          continue;
        }
        const float radius = instance->sphere.Radius;
        if (!std::isfinite(instance->sphere.Center.x) || !std::isfinite(radius)) {
          continue;
        }

        // The colour: the albedo hue of the glowing parts, and the army colour for the share of the glow that is the army colour. The
        // instance colour is the army colour packed as ARGB; -1 (props, projectiles) has no army.
        float rgb[3] = {glowCopy.glow.hue[0], glowCopy.glow.hue[1], glowCopy.glow.hue[2]};
        if (glowCopy.glow.teamShare > 0.0f && instance->color != -1) {
          const auto packed = static_cast<std::uint32_t>(instance->color);
          const float team[3] = {
            static_cast<float>((packed >> 16) & 0xFF) / 255.0f,
            static_cast<float>((packed >> 8) & 0xFF) / 255.0f,
            static_cast<float>(packed & 0xFF) / 255.0f
          };
          const float s = glowCopy.glow.teamShare;
          for (int c = 0; c < 3; ++c) {
            rgb[c] = rgb[c] * (1.0f - s) + team[c] * s;
          }
        }
        const float top = std::max(rgb[0], std::max(rgb[1], rgb[2]));
        if (top > 1.0e-4f) {
          for (float& c : rgb) {
            c /= top;
          }
        }
        ReduceWhite(rgb, 0.5f); // a glow is coloured at its edge and white-hot in its core: the light has the colour

        Emission e;
        e.kind = Kind::Mesh;
        e.pos[0] = instance->sphere.Center.x;
        e.pos[1] = instance->sphere.Center.y;
        e.pos[2] = instance->sphere.Center.z;
        e.rgb[0] = rgb[0];
        e.rgb[1] = rgb[1];
        e.rgb[2] = rgb[2];
        e.radius = MeshLightRadius(radius, glowCopy.glow.effect ? kMaxEffectLightRadius : kMaxUnitLightRadius);
        e.intensity = glowCopy.intensity * std::min(1.0f, std::max(0.0f, instance->dissolve)); // MeshInstance::dissolve is 1 for a solid mesh, 0 for one that has dissolved away
        const std::uint64_t key = kKeyMesh | (reinterpret_cast<std::uintptr_t>(instance) & 0xFFFFFFFFFFull);
        if (collector.Track(key, e, nowTick, kHoldTicks)) {
          ++emitted;
        }
      }
    }
    if (emitted > 0 && (++sFrame % 240) == 1) {
      gpg::Logf("DynLights: %u mesh lights this frame", static_cast<unsigned>(emitted));
    }
  }
} // namespace moho::lights
