#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace gpg::gal
{
  /**
   * FAF addition, not in the shipped binary.
   *
   * What it does:
   * The post-process anti-aliasing modes `Device::ApplyPostProcessAA` takes. The
   * flip-model swap chains of Direct3D 11 and 12 cannot be multisampled, so
   * these passes filter the finished 3D world in the back buffer instead (the
   * UI is drawn afterwards and stays sharp).
   */
  enum PostProcessAAMode : std::uint32_t
  {
    kPostProcessAAOff = 0,
    kPostProcessAAFxaa = 1,
    kPostProcessAASmaa = 2,
  };
}

namespace gpg::gal::backend
{
  /**
   * HLSL shared by the Direct3D 11 (FXC, `ps_4_0`) and Direct3D 12 (DXC) post-process passes: one full-screen
   * triangle vertex shader `AaVS` and the FXAA 3.11 quality pixel shader `FxaaPS` (luma computed from the colour,
   * 12 search steps, sub-pixel quality 0.75, edge threshold 1/6, minimum threshold 1/12). Texture register t0, sampler
   * s0 (linear clamp); the texel size comes from `GetDimensions`, so the pass needs no constant buffer.
   */
  inline constexpr char kPostProcessAAFxaaSource[] = R"hlsl(
struct AaVertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
AaVertex AaVS(uint id : SV_VertexID)
{
    AaVertex o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}
Texture2D faf_source : register(t0);
SamplerState faf_linear : register(s0);

float AaLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
#define AA_TAP(uv) faf_source.SampleLevel(faf_linear, uv, 0).rgb
#define AA_OFF(uv, x, y) AaLuma(faf_source.SampleLevel(faf_linear, uv, 0, int2(x, y)).rgb)

float4 FxaaPS(AaVertex v) : SV_Target0
{
    const float kSubpix = 0.75;
    const float kEdgeThreshold = 0.166;
    const float kEdgeThresholdMin = 0.0833;
    const float kSteps[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

    uint width, height;
    faf_source.GetDimensions(width, height);
    float2 rcpFrame = 1.0 / float2(width, height);
    float2 pos = v.uv;

    float4 centre = faf_source.SampleLevel(faf_linear, pos, 0);
#ifdef FAF_AA_SPLIT
    if (v.position.x < 0.5 * width)
        return centre;
#endif
    float lumaM = AaLuma(centre.rgb);
    float lumaS = AA_OFF(pos, 0, 1);
    float lumaE = AA_OFF(pos, 1, 0);
    float lumaN = AA_OFF(pos, 0, -1);
    float lumaW = AA_OFF(pos, -1, 0);

    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(kEdgeThresholdMin, rangeMax * kEdgeThreshold))
    {
#ifdef FAF_AA_DIFF
        return float4(0, 0, 0, 1);
#else
        return centre;
#endif
    }

    float lumaNW = AA_OFF(pos, -1, -1);
    float lumaSE = AA_OFF(pos, 1, 1);
    float lumaNE = AA_OFF(pos, 1, -1);
    float lumaSW = AA_OFF(pos, -1, 1);

    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = (-2.0 * lumaM) + lumaNS;
    float edgeVert1 = (-2.0 * lumaM) + lumaWE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
    float edgeVert2 = (-2.0 * lumaN) + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
    float edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
    float edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
    float edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    float edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    bool horzSpan = edgeHorz >= edgeVert;
    float lengthSign = horzSpan ? rcpFrame.y : rcpFrame.x;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    float subpixB = (subpixA * (1.0 / 12.0)) - lumaM;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    float lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);

    float2 posB = pos;
    float2 offNP = horzSpan ? float2(rcpFrame.x, 0.0) : float2(0.0, rcpFrame.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;

    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    bool lumaMLTZero = lumaMM < 0.0;

    float2 posN = posB;
    float2 posP = posB;
    float lumaEndN = 0.0;
    float lumaEndP = 0.0;
    bool doneN = false;
    bool doneP = false;
    [loop] for (int i = 0; i < 12; i++)
    {
        if (!doneN)
        {
            posN -= offNP * kSteps[i];
            lumaEndN = AaLuma(AA_TAP(posN)) - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP)
        {
            posP += offNP * kSteps[i];
            lumaEndP = AaLuma(AA_TAP(posP)) - lumaNN * 0.5;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
        if (doneN && doneP) break;
    }

    float dstN = horzSpan ? (pos.x - posN.x) : (pos.y - posN.y);
    float dstP = horzSpan ? (posP.x - pos.x) : (posP.y - pos.y);
    bool directionN = dstN < dstP;
    float lumaEndG = directionN ? lumaEndN : lumaEndP;
    bool goodSpan = (lumaEndG < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    float dstMin = directionN ? dstN : dstP;
    float pixelOffset = dstMin * (-1.0 / spanLength) + 0.5;
    float subpixF = (-2.0 * subpixC + 3.0) * (subpixC * subpixC);
    float subpixH = subpixF * subpixF * kSubpix;
    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);

    float2 posM = pos;
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    float3 filtered = AA_TAP(posM);
#ifdef FAF_AA_DIFF
    return float4(saturate(abs(filtered - centre.rgb) * 4.0), 1);
#else
    return float4(filtered, centre.a);
#endif
}
)hlsl";

  /**
   * The source a backend compiles. FAF_AA_SPLIT=1 (diagnostics) leaves the left half of the screen unfiltered, so one frame
   * holds the before and after of a scene; FAF_AA_SPLIT=2 draws the amplified difference the filter makes (4x), which shows
   * which pixels it touches.
   */
  [[nodiscard]] inline std::string PostProcessAASource()
  {
    const char* const split = std::getenv("FAF_AA_SPLIT");
    std::string source;
    if (split != nullptr && split[0] == '1') {
      source = "#define FAF_AA_SPLIT 1\n";
    } else if (split != nullptr && split[0] == '2') {
      source = "#define FAF_AA_DIFF 1\n";
    }
    source += kPostProcessAAFxaaSource;
    return source;
  }
}
