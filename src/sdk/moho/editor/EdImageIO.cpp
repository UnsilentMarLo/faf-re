// Editor natives for image files and the Windows file dialogs (heightmap import / export, later textures and previews):
//
//   ed_image_write16(path, w, h, values)   -> true | nil, message    16-bit greyscale; `.raw` = raw little endian words, otherwise PNG / TIFF by extension
//   ed_image_read16(path, tw, th)          -> values, w, h | nil, message   16-bit greyscale of any WIC format (PNG, TIFF, BMP, JPEG ...) or a square
//                                             `.raw`; resampled (bilinear) to tw x th when those are given and differ from the file's size
//   ed_file_dialog(save, title, filter, name) -> path | nil          the common dialog; `filter` is "Description|*.png;*.raw|Other|*.*"
//
// `values` is a table of numbers 0..65535, row major. The natives run on the UI thread (the Lua UI thread) and initialise COM for WIC themselves.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <wincodec.h>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace
{
  std::wstring Widen(const char* const utf8)
  {
    if (utf8 == nullptr) {
      return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1) {
      return std::wstring();
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    out.resize(static_cast<std::size_t>(n - 1));
    return out;
  }

  std::string Narrow(const std::wstring& wide)
  {
    if (wide.empty()) {
      return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
  }

  std::wstring Extension(const std::wstring& path)
  {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) {
      return std::wstring();
    }
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) {
      c = static_cast<wchar_t>(towlower(c));
    }
    return ext;
  }

  struct ComScope
  {
    bool uninit = false;
    ComScope()
    {
      const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      uninit = SUCCEEDED(hr);       // RPC_E_CHANGED_MODE: COM is already up in another mode, which is fine for WIC
    }
    ~ComScope()
    {
      if (uninit) {
        CoUninitialize();
      }
    }
  };

  template <typename T>
  struct Ptr
  {
    T* p = nullptr;
    ~Ptr()
    {
      if (p != nullptr) {
        p->Release();
      }
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
  };

  int Fail(lua_State* const L, const char* const message)
  {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
  }

  // ---- ed_image_write16 ------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_EdImageWrite16(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    const int w = static_cast<int>(luaL_checknumber(L, 2));
    const int h = static_cast<int>(luaL_checknumber(L, 3));
    luaL_checktype(L, 4, LUA_TTABLE);
    if (utf8 == nullptr || w < 1 || h < 1 || static_cast<long long>(w) * h > 64LL * 1024 * 1024) {
      return Fail(L, "bad arguments");
    }
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint16_t> words(count);
    for (std::size_t i = 0; i < count; ++i) {
      lua_rawgeti(L, 4, static_cast<int>(i + 1));
      const double v = lua_tonumber(L, -1);
      lua_settop(L, -2);
      words[i] = static_cast<std::uint16_t>(std::max(0.0, std::min(65535.0, v)));
    }
    const std::wstring path = Widen(utf8);
    const std::wstring ext = Extension(path);

    if (ext == L"raw" || ext == L"r16") {
      HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        return Fail(L, "cannot create the file");
      }
      DWORD written = 0;
      const BOOL ok = WriteFile(file, words.data(), static_cast<DWORD>(count * 2), &written, nullptr);
      CloseHandle(file);
      if (!ok || written != count * 2) {
        return Fail(L, "write failed");
      }
      lua_pushboolean(L, 1);
      return 1;
    }

    ComScope com;
    Ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
      return Fail(L, "WIC is not available");
    }
    const GUID container = (ext == L"tif" || ext == L"tiff") ? GUID_ContainerFormatTiff : GUID_ContainerFormatPng;
    Ptr<IWICStream> stream;
    Ptr<IWICBitmapEncoder> encoder;
    Ptr<IWICBitmapFrameEncode> frame;
    Ptr<IPropertyBag2> options;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(container, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &options)) || FAILED(frame->Initialize(options.p)) ||
        FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) {
      return Fail(L, "cannot create the image");
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat16bppGray;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat16bppGray) {
      return Fail(L, "the format has no 16-bit greyscale");
    }
    if (FAILED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w) * 2U, static_cast<UINT>(count * 2),
                                  reinterpret_cast<BYTE*>(words.data()))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
      return Fail(L, "cannot write the image");
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  // ---- ed_image_read16 -------------------------------------------------------------------------------------------------------------------

  bool ReadRaw(const std::wstring& path, std::vector<std::uint16_t>& out, int& w, int& h)
  {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const long long words = size.QuadPart / 2;
    const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(words))));
    if (side < 2 || static_cast<long long>(side) * side != words) {
      CloseHandle(file);
      return false;
    }
    out.resize(static_cast<std::size_t>(words));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, out.data(), static_cast<DWORD>(words * 2), &read, nullptr);
    CloseHandle(file);
    w = side;
    h = side;
    return ok && read == words * 2;
  }

  bool ReadWic(const std::wstring& path, std::vector<std::uint16_t>& out, int& w, int& h)
  {
    Ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
      return false;
    }
    Ptr<IWICBitmapDecoder> decoder;
    Ptr<IWICBitmapFrameDecode> frame;
    Ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat16bppGray, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      return false;
    }
    UINT uw = 0;
    UINT uh = 0;
    if (FAILED(converter->GetSize(&uw, &uh)) || uw < 2 || uh < 2) {
      return false;
    }
    w = static_cast<int>(uw);
    h = static_cast<int>(uh);
    out.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    return SUCCEEDED(converter->CopyPixels(nullptr, uw * 2U, static_cast<UINT>(out.size() * 2), reinterpret_cast<BYTE*>(out.data())));
  }

  int __cdecl cfunc_EdImageRead16(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    const int tw = lua_type(L, 2) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, 2)) : 0;
    const int th = lua_type(L, 3) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, 3)) : 0;
    if (utf8 == nullptr) {
      return Fail(L, "bad arguments");
    }
    const std::wstring path = Widen(utf8);
    const std::wstring ext = Extension(path);
    std::vector<std::uint16_t> src;
    int w = 0;
    int h = 0;
    if (ext == L"raw" || ext == L"r16") {
      if (!ReadRaw(path, src, w, h)) {
        return Fail(L, "not a square 16-bit raw file");
      }
    } else {
      ComScope com;
      if (!ReadWic(path, src, w, h)) {
        return Fail(L, "cannot read the image");
      }
    }

    lua_newtable(L);
    if (tw >= 2 && th >= 2 && (tw != w || th != h)) {
      // bilinear resample onto the target grid (the first and last samples map onto the first and last of the image)
      for (int z = 0; z < th; ++z) {
        const float fz = (static_cast<float>(z) / static_cast<float>(th - 1)) * static_cast<float>(h - 1);
        const int z0 = std::min(h - 2, static_cast<int>(fz));
        const float tz = fz - static_cast<float>(z0);
        for (int x = 0; x < tw; ++x) {
          const float fx = (static_cast<float>(x) / static_cast<float>(tw - 1)) * static_cast<float>(w - 1);
          const int x0 = std::min(w - 2, static_cast<int>(fx));
          const float tx = fx - static_cast<float>(x0);
          const auto at = [&](const int xx, const int zz) {
            return static_cast<float>(src[static_cast<std::size_t>(zz) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xx)]);
          };
          const float top = at(x0, z0) * (1.0f - tx) + at(x0 + 1, z0) * tx;
          const float bottom = at(x0, z0 + 1) * (1.0f - tx) + at(x0 + 1, z0 + 1) * tx;
          const float v = top * (1.0f - tz) + bottom * tz;
          lua_pushnumber(L, static_cast<lua_Number>(z * tw + x + 1));
          lua_pushnumber(L, static_cast<lua_Number>(std::floor(v + 0.5f)));
          lua_settable(L, -3);
        }
      }
    } else {
      for (std::size_t i = 0; i < src.size(); ++i) {
        lua_pushnumber(L, static_cast<lua_Number>(i + 1));
        lua_pushnumber(L, static_cast<lua_Number>(src[i]));
        lua_settable(L, -3);
      }
    }
    lua_pushnumber(L, static_cast<lua_Number>(w));
    lua_pushnumber(L, static_cast<lua_Number>(h));
    return 3;
  }

  // ---- ed_file_dialog --------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_EdFileDialog(lua_State* const L)
  {
    const bool save = lua_toboolean(L, 1) != 0;
    const char* const title = lua_tostring(L, 2);
    const char* const filter = lua_tostring(L, 3);
    const char* const name = lua_tostring(L, 4);

    // "Description|*.png;*.raw|Other|*.*" -> the double-NUL separated list the dialog wants
    std::wstring spec = Widen(filter != nullptr ? filter : "All files|*.*");
    for (wchar_t& c : spec) {
      if (c == L'|') {
        c = L'\0';
      }
    }
    spec.push_back(L'\0');
    spec.push_back(L'\0');

    wchar_t buffer[MAX_PATH * 2] = {};
    if (name != nullptr) {
      const std::wstring initial = Widen(name);
      wcsncpy_s(buffer, initial.c_str(), _TRUNCATE);
    }
    const std::wstring caption = Widen(title != nullptr ? title : (save ? "Save" : "Open"));

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = spec.c_str();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    ofn.lpstrTitle = caption.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) {
      lua_pushnil(L);
      return 1;
    }
    const std::string path = Narrow(std::wstring(buffer));
    lua_pushstring(L, path.c_str());
    return 1;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& UserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  struct EdImageIOLuaBootstrap
  {
    EdImageIOLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_image_write16", &cfunc_EdImageWrite16, nullptr, "<global>", "ed_image_write16(path, w, h, values) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_image_read16", &cfunc_EdImageRead16, nullptr, "<global>", "ed_image_read16(path[, tw, th]) -> values, w, h | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_file_dialog", &cfunc_EdFileDialog, nullptr, "<global>", "ed_file_dialog(save, title, filter, name) -> path | nil");
    }
  };

  const EdImageIOLuaBootstrap gEdImageIOLuaBootstrap{};
} // namespace
