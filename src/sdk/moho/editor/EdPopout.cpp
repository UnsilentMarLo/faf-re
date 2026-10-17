#include "moho/editor/EdPopout.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "platform/Platform.h"
#include "platform/WxWidgets.h"

#include <wx/dcclient.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/window.h>

#include <d3d9.h>
#include <d3dx9.h>

#include "gpg/gal/backends/d3d9/DeviceD3D9.hpp"
#include "lua/LuaRuntimeTypes.h"
#include "lua/LuaPrimitives.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/misc/Listener.h"
#include "moho/render/WRenViewport.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/CD3DPrimBatcher.h"
#include "moho/render/textures/DeviceExitListener.h"
#include "moho/ui/CUIManager.h"
#include "moho/ui/IUIManager.h"

namespace moho
{
  namespace
  {
    struct Popout;

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
        wxPaintDC dc(this); // validates the update region; the engine draws the content every frame
      }

      void OnEraseBackground(wxEraseEvent& event) { (void)event; }

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

      Popout* mOwner;

      DECLARE_EVENT_TABLE()
    };

    BEGIN_EVENT_TABLE(EdPopoutFrame, wxFrame)
      EVT_CLOSE(EdPopoutFrame::OnClose)
      EVT_SIZE(EdPopoutFrame::OnSize)
    END_EVENT_TABLE()

    struct Popout
    {
      int id = -1; // = the Maui root-frame index
      EdPopoutFrame* frame = nullptr;
      EdPopoutView* view = nullptr;
      IDirect3DSwapChain9* chain = nullptr;
      IDirect3DSurface9* back = nullptr;
      int chainWidth = 0;
      int chainHeight = 0;
      std::string capturePath;
      std::string lastError;
    };

    std::map<int, Popout*> gPopouts;
    unsigned long gFramesDrawn = 0;

    CUIManager* UiManager()
    {
      return static_cast<CUIManager*>(UI_GetManager());
    }

    IDirect3DDevice9* RawDevice()
    {
      CD3DDevice* const device = D3D_GetDevice();
      if (device == nullptr || device->GetGalDevice() == nullptr) {
        return nullptr;
      }
      return static_cast<gpg::gal::DeviceD3D9*>(device->GetGalDevice())->mDevice;
    }

    void ReleaseChain(Popout& p)
    {
      if (p.back != nullptr) {
        p.back->Release();
        p.back = nullptr;
      }
      if (p.chain != nullptr) {
        p.chain->Release();
        p.chain = nullptr;
      }
      p.chainWidth = 0;
      p.chainHeight = 0;
    }

    // The swap chain must be created at the window's real client size (wx 2.4.2 reports a few pixels more than asked).
    bool CreateChain(Popout& p, const int width, const int height)
    {
      IDirect3DDevice9* const dev = RawDevice();
      if (dev == nullptr || width <= 0 || height <= 0) {
        return false;
      }
      ReleaseChain(p);

      D3DPRESENT_PARAMETERS pp{};
      pp.BackBufferWidth = static_cast<UINT>(width);
      pp.BackBufferHeight = static_cast<UINT>(height);
      pp.BackBufferFormat = D3DFMT_UNKNOWN; // windowed: the desktop format
      pp.BackBufferCount = 1;
      pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
      pp.hDeviceWindow = reinterpret_cast<HWND>(p.view->GetHWND());
      pp.Windowed = TRUE;
      pp.EnableAutoDepthStencil = FALSE;
      pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; // never block the single main thread on a pop-out

      const HRESULT hr = dev->CreateAdditionalSwapChain(&pp, &p.chain);
      if (FAILED(hr) || p.chain == nullptr) {
        char text[96];
        std::snprintf(text, sizeof(text), "CreateAdditionalSwapChain failed 0x%08lX", static_cast<unsigned long>(hr));
        p.lastError = text;
        p.chain = nullptr;
        return false;
      }
      if (FAILED(p.chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &p.back)) || p.back == nullptr) {
        ReleaseChain(p);
        p.lastError = "GetBackBuffer failed";
        return false;
      }
      p.chainWidth = width;
      p.chainHeight = height;
      p.lastError.clear();
      return true;
    }

    // Device reset / shutdown: additional swap chains do not survive IDirect3DDevice9::Reset, so they are released on the
    // exit event (type 1) and created again lazily by RenderAll.
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
      DestroyPopout(mOwner); // Destroy() of this frame is issued there
    }

    void EdPopoutFrame::OnSize(wxSizeEvent& event)
    {
      event.Skip();
      if (mOwner != nullptr && mOwner->view != nullptr) {
        mOwner->view->SetSize(GetClientSize()); // the child fills the frame; the chain follows in RenderAll
      }
    }
  } // namespace

  namespace edpopout
  {
    void RenderAll()
    {
      if (gPopouts.empty()) {
        return;
      }
      IDirect3DDevice9* const dev = RawDevice();
      CUIManager* const ui = UiManager();
      WRenViewport* const viewport = ren_Viewport;
      if (dev == nullptr || ui == nullptr || viewport == nullptr || viewport->GetPrimBatcher() == nullptr) {
        return;
      }
      CD3DPrimBatcher* const batcher = viewport->GetPrimBatcher();

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
        if (p.chain == nullptr || p.chainWidth != client.x || p.chainHeight != client.y) {
          if (!CreateChain(p, client.x, client.y)) {
            continue;
          }
        }

        ui->ValidateFrame(p.id);

        IDirect3DSurface9* oldTarget = nullptr;
        IDirect3DSurface9* oldDepth = nullptr;
        (void)dev->GetRenderTarget(0, &oldTarget);
        (void)dev->GetDepthStencilSurface(&oldDepth);

        if (SUCCEEDED(dev->SetRenderTarget(0, p.back))) {
          (void)dev->SetDepthStencilSurface(nullptr); // UI techniques have depth disabled; the main depth buffer may be smaller
          (void)dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 34, 36, 40), 1.0f, 0);
          if (SUCCEEDED(dev->BeginScene())) {
            ui->RenderFrames(p.id, batcher);
            ui->DrawUI(p.id, batcher);
            ui->DrawHead(p.id, batcher);
            (void)dev->EndScene();
          }

          if (!p.capturePath.empty()) {
            // GetRenderTargetData needs a system-memory surface of the same size and format as the back buffer.
            D3DSURFACE_DESC desc{};
            IDirect3DSurface9* system = nullptr;
            if (SUCCEEDED(p.back->GetDesc(&desc)) &&
                SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system, nullptr)) &&
                system != nullptr) {
              if (SUCCEEDED(dev->GetRenderTargetData(p.back, system))) {
                const HRESULT saved = D3DXSaveSurfaceToFileA(p.capturePath.c_str(), D3DXIFF_PNG, system, nullptr, nullptr);
                char text[160];
                std::snprintf(text, sizeof(text), "capture %s hr=0x%08lX fmt=%u", FAILED(saved) ? "failed" : "saved", static_cast<unsigned long>(saved), static_cast<unsigned>(desc.Format));
                p.lastError = text; // reported by ed_window_get_rect until the next error
              } else {
                p.lastError = "capture GetRenderTargetData failed";
              }
              system->Release();
            } else {
              p.lastError = "capture surface failed";
            }
            p.capturePath.clear();
          }

          (void)p.chain->Present(nullptr, nullptr, nullptr, nullptr, 0);
        }

        // Back to the main frame's targets (the next WRenViewport::Render rebinds them anyway).
        (void)dev->SetRenderTarget(0, oldTarget);
        (void)dev->SetDepthStencilSurface(oldDepth);
        if (oldTarget != nullptr) {
          oldTarget->Release();
        }
        if (oldDepth != nullptr) {
          oldDepth->Release();
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

    // ed_window_get_rect(id) -> x, y, w, h (outer), clientW, clientH, chainW, chainH, framesDrawn, lastError   (nil if unknown)
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
      return 10;
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
      lua_pushboolean(state, 1);
      return 1;
    }

    // ed_window_capture(id, path) -> bool   (the PNG is written by the next frame, just before the present)
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

    [[nodiscard]] CScrLuaInitFormSet& UserLuaInitSet()
    {
      if (CScrLuaInitFormSet* const set = SCR_FindLuaInitFormSet("User"); set != nullptr) {
        return *set;
      }
      static CScrLuaInitFormSet fallbackSet("User");
      return fallbackSet;
    }

    struct EdPopoutLuaBootstrap
    {
      EdPopoutLuaBootstrap()
      {
        (void)new CScrLuaBinder(UserLuaInitSet(), "ed_window_create", &cfunc_EdWindowCreate, nullptr, "<global>",
                                "ed_window_create(x, y, w, h [, title]) -> id | nil, message");
        (void)new CScrLuaBinder(UserLuaInitSet(), "ed_window_destroy", &cfunc_EdWindowDestroy, nullptr, "<global>", "ed_window_destroy(id) -> bool");
        (void)new CScrLuaBinder(UserLuaInitSet(), "ed_window_get_rect", &cfunc_EdWindowGetRect, nullptr, "<global>",
                                "ed_window_get_rect(id) -> x, y, w, h, clientW, clientH, chainW, chainH, framesDrawn, lastError");
        (void)new CScrLuaBinder(UserLuaInitSet(), "ed_window_set_rect", &cfunc_EdWindowSetRect, nullptr, "<global>", "ed_window_set_rect(id, x, y, w, h) -> bool");
        (void)new CScrLuaBinder(UserLuaInitSet(), "ed_window_capture", &cfunc_EdWindowCapture, nullptr, "<global>", "ed_window_capture(id, path) -> bool");
      }
    };

    const EdPopoutLuaBootstrap gEdPopoutLuaBootstrap{};
  } // namespace
} // namespace moho
