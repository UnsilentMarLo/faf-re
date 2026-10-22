// EDITOR: texture conversion maths for the layer converter (faf_studio editor/runtime/lua/editor/stratumconv.lua, texconv.lua), header-only and free of engine types so it can be tested on
// its own (moho/editor/tests/EdTexConvTest.cpp). The Lua natives that read and write the files are EdTexConv.cpp (ed_tex_convert, ed_tex_atlas).
//
// A picture is handled as planes of floats 0..1 (one per channel). A channel of the result is made from one channel of the source by a Chan:
//
//   v = source value (r g b a, lum = 0.299 r + 0.587 g + 0.114 b, a constant, or a plane made elsewhere: `ext`)
//   v = 1 - v                                  when invert
//   v = (v - mean) * scale + 0.5 + bias        when center (the mean over the whole plane: the contrast of a texture around a neutral grey)
//   v = v * scale + bias                       otherwise
//   v = clamp(v, lo, hi)
//
// The PBR terrain shaders (effects/terrain.fx, Terrain1xx / 2xx) read the roughness and heights of the layers from the Upper layer, a 2 x 2 atlas: eight tiles (the base layer and layers 1-7),
// roughness in R and height in G for the first four, roughness in B and height in A for the second four. The shader samples a tile through
//     uv = frac(layerUv) / 4 + tileOffset + 0.125
// (atlas2D: only the middle half of a tile is read, one period of a 2 x 2 repetition), so a tile holds its picture twice in each direction, shifted by half a period: BuildAtlas.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "moho/editor/EdDdsEncode.h"

namespace moho::editor::texconv
{
  using moho::editor::dds::Image;
  using moho::editor::dds::Px;

  struct Plane
  {
    int w = 0;
    int h = 0;
    std::vector<float> v;

    float At(const int x, const int y) const { return v[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
  };

  enum class From { Keep, R, G, B, A, Lum, Const, Ext };

  struct Chan
  {
    From from = From::Keep;      // Keep: the channel passes unchanged
    float value = 0.0f;          // Const
    float scale = 1.0f;
    float bias = 0.0f;
    bool invert = false;
    bool center = false;
    float lo = 0.0f;
    float hi = 1.0f;
    const Plane* ext = nullptr;  // Ext: a plane of the size of the result
  };

  inline float Clamp01(const float v) { return std::max(0.0f, std::min(1.0f, v)); }

  inline std::uint8_t ToByte(const float v) { return static_cast<std::uint8_t>(Clamp01(v) * 255.0f + 0.5f); }

  /** One channel (R G B A Lum) of pixels as a plane. */
  inline Plane PlaneOf(const std::vector<Px>& px, const int w, const int h, const From from)
  {
    Plane p;
    p.w = w;
    p.h = h;
    p.v.resize(px.size());
    for (std::size_t i = 0; i < px.size(); ++i) {
      const Px& c = px[i];
      float v = 0.0f;
      switch (from) {
        case From::R: v = c.r / 255.0f; break;
        case From::G: v = c.g / 255.0f; break;
        case From::B: v = c.b / 255.0f; break;
        case From::A: v = c.a / 255.0f; break;
        case From::Lum: v = (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f; break;
        default: break;
      }
      p.v[i] = v;
    }
    return p;
  }

  inline Plane ConstPlane(const int w, const int h, const float value)
  {
    Plane p;
    p.w = w;
    p.h = h;
    p.v.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), value);
    return p;
  }

  namespace detail
  {
    /** Resamples one axis: area average when the result is smaller (fractional coverage at the edges), linear between texel centres when it is larger. */
    inline void Axis(const std::vector<float>& src, const int sw, const int sh, std::vector<float>& dst, const int dw, const bool horizontal)
    {
      const int srcLen = horizontal ? sw : sh;
      const int lines = horizontal ? sh : sw;
      dst.assign(static_cast<std::size_t>(lines) * static_cast<std::size_t>(dw), 0.0f);
      const auto get = [&](const int line, const int i) {
        return horizontal ? src[static_cast<std::size_t>(line) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(i)]
                          : src[static_cast<std::size_t>(i) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(line)];
      };
      for (int line = 0; line < lines; ++line) {
        for (int i = 0; i < dw; ++i) {
          float value = 0.0f;
          if (dw < srcLen) {
            const double a = static_cast<double>(i) * srcLen / dw;
            const double b = static_cast<double>(i + 1) * srcLen / dw;
            double sum = 0.0;
            for (int s = static_cast<int>(std::floor(a)); s < srcLen && s < b; ++s) {
              const double cover = std::min<double>(b, s + 1) - std::max<double>(a, s);
              if (cover > 0.0) {
                sum += cover * get(line, s);
              }
            }
            value = static_cast<float>(sum / (b - a));
          } else if (dw == srcLen) {
            value = get(line, i);
          } else {
            const double pos = std::max(0.0, std::min(static_cast<double>(srcLen - 1), (i + 0.5) * srcLen / dw - 0.5));
            const int i0 = static_cast<int>(std::floor(pos));
            const int i1 = std::min(srcLen - 1, i0 + 1);
            const double f = pos - i0;
            value = static_cast<float>(get(line, i0) * (1.0 - f) + get(line, i1) * f);
          }
          if (horizontal) {
            dst[static_cast<std::size_t>(line) * static_cast<std::size_t>(dw) + static_cast<std::size_t>(i)] = value;
          } else {
            dst[static_cast<std::size_t>(i) * static_cast<std::size_t>(lines) + static_cast<std::size_t>(line)] = value;
          }
        }
      }
    }
  } // namespace detail

  /** The plane at another size (see detail::Axis). */
  inline Plane Resample(const Plane& s, const int w, const int h)
  {
    if (s.w == w && s.h == h) {
      return s;
    }
    if (s.w < 1 || s.h < 1) {
      return ConstPlane(w, h, 0.0f);
    }
    std::vector<float> mid;
    detail::Axis(s.v, s.w, s.h, mid, w, true);
    Plane out;
    out.w = w;
    out.h = h;
    detail::Axis(mid, w, s.h, out.v, h, false);
    return out;
  }

  /** Applies the arithmetic of a Chan (invert, centre, scale, bias, clamp) to a plane. */
  inline void Shape(Plane& p, const Chan& c)
  {
    if (c.invert) {
      for (float& v : p.v) {
        v = 1.0f - v;
      }
    }
    float mean = 0.0f;
    if (c.center && !p.v.empty()) {
      double sum = 0.0;
      for (const float v : p.v) {
        sum += v;
      }
      mean = static_cast<float>(sum / static_cast<double>(p.v.size()));
    }
    for (float& v : p.v) {
      v = c.center ? (v - mean) * c.scale + 0.5f + c.bias : v * c.scale + c.bias;
      v = std::max(c.lo, std::min(c.hi, v));
    }
  }

  /** The plane a Chan describes for a picture of w x h pixels: Const a plane of the value, Ext the given plane (resampled), the others a channel of `src` (resampled to w x h; the picture is
   *  w x h already when `srcW` x `srcH` are). Returns false for Keep (the caller keeps its own channel). */
  inline bool MakePlane(const Chan& c, const std::vector<Px>& src, const int srcW, const int srcH, const int w, const int h, Plane& out)
  {
    switch (c.from) {
      case From::Keep:
        return false;
      case From::Const:
        out = ConstPlane(w, h, c.value);
        return true;
      case From::Ext:
        out = c.ext != nullptr ? Resample(*c.ext, w, h) : ConstPlane(w, h, c.value);
        break;
      default:
        out = Resample(PlaneOf(src, srcW, srcH, c.from), w, h);
        break;
    }
    Shape(out, c);
    return true;
  }

  /** The converted picture: the four channels r g b a each from its Chan (Keep = the source's own channel), at w x h (the source is resampled when its size differs). */
  inline Image Convert(const Image& src, const int w, const int h, const Chan* const chans)
  {
    Image out;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    static const From own[4] = {From::R, From::G, From::B, From::A};
    for (int c = 0; c < 4; ++c) {
      Plane p;
      if (!MakePlane(chans[c], src.px, src.w, src.h, w, h, p)) {
        p = Resample(PlaneOf(src.px, src.w, src.h, own[c]), w, h);
      }
      for (std::size_t i = 0; i < out.px.size(); ++i) {
        const std::uint8_t b = ToByte(p.v[i]);
        Px& d = out.px[i];
        if (c == 0) d.r = b;
        else if (c == 1) d.g = b;
        else if (c == 2) d.b = b;
        else d.a = b;
      }
    }
    return out;
  }

  // ---- the roughness / height atlas -------------------------------------------------------------------------------------------------------------

  constexpr int kAtlasTiles = 8;

  /** Where tile k (0..7: base layer, layers 1-7) lives: the quadrant (0 top left, 1 top right, 2 bottom left, 3 bottom right) and the channel pair (0: R roughness + G height, 1: B + A). */
  inline void TileSlot(const int k, int& quadrant, int& pair)
  {
    quadrant = k % 4;
    pair = k / 4;
  }

  /** Builds the atlas (size x size, size a multiple of 8) from the 8 tiles' roughness and height pictures, each (size / 4) square. A tile without pictures (empty planes) gets roughness
   *  `roughDefault` and height `heightDefault`. */
  inline Image BuildAtlas(const int size, const Plane* const rough, const Plane* const height, const float roughDefault, const float heightDefault)
  {
    Image out;
    out.w = size;
    out.h = size;
    out.px.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), Px{});
    const int T = size / 2;
    const int D = size / 4;
    for (int k = 0; k < kAtlasTiles; ++k) {
      int quadrant = 0;
      int pair = 0;
      TileSlot(k, quadrant, pair);
      const int x0 = (quadrant % 2) * T;
      const int y0 = (quadrant / 2) * T;
      const Plane* const r = (rough != nullptr && rough[k].w == D && rough[k].h == D) ? &rough[k] : nullptr;
      const Plane* const g = (height != nullptr && height[k].w == D && height[k].h == D) ? &height[k] : nullptr;
      for (int y = 0; y < T; ++y) {
        const int sy = ((y - D / 2) % D + D) % D;
        for (int x = 0; x < T; ++x) {
          const int sx = ((x - D / 2) % D + D) % D;
          const std::uint8_t rv = ToByte(r != nullptr ? r->At(sx, sy) : roughDefault);
          const std::uint8_t hv = ToByte(g != nullptr ? g->At(sx, sy) : heightDefault);
          Px& px = out.px[static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x0 + x)];
          if (pair == 0) {
            px.r = rv;
            px.g = hv;
          } else {
            px.b = rv;
            px.a = hv;
          }
        }
      }
    }
    return out;
  }

  /** The picture of channel `channel` (0 r, 1 g, 2 b, 3 a) of tile k of an atlas, as a w x h plane in the layer's own orientation (the inverse of BuildAtlas: the middle half of the tile, shifted
   *  back by half a period). */
  inline Plane AtlasTilePlane(const Image& atlas, const int k, const int channel, const int w, const int h)
  {
    const int T = atlas.w / 2;
    const int D = std::max(1, atlas.w / 4);
    int quadrant = 0;
    int pair = 0;
    TileSlot(k, quadrant, pair);
    const int x0 = (quadrant % 2) * T;
    const int y0 = (quadrant / 2) * T;
    Plane d;
    d.w = D;
    d.h = D;
    d.v.resize(static_cast<std::size_t>(D) * static_cast<std::size_t>(D));
    for (int y = 0; y < D; ++y) {
      for (int x = 0; x < D; ++x) {
        const Px& px = atlas.px[static_cast<std::size_t>(y0 + D / 2 + y) * static_cast<std::size_t>(atlas.w) + static_cast<std::size_t>(x0 + D / 2 + x)];
        const int c = channel;
        d.v[static_cast<std::size_t>(y) * static_cast<std::size_t>(D) + static_cast<std::size_t>(x)] = (c == 0 ? px.r : c == 1 ? px.g : c == 2 ? px.b : px.a) / 255.0f;
      }
    }
    return Resample(d, w, h);
  }

  /** The 256 entry table of an alpha-only change (the alpha of every texel is a function of its old alpha: keep, constant, invert, scale and bias), or false when the Chan reads something else
   *  (another channel, the colour, an external plane, the mean). */
  inline bool AlphaTable(const Chan& c, std::uint8_t* const lut)
  {
    if (c.from == From::Keep || c.from == From::A || c.from == From::Const) {
      if (c.center) {
        return false;
      }
      for (int i = 0; i < 256; ++i) {
        float v = c.from == From::Const ? c.value : i / 255.0f;
        if (c.invert) {
          v = 1.0f - v;
        }
        v = v * c.scale + c.bias;
        lut[i] = ToByte(std::max(c.lo, std::min(c.hi, v)));
      }
      return true;
    }
    return false;
  }
} // namespace moho::editor::texconv
