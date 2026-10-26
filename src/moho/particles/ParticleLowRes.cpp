#include "moho/particles/ParticleLowRes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
  int ren_ParticleLowRes = 0;
  float ren_ParticleLowResSize = 320.0f;

  namespace
  {
    TConVar<int> gTConVar_ren_ParticleLowRes(
      "ren_ParticleLowRes",
      "Draw the large soft particles (explosions, smoke, fire: quads of ren_ParticleLowResSize pixels and more) into a lower-resolution "
      "target and composite it onto the scene, which cuts the blend and fill cost of big explosions. 0 off, 1 half resolution, 2 quarter "
      "resolution. Direct3D 11 and 12 only. The environment variable FAF_PARTICLE_LOWRES overrides it.",
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
      feature.defaultLevel = 0;
      feature.apis[static_cast<int>(Api::D3D9)].reason =
        "Direct3D 9 draws with the shipped effects (no patched particle shaders) and a smaller target would not pay for itself there";
      feature.apis[static_cast<int>(Api::D3D10)].reason = "the Direct3D 10 renderer does not start";
      feature.apis[static_cast<int>(Api::D3D11)].supported = true;
      feature.apis[static_cast<int>(Api::D3D12)].supported = true;
      feature.apis[static_cast<int>(Api::Vulkan)].reason = "not implemented yet: the Vulkan backend cannot sample the scene depth in an effect";
      feature.apply = [](const int level) { ren_ParticleLowRes = level; };
      render::RegisterFeature(feature);
      return true;
    }();

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
      boost::shared_ptr<gpg::gal::RenderTarget> depth; // the downsampled scene depth, packed (RG16)
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
      bool active = false;
      bool depthReady = false;
      gpg::gal::OutputContext saved;
      D3DVIEWPORT9 savedViewport{};
      std::uint32_t sceneWidth = 0U;
      std::uint32_t sceneHeight = 0U;
      float areaThreshold = 0.0f;
      float cameraPosition[3] = {};
      float pixelScaleX = 0.0f; // projection scale times half the viewport: a quad of half size s at depth d is 2*s*scale/d pixels wide
      float pixelScaleY = 0.0f;
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
        // 2: B8G8R8A8; 7: R16G16 (render target formats of the dxgi table).
        state.accum = gpg::gal::RenderTarget::Create(gpg::gal::RenderTargetContext(width, height, 2U));
        state.depth = gpg::gal::RenderTarget::Create(gpg::gal::RenderTargetContext(width, height, 7U));
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

    void BindSceneDepth(State& state, const bool bind)
    {
      Variables& vars = GetVariables();
      if (vars.sceneDepth.Exists()) {
        vars.sceneDepth.mEffectVariable->SetDepthStencil(bind ? state.saved.depthStencil : boost::shared_ptr<gpg::gal::DepthStencilTarget>());
      }
    }

    void BindTexture(ShaderVar& variable, const boost::shared_ptr<gpg::gal::RenderTarget>& target)
    {
      if (variable.Exists()) {
        variable.mEffectVariable->SetRenderTarget(target);
      }
    }

    void SetMode(State& state, const int mode)
    {
      Variables& vars = GetVariables();
      const float side = SizeSetting();
      SetVector(
        vars.params, static_cast<float>(mode), side * side, static_cast<float>(state.savedViewport.Width) * 0.5f,
        static_cast<float>(state.savedViewport.Height) * 0.5f
      );
    }

    // The farthest scene depth of each low-resolution pixel's footprint, into the depth target (once per RenderEffects call).
    void DownsampleDepth(State& state)
    {
      if (state.depthReady) {
        return;
      }
      state.depthReady = true;
      CD3DDevice* const d3d = D3D_GetDevice();
      gpg::gal::Device* const device = state.device;

      SetSceneUniforms(state, state.savedViewport);
      BindSceneDepth(state, true);

      const gpg::gal::OutputContext output(state.depth, boost::shared_ptr<gpg::gal::DepthStencilTarget>());
      device->ClearTarget(&output);
      D3DVIEWPORT9 viewport{};
      viewport.Width = state.lowWidth;
      viewport.Height = state.lowHeight;
      viewport.MaxZ = 1.0f;
      device->SetViewport(&viewport);
      SetVector(GetVariables().quad, static_cast<float>(state.lowWidth), static_cast<float>(state.lowHeight),
                1.0f / static_cast<float>(state.lowWidth), 1.0f / static_cast<float>(state.lowHeight));
      BindQuad(state.lowQuad, state.lowQuadWidth, state.lowQuadHeight, static_cast<float>(state.lowWidth), static_cast<float>(state.lowHeight));
      SelectAndDraw(*d3d, state.lowQuad, state.scale == 4U ? "LRDownsample4" : "LRDownsample2");

      BindSceneDepth(state, false);
      device->ClearTarget(&state.saved);
      device->SetViewport(&state.savedViewport);
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
    return (api == gpg::gal::DeviceApi::Direct3D11 || api == gpg::gal::DeviceApi::Direct3D12) && ParticleEffectPatched();
  }

  bool ParticleLowResEligible(const SParticleRenderBucket& bucket) noexcept
  {
    if (bucket.tag.size() < 5U || bucket.tag.compare(0U, 5U, "TRamp", 5U) != 0) {
      return false;
    }
    return bucket.blendMode == 0 || bucket.blendMode == 3 || bucket.blendMode == 4;
  }

  int ParticleLowResClass(const SParticleRenderBucket& bucket) noexcept
  {
    return bucket.blendMode == 3 ? 0 : 1;
  }

  namespace particlelowres
  {
    bool Begin(const GeomCamera3& camera)
    {
      State& state = GetState();
      state.active = false;
      const int mode = Mode();
      if (mode <= 0 || !ParticleLowResSupported()) {
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
      if (effect == nullptr || !HasTechnique(*effect, "LRComposite_ADD") || !HasTechnique(*effect, "LRDownsample2") ||
          !HasTechnique(*effect, "LRDownsample4")) {
        return false;
      }

      device->GetContext(&state.saved);
      if (state.saved.surface == nullptr || state.saved.depthStencil == nullptr || !state.saved.depthStencil->GetContext()->field0x10_) {
        return false;
      }
      device->GetViewport(&state.savedViewport);
      if (state.savedViewport.Width < 256U || state.savedViewport.Height < 128U) {
        return false;
      }
      state.sceneWidth = state.saved.surface->GetContext()->width_;
      state.sceneHeight = state.saved.surface->GetContext()->height_;
      const std::uint32_t scale = mode >= 2 ? 4U : 2U;
      if (!EnsureTargets(state, scale, state.savedViewport)) {
        return false;
      }
      BindQuad(state.sceneQuad, state.sceneQuadWidth, state.sceneQuadHeight, static_cast<float>(state.sceneWidth),
               static_cast<float>(state.sceneHeight));

      const float side = SizeSetting();
      state.areaThreshold = side * side;
      state.cameraPosition[0] = camera.inverseView.r[3].x;
      state.cameraPosition[1] = camera.inverseView.r[3].y;
      state.cameraPosition[2] = camera.inverseView.r[3].z;
      state.pixelScaleX = camera.projection.r[0].x * static_cast<float>(state.savedViewport.Width) * 0.5f;
      state.pixelScaleY = camera.projection.r[1].y * static_cast<float>(state.savedViewport.Height) * 0.5f;

      state.active = true;
      state.depthReady = false;
      state.runs = 0U;
      SetMode(state, 0);
      return true;
    }

    bool MayHaveLargeQuads(const SParticleRenderBucket& bucket)
    {
      const State& state = GetState();
      return ParticleBucketMayHaveLargeQuads(bucket, state.cameraPosition, state.pixelScaleX, state.pixelScaleY, state.areaThreshold);
    }

    void BeginAccumulate()
    {
      State& state = GetState();
      DownsampleDepth(state);

      gpg::gal::Device* const device = state.device;
      SetSceneUniforms(state, state.savedViewport);
      Variables& vars = GetVariables();
      BindTexture(vars.depth, state.depth);
      BindTexture(vars.accum, boost::shared_ptr<gpg::gal::RenderTarget>());

      const gpg::gal::OutputContext output(state.accum, boost::shared_ptr<gpg::gal::DepthStencilTarget>());
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
    }

    void Composite(const bool overClass)
    {
      State& state = GetState();
      gpg::gal::Device* const device = state.device;
      CD3DDevice* const d3d = D3D_GetDevice();
      Variables& vars = GetVariables();

      // Output: the scene colour target, no depth (the scene depth is sampled).
      const gpg::gal::OutputContext output(state.saved.surface, boost::shared_ptr<gpg::gal::DepthStencilTarget>());
      device->ClearTarget(&output);
      device->SetViewport(&state.savedViewport);
      device->SetColorWriteState(true, false);

      BindTexture(vars.accum, state.accum);
      BindTexture(vars.depth, state.depth);
      BindSceneDepth(state, true);
      SetVector(vars.quad, static_cast<float>(state.sceneWidth), static_cast<float>(state.sceneHeight),
                1.0f / static_cast<float>(state.sceneWidth), 1.0f / static_cast<float>(state.sceneHeight));
      SelectAndDraw(*d3d, state.sceneQuad, overClass ? "LRComposite_OVER" : "LRComposite_ADD");
      BindSceneDepth(state, false);
      BindTexture(vars.accum, boost::shared_ptr<gpg::gal::RenderTarget>());
      BindTexture(vars.depth, boost::shared_ptr<gpg::gal::RenderTarget>());

      device->ClearTarget(&state.saved);
      device->SetViewport(&state.savedViewport);
      SetMode(state, 1);
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
      SetMode(state, 0);
      ++state.frames;
      state.totalRuns += state.runs;
      if (Debugging() && state.frames % 120U == 0U) {
        gpg::Logf("ParticleLowRes: %u calls, %u runs so far (%u in the last one)", state.frames, state.totalRuns, state.runs);
      }
    }
  } // namespace particlelowres
} // namespace moho
