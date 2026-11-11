// EDITOR: the particle editor's preview (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP8). Upstream's EdParticlePreview (compiled as it is: an emitter
// emitting at a point without a sim, into the game's CWorldParticles) and a viewport after upstream's EdParticleViewport (its own orbit camera and render target)
// WITHOUT the ImGui renderer: the viewport's target is read back and offered to the Maui UI as a dynamic texture under the name `/ed_dyn/fxview`
// (CD3DBatchTexture::FromFile hook, CD3DBatchTexture.h), so any Maui Bitmap of a panel or of a pop-out window shows it with `bitmap:SetTexture('/ed_dyn/fxview')`.
// Direct3D 11 only, like the rest of the editor.
//
//   ed_fxv_view(opts)          -> info     opens / updates the viewport.  opts: pictureWidth, pictureHeight (the target, default 1024 x 640: a Maui texture narrower than 1024 is copied once into the UI's texture atlas and never updated, one of 1024 or more is drawn from its own sheet), background (0 night, 1 grey, 2 sky, 3 black), ground, grid,
//                                          height (the emitter above the ground), turn (the camera circles the effect), playing, loop, scale, show (true: render now)
//   ed_fxv_view_close()                    stops rendering and frees the render target (the texture object stays: panels hold it)
//   ed_fxv_emitter(emitter[, restart])     the emitter the viewport plays (the Lua table of ed_fx_emitter_*); its curves and settings count from the next frame on
//   ed_fxv_flat(flat[, restart])           a beam or trail instead
//   ed_fxv_restart()                       starts over at tick 0 at a fresh spot (the particles already out live on beyond the view)
//   ed_fxv_camera(action[, a, b])          'turn' (dx, dy pixels), 'zoom' (steps), 'look' (heading, pitch), 'reset' (size)
//   ed_fxv_touch()                         "a panel shows the view": the viewport renders for the next second (a panel calls it about four times a second)
//   ed_fxv_stats()                         -> { open, rendering, frames, age, emitted, tick, width, height, lit, texture }    `lit` = pixels of the last picture that differ from the background
//   ed_fxv_world_set(list)                 list = { { id, effect = 'path;path', x, y, z, scale }, ... }: effect decorations played live in the map's view (replaces the previous list; an
//                                          emitter file is read and parsed once and kept); every path is a game file (a mod / map folder's too)
//   ed_fxv_world_clear()                   stops them
//   ed_fxv_world_test(emitter, x, y, z)    plays one emitter table at a place of the map's view (the test of a placement), replacing the last test; call again with nil to stop
//
// The tick clock is the renderer's (REN_GetGameTick), as upstream's preview uses it.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "editor/doc/EdEmitter.h"
#include "editor/doc/EdMapFile.h"
#include "editor/view/EdParticlePreview.h"
#include "gpg/core/utils/Logging.h"
#include "gpg/gal/backends/d3d11/RenderTargetD3D11.h"
#include "moho/editor/EdD3D11Device.h"
#include "moho/editor/EdUpCommon.h"
#include "moho/editor/EdUpFx.h"
#include "moho/math/QuaternionMath.h"
#include "moho/particles/CWorldParticles.h"
#include "moho/render/WRenViewport.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/camera/VTransform.h"
#include "moho/render/d3d/CD3DDepthStencil.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/CD3DPrimBatcher.h"
#include "moho/render/d3d/CD3DRenderTarget.h"
#include "moho/render/textures/CD3DBatchTexture.h"
#include "moho/render/textures/CD3DDynamicTextureSheet.h"

namespace moho
{
  extern boost::shared_ptr<CD3DBatchTexture> (*sDynamicBatchTextureLookup)(const char* name); // CD3DFileBatchTexture.cpp
}

namespace
{
  namespace up = moho::editor::up;
  using faf::editor::EdParticlePreview;
  using faf::editor::EmitterBlueprint;
  using faf::editor::FlatBlueprint;

  constexpr const char* kTextureName = "/ed_dyn/fxview";

  // ---- the viewport's picture as a Maui texture ---------------------------------------------------------------------------------------------------

  /**
   * A B8G8R8A8 picture offered to Maui under a name; `Set` replaces the pixels (and the dynamic sheet's, when it exists). The picture must be 1024 wide or tall (or more):
   * CD3DPrimBatcher::SetTexture packs smaller textures into the shared atlas once, by address, and never looks at them again; a texture of at least half the atlas (1024 of 2048)
   * is drawn from its own sheet every frame.
   */
  class CPictureTexture final : public moho::CD3DBatchTexture
  {
  public:
    CPictureTexture(const std::uint32_t width, const std::uint32_t height)
      : moho::CD3DBatchTexture(width, height, 0u)
      , mPixels(static_cast<std::size_t>(width) * height, 0xFF14171Du)
    {}

    void BuildTextureData(void* const destination, const std::uint32_t destinationPitchBytes) override
    {
      ++mBuilds;
      auto* row = static_cast<std::uint8_t*>(destination);
      for (std::uint32_t y = 0; y < mHeight; ++y) {
        std::memcpy(row, &mPixels[static_cast<std::size_t>(y) * mWidth], static_cast<std::size_t>(mWidth) * 4u);
        row += destinationPitchBytes;
      }
    }

    [[nodiscard]] std::uint8_t GetAlphaAt(const std::uint32_t, const std::uint32_t) const override { return 0xFFu; }
    [[nodiscard]] std::uint32_t GetSheetFormat() const override { return 2u; } // A8R8G8B8

    /** Same size only: the rows of `bgra` (row pitch in bytes) become the picture. */
    void Set(const std::uint8_t* const bgra, const std::uint32_t pitch)
    {
      for (std::uint32_t y = 0; y < mHeight; ++y) {
        std::memcpy(&mPixels[static_cast<std::size_t>(y) * mWidth], bgra + static_cast<std::size_t>(y) * pitch, static_cast<std::size_t>(mWidth) * 4u);
      }
      for (std::uint32_t& pixel : mPixels) {
        pixel |= 0xFF000000u;
      }
      ++mSets;
      if (mTextureSheet) {
        D3DLOCKED_RECT locked{};
        if (mTextureSheet->Lock(reinterpret_cast<std::uint32_t*>(&locked.Pitch), &locked.pBits)) {
          BuildTextureData(locked.pBits, static_cast<std::uint32_t>(locked.Pitch));
          (void)mTextureSheet->Unlock();
          ++mUploads;
        } else {
          ++mLockFailures;
        }
      }
    }

    std::uint32_t mBuilds = 0;      // times a dynamic sheet was filled (by the renderer or by Set)
    std::uint32_t mSets = 0;        // pictures handed over
    std::uint32_t mUploads = 0;     // of them written into the dynamic sheet
    std::uint32_t mLockFailures = 0;

    std::vector<std::uint32_t> mPixels;
  };

  boost::shared_ptr<moho::CD3DBatchTexture> gPicture; // what the name resolves to (null when closed)
  std::uint32_t gLookups = 0;

  boost::shared_ptr<moho::CD3DBatchTexture> LookupDynamic(const char* const name)
  {
    if (name != nullptr && std::strcmp(name, kTextureName) == 0) {
      ++gLookups;
      return gPicture;
    }
    return {};
  }

  // ---- the viewport ---------------------------------------------------------------------------------------------------------------------------------

  /** COORDS_Orient's look turned into a view orientation, as CameraImpl::UpdateCoords does. */
  Wm3::Quaternionf ViewOrient(const Wm3::Quaternionf& look)
  {
    Wm3::Quaternionf orient{};
    orient.w = -look.y;
    orient.x = -look.z;
    orient.y = look.w;
    orient.z = look.x;
    return orient;
  }

  std::uint32_t Background(const int kind)
  {
    switch (kind) {
    case 1: return 0xFF5A5F66u;
    case 2: return 0xFF86A9CCu;
    case 3: return 0xFF000000u;
    default: return 0xFF14171Du;
    }
  }

  struct Viewport
  {
    // options
    int background = 0;
    bool ground = true;
    bool grid = true;
    float height = 0.0f;
    bool turn = false;
    float scale = 1.0f;
    // the picture
    int width = 1024;
    int height2 = 640;
    // the camera
    float heading = 2.4f;
    float pitch = 0.45f;
    float distance = 30.0f;
    // state
    bool open = false;
    int spot = 0;
    std::uint64_t frames = 0;
    std::uint32_t lit = 0;
    std::chrono::steady_clock::time_point touched{};
    std::chrono::steady_clock::time_point lastRender{};
    bool shownAlways = false;
    EdParticlePreview preview;
    bool hasContent = false;
    moho::ID3DDeviceResources::RenderTargetHandle target;
    moho::ID3DDeviceResources::DepthStencilHandle depth;
    int targetWidth = 0;
    int targetHeight = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    int stagingWidth = 0;
    int stagingHeight = 0;

    bool placeOverride = false; // test runs: the effect plays at (overrideX, overrideZ) instead of far off the map
    float overrideX = 0.0f;
    float overrideZ = 0.0f;
    [[nodiscard]] float PlaceX() const noexcept { return placeOverride ? overrideX : -6000.0f - 3000.0f * static_cast<float>(spot); }
    [[nodiscard]] float PlaceZ() const noexcept { return placeOverride ? overrideZ : -6000.0f; }
    [[nodiscard]] bool Active() const
    {
      return open && (shownAlways || std::chrono::steady_clock::now() - touched < std::chrono::milliseconds(1000));
    }
  };

  Viewport& View()
  {
    static Viewport view;
    return view;
  }

  /** The picture of the render target, read back and given to the texture. */
  void ReadBack(Viewport& v, moho::CD3DDevice* const device)
  {
    boost::shared_ptr<gpg::gal::RenderTarget> surface{};
    if (!v.target || !v.target->GetSurface(surface) || !surface) {
      return;
    }
    auto* const d3dSurface = static_cast<gpg::gal::RenderTargetD3D11*>(surface.get());
    ID3D11Texture2D* const texture = d3dSurface != nullptr ? d3dSurface->GetTexture() : nullptr;
    ID3D11Device* const dev = moho::editor::D3D11RawDevice(device->GetGalDevice());
    ID3D11DeviceContext* const ctx = moho::editor::D3D11RawContext(device->GetGalDevice());
    if (texture == nullptr || dev == nullptr || ctx == nullptr) {
      return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || static_cast<int>(desc.Width) != v.width || static_cast<int>(desc.Height) != v.height2) {
      return;
    }
    if (!v.staging || v.stagingWidth != v.width || v.stagingHeight != v.height2) {
      D3D11_TEXTURE2D_DESC sd = desc;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      sd.SampleDesc.Count = 1;
      sd.SampleDesc.Quality = 0;
      v.staging.Reset();
      if (FAILED(dev->CreateTexture2D(&sd, nullptr, &v.staging))) {
        return;
      }
      v.stagingWidth = v.width;
      v.stagingHeight = v.height2;
    }
    ctx->CopyResource(v.staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(v.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return;
    }
    if (auto* const picture = static_cast<CPictureTexture*>(gPicture.get()); picture != nullptr && picture->mWidth == static_cast<std::uint32_t>(v.width) &&
                                                                              picture->mHeight == static_cast<std::uint32_t>(v.height2)) {
      picture->Set(static_cast<const std::uint8_t*>(mapped.pData), mapped.RowPitch);
      const std::uint32_t background = Background(v.background);
      std::uint32_t lit = 0;
      for (const std::uint32_t pixel : picture->mPixels) {
        const std::uint32_t d = ((pixel & 0xFF) > (background & 0xFF) ? (pixel & 0xFF) - (background & 0xFF) : (background & 0xFF) - (pixel & 0xFF)) +
                                (((pixel >> 8) & 0xFF) > ((background >> 8) & 0xFF) ? ((pixel >> 8) & 0xFF) - ((background >> 8) & 0xFF)
                                                                                    : ((background >> 8) & 0xFF) - ((pixel >> 8) & 0xFF)) +
                                (((pixel >> 16) & 0xFF) > ((background >> 16) & 0xFF) ? ((pixel >> 16) & 0xFF) - ((background >> 16) & 0xFF)
                                                                                      : ((background >> 16) & 0xFF) - ((pixel >> 16) & 0xFF));
        if (d > 24) {
          ++lit;
        }
      }
      v.lit = lit;
    }
    ctx->Unmap(v.staging.Get(), 0);
  }

  /** The effect alone from an orbit camera, into a target of its own (after upstream's EdParticleViewport::Render). */
  void RenderViewport(Viewport& v, const int headWidth, const int headHeight)
  {
    moho::CD3DDevice* const device = moho::D3D_GetDevice();
    if (device == nullptr || device->GetResources() == nullptr || moho::ren_Viewport == nullptr) {
      return;
    }
    if (!v.target || !v.depth || v.targetWidth != v.width || v.targetHeight != v.height2) {
      v.target.reset();
      v.depth.reset();
      (void)device->GetResources()->CreateRenderTarget(v.target, v.width, v.height2, 2);
      (void)device->GetResources()->CreateDepthStencil(v.depth, v.width, v.height2, 3);
      v.targetWidth = v.width;
      v.targetHeight = v.height2;
    }
    if (!v.target || !v.depth) {
      return;
    }
    if (v.turn) {
      v.heading += 0.016f * 0.35f;
    }

    const float placeX = v.PlaceX();
    const float placeZ = v.PlaceZ();
    constexpr float kPlaceY = 0.0f;
    const Wm3::Vec3f target{placeX, kPlaceY + v.height + 1.0f, placeZ};
    moho::VTransform view{};
    view.orient_ = ViewOrient(moho::COORDS_Orient(v.heading, v.pitch));
    const Wm3::Vector3f eye = moho::QuatForwardAxis(view.orient_);
    view.pos_ = Wm3::Vec3f{target.x + eye.x * v.distance, target.y + eye.y * v.distance, target.z + eye.z * v.distance};
    const float fov = 0.9f;
    const float nearClip = -std::max(v.distance * 0.01f, 0.05f);
    const float farClip = -(v.distance + 2000.0f); // short of the next spot (3000 away)
    const moho::VMatrix4 projection =
      moho::VEC_D3DProjectionMatrixFOV(fov, fov, nearClip, farClip, static_cast<float>(v.height2) / static_cast<float>(v.width));
    moho::GeomCamera3 camera(view, projection);

    device->SetRenderTarget1(v.target.get(), v.depth.get(), true, static_cast<int>(Background(v.background)), 1.0f, 0);
    Wm3::Vector2i origin{0, 0};
    Wm3::Vector2i size{v.width, v.height2};
    device->SetViewport(&origin, &size, 0.0f, 1.0f);

    moho::CD3DPrimBatcher* const batcher = moho::ren_Viewport->GetPrimBatcher();
    if (batcher != nullptr && (v.ground || v.grid)) {
      (void)batcher->Setup("TAlphaBlendLinearSample");
      batcher->SetViewProjMatrix(camera);
      batcher->SetTexture(moho::CD3DBatchTexture::FromSolidColor(0xFFFFFFFFu));
      const auto vertex = [](const float x, const float y, const float z, const std::uint32_t color) {
        moho::CD3DPrimBatcher::Vertex out{};
        out.mX = x;
        out.mY = y;
        out.mZ = z;
        out.mColor = color;
        return out;
      };
      constexpr float kHalf = 60.0f;
      if (v.ground) {
        const std::uint32_t ground = v.background == 2 ? 0xFF5F6B52u : 0xFF2E3238u;
        const moho::CD3DPrimBatcher::Vertex quad[4] = {vertex(placeX - kHalf, kPlaceY, placeZ - kHalf, ground), vertex(placeX + kHalf, kPlaceY, placeZ - kHalf, ground),
                                                        vertex(placeX + kHalf, kPlaceY, placeZ + kHalf, ground), vertex(placeX - kHalf, kPlaceY, placeZ + kHalf, ground)};
        batcher->DrawQuads(quad, 1);
      }
      if (v.grid) {
        const float y = kPlaceY + 0.02f;
        for (int i = -static_cast<int>(kHalf); i <= static_cast<int>(kHalf); ++i) {
          const std::uint32_t color = i == 0 ? 0xC0D0D0D0u : (i % 5 == 0 ? 0x80A0A0A0u : 0x40808080u);
          const float o = static_cast<float>(i);
          batcher->DrawLine(vertex(placeX + o, y, placeZ - kHalf, color), vertex(placeX + o, y, placeZ + kHalf, color));
          batcher->DrawLine(vertex(placeX - kHalf, y, placeZ + o, color), vertex(placeX + kHalf, y, placeZ + o, color));
        }
        const float h = kPlaceY + v.height;
        batcher->DrawLine(vertex(placeX, kPlaceY, placeZ, 0xFFF0B040u), vertex(placeX, h + 0.01f, placeZ, 0xFFF0B040u));
        batcher->DrawLine(vertex(placeX - 0.5f, h, placeZ, 0xFFF0B040u), vertex(placeX + 0.5f, h, placeZ, 0xFFF0B040u));
        batcher->DrawLine(vertex(placeX, h, placeZ - 0.5f, 0xFFF0B040u), vertex(placeX, h, placeZ + 0.5f, 0xFFF0B040u));
      }
      batcher->Flush();
    }

    // The particles through this camera, both sides of the water surface (as WRenViewport::RenderEffects draws them).
    device->SetColorWriteState(true, false);
    (void)moho::sWorldParticles.RenderEffects(&camera, 0, 0, moho::REN_GetGameTick(), moho::REN_GetSimDeltaSeconds());
    (void)moho::sWorldParticles.RenderEffects(&camera, 1, 0, moho::REN_GetGameTick(), moho::REN_GetSimDeltaSeconds());

    ReadBack(v, device);

    // the head's own viewport back (its target is bound again by the caller)
    Wm3::Vector2i headSize{std::max(headWidth, 1), std::max(headHeight, 1)};
    device->SetViewport(&origin, &headSize, 0.0f, 1.0f);
  }

  // ---- decorations in the map's view ----------------------------------------------------------------------------------------------------------------

  struct Decoration
  {
    std::string effect;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float scale = 1.0f;
    std::vector<std::unique_ptr<EdParticlePreview>> previews;
  };

  std::map<std::string, Decoration>& Decorations()
  {
    static std::map<std::string, Decoration> list;
    return list;
  }

  std::unique_ptr<EdParticlePreview>& TestPreview()
  {
    static std::unique_ptr<EdParticlePreview> preview;
    return preview;
  }

  /** The game's emitter files already read: a path -> its parsed emitter (nullopt when it is no emitter: a beam, a missing file). */
  std::map<std::string, std::unique_ptr<EmitterBlueprint>>& EmitterCache()
  {
    static std::map<std::string, std::unique_ptr<EmitterBlueprint>> cache;
    return cache;
  }

  const EmitterBlueprint* CachedEmitter(const std::string& path)
  {
    auto& cache = EmitterCache();
    const auto found = cache.find(path);
    if (found != cache.end()) {
      return found->second.get();
    }
    std::string text;
    std::unique_ptr<EmitterBlueprint> parsed;
    if (faf::editor::ReadGameFile(path, text)) {
      text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
      auto emitter = std::make_unique<EmitterBlueprint>();
      std::string error;
      if (faf::editor::ParseEmitter(text, *emitter, error)) {
        parsed = std::move(emitter);
      }
    }
    return cache.emplace(path, std::move(parsed)).first->second.get();
  }

  // ---- the frame hook -------------------------------------------------------------------------------------------------------------------------------

  void (*gPreviousHook)(int) = nullptr;
  bool gHooked = false;

  void BeforeFrame(const int head)
  {
    if (gPreviousHook != nullptr) {
      gPreviousHook(head);
    }
    if (head != 0) {
      return;
    }
    // the decorations and the placement test play in the map's view
    for (auto& [id, decoration] : Decorations()) {
      for (auto& preview : decoration.previews) {
        preview->Frame();
      }
    }
    if (TestPreview()) {
      TestPreview()->Frame();
    }
    Viewport& v = View();
    if (!v.Active()) {
      return;
    }
    v.preview.Frame();
    gpg::gal::Device* const gal = gpg::gal::Device::GetInstance();
    const gpg::gal::DeviceContext* const context = gal != nullptr ? gal->GetDeviceContext() : nullptr;
    if (context == nullptr || context->GetHeadCount() < 1) {
      return;
    }
    const gpg::gal::Head& game = context->GetHead(0);
    RenderViewport(v, static_cast<int>(game.mWidth), static_cast<int>(game.mHeight));
    ++v.frames;
  }

  void EnsureHook()
  {
    if (!gHooked) {
      gHooked = true;
      gPreviousHook = moho::gViewportBeforeFrame;
      moho::gViewportBeforeFrame = &BeforeFrame;
      moho::sDynamicBatchTextureLookup = &LookupDynamic;
    }
  }

  // ---- natives --------------------------------------------------------------------------------------------------------------------------------------

  /** Whether t[key] is a number (the stack is left as it was). */
  bool PushField2(lua_State* const L, const int index, const char* const key)
  {
    const bool is = up::PushField(L, index, key) == LUA_TNUMBER;
    lua_settop(L, -2);
    return is;
  }

  void ApplyViewOptions(lua_State* const L, const int index, Viewport& v)
  {
    v.background = std::clamp(up::FieldInt(L, index, "background", v.background), 0, 3);
    v.ground = up::FieldBool(L, index, "ground", v.ground);
    v.grid = up::FieldBool(L, index, "grid", v.grid);
    v.height = static_cast<float>(up::FieldNumber(L, index, "height", static_cast<double>(v.height)));
    v.turn = up::FieldBool(L, index, "turn", v.turn);
    v.scale = static_cast<float>(up::FieldNumber(L, index, "scale", static_cast<double>(v.scale)));
    v.preview.SetScale(v.scale);
    v.preview.SetPlaying(up::FieldBool(L, index, "playing", true));
    v.preview.SetLoop(up::FieldBool(L, index, "loop", true));
    v.shownAlways = up::FieldBool(L, index, "show", v.shownAlways);
    if (PushField2(L, index, "placeX")) {
      v.placeOverride = true;
      v.overrideX = static_cast<float>(up::FieldNumber(L, index, "placeX", 0.0));
      v.overrideZ = static_cast<float>(up::FieldNumber(L, index, "placeZ", 0.0));
    } else if (up::FieldBool(L, index, "placeFar", false)) {
      v.placeOverride = false;
    }
  }

  void PlaceViewPreview(Viewport& v)
  {
    v.preview.SetPlace(v.PlaceX(), 0.0f + v.height, v.PlaceZ());
  }

  int __cdecl cfunc_View(lua_State* const L)
  {
    Viewport& v = View();
    const int width = std::clamp(up::FieldInt(L, 1, "pictureWidth", v.width), 64, 2048);
    const int height = std::clamp(up::FieldInt(L, 1, "pictureHeight", v.height2), 64, 2048);
    // The texture object is made once and kept (Maui bitmaps hold it: a new object would leave them showing a picture nobody updates); only another size makes a new one.
    if (width != v.width || height != v.height2 || !gPicture) {
      v.width = width;
      v.height2 = height;
      gPicture.reset(new CPictureTexture(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
    }
    ApplyViewOptions(L, 1, v);
    PlaceViewPreview(v);
    EnsureHook();
    v.open = true;
    lua_newtable(L);
    up::SetNumber(L, "width", v.width);
    up::SetNumber(L, "height", v.height2);
    up::SetString(L, "texture", kTextureName);
    return 1;
  }

  int __cdecl cfunc_ViewClose(lua_State*)
  {
    Viewport& v = View();
    v.open = false;
    v.shownAlways = false;
    v.target.reset();
    v.depth.reset();
    v.staging.Reset();
    return 0;
  }

  void Restart(Viewport& v)
  {
    v.spot = (v.spot + 1) % 8;
    PlaceViewPreview(v);
    v.preview.Restart();
  }

  int __cdecl cfunc_Emitter(lua_State* const L)
  {
    Viewport& v = View();
    v.preview.SetEmitter(up::FxReadEmitter(L, 1));
    v.hasContent = true;
    if (lua_toboolean(L, 2) != 0) {
      Restart(v);
    }
    return 0;
  }

  int __cdecl cfunc_Flat(lua_State* const L)
  {
    Viewport& v = View();
    v.preview.SetFlat(up::FxReadFlat(L, 1));
    v.hasContent = true;
    if (lua_toboolean(L, 2) != 0) {
      Restart(v);
    }
    return 0;
  }

  int __cdecl cfunc_Restart(lua_State*)
  {
    Restart(View());
    return 0;
  }

  int __cdecl cfunc_Camera(lua_State* const L)
  {
    Viewport& v = View();
    const std::string action = up::ArgString(L, 1, "ed_fxv_camera");
    const float a = lua_type(L, 2) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, 2)) : 0.0f;
    const float b = lua_type(L, 3) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, 3)) : 0.0f;
    if (action == "turn") {
      v.heading += a * 0.01f;
      v.pitch = std::clamp(v.pitch + b * 0.01f, -0.2f, 1.55f);
    } else if (action == "zoom") {
      v.distance = std::clamp(v.distance * std::pow(0.87f, a), 2.0f, 400.0f);
    } else if (action == "look") {
      v.heading = a;
      v.pitch = b;
    } else if (action == "reset") {
      v.heading = 2.4f;
      v.pitch = 0.45f;
      v.distance = std::clamp((a > 0.0f ? a : 12.0f) * 2.5f, 6.0f, 200.0f);
    }
    return 0;
  }

  int __cdecl cfunc_Touch(lua_State*)
  {
    View().touched = std::chrono::steady_clock::now();
    return 0;
  }

  int __cdecl cfunc_Stats(lua_State* const L)
  {
    Viewport& v = View();
    lua_newtable(L);
    up::SetBool(L, "open", v.open);
    up::SetBool(L, "rendering", v.Active());
    up::SetNumber(L, "frames", static_cast<double>(v.frames));
    up::SetNumber(L, "age", v.preview.Age());
    up::SetNumber(L, "emitted", static_cast<double>(v.preview.Emitted()));
    up::SetNumber(L, "tick", moho::REN_GetGameTick());
    up::SetNumber(L, "width", v.width);
    up::SetNumber(L, "height", v.height2);
    up::SetNumber(L, "lit", v.lit);
    up::SetBool(L, "texture", static_cast<bool>(gPicture));
    if (const auto* const picture = static_cast<CPictureTexture*>(gPicture.get()); picture != nullptr) {
      up::SetNumber(L, "pictures", picture->mSets);
      up::SetNumber(L, "uploads", picture->mUploads);
      up::SetNumber(L, "lockFailures", picture->mLockFailures);
      up::SetNumber(L, "builds", picture->mBuilds);
      up::SetBool(L, "sheet", static_cast<bool>(picture->mTextureSheet));
      up::SetNumber(L, "lookups", gLookups);
      up::SetNumber(L, "refs", static_cast<double>(gPicture.use_count()));
    }
    up::SetNumber(L, "decorations", static_cast<double>(Decorations().size()));
    return 1;
  }

  std::vector<std::string> SplitPaths(const std::string& effect)
  {
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from <= effect.size()) {
      const std::size_t to = effect.find(';', from);
      const std::string piece = effect.substr(from, to == std::string::npos ? std::string::npos : to - from);
      if (!piece.empty()) {
        out.push_back(piece);
      }
      if (to == std::string::npos) {
        break;
      }
      from = to + 1;
    }
    return out;
  }

  int __cdecl cfunc_WorldSet(lua_State* const L)
  {
    EnsureHook();
    auto& list = Decorations();
    std::map<std::string, Decoration> next;
    int emitters = 0;
    if (lua_type(L, 1) == LUA_TTABLE) {
      for (int i = 1;; ++i) {
        lua_rawgeti(L, 1, i);
        if (lua_type(L, -1) == LUA_TNIL) {
          lua_settop(L, -2);
          break;
        }
        const int item = lua_gettop(L);
        const std::string id = up::FieldString(L, item, "id", std::to_string(i));
        Decoration d;
        d.effect = up::FieldString(L, item, "effect");
        d.x = static_cast<float>(up::FieldNumber(L, item, "x", 0.0));
        d.y = static_cast<float>(up::FieldNumber(L, item, "y", 0.0));
        d.z = static_cast<float>(up::FieldNumber(L, item, "z", 0.0));
        d.scale = static_cast<float>(up::FieldNumber(L, item, "scale", 1.0));
        const auto old = list.find(id);
        if (old != list.end() && old->second.effect == d.effect && old->second.x == d.x && old->second.y == d.y && old->second.z == d.z && old->second.scale == d.scale) {
          emitters += static_cast<int>(old->second.previews.size());
          next.emplace(id, std::move(old->second)); // unchanged: keeps playing
        } else {
          for (const std::string& path : SplitPaths(d.effect)) {
            if (const EmitterBlueprint* const emitter = CachedEmitter(path); emitter != nullptr) {
              auto preview = std::make_unique<EdParticlePreview>();
              preview->SetEmitter(*emitter);
              preview->SetPlace(d.x, d.y, d.z);
              preview->SetScale(d.scale);
              d.previews.push_back(std::move(preview));
              ++emitters;
            }
          }
          next.emplace(id, std::move(d));
        }
        lua_settop(L, -2);
      }
    }
    list = std::move(next);
    lua_newtable(L);
    up::SetNumber(L, "decorations", static_cast<double>(list.size()));
    up::SetNumber(L, "emitters", emitters);
    return 1;
  }

  int __cdecl cfunc_WorldClear(lua_State*)
  {
    Decorations().clear();
    TestPreview().reset();
    return 0;
  }

  int __cdecl cfunc_WorldTest(lua_State* const L)
  {
    EnsureHook();
    if (lua_type(L, 1) != LUA_TTABLE) {
      TestPreview().reset();
      return 0;
    }
    auto preview = std::make_unique<EdParticlePreview>();
    preview->SetEmitter(up::FxReadEmitter(L, 1));
    preview->SetPlace(static_cast<float>(lua_tonumber(L, 2)), static_cast<float>(lua_tonumber(L, 3)), static_cast<float>(lua_tonumber(L, 4)));
    TestPreview() = std::move(preview);
    return 0;
  }

  const up::Natives gEdFxViewNatives{{
    {"ed_fxv_view", &cfunc_View, "ed_fxv_view(opts) -> { width, height, texture }"},
    {"ed_fxv_view_close", &cfunc_ViewClose, "ed_fxv_view_close()"},
    {"ed_fxv_emitter", &cfunc_Emitter, "ed_fxv_emitter(emitter[, restart])"},
    {"ed_fxv_flat", &cfunc_Flat, "ed_fxv_flat(flat[, restart])"},
    {"ed_fxv_restart", &cfunc_Restart, "ed_fxv_restart()"},
    {"ed_fxv_camera", &cfunc_Camera, "ed_fxv_camera(action[, a, b])"},
    {"ed_fxv_touch", &cfunc_Touch, "ed_fxv_touch()"},
    {"ed_fxv_stats", &cfunc_Stats, "ed_fxv_stats() -> { open, rendering, frames, age, emitted, tick, width, height, lit, texture, decorations }"},
    {"ed_fxv_world_set", &cfunc_WorldSet, "ed_fxv_world_set({ { id, effect, x, y, z, scale }, ... }) -> { decorations, emitters }"},
    {"ed_fxv_world_clear", &cfunc_WorldClear, "ed_fxv_world_clear()"},
    {"ed_fxv_world_test", &cfunc_WorldTest, "ed_fxv_world_test(emitter, x, y, z)"},
  }};
} // namespace
