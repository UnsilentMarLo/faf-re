#include "moho/render/lights/LightEffectPatch.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace moho::lights
{
  namespace
  {
    using std::size_t;
    using std::string;
    using std::string_view;

    constexpr size_t npos = string::npos;

    // ------------------------------------------------------------------------------------------------ HLSL (effect dialect)

    // Declarations and the grid walk both effects share. Positions are absolute world units. The data layout is LightGrid.h's.
    constexpr const char* kCommonDecl = R"HLSL(
// ---- FAF dynamic lights (LightEffectPatch): light data and light grid textures, see docs/DYNAMIC_LIGHTS.md ----
texture dynLightData;
texture dynLightGrid;
sampler2D dynLightDataSampler = sampler_state
{
    Texture   = (dynLightData);
    MipFilter = NONE;
    MinFilter = POINT;
    MagFilter = POINT;
    AddressU  = CLAMP;
    AddressV  = CLAMP;
};
sampler2D dynLightGridSampler = sampler_state
{
    Texture   = (dynLightGrid);
    MipFilter = NONE;
    MinFilter = POINT;
    MagFilter = POINT;
    AddressU  = CLAMP;
    AddressV  = CLAMP;
};
float4 dynLightParams;     // gridOriginX, gridOriginZ, 1 / cellSize, lightCount
float4 dynLightGridSize;   // cellsX, cellsZ, 1 / (2 cellsX), 1 / cellsZ
float4 dynLightDataSize;   // 1 / dataWidth, 1 / dataRows, dataWidth, dataRows

float4 DynLightTexel(float k)
{
    float row = floor(k * dynLightDataSize.x);
    float col = k - row * dynLightDataSize.z;
    return tex2Dlod(dynLightDataSampler, float4((col + 0.5) * dynLightDataSize.x, (row + 0.5) * dynLightDataSize.y, 0, 0));
}

// The up to 8 entries (light index + 1, 0 = none) of the grid cell that holds the world position; all 0 outside the map.
void DynLightCell(float3 worldPos, out float4 e0, out float4 e1, out float4 e2, out float4 e3)
{
    e0 = float4(0, 0, 0, 0);
    e1 = float4(0, 0, 0, 0);
    e2 = float4(0, 0, 0, 0);
    e3 = float4(0, 0, 0, 0);
    float2 cell = floor((worldPos.xz - dynLightParams.xy) * dynLightParams.z);
    if (cell.x >= 0 && cell.y >= 0 && cell.x < dynLightGridSize.x && cell.y < dynLightGridSize.y && dynLightParams.w > 0.5)
    {
        float v = (cell.y + 0.5) * dynLightGridSize.w;
        float u = cell.x * 2.0 + 0.5;
        e0 = tex2Dlod(dynLightGridSampler, float4(u * dynLightGridSize.z, v, 0, 0));
        e1 = tex2Dlod(dynLightGridSampler, float4((u + 1.0) * dynLightGridSize.z, v, 0, 0));
    }
}

// One light as the pixel sees it: closest point of the light's segment (a point light has a zero-length one; a spotlight is a point
// light with a cone), windowed falloff. Returns the attenuation; `toLight` is the vector from the pixel to that point, `colour` the
// light's colour x intensity.
float DynLightAt(float entry, float3 worldPos, out float3 toLight, out float3 colour)
{
    float k = (entry - 1.0) * 3.0;
    float4 a = DynLightTexel(k);
    float4 c = DynLightTexel(k + 2.0);
    bool spot = a.w < 0.0;   // a spotlight is stored with a negative radius
    float r = max(abs(a.w), 0.001);
    float3 seg = spot ? float3(0, 0, 0) : c.xyz - a.xyz;
    float t = saturate(dot(worldPos - a.xyz, seg) / max(dot(seg, seg), 0.0001));
    toLight = a.xyz + seg * t - worldPos;
    float atten = saturate(1 - dot(toLight, toLight) / (r * r));
    atten *= 0.35 + 0.65 * atten;   // softer tail than atten^2: a light just above the ground must still reach the ground around it
    if (spot)
    {
        float cd = dot(-normalize(toLight), c.xyz);
        atten *= saturate((cd - c.w) / max((1.0 - c.w) * 0.5, 0.0001));
    }
    colour = float3(0, 0, 0);
    if (atten > 0.002)
    {
        colour = DynLightTexel(k + 1.0).rgb;   // fetched only by the lights that reach this pixel
    }
    return atten;
}
)HLSL";

    constexpr const char* kMeshLightCode = R"HLSL(
float4 dynLightCamera;   // world position of the camera (set per view)

void DynLightsBRDFOne(float entry, float3 worldPos, float3 n, float3 v, float nDotV, float3 F0, float3 albedo, float metallic, float roughness, inout float3 total)
{
    float3 d;
    float3 lightColour;
    float atten = DynLightAt(entry, worldPos, d, lightColour);
    if (atten > 0.002)   // the cell is bigger than the light: most entries do not reach this pixel, and the BRDF is the expensive part
    {
        float3 l = normalize(d);
        float3 h = normalize(v + l);
        float nDotL = max(dot(n, l), 0.0);
        float3 F = FresnelSchlick(max(dot(h, v), 0.0), F0);
        float NDF = NormalDistribution(n, h, roughness);
        float G = GeometrySmith(n, nDotV, l, roughness);
        float3 reflected = PI * NDF * G * F / (4.0 * nDotV * nDotL + 0.0001);
        float3 refracted = (float3(1, 1, 1) - F) * (1.0 - metallic) * albedo;
        float wrapL = saturate((dot(n, l) + 0.35) / 1.35);   // wrapped diffuse: lights next to a surface still light it
        total += (refracted * wrapL + reflected * nDotL) * lightColour * atten;
    }
}

void DynLightsBRDFQuad(float4 e, float3 worldPos, float3 n, float3 v, float nDotV, float3 F0, float3 albedo, float metallic, float roughness, inout float3 total)
{
    if (e.x > 0.5) { DynLightsBRDFOne(e.x, worldPos, n, v, nDotV, F0, albedo, metallic, roughness, total); }
    if (e.y > 0.5) { DynLightsBRDFOne(e.y, worldPos, n, v, nDotV, F0, albedo, metallic, roughness, total); }
    if (e.z > 0.5) { DynLightsBRDFOne(e.z, worldPos, n, v, nDotV, F0, albedo, metallic, roughness, total); }
    if (e.w > 0.5) { DynLightsBRDFOne(e.w, worldPos, n, v, nDotV, F0, albedo, metallic, roughness, total); }
}

float3 DynLightsBRDF(float3 worldPos, float3 n, float3 v, float3 albedo, float metallic, float roughness)
{
    float4 e0, e1, e2, e3;
    DynLightCell(worldPos, e0, e1, e2, e3);
    float nDotV = abs(dot(n, v)) + 0.001;
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 total = float3(0, 0, 0);
    // the entries of a cell are packed at the front, strongest first: a quad is looked at only when the one before it was full
    DynLightsBRDFQuad(e0, worldPos, n, v, nDotV, F0, albedo, metallic, roughness, total);
    if (e0.w > 0.5)
    {
        DynLightsBRDFQuad(e1, worldPos, n, v, nDotV, F0, albedo, metallic, roughness, total);
        if (e1.w > 0.5)
        {
            DynLightsBRDFQuad(e2, worldPos, n, v, nDotV, F0, albedo, metallic, roughness, total);
            if (e2.w > 0.5)
            {
                DynLightsBRDFQuad(e3, worldPos, n, v, nDotV, F0, albedo, metallic, roughness, total);
            }
        }
    }
    return total / (1.0 + 0.5 * total);   // soft limit: many overlapping lights must not white out the unit
}

// tangent-space meshes (units, normal-mapped props). The result is masked for the water-mirrored pass instead of clip()ped.
float4 DynLightsPS(NORMALMAPPED_VERTEX vertex) : COLOR0
{
    float3x3 rotationMatrix = float3x3(vertex.binormal, vertex.tangent, vertex.normal);
    float3 n = ComputeNormal(normalsSampler, vertex.texcoord0.zw, rotationMatrix);
    float4 albedoTex = tex2D(albedoSampler, vertex.texcoord0.xy);
    float4 specular = tex2D(specularSampler, vertex.texcoord0.xy);
    float3 albedo = lerp(albedoTex.rgb, vertex.color.rgb * 0.6, saturate(specular.a * 2.5));
    float3 v = normalize(vertex.viewDirection);
    float3 lit = float3(0, 0, 0);
    if (mirrored < 0.5)   // the water reflection pass draws the mesh again: it gets no lights (the result was masked away, now it is not even computed)
    {
        lit = DynLightsBRDF(dynLightCamera.xyz - v * vertex.depth.y, n, v, albedo, saturate(specular.r), saturate(0.8 - specular.g * 0.5));
    }
    return float4(lit, albedoTex.a);   // alpha: the alpha test of the technique still applies
}

// vertex-normal props (trees, rocks, ...): no tangent frame, no specular map
float4 DynLightsVertexNormalPS(VERTEXNORMAL_VERTEX vertex) : COLOR0
{
    float4 albedoTex = tex2D(albedoSampler, vertex.texcoord0.xy);
    float3 v = normalize(vertex.viewDirection);
    float3 lit = float3(0, 0, 0);
    if (mirrored < 0.5)
    {
        lit = DynLightsBRDF(dynLightCamera.xyz - v * vertex.texcoord0.z, normalize(vertex.normal), v, albedoTex.rgb, 0.0, 0.8);
    }
    return float4(lit, albedoTex.a);
}
)HLSL";

    // Terrain: the light term at a world position with a normal, shared by the composite light pass and CalculateLighting.
    constexpr const char* kTerrainLightFunction = R"HLSL(
void DynLightTerrainOne(float entry, float3 worldPos, float3 normal, inout float3 total)
{
    float3 d;
    float3 lightColour;
    float atten = DynLightAt(entry, worldPos, d, lightColour);
    if (atten > 0.002)
    {
        total += lightColour * (atten * saturate((dot(normal, normalize(d)) + 0.5) / 1.5));   // wrapped: a light just above the ground lights it
    }
}

void DynLightTerrainQuad(float4 e, float3 worldPos, float3 normal, inout float3 total)
{
    if (e.x > 0.5) { DynLightTerrainOne(e.x, worldPos, normal, total); }
    if (e.y > 0.5) { DynLightTerrainOne(e.y, worldPos, normal, total); }
    if (e.z > 0.5) { DynLightTerrainOne(e.z, worldPos, normal, total); }
    if (e.w > 0.5) { DynLightTerrainOne(e.w, worldPos, normal, total); }
}

// All the lights of the cell (the composite light pass).
float3 DynLightTerrain(float3 worldPos, float3 normal)
{
    float4 e0, e1, e2, e3;
    DynLightCell(worldPos, e0, e1, e2, e3);
    float3 total = float3(0, 0, 0);
    DynLightTerrainQuad(e0, worldPos, normal, total);
    if (e0.w > 0.5)
    {
        DynLightTerrainQuad(e1, worldPos, normal, total);
        if (e1.w > 0.5)
        {
            DynLightTerrainQuad(e2, worldPos, normal, total);
            if (e2.w > 0.5)
            {
                DynLightTerrainQuad(e3, worldPos, normal, total);
            }
        }
    }
    return total;
}

// The eight strongest lights of the cell: every decal and splat layer lights itself through CalculateLighting, so this one runs several
// times per pixel where the composite pass runs once.
float3 DynLightTerrainFew(float3 worldPos, float3 normal)
{
    float4 e0, e1, e2, e3;
    DynLightCell(worldPos, e0, e1, e2, e3);
    float3 total = float3(0, 0, 0);
    DynLightTerrainQuad(e0, worldPos, normal, total);
    if (e0.w > 0.5)
    {
        DynLightTerrainQuad(e1, worldPos, normal, total);
    }
    return total;
}
)HLSL";

    constexpr const char* kTerrainPassCode = R"HLSL(
// FAF dynamic lights: an extra pass after the terrain composite multiplies the lit terrain colour by (1 + light), using the terrain
// normals buffer and the interpolated world position (decals and splats add the same lights inside CalculateLighting).
float4 TerrainLightsPS( VS_OUTPUT inV ) : COLOR
{
    float3 normal = normalize(SampleScreen(NormalSampler, inV.mTexSS).xyz * 2 - 1);
    float3 total = DynLightTerrain(inV.mTexWT.xzy, normal);   // mTexWT = (x, z, height)
    return float4(total / (1.0 + 1.0 * total), 0);   // soft limit; the pass multiplies the terrain by (1 + light)
}

)HLSL";

    constexpr const char* kTerrainPass = R"HLSL(
    pass PDynLight
    {
        // FAF dynamic lights: dest * (src + 1), colour only (the alpha channel carries the specular/glow mask)
        AlphaBlendEnable = true;
        ColorWriteEnable = 0x07;
        SrcBlend = DestColor;
        DestBlend = One;
        ZEnable = true;
        ZFunc = LessEqual;
        ZWriteEnable = false;

        VertexShader = compile vs_1_1 TerrainVS(true);
        PixelShader = compile ps_3_0 TerrainLightsPS();
    }
)HLSL";

    // ------------------------------------------------------------------------------------------------ text helpers

    bool IsIdentChar(const char c)
    {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    bool IsSpace(const char c)
    {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    size_t SkipSpace(const string& s, size_t i)
    {
      while (i < s.size() && IsSpace(s[i])) {
        ++i;
      }
      return i;
    }

    size_t SkipInlineSpace(const string& s, size_t i)
    {
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
      }
      return i;
    }

    // `needle` at the start of a line (position 0 or after '\n') from `from`; npos if none.
    size_t FindAtLineStart(const string& s, const string_view needle, size_t from)
    {
      for (;;) {
        const size_t p = s.find(needle, from);
        if (p == npos) {
          return npos;
        }
        if (p == 0 || s[p - 1] == '\n') {
          return p;
        }
        from = p + 1;
      }
    }

    size_t CountOccurrences(const string& s, const string_view needle)
    {
      size_t n = 0;
      for (size_t p = s.find(needle); p != npos; p = s.find(needle, p + needle.size())) {
        ++n;
      }
      return n;
    }

    // The end of a function that starts at `start`: the first "\n}\n" after it (the engine's FX style closes functions at column 0).
    size_t FunctionEnd(const string& s, const size_t start)
    {
      const size_t p = s.find("\n}\n", start);
      return p == npos ? npos : p + 3;
    }

    // Parses `<key> = compile <profile> <name>(<args>);` anywhere in `block`; false if absent or oddly shaped.
    struct ShaderCall
    {
      string profile;
      string call; // name(args)
      string name;
    };

    bool ParseShaderAssignment(const string& block, const string_view key, ShaderCall& out)
    {
      for (size_t p = block.find(key); p != npos; p = block.find(key, p + 1)) {
        if (p > 0 && IsIdentChar(block[p - 1])) {
          continue;
        }
        size_t i = SkipSpace(block, p + key.size());
        if (i >= block.size() || block[i] != '=') {
          continue;
        }
        i = SkipSpace(block, i + 1);
        if (block.compare(i, 7, "compile") != 0) {
          continue;
        }
        i = SkipSpace(block, i + 7);
        size_t j = i;
        while (j < block.size() && IsIdentChar(block[j])) {
          ++j;
        }
        out.profile = block.substr(i, j - i);
        i = SkipSpace(block, j);
        j = i;
        while (j < block.size() && IsIdentChar(block[j])) {
          ++j;
        }
        out.name = block.substr(i, j - i);
        if (out.name.empty() || j >= block.size() || block[j] != '(') {
          return false;
        }
        const size_t close = block.find(')', j);
        if (close == npos) {
          return false;
        }
        out.call = block.substr(i, close + 1 - i);
        const size_t semi = SkipSpace(block, close + 1);
        return semi < block.size() && block[semi] == ';';
      }
      return false;
    }

    // The first line of `block` whose text (ignoring blanks) is `RasterizerState(...)`; the trimmed line, or empty.
    string FindRasterizerLine(const string& block)
    {
      size_t pos = 0;
      while (pos < block.size()) {
        size_t eol = block.find('\n', pos);
        if (eol == npos) {
          eol = block.size();
        }
        size_t a = SkipInlineSpace(block, pos);
        size_t b = eol;
        while (b > a && (block[b - 1] == '\r' || block[b - 1] == ' ' || block[b - 1] == '\t')) {
          --b;
        }
        if (block.compare(a, 15, "RasterizerState") == 0 && b > a && block[b - 1] == ')') {
          return block.substr(a, b - a);
        }
        pos = eol + 1;
      }
      return {};
    }

    // `#ifndef DIRECT3D10 / AlphaTestEnable = true; ... #endif` as written in the block, or empty.
    string FindAlphaTestBlock(const string& block)
    {
      for (size_t p = block.find("#ifndef DIRECT3D10"); p != npos; p = block.find("#ifndef DIRECT3D10", p + 1)) {
        size_t i = SkipSpace(block, p + 18);
        if (block.compare(i, 22, "AlphaTestEnable = true") == 0) {
          const size_t end = block.find("#endif", i);
          if (end == npos) {
            return {};
          }
          return block.substr(p, end + 6 - p);
        }
      }
      return {};
    }

    struct TechniqueSpan
    {
      string name;
      size_t begin; // "technique" keyword
      size_t end;   // start of the next technique (or the end of the text)
    };

    std::vector<TechniqueSpan> FindTechniques(const string& s)
    {
      std::vector<TechniqueSpan> spans;
      size_t p = FindAtLineStart(s, "technique", 0);
      while (p != npos) {
        size_t i = p + 9;
        if (i >= s.size() || !IsSpace(s[i])) {
          p = FindAtLineStart(s, "technique", p + 1);
          continue;
        }
        i = SkipSpace(s, i);
        size_t j = i;
        while (j < s.size() && IsIdentChar(s[j])) {
          ++j;
        }
        TechniqueSpan span;
        span.name = s.substr(i, j - i);
        span.begin = p;
        spans.push_back(span);
        p = FindAtLineStart(s, "technique", j);
        // techniques need whitespace after the keyword, so this skips the word "techniques" in comments at line start as well
      }
      for (size_t k = 0; k < spans.size(); ++k) {
        spans[k].end = k + 1 < spans.size() ? spans[k + 1].begin : s.size();
      }
      return spans;
    }

    // Puts `annotation` (one line, no newline) at the start of the technique's annotation block `< ... >`; false if it has none.
    bool AddAnnotation(string& block, const string_view annotation)
    {
      // technique Name [ws] <
      size_t i = 9;
      i = SkipSpace(block, i);
      while (i < block.size() && IsIdentChar(block[i])) {
        ++i;
      }
      i = SkipSpace(block, i);
      if (i >= block.size() || block[i] != '<') {
        return false;
      }
      block.insert(i + 1, string("\n    ") + string(annotation));
      return true;
    }

    bool ContainsTechnique(const char* const* names, const size_t count, const string& name)
    {
      for (size_t i = 0; i < count; ++i) {
        if (name == names[i]) {
          return true;
        }
        const string hf = string(names[i]) + "_HighFidelity";
        if (name == hf) {
          return true;
        }
      }
      return false;
    }

    // ------------------------------------------------------------------------------------------------ mesh.fx

    // Mesh techniques with a tangent frame that get the light pass (their name, with or without _HighFidelity), as make_lab_shaders.py.
    constexpr const char* kLitTangent[] = {
      "Unit",
      "Insect",
      "Aeon",
      "Seraphim",
      "AeonCZAR",
      "UEFNavy",
      "CybranNavy",
      "AeonNavy",
      "SeraphimNavy",
      "Metal",
      "NomadsUnit",
      "NormalMappedAlpha",
      "NormalMappedAlphaNoShadow",
      "UndulatingNormalMappedAlpha",
      "BloatingNormalMappedAlpha",
      "BlackenedNormalMappedAlpha",
      "NormalMappedGlow",
      "NormalMappedTerrain",
    };
    constexpr const char* kTangentVertexShaders[] = {
      "NormalMappedVS", "UnitFalloffVS", "UndulatingNormalMappedVS", "BloatingNormalMappedVS"
    };

    // Vertex shaders that return the lit structs: store the view distance (the pixel shader rebuilds the world position from it).
    bool PatchVertexShaders(string& s, int& patched, string& error)
    {
      static const string_view kTypes[] = {"NORMALMAPPED_VERTEX", "VERTEXNORMAL_VERTEX"};
      static const string_view kNormalize = "    vertex.viewDirection = normalize(vertex.viewDirection);";
      for (const string_view type : kTypes) {
        size_t p = FindAtLineStart(s, type, 0);
        while (p != npos) {
          // "<TYPE> <Name>(" at the start of a line
          size_t i = p + type.size();
          if (i < s.size() && s[i] == ' ') {
            size_t j = i + 1;
            while (j < s.size() && IsIdentChar(s[j])) {
              ++j;
            }
            if (j > i + 1 && j < s.size() && s[j] == '(') {
              const string name = s.substr(i + 1, j - i - 1);
              const size_t end = FunctionEnd(s, j);
              if (end == npos) {
                error = "mesh.fx: no end for vertex shader " + name;
                return false;
              }
              string body = s.substr(p, end - p);
              const size_t norm = body.find(kNormalize);
              if (norm != npos) {
                bool done = false;
                if (type == "NORMALMAPPED_VERTEX" && body.find("vertex.depth") != npos) {
                  body.insert(norm, "    vertex.depth.y = length(vertex.viewDirection); // FAF dynamic lights: view distance\n");
                  done = true;
                } else if (name == "VertexNormalVS") {
                  // texcoord0.zw is unused by the VertexNormal pixel shaders; the uv is assigned AFTER the view direction here,
                  // so keep the distance in a local and write .z after the uv line
                  body.insert(
                    norm,
                    "    float dynLightDist = length(vertex.viewDirection); // FAF dynamic lights: view distance\n"
                  );
                  const size_t uv = body.find("    vertex.texcoord0 = ");
                  const size_t eol = uv == npos ? npos : body.find('\n', uv);
                  if (eol == npos) {
                    error = "mesh.fx: VertexNormalVS has no texcoord0 line";
                    return false;
                  }
                  body.insert(eol + 1, "    vertex.texcoord0.z = dynLightDist;\n");
                  done = true;
                }
                if (done) {
                  s.replace(p, end - p, body);
                  ++patched;
                  p = FindAtLineStart(s, type, p + body.size());
                  continue;
                }
              }
              p = FindAtLineStart(s, type, end);
              continue;
            }
          }
          p = FindAtLineStart(s, type, p + 1);
        }
      }
      return true;
    }
  } // namespace

  PatchedEffect EffectForName(const std::string& baseName)
  {
    if (baseName == "mesh") {
      return PatchedEffect::Mesh;
    }
    if (baseName == "terrain") {
      return PatchedEffect::Terrain;
    }
    return PatchedEffect::None;
  }

  EffectPatchResult PatchMeshEffect(std::string& source, const int flood)
  {
    EffectPatchResult result;
    string s = source;

    if (s.find("dynLightData") != npos) {
      result.error = "mesh.fx: already patched";
      return result;
    }

    int vsPatched = 0;
    if (!PatchVertexShaders(s, vsPatched, result.error)) {
      return result;
    }
    if (vsPatched < 3) {
      result.error = "mesh.fx: only " + std::to_string(vsPatched) + " vertex shaders could store the view distance";
      return result;
    }

    // the one pixel shader that read depth.y (WreckagePS)
    const string depthRead = "0.01 * vertex.depth.y";
    if (CountOccurrences(s, depthRead) != 2) {
      result.error = "mesh.fx: the wreckage shader changed (depth.y reads)";
      return result;
    }
    for (size_t p = s.find(depthRead); p != npos; p = s.find(depthRead, p)) {
      const string replacement = "0.01 * (time - vertex.material.x)";
      s.replace(p, depthRead.size(), replacement);
      p += replacement.size();
    }

    // light shaders after PBR_Seraphim (they need ComputeNormal, the samplers and the PBR helper functions)
    const size_t pbr = s.find("float4 PBR_Seraphim(");
    const size_t pbrEnd = pbr == npos ? npos : FunctionEnd(s, pbr);
    if (pbrEnd == npos) {
      result.error = "mesh.fx: PBR_Seraphim not found";
      return result;
    }
    {
      string code = string(kCommonDecl) + kMeshLightCode;
      if (flood != 0) {
        for (const char* const ret : {"    return float4(lit * (1 - saturate(mirrored)), albedoTex.a);   // alpha: the alpha test of the technique still applies",
                                      "    return float4(lit * (1 - saturate(mirrored)), albedoTex.a);"}) {
          const size_t at = code.find(ret);
          if (at != npos) {
            code.replace(at, strlen(ret), "    return float4(1, 0, 1, albedoTex.a);   // /dynlightsflood");
          }
        }
        for (size_t at = code.find("    return float4(lit * (1 - saturate(mirrored)), albedoTex.a);"); at != npos;
             at = code.find("    return float4(lit * (1 - saturate(mirrored)), albedoTex.a);")) {
          code.replace(at, strlen("    return float4(lit * (1 - saturate(mirrored)), albedoTex.a);"), "    return float4(1, 0, 1, albedoTex.a);   // /dynlightsflood");
        }
      }
      s.insert(pbrEnd, code);
    }

    // the light pass on the single-pass lit techniques
    std::vector<TechniqueSpan> spans = FindTechniques(s);
    string out;
    out.reserve(s.size() + 64 * 1024);
    size_t pos = 0;
    constexpr size_t kTangentCount = sizeof(kLitTangent) / sizeof(kLitTangent[0]);
    constexpr size_t kVsCount = sizeof(kTangentVertexShaders) / sizeof(kTangentVertexShaders[0]);
    for (const TechniqueSpan& span : spans) {
      const bool tangent = ContainsTechnique(kLitTangent, kTangentCount, span.name);
      const bool vertexNormal = span.name == "VertexNormal_HighFidelity";
      if (!tangent && !vertexNormal) {
        continue;
      }
      string block = s.substr(span.begin, span.end - span.begin);
      if (CountOccurrences(block, "pass ") != 1) {
        continue; // multi-pass techniques are left alone, as in the generator
      }
      ShaderCall vs;
      if (!ParseShaderAssignment(block, "VertexShader", vs)) {
        continue;
      }
      if (tangent) {
        bool known = false;
        for (size_t k = 0; k < kVsCount; ++k) {
          known = known || vs.name == kTangentVertexShaders[k];
        }
        if (!known) {
          continue;
        }
      }
      const string rasterizer = FindRasterizerLine(block);
      const string alphaTest = FindAlphaTestBlock(block);
      const char* const ps = tangent ? "DynLightsPS" : "DynLightsVertexNormalPS";

      string pass = "\n    pass PDynLight\n    {\n        // FAF dynamic lights: light-only pass, additive (see " + string(ps) + ")\n";
      if (!rasterizer.empty()) {
        pass += "        " + rasterizer + "\n";
      }
      if (!alphaTest.empty()) {
        pass += alphaTest + "\n";
      }
      pass += "        AlphaBlendEnable = true;\n        ColorWriteEnable = 0x07;\n        SrcBlend = One;\n        DestBlend = One;\n"
              "        ZEnable = true;\n        ZFunc = LessEqual;\n        ZWriteEnable = false;\n\n";
      pass += "        VertexShader = compile " + vs.profile + " " + vs.call + ";\n        PixelShader = compile ps_3_0 " + ps + "();\n    }\n";

      const size_t close = block.find_last_of('}');
      if (close == npos) {
        continue;
      }
      // keep the text before the closing brace as it is, trimmed of trailing blank lines like the generator
      size_t cut = close;
      while (cut > 0 && block[cut - 1] == '\n') {
        --cut;
      }
      string patched = block.substr(0, cut) + "\n" + pass + block.substr(close);
      if (!AddAnnotation(patched, "int dynLightPasses = 1;")) {
        continue; // no annotation block to mark it: the pass could not be dropped when idle, so do not add it
      }
      out.append(s, pos, span.begin - pos);
      out += patched;
      pos = span.end;
      ++result.techniques;
    }
    if (result.techniques < 10) {
      result.error = "mesh.fx: only " + std::to_string(result.techniques) + " techniques could take the light pass";
      return result;
    }
    out.append(s, pos, npos);

    source = std::move(out);
    result.patched = true;
    return result;
  }

  EffectPatchResult PatchTerrainEffect(std::string& source, const int flood)
  {
    EffectPatchResult result;
    string s = source;

    if (s.find("dynLightData") != npos) {
      result.error = "terrain.fx: already patched";
      return result;
    }

    const string marker = "technique TTerrainDepth";
    if (CountOccurrences(s, marker) != 1) {
      result.error = "terrain.fx: TTerrainDepth not found";
      return result;
    }

    // CalculateLighting: the decal and splat shaders light through it, so the lights go into its sun term
    const string anchor = "// calculate the lit pixels\nfloat4 CalculateLighting(";
    if (CountOccurrences(s, anchor) != 1) {
      result.error = "terrain.fx: CalculateLighting not found";
      return result;
    }
    const string sunTerm =
      "    float3 light = SunColor * saturate( SunDotNormal) * shadow + SunAmbience * ambientOcclusion + specular;\n";
    if (CountOccurrences(s, sunTerm) != 1) {
      result.error = "terrain.fx: the sun term of CalculateLighting changed";
      return result;
    }
    {
      const size_t p = s.find(sunTerm);
      s.insert(p + sunTerm.size(), "    light += DynLightTerrainFew(worldTerrain.xzy, inNormal);   // FAF dynamic lights\n");
    }
    {
      const size_t p = s.find(anchor);
      s.insert(p, string(kCommonDecl) + kTerrainLightFunction + "\n");
    }

    // the pixel shader of the composite pass goes ahead of the first technique
    {
      const size_t p = s.find(marker);
      string code = kTerrainPassCode;
      if (flood != 0) {
        const string ret = "    return float4(total / (1.0 + 1.0 * total), 0);";
        const size_t at = code.find(ret);
        if (at != npos) {
          code.replace(
            at,
            ret.size(),
            flood == 2 ? // the world position as a 20-unit checker: shows the pass gets world coordinates
                         "    return float4(frac(inV.mTexWT.x / 20.0), frac(inV.mTexWT.y / 20.0), 0, 0);   // /dynlightsflood2"
                       : "    return float4(1, 0, 1, 0);   // /dynlightsflood"
          );
        }
      }
      s.insert(p, code);
    }

    // the light pass at the end of every composite technique
    std::vector<TechniqueSpan> spans = FindTechniques(s);
    string out;
    out.reserve(s.size() + 8 * 1024);
    size_t pos = 0;
    for (const TechniqueSpan& span : spans) {
      string block = s.substr(span.begin, span.end - span.begin);
      const size_t header = block.find('>');
      if (header == npos) {
        continue;
      }
      if (block.substr(0, header).find("usage = \"composite\"") == npos) {
        continue;
      }
      // closing brace of the technique body: match braces from the first '{' after the annotation block
      size_t k = block.find('{', header);
      if (k == npos) {
        continue;
      }
      int depth = 0;
      for (;; ++k) {
        if (k >= block.size()) {
          k = npos;
          break;
        }
        if (block[k] == '{') {
          ++depth;
        } else if (block[k] == '}') {
          if (--depth == 0) {
            break;
          }
        }
      }
      if (k == npos) {
        continue;
      }
      string patched = block.substr(0, k) + kTerrainPass + block.substr(k);
      if (!AddAnnotation(patched, "int dynLightPasses = 1;")) {
        continue;
      }
      out.append(s, pos, span.begin - pos);
      out += patched;
      pos = span.end;
      ++result.techniques;
    }
    if (result.techniques < 5) {
      result.error = "terrain.fx: only " + std::to_string(result.techniques) + " composite techniques found";
      return result;
    }
    out.append(s, pos, npos);

    source = std::move(out);
    result.patched = true;
    return result;
  }
} // namespace moho::lights
