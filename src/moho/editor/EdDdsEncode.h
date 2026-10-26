// EDITOR: DXT5 DDS encoding for the editor natives that write textures (EdTexConv.cpp: ed_tex_convert, ed_tex_atlas), header-only and free of engine types so it can be tested on its own
// (moho/editor/tests/EdTexConvTest.cpp). The decoder is EdDdsDecode.h; EdIcon.cpp (ed_dds_write) still has its own older copy of this encoder.
//
//   EncodeDxt5(image, levels)   the DDS file bytes of an image as DXT5 with `levels` mip levels (1 = none; a number above the possible count gives the whole chain down to 1 x 1)
//   Dxt5MapAlpha(data, lut)     rewrites only the alpha blocks of a DXT5 file (every level) through a 256 entry table: the colour bytes stay exactly as they are
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "moho/editor/EdDdsDecode.h"

namespace moho::editor::dds
{
  struct Image
  {
    int w = 0;
    int h = 0;
    std::vector<Px> px;
  };

  inline std::uint16_t Pack565(const int r, const int g, const int b)
  {
    return static_cast<std::uint16_t>(((r * 31 + 127) / 255 << 11) | ((g * 63 + 127) / 255 << 5) | ((b * 31 + 127) / 255));
  }

  inline void Unpack565(const std::uint16_t c, int& r, int& g, int& b)
  {
    r = ((c >> 11) & 31) * 255 / 31;
    g = ((c >> 5) & 63) * 255 / 63;
    b = (c & 31) * 255 / 31;
  }

  /** The 8 byte alpha half of a DXT5 block from 16 values (0..255): the extremes as end points, the 8 value mode (a0 > a1); a block of one value keeps a0 == a1 and index 0. */
  inline void EncodeAlphaBlock(const int* const a, std::uint8_t* const out)
  {
    int amin = 255;
    int amax = 0;
    for (int i = 0; i < 16; ++i) {
      amin = std::min(amin, a[i]);
      amax = std::max(amax, a[i]);
    }
    int pal[8];
    pal[0] = amax;
    pal[1] = amin;
    for (int i = 1; i < 7; ++i) {
      pal[1 + i] = ((7 - i) * amax + i * amin) / 7;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      int bestErr = 1 << 30;
      for (int k = 0; k < 8; ++k) {
        const int e = std::abs(a[i] - pal[k]);
        if (e < bestErr) {
          bestErr = e;
          best = k;
        }
      }
      if (amax == amin) {
        best = 0;
      }
      bits |= static_cast<std::uint64_t>(best) << (3 * i);
    }
    out[0] = static_cast<std::uint8_t>(amax);
    out[1] = static_cast<std::uint8_t>(amin);
    for (int i = 0; i < 6; ++i) {
      out[2 + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
    }
  }

  /** One DXT5 block (16 bytes) from 16 pixels (row major, 4 x 4): the alpha as above, the colour as end points along the principal axis of the pixels that carry alpha. */
  inline void EncodeBlockDxt5(const Px* const px, std::uint8_t* const out)
  {
    int alpha[16];
    for (int i = 0; i < 16; ++i) {
      alpha[i] = px[i].a;
    }
    EncodeAlphaBlock(alpha, out);

    float mean[3] = {0, 0, 0};
    float weight = 0;
    for (int i = 0; i < 16; ++i) {
      const float w = static_cast<float>(px[i].a) + 1.0f;
      mean[0] += w * px[i].r;
      mean[1] += w * px[i].g;
      mean[2] += w * px[i].b;
      weight += w;
    }
    for (float& m : mean) {
      m /= weight;
    }
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
      const float w = static_cast<float>(px[i].a) + 1.0f;
      const float d[3] = {px[i].r - mean[0], px[i].g - mean[1], px[i].b - mean[2]};
      cov[0] += w * d[0] * d[0];
      cov[1] += w * d[0] * d[1];
      cov[2] += w * d[0] * d[2];
      cov[3] += w * d[1] * d[1];
      cov[4] += w * d[1] * d[2];
      cov[5] += w * d[2] * d[2];
    }
    float axis[3] = {1.0f, 1.0f, 1.0f};
    for (int it = 0; it < 8; ++it) {
      const float nx = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
      const float ny = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
      const float nz = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
      const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (len < 1e-6f) {
        break;
      }
      axis[0] = nx / len;
      axis[1] = ny / len;
      axis[2] = nz / len;
    }
    float tmin = 1e30f;
    float tmax = -1e30f;
    for (int i = 0; i < 16; ++i) {
      const float t = (px[i].r - mean[0]) * axis[0] + (px[i].g - mean[1]) * axis[1] + (px[i].b - mean[2]) * axis[2];
      tmin = std::min(tmin, t);
      tmax = std::max(tmax, t);
    }
    const auto clamp255 = [](const float v) { return static_cast<int>(std::max(0.0f, std::min(255.0f, v + 0.5f))); };
    std::uint16_t c0 = Pack565(clamp255(mean[0] + axis[0] * tmax), clamp255(mean[1] + axis[1] * tmax), clamp255(mean[2] + axis[2] * tmax));
    std::uint16_t c1 = Pack565(clamp255(mean[0] + axis[0] * tmin), clamp255(mean[1] + axis[1] * tmin), clamp255(mean[2] + axis[2] * tmin));
    if (c0 < c1) {
      std::swap(c0, c1);
    }
    int col[4][3];
    Unpack565(c0, col[0][0], col[0][1], col[0][2]);
    Unpack565(c1, col[1][0], col[1][1], col[1][2]);
    for (int k = 0; k < 3; ++k) {
      col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
      col[3][k] = (col[0][k] + 2 * col[1][k]) / 3;
    }
    std::uint32_t cbits = 0;
    if (c0 != c1) {
      for (int i = 0; i < 16; ++i) {
        int best = 0;
        int bestErr = 1 << 30;
        for (int k = 0; k < 4; ++k) {
          const int dr = px[i].r - col[k][0];
          const int dg = px[i].g - col[k][1];
          const int db = px[i].b - col[k][2];
          const int e = dr * dr + dg * dg + db * db;
          if (e < bestErr) {
            bestErr = e;
            best = k;
          }
        }
        cbits |= static_cast<std::uint32_t>(best) << (2 * i);
      }
    }
    std::memcpy(out + 8, &c0, 2);
    std::memcpy(out + 10, &c1, 2);
    std::memcpy(out + 12, &cbits, 4);
  }

  /** The next mip level: each texel the average of 2 x 2 (the edge texel repeats for an odd side). */
  inline Image HalfSize(const Image& s)
  {
    Image d;
    d.w = std::max(1, s.w / 2);
    d.h = std::max(1, s.h / 2);
    d.px.resize(static_cast<std::size_t>(d.w) * static_cast<std::size_t>(d.h));
    for (int y = 0; y < d.h; ++y) {
      for (int x = 0; x < d.w; ++x) {
        int sum[4] = {0, 0, 0, 0};
        for (int dy = 0; dy < 2; ++dy) {
          for (int dx = 0; dx < 2; ++dx) {
            const int sx = std::min(s.w - 1, x * 2 + dx);
            const int sy = std::min(s.h - 1, y * 2 + dy);
            const Px& p = s.px[static_cast<std::size_t>(sy) * static_cast<std::size_t>(s.w) + static_cast<std::size_t>(sx)];
            sum[0] += p.r;
            sum[1] += p.g;
            sum[2] += p.b;
            sum[3] += p.a;
          }
        }
        d.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(d.w) + static_cast<std::size_t>(x)] =
          Px{static_cast<std::uint8_t>((sum[0] + 2) / 4), static_cast<std::uint8_t>((sum[1] + 2) / 4), static_cast<std::uint8_t>((sum[2] + 2) / 4), static_cast<std::uint8_t>((sum[3] + 2) / 4)};
      }
    }
    return d;
  }

  /** The bytes of a DXT5 DDS file with `levels` mip levels (at least 1; more than the chain has gives the whole chain). */
  inline std::string EncodeDxt5(const Image& base, const int levels)
  {
    std::vector<Image> chain;
    chain.push_back(base);
    while (static_cast<int>(chain.size()) < levels && (chain.back().w > 1 || chain.back().h > 1)) {
      chain.push_back(HalfSize(chain.back()));
    }
    const int count = static_cast<int>(chain.size());

    std::string out;
    out.resize(128, '\0');
    const auto put = [&](const std::size_t at, const std::uint32_t v) { std::memcpy(&out[at], &v, 4); };
    std::memcpy(&out[0], "DDS ", 4);
    put(4, 124);
    // DDSD_CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE, plus MIPMAPCOUNT when there are levels
    put(8, 0x00081007u | (count > 1 ? 0x00020000u : 0u));
    put(12, static_cast<std::uint32_t>(base.h));
    put(16, static_cast<std::uint32_t>(base.w));
    put(20, static_cast<std::uint32_t>(std::max(1, (base.w + 3) / 4) * std::max(1, (base.h + 3) / 4) * 16));
    put(28, count > 1 ? static_cast<std::uint32_t>(count) : 0u);
    put(76, 32);                          // DDS_PIXELFORMAT.dwSize
    put(80, 0x4);                         // DDPF_FOURCC
    std::memcpy(&out[84], "DXT5", 4);
    put(108, count > 1 ? 0x00401008u : 0x00001000u);

    for (const Image& level : chain) {
      const int bw = std::max(1, (level.w + 3) / 4);
      const int bh = std::max(1, (level.h + 3) / 4);
      for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
          Px block[16];
          for (int i = 0; i < 16; ++i) {
            const int x = std::min(level.w - 1, bx * 4 + (i & 3));
            const int y = std::min(level.h - 1, by * 4 + (i >> 2));
            block[i] = level.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(level.w) + static_cast<std::size_t>(x)];
          }
          std::uint8_t encoded[16];
          EncodeBlockDxt5(block, encoded);
          out.append(reinterpret_cast<const char*>(encoded), 16);
        }
      }
    }
    return out;
  }

  /** Decodes the 16 alpha values of the alpha half (8 bytes) of a DXT5 block. */
  inline void DecodeAlphaBlock(const std::uint8_t* const blk, int* const a)
  {
    const int a0 = blk[0];
    const int a1 = blk[1];
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
      bits |= static_cast<std::uint64_t>(blk[2 + i]) << (8 * i);
    }
    int pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
      for (int i = 1; i < 7; ++i) {
        pal[1 + i] = ((7 - i) * a0 + i * a1) / 7;
      }
    } else {
      for (int i = 1; i < 5; ++i) {
        pal[1 + i] = ((5 - i) * a0 + i * a1) / 5;
      }
      pal[6] = 0;
      pal[7] = 255;
    }
    for (int i = 0; i < 16; ++i) {
      a[i] = pal[(bits >> (3 * i)) & 7];
    }
  }

  /** Rewrites the alpha of a DXT5 file in place: every alpha value v of every mip level becomes lut[v]; the colour half of every block is not touched, so the colours stay bit for bit as they were.
   *  False (nothing changed) when the file is not DXT5 or is too short. */
  inline bool Dxt5MapAlpha(std::string& data, const std::uint8_t* const lut)
  {
    Header hd;
    if (!ParseHeader(data, hd) || hd.format != Format::Dxt5) {
      return false;
    }
    const int levels = Levels(hd);
    if (data.size() < LevelOffset(hd, levels - 1) + LevelBytes(hd.format, std::max(1, hd.w >> (levels - 1)), std::max(1, hd.h >> (levels - 1)))) {
      return false;
    }
    auto* const bytes = reinterpret_cast<std::uint8_t*>(&data[0]);
    for (int level = 0; level < levels; ++level) {
      int w = 0;
      int h = 0;
      LevelSize(hd, level, w, h);
      const std::size_t blocks = static_cast<std::size_t>(std::max(1, (w + 3) / 4)) * static_cast<std::size_t>(std::max(1, (h + 3) / 4));
      std::uint8_t* blk = bytes + LevelOffset(hd, level);
      for (std::size_t b = 0; b < blocks; ++b, blk += 16) {
        int a[16];
        DecodeAlphaBlock(blk, a);
        for (int& v : a) {
          v = lut[v];
        }
        EncodeAlphaBlock(a, blk);
      }
    }
    return true;
  }
} // namespace moho::editor::dds
