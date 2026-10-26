#include "moho/render/lights/LightTextureCache.h"

#include <unordered_map>

#include "gpg/core/utils/Logging.h"
#include "moho/misc/FileWaitHandleSet.h"

namespace moho::lights
{
  namespace
  {
    constexpr std::size_t kTextureCacheMax = 512;
    std::unordered_map<std::string, TexEntry> gTextures;
    int gLogBudget = 400;
  } // namespace

const TexEntry* LoadTexture(const std::string& path)
{
  if (path.empty()) {
    return nullptr;
  }
  const auto found = gTextures.find(path);
  if (found != gTextures.end()) {
    return found->second.ok ? &found->second : nullptr;
  }
  if (gTextures.size() >= kTextureCacheMax) {
    gTextures.clear();
  }
  TexEntry entry;
  // The engine's own two steps (CD3DEffect::InitEffectFromFile does the same): the virtual path is resolved to the file or archive entry
  // first, the resolved name is what DISK_MemoryMapFile maps.
  gpg::MemBuffer<const char> file;
  FILE_EnsureWaitHandleSet();
  if (FWaitHandleSet* const waitHandleSet = FILE_GetWaitHandleSet(); waitHandleSet != nullptr && waitHandleSet->mHandle != nullptr) {
    msvc8::string resolved{};
    (void)waitHandleSet->mHandle->FindFile(&resolved, path.c_str(), nullptr);
    if (!resolved.empty()) {
      file = DISK_MemoryMapFile(resolved.c_str());
    }
  }
  const std::size_t fileSize = file.Size();
  bool decoded = false;
  if (fileSize > 0 && fileSize <= (8u << 20)) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(file.GetPtr(0U, 0U));
    decoded = bytes != nullptr && DecodeDds(bytes, fileSize, 256, entry.image);
    entry.ok = decoded;
    if (entry.ok) {
      entry.colour = Summarise(entry.image);
      entry.ok = entry.colour.valid;
    }
  }
  if (gLogBudget > 0) {
    --gLogBudget;
    if (entry.ok) {
      gpg::Logf(
        "DynLights: texture %s %ux%u hue=(%.2f %.2f %.2f) brightness=%.3f peak=%.2f alpha=%.2f",
        path.c_str(),
        entry.image.width,
        entry.image.height,
        entry.colour.hue[0],
        entry.colour.hue[1],
        entry.colour.hue[2],
        entry.colour.brightness,
        entry.colour.peak,
        entry.colour.alpha
      );
    } else {
      gpg::Logf("DynLights: texture %s could not be read (file %u bytes, decoded %d)", path.c_str(), static_cast<unsigned>(fileSize), decoded ? 1 : 0);
    }
  }
  const auto inserted = gTextures.emplace(path, std::move(entry));
  return inserted.first->second.ok ? &inserted.first->second : nullptr;
}
} // namespace moho::lights
