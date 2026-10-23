// EDITOR: the logic of the shadow map cache (engine independent, tested on its own: moho/editor/tests/EdShadowCacheTest.cpp).
//
// The shadow pass of a frame (Shadow::RenderShadowMap) draws every mesh in the light's frustum into a depth map and blurs it twice. In an editor the scene is still most of the time
// (the user looks at it, moves the mouse, types in a panel), and then the map of this frame is the map of the last one. The pass is skipped when everything that decides what it draws
// is the same as when the map in the render target was made:
//   * the light camera (view projection matrix, bit for bit: it follows from the view camera, the sun and the terrain extent),
//   * the shadow settings and the render targets (size, fidelity, blur, and the target objects themselves: a device reset or a changed setting builds new ones),
//   * the scene as MeshRenderer::Batch collected it for the light camera: which instances, with which transform, scale, colour, parameters, dissolve, scroll, and the bones of the
//     skinned ones (a signature over all of them, see Shadow.cpp ShadowSceneSignature),
//   * time: a depth technique that moves with the clock (the wind of the trees: UndulatingDepthClip) makes the map different every frame, such a map is never reused (unless the
//     user asks for it with ren_ShadowCacheTrees: the sway is a fraction of a texel).
// A map is also made again after `maxAge` frames whatever the checks say (a safety net: a stale map can never stay for long).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace moho::editor::shadowcache
{
  /** 64 bit mixing hash over 32 bit words (not cryptographic: the aim is that two different scenes do not give the same value). */
  class Hasher
  {
  public:
    void Add(const std::uint32_t word) noexcept
    {
      state_ ^= word + 0x9E3779B9u + (state_ << 6) + (state_ >> 2);
      state_ *= 0xFF51AFD7ED558CCDull;
      state_ ^= state_ >> 32;
    }

    void AddFloat(const float value) noexcept
    {
      std::uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      Add(bits);
    }

    void AddPointer(const void* const pointer) noexcept
    {
      const auto value = reinterpret_cast<std::uintptr_t>(pointer);
      Add(static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
      Add(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) >> 32));
    }

    void AddBytes(const void* const data, const std::size_t size) noexcept
    {
      const auto* bytes = static_cast<const unsigned char*>(data);
      std::size_t i = 0;
      for (; i + 4 <= size; i += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, bytes + i, 4);
        Add(word);
      }
      std::uint32_t tail = 0;
      std::size_t n = 0;
      for (; i < size; ++i, ++n) {
        tail |= static_cast<std::uint32_t>(bytes[i]) << (8u * static_cast<unsigned>(n));
      }
      if (n != 0) {
        Add(tail ^ 0xA5000000u);
      }
    }

    [[nodiscard]] std::uint64_t Value() const noexcept { return state_; }

  private:
    std::uint64_t state_ = 0x243F6A8885A308D3ull;
  };

  /** Everything the map in the render target depends on. */
  struct Key
  {
    std::uint64_t scene = 0;       // signature of the batched instances and their bones (sum of per instance hashes: the order the spatial db lists them in does not matter)
    float lightViewProjection[16] = {};
    int size = 0;
    int fidelity = 0;
    bool blur = false;
    const void* owner = nullptr;   // the Shadow object
    const void* targets[3] = {};   // shadow map, blur targets A and B (rebuilt targets are other objects)
  };

  [[nodiscard]] inline bool SameKey(const Key& a, const Key& b) noexcept
  {
    return a.scene == b.scene && std::memcmp(a.lightViewProjection, b.lightViewProjection, sizeof(a.lightViewProjection)) == 0 && a.size == b.size && a.fidelity == b.fidelity &&
           a.blur == b.blur && a.owner == b.owner && a.targets[0] == b.targets[0] && a.targets[1] == b.targets[1] && a.targets[2] == b.targets[2];
  }

  struct State
  {
    bool valid = false;
    Key key;
    std::uint8_t timeMask = 0;     // the map that is held was drawn with depth techniques that depend on the clock (kTimeWind, kTimeOther)
    int age = 0;                   // frames the held map has been reused
    bool cleared = false;          // the targets hold the cleared map of a frame without a valid light camera (zoomed out beyond ren_ShadowLOD)
    Key clearedKey;                // which targets were cleared (only owner, size, fidelity, blur and targets count)
    // counters for ed_render_stats (since the start)
    std::uint32_t hits = 0;
    std::uint32_t misses = 0;
  };

  constexpr std::uint8_t kTimeWind = 1;    // UndulatingDepthClip: trees that sway in the wind
  constexpr std::uint8_t kTimeOther = 2;   // a technique this code does not know

  /**
   * Hooks of the mesh code (render thread only). `gPassTimeMask`: MeshRenderer::RenderDepth ORs in the kTime bits of every depth technique it draws with; the shadow pass clears it
   * before the call and reads it after. `gAccumulateBones` / `gBoneSum`: while the flag is set, the bone palette code adds one hash of every instance's bones to the sum (the shadow
   * pass sets the flag around its MeshRenderer::Batch call, which writes the bones of all skinned instances it collects).
   */
  inline std::uint8_t gPassTimeMask = 0;
  inline bool gAccumulateBones = false;
  inline std::uint64_t gBoneSum = 0;

  enum class Decision
  {
    Render,   // draw the map (and Store the result)
    Reuse,    // the render target still holds the right map
  };

  /**
   * Whether this frame's map can be the held one. `enabled` is ren_ShadowCache, `treesStatic` ren_ShadowCacheTrees, `maxAge` ren_ShadowCacheMaxAge (<= 0: no limit).
   * A hit counts one more frame of age; every other outcome leaves the held map as it is (Store replaces it after the pass).
   */
  [[nodiscard]] inline Decision Decide(State& state, const Key& now, const bool enabled, const bool treesStatic, const int maxAge) noexcept
  {
    const std::uint8_t blocking = static_cast<std::uint8_t>(state.timeMask & static_cast<std::uint8_t>(treesStatic ? kTimeOther : (kTimeWind | kTimeOther)));
    const bool timeBlocks = blocking != 0;
    if (enabled && state.valid && !timeBlocks && SameKey(state.key, now) && (maxAge <= 0 || state.age < maxAge)) {
      ++state.age;
      ++state.hits;
      return Decision::Reuse;
    }
    ++state.misses;
    return Decision::Render;
  }

  [[nodiscard]] inline bool SameTargets(const Key& a, const Key& b) noexcept
  {
    return a.size == b.size && a.fidelity == b.fidelity && a.blur == b.blur && a.owner == b.owner && a.targets[0] == b.targets[0] && a.targets[1] == b.targets[1] &&
           a.targets[2] == b.targets[2];
  }

  /**
   * A frame with no valid light camera clears the targets so the shaders see "no shadow". The clear is not repeated while the targets are the same objects and nothing was drawn
   * into them since (a drawn map goes through Store, which forgets the clear).
   */
  [[nodiscard]] inline bool StillCleared(const State& state, const Key& now, const bool enabled) noexcept
  {
    return enabled && state.cleared && SameTargets(state.clearedKey, now);
  }

  inline void MarkCleared(State& state, const Key& now) noexcept
  {
    state.valid = false;
    state.age = 0;
    state.cleared = true;
    state.clearedKey = now;
  }

  /** The pass has just drawn the map for `key`; `timeMask`: the kTime... bits of the depth techniques it used. */
  inline void Store(State& state, const Key& key, const std::uint8_t timeMask) noexcept
  {
    state.valid = true;
    state.key = key;
    state.timeMask = timeMask;
    state.age = 0;
    state.cleared = false;
  }

  /** The held map is gone or can no longer be trusted (targets released, settings changed). */
  inline void Invalidate(State& state) noexcept
  {
    state.valid = false;
    state.age = 0;
    state.cleared = false;
  }

  /**
   * The kTime... bits of one depth technique of mesh.fx. `Depth`, `DepthClip` (transform and albedo alpha only) and `SeraphimBuildDepth` (also the instance's fraction complete
   * parameter, which the signature holds) look the same in every frame of a still scene: 0. `UndulatingDepthClip` moves with the clock (wind): kTimeWind. Any other name: kTimeOther.
   */
  [[nodiscard]] inline std::uint8_t ClassifyDepthTechnique(const char* const name) noexcept
  {
    if (name == nullptr) {
      return kTimeOther;
    }
    if (std::strcmp(name, "Depth") == 0 || std::strcmp(name, "DepthClip") == 0 || std::strcmp(name, "SeraphimBuildDepth") == 0) {
      return 0;
    }
    if (std::strcmp(name, "UndulatingDepthClip") == 0) {
      return kTimeWind;
    }
    return kTimeOther;
  }
} // namespace moho::editor::shadowcache
