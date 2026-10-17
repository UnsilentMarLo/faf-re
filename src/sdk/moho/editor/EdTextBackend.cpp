#include "moho/editor/EdTextBackend.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "platform/Platform.h"

#include "lua/LuaRuntimeTypes.h"
#include "lua/LuaPrimitives.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/CD3DPrimBatcher.h"
#include "moho/render/textures/CD3DBatchTexture.h"

namespace moho
{
  namespace
  {
    // Copy of the C ABI of editor/edtext/edtext.h (keep in sync).
    struct EdFont;
    struct EdGlyph
    {
      std::uint32_t face;
      std::uint32_t glyph;
      float x;
      float advance;
    };

    struct EdApi
    {
      bool ok = false;
      EdFont* (*fontCreate)(const wchar_t*, float) = nullptr;
      void (*fontDestroy)(EdFont*) = nullptr;
      void (*fontMetrics)(EdFont*, float*, float*, float*) = nullptr;
      int (*layout)(EdFont*, const wchar_t*, int, EdGlyph*, int, float*) = nullptr;
      int (*positions)(EdFont*, const wchar_t*, int, float*) = nullptr;
      int (*raster)(EdFont*, std::uint32_t, std::uint32_t, int, int*, int*, int*, int*, std::uint8_t*, int) = nullptr;
    };

    EdApi LoadApi()
    {
      EdApi api;
      wchar_t path[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, path, MAX_PATH);
      std::wstring dir(path);
      const std::size_t slash = dir.find_last_of(L"\\/");
      dir = (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash + 1);
      HMODULE mod = LoadLibraryW((dir + L"edtext.dll").c_str());
      if (mod == nullptr) {
        mod = LoadLibraryW(L"edtext.dll");
      }
      if (mod == nullptr) {
        return api;
      }
      api.fontCreate = reinterpret_cast<decltype(api.fontCreate)>(GetProcAddress(mod, "edt_font_create"));
      api.fontDestroy = reinterpret_cast<decltype(api.fontDestroy)>(GetProcAddress(mod, "edt_font_destroy"));
      api.fontMetrics = reinterpret_cast<decltype(api.fontMetrics)>(GetProcAddress(mod, "edt_font_metrics"));
      api.layout = reinterpret_cast<decltype(api.layout)>(GetProcAddress(mod, "edt_layout"));
      api.positions = reinterpret_cast<decltype(api.positions)>(GetProcAddress(mod, "edt_positions"));
      api.raster = reinterpret_cast<decltype(api.raster)>(GetProcAddress(mod, "edt_raster"));
      api.ok = api.fontCreate && api.fontDestroy && api.fontMetrics && api.layout && api.positions && api.raster;
      return api;
    }

    const EdApi& GetApi()
    {
      static const EdApi api = LoadApi();
      return api;
    }

    /**
     * One glyph bitmap as a batch texture: A8R8G8B8 (white with coverage in alpha), uploaded into a dynamic sheet of that
     * format. The CPU copy is kept so the sheet can be rebuilt after a device reset.
     */
    class CEdGlyphTexture final : public CD3DBatchTexture
    {
    public:
      CEdGlyphTexture(const std::uint32_t width, const std::uint32_t height, std::vector<std::uint32_t>&& argb)
        : CD3DBatchTexture(width, height, 0u)
        , mPixels(std::move(argb))
      {}

      void BuildTextureData(void* const destination, const std::uint32_t destinationPitchBytes) override
      {
        auto* row = static_cast<std::uint8_t*>(destination);
        for (std::uint32_t y = 0; y < mHeight; ++y) {
          std::memcpy(row, &mPixels[static_cast<std::size_t>(y) * mWidth], static_cast<std::size_t>(mWidth) * 4u);
          row += destinationPitchBytes;
        }
      }

      [[nodiscard]] std::uint8_t GetAlphaAt(const std::uint32_t x, const std::uint32_t y) const override
      {
        return (x < mWidth && y < mHeight) ? static_cast<std::uint8_t>(mPixels[static_cast<std::size_t>(y) * mWidth + x] >> 24) : 0u;
      }

      [[nodiscard]] std::uint32_t GetSheetFormat() const override { return 2u; } // A8R8G8B8

    private:
      std::vector<std::uint32_t> mPixels;
    };

    struct Layout
    {
      std::vector<EdGlyph> glyphs;
      float width = 0.0f;
    };

    // Counters for `ed_text_stats()` (cost measurements of P2).
    struct Stats
    {
      std::uint64_t runs = 0;
      std::uint64_t quads = 0;
      std::uint64_t flushes = 0;
      std::uint64_t rasterised = 0;
      std::uint64_t rasterTicks = 0;
      std::uint64_t renderTicks = 0;
    };

    Stats gStats;

    Stats GetTextStatsAndReset()
    {
      const Stats snapshot = gStats;
      gStats = Stats{};
      return snapshot;
    }

    std::uint64_t Ticks()
    {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return static_cast<std::uint64_t>(t.QuadPart);
    }

    struct State
    {
      EdFont* font = nullptr;
      float pixelSize = 0.0f;
      bool lcd = true;
      std::unordered_map<std::uint64_t, CD3DFontCharInfo> glyphCache;
      std::unordered_map<std::string, Layout> layoutCache;
      std::map<wchar_t, CD3DFontCharInfo> charInfoCache;
      std::uint8_t grayLut[256];
    };

    // Gray mode (P1): mean of the three sub-pixel coverages with a boost, because the plain mean looks dim (P0 result).
    constexpr float kGrayExponent = 0.9f;

    State& AsState(void* const state)
    {
      return *static_cast<State*>(state);
    }

    // UTF-8 to UTF-16 with a local decoder (the engine's decoder is BMP only). `starts` receives the UTF-16 offset of
    // every code point.
    void DecodeUtf8(const char* s, std::wstring& out, std::vector<int>* starts)
    {
      const auto* p = reinterpret_cast<const unsigned char*>(s);
      while (*p != 0) {
        std::uint32_t cp = 0xFFFDu;
        int n = 1;
        const unsigned char c = *p;
        if (c < 0x80) {
          cp = c;
        } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
          cp = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
          n = 2;
        } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
          cp = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
          n = 3;
        } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
          cp = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
          n = 4;
        }
        if (starts != nullptr) {
          starts->push_back(static_cast<int>(out.size()));
        }
        if (cp >= 0x10000u) {
          cp -= 0x10000u;
          out.push_back(static_cast<wchar_t>(0xD800u + (cp >> 10)));
          out.push_back(static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu)));
        } else {
          out.push_back(static_cast<wchar_t>(cp));
        }
        p += n;
      }
    }

    const Layout& GetLayout(State& st, const char* text)
    {
      const std::string key(text != nullptr ? text : "");
      const auto it = st.layoutCache.find(key);
      if (it != st.layoutCache.end()) {
        return it->second;
      }
      if (st.layoutCache.size() > 4096u) {
        st.layoutCache.clear();
      }
      Layout layout;
      std::wstring wide;
      DecodeUtf8(key.c_str(), wide, nullptr);
      const EdApi& api = GetApi();
      if (!wide.empty()) {
        layout.glyphs.resize(wide.size() + 8u);
        const int n = api.layout(st.font, wide.c_str(), static_cast<int>(wide.size()), layout.glyphs.data(), static_cast<int>(layout.glyphs.size()), &layout.width);
        if (n > static_cast<int>(layout.glyphs.size())) {
          layout.glyphs.resize(static_cast<std::size_t>(n));
          (void)api.layout(st.font, wide.c_str(), static_cast<int>(wide.size()), layout.glyphs.data(), n, &layout.width);
        }
        layout.glyphs.resize(static_cast<std::size_t>(n > 0 ? n : 0));
      }
      return st.layoutCache.emplace(key, std::move(layout)).first->second;
    }

    CD3DFontCharInfo MakeGlyph(State& st, const EdGlyph& g, const int phase)
    {
      CD3DFontCharInfo info;
      info.mAdvance = g.advance;
      const EdApi& api = GetApi();
      const std::uint64_t rasterStart = Ticks();
      ++gStats.rasterised;
      std::vector<std::uint8_t> buf(4096u);
      int left = 0;
      int top = 0;
      int w = 0;
      int h = 0;
      int ok = api.raster(st.font, g.face, g.glyph, phase, &left, &top, &w, &h, buf.data(), static_cast<int>(buf.size()));
      if (!ok && w > 0 && h > 0 && static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u > buf.size()) {
        buf.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u);
        ok = api.raster(st.font, g.face, g.glyph, phase, &left, &top, &w, &h, buf.data(), static_cast<int>(buf.size()));
      }
      if (!ok) {
        gStats.rasterTicks += Ticks() - rasterStart;
        return info; // empty glyph (space) or failure: nothing to draw
      }

      // One transparent texel of padding on every side so bilinear sampling never reaches the sheet border.
      // The atlas rounds every texture up to a multiple of 4 and derives its UV rectangle from that size, so the bitmap
      // is padded to a multiple of 4 as well (legacy DXT glyphs are 4-aligned by construction).
      const std::uint32_t pw = (static_cast<std::uint32_t>(w) + 2u + 3u) & ~3u;
      const std::uint32_t ph = (static_cast<std::uint32_t>(h) + 2u + 3u) & ~3u;
      std::vector<std::uint32_t> argb(static_cast<std::size_t>(pw) * ph, 0x00FFFFFFu);
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const std::uint8_t* s = &buf[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3u];
          std::uint32_t texel = 0;
          if (st.lcd) {
            // LCD: the three sub-pixel coverages in R, G, B (the TTextLcd shader reads them); alpha is unused.
            texel = 0xFF000000u | (static_cast<std::uint32_t>(s[0]) << 16) | (static_cast<std::uint32_t>(s[1]) << 8) | s[2];
          } else {
            const unsigned mean = (static_cast<unsigned>(s[0]) + s[1] + s[2] + 1u) / 3u;
            texel = (static_cast<std::uint32_t>(st.grayLut[mean]) << 24) | 0x00FFFFFFu;
          }
          argb[(static_cast<std::size_t>(y) + 1u) * pw + static_cast<std::size_t>(x) + 1u] = texel;
        }
      }
      if (st.lcd) {
        // padding texels: zero coverage (the LCD shader ignores alpha)
        for (std::uint32_t& texel : argb) {
          if (texel == 0x00FFFFFFu) {
            texel = 0xFF000000u;
          }
        }
      }
      boost::shared_ptr<CEdGlyphTexture> texture(new (std::nothrow) CEdGlyphTexture(pw, ph, std::move(argb)));
      info.mTex = texture;
      // Offsets relative to the baseline origin, y up (legacy convention): left, bottom, right, top.
      // The quad covers the whole padded bitmap: the glyph sits at (1,1) from its top-left corner.
      info.mV2 = static_cast<float>(left - 1);
      info.mV4 = info.mV2 + static_cast<float>(pw);
      info.mV5 = static_cast<float>(-top + 1);
      info.mV3 = info.mV5 - static_cast<float>(ph);
      gStats.rasterTicks += Ticks() - rasterStart;
      return info;
    }

    const CD3DFontCharInfo& GetGlyph(State& st, const EdGlyph& g, const int phase)
    {
      const std::uint64_t key = (static_cast<std::uint64_t>(g.face) << 32) | (static_cast<std::uint64_t>(g.glyph & 0xFFFFu) << 8) | static_cast<std::uint64_t>(phase);
      const auto it = st.glyphCache.find(key);
      if (it != st.glyphCache.end()) {
        return it->second;
      }
      return st.glyphCache.emplace(key, MakeGlyph(st, g, phase)).first->second;
    }
  } // namespace

  namespace edtext
  {
    bool IsEdFace(const char* const face)
    {
      return face != nullptr && std::strncmp(face, "ed:", 3) == 0;
    }

    std::string FamilyOf(const char* const face)
    {
      std::string s(face != nullptr ? face : "");
      if (s.compare(0, 3, "ed:") == 0) {
        s.erase(0, 3);
      }
      const std::size_t flag = s.rfind(';');
      if (flag != std::string::npos) {
        s.erase(flag);
      }
      return s;
    }

    bool WantsLcd(const char* const face)
    {
      const char* const flag = face != nullptr ? std::strrchr(face, ';') : nullptr;
      return flag == nullptr || std::strcmp(flag, ";g") != 0;
    }

    void* Attach(const char* const face, const float pixelSize, float& ascent, float& descent, float& lineGap)
    {
      const EdApi& api = GetApi();
      if (!api.ok) {
        return nullptr;
      }
      const std::string family = FamilyOf(face);
      const int wideLength = MultiByteToWideChar(CP_UTF8, 0, family.c_str(), -1, nullptr, 0);
      std::wstring wide(static_cast<std::size_t>(wideLength > 0 ? wideLength : 1), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, family.c_str(), -1, wide.data(), wideLength);
      EdFont* font = api.fontCreate(wide.c_str(), pixelSize);
      if (font == nullptr) {
        return nullptr;
      }
      auto* st = new State();
      st->font = font;
      st->pixelSize = pixelSize;
      st->lcd = WantsLcd(face);
      for (int i = 0; i < 256; ++i) {
        st->grayLut[i] = static_cast<std::uint8_t>(std::lround(255.0f * std::pow(static_cast<float>(i) / 255.0f, kGrayExponent)));
      }
      api.fontMetrics(font, &ascent, &descent, &lineGap);
      return st;
    }

    void Destroy(void* const state)
    {
      if (state == nullptr) {
        return;
      }
      State* st = static_cast<State*>(state);
      GetApi().fontDestroy(st->font);
      delete st;
    }

    Vector3f Render(
      void* const state,
      const char* const text,
      CD3DPrimBatcher* const primBatcher,
      const Vector3f& origin,
      const Vector3f& xAxis,
      const Vector3f& yAxis,
      const std::uint32_t color,
      const float maxAdvance
    )
    {
      State& st = AsState(state);
      const std::uint64_t renderStart = Ticks();
      ++gStats.runs;
      const Layout& layout = GetLayout(st, text);

      // Standard screen axes: snap the glyph quads to whole pixels and select the sub-pixel phase from the fraction.
      const bool standardAxes = xAxis.x == 1.0f && xAxis.y == 0.0f && xAxis.z == 0.0f && yAxis.x == 0.0f && yAxis.z == 0.0f &&
        std::fabs(yAxis.y) == 1.0f;

      float end = 0.0f;
      for (const EdGlyph& g : layout.glyphs) {
        if (std::isfinite(maxAdvance) && g.x + g.advance > maxAdvance) {
          break;
        }
        end = g.x + g.advance;

        Vector3f cursor = origin;
        int phase = 0;
        if (standardAxes) {
          const float absolute = origin.x + g.x;
          float whole = std::floor(absolute);
          phase = static_cast<int>(std::floor((absolute - whole) * 4.0f + 0.5f));
          if (phase >= 4) {
            phase = 0;
            whole += 1.0f;
          }
          cursor.x = whole;
          cursor.y = std::floor(origin.y + 0.5f);
        } else {
          cursor.x += xAxis.x * g.x;
          cursor.y += xAxis.y * g.x;
          cursor.z += xAxis.z * g.x;
        }

        const CD3DFontCharInfo& info = GetGlyph(st, g, phase);
        if (info.mTex) {
          if (st.lcd) {
            if (!primBatcher->mLcdTextActive) {
              // Switch to the two-pass LCD technique: flush what is queued with the caller's technique first. The technique
              // stays active for the following glyph runs; CD3DPrimBatcher::EndTextRun restores the UI technique when a
              // non-glyph quad, Setup, or the end of the UI pass comes.
              primBatcher->Flush();
              D3D_GetDevice()->SelectTechnique("TTextLcd");
              primBatcher->mLcdTextActive = true;
              ++gStats.flushes;
            }
          } else if (primBatcher->mLcdTextActive) {
            primBatcher->EndTextRun(); // gray text uses the normal technique
            ++gStats.flushes;
          }
          primBatcher->DrawChar(xAxis, yAxis, info, cursor, color);
          ++gStats.quads;
        }
      }
      gStats.renderTicks += Ticks() - renderStart;

      return Vector3f{origin.x + xAxis.x * end, origin.y + xAxis.y * end, origin.z + xAxis.z * end};
    }

    float GetAdvance(void* const state, const char* const text)
    {
      return GetLayout(AsState(state), text).width;
    }

    std::int32_t GetNearestCharacterIndex(void* const state, const char* const text, const float targetAdvance)
    {
      State& st = AsState(state);
      std::wstring wide;
      std::vector<int> starts;
      DecodeUtf8(text != nullptr ? text : "", wide, &starts);
      if (wide.empty()) {
        return 0;
      }
      std::vector<float> xs(wide.size() + 1u, 0.0f);
      (void)GetApi().positions(st.font, wide.c_str(), static_cast<int>(wide.size()), xs.data());

      std::int32_t index = 0;
      for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::size_t endIndex = (k + 1u < starts.size()) ? static_cast<std::size_t>(starts[k + 1u]) : wide.size();
        if (xs[endIndex] > targetAdvance) {
          break;
        }
        ++index;
      }
      return index;
    }

    const CD3DFontCharInfo& GetCharInfo(void* const state, const wchar_t chText)
    {
      State& st = AsState(state);
      const auto it = st.charInfoCache.find(chText);
      if (it != st.charInfoCache.end()) {
        return it->second;
      }
      std::wstring wide(1u, chText);
      EdGlyph glyphs[4] = {};
      float width = 0.0f;
      const int n = GetApi().layout(st.font, wide.c_str(), 1, glyphs, 4, &width);
      CD3DFontCharInfo info;
      if (n > 0) {
        info = GetGlyph(st, glyphs[0], 0);
      }
      info.mAdvance = width;
      return st.charInfoCache.emplace(chText, std::move(info)).first->second;
    }
  } // namespace edtext
} // namespace moho

namespace
{
  // ed_text_stats() -> runs, quads, flushes, glyphs rasterised, raster milliseconds, render milliseconds
  // Returns the counters since the last call and resets them (cost measurements, docs/projects/editor_research/P2_LCD_TEXT.md).
  int __cdecl cfunc_EdTextStats(lua_State* const state)
  {
    const moho::Stats snapshot = moho::GetTextStatsAndReset();
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
    lua_pushnumber(state, static_cast<lua_Number>(snapshot.runs));
    lua_pushnumber(state, static_cast<lua_Number>(snapshot.quads));
    lua_pushnumber(state, static_cast<lua_Number>(snapshot.flushes));
    lua_pushnumber(state, static_cast<lua_Number>(snapshot.rasterised));
    lua_pushnumber(state, static_cast<lua_Number>(static_cast<double>(snapshot.rasterTicks) * toMs));
    lua_pushnumber(state, static_cast<lua_Number>(static_cast<double>(snapshot.renderTicks) * toMs));
    return 6;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& UserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  struct EdTextLuaBootstrap
  {
    EdTextLuaBootstrap()
    {
      (void)new moho::CScrLuaBinder(UserLuaInitSet(), "ed_text_stats", &cfunc_EdTextStats, nullptr, "<global>",
                                    "ed_text_stats() -> runs, quads, flushes, rasterised, rasterMs, renderMs (and resets them)");
    }
  };

  const EdTextLuaBootstrap gEdTextLuaBootstrap{};
} // namespace
