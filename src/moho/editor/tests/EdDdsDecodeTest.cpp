// Stand-alone test of the DDS decoder of the editor natives (EdDdsDecode.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdDdsDecodeTest.cpp -o EdDdsDecodeTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk EdDdsDecodeTest.cpp
//
// What it checks: the header, the size and offset of every mip level (power of two and odd sizes, DXT1 / DXT3 / DXT5 / 32 bit), that a level decodes to its own texels (each level of the
// test files is painted in a colour of its own, so a wrong offset shows at once), the alpha and colour modes of the block formats, which level a thumbnail starts from, truncated and
// foreign files, and the cost of decoding the base level of a 2048 x 2048 texture against the level ed_dds_thumb(src, dst, 96, 1) starts from.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "moho/editor/EdDdsDecode.h"

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

static void Put32(std::string& s, const std::size_t at, const std::uint32_t v) { std::memcpy(&s[at], &v, 4); }

// the 565 colour that unpacks to (r, g, b) exactly: r, b in {0, 8 * k + k / 4...}: use values that are multiples of the unpack steps
static std::uint16_t Pack(const int r5, const int g6, const int b5) { return static_cast<std::uint16_t>((r5 << 11) | (g6 << 5) | b5); }

enum class Kind { Dxt1, Dxt3, Dxt5, Bgra };

// A DDS of w x h with `levels` mip levels (1 = none). Level k is painted in the colour (r5, g6, b5) = (k + 1, 2 * k + 3, 30 - k) and, for DXT3 / DXT5 / 32 bit, the alpha 16 * k + 5.
static std::string Make(const Kind kind, const int w, const int h, const int levels)
{
  std::string s(128, '\0');
  std::memcpy(&s[0], "DDS ", 4);
  Put32(s, 4, 124);
  Put32(s, 8, 0x00081007u | (levels > 1 ? 0x20000u : 0u));
  Put32(s, 12, static_cast<std::uint32_t>(h));
  Put32(s, 16, static_cast<std::uint32_t>(w));
  Put32(s, 28, levels > 1 ? static_cast<std::uint32_t>(levels) : 0u);
  Put32(s, 76, 32);
  if (kind == Kind::Bgra) {
    Put32(s, 80, 0x41);
    Put32(s, 88, 32);
  } else {
    Put32(s, 80, 0x4);
    std::memcpy(&s[84], kind == Kind::Dxt1 ? "DXT1" : (kind == Kind::Dxt3 ? "DXT3" : "DXT5"), 4);
  }
  for (int k = 0; k < levels; ++k) {
    const int lw = std::max(1, w >> k);
    const int lh = std::max(1, h >> k);
    const int a = 16 * k + 5;
    if (kind == Kind::Bgra) {
      const int r = 8 * (k + 1), g = 4 * (2 * k + 3), b = 8 * (30 - k);
      for (int i = 0; i < lw * lh; ++i) {
        s.push_back(static_cast<char>(b));
        s.push_back(static_cast<char>(g));
        s.push_back(static_cast<char>(r));
        s.push_back(static_cast<char>(a));
      }
      continue;
    }
    const int bw = std::max(1, (lw + 3) / 4), bh = std::max(1, (lh + 3) / 4);
    const std::uint16_t c = Pack(k + 1, 2 * k + 3, 30 - k);
    for (int i = 0; i < bw * bh; ++i) {
      if (kind == Kind::Dxt3) {
        for (int j = 0; j < 8; ++j) s.push_back(static_cast<char>(((a / 17) << 4) | (a / 17)));
      } else if (kind == Kind::Dxt5) {
        s.push_back(static_cast<char>(a));       // alpha 0 = a1 = a, all indices 0: the texel is alpha0
        s.push_back(static_cast<char>(a));
        for (int j = 0; j < 6; ++j) s.push_back('\0');
      }
      s.append(reinterpret_cast<const char*>(&c), 2);      // colour 0
      s.append(reinterpret_cast<const char*>(&c), 2);      // colour 1 (the same: every texel is this colour)
      for (int j = 0; j < 4; ++j) s.push_back('\0');       // indices 0
    }
  }
  return s;
}

static int Expand(const int v, const int max) { return v * 255 / max; }

static void TestLevels(const Kind kind, const char* const name, const int w, const int h, const int levels)
{
  const std::string file = Make(kind, w, h, levels);
  dds::Header hd;
  CHECK(dds::ParseHeader(file, hd), "%s %dx%d: header", name, w, h);
  CHECK(hd.w == w && hd.h == h, "%s: size %dx%d", name, hd.w, hd.h);
  CHECK(dds::Levels(hd) == levels, "%s: %d levels, expected %d", name, dds::Levels(hd), levels);
  for (int k = 0; k < levels; ++k) {
    std::vector<dds::Px> px;
    int lw = 0, lh = 0;
    CHECK(dds::DecodeLevel(file, hd, k, px, lw, lh), "%s %dx%d: level %d decodes", name, w, h, k);
    CHECK(lw == std::max(1, w >> k) && lh == std::max(1, h >> k), "%s level %d is %dx%d", name, k, lw, lh);
    CHECK(px.size() == static_cast<std::size_t>(lw) * static_cast<std::size_t>(lh), "%s level %d has %zu texels", name, k, px.size());
    int wrong = 0;
    for (const dds::Px& p : px) {
      int r, g, b, a = 255;
      if (kind == Kind::Bgra) {
        r = 8 * (k + 1); g = 4 * (2 * k + 3); b = 8 * (30 - k); a = 16 * k + 5;
      } else {
        r = Expand(k + 1, 31); g = Expand(2 * k + 3, 63); b = Expand(30 - k, 31);
        if (kind == Kind::Dxt3) a = ((16 * k + 5) / 17) * 17;
        if (kind == Kind::Dxt5) a = 16 * k + 5;
      }
      if (p.r != r || p.g != g || p.b != b || p.a != a) ++wrong;
    }
    CHECK(wrong == 0, "%s %dx%d level %d: %d texels have another colour than the level's own", name, w, h, k, wrong);
  }
  std::vector<dds::Px> px;
  int lw, lh;
  CHECK(!dds::DecodeLevel(file, hd, levels, px, lw, lh), "%s: level %d does not exist", name, levels);
  // a file cut off inside the last level fails for that level and decodes the ones before
  if (levels > 1) {
    std::string cut = file.substr(0, file.size() - 1);
    CHECK(!dds::DecodeLevel(cut, hd, levels - 1, px, lw, lh), "%s: a truncated last level is refused", name);
    CHECK(dds::DecodeLevel(cut, hd, 0, px, lw, lh), "%s: the base level of a truncated file still decodes", name);
  }
  // the sizes add up to the file
  std::size_t total = 128;
  for (int k = 0; k < levels; ++k) {
    int a, b;
    dds::LevelSize(hd, k, a, b);
    total += dds::LevelBytes(hd.format, a, b);
  }
  CHECK(total == file.size(), "%s %dx%d: the levels add up to %zu bytes, the file has %zu", name, w, h, total, file.size());
  CHECK(dds::LevelOffset(hd, levels) == file.size(), "%s: the offset after the last level is the end of the file", name);
}

static void TestBlockModes()
{
  // DXT5 alpha with a0 > a1: 8 levels interpolated; with a0 <= a1: 6 interpolated, then 0 and 255
  for (const int mode : {0, 1}) {
    std::string s = Make(Kind::Dxt5, 4, 4, 1);
    const std::size_t block = 128;
    s[block] = static_cast<char>(mode == 0 ? 200 : 40);        // a0
    s[block + 1] = static_cast<char>(mode == 0 ? 40 : 200);    // a1
    // indices 0..7 for the first eight texels, 7..0 for the rest (3 bits each, little endian)
    std::uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= static_cast<std::uint64_t>(i < 8 ? i : 15 - i) << (3 * i);
    for (int i = 0; i < 6; ++i) s[block + 2 + i] = static_cast<char>((bits >> (8 * i)) & 255);
    dds::Header hd;
    std::vector<dds::Px> px;
    int w, h;
    CHECK(dds::ParseHeader(s, hd) && dds::DecodeLevel(s, hd, 0, px, w, h), "dxt5 alpha block decodes");
    const int a0 = mode == 0 ? 200 : 40, a1 = mode == 0 ? 40 : 200;
    int pal[8] = {a0, a1};
    if (a0 > a1) { for (int i = 1; i < 7; ++i) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7; }
    else { for (int i = 1; i < 5; ++i) pal[1 + i] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
    int wrong = 0;
    for (int i = 0; i < 16; ++i) {
      if (px[i].a != pal[i < 8 ? i : 15 - i]) ++wrong;
    }
    CHECK(wrong == 0, "dxt5 alpha mode %d: %d texels differ from the palette", mode, wrong);
  }
  // DXT1 three colour mode (c0 <= c1): texel 2 is the middle, texel 3 is black
  {
    std::string s = Make(Kind::Dxt1, 4, 4, 1);
    const std::uint16_t c0 = Pack(0, 0, 0), c1 = Pack(31, 63, 31);
    std::memcpy(&s[128], &c0, 2);
    std::memcpy(&s[130], &c1, 2);
    const std::uint32_t idx = 0xE4;                    // texels 0..3 = indices 0, 1, 2, 3
    std::memcpy(&s[132], &idx, 4);
    dds::Header hd;
    std::vector<dds::Px> px;
    int w, h;
    CHECK(dds::ParseHeader(s, hd) && dds::DecodeLevel(s, hd, 0, px, w, h), "dxt1 block decodes");
    CHECK(px[0].r == 0 && px[1].r == 255 && px[2].r == 127 && px[3].r == 0 && px[2].a == 255, "dxt1 three colour mode: %d %d %d %d", px[0].r, px[1].r, px[2].r, px[3].r);
    std::swap(s[128], s[130]);
    std::swap(s[129], s[131]);                         // c0 > c1: four colours, texel 2 is two thirds of the way
    CHECK(dds::DecodeLevel(s, hd, 0, px, w, h) && px[2].r == 170 && px[3].r == 85, "dxt1 four colour mode: %d %d", px[2].r, px[3].r);
  }
}

static void TestPick()
{
  dds::Header h;
  h.w = 2048; h.h = 2048; h.mips = 12; h.format = dds::Format::Dxt5;
  CHECK(dds::PickLevel(h, 192) == 3, "2048 for a 96 thumbnail starts at level 3 (256), not %d", dds::PickLevel(h, 192));
  CHECK(dds::PickLevel(h, 8) == 8, "a 4 pixel thumbnail (texscale reads the width) takes the 8 pixel level: %d", dds::PickLevel(h, 8));
  h.w = 1024; h.h = 1024; h.mips = 11;
  CHECK(dds::PickLevel(h, 192) == 2, "1024 starts at level 2 (256)");
  h.w = 256; h.h = 256; h.mips = 9;
  CHECK(dds::PickLevel(h, 192) == 0, "256 stays at the base level (128 would be too small)");
  h.w = 64; h.h = 64;
  CHECK(dds::PickLevel(h, 192) == 0, "a texture smaller than the target keeps its base level");
  h.w = 2048; h.h = 512; h.mips = 12;
  CHECK(dds::PickLevel(h, 192) == 1, "the shorter side counts: 2048 x 512 starts at level 1 (1024 x 256)");
  h.w = 2048; h.h = 2048; h.mips = 0;
  CHECK(dds::PickLevel(h, 192) == 0, "a file without mips stays at the base level");
  h.mips = 1;
  CHECK(dds::PickLevel(h, 192) == 0, "a mip count of 1 is no chain");
}

static void TestForeign()
{
  dds::Header hd;
  CHECK(!dds::ParseHeader(std::string("not a dds"), hd), "short junk is refused");
  std::string s = Make(Kind::Dxt5, 8, 8, 1);
  s[0] = 'X';
  CHECK(!dds::ParseHeader(s, hd), "a wrong magic is refused");
  s = Make(Kind::Dxt5, 8, 8, 1);
  Put32(s, 16, 8192);
  CHECK(!dds::ParseHeader(s, hd), "a side above 4096 is refused");
  s = Make(Kind::Dxt5, 8, 8, 1);
  std::memcpy(&s[84], "DX10", 4);
  CHECK(!dds::ParseHeader(s, hd), "a DX10 file is not read (as before)");
}

static void TestSpeed()
{
  // the cost of the decode for a 2048 x 2048 DXT5 texture (the size of most layer textures): the base level against the level a 96 pixel thumbnail starts from
  const std::string file = Make(Kind::Dxt5, 2048, 2048, 12);
  dds::Header hd;
  dds::ParseHeader(file, hd);
  std::vector<dds::Px> px;
  int w, h;
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  for (int i = 0; i < 5; ++i) dds::DecodeLevel(file, hd, 0, px, w, h);
  const auto t1 = Clock::now();
  const int level = dds::PickLevel(hd, 192);
  for (int i = 0; i < 5; ++i) dds::DecodeLevel(file, hd, level, px, w, h);
  const auto t2 = Clock::now();
  const double base = std::chrono::duration<double, std::milli>(t1 - t0).count() / 5.0;
  const double small = std::chrono::duration<double, std::milli>(t2 - t1).count() / 5.0;
  std::printf("decode 2048x2048 DXT5: base level %.2f ms, level %d (%dx%d) %.3f ms (this machine, one core)\n", base, level, w, h, small);
  CHECK(small * 8.0 < base, "the small level decodes much faster than the base level (%.3f vs %.3f ms)", small, base);
}

int main()
{
  for (const auto& d : {std::pair<int, int>{256, 256}, {64, 32}, {100, 60}, {17, 5}, {4, 4}, {1, 1}}) {
    int levels = 1;
    int m = std::max(d.first, d.second);
    while (m > 1) { m >>= 1; ++levels; }
    TestLevels(Kind::Dxt1, "DXT1", d.first, d.second, levels);
    TestLevels(Kind::Dxt3, "DXT3", d.first, d.second, levels);
    TestLevels(Kind::Dxt5, "DXT5", d.first, d.second, levels);
    TestLevels(Kind::Bgra, "BGRA32", d.first, d.second, levels);
    TestLevels(Kind::Dxt5, "DXT5 (no mips)", d.first, d.second, 1);
  }
  TestBlockModes();
  TestPick();
  TestForeign();
  TestSpeed();
  std::printf("EdDdsDecodeTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
