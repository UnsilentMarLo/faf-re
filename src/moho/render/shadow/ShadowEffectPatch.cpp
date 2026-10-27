#include "moho/render/shadow/ShadowEffectPatch.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace moho::shadowfilter
{
  namespace
  {
    using std::size_t;
    using std::string;
    using std::string_view;

    constexpr size_t npos = string::npos;

    // The lookup both effects share. Tokens: @FILTER@ / @FILTER2@ (the parameters), @SIZE@ (shadow map size in texels), @BIAS@ (the effect's
    // constant bias, in depth units), @TAPS@ (tap count expression), @FETCH@ (one point sample of the depth in the shadow map's red
    // channel at `uv`).
    //
    // The shadow map holds the light-space depth of every caster in R (cleared to 1 = nothing in the way) and 0 in G wherever a caster
    // was drawn. A lookup compares the receiver's depth with the casters' depth at several points of a rotated Vogel disk (equal-area
    // spiral, golden angle). Each tap compares against the receiver depth moved along the receiver's plane (derived from the screen
    // space derivatives of the shadow coordinates), so a wide kernel on a slope does not shadow its own surface.
    // With a penumbra scale a first pass over the disk averages the depth of the casters in front of the receiver; the kernel radius then
    // grows with the receiver-to-caster distance (percentage-closer soft shadows): contact shadows stay sharp, far ones soften.
    constexpr const char* kLookupCode = R"HLSL(
// ---- FAF shadow filter (ShadowEffectPatch): filtered shadow-map lookup, see docs/SHADOW_FILTERING.md ----
float4 @FILTER@;    // x taps (0 = shipped lookup), y radius (texels), z largest radius (texels), w penumbra scale (uv per depth unit)
float4 @FILTER2@;   // x constant bias scale, y receiver-plane bias scale

float ShadowFilterFetch(float2 uv)
{
    return @FETCH@;
}

// Point i of n on a unit disk, rotated by `rot` (cos, sin).
float2 ShadowFilterDisk(float i, float n, float2 rot)
{
    float r = sqrt((i + 0.5) / n);
    float a = i * 2.39996323;
    float2 d = r * float2(cos(a), sin(a));
    return float2(d.x * rot.x - d.y * rot.y, d.x * rot.y + d.y * rot.x);
}

// The lit fraction (1 = sun, 0 = shadow) at a shadow coordinate: xy = uv in the shadow map, z = depth from the light. Needs screen space
// derivatives, so it must be reached by uniform control flow.
float ShadowFilterLookup(float4 coords)
{
    float2 uv = coords.xy;
    float z = coords.z;

    // receiver plane: dz/duv from the screen space derivatives of uv and z
    float2 duvdx = ddx(uv);
    float2 duvdy = ddy(uv);
    float2 dzdp = float2(ddx(z), ddy(z));
    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    float2 slope = float2(dzdp.x * duvdy.y - dzdp.y * duvdx.y, dzdp.y * duvdx.x - dzdp.x * duvdy.x);
    slope = (abs(det) > 1e-10) ? slope / det : float2(0, 0);
    slope = clamp(slope, float2(-4, -4), float2(4, 4)) * @FILTER2@.y;

    float texel = 1.0 / @SIZE@;
    float bias = @FILTER2@.x * (@BIAS@);

    // a per-texel rotation of the disk (interleaved gradient noise on the shadow map texel: fixed in the world, no shimmer when the camera moves)
    float angle = 6.2831853 * frac(52.9829189 * frac(dot(floor(uv * @SIZE@), float2(0.06711056, 0.00583715))));
    float2 rot = float2(cos(angle), sin(angle));

    float radius = @FILTER@.y;
    if (@FILTER@.w > 0.0)
    {
        // blocker search: the average depth of the casters in front of the receiver within the largest radius
        float blockerSum = 0.0;
        float blockers = 0.0;
        for (int j = 0; j < 8; ++j)
        {
            float2 o = ShadowFilterDisk((float)j, 8.0, rot) * (@FILTER@.z * texel);
            float depth = ShadowFilterFetch(uv + o);
            if (depth + bias < z + dot(slope, o))
            {
                blockerSum += depth;
                blockers += 1.0;
            }
        }
        if (blockers < 0.5)
        {
            return 1.0;
        }
        radius = clamp((z - blockerSum / blockers) * @FILTER@.w / texel, @FILTER@.y, @FILTER@.z);
    }

    int taps = @TAPS@;
    float lit = 0.0;
    float weights = 0.0;
    for (int i = 0; i < taps; ++i)
    {
        float2 d = ShadowFilterDisk((float)i, (float)taps, rot);
        float2 o = d * (radius * texel);
        float depth = ShadowFilterFetch(uv + o);
        float w = 1.0 - 0.5 * dot(d, d);   // the rim counts less: a smoother falloff than a flat disk
        lit += (depth + bias >= z + dot(slope, o)) ? w : 0.0;
        weights += w;
    }
    return lit / weights;
}
)HLSL";

    string Replace(string text, const string_view token, const string_view value)
    {
      for (size_t p = text.find(token); p != npos; p = text.find(token, p + value.size())) {
        text.replace(p, token.size(), value);
      }
      return text;
    }

    string BuildLookup(
      const string_view filter, const string_view filter2, const string_view size, const string_view bias, const string_view sampler,
      const bool pointFromLinear
    )
    {
      string code = kLookupCode;
      code = Replace(code, "@FILTER2@", filter2);
      code = Replace(code, "@FILTER@", filter);
      code = Replace(code, "@SIZE@", size);
      code = Replace(code, "@BIAS@", bias);
      code = Replace(code, "@TAPS@", "(int)" + string(filter) + ".x");
      // a gradient fetch (tex2D) must not sit inside a dynamic loop
      // A linear sampler reads exactly one texel at the texel's centre: terrain.fx has no sampler to spare (a pass of it already uses all 16
      // that shader model 4.1 allows), so its lookups go through the shipped bilinear ShadowSampler at texel centres.
      const string at = pointFromLinear ? "(floor(uv * " + string(size) + ") + 0.5) / " + string(size) : string("uv");
      const string fetch = "tex2Dlod(" + string(sampler) + ", float4(" + at + ", 0, 0)).r";
      code = Replace(code, "@FETCH@", fetch);
      return code;
    }

    size_t CountOccurrences(const string& s, const string_view needle)
    {
      size_t n = 0;
      for (size_t p = s.find(needle); p != npos; p = s.find(needle, p + needle.size())) {
        ++n;
      }
      return n;
    }

    // The effect files are CRLF on disk; every anchor below is written with LF.
    void NormaliseLineEnds(string& s)
    {
      if (s.find('\r') == npos) {
        return;
      }
      string out;
      out.reserve(s.size());
      for (const char c : s) {
        if (c != '\r') {
          out.push_back(c);
        }
      }
      s = std::move(out);
    }

    // The index of the ')' that closes the '(' at `open`, or npos.
    size_t MatchParen(const string& s, const size_t open)
    {
      int depth = 0;
      for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == '(') {
          ++depth;
        } else if (s[i] == ')' && --depth == 0) {
          return i;
        }
      }
      return npos;
    }

    string Trim(const string& s)
    {
      size_t a = 0;
      size_t b = s.size();
      while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
      }
      while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        --b;
      }
      return s.substr(a, b - a);
    }
  } // namespace

  bool HandlesEffect(const std::string& baseName)
  {
    return baseName == "mesh" || baseName == "terrain";
  }

  PatchResult PatchMeshEffect(std::string& source)
  {
    PatchResult result;
    string s = source;
    NormaliseLineEnds(s);

    if (s.find("shadowFilter") != npos) {
      result.error = "mesh.fx: already patched";
      return result;
    }

    // the lookup goes in front of the first shadow function: the shadow samplers, shadowSize and shadowBias are declared above it
    const string anchor = "// *** Standard Shadow Mapping ***\nfloat ComputeShadowStandard( float4 shadowCoords)";
    if (CountOccurrences(s, anchor) != 1 || s.find("sampler2D shadowSampler") == npos || s.find("float       shadowBias;") == npos) {
      result.error = "mesh.fx: the shadow functions moved";
      return result;
    }
    // ComputeShadow: the filtered lookup replaces the shipped one for the high-fidelity techniques
    const string hook =
      "        return shadowBlur ? (shadowsEnabled ? ComputeShadowPCF( shadowCoords) : 1.0f) : ComputeShadowStandard( shadowCoords);";
    if (CountOccurrences(s, hook) != 1) {
      result.error = "mesh.fx: ComputeShadow changed";
      return result;
    }
    // one return per function (the Direct3D 9 compiler, which this text must stay acceptable to, rejects early returns)
    const string filtered =
      "        float fafShadow;   // FAF shadow filter\n"
      "        if (shadowFilter.x > 0.5)\n"
      "        {\n"
      "            fafShadow = 1.0f;\n"
      "            if (shadowsEnabled)\n"
      "            {\n"
      "                fafShadow = ComputeShadowFiltered( shadowCoords);\n"
      "            }\n"
      "        }\n"
      "        else\n"
      "        {\n"
      "            fafShadow = shadowBlur ? (shadowsEnabled ? ComputeShadowPCF( shadowCoords) : 1.0f) : ComputeShadowStandard( shadowCoords);\n"
      "        }\n"
      "        return fafShadow;\n";
    s.replace(s.find(hook), hook.size(), filtered);

    string code = BuildLookup("shadowFilter", "shadowFilter2", "shadowSize", "shadowBias + 0.001", "shadowSampler", false);
    code +=
      "\nfloat ComputeShadowFiltered( float4 shadowCoords)\n"
      "{\n"
      "    shadowCoords.xy /= shadowCoords.w;\n"
      "    return ShadowFilterLookup(shadowCoords);\n"
      "}\n\n";
    s.insert(s.find(anchor), code);
    result.lookups = 1;

    source = std::move(s);
    result.patched = true;
    return result;
  }

  PatchResult PatchTerrainEffect(std::string& source)
  {
    PatchResult result;
    string s = source;
    NormaliseLineEnds(s);

    if (s.find("ShadowFilter") != npos) {
      result.error = "terrain.fx: already patched";
      return result;
    }

    const string anchor = "float ComputeShadow( float4 vShadowCoord )";
    if (CountOccurrences(s, anchor) != 1 || s.find("float ShadowSize;") == npos || s.find("texture ShadowTexture;") == npos ||
        s.find("sampler2D ShadowSampler") == npos) {
      result.error = "terrain.fx: the shadow functions moved";
      return result;
    }

    // every `tex2D(ShadowSampler, <coords>).g` lookup goes through ShadowLookup (done before the lookup code is inserted, which has one itself)
    const string needle = "tex2D";
    int replaced = 0;
    for (size_t p = s.find(needle); p != npos; p = s.find(needle, p)) {
      size_t open = p + needle.size();
      while (open < s.size() && s[open] == ' ') {
        ++open;
      }
      if (open >= s.size() || s[open] != '(') {
        p += needle.size();
        continue;
      }
      const size_t close = MatchParen(s, open);
      if (close == npos || s.compare(close + 1, 2, ".g") != 0) {
        p += needle.size();
        continue;
      }
      const string inside = s.substr(open + 1, close - open - 1);
      const size_t comma = inside.find(',');
      if (comma == npos || Trim(inside.substr(0, comma)) != "ShadowSampler") {
        p += needle.size();
        continue;
      }
      string coords = Trim(inside.substr(comma + 1));
      if (coords.size() > 3 && coords.compare(coords.size() - 3, 3, ".xy") == 0) {
        coords.erase(coords.size() - 3); // mShadow.xy -> mShadow: the lookup wants the depth too
      }
      const string replacement = "ShadowLookup(" + coords + ")";
      s.replace(p, close + 3 - p, replacement);
      p += replacement.size();
      ++replaced;
    }
    if (replaced < 5) {
      result.error = "terrain.fx: only " + std::to_string(replaced) + " shadow lookups found";
      return result;
    }
    result.lookups = replaced;

    string code = BuildLookup("ShadowFilter", "ShadowFilter2", "ShadowSize", "0.003", "ShadowSampler", true);
    code += R"HLSL(
// The sun term at a shadow coordinate (xy = uv, z = depth from the light): the filtered lookup, or the shipped one (the blurred G channel).
float ShadowLookup(float4 coords)
{
    float sun;
    if (@COND@)
    {
        sun = ShadowFilterLookup(coords);
    }
    else
    {
        sun = tex2D( ShadowSampler, coords.xy ).g;
    }
    return sun;
}

)HLSL";
    code = Replace(code, "@COND@", "ShadowFilter.x > 0.5 && ShadowsEnabled == 1");
    s.insert(s.find(anchor), code);

    source = std::move(s);
    result.patched = true;
    return result;
  }
} // namespace moho::shadowfilter
