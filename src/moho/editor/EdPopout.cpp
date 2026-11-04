#include "moho/editor/EdPopout.h"
#include "moho/editor/EdD3D11Device.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "platform/Platform.h"
#include "platform/WxWidgets.h"

#include <wx/dcclient.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/window.h>

#include <d3d11.h>
#include <wincodec.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "boost/shared_ptr.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/Error.hpp"
#include "gpg/gal/OutputContext.hpp"
#include "gpg/gal/backends/d3d11/RenderTargetD3D11.h"
#include "gpg/core/utils/Logging.h"
#include "lua/LuaRuntimeTypes.h"
#include "lua/LuaPrimitives.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/app/WxRuntimeTypes.h"
#include "moho/misc/Listener.h"
#include "moho/render/WRenViewport.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/CD3DPrimBatcher.h"
#include "moho/render/d3d/CD3DRenderTarget.h"
#include "moho/misc/ID3DDeviceResources.h"
#include "moho/render/textures/DeviceExitListener.h"
#include "moho/ui/CUIManager.h"
#include "moho/ui/IUIManager.h"

namespace moho
{
  namespace
  {
    struct Popout;

    // A pop-out is redrawn when it is dirty (creation, resize, input, a device reset, `ed_window_invalidate` from Lua) and at
    // least every kHeartbeatMs otherwise, so Lua-driven changes without an explicit invalidate still show up quickly.
    constexpr unsigned long long kHeartbeatMs = 100;
    constexpr int kDirtyFrames = 3; // a change is drawn for a few frames (layout settles, hover states)

    struct Event
    {
      int id;
      std::string kind;
      int a;
      int b;
    };

    std::deque<Event> gEvents;

    void PostEvent(const int id, const char* const kind, const int a, const int b, const bool coalesce)
    {
      if (id < 0) {
        return; // the window is still being created
      }
      if (coalesce) {
        for (auto it = gEvents.rbegin(); it != gEvents.rend(); ++it) {
          if (it->id == id && it->kind == kind) {
            it->a = a;
            it->b = b;
            return;
          }
        }
      }
      if (gEvents.size() > 4096u) {
        gEvents.pop_front();
      }
      gEvents.push_back(Event{id, kind, a, b});
    }

    class EdPopoutView final : public wxWindow
    {
    public:
      EdPopoutView(wxWindow* const parent, Popout* const owner)
        : wxWindow(parent, -1, wxDefaultPosition, wxDefaultSize, 0, wxT("edpopout"))
        , mOwner(owner)
      {}

      void OnPaint(wxPaintEvent& event)
      {
        (void)event;
        wxPaintDC dc(this); // validates the update region; the engine draws the content
        MarkDirty();
      }

      void OnEraseBackground(wxEraseEvent& event) { (void)event; }

      long MSWWindowProc(const WXUINT message, const WXWPARAM wParam, const WXLPARAM lParam) override
      {
        // Input and focus changes mean the UI may look different next frame (hover, caret, selection).
        if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR ||
            message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_MOUSELEAVE) {
          MarkDirty();
        }
        return wxWindow::MSWWindowProc(message, wParam, lParam);
      }

      void MarkDirty();

      Popout* mOwner;

      DECLARE_EVENT_TABLE()
    };

    BEGIN_EVENT_TABLE(EdPopoutView, wxWindow)
      EVT_PAINT(EdPopoutView::OnPaint)
      EVT_ERASE_BACKGROUND(EdPopoutView::OnEraseBackground)
    END_EVENT_TABLE()

    class EdPopoutFrame final : public wxFrame
    {
    public:
      EdPopoutFrame(const wxString& title, const wxPoint& position, const wxSize& size, Popout* const owner)
        : wxFrame(nullptr, -1, title, position, size, wxDEFAULT_FRAME_STYLE)
        , mOwner(owner)
      {}

      void OnClose(wxCloseEvent& event);
      void OnSize(wxSizeEvent& event);
      void OnMove(wxMoveEvent& event);

      Popout* mOwner;

      DECLARE_EVENT_TABLE()
    };

    BEGIN_EVENT_TABLE(EdPopoutFrame, wxFrame)
      EVT_CLOSE(EdPopoutFrame::OnClose)
      EVT_SIZE(EdPopoutFrame::OnSize)
      EVT_MOVE(EdPopoutFrame::OnMove)
    END_EVENT_TABLE()

    struct Popout
    {
      int id = -1; // = the Maui root-frame index
      EdPopoutFrame* frame = nullptr;
      EdPopoutView* view = nullptr;
      Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;        // the window's own flip-model swap chain on the game's Direct3D 11 device
      boost::shared_ptr<CD3DRenderTarget> target;          // what the frame is drawn into (the engine's targets, copied to the chain's buffer)
      int chainWidth = 0;
      int chainHeight = 0;
      int dirty = kDirtyFrames;
      unsigned long long lastDrawMs = 0;
      bool deferClose = false; // true: the X button only posts a 'close' event, Lua decides (re-dock panels, then destroy)
      std::string capturePath;
      std::string lastError;
    };

    std::map<int, Popout*> gPopouts;
    unsigned long gFramesDrawn = 0;

    void EdPopoutView::MarkDirty()
    {
      if (mOwner != nullptr) {
        mOwner->dirty = kDirtyFrames;
      }
    }

    CUIManager* UiManager()
    {
      return static_cast<CUIManager*>(UI_GetManager());
    }

    ID3D11Device* RawDevice()
    {
      CD3DDevice* const device = D3D_GetDevice();
      if (device == nullptr || device->GetGalDevice() == nullptr) {
        return nullptr;
      }
      return moho::editor::D3D11RawDevice(device->GetGalDevice());
    }

    ID3D11DeviceContext* RawContext()
    {
      CD3DDevice* const device = D3D_GetDevice();
      if (device == nullptr || device->GetGalDevice() == nullptr) {
        return nullptr;
      }
      return moho::editor::D3D11RawContext(device->GetGalDevice());
    }

    std::wstring Widen(const char* text);

    // The window's picture as a PNG (what ed_window_capture writes): the target's texture is copied to a staging texture and encoded by WIC. The format is the
    // swap chain's, B8G8R8A8; the alpha is made opaque (the PNG encoder takes 32bppBGRA).
    bool CapturePng(ID3D11Device* const dev, ID3D11DeviceContext* const ctx, ID3D11Texture2D* const texture, const std::string& path, std::string& error)
    {
      D3D11_TEXTURE2D_DESC desc{};
      texture->GetDesc(&desc);
      if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        error = "capture: the target is not B8G8R8A8";
        return false;
      }
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
      if (FAILED(dev->CreateTexture2D(&desc, nullptr, &staging))) {
        error = "capture: no staging texture";
        return false;
      }
      ctx->CopyResource(staging.Get(), texture);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "capture: Map failed";
        return false;
      }
      bool ok = false;
      HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // already initialized by the window system: S_FALSE / RPC_E_CHANGED_MODE are fine
      (void)hr;
      Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
      Microsoft::WRL::ComPtr<IWICStream> stream;
      Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
      Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
      Microsoft::WRL::ComPtr<IPropertyBag2> properties;
      WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
      std::vector<std::uint8_t> opaque(static_cast<std::size_t>(desc.Width) * desc.Height * 4u);
      for (UINT row = 0; row < desc.Height; ++row) {
        const std::uint8_t* const src = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch;
        std::uint8_t* const dst = opaque.data() + static_cast<std::size_t>(row) * desc.Width * 4u;
        std::memcpy(dst, src, static_cast<std::size_t>(desc.Width) * 4u);
        for (UINT x = 0; x < desc.Width; ++x) {
          dst[x * 4u + 3u] = 0xFF;
        }
      }
      const std::wstring widePath = Widen(path.c_str());
      if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->CreateStream(&stream)) &&
          SUCCEEDED(stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE)) && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
          SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)) && SUCCEEDED(frame->Initialize(properties.Get())) &&
          SUCCEEDED(frame->SetSize(desc.Width, desc.Height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
          SUCCEEDED(frame->WritePixels(desc.Height, desc.Width * 4u, static_cast<UINT>(opaque.size()), opaque.data())) && SUCCEEDED(frame->Commit()) &&
          SUCCEEDED(encoder->Commit())) {
        ok = true;
      } else {
        error = "capture: the PNG could not be written";
      }
      ctx->Unmap(staging.Get(), 0);
      return ok;
    }

    void ReleaseChain(Popout& p)
    {
      p.target.reset();
      p.chain.Reset();
      p.chainWidth = 0;
      p.chainHeight = 0;
    }

    // The swap chain and the target are made at the window's real client size (wx 2.4.2 reports a few pixels more than asked).
    bool CreateChain(Popout& p, const int width, const int height)
    {
      ID3D11Device* const dev = RawDevice();
      CD3DDevice* const device = D3D_GetDevice();
      if (dev == nullptr || device == nullptr || width <= 0 || height <= 0) {
        return false;
      }
      ReleaseChain(p);

      // the target the engine draws into: the same kind of target the main frame's scene uses (format 2, as WRenViewport's head sized targets)
      constexpr int kPopoutTargetFormat = 2;
      device->GetResources()->CreateRenderTarget(p.target, width, height, kPopoutTargetFormat);
      boost::shared_ptr<gpg::gal::RenderTarget> surface{};
      if (p.target) {
        p.target->GetSurface(surface);
      }
      auto* const surfaceD3D11 = static_cast<gpg::gal::RenderTargetD3D11*>(surface.get());
      if (surfaceD3D11 == nullptr || surfaceD3D11->GetTexture() == nullptr) {
        ReleaseChain(p);
        p.lastError = "the pop-out target could not be created";
        return false;
      }
      D3D11_TEXTURE2D_DESC targetDesc{};
      surfaceD3D11->GetTexture()->GetDesc(&targetDesc);

      // the window's flip-model swap chain on the factory of the device's adapter
      Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
      Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
      Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
      if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        ReleaseChain(p);
        p.lastError = "no DXGI factory for the device";
        return false;
      }
      DXGI_SWAP_CHAIN_DESC1 desc{};
      desc.Width = static_cast<UINT>(width);
      desc.Height = static_cast<UINT>(height);
      desc.Format = targetDesc.Format;   // the copy needs the same format as the target
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      desc.Scaling = DXGI_SCALING_NONE;
      desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
      const HRESULT hr = factory->CreateSwapChainForHwnd(dev, reinterpret_cast<HWND>(p.view->GetHWND()), &desc, nullptr, nullptr, &p.chain);
      if (FAILED(hr) || !p.chain) {
        char text[96];
        std::snprintf(text, sizeof(text), "CreateSwapChainForHwnd failed 0x%08lX (format %u)", static_cast<unsigned long>(hr), static_cast<unsigned>(targetDesc.Format));
        p.lastError = text;
        ReleaseChain(p);
        return false;
      }
      (void)factory->MakeWindowAssociation(reinterpret_cast<HWND>(p.view->GetHWND()), DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
      gpg::Logf("EDITOR popout %d: swap chain %dx%d, DXGI format %u, flip-discard", p.id, width, height, static_cast<unsigned>(targetDesc.Format));
      p.chainWidth = width;
      p.chainHeight = height;
      p.dirty = kDirtyFrames;
      p.lastError.clear();
      return true;
    }

    // Device reset / shutdown: the engine's render targets are released on the exit event (type 1), so the pop-outs' targets go with them and are made again lazily by
    // RenderAll (their swap chains would survive, they are released with them for simplicity).
    class DeviceListener final : public Listener<const SD3DDeviceEvent&>
    {
    public:
      DeviceListener() { D3D_GetDevice()->AddListener(this); }

      void OnEvent(const SD3DDeviceEvent& event) override
      {
        if (event.mEventType == 1u) {
          for (auto& entry : gPopouts) {
            ReleaseChain(*entry.second);
          }
        }
      }
    };

    void EnsureDeviceListener()
    {
      static DeviceListener* const listener = new DeviceListener();
      (void)listener;
    }

    void DestroyPopout(Popout* const p)
    {
      if (p == nullptr) {
        return;
      }
      ReleaseChain(*p);
      if (CUIManager* const ui = UiManager(); ui != nullptr) {
        ui->RemovePopoutFrame(p->id);
      }
      gPopouts.erase(p->id);
      EdPopoutFrame* const frame = p->frame;
      if (frame != nullptr) {
        frame->mOwner = nullptr;
      }
      if (p->view != nullptr) {
        p->view->mOwner = nullptr;
      }
      p->frame = nullptr;
      p->view = nullptr;
      delete p;
      if (frame != nullptr) {
        frame->Destroy();
      }
    }

    void EdPopoutFrame::OnClose(wxCloseEvent& event)
    {
      (void)event;
      if (mOwner == nullptr) {
        return;
      }
      PostEvent(mOwner->id, "close", 0, 0, false);
      if (!mOwner->deferClose) {
        DestroyPopout(mOwner); // Destroy() of this frame is issued there
      }
    }

    void EdPopoutFrame::OnSize(wxSizeEvent& event)
    {
      event.Skip();
      if (mOwner != nullptr && mOwner->view != nullptr) {
        mOwner->view->SetSize(GetClientSize()); // the child fills the frame; the chain follows in RenderAll
        const wxSize client = GetClientSize();
        mOwner->dirty = kDirtyFrames;
        PostEvent(mOwner->id, "size", client.x, client.y, true);
      }
    }

    void EdPopoutFrame::OnMove(wxMoveEvent& event)
    {
      event.Skip();
      if (mOwner != nullptr) {
        const wxPoint position = GetPosition();
        PostEvent(mOwner->id, "move", position.x, position.y, true);
      }
    }

    // ---- monitors -------------------------------------------------------------------------------------------------------
    struct MonitorInfo
    {
      int x, y, w, h, wx, wy, ww, wh;
      bool primary;
    };

    BOOL CALLBACK MonitorEnum(HMONITOR monitor, HDC, LPRECT, LPARAM data)
    {
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(monitor, &info)) {
        auto* const list = reinterpret_cast<std::vector<MonitorInfo>*>(data);
        list->push_back(MonitorInfo{info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                                    info.rcMonitor.bottom - info.rcMonitor.top, info.rcWork.left, info.rcWork.top,
                                    info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top,
                                    (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
      }
      return TRUE;
    }

    std::vector<MonitorInfo> Monitors()
    {
      std::vector<MonitorInfo> list;
      EnumDisplayMonitors(nullptr, nullptr, &MonitorEnum, reinterpret_cast<LPARAM>(&list));
      return list;
    }

    int MonitorIndexOf(HWND window)
    {
      HMONITOR target = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      if (!GetMonitorInfoW(target, &info)) {
        return -1;
      }
      const std::vector<MonitorInfo> list = Monitors();
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].x == info.rcMonitor.left && list[i].y == info.rcMonitor.top) {
          return static_cast<int>(i) + 1; // 1-based like ed_monitor_info
        }
      }
      return -1;
    }
  } // namespace

  namespace edpopout
  {
    void DestroyAll()
    {
      std::vector<Popout*> all;
      for (auto& entry : gPopouts) {
        all.push_back(entry.second);
      }
      for (Popout* const p : all) {
        DestroyPopout(p);
      }
      gEvents.clear();
    }

    void RenderAll()
    {
      if (gPopouts.empty()) {
        return;
      }
      ID3D11DeviceContext* const context = RawContext();
      gpg::gal::Device* const gal = D3D_GetDevice() != nullptr ? D3D_GetDevice()->GetGalDevice() : nullptr;
      CUIManager* const ui = UiManager();
      WRenViewport* const viewport = ren_Viewport;
      if (context == nullptr || gal == nullptr || ui == nullptr || viewport == nullptr || viewport->GetPrimBatcher() == nullptr) {
        return;
      }
      CD3DPrimBatcher* const batcher = viewport->GetPrimBatcher();
      const unsigned long long now = GetTickCount64();

      // Copy the ids: a close event handled while drawing must not invalidate the iteration.
      std::vector<int> ids;
      for (const auto& entry : gPopouts) {
        ids.push_back(entry.first);
      }

      for (const int id : ids) {
        const auto found = gPopouts.find(id);
        if (found == gPopouts.end()) {
          continue;
        }
        Popout& p = *found->second;
        if (p.frame == nullptr || p.view == nullptr || p.frame->IsIconized() || !p.frame->IsShown()) {
          continue;
        }
        const wxSize client = p.view->GetClientSize();
        if (client.x <= 0 || client.y <= 0) {
          continue;
        }
        const bool needChain = !p.chain || !p.target || p.chainWidth != client.x || p.chainHeight != client.y;
        const bool wantsCapture = !p.capturePath.empty();
        if (!needChain && !wantsCapture && p.dirty <= 0 && now - p.lastDrawMs < kHeartbeatMs) {
          continue; // nothing changed and the heartbeat is not due
        }
        if (needChain) {
          if (!CreateChain(p, client.x, client.y)) {
            continue;
          }
        }
        if (p.dirty > 0) {
          --p.dirty;
        }
        p.lastDrawMs = now;

        ui->ValidateFrame(p.id);

        boost::shared_ptr<gpg::gal::RenderTarget> surface{};
        p.target->GetSurface(surface);
        auto* const surfaceD3D11 = static_cast<gpg::gal::RenderTargetD3D11*>(surface.get());
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        if (surfaceD3D11 != nullptr && SUCCEEDED(p.chain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
          // the UI of this window is drawn into the target (no depth: the UI techniques have depth disabled), then copied to the swap chain's buffer
          const gpg::gal::OutputContext output(surface, boost::shared_ptr<gpg::gal::DepthStencilTarget>());
          gal->ClearTarget(&output);
          gal->Clear(true, false, false, 0xFF222428u, 1.0f, 0);
          gal->BeginScene();
          ui->RenderFrames(p.id, batcher);
          ui->DrawUI(p.id, batcher);
          ui->DrawHead(p.id, batcher);
          gal->EndScene();

          if (wantsCapture) {
            std::string error;
            p.lastError = CapturePng(RawDevice(), context, surfaceD3D11->GetTexture(), p.capturePath, error) ? "capture saved" : error;   // reported by ed_window_get_rect until the next error
            p.capturePath.clear();
          }

          context->CopyResource(back.Get(), surfaceD3D11->GetTexture());
          (void)p.chain->Present(0, 0);   // never block the single main thread on a pop-out
        }

        // Back to the main frame's targets (the next WRenViewport::Render rebinds them anyway).
        if (const gpg::gal::OutputContext* const head = gal->GetHeadOutputContext(0); head != nullptr) {
          gal->ClearTarget(head);
        }
        ++gFramesDrawn;
      }
    }
  } // namespace edpopout

  namespace
  {
    Popout* Find(lua_State* const state, const int argument)
    {
      const auto it = gPopouts.find(static_cast<int>(lua_tonumber(state, argument)));
      return it == gPopouts.end() ? nullptr : it->second;
    }

    std::wstring Widen(const char* const text)
    {
      const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
      std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), n);
      return wide;
    }

    // ed_window_create(x, y, w, h [, title]) -> id | nil, message
    int __cdecl cfunc_EdWindowCreate(lua_State* const state)
    {
      if (D3D_GetDevice() == nullptr || UiManager() == nullptr) {
        lua_pushnil(state);
        lua_pushstring(state, "no device or UI manager");
        return 2;
      }
      const int x = static_cast<int>(lua_tonumber(state, 1));
      const int y = static_cast<int>(lua_tonumber(state, 2));
      const int w = static_cast<int>(lua_tonumber(state, 3));
      const int h = static_cast<int>(lua_tonumber(state, 4));
      const char* const title = lua_isstring(state, 5) ? lua_tostring(state, 5) : "Panel";

      EnsureDeviceListener();
      Popout* const p = new Popout();
      p->frame = new EdPopoutFrame(wxString(Widen(title).c_str()), wxPoint(x, y), wxSize(w, h), p);
      p->view = new EdPopoutView(p->frame, p);
      // EDITOR: the main window owns the pop-out (a Win32 owner, not a wx parent: wx would delete the frame with its parent behind our back). An owned window always stays in front of
      // its owner and goes away with it, so a pop-out that opens while the main window takes the focus (the Run window of a test run) no longer ends up behind it with nothing to bring it back.
      if (sMainWindow != nullptr) {
        const HWND owner = reinterpret_cast<HWND>(sMainWindow->GetHWND());
        const HWND self = reinterpret_cast<HWND>(p->frame->GetHWND());
        if (owner != nullptr && self != nullptr && owner != self) {
          (void)::SetWindowLongPtrW(self, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
        }
      }
      p->frame->Show(true);
      p->view->SetSize(p->frame->GetClientSize());

      p->id = UiManager()->AddPopoutFrame(p->view);
      if (p->id < 0) {
        p->frame->mOwner = nullptr;
        p->view->mOwner = nullptr;
        p->frame->Destroy();
        delete p;
        lua_pushnil(state);
        lua_pushstring(state, "AddPopoutFrame failed");
        return 2;
      }
      gPopouts[p->id] = p;
      lua_pushnumber(state, p->id);
      return 1;
    }

    // ed_window_destroy(id) -> bool
    int __cdecl cfunc_EdWindowDestroy(lua_State* const state)
    {
      Popout* const p = Find(state, 1);
      if (p == nullptr) {
        lua_pushboolean(state, 0);
        return 1;
      }
      DestroyPopout(p);
      lua_pushboolean(state, 1);
      return 1;
    }

    // ed_window_get_rect(id) -> x, y, w, h (outer), clientW, clientH, chainW, chainH, framesDrawn, lastError, monitor (1-based)   (nil if unknown)
    int __cdecl cfunc_EdWindowGetRect(lua_State* const state)
    {
      Popout* const p = Find(state, 1);
      if (p == nullptr || p->frame == nullptr) {
        lua_pushnil(state);
        return 1;
      }
      const wxPoint pos = p->frame->GetPosition();
      const wxSize size = p->frame->GetSize();
      const wxSize client = p->view->GetClientSize();
      lua_pushnumber(state, pos.x);
      lua_pushnumber(state, pos.y);
      lua_pushnumber(state, size.x);
      lua_pushnumber(state, size.y);
      lua_pushnumber(state, client.x);
      lua_pushnumber(state, client.y);
      lua_pushnumber(state, p->chainWidth);
      lua_pushnumber(state, p->chainHeight);
      lua_pushnumber(state, static_cast<lua_Number>(gFramesDrawn));
      lua_pushstring(state, p->lastError.c_str());
      lua_pushnumber(state, MonitorIndexOf(reinterpret_cast<HWND>(p->frame->GetHWND())));
      return 11;
    }

    // ed_window_set_rect(id, x, y, w, h) -> bool
    int __cdecl cfunc_EdWindowSetRect(lua_State* const state)
    {
      Popout* const p = Find(state, 1);
      if (p == nullptr || p->frame == nullptr) {
        lua_pushboolean(state, 0);
        return 1;
      }
      p->frame->SetSize(static_cast<int>(lua_tonumber(state, 2)), static_cast<int>(lua_tonumber(state, 3)),
                        static_cast<int>(lua_tonumber(state, 4)), static_cast<int>(lua_tonumber(state, 5)));
      p->dirty = kDirtyFrames;
      lua_pushboolean(state, 1);
      return 1;
    }

    // ed_window_raise(id) -> bool   (restores the window when it is minimised and brings it to the front, with the keyboard focus)
    int __cdecl cfunc_EdWindowRaise(lua_State* const state)
    {
      Popout* const p = Find(state, 1);
      if (p == nullptr || p->frame == nullptr) {
        lua_pushboolean(state, 0);
        return 1;
      }
      const HWND hwnd = reinterpret_cast<HWND>(p->frame->GetHWND());
      if (hwnd != nullptr) {
        if (::IsIconic(hwnd)) {
          ::ShowWindow(hwnd, SW_RESTORE);
        }
        ::SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        (void)::SetForegroundWindow(hwnd);
      }
      p->frame->Raise();
      p->dirty = kDirtyFrames;
      lua_pushboolean(state, 1);
      return 1;
    }

    // ed_window_capture(id, path) -> bool   (the PNG is written by the next frame, just before the present; absolute path, forward slashes)
    int __cdecl cfunc_EdWindowCapture(lua_State* const state)
    {
      Popout* const p = Find(state, 1);
      const char* const path = lua_isstring(state, 2) ? lua_tostring(state, 2) : nullptr;
      if (p == nullptr || path == nullptr) {
        lua_pushboolean(state, 0);
        return 1;
      }
      p->capturePath = path;
      lua_pushboolean(state, 1);
      return 1;
    }

    // ed_window_invalidate(id) -> bool   (redraw within the next frames; Lua calls it when a panel's content changed)
    int __cdecl cfunc_EdWindowInvalidate(lua_State* const state)
    {
      Popout* const p = Find(state, 1);
      if (p == nullptr) {
        lua_pushboolean(state, 0);
        return 1;
      }
      p->dirty = kDirtyFrames;
      lua_pushboolean(state, 1);
      return 1;
    }

    // ed_window_set_title(id, title) -> bool
    int __cdecl cfunc_EdWindowSetTitle(lua_State* const state)
    {
      Popout* const p = Find(state, 1);
      if (p == nullptr || p->frame == nullptr || !lua_isstring(state, 2)) {
        lua_pushboolean(state, 0);
        return 1;
      }
      p->frame->SetTitle(wxString(Widen(lua_tostring(state, 2)).c_str()));
      lua_pushboolean(state, 1);
      return 1;
    }

    // ed_window_set_defer_close(id, bool) -> bool   (true: the X button only posts a 'close' event and Lua destroys the window)
    int __cdecl cfunc_EdWindowSetDeferClose(lua_State* const state)
    {
      Popout* const p = Find(state, 1);
      if (p == nullptr) {
        lua_pushboolean(state, 0);
        return 1;
      }
      p->deferClose = lua_toboolean(state, 2) != 0;
      lua_pushboolean(state, 1);
      return 1;
    }

    // ed_window_poll_event() -> id, kind, a, b | nil     kinds: 'close', 'move' (x, y), 'size' (client w, h); move / size are coalesced
    int __cdecl cfunc_EdWindowPollEvent(lua_State* const state)
    {
      if (gEvents.empty()) {
        lua_pushnil(state);
        return 1;
      }
      const Event e = gEvents.front();
      gEvents.pop_front();
      lua_pushnumber(state, e.id);
      lua_pushstring(state, e.kind.c_str());
      lua_pushnumber(state, e.a);
      lua_pushnumber(state, e.b);
      return 4;
    }

    // ed_main_window_rect() -> clientScreenX, clientScreenY, clientW, clientH, outerX, outerY, outerW, outerH   (the editor's main window; nil without one)
    int __cdecl cfunc_EdMainWindowRect(lua_State* const state)
    {
      wxWindow* const main = sMainWindow;
      if (main == nullptr) {
        lua_pushnil(state);
        return 1;
      }
      int cx = 0;
      int cy = 0;
      main->ClientToScreen(&cx, &cy);
      const wxSize client = main->GetClientSize();
      wxPoint position = main->GetPosition();
      if (main->GetParent() != nullptr) {
        position = main->GetParent()->ClientToScreen(position);
      }
      const wxSize size = main->GetSize();
      lua_pushnumber(state, cx);
      lua_pushnumber(state, cy);
      lua_pushnumber(state, client.x);
      lua_pushnumber(state, client.y);
      lua_pushnumber(state, position.x);
      lua_pushnumber(state, position.y);
      lua_pushnumber(state, size.x);
      lua_pushnumber(state, size.y);
      return 8;
    }

    // ed_monitor_count() -> n
    int __cdecl cfunc_EdMonitorCount(lua_State* const state)
    {
      lua_pushnumber(state, static_cast<lua_Number>(Monitors().size()));
      return 1;
    }

    // ed_monitor_info(i) -> x, y, w, h, workX, workY, workW, workH, primary     (i = 1..n, screen coordinates)
    int __cdecl cfunc_EdMonitorInfo(lua_State* const state)
    {
      const std::vector<MonitorInfo> list = Monitors();
      const int i = static_cast<int>(lua_tonumber(state, 1)) - 1;
      if (i < 0 || i >= static_cast<int>(list.size())) {
        lua_pushnil(state);
        return 1;
      }
      const MonitorInfo& m = list[static_cast<std::size_t>(i)];
      lua_pushnumber(state, m.x);
      lua_pushnumber(state, m.y);
      lua_pushnumber(state, m.w);
      lua_pushnumber(state, m.h);
      lua_pushnumber(state, m.wx);
      lua_pushnumber(state, m.wy);
      lua_pushnumber(state, m.ww);
      lua_pushnumber(state, m.wh);
      lua_pushnumber(state, m.primary ? 1 : 0);
      return 9;
    }

    [[nodiscard]] CScrLuaInitFormSet& UserLuaInitSet()
    {
      if (CScrLuaInitFormSet* const set = SCR_FindLuaInitFormSet("User"); set != nullptr) {
        return *set;
      }
      static CScrLuaInitFormSet fallbackSet("User");
      return fallbackSet;
    }

    void Bind(const char* const name, int(__cdecl* const function)(lua_State*), const char* const help)
    {
      (void)new CScrLuaBinder(UserLuaInitSet(), name, function, nullptr, "<global>", help);
    }

    struct EdPopoutLuaBootstrap
    {
      EdPopoutLuaBootstrap()
      {
        Bind("ed_window_create", &cfunc_EdWindowCreate, "ed_window_create(x, y, w, h [, title]) -> id | nil, message");
        Bind("ed_window_destroy", &cfunc_EdWindowDestroy, "ed_window_destroy(id) -> bool");
        Bind("ed_window_get_rect", &cfunc_EdWindowGetRect,
             "ed_window_get_rect(id) -> x, y, w, h, clientW, clientH, chainW, chainH, framesDrawn, lastError, monitor");
        Bind("ed_window_set_rect", &cfunc_EdWindowSetRect, "ed_window_set_rect(id, x, y, w, h) -> bool");
        Bind("ed_window_raise", &cfunc_EdWindowRaise, "ed_window_raise(id) -> bool");
        Bind("ed_window_capture", &cfunc_EdWindowCapture, "ed_window_capture(id, absolutePath) -> bool");
        Bind("ed_window_invalidate", &cfunc_EdWindowInvalidate, "ed_window_invalidate(id) -> bool");
        Bind("ed_window_set_title", &cfunc_EdWindowSetTitle, "ed_window_set_title(id, title) -> bool");
        Bind("ed_window_set_defer_close", &cfunc_EdWindowSetDeferClose, "ed_window_set_defer_close(id, bool) -> bool");
        Bind("ed_window_poll_event", &cfunc_EdWindowPollEvent, "ed_window_poll_event() -> id, kind, a, b | nil");
        Bind("ed_main_window_rect", &cfunc_EdMainWindowRect, "ed_main_window_rect() -> clientScreenX, clientScreenY, clientW, clientH, outerX, outerY, outerW, outerH");
        Bind("ed_monitor_count", &cfunc_EdMonitorCount, "ed_monitor_count() -> n");
        Bind("ed_monitor_info", &cfunc_EdMonitorInfo, "ed_monitor_info(i) -> x, y, w, h, workX, workY, workW, workH, primary");
      }
    };

    const EdPopoutLuaBootstrap gEdPopoutLuaBootstrap{};
  } // namespace
} // namespace moho
