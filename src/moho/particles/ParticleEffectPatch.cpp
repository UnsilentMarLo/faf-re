#include "moho/particles/ParticleEffectPatch.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace moho
{
  namespace
  {
    // Declarations, inserted ahead of `struct WORLD_OUTPUT` (the vertex shaders use them).
    constexpr std::string_view kDeclarations = R"HLSL(
// ---- FAF: large particles in a lower-resolution target (ParticleLowRes.cpp) ---------------------------------------------------
float4 LRParams = float4(0, 100000, 0, 0); // x: 0 every quad, 1 only the small ones, 2 only the large ones; y: threshold in pixels^2 at full resolution; zw: half the viewport size
float4 LRTarget = float4(1, 1, 1, 1);      // low-resolution target: width, height, 1/width, 1/height
float4 LRView = float4(0, 0, 1, 1);        // the viewport in the scene target: x, y, width, height
float4 LRScene = float4(1, 1, 1, 1);       // the scene target: width, height, 1/width, 1/height
float4 LRRatio = float4(2, 2, 2, 0);       // xy: viewport pixels per low-resolution pixel, z: the integer scale
float4 LRQuad = float4(1, 1, 1, 1);        // the output target of the full-screen passes: width, height, 1/width, 1/height

texture LRAccumTexture;
texture LRDepthTexture;
texture LRSceneDepthTexture;

sampler2D LRAccumSampler = sampler_state
{
	Texture   = (LRAccumTexture);
	MipFilter = NONE;
	MinFilter = POINT;
	MagFilter = POINT;
	AddressU  = CLAMP;
	AddressV  = CLAMP;
};

sampler2D LRDepthSampler = sampler_state
{
	Texture   = (LRDepthTexture);
	MipFilter = NONE;
	MinFilter = POINT;
	MagFilter = POINT;
	AddressU  = CLAMP;
	AddressV  = CLAMP;
};

sampler2D LRSceneDepthSampler = sampler_state
{
	Texture   = (LRSceneDepthTexture);
	MipFilter = NONE;
	MinFilter = POINT;
	MagFilter = POINT;
	AddressU  = CLAMP;
	AddressV  = CLAMP;
};

// Whether a quad of half size `halfSize` (world units) at view depth `viewDepth` covers LRParams.y pixels^2 or more at full resolution.
bool LRIsLarge(float halfSize, float viewDepth)
{
	float w = max(viewDepth, 0.001);
	float2 pixels = halfSize * 2.0 * float2(Projection[0][0] * LRParams.z, Projection[1][1] * LRParams.w) / w;
	return (pixels.x * pixels.y) >= LRParams.y;
}

// True when the quad is not drawn by the pass LRParams.x selects.
bool LRCull(float halfSize, float viewDepth)
{
	bool large = LRIsLarge(halfSize, viewDepth);
	return (LRParams.x < 1.5) ? large : !large;
}

float LRViewZ(float depth)
{
	return Projection[3][2] / (depth - Projection[2][2]);
}

// 24 bit depth in two 16 bit unorm channels (the low-resolution depth target is RG16).
float2 LRPackDepth(float depth)
{
	float v = floor(depth * 16777215.0 + 0.5);
	float hi = floor(v / 256.0);
	return float2(hi / 65535.0, (v - hi * 256.0) / 255.0);
}

float LRUnpackDepth(float2 value)
{
	return (floor(value.x * 65535.0 + 0.5) * 256.0 + floor(value.y * 255.0 + 0.5)) / 16777215.0;
}

BlendState LR_Accumulate_Alpha
{
	BlendEnable[0] = true;
	RenderTargetWriteMask[0] = 0x0F;
	SrcBlend = SRC_ALPHA;
	DestBlend = INV_SRC_ALPHA;
	BlendOp = ADD;
	SrcBlendAlpha = ONE;
	DestBlendAlpha = INV_SRC_ALPHA;
	BlendOpAlpha = ADD;
};

BlendState LR_Accumulate_Premod
{
	BlendEnable[0] = true;
	RenderTargetWriteMask[0] = 0x0F;
	SrcBlend = ONE;
	DestBlend = INV_SRC_ALPHA;
	BlendOp = ADD;
	SrcBlendAlpha = ONE;
	DestBlendAlpha = INV_SRC_ALPHA;
	BlendOpAlpha = ADD;
};

)HLSL";

    // Appended at the end of the file (everything it uses is declared above).
    constexpr std::string_view kShaders = R"HLSL(

// ---- FAF: large particles in a lower-resolution target (ParticleLowRes.cpp) ---------------------------------------------------
PS_OUTPUT WorldPSLR(WORLD_OUTPUT inData)
{
	PS_OUTPUT o = (PS_OUTPUT)0;
	float lowDepth = LRUnpackDepth(tex2D(LRDepthSampler, inData.mPos.xy * LRTarget.zw).xy);
	float4 color = tex2D(ParticleSampler0,inData.mTex0) * tex2D(ParticleSampler1,inData.mTex1);
	if (inData.mPos.z >= lowDepth)
		discard;
	o.color = color;
	return o;
}

struct LR_QUAD_IN {
   float4 Pos : POSITION;
   float2 Tex : TEXCOORD0;
};

struct LR_QUAD_OUT {
   float4 mPos : POSITION;
   float2 mTex : TEXCOORD0;
};

LR_QUAD_OUT LRQuadVS(LR_QUAD_IN In)
{
	LR_QUAD_OUT Out = (LR_QUAD_OUT)0;
	float2 position = (In.Pos.xy + float2(0.5, 0.5)) * LRQuad.zw;
	Out.mPos = float4(2 * position.x - 1, 1 - 2 * position.y, In.Pos.z, In.Pos.w);
	Out.mTex = In.Tex;
	return Out;
}

@DOWNSAMPLE@

float4 LRCompositePS(LR_QUAD_OUT inData) : COLOR
{
	// The engine's pixel coordinates (pixel j is centred on j) and the low-resolution texel (i) they fall between.
	float2 pixel = inData.mPos.xy - float2(0.5, 0.5);
	float sceneDepth = tex2D(LRSceneDepthSampler, inData.mPos.xy * LRScene.zw).x;
	float2 q = (pixel - LRView.xy) / LRRatio.xy;
	float2 texel = floor(q);
	float2 f = q - texel;
	float2 uv = (texel + float2(0.5, 0.5)) * LRTarget.zw;
	float2 du = float2(LRTarget.z, 0);
	float2 dv = float2(0, LRTarget.w);

	float4 c00 = tex2D(LRAccumSampler, uv);
	float4 c10 = tex2D(LRAccumSampler, uv + du);
	float4 c01 = tex2D(LRAccumSampler, uv + dv);
	float4 c11 = tex2D(LRAccumSampler, uv + du + dv);

	float z = LRViewZ(sceneDepth);
	float e00 = abs(LRViewZ(LRUnpackDepth(tex2D(LRDepthSampler, uv).xy)) - z);
	float e10 = abs(LRViewZ(LRUnpackDepth(tex2D(LRDepthSampler, uv + du).xy)) - z);
	float e01 = abs(LRViewZ(LRUnpackDepth(tex2D(LRDepthSampler, uv + dv).xy)) - z);
	float e11 = abs(LRViewZ(LRUnpackDepth(tex2D(LRDepthSampler, uv + du + dv).xy)) - z);

	float4 result = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
	if (max(max(e00, e10), max(e01, e11)) > 0.03 * z)
	{
		// A depth edge: take the one low-resolution texel whose depth is closest to this pixel's.
		float best = e00;
		result = c00;
		if (e10 < best) { best = e10; result = c10; }
		if (e01 < best) { best = e01; result = c01; }
		if (e11 < best) { best = e11; result = c11; }
	}
	return result;
}

technique LRDownsample2
{
	pass P0
	{
		AlphaState( AlphaBlend_Disable )
		DepthState( Depth_Disable_Write_None )
		RasterizerState( Rasterizer_Cull_None )

		VertexShader = compile vs_1_1 LRQuadVS();
		PixelShader = compile ps_2_0 LRDownsamplePS2();
	}
}

technique LRDownsample4
{
	pass P0
	{
		AlphaState( AlphaBlend_Disable )
		DepthState( Depth_Disable_Write_None )
		RasterizerState( Rasterizer_Cull_None )

		VertexShader = compile vs_1_1 LRQuadVS();
		PixelShader = compile ps_2_0 LRDownsamplePS4();
	}
}

technique LRComposite_ADD
{
	pass P0
	{
		AlphaState( AlphaBlend_One_One_Write_RGB )
		DepthState( Depth_Disable_Write_None )
		RasterizerState( Rasterizer_Cull_None )

		VertexShader = compile vs_1_1 LRQuadVS();
		PixelShader = compile ps_2_0 LRCompositePS();
	}
}

technique LRComposite_OVER
{
	pass P0
	{
		AlphaState( AlphaBlend_One_InvSrcAlpha_Write_RGB )
		DepthState( Depth_Disable_Write_None )
		RasterizerState( Rasterizer_Cull_None )

		VertexShader = compile vs_1_1 LRQuadVS();
		PixelShader = compile ps_2_0 LRCompositePS();
	}
}
)HLSL";

    // The farthest scene depth in the (2n-1)^2 footprint around the full-resolution pixel a low-resolution pixel sits on; n is the scale.
    std::string DownsampleShader(const int scale)
    {
      const std::string n = std::to_string(scale);
      const std::string reach = std::to_string(scale - 1);
      std::string text;
      text += "float4 LRDownsamplePS" + n + "(LR_QUAD_OUT inData) : COLOR\n{\n";
      text += "\tfloat2 center = LRView.xy + floor(inData.mPos.xy) * LRRatio.xy;\n";
      text += "\tfloat2 minPixel = LRView.xy;\n\tfloat2 maxPixel = LRView.xy + LRView.zw - float2(1, 1);\n";
      text += "\tfloat farthest = 0;\n";
      text += "\tfor (int y = -" + reach + "; y <= " + reach + "; ++y)\n\t{\n";
      text += "\t\tfor (int x = -" + reach + "; x <= " + reach + "; ++x)\n\t\t{\n";
      text += "\t\t\tfloat2 pixel = clamp(center + float2(x, y), minPixel, maxPixel);\n";
      text += "\t\t\tfarthest = max(farthest, tex2D(LRSceneDepthSampler, (pixel + float2(0.5, 0.5)) * LRScene.zw).x);\n";
      text += "\t\t}\n\t}\n";
      text += "\treturn float4(LRPackDepth(farthest), 0, 0);\n}\n";
      return text;
    }

    [[nodiscard]] std::size_t CountOf(const std::string& text, const std::string_view what)
    {
      std::size_t count = 0U;
      for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
        ++count;
      }
      return count;
    }

    [[nodiscard]] std::size_t MatchingBrace(const std::string& text, const std::size_t open)
    {
      int depth = 0;
      for (std::size_t index = open; index < text.size(); ++index) {
        if (text[index] == '{') {
          ++depth;
        } else if (text[index] == '}' && --depth == 0) {
          return index;
        }
      }
      return std::string::npos;
    }

    /** Replaces the call `<key>( ... )` (first one in `text`) with `<key>( replacement )`; false when there is none. */
    bool ReplaceCallArgument(std::string& text, const std::string_view key, const std::string_view replacement)
    {
      const std::size_t at = text.find(key);
      if (at == std::string::npos) {
        return false;
      }
      const std::size_t open = text.find('(', at);
      const std::size_t close = text.find(')', open);
      if (open == std::string::npos || close == std::string::npos) {
        return false;
      }
      text.replace(open + 1U, close - open - 1U, std::string(" ") + std::string(replacement) + " ");
      return true;
    }

    [[nodiscard]] bool EndsWith(const std::string_view text, const std::string_view suffix) noexcept
    {
      return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    /** The `_LR` twins of every world-particle technique with an ADD, ALPHABLEND or PREMODALPHA blend. */
    std::string BuildLowResolutionTechniques(const std::string& source, int& count)
    {
      std::string twins;
      constexpr std::string_view kKeyword = "technique ";
      std::size_t at = 0U;
      while ((at = source.find(kKeyword, at)) != std::string::npos) {
        const bool lineStart = at == 0U || source[at - 1U] == '\n';
        std::size_t nameBegin = at + kKeyword.size();
        const std::size_t nameEnd = source.find_first_of(" \t\r\n{", nameBegin);
        at = nameBegin;
        if (!lineStart || nameEnd == std::string::npos) {
          continue;
        }
        const std::string name = source.substr(nameBegin, nameEnd - nameBegin);
        const std::size_t open = source.find('{', nameEnd);
        const std::size_t close = (open != std::string::npos) ? MatchingBrace(source, open) : std::string::npos;
        if (close == std::string::npos) {
          continue;
        }
        at = close;
        const bool add = EndsWith(name, "_ADD");
        const bool alpha = EndsWith(name, "_ALPHABLEND");
        const bool premod = EndsWith(name, "_PREMODALPHA");
        if (!(add || alpha || premod) || name.rfind("TRamp", 0U) != 0U) {
          continue;
        }
        std::string body = source.substr(open, close - open + 1U);
        if (body.find("WorldPS()") == std::string::npos ||
            (body.find("WorldVS(") == std::string::npos && body.find("WorldVSAlign(") == std::string::npos)) {
          continue;
        }
        if (!add && !ReplaceCallArgument(body, "AlphaState", alpha ? "LR_Accumulate_Alpha" : "LR_Accumulate_Premod")) {
          continue;
        }
        if (!ReplaceCallArgument(body, "DepthState", "Depth_Disable_Write_None")) {
          continue;
        }
        const std::size_t shader = body.find("WorldPS()");
        body.replace(shader, 9U, "WorldPSLR()");
        twins += "\ntechnique " + name + "_LR\n" + body + "\n";
        ++count;
      }
      return twins;
    }
  } // namespace

  namespace
  {
    bool sPatched = false;
  }

  void SetParticleEffectPatched(const bool patched) noexcept
  {
    sPatched = patched;
  }

  bool ParticleEffectPatched() noexcept
  {
    return sPatched;
  }

  ParticleEffectPatchResult PatchParticleEffect(std::string& source)
  {
    ParticleEffectPatchResult result;

    constexpr std::string_view kStruct = "struct WORLD_OUTPUT";
    constexpr std::string_view kBillboard = "// Billboard the quads.";
    constexpr std::string_view kViewSpace = "pos = mul( float4(pos, 1), ViewMatrix );";
    const std::size_t structAt = source.find(kStruct);
    if (structAt == std::string::npos || source.find("WORLD_OUTPUT WorldVS(") == std::string::npos ||
        source.find("WORLD_OUTPUT WorldVSAlign(") == std::string::npos || source.find("PS_OUTPUT WorldPS(") == std::string::npos) {
      result.error = "WORLD_OUTPUT, WorldVS, WorldVSAlign or WorldPS not found";
      return result;
    }
    if (CountOf(source, kBillboard) != 1U || CountOf(source, kViewSpace) != 1U || source.find("float4x4 Projection;") == std::string::npos) {
      result.error = "the vertex shader anchors are not as expected";
      return result;
    }
    if (source.find("LRParams") != std::string::npos) {
      result.error = "already patched";
      return result;
    }

    std::string patched = source;
    // The vertex shader hooks first (they are found by text that the insertions below do not touch).
    {
      const std::size_t at = patched.find(kBillboard);
      patched.insert(
        at,
        "if( LRParams.x != 0 && LRCull(Size.x + (Size.y * t), mul(float4(pos, 1), WorldToProjection).w) )\n"
        "\t\t\treturn Out;\n\n\t\t"
      );
    }
    {
      const std::size_t at = patched.find(kViewSpace);
      patched.insert(
        at + kViewSpace.size(),
        "\n\t\tif( LRParams.x != 0 && LRCull(Size.x + (Size.y * t), pos.z) )\n\t\t\treturn Out;\n"
      );
    }
    {
      const std::size_t at = patched.find(kStruct);
      patched.insert(at, std::string(kDeclarations));
    }

    int twins = 0;
    const std::string lowResolution = BuildLowResolutionTechniques(patched, twins);
    if (twins == 0) {
      result.error = "no ADD / ALPHABLEND / PREMODALPHA world-particle technique found";
      return result;
    }

    std::string shaders(kShaders);
    const std::string downsample = DownsampleShader(2) + "\n" + DownsampleShader(4);
    const std::size_t marker = shaders.find("@DOWNSAMPLE@");
    shaders.replace(marker, 12U, downsample);

    patched += shaders;
    patched += lowResolution;

    source = std::move(patched);
    result.patched = true;
    result.techniques = twins;
    return result;
  }
} // namespace moho
