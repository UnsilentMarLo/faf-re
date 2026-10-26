// Engine side of the editor's MapDocument, see EditorDocument.h.
#include "moho/editor/EditorDocument.h"
#include "moho/editor/EdDirtyRect.h"
#include "moho/editor/EdOverlay.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "gpg/core/containers/Rect2.h"
#include "moho/path/PathTables.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"
#include "moho/sim/ISessionListener.h"
#include "moho/sim/ISessionListener.h"
#include "moho/sim/Sim.h"
#include "moho/terrain/splat/CWldSplat.h"

namespace moho::editor
{
  namespace
  {
    struct PendingHeights
    {
      CellRect rect;
      std::vector<std::uint16_t> words;
    };

    MapDocument gDocument;
    const void* gAttachedTerrain = nullptr;
    CHeightField* gViewField = nullptr;
    std::mutex gQueueMutex;
    std::vector<PendingHeights> gQueue;
    std::atomic<std::size_t> gQueueSize{0};

    // Heights the path finder reads around a cell: dirty a margin so no cluster that could see the change keeps
    // its old answer (same margin as ed_sim_terrain_changed).
    constexpr int kPathMargin = 10;

    [[nodiscard]] gpg::Rect2i ToRect2i(const CellRect& r)
    {
      gpg::Rect2i out{};
      out.x0 = r.x0;
      out.z0 = r.z0;
      out.x1 = r.x1;
      out.z1 = r.z1;
      return out;
    }

    [[nodiscard]] IWldTerrainRes* ActiveTerrain()
    {
      CWldSession* const session = WLD_GetActiveSession();
      if (session == nullptr || session->mWldMap == nullptr) {
        return nullptr;
      }
      return session->mWldMap->mTerrainRes;
    }

    // The projection: view field, shading, mesh refresh now; sim field later through the queue.
    void ProjectHeights(const CellRect& rect, const std::uint16_t* const words)
    {
      moho::edoverlay::NotifyHeights(rect.x0, rect.z0, rect.x1, rect.z1);   // the slope view re-rates what looks at these samples
      if (IWldTerrainRes* const res = ActiveTerrain(); res != nullptr) {
        if (CHeightField* const view = res->GetHeightField(); view != nullptr) {
          const gpg::Rect2i r = ToRect2i(rect);
          view->SetElevationRectRaw(r, words, rect.Width());
          // Everything derived from the heights is recomputed over the changed rectangle GROWN by kDerivedDataPad samples (EdDirtyRect.h): a normal map texel reads the
          // samples i .. i + 1 (a changed sample also changes the texel to its left / top) and the engine reads the end of its rectangle as exclusive; the tier error
          // cells share their corner samples with the cell beside them. A tight rectangle left a one texel line of old normals / old error on its border: a straight
          // crease around every brushed rectangle. Clamped to the field: the field copies below are inclusive.
          const DirtyRect grown = PadDirtyRect(DirtyRect{rect.x0, rect.z0, rect.x1, rect.z1}, kDerivedDataPad, view->Width(), view->Height(), 1);
          gpg::Rect2i dirty{};
          dirty.x0 = grown.x0;
          dirty.z0 = grown.z0;
          dirty.x1 = grown.x1;
          dirty.z1 = grown.z1;
          view->UpdateError(dirty);
          // NotifyMapChange = (finalize once) + UpdateNormalMap + a queue entry that CWldTerrainRes::SyncTerrain later copies out of the SIM field over the view field. In
          // the editor the sim field only receives the edit at its next beat (ApplyPendingHeightEdits), so a sync between the edit and that beat copied the OLD heights over the
          // view (normals already new, mesh old, and a later dab's normals computed from the old heights: borders at the rectangles). The view is the editor's own projection
          // and already holds the heights, so once the terrain is finalized only the normal map is refreshed and no copy is queued.
          if (res->GetBool()) {
            res->UpdateNormalMap(dirty);
          } else {
            res->NotifyMapChange(dirty);
          }
          // The renderer re-tessellates only when the camera moved, the terrain is in edit mode or the decal
          // manager has pending changes (HighFidelityTerrain::UpdateRenderContext): ask for one rebuild.
          if (IDecalManager* const decals = res->GetDecalManager(); decals != nullptr) {
            decals->MarkPendingChanges();
          }
        }
      }
      PendingHeights pending;
      pending.rect = rect;
      pending.words.assign(words, words + rect.Count());
      std::lock_guard<std::mutex> lock(gQueueMutex);
      gQueue.push_back(std::move(pending));
      gQueueSize.store(gQueue.size(), std::memory_order_release);
    }
  } // namespace

  MapDocument& Document()
  {
    return gDocument;
  }

  bool AttachDocument(const bool force)
  {
    IWldTerrainRes* const res = ActiveTerrain();
    CHeightField* const view = res != nullptr ? res->GetHeightField() : nullptr;
    if (view == nullptr) {
      return false;
    }
    const int width = view->Width();
    const int height = view->Height();
    if (!force && gDocument.Valid() && gAttachedTerrain == res && gViewField == view && gDocument.Width() == width &&
        gDocument.Height() == height) {
      return true;
    }
    std::vector<std::uint16_t> words(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::size_t i = 0;
    for (int z = 0; z < height; ++z) {
      for (int x = 0; x < width; ++x, ++i) {
        words[i] = view->GetHeightAt(x, z);
      }
    }
    {
      std::lock_guard<std::mutex> lock(gQueueMutex);
      gQueue.clear();
      gQueueSize.store(0, std::memory_order_release);
    }
    gDocument.Reset(width, height, std::move(words), &ProjectHeights);
    moho::edoverlay::NotifyHeightsAll();
    gAttachedTerrain = res;
    gViewField = view;
    return true;
  }

  std::size_t ApplyPendingHeightEdits(Sim* const sim)
  {
    if (sim == nullptr || gQueueSize.load(std::memory_order_acquire) == 0) {
      return 0;
    }
    std::vector<PendingHeights> work;
    {
      std::lock_guard<std::mutex> lock(gQueueMutex);
      work.swap(gQueue);
      gQueueSize.store(0, std::memory_order_release);
    }
    if (sim->mMapData == nullptr) {
      return 0;
    }
    CHeightField* const field = sim->mMapData->mHeightField.get();
    if (field == nullptr) {
      return 0;
    }
    for (const PendingHeights& edit : work) {
      const gpg::Rect2i r = ToRect2i(edit.rect);
      // outside a live sim the sim and the view may share one field: it was written by the sink already
      if (field != gViewField) {
        field->SetElevationRectRaw(r, edit.words.data(), edit.rect.Width());
        const DirtyRect grown = PadDirtyRect(DirtyRect{edit.rect.x0, edit.rect.z0, edit.rect.x1, edit.rect.z1}, kDerivedDataPad, field->Width(), field->Height(), 1);
        gpg::Rect2i errorRect{};
        errorRect.x0 = grown.x0;
        errorRect.z0 = grown.z0;
        errorRect.x1 = grown.x1;
        errorRect.z1 = grown.z1;
        field->UpdateError(errorRect);
      }
      if (sim->mPathTables != nullptr) {
        gpg::Rect2i dirty{};
        dirty.x0 = std::max(0, r.x0 - kPathMargin);
        dirty.z0 = std::max(0, r.z0 - kPathMargin);
        dirty.x1 = std::min(field->Width() - 1, r.x1 + kPathMargin);
        dirty.z1 = std::min(field->Height() - 1, r.z1 + kPathMargin);
        sim->mPathTables->DirtyClusters(dirty);
      }
    }
    return work.size();
  }

  std::uint64_t SimFieldChecksum(Sim* const sim, const CellRect& requested)
  {
    if (sim == nullptr || sim->mMapData == nullptr || sim->mMapData->mHeightField.get() == nullptr) {
      return 0;
    }
    const CHeightField* const field = sim->mMapData->mHeightField.get();
    // same clamping as MapDocument::Clamp, so the two checksums are comparable for any request
    CellRect rect = requested;
    rect.x0 = std::max(0, rect.x0);
    rect.z0 = std::max(0, rect.z0);
    rect.x1 = std::min(field->Width() - 1, rect.x1);
    rect.z1 = std::min(field->Height() - 1, rect.z1);
    if (rect.Empty()) {
      return 0;
    }
    std::vector<std::uint16_t> words(rect.Count());
    std::size_t i = 0;
    for (int z = rect.z0; z <= rect.z1; ++z) {
      for (int x = rect.x0; x <= rect.x1; ++x, ++i) {
        words[i] = field->GetHeightAt(x, z);
      }
    }
    return MapDocument::ChecksumWords(rect, words.data());
  }

  std::string HexChecksum(const std::uint64_t value)
  {
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
  }

  namespace
  {
    std::vector<void (*)()>& SessionEndHandlers()
    {
      static std::vector<void (*)()> handlers;
      return handlers;
    }
  } // namespace

  void OnSessionEnd(void (*const handler)())
  {
    SessionEndHandlers().push_back(handler);
  }

  void EndSession()
  {
    {
      std::lock_guard<std::mutex> lock(gQueueMutex);
      gQueue.clear();
      gQueueSize.store(0, std::memory_order_release);
    }
    gAttachedTerrain = nullptr;
    gViewField = nullptr;
    for (void (*const handler)() : SessionEndHandlers()) {
      handler();
    }
  }

  namespace
  {
    /** Hooked into WLD_Teardown through the engine's teardown-callback vector (the lane SelectionListener uses). */
    struct EditorSessionListener final : ISessionListener
    {
      void AttachToSessionListenerLane(CWldSession* /*session*/) override {}
      void DetachFromSessionListenerLane(CWldSession* /*session*/) override { EndSession(); }
    };

    [[maybe_unused]] const bool kEditorSessionListenerInit = []() noexcept {
      static EditorSessionListener listener;
      (void)WLD_AddOnTeardownCallback(static_cast<ISessionListener*>(&listener));
      return true;
    }();
  } // namespace
} // namespace moho::editor
