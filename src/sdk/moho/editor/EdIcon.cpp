// EDITOR: natives of the unit icon generator (docs/projects/MOD_UNIT_EDITOR_DESIGN.md, stage S5).
//
//   ed_icon_request(x, y, z, heading, pitch, zoom, fovDeg, size) -> true | nil, message
//                                  asks the next frame for an offscreen render of the world around (x, y, z): the viewport camera is not touched. The pass renders
//                                  the scene twice (cleared to black and to white, terrain / sky / water / effects / UI off) and keeps both pictures
//   ed_icon_status()               -> "idle" | "pending" | "done" | "error", message
//   ed_icon_result(outSize[, cx, cy, side]) -> pixels, w, h | nil, message     the two pictures matted into one RGBA picture (alpha from the black / white difference),
//                                  the square (cx, cy, side in render pixels; default all) reduced to outSize x outSize by averaging; `pixels` is a flat table r, g, b, a, r, g, b, a ... of 0..255 (not premultiplied)
//   ed_icon_bounds(threshold[, keepFraction[, fx, fy, minProj]]) -> minx, miny, maxx, maxy, count, size   box of the pixels with matted alpha above threshold (render pixels); separate areas smaller than
//                                  keepFraction (default 0.1) of the largest one are ignored here and in ed_icon_result (its 5th argument); with a screen direction (fx, fy) only the pixels in front of
//                                  the middle of the body along it count (minProj: -1 all .. 0 the front half .. 1 nothing); ed_icon_result reduces with a Lanczos 3 filter
//   ed_dds_write(path, w, h, pixels, mips) -> bytes | nil, message    DXT5 DDS (the layout of the shipped unit icons: no mip maps unless mips > 1)
//   ed_dds_read(path)              -> pixels, w, h, mips, fourcc, bytes | nil, message    DXT1 / DXT3 / DXT5 / 32 bit DDS, mip 0; `path` is a disk path or a VFS
//                                  path resolved by ed_vfs_to_disk (entries inside .scd archives work)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/editor/EdIcon.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace moho::edicon
{
  bool gActive = false;
  std::uint32_t gClearArgb = 0;
}

namespace
{
  using moho::edicon::Request;

  enum class State { Idle, Pending, Done, Error };

  std::mutex gLock;
  State gState = State::Idle;
  Request gRequest;
  std::string gMessage;

  struct Rgba
  {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
  };

  int Fail(lua_State* const L, const char* const message)
  {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
  }

  float NumArg(lua_State* const L, const int index, const float fallback)
  {
    return lua_type(L, index) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, index)) : fallback;
  }

  // ---- BMP reader (what SaveTexture writes) ---------------------------------------------------------------------------------------------

  struct Bitmap
  {
    int w = 0, h = 0;
    std::vector<std::uint8_t> bgra;       // top-down, 4 bytes per pixel
  };

  bool ReadBmp(const std::string& path, Bitmap& out)
  {
    std::string data;
    if (!moho::edicon::ReadAllBytes(path, data) || data.size() < 54 || data[0] != 'B' || data[1] != 'M') {
      return false;
    }
    const auto u32 = [&](const std::size_t at) {
      std::uint32_t v = 0;
      std::memcpy(&v, data.data() + at, 4);
      return v;
    };
    const std::uint32_t offset = u32(10);
    const std::int32_t w = static_cast<std::int32_t>(u32(18));
    const std::int32_t hSigned = static_cast<std::int32_t>(u32(22));
    std::uint16_t bpp = 0;
    std::memcpy(&bpp, data.data() + 28, 2);
    const bool topDown = hSigned < 0;
    const int h = hSigned < 0 ? -hSigned : hSigned;
    if (w < 1 || h < 1 || (bpp != 24 && bpp != 32)) {
      return false;
    }
    const std::size_t stride = (static_cast<std::size_t>(w) * (bpp / 8) + 3) & ~static_cast<std::size_t>(3);
    if (offset + stride * static_cast<std::size_t>(h) > data.size()) {
      return false;
    }
    out.w = w;
    out.h = h;
    out.bgra.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
    for (int y = 0; y < h; ++y) {
      const int srcRow = topDown ? y : (h - 1 - y);
      const std::uint8_t* const src = reinterpret_cast<const std::uint8_t*>(data.data()) + offset + stride * static_cast<std::size_t>(srcRow);
      std::uint8_t* const dst = out.bgra.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
      for (int x = 0; x < w; ++x) {
        dst[x * 4 + 0] = src[x * (bpp / 8) + 0];
        dst[x * 4 + 1] = src[x * (bpp / 8) + 1];
        dst[x * 4 + 2] = src[x * (bpp / 8) + 2];
      }
    }
    return true;
  }

  // The matted alpha of the size x size render (white - black difference), and the pixels that belong to the main body: connected areas (8 neighbours, alpha above `threshold`) smaller than
  // `keepFraction` of the largest one are dropped (a stray spark or a drone in the corner of the picture must not move the frame of the icon).
  bool MatteOf(const Bitmap& onBlack, const Bitmap& onWhite, const int size, const float threshold, const float keepFraction, std::vector<float>& alpha, std::vector<std::uint8_t>& keep)
  {
    alpha.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0.0f);
    keep.assign(alpha.size(), 1);
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        const std::size_t sb = (static_cast<std::size_t>(y) * static_cast<std::size_t>(onBlack.w) + static_cast<std::size_t>(x)) * 4;
        float diff = 0.0f;
        for (int c = 0; c < 3; ++c) {
          diff += static_cast<float>(onWhite.bgra[sb + c]) - static_cast<float>(onBlack.bgra[sb + c]);
        }
        alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] = std::max(0.0f, std::min(1.0f, 1.0f - diff / (3.0f * 255.0f)));
      }
    }
    if (keepFraction <= 0.0f) {
      return true;
    }
    std::vector<int> label(alpha.size(), 0);
    std::vector<int> area(1, 0);
    std::vector<int> stack;
    int largest = 0;
    for (int y0 = 0; y0 < size; ++y0) {
      for (int x0 = 0; x0 < size; ++x0) {
        const std::size_t i0 = static_cast<std::size_t>(y0) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x0);
        if (alpha[i0] <= threshold || label[i0] != 0) {
          continue;
        }
        const int id = static_cast<int>(area.size());
        area.push_back(0);
        stack.clear();
        stack.push_back(static_cast<int>(i0));
        label[i0] = id;
        while (!stack.empty()) {
          const int cur = stack.back();
          stack.pop_back();
          ++area[static_cast<std::size_t>(id)];
          const int cx = cur % size;
          const int cy = cur / size;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              const int nx = cx + dx;
              const int ny = cy + dy;
              if (nx < 0 || ny < 0 || nx >= size || ny >= size) {
                continue;
              }
              const std::size_t ni = static_cast<std::size_t>(ny) * static_cast<std::size_t>(size) + static_cast<std::size_t>(nx);
              if (alpha[ni] > threshold && label[ni] == 0) {
                label[ni] = id;
                stack.push_back(static_cast<int>(ni));
              }
            }
          }
        }
        largest = std::max(largest, area[static_cast<std::size_t>(id)]);
      }
    }
    const int minArea = static_cast<int>(keepFraction * static_cast<float>(largest));
    for (std::size_t i = 0; i < alpha.size(); ++i) {
      if (label[i] != 0 && area[static_cast<std::size_t>(label[i])] < minArea) {
        keep[i] = 0;
      }
    }
    return true;
  }

  // ---- DXT ----------------------------------------------------------------------------------------------------------------------------------

  std::uint16_t Pack565(const int r, const int g, const int b)
  {
    return static_cast<std::uint16_t>(((r * 31 + 127) / 255 << 11) | ((g * 63 + 127) / 255 << 5) | ((b * 31 + 127) / 255));
  }

  void Unpack565(const std::uint16_t c, int& r, int& g, int& b)
  {
    r = ((c >> 11) & 31) * 255 / 31;
    g = ((c >> 5) & 63) * 255 / 63;
    b = (c & 31) * 255 / 31;
  }

  // One DXT5 block (16 bytes) from 16 pixels (row major, 4 x 4).
  void EncodeBlockDxt5(const Rgba* const px, std::uint8_t* const out)
  {
    // alpha: the extremes, 8 value mode (a0 > a1)
    int amin = 255;
    int amax = 0;
    for (int i = 0; i < 16; ++i) {
      amin = std::min<int>(amin, px[i].a);
      amax = std::max<int>(amax, px[i].a);
    }
    int pal[8];
    pal[0] = amax;
    pal[1] = amin;
    for (int i = 1; i < 7; ++i) {
      pal[1 + i] = ((7 - i) * amax + i * amin) / 7;
    }
    std::uint64_t abits = 0;
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      int bestErr = 1 << 30;
      for (int k = 0; k < 8; ++k) {
        const int e = std::abs(px[i].a - pal[k]);
        if (e < bestErr) {
          bestErr = e;
          best = k;
        }
      }
      if (amax == amin) {
        best = 0;
      }
      abits |= static_cast<std::uint64_t>(best) << (3 * i);
    }
    out[0] = static_cast<std::uint8_t>(amax);
    out[1] = static_cast<std::uint8_t>(amin);
    for (int i = 0; i < 6; ++i) {
      out[2 + i] = static_cast<std::uint8_t>((abits >> (8 * i)) & 0xFF);
    }

    // colour: endpoints along the principal axis of the pixels that carry alpha (nearly transparent pixels do not pull the fit)
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

  struct Image
  {
    int w = 0, h = 0;
    std::vector<Rgba> px;
  };

  std::string EncodeDds(const Image& base, const int mips)
  {
    std::vector<Image> levels;
    levels.push_back(base);
    while (static_cast<int>(levels.size()) < mips && (levels.back().w > 1 || levels.back().h > 1)) {
      const Image& s = levels.back();
      Image d;
      d.w = std::max(1, s.w / 2);
      d.h = std::max(1, s.h / 2);
      d.px.resize(static_cast<std::size_t>(d.w) * static_cast<std::size_t>(d.h));
      for (int y = 0; y < d.h; ++y) {
        for (int x = 0; x < d.w; ++x) {
          int sum[4] = {0, 0, 0, 0};
          int n = 0;
          for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
              const int sx = std::min(s.w - 1, x * 2 + dx);
              const int sy = std::min(s.h - 1, y * 2 + dy);
              const Rgba& p = s.px[static_cast<std::size_t>(sy) * static_cast<std::size_t>(s.w) + static_cast<std::size_t>(sx)];
              sum[0] += p.r;
              sum[1] += p.g;
              sum[2] += p.b;
              sum[3] += p.a;
              ++n;
            }
          }
          d.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(d.w) + static_cast<std::size_t>(x)] =
            Rgba{static_cast<std::uint8_t>(sum[0] / n), static_cast<std::uint8_t>(sum[1] / n), static_cast<std::uint8_t>(sum[2] / n), static_cast<std::uint8_t>(sum[3] / n)};
        }
      }
      levels.push_back(std::move(d));
    }
    const int count = static_cast<int>(levels.size());

    std::string out;
    out.resize(128, '\0');
    const auto put = [&](const std::size_t at, const std::uint32_t v) { std::memcpy(&out[at], &v, 4); };
    std::memcpy(&out[0], "DDS ", 4);
    put(4, 124);
    // DDSD_CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE, plus MIPMAPCOUNT when there are mips: the flags of the shipped icons
    put(8, 0x00081007u | (count > 1 ? 0x00020000u : 0u));
    put(12, static_cast<std::uint32_t>(base.h));
    put(16, static_cast<std::uint32_t>(base.w));
    put(20, static_cast<std::uint32_t>(std::max(1, (base.w + 3) / 4) * std::max(1, (base.h + 3) / 4) * 16));
    put(28, count > 1 ? static_cast<std::uint32_t>(count) : 0u);
    put(76, 32);                          // DDS_PIXELFORMAT.dwSize
    put(80, 0x4);                         // DDPF_FOURCC
    std::memcpy(&out[84], "DXT5", 4);
    put(108, count > 1 ? 0x00401008u : 0x00001000u);

    for (const Image& level : levels) {
      const int bw = std::max(1, (level.w + 3) / 4);
      const int bh = std::max(1, (level.h + 3) / 4);
      for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
          Rgba block[16];
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

  int Rgb565(const std::uint16_t c, int& r, int& g, int& b)
  {
    Unpack565(c, r, g, b);
    return 0;
  }

  bool DecodeDds(const std::string& data, Image& out, int& mips, std::string& fourcc)
  {
    if (data.size() < 128 || std::memcmp(data.data(), "DDS ", 4) != 0) {
      return false;
    }
    const auto u32 = [&](const std::size_t at) {
      std::uint32_t v = 0;
      std::memcpy(&v, data.data() + at, 4);
      return v;
    };
    const int h = static_cast<int>(u32(12));
    const int w = static_cast<int>(u32(16));
    mips = static_cast<int>(u32(28));
    const std::uint32_t pfFlags = u32(80);
    fourcc.assign(data.data() + 84, 4);
    const std::uint32_t bits = u32(88);
    if (w < 1 || h < 1 || w > 4096 || h > 4096) {
      return false;
    }
    out.w = w;
    out.h = h;
    out.px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), Rgba{});
    std::size_t off = 128;
    if ((pfFlags & 0x4) != 0 && (fourcc == "DXT1" || fourcc == "DXT3" || fourcc == "DXT5")) {
      const bool dxt1 = fourcc == "DXT1";
      const std::size_t blockBytes = dxt1 ? 8 : 16;
      const int bw = (w + 3) / 4;
      const int bh = (h + 3) / 4;
      if (data.size() < off + blockBytes * static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh)) {
        return false;
      }
      const auto* const bytes = reinterpret_cast<const std::uint8_t*>(data.data());
      for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
          const std::uint8_t* blk = bytes + off;
          off += blockBytes;
          int alpha[16];
          if (fourcc == "DXT5") {
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
          } else if (fourcc == "DXT3") {
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
          Rgb565(c0, col[0][0], col[0][1], col[0][2]);
          Rgb565(c1, col[1][0], col[1][1], col[1][2]);
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
            if (x < w && y < h) {
              const int k = static_cast<int>((idx >> (2 * i)) & 3);
              out.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                Rgba{static_cast<std::uint8_t>(col[k][0]), static_cast<std::uint8_t>(col[k][1]), static_cast<std::uint8_t>(col[k][2]), static_cast<std::uint8_t>(alpha[i])};
            }
          }
        }
      }
      return true;
    }
    if (bits == 32 && data.size() >= off + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4) {
      const auto* const bytes = reinterpret_cast<const std::uint8_t*>(data.data()) + off;
      for (std::size_t i = 0; i < out.px.size(); ++i) {
        out.px[i] = Rgba{bytes[i * 4 + 2], bytes[i * 4 + 1], bytes[i * 4 + 0], bytes[i * 4 + 3]};
      }
      return true;
    }
    return false;
  }

  void PushPixels(lua_State* const L, const Image& img)
  {
    lua_newtable(L);
    std::size_t n = 1;
    for (const Rgba& p : img.px) {
      for (const int v : {static_cast<int>(p.r), static_cast<int>(p.g), static_cast<int>(p.b), static_cast<int>(p.a)}) {
        lua_pushnumber(L, static_cast<lua_Number>(n++));
        lua_pushnumber(L, static_cast<lua_Number>(v));
        lua_settable(L, -3);
      }
    }
  }

  void CreateParents(const std::string& path)
  {
    for (std::size_t i = 0; i < path.size(); ++i) {
      if ((path[i] == '/' || path[i] == '\\') && i > 2) {
        CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
      }
    }
  }

  // ---- Lua natives ---------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_EdIconRequest(lua_State* const L)
  {
    Request r;
    r.x = NumArg(L, 1, 0.0f);
    r.y = NumArg(L, 2, 0.0f);
    r.z = NumArg(L, 3, 0.0f);
    r.heading = NumArg(L, 4, 0.0f);
    r.pitch = NumArg(L, 5, 0.5f);
    r.zoom = NumArg(L, 6, 10.0f);
    r.fovDeg = NumArg(L, 7, 30.0f);
    r.size = static_cast<int>(NumArg(L, 8, 256.0f));
    r.keepMask = static_cast<std::uint32_t>(NumArg(L, 9, 0.0f));
    if (r.size < 16 || r.size > 1024 || r.zoom <= 0.0f || r.fovDeg < 5.0f || r.fovDeg > 120.0f) {
      return Fail(L, "bad arguments");
    }
    std::lock_guard<std::mutex> guard(gLock);
    if (gState == State::Pending) {
      return Fail(L, "a render is still pending");
    }
    gRequest = r;
    gState = State::Pending;
    gMessage.clear();
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_EdIconStatus(lua_State* const L)
  {
    std::lock_guard<std::mutex> guard(gLock);
    const char* const names[] = {"idle", "pending", "done", "error"};
    lua_pushstring(L, names[static_cast<int>(gState)]);
    lua_pushstring(L, gMessage.c_str());
    return 2;
  }

  int __cdecl cfunc_EdIconResult(lua_State* const L)
  {
    const int outSize = static_cast<int>(NumArg(L, 1, 64.0f));
    {
      std::lock_guard<std::mutex> guard(gLock);
      if (gState != State::Done) {
        return Fail(L, "no finished render");
      }
    }
    Bitmap onBlack;
    Bitmap onWhite;
    if (!ReadBmp(moho::edicon::TempPath(0), onBlack) || !ReadBmp(moho::edicon::TempPath(1), onWhite) || onBlack.w != onWhite.w || onBlack.h != onWhite.h) {
      return Fail(L, "cannot read the captures");
    }
    int size = 0;
    {
      std::lock_guard<std::mutex> guard(gLock);
      size = std::min(gRequest.size, std::min(onBlack.w, onBlack.h));
    }
    // the square of the render that becomes the icon: centre (cx, cy) and side in render pixels (default: the whole render)
    const float cx = NumArg(L, 2, size * 0.5f);
    const float cy = NumArg(L, 3, size * 0.5f);
    const float side = NumArg(L, 4, static_cast<float>(size));
    if (outSize < 1 || outSize > 1024 || side < 4.0f) {
      return Fail(L, "bad output size");
    }
    const float keepFraction = NumArg(L, 5, 0.1f);
    std::vector<float> matte;
    std::vector<std::uint8_t> keep;
    MatteOf(onBlack, onWhite, size, 0.05f, keepFraction, matte, keep);
    // matting: on black c * a, on white c * a + (1 - a) * 255, so a = 1 - (white - black) / 255 and the black picture is the premultiplied colour.
    // Reduction: separable Lanczos 3 on the premultiplied picture (sharper than a box average, and every output pixel is normalised by its own weights, so no moire from a
    // non-integer ratio); render pixels outside the picture or outside the main body count as transparent.
    const float left = cx - side * 0.5f;
    const float top = cy - side * 0.5f;
    const float step = side / static_cast<float>(outSize);
    const float scale = std::max(1.0f, step);                // filter width in source pixels per tap unit
    const int support = static_cast<int>(std::ceil(3.0f * scale));
    const auto lanczos = [](const float x) {
      const float ax = std::fabs(x);
      if (ax >= 3.0f) {
        return 0.0f;
      }
      if (ax < 1e-5f) {
        return 1.0f;
      }
      const float px = 3.14159265f * ax;
      return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
    };
    const auto sample = [&](const int sx, const int sy, float* const out) {       // premultiplied r, g, b, a (0..255 / 0..1) of one render pixel
      out[0] = out[1] = out[2] = out[3] = 0.0f;
      if (sx < 0 || sy < 0 || sx >= size || sy >= size) {
        return;
      }
      const std::size_t mi = static_cast<std::size_t>(sy) * static_cast<std::size_t>(size) + static_cast<std::size_t>(sx);
      if (keep[mi] == 0) {
        return;
      }
      const std::size_t sb = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(onBlack.w) + static_cast<std::size_t>(sx)) * 4;
      out[0] = onBlack.bgra[sb + 2];
      out[1] = onBlack.bgra[sb + 1];
      out[2] = onBlack.bgra[sb + 0];
      out[3] = matte[mi] * 255.0f;
    };
    // horizontal pass over the rows the vertical pass will need
    const int rowFirst = static_cast<int>(std::floor(top)) - support - 1;
    const int rowLast = static_cast<int>(std::ceil(top + side)) + support + 1;
    const int rowCount = rowLast - rowFirst + 1;
    std::vector<float> horiz(static_cast<std::size_t>(rowCount) * static_cast<std::size_t>(outSize) * 4, 0.0f);
    for (int ox = 0; ox < outSize; ++ox) {
      const float centre = left + (static_cast<float>(ox) + 0.5f) * step;
      const int first = static_cast<int>(std::floor(centre - static_cast<float>(support)));
      const int last = static_cast<int>(std::ceil(centre + static_cast<float>(support)));
      std::vector<float> weights;
      float wsum = 0.0f;
      for (int sx = first; sx <= last; ++sx) {
        const float w = lanczos((static_cast<float>(sx) + 0.5f - centre) / scale);
        weights.push_back(w);
        wsum += w;
      }
      if (wsum < 1e-6f) {
        continue;
      }
      for (int r = 0; r < rowCount; ++r) {
        const int sy = rowFirst + r;
        if (sy < 0 || sy >= size) {
          continue;
        }
        float acc[4] = {0, 0, 0, 0};
        float px[4];
        for (int sx = first; sx <= last; ++sx) {
          const float w = weights[static_cast<std::size_t>(sx - first)];
          if (w == 0.0f) {
            continue;
          }
          sample(sx, sy, px);
          for (int c = 0; c < 4; ++c) {
            acc[c] += w * px[c];
          }
        }
        float* const dst = &horiz[(static_cast<std::size_t>(r) * static_cast<std::size_t>(outSize) + static_cast<std::size_t>(ox)) * 4];
        for (int c = 0; c < 4; ++c) {
          dst[c] = acc[c] / wsum;
        }
      }
    }
    Image img;
    img.w = outSize;
    img.h = outSize;
    img.px.resize(static_cast<std::size_t>(outSize) * static_cast<std::size_t>(outSize));
    for (int oy = 0; oy < outSize; ++oy) {
      const float centre = top + (static_cast<float>(oy) + 0.5f) * step;
      const int first = static_cast<int>(std::floor(centre - static_cast<float>(support)));
      const int last = static_cast<int>(std::ceil(centre + static_cast<float>(support)));
      std::vector<float> weights;
      float wsum = 0.0f;
      for (int sy = first; sy <= last; ++sy) {
        const float w = lanczos((static_cast<float>(sy) + 0.5f - centre) / scale);
        weights.push_back(w);
        wsum += w;
      }
      if (wsum < 1e-6f) {
        continue;
      }
      for (int ox = 0; ox < outSize; ++ox) {
        float acc[4] = {0, 0, 0, 0};
        for (int sy = first; sy <= last; ++sy) {
          const int r = sy - rowFirst;
          if (r < 0 || r >= rowCount) {
            continue;
          }
          const float w = weights[static_cast<std::size_t>(sy - first)];
          const float* const src = &horiz[(static_cast<std::size_t>(r) * static_cast<std::size_t>(outSize) + static_cast<std::size_t>(ox)) * 4];
          for (int c = 0; c < 4; ++c) {
            acc[c] += w * src[c];
          }
        }
        const float a = std::max(0.0f, std::min(255.0f, acc[3] / wsum));
        Rgba& p = img.px[static_cast<std::size_t>(oy) * static_cast<std::size_t>(outSize) + static_cast<std::size_t>(ox)];
        p.a = static_cast<std::uint8_t>(a + 0.5f);
        if (a > 1.0f) {
          for (int c = 0; c < 3; ++c) {
            const float v = (acc[c] / wsum) * 255.0f / a;       // un-premultiply (the colours are premultiplied 0..255, the alpha 0..255)
            (&p.r)[c] = static_cast<std::uint8_t>(std::max(0.0f, std::min(255.0f, v + 0.5f)));
          }
        }
      }
    }
    PushPixels(L, img);
    lua_pushnumber(L, outSize);
    lua_pushnumber(L, outSize);
    return 3;
  }

  // ed_icon_bounds(threshold) -> minx, miny, maxx, maxy, count, size: the box of the pixels whose matted alpha exceeds threshold (0..1, default 0.05), in render pixels
  int __cdecl cfunc_EdIconBounds(lua_State* const L)
  {
    const float threshold = NumArg(L, 1, 0.05f);
    {
      std::lock_guard<std::mutex> guard(gLock);
      if (gState != State::Done) {
        return Fail(L, "no finished render");
      }
    }
    Bitmap onBlack;
    Bitmap onWhite;
    if (!ReadBmp(moho::edicon::TempPath(0), onBlack) || !ReadBmp(moho::edicon::TempPath(1), onWhite) || onBlack.w != onWhite.w || onBlack.h != onWhite.h) {
      return Fail(L, "cannot read the captures");
    }
    int size = 0;
    {
      std::lock_guard<std::mutex> guard(gLock);
      size = std::min(gRequest.size, std::min(onBlack.w, onBlack.h));
    }
    const float keepFraction = NumArg(L, 2, 0.1f);
    std::vector<float> matte;
    std::vector<std::uint8_t> keep;
    MatteOf(onBlack, onWhite, size, threshold, keepFraction, matte, keep);
    // optional: only the front half. (fx, fy) is the direction on the screen the unit's front points to; pixels behind the middle of the body along it (middle + minProj * half extent) are left out
    const float fdx = NumArg(L, 3, 0.0f);
    const float fdy = NumArg(L, 4, 0.0f);
    const float minProj = NumArg(L, 5, 0.0f);
    float cut = -1e30f;
    const float flen = std::sqrt(fdx * fdx + fdy * fdy);
    if (flen > 1e-6f) {
      float pmin = 1e30f;
      float pmax = -1e30f;
      for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
          const std::size_t mi = static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
          if (matte[mi] > threshold && keep[mi] != 0) {
            const float p = (static_cast<float>(x) * fdx + static_cast<float>(y) * fdy) / flen;
            pmin = std::min(pmin, p);
            pmax = std::max(pmax, p);
          }
        }
      }
      if (pmax > pmin) {
        cut = (pmin + pmax) * 0.5f + minProj * (pmax - pmin) * 0.5f;
      }
    }
    int minx = size;
    int miny = size;
    int maxx = -1;
    int maxy = -1;
    long long count = 0;
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        const std::size_t mi = static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
        if (matte[mi] > threshold && keep[mi] != 0) {
          if (flen > 1e-6f && (static_cast<float>(x) * fdx + static_cast<float>(y) * fdy) / flen < cut) {
            continue;
          }
          minx = std::min(minx, x);
          maxx = std::max(maxx, x);
          miny = std::min(miny, y);
          maxy = std::max(maxy, y);
          ++count;
        }
      }
    }
    lua_pushnumber(L, minx);
    lua_pushnumber(L, miny);
    lua_pushnumber(L, maxx);
    lua_pushnumber(L, maxy);
    lua_pushnumber(L, static_cast<lua_Number>(count));
    lua_pushnumber(L, size);
    return 6;
  }

  int __cdecl cfunc_EdDdsWrite(lua_State* const L)
  {
    const char* const path = lua_tostring(L, 1);
    const int w = static_cast<int>(NumArg(L, 2, 0.0f));
    const int h = static_cast<int>(NumArg(L, 3, 0.0f));
    luaL_checktype(L, 4, LUA_TTABLE);
    const int mips = static_cast<int>(NumArg(L, 5, 1.0f));
    if (path == nullptr || w < 1 || h < 1 || w > 4096 || h > 4096) {
      return Fail(L, "bad arguments");
    }
    Image img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < img.px.size(); ++i) {
      int v[4];
      for (int c = 0; c < 4; ++c) {
        lua_rawgeti(L, 4, static_cast<int>(i * 4 + static_cast<std::size_t>(c) + 1));
        v[c] = static_cast<int>(std::max(0.0, std::min(255.0, lua_tonumber(L, -1) + 0.5)));
        lua_settop(L, -2);
      }
      img.px[i] = Rgba{static_cast<std::uint8_t>(v[0]), static_cast<std::uint8_t>(v[1]), static_cast<std::uint8_t>(v[2]), static_cast<std::uint8_t>(v[3])};
    }
    const std::string bytes = EncodeDds(img, mips);
    CreateParents(path);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
      return Fail(L, "cannot create the file");
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != bytes.size()) {
      return Fail(L, "write failed");
    }
    lua_pushnumber(L, static_cast<lua_Number>(bytes.size()));
    return 1;
  }

  int __cdecl cfunc_EdDdsRead(lua_State* const L)
  {
    const char* const path = lua_tostring(L, 1);
    if (path == nullptr) {
      return Fail(L, "bad arguments");
    }
    std::string data;
    if (!moho::edicon::ReadAllBytes(path, data)) {
      return Fail(L, "cannot read the file");
    }
    Image img;
    int mips = 0;
    std::string fourcc;
    if (!DecodeDds(data, img, mips, fourcc)) {
      return Fail(L, "unsupported DDS");
    }
    PushPixels(L, img);
    lua_pushnumber(L, img.w);
    lua_pushnumber(L, img.h);
    lua_pushnumber(L, mips);
    lua_pushstring(L, fourcc.c_str());
    lua_pushnumber(L, static_cast<lua_Number>(data.size()));
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

  struct EdIconLuaBootstrap
  {
    EdIconLuaBootstrap()
    {
      auto& set = UserLuaInitSet();
      (void)new moho::CScrLuaBinder(set, "ed_icon_request", &cfunc_EdIconRequest, nullptr, "<global>", "ed_icon_request(x, y, z, heading, pitch, zoom, fovDeg, size) -> true | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_icon_status", &cfunc_EdIconStatus, nullptr, "<global>", "ed_icon_status() -> state, message");
      (void)new moho::CScrLuaBinder(set, "ed_icon_result", &cfunc_EdIconResult, nullptr, "<global>", "ed_icon_result(outSize) -> pixels, w, h | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_icon_bounds", &cfunc_EdIconBounds, nullptr, "<global>", "ed_icon_bounds(threshold) -> minx, miny, maxx, maxy, count, size | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_dds_write", &cfunc_EdDdsWrite, nullptr, "<global>", "ed_dds_write(path, w, h, pixels, mips) -> bytes | nil, message");
      (void)new moho::CScrLuaBinder(set, "ed_dds_read", &cfunc_EdDdsRead, nullptr, "<global>", "ed_dds_read(path) -> pixels, w, h, mips, fourcc, bytes | nil, message");
    }
  };

  const EdIconLuaBootstrap gEdIconLuaBootstrap{};
} // namespace

namespace moho::edicon
{
  bool TakeRequest(Request& out)
  {
    std::lock_guard<std::mutex> guard(gLock);
    if (gState != State::Pending) {
      return false;
    }
    out = gRequest;
    return true;
  }

  void Complete(const bool ok, const char* const message)
  {
    std::lock_guard<std::mutex> guard(gLock);
    gState = ok ? State::Done : State::Error;
    gMessage = message != nullptr ? message : "";
  }

  std::string TempPath(const int pass)
  {
    char dir[MAX_PATH] = {};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    std::string path(dir, n);
    path += pass == 0 ? "ed_icon_black.bmp" : "ed_icon_white.bmp";
    return path;
  }
} // namespace moho::edicon
