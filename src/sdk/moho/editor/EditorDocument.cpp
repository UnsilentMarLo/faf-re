// Engine side of the editor's MapDocument, see EditorDocument.h.
#include "moho/editor/EditorDocument.h"

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
      if (IWldTerrainRes* const res = ActiveTerrain(); res != nullptr) {
        if (CHeightField* const view = res->GetHeightField(); view != nullptr) {
          const gpg::Rect2i r = ToRect2i(rect);
          view->SetElevationRectRaw(r, words, rect.Width());
          view->UpdateError(r);
          gpg::Rect2i dirty = r;
          dirty.x0 = std::max(0, r.x0 - 1);
          dirty.z0 = std::max(0, r.z0 - 1);
          dirty.x1 = r.x1 + 1;
          dirty.z1 = r.z1 + 1;
          res->NotifyMapChange(dirty);
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
        field->UpdateError(r);
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
} // namespace moho::editor
