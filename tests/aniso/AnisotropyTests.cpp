// Host test of the forced anisotropic filter rules (src/gpg/gal/ForcedAnisotropy.h): which filters become anisotropic, the level table, the
// epoch and the data-sampler names. Header only, no engine objects.

#include <cstdint>
#include <cstdio>
#include <string>

#include "gpg/gal/ForcedAnisotropy.h"

namespace
{
  int gFailures = 0;
  int gChecks = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    ++gChecks;                                                                                                         \
    if (!(cond)) {                                                                                                     \
      ++gFailures;                                                                                                     \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                      \
    }                                                                                                                  \
  } while (0)

  using namespace gpg::gal;

  // D3D11_FILTER values: MIN_MAG_MIP_POINT 0x00, MIN_MAG_POINT_MIP_LINEAR 0x01, MIN_POINT_MAG_LINEAR_MIP_POINT 0x04, MIN_LINEAR_MAG_MIP_POINT 0x10,
  // MIN_MAG_LINEAR_MIP_POINT 0x14, MIN_MAG_MIP_LINEAR 0x15, ANISOTROPIC 0x55, COMPARISON_MIN_MAG_MIP_LINEAR 0x95.
  // The members effects::SamplerDesc has that the rule reads.
  struct MockSampler
  {
    enum class Kind { Texture2D, TextureCube, Texture3D };
    std::string name = "albedoSampler";
    Kind kind = Kind::Texture2D;
    std::uint32_t filter = 0x15;
    std::uint32_t maxAnisotropy = 16;
    float maxLod = 3.4e38f;
  };

  bool Forced(const std::uint32_t in, std::uint32_t& out)
  {
    out = in;
    return ForceAnisotropicFilter(out);
  }
} // namespace

int main()
{
  std::uint32_t out = 0;

  // Trilinear and bilinear (mip point) become anisotropic.
  CHECK(Forced(0x15, out) && out == 0x55);
  CHECK(Forced(0x14, out) && out == 0x55);
  // Point, mixed, anisotropic and comparison filters stay.
  CHECK(!Forced(0x00, out) && out == 0x00);
  CHECK(!Forced(0x01, out) && out == 0x01);
  CHECK(!Forced(0x04, out) && out == 0x04);
  CHECK(!Forced(0x10, out) && out == 0x10);
  CHECK(!Forced(0x55, out) && out == 0x55);
  CHECK(!Forced(0x95, out) && out == 0x95);
  CHECK(!Forced(0xD5, out) && out == 0xD5);

  // Levels: off, 2x, 4x, 8x, 16x; out-of-range levels clamp.
  CHECK(AnisotropyForLevel(-3) == 1);
  CHECK(AnisotropyForLevel(0) == 1);
  CHECK(AnisotropyForLevel(1) == 2);
  CHECK(AnisotropyForLevel(2) == 4);
  CHECK(AnisotropyForLevel(3) == 8);
  CHECK(AnisotropyForLevel(4) == 16);
  CHECK(AnisotropyForLevel(9) == 16);

  // The epoch is never 0 and changes with the level.
  gForcedAnisotropyLevel = 0;
  const unsigned off = ForcedAnisotropyEpoch();
  CHECK(off != 0U && ForcedAnisotropy() == 1);
  gForcedAnisotropyLevel = 4;
  CHECK(ForcedAnisotropyEpoch() != off && ForcedAnisotropy() == 16);
  gForcedAnisotropyLevel = 7;
  CHECK(ForcedAnisotropy() == 16 && ForcedAnisotropyEpoch() == 5U);
  gForcedAnisotropyLevel = -2;
  CHECK(ForcedAnisotropy() == 1 && ForcedAnisotropyEpoch() == 1U);

  // Data samplers, case-insensitive substring; world samplers are not.
  CHECK(IsDataSampler("dynLightDataSampler"));
  CHECK(IsDataSampler("dynLightGridSampler"));
  CHECK(IsDataSampler("lookupSampler"));
  CHECK(IsDataSampler("horizonLookupSampler"));
  CHECK(IsDataSampler("anisotropicSampler"));
  CHECK(IsDataSampler("WaterRampSampler"));
  CHECK(IsDataSampler("FresnelSampler"));
  CHECK(!IsDataSampler("albedoSampler"));
  CHECK(!IsDataSampler("normalsSampler"));
  CHECK(!IsDataSampler("decalAlbedoSampler"));
  CHECK(!IsDataSampler("UtilitySamplerA"));
  CHECK(!IsDataSampler(""));

  // The sampler rule: world 2D samplers become anisotropic at the level, everything else stays.
  {
    MockSampler s;
    CHECK(ForceAnisotropicSampler(s, 8) && s.filter == 0x55 && s.maxAnisotropy == 8);
    MockSampler off;
    CHECK(!ForceAnisotropicSampler(off, 1) && off.filter == 0x15);
    MockSampler cube;
    cube.kind = MockSampler::Kind::TextureCube;
    CHECK(!ForceAnisotropicSampler(cube, 16) && cube.filter == 0x15);
    MockSampler data;
    data.name = "dynLightDataSampler";
    CHECK(!ForceAnisotropicSampler(data, 16) && data.filter == 0x15);
    MockSampler point;
    point.filter = 0x00;
    CHECK(!ForceAnisotropicSampler(point, 16) && point.filter == 0x00);
    MockSampler noMips;
    noMips.maxLod = 0.0f;
    CHECK(!ForceAnisotropicSampler(noMips, 16) && noMips.filter == 0x15);
    MockSampler already;
    already.filter = 0x55;
    already.maxAnisotropy = 4;
    CHECK(!ForceAnisotropicSampler(already, 16) && already.maxAnisotropy == 4);
  }

  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
