// Stand-alone test of the layer converter's texture maths (EdTexConv.h, EdDdsEncode.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdTexConvTest.cpp -o EdTexConvTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdTexConvTest.cpp
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "moho/editor/EdTexConv.h"

namespace tc = moho::editor::texconv;
namespace dds = moho::editor::dds;

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    ++gChecks;                                         \
    if (!(cond)) {                                     \
      ++gFailures;                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

namespace
{
  dds::Image Noise(const int w, const int h, unsigned seed)
  {
    dds::Image img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<std::size_t>(w) * h);
    for (auto& p : img.px) {
      seed = seed * 1664525u + 1013904223u;
      p.r = static_cast<std::uint8_t>(seed >> 8);
      p.g = static_cast<std::uint8_t>(seed >> 16);
      p.b = static_cast<std::uint8_t>(seed >> 24);
      p.a = static_cast<std::uint8_t>((seed >> 12) ^ (seed >> 20));
    }
    return img;
  }

  // a smooth picture: DXT5 reproduces it closely, so the tests can compare to a tolerance
  dds::Image Smooth(const int w, const int h)
  {
    dds::Image img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        dds::Px& p = img.px[static_cast<std::size_t>(y) * w + x];
        p.r = static_cast<std::uint8_t>(40 + x * 150 / w);
        p.g = static_cast<std::uint8_t>(60 + y * 120 / h);
        p.b = static_cast<std::uint8_t>(90 + (x + y) * 60 / (w + h));
        p.a = static_cast<std::uint8_t>((x * 3 + y * 5) % 256);
      }
    }
    return img;
  }

  std::vector<dds::Px> Decode(const std::string& bytes, int level, int& w, int& h)
  {
    dds::Header hd;
    std::vector<dds::Px> px;
    if (!dds::ParseHeader(bytes, hd) || !dds::DecodeLevel(bytes, hd, level, px, w, h)) {
      w = h = 0;
    }
    return px;
  }

  int Chan(const dds::Px& p, const int c) { return c == 0 ? p.r : c == 1 ? p.g : c == 2 ? p.b : p.a; }
}

int main()
{
  // ---- resampling ----
  {
    tc::Plane c = tc::ConstPlane(8, 8, 0.37f);
    for (const auto& [w, h] : {std::pair{4, 4}, {3, 5}, {16, 16}, {7, 9}, {1, 1}}) {
      const tc::Plane r = tc::Resample(c, w, h);
      bool same = r.w == w && r.h == h;
      for (const float v : r.v) same = same && std::fabs(v - 0.37f) < 1e-5f;
      CHECK(same, "a constant plane stays constant at %d x %d", w, h);
    }
    tc::Plane g;
    g.w = 4;
    g.h = 2;
    g.v = {0, 1, 2, 3, 4, 5, 6, 7};
    const tc::Plane d = tc::Resample(g, 2, 1);
    CHECK(d.v.size() == 2 && std::fabs(d.v[0] - 2.5f) < 1e-5f && std::fabs(d.v[1] - 4.5f) < 1e-5f, "2 x 2 area average: %f %f", d.v[0], d.v[1]);
    // a non integer ratio keeps the mean
    tc::Plane n;
    n.w = 10;
    n.h = 10;
    for (int i = 0; i < 100; ++i) n.v.push_back(static_cast<float>((i * 37) % 11) / 10.0f);
    double m0 = 0, m1 = 0;
    for (const float v : n.v) m0 += v;
    const tc::Plane r = tc::Resample(n, 4, 4);
    for (const float v : r.v) m1 += v;
    CHECK(std::fabs(m0 / 100.0 - m1 / 16.0) < 1e-4, "area average keeps the mean: %f vs %f", m0 / 100.0, m1 / 16.0);
    const tc::Plane up = tc::Resample(g, 8, 4);
    CHECK(up.w == 8 && up.h == 4 && std::fabs(up.v[0]) < 1e-5f && std::fabs(up.v[7] - 3.0f) < 1e-5f && std::fabs(up.v[3 * 8] - 4.0f) < 1e-5f && std::fabs(up.v[31] - 7.0f) < 1e-5f,
          "upsampling holds the edge texels: %f %f %f %f", up.v[0], up.v[7], up.v[24], up.v[31]);
  }

  // ---- channel repack is exact (no resampling, no compression) ----
  {
    const dds::Image src = Noise(16, 8, 5);
    tc::Chan ch[4];
    ch[0].from = tc::From::A;                 // r <- a
    ch[1].from = tc::From::R;                 // g <- r
    ch[2].from = tc::From::Const;             // b = 128
    ch[2].value = 128.0f / 255.0f;
    ch[3].from = tc::From::B;                 // a <- 1 - b
    ch[3].invert = true;
    const dds::Image out = tc::Convert(src, 16, 8, ch);
    bool ok = out.w == 16 && out.h == 8;
    for (std::size_t i = 0; i < out.px.size() && ok; ++i) {
      ok = out.px[i].r == src.px[i].a && out.px[i].g == src.px[i].r && out.px[i].b == 128 && out.px[i].a == 255 - src.px[i].b;
    }
    CHECK(ok, "swizzle, constant and inverted channels are exact");
    tc::Chan keep[4];
    const dds::Image same = tc::Convert(src, 16, 8, keep);
    bool eq = true;
    for (std::size_t i = 0; i < same.px.size(); ++i) {
      eq = eq && same.px[i].r == src.px[i].r && same.px[i].g == src.px[i].g && same.px[i].b == src.px[i].b && same.px[i].a == src.px[i].a;
    }
    CHECK(eq, "Keep everywhere is the identity");
    // scale / bias / clamp
    tc::Chan sb[4];
    sb[3].from = tc::From::A;
    sb[3].scale = -0.45f;
    sb[3].bias = 0.92f;
    sb[3].lo = 0.45f;
    sb[3].hi = 0.92f;
    const dds::Image rough = tc::Convert(src, 16, 8, sb);
    bool rok = true;
    for (std::size_t i = 0; i < rough.px.size(); ++i) {
      const float want = std::max(0.45f, std::min(0.92f, src.px[i].a / 255.0f * -0.45f + 0.92f));
      rok = rok && std::abs(static_cast<int>(rough.px[i].a) - static_cast<int>(want * 255.0f + 0.5f)) <= 1 && rough.px[i].r == src.px[i].r;
    }
    CHECK(rok, "roughness from spec alpha: scale -0.45, bias 0.92, clamp 0.45..0.92");
    // a channel that keeps its source but has arithmetic (the converter's "invert the alpha") still works on that channel when the whole picture is converted
    tc::Chan invA[4];
    invA[3].invert = true;
    const dds::Image ia = tc::Convert(src, 16, 8, invA);
    bool iaok = true;
    for (std::size_t i = 0; i < ia.px.size(); ++i) iaok = iaok && ia.px[i].a == 255 - src.px[i].a && ia.px[i].r == src.px[i].r && ia.px[i].b == src.px[i].b;
    CHECK(iaok, "Keep + invert inverts the channel's own values");
    // luminance
    tc::Chan lum[4];
    lum[1].from = tc::From::Lum;
    const dds::Image l = tc::Convert(src, 16, 8, lum);
    const int want = static_cast<int>((0.299f * src.px[3].r + 0.587f * src.px[3].g + 0.114f * src.px[3].b) + 0.5f);
    CHECK(std::abs(static_cast<int>(l.px[3].g) - want) <= 1, "luminance %d vs %d", l.px[3].g, want);
    // centring: the mean of the plane goes to 0.5
    tc::Chan hc[4];
    hc[1].from = tc::From::Lum;
    hc[1].center = true;
    hc[1].scale = 1.0f;
    const dds::Image hh = tc::Convert(Smooth(32, 32), 32, 32, hc);
    double mean = 0;
    for (const auto& p : hh.px) mean += p.g;
    mean /= static_cast<double>(hh.px.size());
    CHECK(std::fabs(mean / 255.0 - 0.5) < 0.01, "a centred plane has mean 0.5: %f", mean / 255.0);
    // a size change
    const dds::Image half = tc::Convert(Smooth(32, 32), 16, 16, keep);
    CHECK(half.w == 16 && half.h == 16, "resized");
  }

  // ---- DXT5 writer: round trip, mip chain ----
  {
    const dds::Image img = Smooth(64, 32);
    const std::string bytes = dds::EncodeDxt5(img, 32);
    dds::Header hd;
    CHECK(dds::ParseHeader(bytes, hd) && hd.w == 64 && hd.h == 32 && hd.format == dds::Format::Dxt5, "a DXT5 file");
    CHECK(dds::Levels(hd) == 7, "a whole chain of 64 x 32 has 7 levels: %d", dds::Levels(hd));
    int w = 0, h = 0;
    const auto px = Decode(bytes, 0, w, h);
    int worst = 0;
    for (std::size_t i = 0; i < px.size(); ++i) {
      for (int c = 0; c < 4; ++c) worst = std::max(worst, std::abs(Chan(px[i], c) - Chan(img.px[i], c)));
    }
    CHECK(w == 64 && h == 32 && worst <= 24, "round trip error %d", worst);
    const auto last = Decode(bytes, 6, w, h);
    CHECK(w == 1 && h == 1 && last.size() == 1, "the last level is 1 x 1");
    const std::string flat = dds::EncodeDxt5(img, 1);
    dds::Header hf;
    dds::ParseHeader(flat, hf);
    CHECK(dds::Levels(hf) == 1 && flat.size() == 128 + 16 * 16 * 8, "without mips: one level (%zu bytes)", flat.size());
  }

  // ---- alpha only: the colour is not touched ----
  {
    const dds::Image img = Smooth(64, 64);
    const std::string bytes = dds::EncodeDxt5(img, 32);
    std::uint8_t lut[256];
    tc::Chan inv;
    inv.from = tc::From::Keep;
    inv.invert = true;
    CHECK(tc::AlphaTable(inv, lut), "an inverted alpha is a table");
    std::string edited = bytes;
    CHECK(dds::Dxt5MapAlpha(edited, lut), "the DXT5 file is rewritten");
    CHECK(edited.size() == bytes.size(), "same size");
    int w = 0, h = 0;
    bool colourSame = true;
    int worstAlpha = 0;
    dds::Header hd;
    dds::ParseHeader(bytes, hd);
    for (int level = 0; level < dds::Levels(hd); ++level) {
      const auto a = Decode(bytes, level, w, h);
      const auto b = Decode(edited, level, w, h);
      for (std::size_t i = 0; i < a.size(); ++i) {
        colourSame = colourSame && a[i].r == b[i].r && a[i].g == b[i].g && a[i].b == b[i].b;
        worstAlpha = std::max(worstAlpha, std::abs(255 - a[i].a - b[i].a));
      }
    }
    CHECK(colourSame, "colour bytes are identical on every level");
    CHECK(worstAlpha <= 2, "inverted alpha, worst deviation %d", worstAlpha);
    // a constant
    tc::Chan zero;
    zero.from = tc::From::Const;
    zero.value = 0.0f;
    CHECK(tc::AlphaTable(zero, lut) && lut[0] == 0 && lut[255] == 0, "constant 0 table");
    std::string cleared = bytes;
    dds::Dxt5MapAlpha(cleared, lut);
    bool allZero = true;
    bool colour2 = true;
    for (int level = 0; level < dds::Levels(hd); ++level) {
      const auto a = Decode(bytes, level, w, h);
      const auto b = Decode(cleared, level, w, h);
      for (std::size_t i = 0; i < a.size(); ++i) {
        allZero = allZero && b[i].a == 0;
        colour2 = colour2 && a[i].r == b[i].r && a[i].g == b[i].g && a[i].b == b[i].b;
      }
    }
    CHECK(allZero && colour2, "alpha 0 everywhere, colours untouched");
    tc::Chan white;
    white.from = tc::From::Const;
    white.value = 1.0f;
    tc::AlphaTable(white, lut);
    std::string opaque = bytes;
    dds::Dxt5MapAlpha(opaque, lut);
    const auto o = Decode(opaque, 0, w, h);
    bool all255 = true;
    for (const auto& p : o) all255 = all255 && p.a == 255;
    CHECK(all255, "alpha 255 everywhere");
    // not a table: other channel, centre
    tc::Chan other;
    other.from = tc::From::R;
    CHECK(!tc::AlphaTable(other, lut), "reading the red channel is not an alpha only change");
    tc::Chan cen;
    cen.from = tc::From::A;
    cen.center = true;
    CHECK(!tc::AlphaTable(cen, lut), "centring is not an alpha only change");
    // a non DXT5 file is refused
    std::string junk(200, '\0');
    CHECK(!dds::Dxt5MapAlpha(junk, lut), "not a DDS");
    // spec alpha <-> matte alpha, the convention of strata alphas: scale -1 bias 1 equals invert
    std::uint8_t a[256], b[256];
    tc::Chan sb;
    sb.from = tc::From::Keep;
    sb.scale = -1.0f;
    sb.bias = 1.0f;
    tc::AlphaTable(sb, a);
    tc::AlphaTable(inv, b);
    bool same = true;
    for (int i = 0; i < 256; ++i) same = same && a[i] == b[i] && a[i] == 255 - i;
    CHECK(same, "scale -1 bias 1 is the inverse");
  }

  // ---- the atlas ----
  {
    const int S = 64;
    const int D = S / 4;
    std::vector<tc::Plane> rough(8), height(8);
    for (int k = 0; k < 8; ++k) {
      rough[k] = tc::ConstPlane(D, D, 0.1f * (k + 1));
      height[k].w = D;
      height[k].h = D;
      height[k].v.resize(D * D);
      for (int y = 0; y < D; ++y)
        for (int x = 0; x < D; ++x) height[k].v[y * D + x] = static_cast<float>((x * 7 + y * 3 + k * 11) % D) / static_cast<float>(D);
    }
    const dds::Image atlas = tc::BuildAtlas(S, rough.data(), height.data(), 0.65f, 0.5f);
    CHECK(atlas.w == S && atlas.h == S, "atlas size");
    // simulate the shader: atlas2D(uv, offset) with uv = tile coordinates in 0..1: pixel = (offset + 0.125 + u / 4) * S
    bool ok = true;
    int bad = 0;
    for (int k = 0; k < 8; ++k) {
      int quadrant = 0, pair = 0;
      tc::TileSlot(k, quadrant, pair);
      const float ox = (quadrant % 2) * 0.5f;
      const float oy = (quadrant / 2) * 0.5f;
      for (int j = 0; j < D; ++j) {
        for (int i = 0; i < D; ++i) {
          const float u = (i + 0.5f) / D;
          const float v = (j + 0.5f) / D;
          const int px = static_cast<int>(std::floor((ox + 0.125f + u / 4.0f) * S));
          const int py = static_cast<int>(std::floor((oy + 0.125f + v / 4.0f) * S));
          const dds::Px& p = atlas.px[static_cast<std::size_t>(py) * S + px];
          const int wantR = tc::ToByte(rough[k].At(i, j));
          const int wantH = tc::ToByte(height[k].At(i, j));
          const int gotR = pair == 0 ? p.r : p.b;
          const int gotH = pair == 0 ? p.g : p.a;
          if (gotR != wantR || gotH != wantH) {
            ok = false;
            ++bad;
          }
        }
      }
    }
    CHECK(ok, "the shader's atlas2D reads back every layer's own picture in its own orientation (%d texels wrong)", bad);
    // the picture repeats twice: a texel one period (D) further right holds the same value
    const dds::Px& a = atlas.px[5 * S + 3];
    const dds::Px& b = atlas.px[5 * S + 3 + D];
    CHECK(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a, "the tile holds its picture twice in each direction");
    // inverse
    const tc::Plane back = tc::AtlasTilePlane(atlas, 5, 3, D, D);
    bool inv = true;
    for (int i = 0; i < D * D; ++i) inv = inv && std::abs(tc::ToByte(back.v[i]) - tc::ToByte(height[5].v[i])) <= 0;
    CHECK(inv, "AtlasTilePlane gives the height of layer 4 (tile 6, channel A) back");
    const tc::Plane rb = tc::AtlasTilePlane(atlas, 2, 0, 8, 8);
    CHECK(std::abs(tc::ToByte(rb.v[0]) - tc::ToByte(0.3f)) <= 0, "roughness of tile 3 from channel R");
    // defaults for a tile without pictures
    std::vector<tc::Plane> none(8);
    const dds::Image neutral = tc::BuildAtlas(S, none.data(), none.data(), 0.65f, 0.5f);
    bool allDefault = true;
    for (const auto& p : neutral.px) allDefault = allDefault && p.r == tc::ToByte(0.65f) && p.b == p.r && p.g == tc::ToByte(0.5f) && p.a == p.g;
    CHECK(allDefault, "an atlas without pictures is the neutral one (roughness 0.65, height 0.5 in all four channels)");
    // writes as a DXT5 file with mips
    const std::string bytes = dds::EncodeDxt5(atlas, 32);
    dds::Header hd;
    CHECK(dds::ParseHeader(bytes, hd) && hd.w == S && dds::Levels(hd) == 7, "the atlas file: 64 x 64, 7 levels");
  }

  std::printf("EdTexConvTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
