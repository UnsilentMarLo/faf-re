// Host test of the shadow filter effect patch (src/moho/render/shadow/ShadowEffectPatch): it must refuse text it does not recognise and
// leave it untouched, and, when FAF_STOCK_FX_DIR names a folder with FAF's stock mesh.fx and terrain.fx (extract them from effects.nx2; the
// files are not part of this repository), patch both completely, consistently, and chain with the dynamic light patch in either order.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "moho/render/lights/LightEffectPatch.h"
#include "moho/render/shadow/ShadowEffectPatch.h"

namespace
{
  using namespace moho::shadowfilter;

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

  bool ReadFile(const std::string& path, std::string& out)
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    // the shipped files are LF; a checkout with autocrlf has CRLF
    std::string lf;
    for (const char c : out) {
      if (c != 0x0D) {
        lf.push_back(c);
      }
    }
    out = std::move(lf);
    return true;
  }

  std::size_t Count(const std::string& s, const std::string& needle)
  {
    std::size_t n = 0;
    for (std::size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + needle.size())) {
      ++n;
    }
    return n;
  }

  long Braces(const std::string& s)
  {
    long depth = 0;
    for (const char c : s) {
      depth += c == '{' ? 1 : (c == '}' ? -1 : 0);
    }
    return depth;
  }

  void TestRefusesUnknownText()
  {
    for (const std::string original : {std::string(), std::string("float4 main() : COLOR { return 0; }\n")}) {
      std::string text = original;
      CHECK(!PatchMeshEffect(text).patched);
      CHECK(text == original);
      CHECK(!PatchTerrainEffect(text).patched);
      CHECK(text == original);
    }
    CHECK(HandlesEffect("mesh") && HandlesEffect("terrain") && !HandlesEffect("water2"));
  }

  void TestMesh(const std::string& stock)
  {
    std::string text = stock;
    const PatchResult result = PatchMeshEffect(text);
    CHECK(result.patched);
    CHECK(result.error.empty());
    CHECK(Braces(text) == Braces(stock));
    CHECK(Count(text, "float4 shadowFilter;") == 1);
    CHECK(Count(text, "ComputeShadowFiltered(") == 2); // definition + the call in ComputeShadow
    CHECK(Count(text, "ComputeShadowPCF(") == Count(stock, "ComputeShadowPCF(")); // the shipped lookup stays as the fallback
    CHECK(Count(text, "tex2Dlod(shadowSampler") == 1);
    CHECK(text.find("int taps = (int)shadowFilter.x;") != std::string::npos);
    CHECK(text.find(char(13)) == std::string::npos);
    std::string again = text;
    CHECK(!PatchMeshEffect(again).patched); // not twice
  }

  void TestTerrain(const std::string& stock)
  {
    std::string text = stock;
    const PatchResult result = PatchTerrainEffect(text);
    CHECK(result.patched);
    CHECK(result.lookups >= 5);
    CHECK(Braces(text) == Braces(stock));
    CHECK(Count(text, "float4 ShadowFilter;") == 1);
    // the only direct lookup of the blurred mask left is the fallback inside ShadowLookup
    CHECK(Count(text, "tex2D( ShadowSampler, coords.xy ).g") == 1);
    CHECK(Count(text, ").g") >= 0);
    CHECK(Count(text, "ShadowLookup(") == static_cast<std::size_t>(result.lookups) + 1);
    CHECK(text.find("tex2D(ShadowSampler") == std::string::npos);
    CHECK(text.find("ShadowLookup(inV.mShadow)") != std::string::npos);
    std::string again = text;
    CHECK(!PatchTerrainEffect(again).patched);
  }

  void TestChainsWithLights(const std::string& mesh, const std::string& terrain)
  {
    // shadow first, then the lights (the order the engine uses), and the other way round
    for (const bool shadowFirst : {true, false}) {
      std::string m = mesh;
      std::string t = terrain;
      if (shadowFirst) {
        CHECK(PatchMeshEffect(m).patched);
        CHECK(PatchTerrainEffect(t).patched);
      }
      CHECK(moho::lights::PatchMeshEffect(m).patched);
      CHECK(moho::lights::PatchTerrainEffect(t).patched);
      if (!shadowFirst) {
        CHECK(PatchMeshEffect(m).patched);
        CHECK(PatchTerrainEffect(t).patched);
      }
      CHECK(m.find("dynLightData") != std::string::npos && m.find("shadowFilter") != std::string::npos);
      CHECK(t.find("dynLightData") != std::string::npos && t.find("ShadowFilter") != std::string::npos);
      CHECK(Braces(m) == Braces(mesh) && Braces(t) == Braces(terrain));
    }
  }
} // namespace

int main()
{
  TestRefusesUnknownText();

  const char* const dir = std::getenv("FAF_STOCK_FX_DIR");
  std::string mesh;
  std::string terrain;
  if (dir != nullptr && ReadFile(std::string(dir) + "/mesh.fx", mesh) && ReadFile(std::string(dir) + "/terrain.fx", terrain)) {
    TestMesh(mesh);
    TestTerrain(terrain);
    TestChainsWithLights(mesh, terrain);
  } else {
    std::printf("FAF_STOCK_FX_DIR not set: the stock effect checks were skipped\n");
  }

  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
