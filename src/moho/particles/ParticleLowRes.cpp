#include "moho/particles/ParticleLowRes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include <d3d9.h>

#include "boost/shared_ptr.h"
#include "gpg/core/utils/Logging.h"
#include "gpg/gal/DepthStencilTarget.hpp"
#include "gpg/gal/DepthStencilTargetContext.hpp"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/DeviceContext.hpp"
#include "gpg/gal/Effect.hpp"
#include "gpg/gal/EffectVariable.hpp"
#include "gpg/gal/Error.hpp"
#include "gpg/gal/OutputContext.hpp"
#include "gpg/gal/RenderTarget.hpp"
#include "gpg/gal/RenderProfiler.h"
#include "gpg/gal/RenderTargetContext.hpp"
#include "moho/console/CConCommand.h"
#include "moho/particles/ParticleEffectPatch.h"
#include "moho/particles/ParticleRenderBuckets.h"
#include "moho/render/CRenFrame.h"
#include "moho/render/RenderFeatures.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/ShaderVar.h"

namespace moho
{
  int ren_ParticleLowRes = 1;
  float ren_ParticleLowResSize = 320.0f;
  float ren_ParticleLowResMinMpix = 30.0f;
  int ren_SoftParticles = 1;
  float ren_SoftParticleScale = 1.0f;

  namespace
  {
    TConVar<int> gTConVar_ren_ParticleLowRes(
      "ren_ParticleLowRes",
      "Draw the large soft particles (explosions, smoke, fire: quads of ren_ParticleLowResSize pixels and more) into a lower-resolution "
      "target and composite it onto the scene, which cuts the blend and fill cost of big explosions. 0 off, 1 half resolution, 2 quarter "
      "resolution. The default is 1; Options, Video has the same setting. Direct3D 11, 12 and Vulkan only. The environment variable FAF_PARTICLE_LOWRES overrides it.",
      &ren_ParticleLowRes
    );

    TConVar<float> gTConVar_ren_ParticleLowResSize(
      "ren_ParticleLowResSize",
      "The side, in pixels at full resolution, of a square particle quad from which ren_ParticleLowRes draws it at the lower resolution.",
      &ren_ParticleLowResSize
    );

    // The Options > Video entry (render features registry): Full / Half / Quarter resolution are ren_ParticleLowRes 0, 1, 2. Live, no restart: the
    // particle effect is patched whenever it loads on the Direct3D 10 effect dialect, the console variable only decides whether the passes run.
    const bool gRegisteredFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "particle_quality";
      feature.title = "Large Particle Quality";
      feature.tip = "Big explosions (nukes, experimentals, smoke and fire clouds) are drawn from the same particles, but the biggest ones into a smaller "
                    "image that is blended onto the scene, which is much cheaper for the graphics card. Half resolution looks like full; Quarter resolution is "
                    "for slower cards. Render only, never affects the game.";
      feature.console = "ren_ParticleLowRes";
      feature.states = {{"Full resolution", "full"}, {"Half resolution", "half"}, {"Quarter resolution", "quarter"}};
      feature.defaultLevel = 1;
      feature.apis[static_cast<int>(Api::D3D9)].reason =
        "Direct3D 9 draws with the shipped effects (no patched particle shaders) and a smaller target would not pay for itself there";
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) { ren_ParticleLowRes = level; };
      render::RegisterFeature(feature);
      return true;
    }();

    TConVar<int> gTConVar_ren_SoftParticles(
      "ren_SoftParticles",
      "Soft particles: ADD, ALPHABLEND and PREMODALPHA particles fade out where they meet terrain, units or water instead of showing a hard cut line, "
      "by the distance between the particle and the scene depth behind it. 0 off, 1 low (narrow fade), 2 high (wide fade). The default is 1; Options, "
      "Video has the same setting. Direct3D 11, 12 and Vulkan only. The environment variable FAF_SOFT_PARTICLES overrides it.",
      &ren_SoftParticles
    );

    TConVar<float> gTConVar_ren_SoftParticleScale(
      "ren_SoftParticleScale",
      "Multiplies the soft-edge distance of the ren_SoftParticles level (1 = the level's own distance in world units; FAF_SOFT_PARTICLE_SCALE overrides it).",
      &ren_SoftParticleScale
    );

    // The soft-edge distance (world units along the view axis) of each ren_SoftParticles level; level 0 is off.
    constexpr float kSoftDistance[3] = {0.0f, 1.0f, 3.0f};

    // Options > Video: Off / Low / High are ren_SoftParticles 0, 1, 2. Live; the patched particle effect carries the shaders whatever the level, the
    // level decides whether the scene depth is copied and bound for the particle pass and how wide the fade is.
    const bool gRegisteredSoftFeature = [] {
      using render::Api;
      render::Feature feature;
      feature.key = "soft_particles";
      feature.title = "Soft Particles";
      feature.tip = "Smoke, fire and explosion particles fade out where they touch the ground, units or water instead of showing a hard line where the "
                    "flat particle cuts into the surface. High fades over a wider distance. Cheap for the graphics card. Render only, never affects the game.";
      feature.console = "ren_SoftParticles";
      feature.states = {{"Off", "off"}, {"Low", "low"}, {"High", "high"}};
      feature.defaultLevel = 1;
      feature.apis[static_cast<int>(Api::D3D9)].reason =
        "Direct3D 9 runs the shipped particle shaders as they are and has no way to read the depth buffer in a pixel shader on all cards";
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].supported = true;
      feature.apply = [](const int level) { ren_SoftParticles = level; };
      render::RegisterFeature(feature);
      return true;
    }();

    TConVar<float> gTConVar_ren_ParticleLowResMinMpix(
      "ren_ParticleLowResMinMpix",
      "Millions of pixels of large particle quads (overdraw included) that a run of particle buckets must be estimated to cover before ren_ParticleLowRes "
      "takes over for it: below that the extra passes cost more than they save. 0 uses the low-resolution path whenever there is a large quad.",
      &ren_ParticleLowResMinMpix
    );

    [[nodiscard]] bool EnvironmentValue(const char* const name, double& value)
    {
      char text[32] = {};
      std::size_t length = 0U;
      if (::getenv_s(&length, text, sizeof(text), name) != 0 || length == 0U) {
        return false;
      }
      value = std::atof(text);
      return true;
    }

    // The effect variables of the patched particle.fx (ParticleEffectPatch.cpp).
    struct Variables
    {
      ShaderVar params;
      ShaderVar target;
      ShaderVar view;
      ShaderVar scene;
      ShaderVar ratio;
      ShaderVar quad;
      ShaderVar accum;
      ShaderVar depth;
      ShaderVar sceneDepth;
      ShaderVar soft;
    };

    Variables& GetVariables()
    {
      static Variables variables;
      static const bool registered = [] {
        RegisterShaderVar("LRParams", &variables.params, "particle");
        RegisterShaderVar("LRTarget", &variables.target, "particle");
        RegisterShaderVar("LRView", &variables.view, "particle");
        RegisterShaderVar("LRScene", &variables.scene, "particle");
        RegisterShaderVar("LRRatio", &variables.ratio, "particle");
        RegisterShaderVar("LRQuad", &variables.quad, "particle");
        RegisterShaderVar("LRAccumTexture", &variables.accum, "particle");
        RegisterShaderVar("LRDepthTexture", &variables.depth, "particle");
        RegisterShaderVar("LRSceneDepthTexture", &variables.sceneDepth, "particle");
        RegisterShaderVar("SoftParams", &variables.soft, "particle");
        return true;
      }();
      static_cast<void>(registered);
      return variables;
    }

    void SetVector(ShaderVar& variable, const float x, const float y, const float z, const float w)
    {
      if (variable.Exists()) {
        const float values[4] = {x, y, z, w};
        variable.mEffectVariable->SetVector(values);
      }
    }

    struct State
    {
      gpg::gal::Device* device = nullptr;
      boost::shared_ptr<gpg::gal::RenderTarget> accum; // premultiplied colour, coverage in alpha
      boost::shared_ptr<gpg::gal::DepthStencilTarget> depth; // the downsampled scene depth (also the depth buffer of the accumulation passes)
      boost::shared_ptr<gpg::gal::DepthStencilTarget> sceneCopy; // a sampled copy of the scene depth, made once per RenderEffects call
      std::uint32_t sceneCopyWidth = 0U;
      std::uint32_t sceneCopyHeight = 0U;
      bool copyFailed = false;
      bool sceneCopyFresh = false; // sceneCopy holds the scene depth of this RenderEffects call
      std::uint32_t lowWidth = 0U;
      std::uint32_t lowHeight = 0U;
      std::uint32_t scale = 0U;
      CRenFrame lowQuad;   // a full-screen quad of the low-resolution size
      CRenFrame sceneQuad; // and of the scene target's
      float lowQuadWidth = 0.0f;
      float lowQuadHeight = 0.0f;
      float sceneQuadWidth = 0.0f;
      float sceneQuadHeight = 0.0f;

      // One RenderEffects call.
      bool active = false;     // the soft particles, the low-resolution path, or both are running
      bool lowActive = false;  // the low-resolution path is
      bool softActive = false; // the soft particles are (the scene depth is bound once the first soft bucket asks, EnsureSoftDepth)
      bool softBound = false;  // the scene depth copy is bound to the particle effect for the rest of the call
      float softDistance = 0.0f;
      bool depthReady = false;
      bool depthOk = false;
      int currentMode = 0; // LRParams.x as last set
      gpg::gal::OutputContext saved;
      D3DVIEWPORT9 savedViewport{};
      std::uint32_t sceneWidth = 0U;
      std::uint32_t sceneHeight = 0U;
      LargeQuadGate gate; // scale: projection scale times half the viewport: a quad of half size s at depth d is 2*s*scale/d pixels wide
      std::optional<gpg::gal::RenderProfileScope> accumScope; // FAF_GPU_PROFILE: the accumulation draws of a run
      std::uint32_t runs = 0U;
      std::uint32_t frames = 0U;
      std::uint32_t totalRuns = 0U;
    };

    State& GetState()
    {
      static State* const state = new State(); // never destroyed: its resources belong to a device that is gone by then
      return *state;
    }

    [[nodiscard]] bool Debugging()
    {
      static const bool debugging = [] {
        double value = 0.0;
        return EnvironmentValue("FAF_PARTICLE_LOWRES_DEBUG", value) && value != 0.0;
      }();
      return debugging;
    }

    [[nodiscard]] int Mode()
    {
      static const int envMode = [] {
        double value = 0.0;
        return EnvironmentValue("FAF_PARTICLE_LOWRES", value) ? static_cast<int>(value) : -1;
      }();
      return envMode >= 0 ? envMode : ren_ParticleLowRes;
    }

    [[nodiscard]] float SizeSetting()
    {
      static const float envSize = [] {
        double value = 0.0;
        return EnvironmentValue("FAF_PARTICLE_LOWRES_SIZE", value) ? static_cast<float>(value) : -1.0f;
      }();
      return envSize > 0.0f ? envSize : ren_ParticleLowResSize;
    }

    // The soft-edge distance in world units (0: off).
    [[nodiscard]] float SoftDistance()
    {
      static const int envLevel = [] {
        double value = 0.0;
        return EnvironmentValue("FAF_SOFT_PARTICLES", value) ? static_cast<int>(value) : -1;
      }();
      static const float envScale = [] {
        double value = 0.0;
        return EnvironmentValue("FAF_SOFT_PARTICLE_SCALE", value) ? static_cast<float>(value) : -1.0f;
      }();
      const int level = std::clamp(envLevel >= 0 ? envLevel : ren_SoftParticles, 0, 2);
      const float scale = envScale >= 0.0f ? envScale : ren_SoftParticleScale;
      return kSoftDistance[level] * std::max(scale, 0.0f);
    }

    [[nodiscard]] bool HasTechnique(CD3DEffect& effect, const char* const name)
    {
      try {
        return effect.GetBaseEffect()->GetTechnique(name) != nullptr;
      } catch (const gpg::gal::Error&) {
        return false;
      }
    }

    void SelectAndDraw(CD3DDevice& device, CRenFrame& quad, const char* const technique)
    {
      device.SelectTechnique(technique);
      SD3DVertexRange range{};
      range.sheet = quad.mVertexSheet;
      range.startVertex = 0;
      range.baseVertex = 0;
      range.endVertex = 3;
      std::int32_t primitiveType = 5; // D3DPT_TRIANGLESTRIP
      static_cast<void>(device.DrawPrimitiveList(&range, &primitiveType));
    }

    void BindQuad(CRenFrame& quad, float& cachedWidth, float& cachedHeight, const float width, const float height)
    {
      if (quad.mVertexSheet == nullptr || cachedWidth != width || cachedHeight != height) {
        quad.InitTransformedVerts(width, height);
        cachedWidth = width;
        cachedHeight = height;
      }
    }

    void ReleaseTargets(State& state)
    {
      state.accum.reset();
      state.depth.reset();
      state.lowWidth = 0U;
      state.lowHeight = 0U;
      state.depthReady = false;
    }

    // The low-resolution targets for this viewport; false when they cannot be made.
    bool EnsureTargets(State& state, const std::uint32_t scale, const D3DVIEWPORT9& viewport)
    {
      const std::uint32_t width = std::max<std::uint32_t>((viewport.Width + scale - 1U) / scale, 1U);
      const std::uint32_t height = std::max<std::uint32_t>((viewport.Height + scale - 1U) / scale, 1U);
      if (state.accum != nullptr && state.depth != nullptr && state.lowWidth == width && state.lowHeight == height && state.scale == scale) {
        return true;
      }
      ReleaseTargets(state);
      try {
        // 2: B8G8R8A8 (render target format of the dxgi table); depth format 3: D24S8, sampled (the composite reads it).
        state.accum = gpg::gal::RenderTarget::Create(gpg::gal::RenderTargetContext(width, height, 2U));
        state.depth = gpg::gal::DepthStencilTarget::Create(gpg::gal::DepthStencilTargetContext(width, height, 3U, true));
      } catch (const gpg::gal::Error& error) {
        gpg::Warnf("ParticleLowRes: cannot create the %ux%u targets: %s", width, height, error.what());
        ReleaseTargets(state);
        return false;
      }
      state.lowWidth = width;
      state.lowHeight = height;
      state.scale = scale;
      if (Debugging()) {
        gpg::Logf("ParticleLowRes: low-resolution targets %ux%u (scale %u)", width, height, scale);
      }
      return state.accum != nullptr && state.depth != nullptr;
    }

    void SetSceneUniforms(State& state, const D3DVIEWPORT9& viewport)
    {
      Variables& vars = GetVariables();
      const float lowWidth = static_cast<float>(state.lowWidth);
      const float lowHeight = static_cast<float>(state.lowHeight);
      SetVector(vars.target, lowWidth, lowHeight, 1.0f / lowWidth, 1.0f / lowHeight);
      SetVector(
        vars.view, static_cast<float>(viewport.X), static_cast<float>(viewport.Y), static_cast<float>(viewport.Width),
        static_cast<float>(viewport.Height)
      );
      SetVector(
        vars.scene, static_cast<float>(state.sceneWidth), static_cast<float>(state.sceneHeight), 1.0f / static_cast<float>(state.sceneWidth),
        1.0f / static_cast<float>(state.sceneHeight)
      );
      SetVector(
        vars.ratio, static_cast<float>(viewport.Width) / lowWidth, static_cast<float>(viewport.Height) / lowHeight,
        static_cast<float>(state.scale), 0.0f
      );
    }

    // The soft particles keep the copy bound from the first bucket that needs it to the end of the call, whatever the low-resolution passes do.
    void BindSceneDepth(State& state, const bool bind)
    {
      Variables& vars = GetVariables();
      if (vars.sceneDepth.Exists()) {
        vars.sceneDepth.mEffectVariable->SetDepthStencil(bind || state.softBound ? state.sceneCopy : boost::shared_ptr<gpg::gal::DepthStencilTarget>());
      }
    }

    // A sampled copy of the scene depth: the depth buffer itself stays a depth output (sampling it would move it between states for
    // every run, and some GPUs decompress it each time).
    bool CopySceneDepth(State& state)
    {
      if (state.copyFailed) {
        return false;
      }
      if (state.sceneCopyFresh) {
        return true; // the soft particles took it already in this call; the particle pass writes no depth
      }
      gpg::gal::DepthStencilTargetContext* const source = state.saved.depthStencil->GetContext();
      if (state.sceneCopy == nullptr || state.sceneCopyWidth != source->width_ || state.sceneCopyHeight != source->height_) {
        state.sceneCopy.reset();
        try {
          state.sceneCopy = gpg::gal::DepthStencilTarget::Create(
            gpg::gal::DepthStencilTargetContext(source->width_, source->height_, source->format_, true)
          );
        } catch (const gpg::gal::Error& error) {
          gpg::Warnf("ParticleLowRes: cannot create the scene depth copy: %s", error.what());
          state.copyFailed = true;
          return false;
        }
        state.sceneCopyWidth = source->width_;
        state.sceneCopyHeight = source->height_;
      }
      if (state.sceneCopy == nullptr || !state.device->CopyDepthStencil(*state.sceneCopy, *state.saved.depthStencil)) {
        gpg::Warnf("ParticleLowRes: the backend cannot copy the scene depth; the path stays off");
        state.copyFailed = true;
        return false;
      }
      state.sceneCopyFresh = true;
      return true;
    }

    void BindTexture(ShaderVar& variable, const boost::shared_ptr<gpg::gal::RenderTarget>& target)
    {
      if (variable.Exists()) {
        variable.mEffectVariable->SetRenderTarget(target);
      }
    }

    void BindDepthTexture(ShaderVar& variable, const boost::shared_ptr<gpg::gal::DepthStencilTarget>& target)
    {
      if (variable.Exists()) {
        variable.mEffectVariable->SetDepthStencil(target);
      }
    }

    void SetMode(State& state, const int mode)
    {
      state.currentMode = mode;
      Variables& vars = GetVariables();
      const float side = SizeSetting();
      SetVector(
        vars.params, static_cast<float>(mode), side * side, static_cast<float>(state.savedViewport.Width) * 0.5f,
        static_cast<float>(state.savedViewport.Height) * 0.5f
      );
    }

    // The farthest scene depth of each low-resolution pixel's footprint, into the depth target (once per RenderEffects call).
    bool DownsampleDepth(State& state)
    {
      if (state.depthReady) {
        return state.depthOk;
      }
      state.depthReady = true;
      state.depthOk = false;
      const gpg::gal::RenderProfileScope profileDepth("LRDepth");
      if (!CopySceneDepth(state)) {
        return false;
      }
      state.depthOk = true;
      CD3DDevice* const d3d = D3D_GetDevice();
      gpg::gal::Device* const device = state.device;

      SetSceneUniforms(state, state.savedViewport);
      BindSceneDepth(state, true);

      // The pixel shader writes the depth; the colour target is the accumulation target, with every write masked.
      const gpg::gal::OutputContext output(state.accum, state.depth);
      device->ClearTarget(&output);
      D3DVIEWPORT9 viewport{};
      viewport.Width = state.lowWidth;
      viewport.Height = state.lowHeight;
      viewport.MaxZ = 1.0f;
      device->SetViewport(&viewport);
      device->SetColorWriteState(false, false);
      SetVector(GetVariables().quad, static_cast<float>(state.lowWidth), static_cast<float>(state.lowHeight),
                1.0f / static_cast<float>(state.lowWidth), 1.0f / static_cast<float>(state.lowHeight));
      BindQuad(state.lowQuad, state.lowQuadWidth, state.lowQuadHeight, static_cast<float>(state.lowWidth), static_cast<float>(state.lowHeight));
      SelectAndDraw(*d3d, state.lowQuad, state.scale == 4U ? "LRDownsample4" : "LRDownsample2");

      BindSceneDepth(state, false);
      device->ClearTarget(&state.saved);
      device->SetViewport(&state.savedViewport);
      device->SetColorWriteState(true, false);
      return true;
    }
  } // namespace

  bool ParticleLowResSupported()
  {
    gpg::gal::Device* const device = gpg::gal::Device::GetInstance();
    const gpg::gal::DeviceContext* const context = device != nullptr ? device->GetDeviceContext() : nullptr;
    if (context == nullptr) {
      return false;
    }
    const gpg::gal::DeviceApi api = context->mDeviceType;
    return (api == gpg::gal::DeviceApi::Direct3D11 || api == gpg::gal::DeviceApi::Direct3D12 || api == gpg::gal::DeviceApi::Vulkan) &&
           ParticleEffectPatched();
  }

  bool SoftParticlesSupported()
  {
    return ParticleLowResSupported();
  }

  bool ParticleLowResEligible(const SParticleRenderBucket& bucket) noexcept
  {
    if (bucket.tag.size() < 5U || bucket.tag.compare(0U, 5U, "TRamp", 5U) != 0) {
      return false;
    }
    return bucket.blendMode == 0 || bucket.blendMode == 3 || bucket.blendMode == 4;
  }

  namespace particlelowres
  {
    bool Begin(const GeomCamera3& camera)
    {
      State& state = GetState();
      state.active = false;
      state.lowActive = false;
      state.softActive = false;
      state.softBound = false;
      state.sceneCopyFresh = false;
      const int mode = Mode();
      const float softDistance = SoftDistance();
      if ((mode <= 0 && softDistance <= 0.0f) || !ParticleLowResSupported()) {
        return false;
      }
      gpg::gal::Device* const device = gpg::gal::Device::GetInstance();
      if (device != state.device) {
        state.device = device;
        ReleaseTargets(state);
        state.lowQuad.mVertexSheet = nullptr; // belonged to the old device
        state.sceneQuad.mVertexSheet = nullptr;
        state.lowQuadWidth = state.lowQuadHeight = state.sceneQuadWidth = state.sceneQuadHeight = 0.0f;
      }

      CD3DDevice* const d3d = D3D_GetDevice();
      CD3DEffect* const effect = d3d != nullptr ? d3d->GetCurEffect() : nullptr;
      if (effect == nullptr || !HasTechnique(*effect, "LRComposite") || !HasTechnique(*effect, "LRDownsample2") ||
          !HasTechnique(*effect, "LRDownsample4")) {
        return false;
      }

      device->GetContext(&state.saved);
      if (state.saved.surface == nullptr || state.saved.depthStencil == nullptr || state.copyFailed) {
        state.saved = gpg::gal::OutputContext();
        return false;
      }
      device->GetViewport(&state.savedViewport);
      state.sceneWidth = state.saved.surface->GetContext()->width_;
      state.sceneHeight = state.saved.surface->GetContext()->height_;
      state.softActive = softDistance > 0.0f;
      state.softDistance = softDistance;
      const std::uint32_t scale = mode >= 2 ? 4U : 2U;
      state.lowActive = mode > 0 && state.savedViewport.Width >= 256U && state.savedViewport.Height >= 128U &&
                        EnsureTargets(state, scale, state.savedViewport);
      if (!state.lowActive && !state.softActive) {
        state.saved = gpg::gal::OutputContext();
        return false;
      }
      if (state.lowActive) {
        BindQuad(state.sceneQuad, state.sceneQuadWidth, state.sceneQuadHeight, static_cast<float>(state.sceneWidth),
                 static_cast<float>(state.sceneHeight));
      }

      const float side = SizeSetting();
      state.gate.area = side * side;
      for (int row = 0; row < 4; ++row) {
        state.gate.viewProjection[row * 4 + 0] = camera.viewProjection.r[row].x;
        state.gate.viewProjection[row * 4 + 1] = camera.viewProjection.r[row].y;
        state.gate.viewProjection[row * 4 + 2] = camera.viewProjection.r[row].z;
        state.gate.viewProjection[row * 4 + 3] = camera.viewProjection.r[row].w;
      }
      state.gate.viewportWidth = static_cast<float>(state.savedViewport.Width);
      state.gate.viewportHeight = static_cast<float>(state.savedViewport.Height);
      state.gate.scaleX = camera.projection.r[0].x * static_cast<float>(state.savedViewport.Width) * 0.5f;
      state.gate.scaleY = camera.projection.r[1].y * static_cast<float>(state.savedViewport.Height) * 0.5f;

      state.active = true;
      state.depthReady = false;
      state.runs = 0U;
      SetMode(state, 0);
      return true;
    }

    bool LowResActive()
    {
      return GetState().lowActive;
    }

    void EnsureSoftDepth()
    {
      State& state = GetState();
      if (!state.softActive || state.softBound) {
        return;
      }
      const gpg::gal::RenderProfileScope profileCopy("SoftDepth");
      if (!CopySceneDepth(state)) {
        state.softActive = false;
        return;
      }
      state.softBound = true;
      SetVector(GetVariables().scene, static_cast<float>(state.sceneWidth), static_cast<float>(state.sceneHeight),
                1.0f / static_cast<float>(state.sceneWidth), 1.0f / static_cast<float>(state.sceneHeight));
      SetVector(GetVariables().soft, state.softDistance, 0.0f, 0.0f, 0.0f);
      BindSceneDepth(state, true);
    }

    const LargeQuadGate& Gate()
    {
      return GetState().gate;
    }

    bool WorthIt(const LargeQuadStats& stats)
    {
      static const float envMinimum = [] {
        double value = 0.0;
        return EnvironmentValue("FAF_PARTICLE_LOWRES_MINMPIX", value) ? static_cast<float>(value) : -1.0f;
      }();
      const float minimum = envMinimum >= 0.0f ? envMinimum : ren_ParticleLowResMinMpix;
      return stats.count != 0U && stats.pixels >= minimum * 1.0e6f;
    }

    bool BeginAccumulate()
    {
      State& state = GetState();
      if (!DownsampleDepth(state)) {
        return false;
      }

      gpg::gal::Device* const device = state.device;
      SetSceneUniforms(state, state.savedViewport);
      Variables& vars = GetVariables();
      BindDepthTexture(vars.depth, boost::shared_ptr<gpg::gal::DepthStencilTarget>());
      BindTexture(vars.accum, boost::shared_ptr<gpg::gal::RenderTarget>());

      const gpg::gal::OutputContext output(state.accum, state.depth);
      device->ClearTarget(&output);
      D3DVIEWPORT9 viewport{};
      viewport.Width = state.lowWidth;
      viewport.Height = state.lowHeight;
      viewport.MaxZ = 1.0f;
      device->SetViewport(&viewport);
      device->SetColorWriteState(true, true);
      device->Clear(true, false, false, 0x00000000U, 1.0f, 0);
      SetMode(state, 2);
      ++state.runs;
      state.accumScope.reset();
      state.accumScope.emplace("LRAccum");
      return true;
    }

    void Composite()
    {
      State& state = GetState();
      state.accumScope.reset();
      const gpg::gal::RenderProfileScope profileComposite("LRComposite");
      gpg::gal::Device* const device = state.device;
      CD3DDevice* const d3d = D3D_GetDevice();
      Variables& vars = GetVariables();

      // Output: the scene colour target, no depth (the scene depth is sampled).
      const gpg::gal::OutputContext output(state.saved.surface, boost::shared_ptr<gpg::gal::DepthStencilTarget>());
      device->ClearTarget(&output);
      device->SetViewport(&state.savedViewport);
      device->SetColorWriteState(true, false);

      BindTexture(vars.accum, state.accum);
      BindDepthTexture(vars.depth, state.depth);
      BindSceneDepth(state, true);
      SetVector(vars.quad, static_cast<float>(state.sceneWidth), static_cast<float>(state.sceneHeight),
                1.0f / static_cast<float>(state.sceneWidth), 1.0f / static_cast<float>(state.sceneHeight));
      SelectAndDraw(*d3d, state.sceneQuad, "LRComposite");
      BindSceneDepth(state, false);
      BindTexture(vars.accum, boost::shared_ptr<gpg::gal::RenderTarget>());
      BindDepthTexture(vars.depth, boost::shared_ptr<gpg::gal::DepthStencilTarget>());

      device->ClearTarget(&state.saved);
      device->SetViewport(&state.savedViewport);
      SetMode(state, 0);
    }

    void NoteRun(const float sortScalar, const std::size_t buckets, const LargeQuadStats& stats)
    {
      State& state = GetState();
      if (Debugging() && (state.frames / 2U) % 120U == 0U) {
        gpg::Logf("ParticleLowRes: run sort %g: %u buckets, %u large quads, about %.1f Mpix, rectangle (%.0f %.0f)-(%.0f %.0f)", sortScalar,
                  static_cast<unsigned>(buckets), stats.count, stats.pixels * 1.0e-6f, stats.left, stats.top, stats.right, stats.bottom);
      }
    }

    void SelectQuads(const bool onlySmall)
    {
      State& state = GetState();
      if (state.currentMode != (onlySmall ? 1 : 0)) {
        SetMode(state, onlySmall ? 1 : 0);
      }
    }

    void EndRun()
    {
      State& state = GetState();
      SetMode(state, 0);
    }

    void End()
    {
      State& state = GetState();
      if (!state.active) {
        return;
      }
      state.active = false;
      if (state.softBound) {
        SetVector(GetVariables().soft, 0.0f, 0.0f, 0.0f, 0.0f); // the plain particle shaders for anything else that draws with the effect
        state.softBound = false;
        BindSceneDepth(state, false);
      }
      state.softActive = false;
      state.lowActive = false;
      SetMode(state, 0);
      // The saved output holds the swap chain's back buffer: kept past the frame, ResizeBuffers fails with DXGI_ERROR_INVALID_CALL
      // when the window is resized and the game stops drawing.
      state.saved = gpg::gal::OutputContext();
      ++state.frames;
      state.totalRuns += state.runs;
      if (Debugging() && state.frames % 120U == 0U) {
        gpg::Logf("ParticleLowRes: %u calls, %u runs so far (%u in the last one)", state.frames, state.totalRuns, state.runs);
      }
    }
  } // namespace particlelowres
} // namespace moho
