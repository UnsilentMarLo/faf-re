#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "moho/math/Vector3f.h"
#include "moho/render/d3d/CD3DFontTypes.h"

// The seams between the game's engine code and the FAF Studio editor (src/moho/editor, built with -DFAF_EDITOR=ON).
//
// The engine files that the editor extends call `moho::ehook::...` instead of the editor's own functions. Every hook
// is null in the game: the wrappers below then do nothing, or answer what the game does without the editor (the
// frame is not throttled, the UI collection runs every time, no font is an editor font, ...). The editor fills the
// table once, at the start of the process (`moho::editor::InstallEngineHooks`, moho/editor/EdEngineHooks.cpp), so an
// executable built without the editor links with no editor symbol and runs as upstream's engine does.
//
// A call site is one line, marked `// EDITOR:`. The wrappers are inline and the table is a plain struct of function
// pointers (constant-initialised to null, so there is no static initialisation order to care about).
//
// The simulation's own hooks (`gSimBeforeBeat`, `gSimGcDue`, `gSimGcDone`) live in moho/sim/SimEditorHook.h.

namespace moho
{
  class CD3DDevice;
  class CD3DPrimBatcher;
  class CMauiControl;
  class CWaterShaderProperties;
  class Shadow;
  struct GeomCamera3;

  namespace editor::uiclip
  {
    struct Rect;
  }

  /// The editor's own switches of the engine's passes. Off (false) in the game; the editor turns them on and registers
  /// them as console variables (moho/editor/EdRenderStats.cpp). Defined in EditorHooks.cpp.
  extern bool ren_ShadowCache;
  extern bool ren_ShadowCacheTrees;
  extern int ren_ShadowCacheMaxAge;
  extern bool ren_WaterSkipDisabled;
  extern bool ui_CullOffscreen;
  extern bool ui_SkipTransparent;

  /// What the editor puts behind the engine's call sites. A null member is "no editor": see the wrappers.
  struct EditorHooks
  {
    // ---- the editor's text backend: `ed:` faces drawn through DirectWrite (moho/editor/EdTextBackend.h) -------------
    struct Text
    {
      bool (*isEdFace)(const char* face);
      std::string (*familyOf)(const char* face);
      void* (*attach)(const char* face, float pixelSize, float& ascent, float& descent, float& lineGap);
      void (*destroy)(void* state);
      const CD3DFontCharInfo& (*getCharInfo)(void* state, wchar_t chText);
      Vector3f (*render)(
        void* state,
        const char* text,
        CD3DPrimBatcher* primBatcher,
        const Vector3f& origin,
        const Vector3f& xAxis,
        const Vector3f& yAxis,
        std::uint32_t color,
        float maxAdvance
      );
      float (*getAdvance)(void* state, const char* text);
      std::int32_t (*getNearestCharacterIndex)(void* state, const char* text, float targetAdvance);
    } text;

    // ---- frame pacing, timing and the editor's worker pool (moho/editor/EdPerf.h, EdFieldCore.h) -------------------
    struct Perf
    {
      bool (*throttleBeforeFrame)(void* mainWindow);
      void (*frameBegin)();
      void (*frameEnd)();
      std::int64_t (*nowNs)();
      void (*noteIdleWait)(std::int64_t startNs);
      void (*notePresent)(std::int64_t startNs);
      void (*noteRender)(std::int64_t startNs);
      bool (*uiGcDue)(int heapKb, float dtSeconds);
      void (*uiGcDone)(int heapKb);
      bool (*parallelNormalMapEnabled)();
      void (*parallelFor)(int count, const std::function<void(int)>& fn);
    } perf;

    // ---- UI clipping of a control's children (moho/editor/EdUiClip.h) ---------------------------------------------
    struct UiClip
    {
      void (*setClipChildren)(const CMauiControl* control, bool on);
      bool (*getClipChildren)(const CMauiControl* control);
      void (*forget)(const CMauiControl* control);
      bool (*pointVisible)(const CMauiControl* control, float x, float y);
      bool (*any)();
      void (*beginFrame)();
      bool (*effectiveRect)(const CMauiControl* control, editor::uiclip::Rect& out);
    } uiclip;

    // ---- the mesh terrain drawn in place of the height field (moho/editor/EdMeshTerrain.h) ------------------------
    struct Mesh
    {
      void (*noteCamera)(const GeomCamera3& camera);
      bool (*meshWanted)();
      bool (*heightFieldWanted)();
      bool (*drawSurface)(const GeomCamera3& camera, Shadow* shadow, const CWaterShaderProperties* water);
      bool (*drawDepth)(const GeomCamera3& lightCamera);
    } mesh;

    // ---- the editor's windows, overlay, FXAA and icon pass ---------------------------------------------------------
    struct Windows
    {
      void (*popoutRenderAll)();
      void (*popoutDestroyAll)();
      void (*overlayRender)(GeomCamera3* camera, CD3DPrimBatcher* batcher);
      void (*fxaaReleaseDeviceResources)();
      void* (*fxaaBegin)(CD3DDevice* device, int head, bool allowed);
      void (*fxaaAddRect)(void* scope, int x, int y, int width, int height);
      void (*fxaaResolve)(void* scope);
      void (*fxaaEnd)(void* scope);
      bool (*iconPassActive)();
      std::uint32_t (*iconClearArgb)();
    } windows;

    // ---- how the process was started (moho/editor/EdLaunch.h) -------------------------------------------------------
    struct Launch
    {
      bool (*active)();
      void (*reportFailure)(const char* mapName, const char* reason);
    } launch;
  };

  /// The table. All null in the game.
  extern EditorHooks gEditorHooks;

  namespace ehook
  {
    // ---- text -------------------------------------------------------------------------------------------------------
    namespace text
    {
      [[nodiscard]] inline bool IsEdFace(
        const char* face
      )
      {
        return gEditorHooks.text.isEdFace != nullptr && gEditorHooks.text.isEdFace(face);
      }

      [[nodiscard]] inline std::string FamilyOf(
        const char* face
      )
      {
        return gEditorHooks.text.familyOf != nullptr ? gEditorHooks.text.familyOf(face) : std::string(face);
      }

      // The rest are only reached through a font that `Attach` gave a backend, so the hooks exist.
      [[nodiscard]] inline void* Attach(
        const char* face,
        float pixelSize,
        float& ascent,
        float& descent,
        float& lineGap
      )
      {
        return gEditorHooks.text.attach != nullptr ? gEditorHooks.text.attach(face, pixelSize, ascent, descent, lineGap)
                                                   : nullptr;
      }

      inline void Destroy(
        void* state
      )
      {
        gEditorHooks.text.destroy(state);
      }

      [[nodiscard]] inline const CD3DFontCharInfo& GetCharInfo(
        void* state,
        wchar_t chText
      )
      {
        return gEditorHooks.text.getCharInfo(state, chText);
      }

      [[nodiscard]] inline Vector3f Render(
        void* state,
        const char* text,
        CD3DPrimBatcher* primBatcher,
        const Vector3f& origin,
        const Vector3f& xAxis,
        const Vector3f& yAxis,
        std::uint32_t color,
        float maxAdvance
      )
      {
        return gEditorHooks.text.render(state, text, primBatcher, origin, xAxis, yAxis, color, maxAdvance);
      }

      [[nodiscard]] inline float GetAdvance(
        void* state,
        const char* text
      )
      {
        return gEditorHooks.text.getAdvance(state, text);
      }

      [[nodiscard]] inline std::int32_t GetNearestCharacterIndex(
        void* state,
        const char* text,
        float targetAdvance
      )
      {
        return gEditorHooks.text.getNearestCharacterIndex(state, text, targetAdvance);
      }
    } // namespace text

    // ---- perf -------------------------------------------------------------------------------------------------------
    namespace perf
    {
      [[nodiscard]] inline bool ThrottleBeforeFrame(
        void* mainWindow
      )
      {
        return gEditorHooks.perf.throttleBeforeFrame != nullptr && gEditorHooks.perf.throttleBeforeFrame(mainWindow);
      }

      inline void FrameBegin()
      {
        if (gEditorHooks.perf.frameBegin != nullptr) {
          gEditorHooks.perf.frameBegin();
        }
      }

      inline void FrameEnd()
      {
        if (gEditorHooks.perf.frameEnd != nullptr) {
          gEditorHooks.perf.frameEnd();
        }
      }

      [[nodiscard]] inline std::int64_t NowNs()
      {
        return gEditorHooks.perf.nowNs != nullptr ? gEditorHooks.perf.nowNs() : 0;
      }

      inline void NoteIdleWait(
        std::int64_t startNs
      )
      {
        if (gEditorHooks.perf.noteIdleWait != nullptr) {
          gEditorHooks.perf.noteIdleWait(startNs);
        }
      }

      inline void NotePresent(
        std::int64_t startNs
      )
      {
        if (gEditorHooks.perf.notePresent != nullptr) {
          gEditorHooks.perf.notePresent(startNs);
        }
      }

      inline void NoteRender(
        std::int64_t startNs
      )
      {
        if (gEditorHooks.perf.noteRender != nullptr) {
          gEditorHooks.perf.noteRender(startNs);
        }
      }

      /// The UI state's forced collection: the game runs it every time.
      [[nodiscard]] inline bool UiGcDue(
        int heapKb,
        float dtSeconds
      )
      {
        return gEditorHooks.perf.uiGcDue == nullptr || gEditorHooks.perf.uiGcDue(heapKb, dtSeconds);
      }

      inline void UiGcDone(
        int heapKb
      )
      {
        if (gEditorHooks.perf.uiGcDone != nullptr) {
          gEditorHooks.perf.uiGcDone(heapKb);
        }
      }

      [[nodiscard]] inline bool ParallelNormalMapEnabled()
      {
        return gEditorHooks.perf.parallelNormalMapEnabled != nullptr && gEditorHooks.perf.parallelNormalMapEnabled();
      }

      /// fn(0) .. fn(count - 1): on the editor's worker pool, in order on this thread in the game.
      inline void ParallelFor(
        int count,
        const std::function<void(int)>& fn
      )
      {
        if (gEditorHooks.perf.parallelFor != nullptr) {
          gEditorHooks.perf.parallelFor(count, fn);
          return;
        }
        for (int i = 0; i < count; ++i) {
          fn(i);
        }
      }
    } // namespace perf

    // ---- UI clip ----------------------------------------------------------------------------------------------------
    namespace uiclip
    {
      inline void SetClipChildren(
        const CMauiControl* control,
        bool on
      )
      {
        if (gEditorHooks.uiclip.setClipChildren != nullptr) {
          gEditorHooks.uiclip.setClipChildren(control, on);
        }
      }

      [[nodiscard]] inline bool GetClipChildren(
        const CMauiControl* control
      )
      {
        return gEditorHooks.uiclip.getClipChildren != nullptr && gEditorHooks.uiclip.getClipChildren(control);
      }

      inline void Forget(
        const CMauiControl* control
      )
      {
        if (gEditorHooks.uiclip.forget != nullptr) {
          gEditorHooks.uiclip.forget(control);
        }
      }

      /// Whether a point of the control can be seen (no clipping parent in the game: always).
      [[nodiscard]] inline bool PointVisible(
        const CMauiControl* control,
        float x,
        float y
      )
      {
        return gEditorHooks.uiclip.pointVisible == nullptr || gEditorHooks.uiclip.pointVisible(control, x, y);
      }

      [[nodiscard]] inline bool Any()
      {
        return gEditorHooks.uiclip.any != nullptr && gEditorHooks.uiclip.any();
      }

      inline void BeginFrame()
      {
        if (gEditorHooks.uiclip.beginFrame != nullptr) {
          gEditorHooks.uiclip.beginFrame();
        }
      }

      [[nodiscard]] inline bool EffectiveRect(
        const CMauiControl* control,
        editor::uiclip::Rect& out
      )
      {
        return gEditorHooks.uiclip.effectiveRect != nullptr && gEditorHooks.uiclip.effectiveRect(control, out);
      }
    } // namespace uiclip

    // ---- mesh terrain -----------------------------------------------------------------------------------------------
    namespace mesh
    {
      inline void NoteCamera(
        const GeomCamera3& camera
      )
      {
        if (gEditorHooks.mesh.noteCamera != nullptr) {
          gEditorHooks.mesh.noteCamera(camera);
        }
      }

      [[nodiscard]] inline bool MeshWanted()
      {
        return gEditorHooks.mesh.meshWanted != nullptr && gEditorHooks.mesh.meshWanted();
      }

      [[nodiscard]] inline bool HeightFieldWanted()
      {
        return gEditorHooks.mesh.heightFieldWanted == nullptr || gEditorHooks.mesh.heightFieldWanted();
      }

      [[nodiscard]] inline bool DrawSurface(
        const GeomCamera3& camera,
        Shadow* shadow,
        const CWaterShaderProperties* water
      )
      {
        return gEditorHooks.mesh.drawSurface != nullptr && gEditorHooks.mesh.drawSurface(camera, shadow, water);
      }

      [[nodiscard]] inline bool DrawDepth(
        const GeomCamera3& lightCamera
      )
      {
        return gEditorHooks.mesh.drawDepth != nullptr && gEditorHooks.mesh.drawDepth(lightCamera);
      }
    } // namespace mesh

    // ---- windows, overlay, FXAA, icon pass ------------------------------------------------------------------------
    inline void PopoutRenderAll()
    {
      if (gEditorHooks.windows.popoutRenderAll != nullptr) {
        gEditorHooks.windows.popoutRenderAll();
      }
    }

    inline void PopoutDestroyAll()
    {
      if (gEditorHooks.windows.popoutDestroyAll != nullptr) {
        gEditorHooks.windows.popoutDestroyAll();
      }
    }

    inline void OverlayRender(
      GeomCamera3* camera,
      CD3DPrimBatcher* batcher
    )
    {
      if (gEditorHooks.windows.overlayRender != nullptr) {
        gEditorHooks.windows.overlayRender(camera, batcher);
      }
    }

    inline void FxaaReleaseDeviceResources()
    {
      if (gEditorHooks.windows.fxaaReleaseDeviceResources != nullptr) {
        gEditorHooks.windows.fxaaReleaseDeviceResources();
      }
    }

    /// The icon pass (moho/editor/EdIcon.h) renders into the window with matte colours: no water, a clear colour.
    [[nodiscard]] inline bool IconPassActive()
    {
      return gEditorHooks.windows.iconPassActive != nullptr && gEditorHooks.windows.iconPassActive();
    }

    [[nodiscard]] inline std::uint32_t IconClearArgb()
    {
      return gEditorHooks.windows.iconClearArgb != nullptr ? gEditorHooks.windows.iconClearArgb() : 0u;
    }

    /// ed_fxaa: from construction the head's render target is an off-screen scene target (when the editor wants it),
    /// `Resolve` smooths it into the back buffer, the destructor puts the real target back. Without the editor it does
    /// nothing.
    class FxaaScope
    {
    public:
      FxaaScope(
        CD3DDevice* device,
        int head,
        bool allowed
      )
        : mState(
            gEditorHooks.windows.fxaaBegin != nullptr ? gEditorHooks.windows.fxaaBegin(device, head, allowed) : nullptr
          )
      {}

      ~FxaaScope()
      {
        if (mState != nullptr) {
          gEditorHooks.windows.fxaaEnd(mState);
        }
      }

      FxaaScope(const FxaaScope&) = delete;
      FxaaScope& operator=(const FxaaScope&) = delete;

      void AddRect(
        int x,
        int y,
        int width,
        int height
      )
      {
        if (mState != nullptr) {
          gEditorHooks.windows.fxaaAddRect(mState, x, y, width, height);
        }
      }

      void Resolve()
      {
        if (mState != nullptr) {
          gEditorHooks.windows.fxaaResolve(mState);
        }
      }

    private:
      void* mState;
    };

    // ---- launch ----------------------------------------------------------------------------------------------------
    namespace launch
    {
      [[nodiscard]] inline bool Active()
      {
        return gEditorHooks.launch.active != nullptr && gEditorHooks.launch.active();
      }

      inline void ReportFailure(
        const char* mapName,
        const char* reason
      )
      {
        if (gEditorHooks.launch.reportFailure != nullptr) {
          gEditorHooks.launch.reportFailure(mapName, reason);
        }
      }
    } // namespace launch
  } // namespace ehook
} // namespace moho
