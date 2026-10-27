// Host test of the particle.fx source patch (src/moho/particles/ParticleEffectPatch): it must refuse text it does not recognise and leave it
// untouched, patch a minimal source that has the shape the patch relies on, and, when FAF_STOCK_FX_DIR names a folder with FAF's stock
// particle.fx (extract it from effects.nx2; the file is not part of this repository), patch it completely and consistently.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "moho/particles/ParticleEffectPatch.h"

namespace
{
  int gFailures = 0;
  int gChecks = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    ++gChecks;                                                     \
    if (!(cond)) {                                                 \
      ++gFailures;                                                 \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
    }                                                              \
  } while (0)

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

  // The parts of particle.fx the patch looks at, in the same order and with the same text.
  const char* kMinimal = R"FX(
float4x4 Projection;
float4x4 WorldToProjection;
struct WORLD_OUTPUT {
   float4 mPos  : POSITION;
};
WORLD_OUTPUT WorldVS(float2 Corner : POSITION0, float2 Size: TEXCOORD0, uniform bool inAnimTexture, uniform bool inFlat)
{
    WORLD_OUTPUT Out = (WORLD_OUTPUT)0;
    float t = 0;
    float3 pos = float3(0,0,0);
		// Billboard the quads.
		pos += float3(Corner, 0) * (Size.x + (Size.y * t));
    return Out;
}
WORLD_OUTPUT WorldVSAlign(float2 Corner : POSITION0, float2 Size: TEXCOORD0, uniform bool inAnimTexture, uniform bool nomovement)
{
    WORLD_OUTPUT Out = (WORLD_OUTPUT)0;
    float t = 0;
    float3 pos = float3(0,0,0);
			pos = mul( float4(pos, 1), ViewMatrix );
    return Out;
}
struct PS_OUTPUT {
   float4 color : COLOR0;
};
PS_OUTPUT WorldPS(WORLD_OUTPUT inData)
{
	PS_OUTPUT o = (PS_OUTPUT)0;
	return o;
}
technique TRamp_ADD
{
	pass P0
	{
		AlphaState( AlphaBlend_SrcAlpha_One )
		DepthState( Depth_Enable_Less_Write_None )
		RasterizerState( Rasterizer_Cull_None )

		VertexShader = compile vs_1_1 WorldVS(false,false);
		PixelShader = compile ps_2_0 WorldPS();
	}
}
technique TRamp_ALPHABLEND
{
	pass P0
	{
		AlphaState( AlphaBlend_SrcAlpha_InvSrcAlpha_Write_RGB )
		DepthState( Depth_Enable_Less_Write_None )
		RasterizerState( Rasterizer_Cull_None )

		VertexShader = compile vs_1_1 WorldVS(false,false);
		PixelShader = compile ps_2_0 WorldPS();
	}
}
technique TRampFlat_ADD
{
	pass P0
	{
		AlphaState( AlphaBlend_SrcAlpha_One )
		DepthState( Depth_Enable_Less_Write_None )
		RasterizerState( Rasterizer_Cull_None )

		VertexShader = compile vs_1_1 WorldVS(false,true);
		PixelShader = compile ps_2_0 WorldPS();
	}
}
technique TRamp_PREMODALPHA
{
	pass P0
	{
		AlphaState( AlphaBlend_One_InvSrcAlpha_Write_RGB )
		DepthState( Depth_Enable_Less_Write_None )
		RasterizerState( Rasterizer_Cull_None )

		VertexShader = compile vs_1_1 WorldVS(false,false);
		PixelShader = compile ps_2_0 WorldPS();
	}
}
technique TRamp_MODULATEINVERSE
{
	pass P0
	{
		AlphaState( AlphaBlend_Zero_InvSrcColor )
		DepthState( Depth_Enable_Less_Write_None )
		VertexShader = compile vs_1_1 WorldVS(false,false);
		PixelShader = compile ps_2_0 WorldPS();
	}
}
technique TLight_ADD
{
	pass P0
	{
		VertexShader = compile vs_1_1 WorldVS(false,true);
		PixelShader = compile ps_2_0 LightPS();
	}
}
)FX";

  void TestRefusesUnknownText()
  {
    for (const std::string original : {std::string(), std::string("float4 main() : COLOR { return 0; }\n")}) {
      std::string text = original;
      const moho::ParticleEffectPatchResult result = moho::PatchParticleEffect(text);
      CHECK(!result.patched);
      CHECK(!result.error.empty());
      CHECK(text == original);
    }
  }

  void TestMinimalSource()
  {
    std::string text = kMinimal;
    const moho::ParticleEffectPatchResult result = moho::PatchParticleEffect(text);
    CHECK(result.patched);
    CHECK(result.techniques == 4); // the ADD, ALPHABLEND, PREMODALPHA and flat ADD ones; neither MODULATE nor the TLight technique
    CHECK(result.softened == 3);   // the flat one stays hard-edged
    CHECK(Count(text, "technique TRamp_ADD_LR") == 1U);
    CHECK(Count(text, "technique TRamp_ALPHABLEND_LR") == 1U);
    CHECK(Count(text, "technique TRamp_PREMODALPHA_LR") == 1U);
    CHECK(Count(text, "MODULATEINVERSE_LR") == 0U);
    CHECK(Count(text, "TLight_ADD_LR") == 0U);
    CHECK(Count(text, "LRCull(") >= 3U); // the declaration and the two vertex shader hooks
    CHECK(Count(text, "WorldPSSoftLRPremultiplied()") == 1U); // only the ALPHABLEND twin
    CHECK(Count(text, "WorldPSSoftLR()") == 1U);              // the ADD twin
    CHECK(Count(text, "WorldPSSoftLRPremod()") == 1U);        // the PREMODALPHA twin
    CHECK(Count(text, "technique LRComposite") == 1U);
    CHECK(Count(text, "LRDownsamplePS2") >= 2U && Count(text, "LRDownsamplePS4") >= 2U);
    CHECK(Braces(text) == Braces(std::string(kMinimal)));

    // Soft particles: the full-resolution ADD / ALPHABLEND techniques use WorldPSSoft, PREMODALPHA the premodulated one; MODULATE and TLight
    // keep their shaders; the soft shaders are defined before the first technique that uses them.
    CHECK(Count(text, "WorldPSSoft()") == 2U);
    CHECK(Count(text, "WorldPSSoftPremod()") == 1U);
    CHECK(Count(text, "WorldPS()") == 3U); // MODULATEINVERSE, and the flat ADD technique with its twin
    CHECK(Count(text, "LightPS()") == 1U);
    CHECK(Count(text, "float4 SoftParams") == 1U);
    CHECK(text.find("PS_OUTPUT WorldPSSoft(") < text.find("technique TRamp_ADD"));
    CHECK(text.find("PS_OUTPUT WorldPSSoftPremod(") < text.find("technique TRamp_PREMODALPHA"));
    CHECK(text.find("PS_OUTPUT WorldPS(") < text.find("PS_OUTPUT WorldPSSoft("));
    CHECK(text.find("float SoftFade(") < text.find("PS_OUTPUT WorldPSSoft("));
    CHECK(text.find("float4 SoftParams") < text.find("float SoftFade("));

    // The vertex shader hooks sit where the quad is still the particle's centre.
    CHECK(text.find("LRCull") < text.find("WORLD_OUTPUT WorldVS("));
    CHECK(text.find("return Out;", text.find("// Billboard the quads.") - 200) < text.find("pos += float3(Corner, 0)"));

    // A patched source is not patched twice.
    std::string again = text;
    const moho::ParticleEffectPatchResult second = moho::PatchParticleEffect(again);
    CHECK(!second.patched);
    CHECK(again == text);
  }

  void TestStockParticleFx()
  {
    const char* const directory = std::getenv("FAF_STOCK_FX_DIR");
    if (directory == nullptr) {
      std::printf("particle.fx check skipped (FAF_STOCK_FX_DIR is not set)\n");
      return;
    }
    std::ifstream in(std::string(directory) + "/particle.fx", std::ios::binary);
    if (!in) {
      std::printf("particle.fx check skipped (no particle.fx in %s)\n", directory);
      return;
    }
    const std::string original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string text = original;
    const moho::ParticleEffectPatchResult result = moho::PatchParticleEffect(text);
    CHECK(result.patched);
    // 8 tags (TRamp, TRampAnimate, TRampAlign, TRampAnimateAlign, TRampAnimateAlignToBone, TRampAlignToBone, TRampFlat, TRampAnimateFlat) x 3 blends.
    CHECK(result.techniques == 24);
    CHECK(result.softened == 18); // not the Flat and AnimateFlat tags
    CHECK(Count(text, "WorldPSSoft()") + Count(text, "WorldPSSoftPremod()") == 18U);
    CHECK(Braces(text) == Braces(original));
    CHECK(Count(text, "_LR\n") + Count(text, "_LR\r\n") == 24U);
  }
} // namespace

int main()
{
  TestRefusesUnknownText();
  TestMinimalSource();
  TestStockParticleFx();
  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
