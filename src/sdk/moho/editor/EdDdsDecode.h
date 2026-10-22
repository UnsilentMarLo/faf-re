// EDITOR: DDS decoding for the editor natives (EdIcon.cpp: ed_dds_read, ed_dds_thumb), header-only and free of engine types so it can be tested on its own
// (moho/editor/tests/EdDdsDecodeTest.cpp).
//
// A DDS file is the header (128 bytes), then the base level, then the mip levels (each half the size of the one before, down to 1 x 1) when the mip count is above 1. The decoder reads
// DXT1 / DXT3 / DXT5 and 32 bit BGRA, and can decode any level instead of the base level: a thumbnail of a 2048 x 2048 texture needs the 256 x 256 level, not 4 million texels.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace moho::editor::dds
{
  struct Px
  {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
  };

  enum class Format { None, Dxt1, Dxt3, Dxt5, Bgra32 };

  struct Header
  {
    int w = 0;
    int h = 0;
    int mips = 0;                // the dwMipMapCount field as stored (0 or 1: no mip levels)
    std::string fourcc;          // the four characters at offset 84 ("DXT5", or whatever bytes a 32 bit file has there)
    Format format = Format::None;
  };

  inline std::uint32_t U32(const std::string& data, const std::size_t at)
  {
    std::uint32_t v = 0;
    std::memcpy(&v, data.data() + at, 4);
    return v;
  }

  /** Reads the header. False for a file that is not a DDS, has a side outside 1..4096 or a pixel format this decoder does not read (`out.fourcc` / `out.mips` are filled as far as they were read). */
  inline bool ParseHeader(const std::string& data, Header& out)
  {
    if (data.size() < 128 || std::memcmp(data.data(), "DDS ", 4) != 0) {
      return false;
    }
    out.h = static_cast<int>(U32(data, 12));
    out.w = static_cast<int>(U32(data, 16));
    out.mips = static_cast<int>(U32(data, 28));
    const std::uint32_t pfFlags = U32(data, 80);
    out.fourcc.assign(data.data() + 84, 4);
    const std::uint32_t bits = U32(data, 88);
    out.format = Format::None;
    if (out.w < 1 || out.h < 1 || out.w > 4096 || out.h > 4096) {
      return false;
    }
    if ((pfFlags & 0x4) != 0) {
      if (out.fourcc == "DXT1") {
        out.format = Format::Dxt1;
      } else if (out.fourcc == "DXT3") {
        out.format = Format::Dxt3;
      } else if (out.fourcc == "DXT5") {
        out.format = Format::Dxt5;
      }
    }
    if (out.format == Format::None && bits == 32) {
      out.format = Format::Bgra32;
    }
    return out.format != Format::None;
  }

  /** The number of levels the file holds (1 when it has no mip chain). */
  inline int Levels(const Header& h)
  {
    return h.mips > 1 ? h.mips : 1;
  }

  /** The size of a level: each side halves (never below 1). */
  inline void LevelSize(const Header& h, const int level, int& w, int& hh)
  {
    w = std::max(1, h.w >> level);
    hh = std::max(1, h.h >> level);
  }

  /** The bytes a level of w x hh texels takes. */
  inline std::size_t LevelBytes(const Format f, const int w, const int hh)
  {
    if (f == Format::Bgra32) {
      return static_cast<std::size_t>(w) * static_cast<std::size_t>(hh) * 4;
    }
    const std::size_t block = (f == Format::Dxt1) ? 8 : 16;
    return static_cast<std::size_t>(std::max(1, (w + 3) / 4)) * static_cast<std::size_t>(std::max(1, (hh + 3) / 4)) * block;
  }

  /** The offset of a level in the file (the base level starts at 128). */
  inline std::size_t LevelOffset(const Header& h, const int level)
  {
    std::size_t off = 128;
    for (int i = 0; i < level; ++i) {
      int w = 0;
      int hh = 0;
      LevelSize(h, i, w, hh);
      off += LevelBytes(h.format, w, hh);
    }
    return off;
  }

  /** The smallest level whose shorter side is still at least `minSide` texels (0 when the file has no mips or no level is that big): a picture reduced to `minSide / 2` from it keeps its detail. */
  inline int PickLevel(const Header& h, const int minSide)
  {
    int best = 0;
    const int levels = Levels(h);
    for (int level = 1; level < levels; ++level) {
      int w = 0;
      int hh = 0;
      LevelSize(h, level, w, hh);
      if (std::min(w, hh) < minSide) {
        break;
      }
      best = level;
    }
    return best;
  }

  /** Decodes one level into `out` (w x hh texels, row by row from the top). False when the file is too short for it or the level does not exist. */
  inline bool DecodeLevel(const std::string& data, const Header& hd, const int level, std::vector<Px>& out, int& w, int& hh)
  {
    if (level < 0 || level >= Levels(hd) || hd.format == Format::None) {
      return false;
    }
    LevelSize(hd, level, w, hh);
    const std::size_t off0 = LevelOffset(hd, level);
    const std::size_t bytes = LevelBytes(hd.format, w, hh);
    if (data.size() < off0 + bytes) {
      return false;
    }
    out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(hh), Px{});
    const auto* const base = reinterpret_cast<const std::uint8_t*>(data.data());
    if (hd.format == Format::Bgra32) {
      const std::uint8_t* const p = base + off0;
      for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = Px{p[i * 4 + 2], p[i * 4 + 1], p[i * 4 + 0], p[i * 4 + 3]};
      }
      return true;
    }
    const bool dxt1 = hd.format == Format::Dxt1;
    const bool dxt3 = hd.format == Format::Dxt3;
    const std::size_t blockBytes = dxt1 ? 8 : 16;
    const int bw = std::max(1, (w + 3) / 4);
    const int bh = std::max(1, (hh + 3) / 4);
    std::size_t off = off0;
    for (int by = 0; by < bh; ++by) {
      for (int bx = 0; bx < bw; ++bx) {
        const std::uint8_t* blk = base + off;
        off += blockBytes;
        int alpha[16];
        if (hd.format == Format::Dxt5) {
          const int a0 = blk[0];
          const int a1 = blk[1];
          std::uint64_t ab = 0;
          for (int i = 0; i < 6; ++i) {
            ab |= static_cast<std::uint64_t>(blk[2 + i]) << (8 * i);
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
            alpha[i] = pal[(ab >> (3 * i)) & 7];
          }
          blk += 8;
        } else if (dxt3) {
          for (int i = 0; i < 16; ++i) {
            alpha[i] = ((blk[i / 2] >> ((i & 1) * 4)) & 15) * 17;
          }
          blk += 8;
        } else {
          for (int& a : alpha) {
            a = 255;
          }
        }
        std::uint16_t c0 = 0;
        std::uint16_t c1 = 0;
        std::uint32_t idx = 0;
        std::memcpy(&c0, blk, 2);
        std::memcpy(&c1, blk + 2, 2);
        std::memcpy(&idx, blk + 4, 4);
        int col[4][3];
        const auto unpack = [](const std::uint16_t c, int* const rgb) {
          rgb[0] = ((c >> 11) & 31) * 255 / 31;
          rgb[1] = ((c >> 5) & 63) * 255 / 63;
          rgb[2] = (c & 31) * 255 / 31;
        };
        unpack(c0, col[0]);
        unpack(c1, col[1]);
        const bool four = c0 > c1 || !dxt1;
        for (int k = 0; k < 3; ++k) {
          if (four) {
            col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
            col[3][k] = (col[0][k] + 2 * col[1][k]) / 3;
          } else {
            col[2][k] = (col[0][k] + col[1][k]) / 2;
            col[3][k] = 0;
          }
        }
        for (int i = 0; i < 16; ++i) {
          const int x = bx * 4 + (i & 3);
          const int y = by * 4 + (i >> 2);
          if (x < w && y < hh) {
            const int k = static_cast<int>((idx >> (2 * i)) & 3);
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
              Px{static_cast<std::uint8_t>(col[k][0]), static_cast<std::uint8_t>(col[k][1]), static_cast<std::uint8_t>(col[k][2]), static_cast<std::uint8_t>(alpha[i])};
          }
        }
      }
    }
    return true;
  }
} // namespace moho::editor::dds
