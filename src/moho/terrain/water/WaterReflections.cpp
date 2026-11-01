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
#include "moho/render/Shadow.h"
#include "moho/render/d3d/CD3DRenderTarget.h"
#include "moho/render/d3d/ShaderVar.h"

namespace moho::water
{
  int ren_WaterSSR = 1;
  static int ren_WaterSSRDebug = 0;
  static int ren_WaterSSRStrength = 100;
  static int ren_WaterSSRDistance = 800;
  static int ren_WaterAbsorb = 1;
  static int ren_WaterAbsorbDepth = 10;
  static int ren_WaterCaustics = 50;
  static int ren_WaterUnitShadows = 45;

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
  static TConVar<int> gTConVar_ren_WaterCaustics(
    "ren_WaterCaustics",
    "Caustics on the sea bed seen through shallow water, in percent (0 off, default 50): the light the animated wave normals focus, fading with the water's "
    "thickness. Needs ren_WaterAbsorb. Takes effect at the next map load for the shader patch; Direct3D 11, 12 and Vulkan.",
    &ren_WaterCaustics
  );
  static TConVar<int> gTConVar_ren_WaterUnitShadows(
    "ren_WaterUnitShadows",
    "How much the sun shadow map (units, buildings, trees) darkens the water it falls on, in percent (0 off, default 45); the sun glint is cut there as well. "
    "Takes effect at the next map load for the shader patch; Direct3D 11, 12 and Vulkan.",
    &ren_WaterUnitShadows
  );
  static TConVar<int> gTConVar_ren_WaterSSRDebug(
    "ren_WaterSSRDebug", "1: the water shows where the screen-space march found a hit (red) and where it did not (blue); 2: the unit sun shadow on the water (white lit, black shade, yellow no shadow map). 0 (the default) normal.", &ren_WaterSSRDebug
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
      ShaderVar shadowMatrix{};
      ShaderVar shadowTexture{};
      ShaderVar shadowParams{};
      EffectVars()
      {
        RegisterShaderVar("waterShadowMatrix", &shadowMatrix, "water2");
        RegisterShaderVar("waterShadowTexture", &shadowTexture, "water2");
        RegisterShaderVar("waterShadowParams", &shadowParams, "water2");
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
      if (s.vars.shadowParams.Exists()) {
        s.vars.shadowParams.mEffectVariable->SetVector(off);
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

// The sun shadow map of the units, buildings and trees (the one the terrain and the meshes read), on the water plane. 1 = lit.
float4x4 waterShadowMatrix;
texture waterShadowTexture;
sampler2D waterShadowSampler = sampler_state
{
    Texture   = (waterShadowTexture);
    MipFilter = LINEAR;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    AddressU  = BORDER;
    AddressV  = BORDER;
    BorderColor = float4(1, 1, 1, 1);
};
float4 waterShadowParams; // x: how much the shade darkens the water (0: off), y: world units per depth unit of the shadow map, z: caster distance that still shades fully, w: distance over which it fades out

float FafWaterShadow(float3 position)
{
    if (waterShadowParams.x <= 0.0)
    {
        return 1.0;
    }
    float4 coord = mul(float4(position, 1.0), waterShadowMatrix);
    coord.xy /= coord.w;
    float4 texel = tex2Dlod(waterShadowSampler, float4(coord.xy, 0.0, 0.0));
    // The map stores the depth of the nearest caster (R) and the blurred shade (G) without knowing the receiver. A caster far up the sun ray from the
    // water (a tree on a hill, a cliff top) is not what shades the water next to it: the terrain, which the map does not hold, does. Only casters
    // within reach of the surface count, the way the shadow of a tree on the shore or of a unit afloat falls on the water.
    float gap = (coord.z - texel.r) * waterShadowParams.y;
    float reach = (gap > 0.0) ? saturate(1.0 - (gap - waterShadowParams.z) / waterShadowParams.w) : 0.0;
    return lerp(1.0, texel.g, reach);
}

// The slope of the small wave layers at a world position (the same normal maps and movement the surface uses).
float2 FafCausticSlope(float2 xz)
{
    float4 a = tex2Dlod(NormalSampler2, float4((xz + normal3Movement * Time) * normalRepeatRate.z, 0.0, 0.0));
    float4 b = tex2Dlod(NormalSampler3, float4((xz + normal4Movement * Time) * normalRepeatRate.w, 0.0, 0.0));
    float3 n = 2.0 * (a.xyz + b.xyz) - 2.0;
    return n.xy / max(n.z, 0.3);
}

// The light the surface waves focus on the sea bed at a world position under `depth` units of water: the divergence of the slope field decides
// whether the rays that enter there gather (above 1) or spread (below 1); it averages about 1.
float FafCaustic(float2 xz, float depth)
{
    const float e = 0.35;
    float2 sx0 = FafCausticSlope(xz - float2(e, 0.0));
    float2 sx1 = FafCausticSlope(xz + float2(e, 0.0));
    float2 sz0 = FafCausticSlope(xz - float2(0.0, e));
    float2 sz1 = FafCausticSlope(xz + float2(0.0, e));
    float divergence = ((sx1.x - sx0.x) + (sz1.y - sz0.y)) / (2.0 * e);
    float focus = 1.0 - depth * divergence * 1.5;
    return clamp(0.6 * pow(1.0 / max(focus, 0.2), 1.6), 0.25, 4.0);
}

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
    return ren_WaterSSR > 0 || ren_WaterAbsorb > 0 || ren_WaterCaustics > 0 || ren_WaterUnitShadows > 0;
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
    const std::string waterMix = "float3 water = lerp(refractedPixels, reflections, saturate(fresnel * skyreflectionAmount));";
    if (count(waterMix) != 1 || count(shader) != 1 || count(mixLine) != 1 || count(unitSun) != 1 || count(sunAdd) != 1 || count(refractOffset) != 1 || count(tintLine) != 1 ||
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
      "if (waterAbsorbParams.x > 1.5) { refractedPixels.rgb *= exp(-float3(1.0, 0.4, 0.15) * fafPath / (waterAbsorbParams.y * 4.0)); }"
      "\n    if (waterAbsorbParams.x > 0.5 && waterAbsorbParams.z > 0.0 && fafPath > 0.0)"
      "\n    {"
      "\n        float fafVerticalDepth = fafPath * abs(viewVector.y);"
      "\n        float3 fafBed = ViewPosition - inV.mViewVec - viewVector * fafPath;"
      "\n        float fafFade = exp(-fafVerticalDepth / (waterAbsorbParams.y * 0.5));"
      "\n        refractedPixels.rgb *= 1.0 + waterAbsorbParams.z * fafFade * waterTexture.r * (FafCaustic(fafBed.xz, fafVerticalDepth) - 1.0);"
      "\n    }"
      "\n    "
    );
    // the sun glint is not seen where a scene hit is reflected: first the later line, so the earlier anchor stays valid
    const std::size_t sunAddAt = text.find(sunAdd);
    text.insert(
      sunAddAt + sunAdd.size(),
      "\n    if (waterSsrParams2.y > 0.5 && waterSsrParams2.y < 1.5) { water = lerp(float3(0.0, 0.0, 0.4), float3(1.0, 0.0, 0.0), ssrHit.a); }"
      "\n    if (waterSsrParams2.y > 1.5) { water = float3(fafShadow, fafShadow, waterShadowParams.x > 0.0 ? fafShadow : 0.0); }"
    );
    const std::size_t unitSunAt = text.find(unitSun);
    text.insert(unitSunAt + unitSun.size(), "\n\tsunReflection *= (1.0 - saturate(ssrHit.a * waterSsrParams2.x));\n\tsunReflection *= fafShadow;");
    // the transmitted light is darker where a unit, a building or a tree shades the surface
    text.insert(text.find(waterMix), "refractedPixels.rgb *= lerp(1.0 - waterShadowParams.x, 1.0, fafShadow);\n    ");
    const std::size_t mixAt = text.find(mixLine);
    text.insert(
      mixAt + mixLine.size(),
      "\n    float fafShadow = FafWaterShadow(ViewPosition - inV.mViewVec);"
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

  void BindWaterReflections(Device* const device, const GeomCamera3& camera, const moho::Shadow* const shadow)
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
        ren_WaterAbsorb <= 0 ? 0.0f : (ren_WaterAbsorb >= 2 ? 2.0f : 1.0f), static_cast<float>(ren_WaterAbsorbDepth < 1 ? 1 : ren_WaterAbsorbDepth),
        0.005f * static_cast<float>(ren_WaterCaustics < 0 ? 0 : ren_WaterCaustics), 0.0f
      };
      s.vars.absorb.mEffectVariable->SetVector(absorb);
    }
    // the sun shadow map of the units (the terrain pass binds the same one); off where the shadow pass did not run this frame
    if (s.vars.shadowParams.Exists()) {
      boost::shared_ptr<CD3DRenderTarget> shadowTexture;
      if (shadow != nullptr && shadow->mShadowCameraValid && ren_WaterUnitShadows > 0) {
        shadowTexture = shadow->GetShadowTexture();
      }
      if (shadowTexture != nullptr && s.vars.shadowMatrix.Exists() && s.vars.shadowTexture.Exists()) {
        s.vars.shadowMatrix.SetMatrix4x4(&shadow->mCamera.viewProjection);
        s.vars.shadowTexture.SetRenderTargetTexture(shadowTexture);
        const auto& vp = shadow->mCamera.viewProjection;
        const float depthAxis = std::sqrt(vp.r[0].z * vp.r[0].z + vp.r[1].z * vp.r[1].z + vp.r[2].z * vp.r[2].z);
        const float worldPerDepth = depthAxis > 1.0e-6f ? 1.0f / depthAxis : 0.0f;
        const float shade[4] = {0.01f * static_cast<float>(ren_WaterUnitShadows > 100 ? 100 : ren_WaterUnitShadows), worldPerDepth, 20.0f, 25.0f};
        s.vars.shadowParams.mEffectVariable->SetVector(shade);
      } else {
        const float off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        s.vars.shadowParams.mEffectVariable->SetVector(off);
      }
    }
    if (s.vars.params2.Exists()) {
      const float params2[4] = {0.01f * static_cast<float>(ren_WaterSSRStrength), static_cast<float>(ren_WaterSSRDebug), 0.0f, 0.0f};
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
