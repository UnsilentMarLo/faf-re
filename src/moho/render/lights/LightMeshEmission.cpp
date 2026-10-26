#include "moho/render/lights/LightMeshEmission.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace moho::lights
{
  namespace
  {
    enum class Rule
    {
      Uef,
      Aeon,
      Cybran,
      Seraphim,
      Glow,   // meshes made to glow as a whole: projectile and effect meshes (TMeshGlow, NormalMappedGlow ...)
      Legacy
    };

    Rule RuleFor(const std::string_view shader)
    {
      std::string s(shader);
      for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      // the shield bubble of a unit draws with the unit's own textures (AeonPhaseShield ...): the bubble is not the unit
      if (s.find("shield") != std::string::npos) {
        return Rule::Legacy;
      }
      // a mesh whose shader says it glows
      if (s.find("glow") != std::string::npos) {
        return Rule::Glow;
      }
      // the shader names of the unit techniques (mesh.fx: Unit, Insect, Aeon, AeonCZAR, Seraphim, and the navy and other variants)
      if (s.rfind("aeon", 0) == 0) {
        return Rule::Aeon;
      }
      if (s.rfind("seraphim", 0) == 0) {
        return Rule::Seraphim;
      }
      if (s.rfind("insect", 0) == 0 || s.rfind("cybran", 0) == 0) {
        return Rule::Cybran;
      }
      if (s == "unit" || s.rfind("uef", 0) == 0 || s.rfind("nomads", 0) == 0) {
        return Rule::Uef;
      }
      return Rule::Legacy;
    }

    float Saturate(const float v)
    {
      return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    void Texel(const Rgba8Image& image, const float u, const float v, float (&rgba)[4])
    {
      const std::uint32_t x = std::min(image.width - 1, static_cast<std::uint32_t>(Saturate(u) * static_cast<float>(image.width)));
      const std::uint32_t y = std::min(image.height - 1, static_cast<std::uint32_t>(Saturate(v) * static_cast<float>(image.height)));
      const std::uint8_t* p = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
      for (int c = 0; c < 4; ++c) {
        rgba[c] = p[c] / 255.0f;
      }
    }

    bool Usable(const Rgba8Image& image)
    {
      return image.width > 0 && image.height > 0 && image.rgba.size() >= static_cast<std::size_t>(image.width) * image.height * 4;
    }
  } // namespace

  MeshGlow AnalyseMeshGlow(const Rgba8Image& albedo, const Rgba8Image* const specular, const std::string_view shader)
  {
    MeshGlow out;
    if (!Usable(albedo)) {
      return out;
    }
    const bool haveSpec = specular != nullptr && Usable(*specular);
    const Rule rule = RuleFor(shader);

    constexpr std::uint32_t kSamples = 64; // the analysis grid: 64 x 64 texels of the albedo and the specular map, by texture coordinate
    double sumEmission = 0.0;
    double sumTeam = 0.0;
    double sumHue[3] = {0.0, 0.0, 0.0};
    double sumHueWeight = 0.0;
    for (std::uint32_t sy = 0; sy < kSamples; ++sy) {
      for (std::uint32_t sx = 0; sx < kSamples; ++sx) {
        const float u = (static_cast<float>(sx) + 0.5f) / static_cast<float>(kSamples);
        const float v = (static_cast<float>(sy) + 0.5f) / static_cast<float>(kSamples);
        float a[4];
        float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Texel(albedo, u, v, a);
        if (haveSpec) {
          Texel(*specular, u, v, s);
        }

        float emission = 0.0f; // the glow that has the albedo's colour
        float team = 0.0f;     // the glow that has the army colour
        switch (rule) {
        case Rule::Uef:
          emission = s[2] * 0.8f;
          break;
        case Rule::Aeon:
          emission = s[2];
          team = s[3] * 0.5f;
          break;
        case Rule::Cybran:
          emission = std::sqrt(std::max(s[2] - 0.04f, 0.0f));
          break;
        case Rule::Seraphim: {
          emission = Saturate(s[2] - 0.1f);
          team = a[3];
          const float bright = std::max(Saturate(a[0] - 0.4f), std::max(Saturate(a[1] - 0.4f), Saturate(a[2] - 0.4f)));
          emission += bright * 1.5f;
          break;
        }
        case Rule::Glow:
          emission = std::max(a[0], std::max(a[1], a[2])); // the whole surface glows, as bright as its albedo
          break;
        case Rule::Legacy:
          break; // props, wreckage, shields, the map border: their specular.b is not a glow (wreckage noise, a shield pattern), no light
        }

        sumEmission += emission + team;
        sumTeam += team;
        if (emission > 0.0f) {
          // the colour of the glowing part: the albedo, weighted by how hard it glows
          for (int c = 0; c < 3; ++c) {
            sumHue[c] += static_cast<double>(emission) * a[c];
          }
          sumHueWeight += emission;
        }
      }
    }

    const double count = static_cast<double>(kSamples) * kSamples;
    out.area = static_cast<float>(sumEmission / count);
    out.teamShare = sumEmission > 1.0e-9 ? static_cast<float>(sumTeam / sumEmission) : 0.0f;
    if (sumHueWeight > 1.0e-9) {
      float m = 0.0f;
      for (int c = 0; c < 3; ++c) {
        out.hue[c] = static_cast<float>(sumHue[c] / sumHueWeight);
        m = std::max(m, out.hue[c]);
      }
      if (m > 1.0e-6f) {
        for (float& h : out.hue) {
          h /= m;
        }
      }
    }
    out.valid = true;
    return out;
  }

  float MeshLightIntensity(const MeshGlow& glow)
  {
    // Below this the glow is a few texels (a lamp on a building seen from afar): the light would be too faint to see and costs a light.
    if (!glow.valid || glow.area < kMinMeshGlowArea) {
      return 0.0f;
    }
    // A unit whose whole surface glows at full strength (area 1) gets the largest light; a few glowing windows (area 0.02) a faint one.
    // The square root keeps small glows visible without letting a big glowing body white out its surroundings.
    return std::min(kMaxMeshIntensity, kMaxMeshIntensity * std::sqrt(std::min(1.0f, glow.area)));
  }

  float MeshLightRadius(const float sphereRadius)
  {
    if (!std::isfinite(sphereRadius) || sphereRadius <= 0.0f) {
      return 2.0f;
    }
    return std::min(60.0f, std::max(2.0f, 1.5f * sphereRadius + 1.0f));
  }
} // namespace moho::lights
