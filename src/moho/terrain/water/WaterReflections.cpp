#include "moho/terrain/water/WaterReflections.h"

#include <cmath>

#include "boost/shared_ptr.h"
#include "gpg/core/utils/Logging.h"
#include "gpg/gal/DepthStencilTarget.hpp"
#include "gpg/gal/DepthStencilTargetContext.hpp"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/EffectVariable.hpp"
#include "gpg/gal/Error.hpp"
#include "gpg/gal/OutputContext.hpp"
#include "gpg/gal/RenderProfiler.h"
#include "moho/console/CConCommand.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/d3d/ShaderVar.h"

namespace moho::water
{
  int ren_WaterSSR = 1;
  static int ren_WaterSSRDebug = 0;
  static int ren_WaterSSRStrength = 100;
  static int ren_WaterSSRDistance = 800;
  static int ren_WaterAbsorb = 1;
  static int ren_WaterAbsorbDepth = 10;

  static TConVar<int> gTConVar_ren_WaterSSR(
    "ren_WaterSSR",
    "Screen-space reflections in the water (terrain, buildings and units show in it): 0 off, 1 (the default) 24 steps of the ray march, 2 48 steps. "
    "Takes effect at the next map load for the shader patch; Direct3D 11, 12 and Vulkan. docs/LIGHTING_V2.md.",
    &ren_WaterSSR
  );
  static TConVar<int> gTConVar_ren_WaterSSRStrength(
    "ren_WaterSSRStrength", "How much of the water reflection the screen-space hits replace, in percent (default 100).", &ren_WaterSSRStrength
  );
  static TConVar<int> gTConVar_ren_WaterSSRDistance(
    "ren_WaterSSRDistance", "The longest reflected ray the march follows, in world units (default 800).", &ren_WaterSSRDistance
  );
  static TConVar<int> gTConVar_ren_WaterAbsorb(
    "ren_WaterAbsorb",
    "Water thickness from the real scene depth instead of the map's baked depth map: 0 off (the shipped look), 1 (the default) tint and refraction follow the "
    "water the view ray really crosses, objects in front of the water are not refracted into it, 2 also absorbs red faster than green and blue. "
    "Takes effect at the next map load for the shader patch; Direct3D 11, 12 and Vulkan. docs/LIGHTING_V2.md.",
    &ren_WaterAbsorb
  );
  static TConVar<int> gTConVar_ren_WaterAbsorbDepth(
    "ren_WaterAbsorbDepth",
    "The water thickness, in world units, at which the real-depth tint is 63 percent of the way to the map's deep colour (default 10; smaller is murkier).",
    &ren_WaterAbsorbDepth
  );
  static TConVar<int> gTConVar_ren_WaterSSRDebug(
    "ren_WaterSSRDebug", "1: the water shows where the screen-space march found a hit (red) and where it did not (blue). 0 (the default) normal.", &ren_WaterSSRDebug
  );

  namespace
  {
    using namespace gpg::gal;

    struct EffectVars
    {
      ShaderVar depth{};
      ShaderVar params{};
      ShaderVar params2{};
      ShaderVar absorb{};
      EffectVars()
      {
        RegisterShaderVar("waterAbsorbParams", &absorb, "water2");
        RegisterShaderVar("waterSceneDepth", &depth, "water2");
        RegisterShaderVar("waterSsrParams", &params, "water2");
        RegisterShaderVar("waterSsrParams2", &params2, "water2");
      }
    };

    struct State
    {
      EffectVars vars;
      Device* device = nullptr;
      boost::shared_ptr<DepthStencilTarget> copy;
      std::uint32_t copyWidth = 0U;
      std::uint32_t copyHeight = 0U;
      bool failed = false;
      bool loggedRefusal = false;
    };

    State& GetState()
    {
      static State* const state = new State(); // the effect unlinks its shader vars on its own schedule: never destroyed
      return *state;
    }

    void BindOff(State& s)
    {
      const float off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      if (s.vars.params.Exists()) {
        s.vars.params.mEffectVariable->SetVector(off);
      }
      if (s.vars.absorb.Exists()) {
        s.vars.absorb.mEffectVariable->SetVector(off);
      }
    }

    // The march reads a copy of the depth: the depth buffer itself stays a depth output while the water pass draws into it.
    bool CopyDepth(State& s, Device& device, DepthStencilTarget& source)
    {
      DepthStencilTargetContext* const context = source.GetContext();
      if (context == nullptr || !context->field0x10_) {
        if (!s.loggedRefusal) {
          s.loggedRefusal = true;
          gpg::Warnf("Water reflections: the head's depth buffer was not created sampled; the screen-space reflections stay off");
        }
        return false;
      }
      if (s.device != &device) {
        s.copy.reset();
        s.copyWidth = 0U;
        s.copyHeight = 0U;
        s.failed = false;
        s.device = &device;
      }
      if (s.failed) {
        return false;
      }
      if (s.copy == nullptr || s.copyWidth != context->width_ || s.copyHeight != context->height_) {
        s.copy.reset();
        try {
          s.copy = DepthStencilTarget::Create(DepthStencilTargetContext(context->width_, context->height_, context->format_, true));
        } catch (const Error& error) {
          gpg::Warnf("Water reflections: cannot create the depth copy: %s", error.what());
          s.failed = true;
          return false;
        }
        s.copyWidth = context->width_;
        s.copyHeight = context->height_;
      }
      if (s.copy == nullptr || !device.CopyDepthStencil(*s.copy, source)) {
        gpg::Warnf("Water reflections: the backend cannot copy the scene depth; the screen-space reflections stay off");
        s.failed = true;
        return false;
      }
      return true;
    }

    constexpr const char* kDeclarations = R"HLSL(
// ---- FAF water reflections (WaterReflections.cpp, docs/LIGHTING_V2.md): screen-space reflections of the scene -------------------
texture waterSceneDepth;
sampler2D waterSceneDepthSampler = sampler_state
{
    Texture   = (waterSceneDepth);
    MipFilter = NONE;
    MinFilter = POINT;
    MagFilter = POINT;
    AddressU  = CLAMP;
    AddressV  = CLAMP;
};
float4 waterSsrParams;    // x: steps of the march (0: off), y: the longest ray in world units, zw: the size of the depth buffer in pixels
float4 waterSsrParams2;   // x: how much the hits replace the reflection, y: 1 = show the hits

// The distance along the view axis of a depth value (the engine's projection is right handed: Projection[2][3] is -1, the eye z is negative).
float FafWaterViewZ(float depth)
{
    return -Projection[3][2] / (depth * Projection[2][3] - Projection[2][2]);
}

float2 FafWaterScreen(float4 clip)
{
    return clip.xy / clip.w * ViewportScaleOffset.xy + ViewportScaleOffset.zw;
}

float FafWaterSceneZ(float2 uv)
{
    return FafWaterViewZ(tex2Dlod(waterSceneDepthSampler, float4(uv, 0.0, 0.0)).x);
}

float4 waterAbsorbParams; // x: 0 = the baked depth map, 1 = the real depth, 2 = the real depth and colour extinction, y: the depth in world units of 63 percent tint

// The distance the view ray travels through the water before it meets what the scene drew at uv (0 when the scene is in front of the water).
float FafWaterPath(float2 uv, float surfaceZ, float eyeDistance)
{
    float sceneZ = FafWaterSceneZ(uv);
    if (!(sceneZ > 0.0))
    {
        sceneZ = 100000.0;                          // nothing drawn there: as deep as it gets
    }
    return max(sceneZ - surfaceZ, 0.0) * eyeDistance / max(surfaceZ, 0.01);
}

// rgb: the scene colour the reflected ray meets; a: how sure the hit is (0: nothing, use the sky and the mirrored units)
float4 FafWaterSSR(float3 position, float3 dir, float2 pixel)
{
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    if (waterSsrParams.x < 0.5 || dir.y < 0.02)
    {
        return result;
    }
    float4x4 viewProjection = mul(WorldToView, Projection);
    float4 c0 = mul(float4(position, 1.0), viewProjection);
    float4 dc = mul(float4(dir, 0.0), viewProjection);
    if (c0.w < 0.5)
    {
        return result;
    }
    float len = waterSsrParams.y;
    if (dc.w < 0.0)
    {
        len = min(len, (c0.w - 0.5) / -dc.w);       // the ray ends in front of the camera plane
    }
    float4 c1 = c0 + dc * len;
    float k0 = 1.0 / c0.w;
    float k1 = 1.0 / c1.w;
    float2 s0 = FafWaterScreen(c0);
    float2 s1 = FafWaterScreen(c1);
    float2 delta = s1 - s0;
    float smax = 1.0;                               // the part of the ray that stays on the screen
    if (delta.x > 0.0001) { smax = min(smax, (1.0 - s0.x) / delta.x); }
    if (delta.x < -0.0001) { smax = min(smax, -s0.x / delta.x); }
    if (delta.y > 0.0001) { smax = min(smax, (1.0 - s0.y) / delta.y); }
    if (delta.y < -0.0001) { smax = min(smax, -s0.y / delta.y); }
    if (smax <= 0.0)
    {
        return result;
    }
    int steps = (int)waterSsrParams.x;
    float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float sPrev = 0.0;
    float hit = -1.0;
    [loop] for (int i = 0; i < steps; ++i)
    {
        float s = smax * (float(i) + jitter) / float(steps);
        float rayZ = 1.0 / lerp(k0, k1, s);
        float sceneZ = FafWaterSceneZ(lerp(s0, s1, s));
        float nextZ = 1.0 / lerp(k0, k1, s + smax / float(steps));
        float stride = abs(nextZ - rayZ);
        float gap = rayZ - sceneZ;
        if (gap > 0.0 && gap < stride * 2.0 + 0.3)  // the ray went behind what the scene drew, by less than a step: a hit, not an occluder
        {
            hit = s;
            break;
        }
        sPrev = s;
    }
    if (hit < 0.0)
    {
        return result;
    }
    float lo = sPrev;
    float hi = hit;
    [unroll] for (int j = 0; j < 5; ++j)
    {
        float mid = 0.5 * (lo + hi);
        float midZ = 1.0 / lerp(k0, k1, mid);
        if (midZ > FafWaterSceneZ(lerp(s0, s1, mid))) { hi = mid; } else { lo = mid; }
    }
    float2 uv = lerp(s0, s1, hi);
    float2 edge = min(uv, 1.0 - uv);
    float confidence = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - smoothstep(0.7, 1.0, hi / smax));
    result.rgb = tex2Dlod(RefractionSampler, float4(uv, 0.0, 0.0)).rgb;
    result.a = confidence;
    return result;
}

)HLSL";
  } // namespace

  bool SsrWanted()
  {
    return ren_WaterSSR > 0 || ren_WaterAbsorb > 0;
  }

  SsrPatchResult PatchWaterReflections(std::string& source)
  {
    SsrPatchResult result;
    const auto count = [&](const std::string& needle) {
      std::size_t n = 0;
      for (std::size_t p = source.find(needle); p != std::string::npos; p = source.find(needle, p + needle.size())) {
        ++n;
      }
      return n;
    };
    const std::string shader = "float4 HighFidelityPS(";
    const std::string mixLine = "float3 reflections = lerp(skyReflection.rgb, unitReflections.rgb, saturate(unitReflections.a));";
    const std::string unitSun = "sunReflection *= (1 - saturate(unitReflections.a * 4));";
    const std::string sunAdd = "water += sunReflection;";
    const std::string refractOffset = "refractionPos -= sqrt(waterDepth) * refractionScale * N.xz * OneOverW;";
    const std::string tintLine = "refractedPixels.rgb = lerp(refractedPixels.rgb, waterColor, factor);";
    if (count(shader) != 1 || count(mixLine) != 1 || count(unitSun) != 1 || count(sunAdd) != 1 || count(refractOffset) != 1 || count(tintLine) != 1 ||
        source.find("FafWaterSSR") != std::string::npos) {
      result.error = "HighFidelityPS, the reflection mix lines or the refraction lines not found exactly once";
      return result;
    }
    if (source.find("sampler RefractionSampler") == std::string::npos || source.find("ViewportScaleOffset;") == std::string::npos ||
        source.find("Projection;") == std::string::npos || source.find("WorldToView;") == std::string::npos ||
        source.find("float waterDepth") == std::string::npos || source.find("mScreenPos") == std::string::npos) {
      result.error = "RefractionSampler, ViewportScaleOffset, Projection, WorldToView, waterDepth or mScreenPos not declared";
      return result;
    }
    std::string text = source;
    // the later line first, so the earlier anchors stay valid: the extinction of the colours (before the tint toward the deep colour)
    const std::size_t tintAt = text.find(tintLine);
    text.insert(
      tintAt,
      "if (waterAbsorbParams.x > 1.5) { refractedPixels.rgb *= exp(-float3(1.0, 0.4, 0.15) * fafPath / (waterAbsorbParams.y * 4.0)); }\n    "
    );
    // the sun glint is not seen where a scene hit is reflected: first the later line, so the earlier anchor stays valid
    const std::size_t sunAddAt = text.find(sunAdd);
    text.insert(
      sunAddAt + sunAdd.size(),
      "\n    if (waterSsrParams2.y > 0.5) { water = lerp(float3(0.0, 0.0, 0.4), float3(1.0, 0.0, 0.0), ssrHit.a); }"
    );
    const std::size_t unitSunAt = text.find(unitSun);
    text.insert(unitSunAt + unitSun.size(), "\n\tsunReflection *= (1.0 - saturate(ssrHit.a * waterSsrParams2.x));");
    const std::size_t mixAt = text.find(mixLine);
    text.insert(
      mixAt + mixLine.size(),
      "\n    float4 ssrHit = FafWaterSSR(ViewPosition - inV.mViewVec, R, screenPos * waterSsrParams.zw);"
      "\n    reflections = lerp(reflections, ssrHit.rgb, saturate(ssrHit.a * waterSsrParams2.x));"
    );
    // The thickness the view ray crosses replaces the baked depth: the map's waterLerp range, the refraction strength and the tint keep their meaning
    // (the depth stays 0..1). The first pass sizes the refraction offset, the second reads the thickness where the refracted ray lands; a refracted
    // pixel in front of the water plane (a unit, a bridge, the shore) falls back to the straight view.
    const std::size_t offsetAt = text.find(refractOffset);
    text.insert(
      offsetAt + refractOffset.size(),
      "\n    if (waterAbsorbParams.x > 0.5)"
      "\n    {"
      "\n        if (FafWaterSceneZ(refractionPos) < fafSurfaceZ - 0.05) { refractionPos = screenPos; }"
      "\n        fafPath = FafWaterPath(refractionPos, fafSurfaceZ, fafEye);"
      "\n        waterDepth = 1.0 - exp(-fafPath / waterAbsorbParams.y);"
      "\n    }"
    );
    text.insert(
      offsetAt,
      "float fafSurfaceZ = inV.mScreenPos.w;"
      "\n    float fafEye = length(inV.mViewVec);"
      "\n    float fafPath = 0.0;"
      "\n    if (waterAbsorbParams.x > 0.5)"
      "\n    {"
      "\n        fafPath = FafWaterPath(screenPos, fafSurfaceZ, fafEye);"
      "\n        waterDepth = 1.0 - exp(-fafPath / waterAbsorbParams.y);"
      "\n    }"
      "\n    "
    );
    text.insert(text.find(shader), kDeclarations);
    source = std::move(text);
    result.patched = true;
    return result;
  }

  void BindWaterReflections(Device* const device, const GeomCamera3& camera)
  {
    State& s = GetState();
    if (!s.vars.params.Exists() || !s.vars.depth.Exists()) {
      return;
    }
    const auto& projection = camera.projection;
    const bool perspective = std::fabs(projection.r[2].w) > 0.5f && std::fabs(projection.r[3].w) < 0.5f;
    if (!SsrWanted() || device == nullptr || !perspective) {
      BindOff(s);
      return;
    }
    const RenderProfileScope profile("WaterDepth");
    OutputContext context;
    device->GetContext(&context);
    if (context.depthStencil == nullptr || !CopyDepth(s, *device, *context.depthStencil)) {
      BindOff(s);
      return;
    }
    s.vars.depth.mEffectVariable->SetDepthStencil(s.copy);
    const float steps = ren_WaterSSR <= 0 ? 0.0f : (ren_WaterSSR >= 2 ? 48.0f : 24.0f);
    const float params[4] = {steps, static_cast<float>(ren_WaterSSRDistance), static_cast<float>(s.copyWidth), static_cast<float>(s.copyHeight)};
    s.vars.params.mEffectVariable->SetVector(params);
    if (s.vars.absorb.Exists()) {
      const float absorb[4] = {
        ren_WaterAbsorb <= 0 ? 0.0f : (ren_WaterAbsorb >= 2 ? 2.0f : 1.0f), static_cast<float>(ren_WaterAbsorbDepth < 1 ? 1 : ren_WaterAbsorbDepth), 0.0f, 0.0f
      };
      s.vars.absorb.mEffectVariable->SetVector(absorb);
    }
    if (s.vars.params2.Exists()) {
      const float params2[4] = {0.01f * static_cast<float>(ren_WaterSSRStrength), ren_WaterSSRDebug != 0 ? 1.0f : 0.0f, 0.0f, 0.0f};
      s.vars.params2.mEffectVariable->SetVector(params2);
    }
  }

  void ReleaseWaterReflections()
  {
    State& s = GetState();
    if (s.vars.depth.mEffectVariable && s.vars.depth.Exists()) {
      s.vars.depth.mEffectVariable->SetDepthStencil(boost::shared_ptr<DepthStencilTarget>());
    }
    s.copy.reset();
    s.copyWidth = 0U;
    s.copyHeight = 0U;
    s.device = nullptr;
    s.failed = false;
  }
} // namespace moho::water
