// EDITOR: image and file natives for the terrain interop (docs/projects/editor/TERRAIN_INTEROP.md; the ops are in EdFieldInterop.cpp, the field store in EdField.cpp).
//
//   ed_img_info(path[, opts])        -> info | nil, message     opts for a raw file: bits (16 | 32), float (0 | 1), big (0 | 1 = big endian), width, height (default: a square from the file size)
//                                    info = { width, height, bits (per channel), channels, float (0 | 1), format ("16bppGray" ...), container ("png" "tiff" "raw" ...), frames, alpha (0 | 1) }
//   ed_img_read(path[, opts])        -> field, info | nil, message     any WIC image (PNG, TIFF 8 / 16 / 32 float, BMP, JPEG, ... what Windows can decode) or a raw file, as a float field
//                                    (cell 1, origin 0). Integer formats come as 0..1 (value / maximum), float formats as stored. opts: channel = gray (default: luma of colour images) |
//                                    r | g | b | a; raw options as above; nodata = value: NaN, infinity, that value and anything below -1e30 become the lowest valid value (info.nodata = how many)
//   ed_img_write(path, field[, opts]) -> true | nil, message    (creates the folder) .png (8 / 16 bit), .tif / .tiff (8 / 16 bit, 32 = float), .raw / .r16 (16 bit words) or .r32 (float). opts: bits (8 | 16 | 32),
//                                    lo, hi (the field value that becomes 0 and the maximum; default: the field's range), big (0 | 1: big endian raw)
//   ed_dds_fields(path, size, span, r, g, b, a[, mips]) -> bytes | nil, message    a size x size DXT5 DDS (mip maps down to 1 x 1 unless mips is given) from four fields of 0..1 (ids; the same id may
//                                    stand for several channels). span = the world size of the map: the texel centres at (i + 0.5) * span / size are sampled in the fields (bilinear), whatever their resolution. No
//                                    Lua tables of pixels, so 4096 and 8192 wide textures fit
//   ed_dir_stamps(dir[, exts])       -> { name = "size:mtime" } | nil   the files of a folder (not recursive), exts = "png;tif;tiff;raw;r16;r32"; cheap change detection for the live link
//   ed_files_dialog(title, filter)   -> { path, ... } | nil     open dialog with multi-selection
//   ed_folder_dialog(title)          -> path | nil              folder picker
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wincodec.h>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/editor/EdDdsEncode.h"
#include "moho/editor/EdFieldCore.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace moho::editor::field
{
  int StoreGrid(Grid&& g);
  GridPtr FindGrid(int id);
} // namespace moho::editor::field

namespace
{
  using moho::editor::field::Grid;
  namespace fs = std::filesystem;

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
      uninit = SUCCEEDED(hr);
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

  int Fail(lua_State* const L, const std::string& message)
  {
    lua_pushnil(L);
    lua_pushstring(L, message.c_str());
    return 2;
  }

  double OptNum(lua_State* const L, const int index, const char* const key, const double fallback)
  {
    if (lua_type(L, index) != LUA_TTABLE) {
      return fallback;
    }
    lua_pushstring(L, key);
    lua_gettable(L, index);
    double v = fallback;
    if (lua_type(L, -1) == LUA_TNUMBER) {
      v = lua_tonumber(L, -1);
    }
    lua_settop(L, -2);
    return v;
  }

  bool OptHas(lua_State* const L, const int index, const char* const key)
  {
    if (lua_type(L, index) != LUA_TTABLE) {
      return false;
    }
    lua_pushstring(L, key);
    lua_gettable(L, index);
    const bool has = lua_type(L, -1) == LUA_TNUMBER;
    lua_settop(L, -2);
    return has;
  }

  std::string OptStr(lua_State* const L, const int index, const char* const key, const char* const fallback)
  {
    if (lua_type(L, index) != LUA_TTABLE) {
      return fallback;
    }
    lua_pushstring(L, key);
    lua_gettable(L, index);
    std::string v = lua_type(L, -1) == LUA_TSTRING ? std::string(lua_tostring(L, -1)) : std::string(fallback);
    lua_settop(L, -2);
    return v;
  }

  void SetNum(lua_State* const L, const char* const key, const double v)
  {
    lua_pushstring(L, key);
    lua_pushnumber(L, static_cast<lua_Number>(v));
    lua_settable(L, -3);
  }

  void SetStr(lua_State* const L, const char* const key, const std::string& v)
  {
    lua_pushstring(L, key);
    lua_pushstring(L, v.c_str());
    lua_settable(L, -3);
  }

  struct Info
  {
    int width = 0;
    int height = 0;
    int bits = 0;
    int channels = 1;
    bool isFloat = false;
    bool alpha = false;
    int frames = 1;
    std::string format;
    std::string container;
    long nodata = 0;
  };

  struct RawSpec
  {
    int bits = 16;
    bool isFloat = false;
    bool big = false;
    int width = 0;
    int height = 0;
  };

  void PushInfo(lua_State* const L, const Info& i)
  {
    lua_newtable(L);
    SetNum(L, "width", i.width);
    SetNum(L, "height", i.height);
    SetNum(L, "bits", i.bits);
    SetNum(L, "channels", i.channels);
    SetNum(L, "float", i.isFloat ? 1 : 0);
    SetNum(L, "alpha", i.alpha ? 1 : 0);
    SetNum(L, "frames", i.frames);
    SetNum(L, "nodata", static_cast<double>(i.nodata));
    SetStr(L, "format", i.format);
    SetStr(L, "container", i.container);
  }

  // ---- raw files ----------------------------------------------------------------------------------------------------------------------------

  bool IsRawExt(const std::wstring& ext)
  {
    return ext == L"raw" || ext == L"r16" || ext == L"r32" || ext == L"bin";
  }

  RawSpec RawSpecOf(lua_State* const L, const int index, const std::wstring& ext)
  {
    RawSpec s;
    if (ext == L"r32") {
      s.bits = 32;
      s.isFloat = true;
    }
    s.bits = static_cast<int>(OptNum(L, index, "bits", s.bits));
    if (OptHas(L, index, "float")) {
      s.isFloat = OptNum(L, index, "float", 0) > 0.5;
    } else if (s.bits == 32) {
      s.isFloat = true;
    }
    s.big = OptNum(L, index, "big", 0) > 0.5;
    s.width = static_cast<int>(OptNum(L, index, "width", 0));
    s.height = static_cast<int>(OptNum(L, index, "height", 0));
    return s;
  }

  bool ReadFileBytes(const std::wstring& path, std::vector<std::uint8_t>& out)
  {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() ? TRUE : ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(file);
    return ok && read == out.size();
  }

  bool ReadRawField(const std::wstring& path, const RawSpec& spec, Grid& out, Info& info, std::string& err)
  {
    std::vector<std::uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) {
      err = "cannot read the file";
      return false;
    }
    if (spec.bits != 16 && spec.bits != 32) {
      err = "raw files are 16 or 32 bit";
      return false;
    }
    const std::size_t bytesPer = static_cast<std::size_t>(spec.bits / 8);
    const std::size_t count = bytes.size() / bytesPer;
    int w = spec.width;
    int h = spec.height;
    if (w < 1 || h < 1) {
      const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(count))));
      if (side < 2 || static_cast<std::size_t>(side) * static_cast<std::size_t>(side) != count) {
        err = "not a square raw file: " + std::to_string(count) + " samples of " + std::to_string(spec.bits) + " bit (give the width and height)";
        return false;
      }
      w = side;
      h = side;
    } else if (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) != count) {
      err = "the file holds " + std::to_string(count) + " samples, not " + std::to_string(w) + " x " + std::to_string(h);
      return false;
    }
    out = Grid(w, h, 1.0f, 0.0f, 0.0f);
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint8_t* p = &bytes[i * bytesPer];
      if (spec.bits == 16) {
        const std::uint32_t word = spec.big ? (static_cast<std::uint32_t>(p[0]) << 8) | p[1] : (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
        out.v[i] = static_cast<float>(word) / 65535.0f;
      } else {
        std::uint32_t bits = spec.big ? (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3]
                                      : (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
        if (spec.isFloat) {
          float f;
          std::memcpy(&f, &bits, 4);
          out.v[i] = f;
        } else {
          out.v[i] = static_cast<float>(static_cast<double>(bits) / 4294967295.0);
        }
      }
    }
    info.width = w;
    info.height = h;
    info.bits = spec.bits;
    info.channels = 1;
    info.isFloat = spec.bits == 32 && spec.isFloat;
    info.format = spec.bits == 32 ? (spec.isFloat ? "raw float32" : "raw uint32") : "raw uint16";
    info.container = "raw";
    return true;
  }

  // ---- WIC ----------------------------------------------------------------------------------------------------------------------------------

  std::string ContainerName(const GUID& g)
  {
    if (g == GUID_ContainerFormatPng) return "png";
    if (g == GUID_ContainerFormatTiff) return "tiff";
    if (g == GUID_ContainerFormatBmp) return "bmp";
    if (g == GUID_ContainerFormatJpeg) return "jpeg";
    if (g == GUID_ContainerFormatGif) return "gif";
    if (g == GUID_ContainerFormatWmp) return "jpegxr";
    if (g == GUID_ContainerFormatDds) return "dds";
    return "other";
  }

  struct WicImage
  {
    Ptr<IWICImagingFactory> factory;
    Ptr<IWICBitmapDecoder> decoder;
    Ptr<IWICBitmapFrameDecode> frame;
    WICPixelFormatGUID format{};
  };

  bool OpenWic(const std::wstring& path, WicImage& img, Info& info, std::string& err)
  {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&img.factory)))) {
      err = "WIC is not available";
      return false;
    }
    if (FAILED(img.factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &img.decoder))) {
      err = "no decoder for this file (Windows can read PNG, TIFF, BMP, JPEG; not EXR: convert it to a 32-bit float TIFF)";
      return false;
    }
    UINT frames = 1;
    img.decoder->GetFrameCount(&frames);
    info.frames = static_cast<int>(frames);
    GUID container{};
    if (SUCCEEDED(img.decoder->GetContainerFormat(&container))) {
      info.container = ContainerName(container);
    }
    if (FAILED(img.decoder->GetFrame(0, &img.frame)) || FAILED(img.frame->GetPixelFormat(&img.format))) {
      err = "cannot read the image frame";
      return false;
    }
    UINT w = 0;
    UINT h = 0;
    img.frame->GetSize(&w, &h);
    info.width = static_cast<int>(w);
    info.height = static_cast<int>(h);
    Ptr<IWICComponentInfo> ci;
    if (SUCCEEDED(img.factory->CreateComponentInfo(img.format, &ci))) {
      Ptr<IWICPixelFormatInfo2> pfi;
      if (SUCCEEDED(ci->QueryInterface(IID_PPV_ARGS(&pfi)))) {
        UINT bpp = 0;
        UINT ch = 1;
        pfi->GetBitsPerPixel(&bpp);
        pfi->GetChannelCount(&ch);
        WICPixelFormatNumericRepresentation rep = WICPixelFormatNumericRepresentationUnspecified;
        pfi->GetNumericRepresentation(&rep);
        BOOL alpha = FALSE;
        pfi->SupportsTransparency(&alpha);
        info.channels = static_cast<int>(ch);
        info.bits = ch > 0 ? static_cast<int>(bpp / ch) : static_cast<int>(bpp);
        info.isFloat = rep == WICPixelFormatNumericRepresentationFloat || rep == WICPixelFormatNumericRepresentationFixed;
        info.alpha = alpha != FALSE;
      }
      WCHAR name[96] = {};
      UINT got = 0;
      ci->GetFriendlyName(96, name, &got);
      info.format = Narrow(std::wstring(name));
    }
    if (info.bits <= 0) {
      info.bits = 8;
    }
    return true;
  }

  bool ReadWicField(const std::wstring& path, const std::string& channel, Grid& out, Info& info, std::string& err)
  {
    WicImage img;
    if (!OpenWic(path, img, info, err)) {
      return false;
    }
    const std::size_t count = static_cast<std::size_t>(info.width) * static_cast<std::size_t>(info.height);
    if (info.width < 2 || info.height < 2 || count > (1ull << 28)) {
      err = "unsupported image size";
      return false;
    }
    out = Grid(info.width, info.height, 1.0f, 0.0f, 0.0f);
    const UINT w = static_cast<UINT>(info.width);
    const UINT h = static_cast<UINT>(info.height);
    const bool colour = info.channels >= 3;
    int pick = 0;                      // 0 luma / gray, 1 r, 2 g, 3 b, 4 a
    if (channel == "r") pick = 1;
    else if (channel == "g") pick = 2;
    else if (channel == "b") pick = 3;
    else if (channel == "a") pick = 4;

    if (img.format == GUID_WICPixelFormat32bppGrayFloat && pick <= 1) {
      if (FAILED(img.frame->CopyPixels(nullptr, w * 4U, static_cast<UINT>(count * 4), reinterpret_cast<BYTE*>(out.v.data())))) {
        err = "cannot read the pixels";
        return false;
      }
      return true;
    }

    if (pick <= 1 && (img.format == GUID_WICPixelFormat16bppGray || img.format == GUID_WICPixelFormat8bppGray)) {
      // greyscale integer images are read as stored: the WIC converter would move 16-bit values by a step
      const bool deep = img.format == GUID_WICPixelFormat16bppGray;
      std::vector<std::uint8_t> raw(count * (deep ? 2 : 1));
      if (FAILED(img.frame->CopyPixels(nullptr, w * (deep ? 2U : 1U), static_cast<UINT>(raw.size()), raw.data()))) {
        err = "cannot read the pixels";
        return false;
      }
      if (deep) {
        const auto* words = reinterpret_cast<const std::uint16_t*>(raw.data());
        for (std::size_t i = 0; i < count; ++i) out.v[i] = static_cast<float>(words[i]) / 65535.0f;
      } else {
        for (std::size_t i = 0; i < count; ++i) out.v[i] = static_cast<float>(raw[i]) / 255.0f;
      }
      return true;
    }

    Ptr<IWICFormatConverter> conv;
    if (FAILED(img.factory->CreateFormatConverter(&conv))) {
      err = "no format converter";
      return false;
    }
    const auto weight = [&](const float r, const float g, const float b, const float a) {
      switch (pick) {
      case 1: return r;
      case 2: return g;
      case 3: return b;
      case 4: return a;
      default: return colour ? 0.299f * r + 0.587f * g + 0.114f * b : r;
      }
    };
    if (info.isFloat) {
      if (FAILED(conv->Initialize(img.frame.p, GUID_WICPixelFormat128bppRGBAFloat, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        err = "cannot convert the float image";
        return false;
      }
      std::vector<float> px(count * 4);
      if (FAILED(conv->CopyPixels(nullptr, w * 16U, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(px.data())))) {
        err = "cannot read the pixels";
        return false;
      }
      for (std::size_t i = 0; i < count; ++i) {
        out.v[i] = weight(px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]);
      }
    } else {
      if (FAILED(conv->Initialize(img.frame.p, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        err = "cannot convert the image";
        return false;
      }
      std::vector<std::uint16_t> px(count * 4);
      if (FAILED(conv->CopyPixels(nullptr, w * 8U, static_cast<UINT>(px.size() * 2), reinterpret_cast<BYTE*>(px.data())))) {
        err = "cannot read the pixels";
        return false;
      }
      for (std::size_t i = 0; i < count; ++i) {
        out.v[i] = weight(px[i * 4] / 65535.0f, px[i * 4 + 1] / 65535.0f, px[i * 4 + 2] / 65535.0f, px[i * 4 + 3] / 65535.0f);
      }
    }
    return true;
  }

  /** Float data from GIS tools carries "no data" cells: replace them by the lowest valid value so they do not become spikes. */
  void FixNoData(Grid& g, Info& info, const bool hasNodata, const double nodata)
  {
    float lowest = 3.4e38f;
    const auto bad = [&](const float v) {
      return !std::isfinite(v) || v < -1.0e30f || (hasNodata && static_cast<double>(v) == static_cast<double>(static_cast<float>(nodata)));
    };
    for (const float v : g.v) {
      if (!bad(v) && v < lowest) lowest = v;
    }
    if (lowest > 3.0e38f) lowest = 0.0f;
    for (float& v : g.v) {
      if (bad(v)) {
        v = lowest;
        ++info.nodata;
      }
    }
  }

  int __cdecl cfunc_ImgInfo(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    if (utf8 == nullptr) {
      return Fail(L, "bad path");
    }
    const std::wstring path = Widen(utf8);
    const std::wstring ext = Extension(path);
    Info info;
    std::string err;
    if (IsRawExt(ext)) {
      const RawSpec spec = RawSpecOf(L, 2, ext);
      WIN32_FILE_ATTRIBUTE_DATA data{};
      if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return Fail(L, "cannot read the file");
      }
      const unsigned long long size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
      const unsigned long long count = size / static_cast<unsigned long long>(spec.bits / 8);
      int w = spec.width;
      int h = spec.height;
      if (w < 1 || h < 1) {
        const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(count))));
        if (side < 2 || static_cast<unsigned long long>(side) * side != count) {
          return Fail(L, "not a square raw file: " + std::to_string(count) + " samples of " + std::to_string(spec.bits) + " bit (give the width and height)");
        }
        w = side;
        h = side;
      }
      info.width = w;
      info.height = h;
      info.bits = spec.bits;
      info.isFloat = spec.bits == 32 && spec.isFloat;
      info.format = spec.bits == 32 ? (spec.isFloat ? "raw float32" : "raw uint32") : "raw uint16";
      info.container = "raw";
    } else {
      ComScope com;
      WicImage img;
      if (!OpenWic(path, img, info, err)) {
        return Fail(L, err);
      }
    }
    PushInfo(L, info);
    return 1;
  }

  int __cdecl cfunc_ImgRead(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    if (utf8 == nullptr) {
      return Fail(L, "bad path");
    }
    const std::wstring path = Widen(utf8);
    const std::wstring ext = Extension(path);
    Grid g;
    Info info;
    std::string err;
    if (IsRawExt(ext)) {
      if (!ReadRawField(path, RawSpecOf(L, 2, ext), g, info, err)) {
        return Fail(L, err);
      }
    } else {
      ComScope com;
      if (!ReadWicField(path, OptStr(L, 2, "channel", "gray"), g, info, err)) {
        return Fail(L, err);
      }
    }
    if (info.isFloat) {
      FixNoData(g, info, OptHas(L, 2, "nodata"), OptNum(L, 2, "nodata", 0));
    }
    lua_pushnumber(L, static_cast<lua_Number>(moho::editor::field::StoreGrid(std::move(g))));
    PushInfo(L, info);
    return 2;
  }

  // ---- writing ------------------------------------------------------------------------------------------------------------------------------

  /** Baseline TIFF, one strip, 32-bit IEEE float, one sample per pixel (WIC's TIFF encoder has no float grey format). Little endian. */
  bool WriteFloatTiff(const std::wstring& path, const Grid& g)
  {
    std::vector<std::uint8_t> out;
    const auto put16 = [&](const std::uint32_t v) { out.push_back(static_cast<std::uint8_t>(v)); out.push_back(static_cast<std::uint8_t>(v >> 8)); };
    const auto put32 = [&](const std::uint32_t v) { put16(v & 0xffffu); put16(v >> 16); };
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(g.Count() * 4);
    constexpr std::uint32_t kEntries = 10;
    constexpr std::uint32_t kData = 8 + 2 + kEntries * 12 + 4;
    out.reserve(kData + dataBytes);
    out.push_back('I'); out.push_back('I'); put16(42); put32(8);
    put16(kEntries);
    const auto entry = [&](const std::uint32_t tag, const std::uint32_t type, const std::uint32_t value) {
      put16(tag); put16(type); put32(1);
      if (type == 3) { put16(value); put16(0); } else { put32(value); }
    };
    entry(256, 4, static_cast<std::uint32_t>(g.w));
    entry(257, 4, static_cast<std::uint32_t>(g.h));
    entry(258, 3, 32);
    entry(259, 3, 1);
    entry(262, 3, 1);
    entry(273, 4, kData);
    entry(277, 3, 1);
    entry(278, 4, static_cast<std::uint32_t>(g.h));
    entry(279, 4, dataBytes);
    entry(339, 3, 3);
    put32(0);
    const std::size_t at = out.size();
    out.resize(at + dataBytes);
    std::memcpy(out.data() + at, g.v.data(), dataBytes);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == out.size();
  }

  int __cdecl cfunc_ImgWrite(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    const int id = static_cast<int>(luaL_checknumber(L, 2));
    if (utf8 == nullptr) {
      return Fail(L, "bad path");
    }
    const moho::editor::field::GridPtr gp = moho::editor::field::FindGrid(id);
    if (!gp) {
      return Fail(L, "no such field");
    }
    const Grid& g = *gp;
    const std::wstring path = Widen(utf8);
    const std::wstring ext = Extension(path);
    {
      std::error_code ec;
      const fs::path parent = fs::path(path).parent_path();
      if (!parent.empty()) {
        fs::create_directories(parent, ec);        // an export into a new folder
      }
    }
    int bits = static_cast<int>(OptNum(L, 3, "bits", 16));
    const bool big = OptNum(L, 3, "big", 0) > 0.5;
    double lo = 0.0;
    double hi = 0.0;
    if (OptHas(L, 3, "lo") && OptHas(L, 3, "hi")) {
      lo = OptNum(L, 3, "lo", 0);
      hi = OptNum(L, 3, "hi", 1);
    } else {
      lo = *std::min_element(g.v.begin(), g.v.end());
      hi = *std::max_element(g.v.begin(), g.v.end());
    }
    const double range = hi > lo ? hi - lo : 1.0;
    const std::size_t count = g.Count();
    const auto norm = [&](const float v) {
      const double t = (static_cast<double>(v) - lo) / range;
      return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    };

    if (IsRawExt(ext)) {
      if (ext == L"r32") bits = 32;
      std::vector<std::uint8_t> bytes;
      if (bits == 32) {
        bytes.resize(count * 4);
        for (std::size_t i = 0; i < count; ++i) {
          std::uint32_t u;
          std::memcpy(&u, &g.v[i], 4);
          std::uint8_t* p = &bytes[i * 4];
          if (big) { p[0] = u >> 24; p[1] = u >> 16; p[2] = u >> 8; p[3] = u; }
          else { p[3] = u >> 24; p[2] = u >> 16; p[1] = u >> 8; p[0] = u; }
        }
      } else {
        bytes.resize(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
          const std::uint32_t word = static_cast<std::uint32_t>(std::floor(norm(g.v[i]) * 65535.0 + 0.5));
          std::uint8_t* p = &bytes[i * 2];
          if (big) { p[0] = word >> 8; p[1] = word; } else { p[1] = word >> 8; p[0] = word; }
        }
      }
      HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        return Fail(L, "cannot create the file");
      }
      DWORD written = 0;
      const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
      CloseHandle(file);
      if (!ok || written != bytes.size()) {
        return Fail(L, "write failed");
      }
      lua_pushboolean(L, 1);
      return 1;
    }

    const bool tiff = ext == L"tif" || ext == L"tiff";
    if (!tiff && ext != L"png") {
      return Fail(L, "write .png, .tif / .tiff, .raw / .r16 or .r32");
    }
    if (bits != 8 && bits != 16 && bits != 32) {
      return Fail(L, "bits must be 8, 16 or 32");
    }
    if (bits == 32 && !tiff) {
      return Fail(L, "PNG has no float format: use .tif or .r32");
    }
    if (bits == 32) {
      if (!WriteFloatTiff(path, g)) {
        return Fail(L, "cannot write the file");
      }
      lua_pushboolean(L, 1);
      return 1;
    }
    ComScope com;
    Ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
      return Fail(L, "WIC is not available");
    }
    Ptr<IWICStream> stream;
    Ptr<IWICBitmapEncoder> encoder;
    Ptr<IWICBitmapFrameEncode> frame;
    Ptr<IPropertyBag2> options;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(tiff ? GUID_ContainerFormatTiff : GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &options)) ||
        FAILED(frame->Initialize(options.p)) || FAILED(frame->SetSize(static_cast<UINT>(g.w), static_cast<UINT>(g.h)))) {
      return Fail(L, "cannot create the image");
    }
    WICPixelFormatGUID format = bits == 8 ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat16bppGray;
    const WICPixelFormatGUID wanted = format;
    if (FAILED(frame->SetPixelFormat(&format)) || format != wanted) {
      return Fail(L, "the encoder has no such greyscale format");
    }
    std::vector<std::uint8_t> bytes;
    UINT stride = 0;
    if (bits == 8) {
      stride = static_cast<UINT>(g.w);
      bytes.resize(count);
      for (std::size_t i = 0; i < count; ++i) bytes[i] = static_cast<std::uint8_t>(std::floor(norm(g.v[i]) * 255.0 + 0.5));
    } else if (bits == 16) {
      stride = static_cast<UINT>(g.w) * 2U;
      bytes.resize(count * 2);
      auto* words = reinterpret_cast<std::uint16_t*>(bytes.data());
      for (std::size_t i = 0; i < count; ++i) words[i] = static_cast<std::uint16_t>(std::floor(norm(g.v[i]) * 65535.0 + 0.5));
    }
    if (FAILED(frame->WritePixels(static_cast<UINT>(g.h), stride, static_cast<UINT>(bytes.size()), bytes.data())) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
      return Fail(L, "cannot write the image");
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  // ---- folders and dialogs ------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_DirStamps(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    if (utf8 == nullptr) {
      return Fail(L, "bad path");
    }
    std::vector<std::wstring> exts;
    if (lua_type(L, 2) == LUA_TSTRING) {
      std::wstring all = Widen(lua_tostring(L, 2));
      std::wstring cur;
      for (const wchar_t c : all) {
        if (c == L';' || c == L',' || c == L' ') {
          if (!cur.empty()) exts.push_back(cur);
          cur.clear();
        } else {
          cur.push_back(static_cast<wchar_t>(towlower(c)));
        }
      }
      if (!cur.empty()) exts.push_back(cur);
    }
    std::error_code ec;
    fs::directory_iterator it(fs::path(Widen(utf8)), ec);
    if (ec) {
      lua_pushnil(L);
      return 1;
    }
    lua_newtable(L);
    for (; it != fs::directory_iterator(); it.increment(ec)) {
      if (ec) break;
      std::error_code e2;
      if (!it->is_regular_file(e2)) continue;
      const std::wstring ext = Extension(it->path().filename().wstring());
      if (!exts.empty() && std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
      const auto size = it->file_size(e2);
      if (e2) continue;
      const auto time = it->last_write_time(e2);
      if (e2) continue;
      char text[64];
      std::snprintf(text, sizeof(text), "%llu:%lld", static_cast<unsigned long long>(size), static_cast<long long>(time.time_since_epoch().count()));
      lua_pushstring(L, Narrow(it->path().filename().wstring()).c_str());
      lua_pushstring(L, text);
      lua_settable(L, -3);
    }
    return 1;
  }

  std::wstring DialogFilter(const char* const filter)
  {
    // "Description|*.png;*.raw|Other|*.*" -> NUL separated pairs
    std::wstring spec = Widen(filter != nullptr && filter[0] != 0 ? filter : "All files|*.*");
    for (wchar_t& c : spec) {
      if (c == L'|') c = L'\0';
    }
    spec.push_back(L'\0');
    spec.push_back(L'\0');
    return spec;
  }

  int __cdecl cfunc_FilesDialog(lua_State* const L)
  {
    const std::wstring spec = DialogFilter(lua_tostring(L, 2));
    const std::wstring caption = Widen(lua_tostring(L, 1) != nullptr ? lua_tostring(L, 1) : "Open");
    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = spec.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = caption.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
      lua_pushnil(L);
      return 1;
    }
    // one file: the full path; several: directory, then the names (all NUL separated)
    std::vector<std::wstring> parts;
    const wchar_t* p = buffer.data();
    while (*p != L'\0') {
      parts.emplace_back(p);
      p += parts.back().size() + 1;
    }
    lua_newtable(L);
    int n = 0;
    if (parts.size() == 1) {
      lua_pushnumber(L, 1);
      lua_pushstring(L, Narrow(parts[0]).c_str());
      lua_settable(L, -3);
    } else {
      for (std::size_t i = 1; i < parts.size(); ++i) {
        lua_pushnumber(L, static_cast<lua_Number>(++n));
        lua_pushstring(L, Narrow(parts[0] + L"\\" + parts[i]).c_str());
        lua_settable(L, -3);
      }
    }
    return 1;
  }

  int __cdecl cfunc_FolderDialog(lua_State* const L)
  {
    ComScope com;
    Ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
      lua_pushnil(L);
      return 1;
    }
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (lua_tostring(L, 1) != nullptr) {
      dialog->SetTitle(Widen(lua_tostring(L, 1)).c_str());
    }
    if (FAILED(dialog->Show(GetActiveWindow()))) {
      lua_pushnil(L);
      return 1;
    }
    Ptr<IShellItem> item;
    PWSTR name = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || name == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushstring(L, Narrow(std::wstring(name)).c_str());
    CoTaskMemFree(name);
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

  int __cdecl cfunc_DdsFields(lua_State* const L)
  {
    const char* const utf8 = lua_tostring(L, 1);
    const int size = static_cast<int>(luaL_checknumber(L, 2));
    const double span = luaL_checknumber(L, 3);
    if (utf8 == nullptr) {
      return Fail(L, "bad path");
    }
    if (size < 4 || size > 16384 || !(span > 0.0)) {
      return Fail(L, "bad size or span");
    }
    moho::editor::field::GridPtr grids[4];
    int ids[4];
    for (int c = 0; c < 4; ++c) {
      ids[c] = static_cast<int>(luaL_checknumber(L, 4 + c));
      grids[c] = moho::editor::field::FindGrid(ids[c]);
      if (!grids[c]) {
        return Fail(L, "no such field");
      }
    }
    const int mips = lua_type(L, 8) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L, 8)) : 99;
    moho::editor::dds::Image img;
    img.w = size;
    img.h = size;
    img.px.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
    const double cell = span / static_cast<double>(size);
    const auto rows = [&](const int z0, const int z1) {
      for (int z = z0; z < z1; ++z) {
        const double wz = (static_cast<double>(z) + 0.5) * cell;
        for (int x = 0; x < size; ++x) {
          const double wx = (static_cast<double>(x) + 0.5) * cell;
          std::uint8_t v[4];
          for (int c = 0; c < 4; ++c) {
            int same = -1;
            for (int d = 0; d < c; ++d) {
              if (ids[d] == ids[c]) {
                same = d;
                break;
              }
            }
            if (same >= 0) {
              v[c] = v[same];
              continue;
            }
            const float f = moho::editor::field::SampleWorld(*grids[c], wx, wz) * 255.0f + 0.5f;
            v[c] = static_cast<std::uint8_t>(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
          }
          img.px[static_cast<std::size_t>(z) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] = moho::editor::dds::Px{v[0], v[1], v[2], v[3]};
        }
      }
    };
    const unsigned workers = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    {
      std::vector<std::thread> pool;
      for (unsigned t = 0; t < workers; ++t) {
        pool.emplace_back(rows, static_cast<int>(static_cast<long long>(size) * t / workers), static_cast<int>(static_cast<long long>(size) * (t + 1) / workers));
      }
      for (std::thread& th : pool) {
        th.join();
      }
    }
    const std::string bytes = moho::editor::dds::EncodeDxt5(img, std::max(1, mips));
    const std::wstring path = Widen(utf8);
    {
      std::error_code ec;
      const fs::path parent = fs::path(path).parent_path();
      if (!parent.empty()) {
        fs::create_directories(parent, ec);
      }
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return Fail(L, "cannot create the file");
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != bytes.size()) {
      return Fail(L, "write failed");
    }
    lua_pushnumber(L, static_cast<double>(bytes.size()));
    return 1;
  }

  struct EdInteropLuaBootstrap
  {
    EdInteropLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_img_info", &cfunc_ImgInfo, nullptr, "<global>", "ed_img_info(path[, opts]) -> info | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_img_read", &cfunc_ImgRead, nullptr, "<global>", "ed_img_read(path[, opts]) -> field, info | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_img_write", &cfunc_ImgWrite, nullptr, "<global>", "ed_img_write(path, field[, opts]) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_dds_fields", &cfunc_DdsFields, nullptr, "<global>", "ed_dds_fields(path, size, span, r, g, b, a[, mips]) -> bytes | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_dir_stamps", &cfunc_DirStamps, nullptr, "<global>", "ed_dir_stamps(dir[, exts]) -> { name = stamp }");
      (void)new moho::CScrLuaBinder(set, "ed_files_dialog", &cfunc_FilesDialog, nullptr, "<global>", "ed_files_dialog(title, filter) -> { path, ... } | nil");
      (void)new moho::CScrLuaBinder(set, "ed_folder_dialog", &cfunc_FolderDialog, nullptr, "<global>", "ed_folder_dialog(title) -> path | nil");
    }
  };

  const EdInteropLuaBootstrap gEdInteropLuaBootstrap{};
} // namespace
