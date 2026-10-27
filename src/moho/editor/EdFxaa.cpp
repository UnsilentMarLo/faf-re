// EDITOR: post-process FXAA for the world view (see EdFxaa.h and docs/projects/editor/FXAA.md).
#include "moho/editor/EdFxaa.h"

#include <d3d9.h>

#include <algorithm>
#include <cstdint>

#include "boost/shared_ptr.h"
#include "gpg/core/utils/Logging.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/DeviceContext.hpp"
#include "gpg/gal/EffectVariable.hpp"
#include "gpg/gal/Error.hpp"
#include "gpg/gal/Head.hpp"
#include "gpg/gal/backends/d3d9/DeviceD3D9.hpp"
#include "moho/console/CConCommand.h"
#include "moho/misc/ID3DDeviceResources.h"
#include "moho/render/CRenFrame.h"
#include "moho/render/ID3DRenderTarget.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/CD3DRenderTarget.h"
#include "moho/render/d3d/ShaderVar.h"

namespace moho
{
  bool ed_fxaa = false;
  float ed_fxaa_subpix = 0.75f;
  float ed_fxaa_edge = 0.166f;
  float ed_fxaa_edge_min = 0.0833f;
  int ed_fxaa_passes = 1;
  bool ed_fxaa_time = false;
} // namespace moho

namespace
{
  moho::TConVar<bool> gTConVar_ed_fxaa(
    "ed_fxaa",
    "Editor: 1 = post-process anti-aliasing (FXAA) of the world view: one full-screen pass, works on every adapter without a multisampled back buffer; the UI stays crisp (default 0)",
    &moho::ed_fxaa
  );
  moho::TConVar<float> gTConVar_ed_fxaa_subpix(
    "ed_fxaa_subpix",
    "Editor: FXAA, how much thin lines and single pixels are blended, 0 to 1 (default 0.75)",
    &moho::ed_fxaa_subpix
  );
  moho::TConVar<float> gTConVar_ed_fxaa_edge(
    "ed_fxaa_edge",
    "Editor: FXAA, edges with less contrast than this fraction of the local maximum are left alone, 0.063 (more edges, softer texture) to 0.333 (fewer) (default 0.166)",
    &moho::ed_fxaa_edge
  );
  moho::TConVar<bool> gTConVar_ed_fxaa_time(
    "ed_fxaa_time",
    "Editor: 1 = the GPU time of the FXAA copy and passes is measured with Direct3D 9 timestamp queries and logged every 120 frames (default 0)",
    &moho::ed_fxaa_time
  );
  moho::TConVar<int> gTConVar_ed_fxaa_passes(
    "ed_fxaa_passes",
    "Editor: FXAA, how often the pass is drawn per frame (1 to 40). Only for measuring: with 20 the frame time minus the frame time without FXAA, divided by 20 (default 1)",
    &moho::ed_fxaa_passes
  );
  moho::TConVar<float> gTConVar_ed_fxaa_edge_min(
    "ed_fxaa_edge_min",
    "Editor: FXAA, edges with less contrast than this (0 to 1 of the luma) are left alone however small the neighbourhood is: raise it to keep fine texture detail (default 0.0833)",
    &moho::ed_fxaa_edge_min
  );

  constexpr int kSceneTargetFormat = 2;   // the format of WRenViewport's head sized targets (kViewportHeadTargetFormat)
  constexpr std::int32_t kTriangleStripPrimitiveToken = 5;
  constexpr std::int32_t kQuadLastVertexIndex = 3;

  // Created on first use, never destroyed at exit (the quad's vertex sheet belongs to the device, which is gone by then); ReleaseDeviceResources empties it.
  struct State
  {
    boost::shared_ptr<moho::CD3DRenderTarget> scene;
    int width = 0;
    int height = 0;
    moho::CRenFrame quad;       // only used for its full-screen vertex sheet
    bool shaderBound = false;   // the texture variable references `scene`
    bool unavailable = false;   // the effect is missing, or the target could not be made: stay off
    bool announced = false;
    // ed_fxaa_time: GPU timestamps around the copy and the passes of the last frame that was measured
    IDirect3DQuery9* qDisjoint = nullptr;
    IDirect3DQuery9* qFreq = nullptr;
    IDirect3DQuery9* qT0 = nullptr;
    IDirect3DQuery9* qT1 = nullptr;
    IDirect3DQuery9* qT2 = nullptr;
    bool timePending = false;
    double sumCopyMs = 0.0;
    double sumPassMs = 0.0;
    int timeFrames = 0;
  };

  State*& StatePtr()
  {
    static State* sState = nullptr;
    return sState;
  }

  State& GetState()
  {
    State*& state = StatePtr();
    if (state == nullptr) {
      state = new State();
    }
    return *state;
  }

  moho::ShaderVar& VarTexture()
  {
    static moho::ShaderVar var{};
    static const bool registered = (moho::RegisterShaderVar("FxaaTexture", &var, "fxaa"), true);
    (void)registered;
    return var;
  }
  moho::ShaderVar& VarFrame()
  {
    static moho::ShaderVar var{};
    static const bool registered = (moho::RegisterShaderVar("fxaaFrame", &var, "fxaa"), true);
    (void)registered;
    return var;
  }
  moho::ShaderVar& VarRect()
  {
    static moho::ShaderVar var{};
    static const bool registered = (moho::RegisterShaderVar("fxaaRect", &var, "fxaa"), true);
    (void)registered;
    return var;
  }
  moho::ShaderVar& VarParams()
  {
    static moho::ShaderVar var{};
    static const bool registered = (moho::RegisterShaderVar("fxaaParams", &var, "fxaa"), true);
    (void)registered;
    return var;
  }

  void SetVec4(moho::ShaderVar& var, const float a, const float b, const float c, const float d)
  {
    if (var.Exists()) {
      const float v[4] = {a, b, c, d};
      var.mEffectVariable->SetFloatArray(4U, v);
    }
  }

  // The head's back buffer is multisampled when the swap chain asks for it: StretchRect from the off-screen target into such a surface is not allowed.
  bool HeadIsMultisampled(const int head)
  {
    if (!gpg::gal::Device::IsReady()) {
      return true;
    }
    gpg::gal::DeviceContext* const context = gpg::gal::Device::GetInstance()->GetDeviceContext();
    if (context == nullptr || context->GetHeadCount() <= head) {
      return true;
    }
    const gpg::gal::Head& h = context->GetHead(static_cast<std::uint32_t>(head));
    return h.antialiasingHigh != 0u || h.antialiasingLow != 0u;
  }

  void ReleaseQueries(State& state)
  {
    for (IDirect3DQuery9** q : {&state.qDisjoint, &state.qFreq, &state.qT0, &state.qT1, &state.qT2}) {
      if (*q != nullptr) {
        (*q)->Release();
        *q = nullptr;
      }
    }
    state.timePending = false;
  }

  IDirect3DDevice9* RawDevice(moho::CD3DDevice* const device)
  {
    return device != nullptr && device->GetGalDevice() != nullptr ? static_cast<gpg::gal::DeviceD3D9*>(device->GetGalDevice())->mDevice : nullptr;
  }

  // Reads the timestamps of the previous measured frame when they are ready; true when a new measurement may be issued.
  bool CollectTimes(State& state, IDirect3DDevice9* const dev)
  {
    if (state.qDisjoint == nullptr) {
      if (dev == nullptr || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &state.qDisjoint)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &state.qFreq))
          || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &state.qT0)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &state.qT1))
          || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &state.qT2))) {
        gpg::Warnf("EDITOR fxaa: this adapter has no timestamp queries; ed_fxaa_time does nothing");
        ReleaseQueries(state);
        moho::ed_fxaa_time = false;
        return false;
      }
      return true;
    }
    if (!state.timePending) {
      return true;
    }
    BOOL disjoint = TRUE;
    UINT64 freq = 0, t0 = 0, t1 = 0, t2 = 0;
    if (state.qDisjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || state.qFreq->GetData(&freq, sizeof(freq), 0) != S_OK || state.qT0->GetData(&t0, sizeof(t0), 0) != S_OK
        || state.qT1->GetData(&t1, sizeof(t1), 0) != S_OK || state.qT2->GetData(&t2, sizeof(t2), 0) != S_OK) {
      return false;   // not ready yet: this frame is not measured
    }
    state.timePending = false;
    if (!disjoint && freq != 0) {
      state.sumCopyMs += 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(freq);
      state.sumPassMs += 1000.0 * static_cast<double>(t2 - t1) / static_cast<double>(freq);
      if (++state.timeFrames >= 120) {
        gpg::Logf("EDITOR fxaa: GPU %.3f ms per frame (copy %.3f + %d pass(es) %.3f), %d frames", (state.sumCopyMs + state.sumPassMs) / state.timeFrames, state.sumCopyMs / state.timeFrames,
                  std::clamp(moho::ed_fxaa_passes, 1, 40), state.sumPassMs / state.timeFrames, state.timeFrames);
        state.sumCopyMs = state.sumPassMs = 0.0;
        state.timeFrames = 0;
      }
    }
    return true;
  }
} // namespace

namespace moho::edfxaa
{
  FrameScope::FrameScope(CD3DDevice* const device, const int head, const bool allowed)
    : mDevice(device)
    , mHead(head)
  {
    if (!allowed || !moho::ed_fxaa || device == nullptr || head < 0 || head > 1) {
      return;
    }
    State& state = GetState();
    if (state.unavailable) {
      return;
    }
    if (HeadIsMultisampled(head)) {
      return;   // swap chain anti-aliasing is on: the user chose that, nothing to add
    }

    const int width = device->GetHeadWidth(static_cast<unsigned int>(head));
    const int height = device->GetHeadHeight(static_cast<unsigned int>(head));
    if (width <= 0 || height <= 0) {
      return;
    }

    try {
      if (!state.scene || state.width != width || state.height != height) {
        if (state.shaderBound) {
          VarTexture().SetRenderTargetTexture(boost::shared_ptr<moho::ID3DRenderTarget>());
          state.shaderBound = false;
        }
        state.scene.reset();
        device->GetResources()->CreateRenderTarget(state.scene, width, height, kSceneTargetFormat);
        if (!state.scene) {
          gpg::Warnf("EDITOR fxaa: the scene target (%d x %d) could not be created; FXAA stays off", width, height);
          state.unavailable = true;
          return;
        }
        state.width = width;
        state.height = height;
        state.quad.InitTransformedVerts(static_cast<float>(width), static_cast<float>(height));
      }
      if (!device->SelectFxFile("fxaa")) {
        gpg::Warnf("EDITOR fxaa: effect file fxaa.fx not found; FXAA stays off");
        state.unavailable = true;
        return;
      }

      boost::shared_ptr<moho::ID3DRenderTarget> headLock{};
      device->GetWriterLock1(headLock, head);
      auto* const lock = static_cast<moho::CD3DRenderTarget*>(headLock.get());
      if (lock == nullptr) {
        return;
      }
      boost::shared_ptr<gpg::gal::RenderTarget> sceneSurface{};
      state.scene->GetSurface(sceneSurface);
      if (!sceneSurface) {
        return;
      }
      mSaved = lock->mSurface;
      lock->mSurface = sceneSurface;
      mHeadLock = lock;
      mActive = true;
      if (!state.announced) {
        state.announced = true;
        gpg::Logf("EDITOR fxaa: on, %d x %d scene target", width, height);
      }
    } catch (const gpg::gal::Error& error) {
      gpg::Warnf("EDITOR fxaa: setup failed (%s); FXAA stays off", error.what());
      GetState().unavailable = true;
      Restore();
    }
  }

  FrameScope::~FrameScope()
  {
    Restore();
  }

  void FrameScope::Restore()
  {
    if (mHeadLock != nullptr) {
      static_cast<moho::CD3DRenderTarget*>(mHeadLock)->mSurface = mSaved;
      mSaved.reset();
      mHeadLock = nullptr;
    }
    mActive = false;
  }

  void FrameScope::AddRect(const int x, const int y, const int width, const int height)
  {
    if (mActive && width > 0 && height > 0) {
      mRects.push_back(Rect{x, y, width, height});
    }
  }

  void FrameScope::Resolve()
  {
    if (!mActive) {
      return;
    }
    State& state = GetState();
    boost::shared_ptr<moho::CD3DRenderTarget> scene = state.scene;
    Restore();   // from here on every pass draws into the real back buffer again

    // the scene is in the off-screen target: all of it goes to the back buffer first (the Maui frames pass, the rest of the window), the world views are smoothed over it
    IDirect3DDevice9* const rawDevice = ed_fxaa_time ? RawDevice(mDevice) : nullptr;
    const bool timed = rawDevice != nullptr && CollectTimes(state, rawDevice);
    if (timed) {
      state.qDisjoint->Issue(D3DISSUE_BEGIN);
      state.qFreq->Issue(D3DISSUE_END);
      state.qT0->Issue(D3DISSUE_END);
    }
    boost::shared_ptr<moho::ID3DRenderTarget> headLock{};
    mDevice->GetWriterLock1(headLock, mHead);
    mDevice->SetViewRect(scene.get(), headLock.get(), nullptr, nullptr);
    if (timed) {
      state.qT1->Issue(D3DISSUE_END);
    }
    if (mRects.empty()) {
      if (timed) {
        state.qT2->Issue(D3DISSUE_END);
        state.qDisjoint->Issue(D3DISSUE_END);
        state.timePending = true;
      }
      return;
    }

    gpg::gal::Device* const gal = gpg::gal::Device::GetInstance();
    D3DVIEWPORT9 savedViewport{};
    savedViewport.MaxZ = 1.0f;
    gal->GetViewport(&savedViewport);

    mDevice->SetRenderTarget2(mHead, false, 0, 1.0f, 0);
    mDevice->SetColorWriteState(true, true);
    mDevice->SelectFxFile("fxaa");
    mDevice->SelectTechnique("TFxaa");

    const float fw = static_cast<float>(state.width);
    const float fh = static_cast<float>(state.height);
    VarTexture().SetRenderTargetTexture(boost::shared_ptr<moho::ID3DRenderTarget>(scene));
    state.shaderBound = true;
    SetVec4(VarFrame(), fw, fh, 1.0f / fw, 1.0f / fh);
    SetVec4(VarParams(), std::clamp(ed_fxaa_subpix, 0.0f, 1.0f), std::clamp(ed_fxaa_edge, 0.02f, 0.5f), std::clamp(ed_fxaa_edge_min, 0.0f, 1.0f), 0.0f);

    for (const Rect& rect : mRects) {
      const int x0 = std::max(rect.x, 0);
      const int y0 = std::max(rect.y, 0);
      const int x1 = std::min(rect.x + rect.w, state.width);
      const int y1 = std::min(rect.y + rect.h, state.height);
      if (x1 <= x0 || y1 <= y0) {
        continue;
      }
      // taps are clamped to the texel centres inside the rectangle, so nothing of the window around the world view bleeds in
      SetVec4(VarRect(), (static_cast<float>(x0) + 0.5f) / fw, (static_cast<float>(y0) + 0.5f) / fh, (static_cast<float>(x1) - 0.5f) / fw, (static_cast<float>(y1) - 0.5f) / fh);
      Wm3::Vector2i pos{x0, y0};
      Wm3::Vector2i size{x1 - x0, y1 - y0};
      mDevice->SetViewport(&pos, &size, 0.0f, 1.0f);

      SD3DVertexRange quadView{};
      quadView.sheet = state.quad.mVertexSheet;
      quadView.startVertex = 0;
      quadView.baseVertex = 0;
      quadView.endVertex = kQuadLastVertexIndex;
      std::int32_t primitiveType = kTriangleStripPrimitiveToken;
      for (int pass = 0, passes = std::clamp(ed_fxaa_passes, 1, 40); pass < passes; ++pass) {
        (void)mDevice->DrawPrimitiveList(&quadView, &primitiveType);   // more than one pass is for measuring only (the same input and output: the same picture)
      }
    }

    // the scene target must not stay referenced by the effect while the frame goes on
    VarTexture().SetRenderTargetTexture(boost::shared_ptr<moho::ID3DRenderTarget>());
    state.shaderBound = false;
    gal->SetViewport(&savedViewport);
    if (timed) {
      state.qT2->Issue(D3DISSUE_END);
      state.qDisjoint->Issue(D3DISSUE_END);
      state.timePending = true;
    }
  }

  void ReleaseDeviceResources()
  {
    State* const state = StatePtr();
    if (state == nullptr) {
      return;
    }
    if (state->shaderBound) {
      try {
        VarTexture().SetRenderTargetTexture(boost::shared_ptr<moho::ID3DRenderTarget>());
      } catch (const gpg::gal::Error&) {
      }
      state->shaderBound = false;
    }
    ReleaseQueries(*state);
    state->scene.reset();
    state->width = 0;
    state->height = 0;
    state->quad.ResetTransientResources();
  }
} // namespace moho::edfxaa
