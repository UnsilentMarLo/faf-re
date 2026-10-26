#include "moho/render/d3d/CD3DTextureBatcher.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "gpg/core/utils/BoostWrappers.h"
#include "moho/editor/EdRenderStats.h"
#include "moho/misc/ID3DDeviceResources.h"
#include "moho/render/ID3DTextureSheet.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/textures/CD3DBatchTexture.h"
#include "moho/render/textures/CD3DDynamicTextureSheet.h"
#include "moho/render/textures/DeviceExitListener.h"

namespace moho
{
  namespace
  {
    using BatchTextureHandle = boost::shared_ptr<CD3DBatchTexture>;

    // EDITOR: a pointer keyed shortcut in front of the atlas' ordered set (ren_AtlasFastLookup). AddTexture runs once per drawn Maui bitmap / border / glyph; the set walk costs
    // a shared_ptr copy (two atomic operations) and ~10 comparisons each. A texture held by the atlas stays at one address until the atlas is reset (the entry holds a
    // reference to it), and a set node does not move, so the UV rectangle can be found by the texture's address. Dropped on Reset and on destruction.
    struct AtlasFastCache
    {
      const CD3DTextureBatcher* owner = nullptr;
      std::unordered_map<const CD3DBatchTexture*, const gpg::Rect2f*> uv;
    };
    constexpr int kAtlasFastSlots = 4;

    // function local: built on first use, whatever the order of the static initialisers
    AtlasFastCache* AtlasFastSlots()
    {
      static AtlasFastCache slots[kAtlasFastSlots];
      return slots;
    }

    AtlasFastCache* AtlasFastFor(const CD3DTextureBatcher* const atlas, const bool create)
    {
      AtlasFastCache* const slots = AtlasFastSlots();
      AtlasFastCache* freeSlot = nullptr;
      for (AtlasFastCache* slotPtr = slots; slotPtr != slots + kAtlasFastSlots; ++slotPtr) {
        AtlasFastCache& slot = *slotPtr;
        if (slot.owner == atlas) {
          return &slot;
        }
        if (slot.owner == nullptr && freeSlot == nullptr) {
          freeSlot = &slot;
        }
      }
      if (create && freeSlot != nullptr) {
        freeSlot->owner = atlas;
        return freeSlot;
      }
      return nullptr;
    }

    void AtlasFastDrop(const CD3DTextureBatcher* const atlas)
    {
      AtlasFastCache* const slots = AtlasFastSlots();
      for (AtlasFastCache* slotPtr = slots; slotPtr != slots + kAtlasFastSlots; ++slotPtr) {
        AtlasFastCache& slot = *slotPtr;
        if (slot.owner == atlas) {
          slot.uv.clear();
          slot.owner = nullptr;
        }
      }
    }

    constexpr std::int32_t kCompositeWidth = 1024;
    constexpr std::int32_t kCompositeHeight = 1024;
    constexpr std::uint32_t kCompositeBytes = 0x00100000u;
    constexpr std::int32_t kDynamicTextureFormat = 12;

    /**
     * Address: 0x00448A30 (FUN_00448A30)
     *
     * What it does:
     * Copies one batch-texture shared handle into destination scratch storage.
     */
    [[nodiscard]] BatchTextureHandle* AssignBatchTextureHandle(
      BatchTextureHandle* const destination,
      const BatchTextureHandle& source
    )
    {
      *destination = source;
      return destination;
    }


    /**
     * Address: 0x00448E10 (FUN_00448E10)
     *
     * What it does:
     * Releases one batch-texture shared handle scratch lane.
     */
    void ReleaseBatchTextureHandle(BatchTextureHandle* const scratchHandle)
    {
      if (scratchHandle != nullptr) {
        scratchHandle->reset();
      }
    }

    /**
     * Address: 0x004492D0 (FUN_004492D0, sub_4492D0)
     *
     * What it does:
     * Returns whether one 32-bit value lane is zero.
     */
    [[nodiscard]] bool IsZero(const std::uint32_t* const valueLane)
    {
      return valueLane != nullptr && *valueLane == 0u;
    }

    /**
     * Address: 0x007FC170 (FUN_007FC170)
     *
     * What it does:
     * Disposes one `sp_counted_impl_p<CD3DTextureBatcher>` payload by running
     * non-deleting `CD3DTextureBatcher` teardown and releasing owned storage.
     */
    void DisposeCountedTextureBatcherStorage(
      boost::SpCountedImplStorage<CD3DTextureBatcher>* const countedStorage
    )
    {
      CD3DTextureBatcher* const ownedBatcher = countedStorage->px;
      if (ownedBatcher != nullptr) {
        ownedBatcher->~CD3DTextureBatcher();
        ::operator delete(static_cast<void*>(ownedBatcher));
      }
    }
  } // namespace

  /**
   * Address: 0x00448FC0 (FUN_00448FC0)
   *
   * What it does:
   * Orders atlas entries by shared-owner control block, then raw pointee.
   */
  bool CD3DTextureBatcher::TextureAtlasEntryLess::operator()(
    const TextureAtlasEntry& lhs,
    const TextureAtlasEntry& rhs
  ) const noexcept
  {
    const boost::SharedPtrRaw<CD3DBatchTexture> lhsRaw = boost::SharedPtrRawFromSharedBorrow(lhs.mTexture);
    const boost::SharedPtrRaw<CD3DBatchTexture> rhsRaw = boost::SharedPtrRawFromSharedBorrow(rhs.mTexture);

    if (lhsRaw.pi == rhsRaw.pi) {
      return lhsRaw.px < rhsRaw.px;
    }
    return lhsRaw.pi < rhsRaw.pi;
  }

  /**
   * Address: 0x00448A60 (FUN_00448A60)
   *
   * What it does:
   * Initializes the 1024x1024 composite atlas, allocates one byte-packed
   * backing store, and creates one dynamic texture sheet.
   */
  CD3DTextureBatcher::CD3DTextureBatcher()
    : CD3DTextureBatcher(kCompositeWidth, kCompositeHeight, kDynamicTextureFormat)
  {
  }

  /**
   * Editor addition: same body as the retail constructor with a caller-chosen size and gal format token
   * (12 = DXT5, 1 byte per texel; 2 = A8R8G8B8, 4 bytes per texel).
   */
  CD3DTextureBatcher::CD3DTextureBatcher(const std::int32_t width, const std::int32_t height, const std::int32_t format)
    : mWidth(width)
    , mHeight(height)
    , mRects()
    , mDynTexSheet()
    , mMap()
    , mDirty(0)
    , mPad2D{0, 0, 0}
    , mPixels()
    , mFormat(format)
  {
    if (CD3DDevice* const device = D3D_GetDevice(); device != nullptr) {
      if (ID3DDeviceResources* const resources = device->GetResources(); resources != nullptr) {
        (void)resources->NewDynamicTextureSheet(mDynTexSheet, width, height, format);
      }
    }

    const std::size_t bytes = (format == 2)
      ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u
      : static_cast<std::size_t>(kCompositeBytes);
    mPixels.resize(bytes);
    Reset();
  }

  /**
   * Editor addition: the shared A8R8G8B8 atlas for text glyphs and other full-colour batch textures.
   */
  namespace
  {
    // Tells the shared A8R8G8B8 atlas about device exits (reset / shutdown): the sheet content is lost, the CPU copy is not.
    class ArgbAtlasListener final : public Listener<const SD3DDeviceEvent&>
    {
    public:
      explicit ArgbAtlasListener(CD3DTextureBatcher* const atlas)
        : mAtlas(atlas)
      {
        D3D_GetDevice()->AddListener(this);
      }

      void OnEvent(const SD3DDeviceEvent& event) override
      {
        // Type 1 = about to reset or destroy the device. A reset sends it with mShouldReleaseTextures false (CD3DDevice::
        // InitContext), so the flag must not be required: the engine's own save / restore of dynamic sheets across a
        // reset did not preserve this atlas cleanly (stray lines after a resize), a fresh sheet does.
        if (event.mEventType == 1u) {
          mAtlas->OnDeviceExit();
        }
      }

    private:
      CD3DTextureBatcher* mAtlas;
    };
  } // namespace

  CD3DTextureBatcher& CD3DTextureBatcher::GetArgbAtlas()
  {
    static CD3DTextureBatcher* const atlas = [] {
      auto* const created = new CD3DTextureBatcher(2048, 2048, 2);
      (void)new ArgbAtlasListener(created);
      return created;
    }();
    return *atlas;
  }

  void CD3DTextureBatcher::OnDeviceExit()
  {
    // The sheet is gone. Start from an empty atlas: glyph textures are added again (and re-uploaded from their own CPU
    // copies) the next time they are drawn, so no stale rectangle can point into the new sheet.
    mDynTexSheet.reset();
    Reset();
  }

  /**
   * Address: 0x00448B60 (FUN_00448B60)
   *
   * What it does:
   * Releases atlas buffers, tree nodes, and retained dynamic-sheet ownership.
   */
  CD3DTextureBatcher::~CD3DTextureBatcher()
  {
    AtlasFastDrop(this);   // EDITOR
    // FUN_00448B30: the explicit atlas-tree clear ahead of member teardown.
    mMap.clear();
  }

  /**
   * Address: 0x00448C30 (FUN_00448C30)
   *
   * What it does:
   * Returns cached UVs for one batch texture or allocates new atlas space,
   * uploads the texture payload, and stores one new UV mapping.
   */
  const gpg::Rect2f* CD3DTextureBatcher::AddTexture(const boost::shared_ptr<CD3DBatchTexture>& texture)
  {
    if (!texture) {
      return nullptr;
    }

    // EDITOR: shortcut by address (see AtlasFastCache)
    AtlasFastCache* fast = nullptr;
    moho::editor::renderstats::Frame& stats = moho::editor::renderstats::gCur;
    ++stats.atlasLookups;
    if (moho::ren_AtlasFastLookup) {
      fast = AtlasFastFor(this, true);
      if (fast != nullptr) {
        const auto hit = fast->uv.find(texture.get());
        if (hit != fast->uv.end()) {
          ++stats.atlasFastHits;
          return hit->second;
        }
      }
    }

    TextureAtlasEntry lookupEntry{};
    (void)AssignBatchTextureHandle(&lookupEntry.mTexture, texture);

    TextureAtlasSet::iterator found = mMap.find(lookupEntry);
    if (found == mMap.end()) {
      gpg::Rect2i allocatedRect{};
      const int border = static_cast<int>(texture->mBorder);
      const int requiredWidth = (static_cast<int>(texture->mWidth) + (2 * border) + 3) & ~3;
      const int requiredHeight = (static_cast<int>(texture->mHeight) + (2 * border) + 3) & ~3;

      if (!FindRect(allocatedRect, requiredWidth, requiredHeight)) {
        return nullptr;
      }

      AllocateRect(allocatedRect);

      TextureAtlasEntry insertedEntry{};
      BatchTextureHandle insertionScratch;
      (void)AssignBatchTextureHandle(&insertionScratch, texture);
      (void)AssignBatchTextureHandle(&insertedEntry.mTexture, insertionScratch);
      insertedEntry.mUvRect.x0 = static_cast<float>(border + allocatedRect.x0) / static_cast<float>(mWidth);
      insertedEntry.mUvRect.z0 = static_cast<float>(border + allocatedRect.z0) / static_cast<float>(mHeight);
      insertedEntry.mUvRect.x1 = static_cast<float>(allocatedRect.x1 - border) / static_cast<float>(mWidth);
      insertedEntry.mUvRect.z1 = static_cast<float>(allocatedRect.z1 - border) / static_cast<float>(mHeight);

      found = mMap.insert(insertedEntry).first;
      ReleaseBatchTextureHandle(&insertionScratch);
      ++stats.atlasAdds;

      // DXT5 (retail): one byte per texel, so a 4-row block row is 4 * mWidth bytes and the offset is
      // mWidth * z0 + 4 * x0. A8R8G8B8 (editor): 4 bytes per texel, a row is 4 * mWidth bytes.
      const std::size_t destinationOffset = (mFormat == 2)
        ? (static_cast<std::size_t>(mWidth) * 4u * static_cast<std::size_t>(allocatedRect.z0)) +
            (static_cast<std::size_t>(allocatedRect.x0) * 4u)
        : (static_cast<std::size_t>(mWidth) * static_cast<std::size_t>(allocatedRect.z0)) +
            (static_cast<std::size_t>(allocatedRect.x0) * 4u);
      texture->BuildTextureData(mPixels.begin() + destinationOffset, static_cast<std::uint32_t>(4 * mWidth));
      mDirty = 1;
    }

    if (fast != nullptr) {
      fast->uv[texture.get()] = &found->mUvRect;
    }
    return &found->mUvRect;
  }

  /**
   * Address: 0x00448E50 (FUN_00448E50)
   *
   * What it does:
   * Clears atlas mappings/free-rect lanes, restores one full free rectangle,
   * and zeroes the byte buffer.
   */
  void CD3DTextureBatcher::Reset()
  {
    AtlasFastDrop(this);   // EDITOR: the rectangles it points to go with the set
    ++moho::editor::renderstats::gCur.atlasResets;
    mMap.clear();
    mRects.clear();

    const gpg::Rect2i fullRect{0, 0, mWidth, mHeight};
    AddAvailableRect(fullRect);

    if (mPixels.begin() != nullptr) {
      std::memset(mPixels.begin(), 0, static_cast<std::size_t>(mPixels.end() - mPixels.begin()));
    }

    mDirty = 1;
  }

  /**
   * Address: 0x00448EF0 (FUN_00448EF0)
   *
   * What it does:
   * Uploads dirty atlas bytes to the dynamic texture and returns the retained
   * sheet handle as `ID3DTextureSheet`.
   */
  boost::shared_ptr<ID3DTextureSheet> CD3DTextureBatcher::GetCompositeTexture()
  {
    if (!mDynTexSheet && mFormat == 2) {
      // editor: the sheet was released by OnDeviceExit; create it again and upload the whole CPU copy
      if (CD3DDevice* const device = D3D_GetDevice(); device != nullptr) {
        if (ID3DDeviceResources* const resources = device->GetResources(); resources != nullptr) {
          (void)resources->NewDynamicTextureSheet(mDynTexSheet, mWidth, mHeight, mFormat);
          mDirty = 1;
        }
      }
    }
    const std::uint32_t dirtyLane = static_cast<std::uint32_t>(mDirty);
    if (!IsZero(&dirtyLane)) {
      std::uint32_t pitch = 0;
      void* lockedBits = nullptr;

      if (mDynTexSheet && mFormat == 2 && mDynTexSheet->Lock(&pitch, &lockedBits)) {
        // Editor A8R8G8B8 atlas: copy row by row into the locked surface.
        const std::size_t rowBytes = static_cast<std::size_t>(mWidth) * 4u;
        for (std::int32_t row = 0; row < mHeight; ++row) {
          std::copy_n(
            mPixels.begin() + (rowBytes * static_cast<std::size_t>(row)),
            rowBytes,
            static_cast<std::uint8_t*>(lockedBits) + (static_cast<std::size_t>(pitch) * static_cast<std::size_t>(row))
          );
        }
        (void)mDynTexSheet->Unlock();
      } else if (mDynTexSheet && mFormat != 2 && mDynTexSheet->Lock(&pitch, &lockedBits)) {
        // Binary 0x00448EF0 memcpy's raw BYTES (FUN_00A89190): the atlas
        // buffer holds one byte per texel (width*height bytes total, sized
        // 0x100000 by the ctor). Fast path copies the whole width*height-byte
        // blob in one call (single imul of width*height, bytes not pixels);
        // otherwise height/4 blocks of 4*width bytes each, destination
        // strided by pitch. Both paths move exactly width*height bytes.
        if (pitch == static_cast<std::uint32_t>(4 * mWidth)) {
          std::copy_n(
            mPixels.begin(),
            static_cast<std::size_t>(mWidth) * static_cast<std::size_t>(mHeight),
            static_cast<std::uint8_t*>(lockedBits)
          );
        } else {
          const std::uint32_t blockCount = static_cast<std::uint32_t>(mHeight) / 4u;
          const std::size_t blockBytes = static_cast<std::size_t>(mWidth) * 4u;
          for (std::uint32_t block = 0; block < blockCount; ++block) {
            std::copy_n(
              mPixels.begin() + (blockBytes * block),
              blockBytes,
              static_cast<std::uint8_t*>(lockedBits) + (static_cast<std::size_t>(pitch) * block)
            );
          }
        }
        (void)mDynTexSheet->Unlock();
      }

      mDirty = 0;
    }

    boost::shared_ptr<ID3DTextureSheet> outTexture = mDynTexSheet;
    return outTexture;
  }

  /**
   * Address: 0x00448FE0 (FUN_00448FE0)
   *
   * What it does:
   * Selects the best-fit free rectangle (top-most, then left-most) that can
   * fit the requested dimensions.
   */
  bool CD3DTextureBatcher::FindRect(gpg::Rect2i& outRect, const int width, const int height) const
  {
    bool found = false;
    for (gpg::Rect2i* it = mRects.begin(); it != mRects.end(); ++it) {
      const gpg::Rect2i& candidate = *it;
      if (width > (candidate.x1 - candidate.x0)) {
        continue;
      }
      if (height > (candidate.z1 - candidate.z0)) {
        continue;
      }

      if (!found ||
          candidate.z0 < outRect.z0 ||
          (candidate.z0 == outRect.z0 && candidate.x0 < outRect.x0)) {
        outRect.x0 = candidate.x0;
        outRect.z0 = candidate.z0;
        outRect.x1 = candidate.x0 + width;
        outRect.z1 = candidate.z0 + height;
        found = true;
      }
    }

    return found;
  }

  /**
   * Address: 0x00449060 (FUN_00449060, sub_449060)
   *
   * What it does:
   * Removes one allocated rectangle from the free list and re-inserts
   * remaining split fragments.
   */
  void CD3DTextureBatcher::AllocateRect(const gpg::Rect2i& allocatedRect)
  {
    AvailableRectVector currentRects(mRects);
    mRects.clear();

    for (gpg::Rect2i* it = currentRects.begin(); it != currentRects.end(); ++it) {
      const gpg::Rect2i freeRect = *it;

      const bool noStrictOverlap =
        allocatedRect.x0 >= freeRect.x1 ||
        freeRect.x0 >= allocatedRect.x1 ||
        allocatedRect.z0 >= freeRect.z1 ||
        freeRect.z0 >= allocatedRect.z1 ||
        allocatedRect.x0 >= allocatedRect.x1 ||
        allocatedRect.z0 >= allocatedRect.z1 ||
        freeRect.x0 >= freeRect.x1 ||
        freeRect.z0 >= freeRect.z1;

      if (noStrictOverlap) {
        AddAvailableRect(freeRect);
        continue;
      }

      if (freeRect.x0 < allocatedRect.x0) {
        AddAvailableRect({freeRect.x0, freeRect.z0, allocatedRect.x0, freeRect.z1});
      }
      if (freeRect.z0 < allocatedRect.z0) {
        AddAvailableRect({freeRect.x0, freeRect.z0, freeRect.x1, allocatedRect.z0});
      }
      if (freeRect.x1 > allocatedRect.x1) {
        AddAvailableRect({allocatedRect.x1, freeRect.z0, freeRect.x1, freeRect.z1});
      }
      if (freeRect.z1 > allocatedRect.z1) {
        AddAvailableRect({freeRect.x0, allocatedRect.z1, freeRect.x1, freeRect.z1});
      }
    }
  }

  /**
   * Address: 0x004491F0 (FUN_004491F0, Moho::CD3DTextureBatcher::AddAvailableRect)
   *
   * What it does:
   * Inserts one free rectangle while removing dominated fragments and skipping
   * insertion when fully covered by an existing entry.
   */
  void CD3DTextureBatcher::AddAvailableRect(const gpg::Rect2i& rect)
  {
    gpg::Rect2i* writeIt = mRects.begin();

    for (gpg::Rect2i* readIt = mRects.begin(); readIt != mRects.end(); ++readIt) {
      const gpg::Rect2i& current = *readIt;

      if (current.x0 <= rect.x0) {
        if (current.x1 >= rect.x1 &&
            rect.z0 >= current.z0 &&
            current.z1 >= rect.z1) {
          return;
        }
        if (current.x0 < rect.x0) {
          if (writeIt != readIt) {
            *writeIt = current;
          }
          ++writeIt;
          continue;
        }
      }

      if (rect.x1 < current.x1 || current.z0 < rect.z0 || rect.z1 < current.z1) {
        if (writeIt != readIt) {
          *writeIt = current;
        }
        ++writeIt;
      }
    }

    if (writeIt != mRects.end()) {
      mRects.erase(writeIt, mRects.end());
    }

    mRects.push_back(rect);
  }
} // namespace moho
