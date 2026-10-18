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
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <wincodec.h>

#include <zlib.h>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/editor/EdIcon.h"

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

  // ---- files inside the game's archives ---------------------------------------------------------------------------------------------------

  // The virtual file system resolves a texture of the game data to a path like `...\gamedata\env.scd\env\evergreen\layers\x.dds`: a name inside a zip archive. This
  // reads such a path (or a plain file) into memory: finds the archive part (.scd / .nx2 / .zip), looks the entry up in the zip central directory and inflates it.
  bool ReadAll(const std::string& path, std::vector<std::uint8_t>& out)
  {
    std::string p = path;
    for (char& c : p) {
      if (c == '\\') c = '/';
    }
    std::string lower = p;
    for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    const auto readPlain = [&](const std::string& file) {
      HANDLE h = CreateFileW(Widen(file.c_str()).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        return false;
      }
      LARGE_INTEGER size{};
      GetFileSizeEx(h, &size);
      out.resize(static_cast<std::size_t>(size.QuadPart));
      DWORD read = 0;
      const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
      CloseHandle(h);
      return ok && read == out.size();
    };

    std::size_t split = std::string::npos;
    for (const char* ext : {".scd/", ".nx2/", ".zip/"}) {
      const std::size_t at = lower.find(ext);
      if (at != std::string::npos && (split == std::string::npos || at < split)) {
        split = at + 4;           // the archive path ends after the extension
      }
    }
    if (split == std::string::npos) {
      return readPlain(p);
    }
    const std::string archive = p.substr(0, split);
    std::string inner = lower.substr(split + 1);

    HANDLE h = CreateFileW(Widen(archive.c_str()).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      return false;
    }
    const auto readAt = [&](const std::uint64_t offset, void* const dst, const DWORD count) {
      LARGE_INTEGER li{};
      li.QuadPart = static_cast<LONGLONG>(offset);
      if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        return false;
      }
      DWORD got = 0;
      return ReadFile(h, dst, count, &got, nullptr) != 0 && got == count;
    };
    LARGE_INTEGER total{};
    GetFileSizeEx(h, &total);
    const std::uint64_t size = static_cast<std::uint64_t>(total.QuadPart);
    bool done = false;
    const std::uint64_t tailSize = std::min<std::uint64_t>(size, 70000);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tailSize));
    if (readAt(size - tailSize, tail.data(), static_cast<DWORD>(tailSize))) {
      std::int64_t eocd = -1;
      for (std::int64_t i = static_cast<std::int64_t>(tail.size()) - 22; i >= 0; --i) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 5 && tail[i + 3] == 6) {
          eocd = i;
          break;
        }
      }
      if (eocd >= 0) {
        std::uint32_t cdSize = 0;
        std::uint32_t cdOffset = 0;
        std::memcpy(&cdSize, tail.data() + eocd + 12, 4);
        std::memcpy(&cdOffset, tail.data() + eocd + 16, 4);
        std::vector<std::uint8_t> cd(cdSize);
        if (readAt(cdOffset, cd.data(), cdSize)) {
          std::size_t pos = 0;
          while (pos + 46 <= cd.size() && !done) {
            std::uint32_t sig = 0;
            std::memcpy(&sig, cd.data() + pos, 4);
            if (sig != 0x02014b50u) {
              break;
            }
            std::uint16_t method = 0;
            std::uint32_t csize = 0;
            std::uint32_t usize = 0;
            std::uint16_t nameLen = 0;
            std::uint16_t extraLen = 0;
            std::uint16_t commentLen = 0;
            std::uint32_t local = 0;
            std::memcpy(&method, cd.data() + pos + 10, 2);
            std::memcpy(&csize, cd.data() + pos + 20, 4);
            std::memcpy(&usize, cd.data() + pos + 24, 4);
            std::memcpy(&nameLen, cd.data() + pos + 28, 2);
            std::memcpy(&extraLen, cd.data() + pos + 30, 2);
            std::memcpy(&commentLen, cd.data() + pos + 32, 2);
            std::memcpy(&local, cd.data() + pos + 42, 4);
            std::string name(reinterpret_cast<const char*>(cd.data() + pos + 46), nameLen);
            for (char& c : name) {
              if (c == '\\') c = '/';
              c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            }
            if (name == inner) {
              std::uint8_t lh[30] = {};
              if (readAt(local, lh, 30)) {
                std::uint16_t ln = 0;
                std::uint16_t le = 0;
                std::memcpy(&ln, lh + 26, 2);
                std::memcpy(&le, lh + 28, 2);
                std::vector<std::uint8_t> raw(csize);
                if (readAt(static_cast<std::uint64_t>(local) + 30 + ln + le, raw.data(), csize)) {
                  if (method == 0) {
                    out = raw;
                    done = true;
                  } else if (method == 8) {
                    out.assign(usize, 0);
                    z_stream zs{};
                    if (inflateInit2(&zs, -15) == Z_OK) {
                      zs.next_in = raw.data();
                      zs.avail_in = csize;
                      zs.next_out = out.data();
                      zs.avail_out = usize;
                      const int rc = inflate(&zs, Z_FINISH);
                      inflateEnd(&zs);
                      done = (rc == Z_STREAM_END);
                    }
                  }
                }
              }
              break;
            }
            pos += 46u + nameLen + extraLen + commentLen;
          }
        }
      }
    }
    CloseHandle(h);
    return done;
  }

  // ---- ed_image_avg ----------------------------------------------------------------------------------------------------------------------

  // ed_image_avg(path) -> r, g, b   the average colour (0..255) of an image (alpha-weighted); nil when it cannot be read
  int __cdecl cfunc_EdImageAvg(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    if (utf8 == nullptr) {
      return Fail(L, "bad arguments");
    }
    ComScope com;
    Ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
      return Fail(L, "WIC is not available");
    }
    std::vector<std::uint8_t> bytes;
    if (!ReadAll(utf8, bytes) || bytes.empty()) {
      return Fail(L, "cannot read the file");
    }
    Ptr<IWICStream> memory;
    Ptr<IWICBitmapDecoder> decoder;
    Ptr<IWICBitmapFrameDecode> frame;
    Ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateStream(&memory)) || FAILED(memory->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(memory.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      return Fail(L, "cannot decode the image");
    }
    UINT w = 0;
    UINT h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || static_cast<unsigned long long>(w) * h > 64ULL * 1024 * 1024) {
      return Fail(L, "bad image size");
    }
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4U, static_cast<UINT>(px.size()), px.data()))) {
      return Fail(L, "cannot copy the pixels");
    }
    double r = 0;
    double g = 0;
    double b = 0;
    double weight = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) {
      const double a = px[i + 3] / 255.0;
      b += px[i] * a;
      g += px[i + 1] * a;
      r += px[i + 2] * a;
      weight += a;
    }
    if (weight <= 0.0) {
      weight = 1.0;
    }
    lua_pushnumber(L, static_cast<lua_Number>(std::floor(r / weight + 0.5)));
    lua_pushnumber(L, static_cast<lua_Number>(std::floor(g / weight + 0.5)));
    lua_pushnumber(L, static_cast<lua_Number>(std::floor(b / weight + 0.5)));
    return 3;
  }

  // ---- ed_scmap_set_preview --------------------------------------------------------------------------------------------------------------

  void Put32(std::string& out, const std::uint32_t v)
  {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    }
  }

  // ed_scmap_set_preview(path, w, h, pixels) -> true | nil, message
  // Replaces the preview image of an .scmap file (the blob after the 30 byte header: an i32 byte count and an uncompressed ARGB DDS) by w x h pixels,
  // `pixels` being numbers 0xRRGGBB, row major. The rest of the file is copied unchanged.
  int __cdecl cfunc_EdScmapSetPreview(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    const int w = static_cast<int>(luaL_checknumber(L, 2));
    const int h = static_cast<int>(luaL_checknumber(L, 3));
    luaL_checktype(L, 4, LUA_TTABLE);
    if (utf8 == nullptr || w < 1 || h < 1 || w > 4096 || h > 4096) {
      return Fail(L, "bad arguments");
    }
    const std::wstring path = Widen(utf8);
    std::string file;
    {
      HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE) {
        return Fail(L, "cannot open the map file");
      }
      LARGE_INTEGER size{};
      GetFileSizeEx(f, &size);
      file.resize(static_cast<std::size_t>(size.QuadPart));
      DWORD read = 0;
      const BOOL ok = ReadFile(f, file.data(), static_cast<DWORD>(file.size()), &read, nullptr);
      CloseHandle(f);
      if (!ok || read != file.size()) {
        return Fail(L, "cannot read the map file");
      }
    }
    if (file.size() < 40) {
      return Fail(L, "not a map file");
    }
    std::uint32_t signature = 0;
    std::memcpy(&signature, file.data(), 4);
    std::uint32_t oldBytes = 0;
    std::memcpy(&oldBytes, file.data() + 30, 4);
    if (signature != 0x1A70614Du || 34ull + oldBytes > file.size()) {
      return Fail(L, "not a version 2 map file");
    }

    std::string dds;
    dds.append("DDS ", 4);
    Put32(dds, 124);
    Put32(dds, 0x1u | 0x2u | 0x4u | 0x1000u | 0x8u);
    Put32(dds, static_cast<std::uint32_t>(h));
    Put32(dds, static_cast<std::uint32_t>(w));
    Put32(dds, static_cast<std::uint32_t>(w) * 4u);
    Put32(dds, 0);
    Put32(dds, 0);
    dds.append(44, '\0');
    Put32(dds, 32);
    Put32(dds, 0x41);
    Put32(dds, 0);
    Put32(dds, 32);
    Put32(dds, 0x00FF0000u);
    Put32(dds, 0x0000FF00u);
    Put32(dds, 0x000000FFu);
    Put32(dds, 0xFF000000u);
    Put32(dds, 0x1000u);
    Put32(dds, 0);
    Put32(dds, 0);
    Put32(dds, 0);
    Put32(dds, 0);
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < count; ++i) {
      lua_rawgeti(L, 4, static_cast<int>(i + 1));
      const std::uint32_t rgb = static_cast<std::uint32_t>(std::max(0.0, lua_tonumber(L, -1))) & 0xFFFFFFu;
      lua_settop(L, -2);
      Put32(dds, 0xFF000000u | rgb);        // B, G, R, A in memory = 0xAARRGGBB little endian
    }

    std::string out;
    out.append(file, 0, 30);
    Put32(out, static_cast<std::uint32_t>(dds.size()));
    out.append(dds);
    out.append(file, 34ull + oldBytes, std::string::npos);

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
      return Fail(L, "cannot write the map file");
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != out.size()) {
      return Fail(L, "write failed");
    }
    lua_pushboolean(L, 1);
    return 1;
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
      (void)new moho::CScrLuaBinder(set, "ed_image_avg", &cfunc_EdImageAvg, nullptr, "<global>", "ed_image_avg(path) -> r, g, b");
      (void)new moho::CScrLuaBinder(set, "ed_scmap_set_preview", &cfunc_EdScmapSetPreview, nullptr, "<global>", "ed_scmap_set_preview(path, w, h, pixels) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_file_dialog", &cfunc_EdFileDialog, nullptr, "<global>", "ed_file_dialog(save, title, filter, name) -> path | nil");
    }
  };

  const EdImageIOLuaBootstrap gEdImageIOLuaBootstrap{};
} // namespace

// EDITOR: file reader of EdImageIO (plain files and entries inside the game's archives) for the icon natives (EdIcon.cpp).
namespace moho::edicon
{
  bool ReadAllBytes(const std::string& path, std::string& out)
  {
    std::vector<std::uint8_t> bytes;
    if (!ReadAll(path, bytes)) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
  }
} // namespace moho::edicon

// EDITOR: image reader for the field engine (EdField.cpp): 16-bit greyscale of any WIC format or a square raw file, no resampling.
namespace moho::edfield
{
  bool ReadImage16(const std::string& utf8Path, std::vector<std::uint16_t>& out, int& w, int& h)
  {
    const std::wstring path = Widen(utf8Path.c_str());
    const std::wstring ext = Extension(path);
    if (ext == L"raw" || ext == L"r16") {
      return ReadRaw(path, out, w, h);
    }
    ComScope com;
    return ReadWic(path, out, w, h);
  }
} // namespace moho::edfield
