// EDITOR: the editor's side of the engine's hook table (moho/app/EditorHooks.h). One place that says which editor
// function answers each call site of the engine.
#include "moho/editor/EdEngineHooks.h"

#include "moho/app/EditorHooks.h"
#include "moho/editor/EdFieldCore.h"
#include "moho/editor/EdFxaa.h"
#include "moho/editor/EdIcon.h"
#include "moho/editor/EdLaunch.h"
#include "moho/editor/EdMeshTerrain.h"
#include "moho/editor/EdOverlay.h"
#include "moho/editor/EdPerf.h"
#include "moho/editor/EdPopout.h"
#include "moho/editor/EdRenderStats.h"
#include "moho/editor/EdTextBackend.h"
#include "moho/editor/EdUiClip.h"

namespace moho::editor
{
  namespace
  {
    void* FxaaBegin(
      CD3DDevice* device,
      int head,
      bool allowed
    )
    {
      return new edfxaa::FrameScope(device, head, allowed);
    }

    void FxaaAddRect(
      void* scope,
      int x,
      int y,
      int width,
      int height
    )
    {
      static_cast<edfxaa::FrameScope*>(scope)->AddRect(x, y, width, height);
    }

    void FxaaResolve(
      void* scope
    )
    {
      static_cast<edfxaa::FrameScope*>(scope)->Resolve();
    }

    void FxaaEnd(
      void* scope
    )
    {
      delete static_cast<edfxaa::FrameScope*>(scope);
    }

    bool IconPassActive()
    {
      return edicon::gActive;
    }

    std::uint32_t IconClearArgb()
    {
      return edicon::gClearArgb;
    }
  } // namespace

  void InstallEngineHooks()
  {
    EditorHooks& h = gEditorHooks;

    h.text.isEdFace = &edtext::IsEdFace;
    h.text.familyOf = &edtext::FamilyOf;
    h.text.attach = &edtext::Attach;
    h.text.destroy = &edtext::Destroy;
    h.text.getCharInfo = &edtext::GetCharInfo;
    h.text.render = &edtext::Render;
    h.text.getAdvance = &edtext::GetAdvance;
    h.text.getNearestCharacterIndex = &edtext::GetNearestCharacterIndex;

    h.perf.throttleBeforeFrame = &perf::ThrottleBeforeFrame;
    h.perf.frameBegin = &perf::FrameBegin;
    h.perf.frameEnd = &perf::FrameEnd;
    h.perf.nowNs = &perf::NowNs;
    h.perf.noteIdleWait = &perf::NoteIdleWait;
    h.perf.notePresent = &perf::NotePresent;
    h.perf.noteRender = &perf::NoteRender;
    h.perf.uiGcDue = &perf::UiGcDue;
    h.perf.uiGcDone = &perf::UiGcDone;
    h.perf.parallelNormalMapEnabled = &perf::ParallelNormalMapEnabled;
    h.perf.parallelFor = &field::ParallelFor;

    h.uiclip.setClipChildren = &uiclip::SetClipChildren;
    h.uiclip.getClipChildren = &uiclip::GetClipChildren;
    h.uiclip.forget = &uiclip::Forget;
    h.uiclip.pointVisible = &uiclip::PointVisible;
    h.uiclip.any = &uiclip::Any;
    h.uiclip.beginFrame = &uiclip::BeginFrame;
    h.uiclip.effectiveRect = &uiclip::EffectiveRect;

    h.mesh.noteCamera = &edmesh::NoteCamera;
    h.mesh.meshWanted = &edmesh::MeshWanted;
    h.mesh.heightFieldWanted = &edmesh::HeightFieldWanted;
    h.mesh.drawSurface = &edmesh::DrawSurface;
    h.mesh.drawDepth = &edmesh::DrawDepth;

    h.windows.popoutRenderAll = &edpopout::RenderAll;
    h.windows.popoutDestroyAll = &edpopout::DestroyAll;
    h.windows.overlayRender = &edoverlay::Render;
    h.windows.fxaaReleaseDeviceResources = &edfxaa::ReleaseDeviceResources;
    h.windows.fxaaBegin = &FxaaBegin;
    h.windows.fxaaAddRect = &FxaaAddRect;
    h.windows.fxaaResolve = &FxaaResolve;
    h.windows.fxaaEnd = &FxaaEnd;
    h.windows.iconPassActive = &IconPassActive;
    h.windows.iconClearArgb = &IconClearArgb;

    h.launch.active = &launch::Active;
    h.launch.reportFailure = &launch::ReportFailure;

    // the editor's switches of the engine's passes (the game keeps them off; the console variables are in
    // EdRenderStats.cpp)
    ren_ShadowCache = true;
    ren_ShadowCacheTrees = false;
    ren_ShadowCacheMaxAge = 300;
    ren_WaterSkipDisabled = true;
    ui_CullOffscreen = true;
    ui_SkipTransparent = true;
  }
} // namespace moho::editor
