#pragma once

// HLSL shared by the water simulation shader (compute), the patched water effect (pixel) and the headless lab (tools/watersimlab), so that the lab
// renders the foam with the very code of the game. Plain HLSL: no effect-file syntax, no samplers.
namespace moho::water
{
  // Hash-based value noise: smooth, no texture, no periodicity (a sum of sines shows as regular chevrons).
  constexpr char kWaterNoiseHlsl[] = R"HLSL(
// float-only hash of a lattice point (Dave Hoskins, "hash without sine"): the effect compiler of the shipped water shader has no integer ops
float FafNoiseCorner(float2 i)
{
    float3 p3 = frac(float3(i.x, i.y, i.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// 0 .. 1, smooth
float FafNoise2(float2 p)
{
    float2 f = floor(p);
    float2 u = p - f;
    u = u * u * (3.0 - 2.0 * u);
    float a = FafNoiseCorner(f);
    float b = FafNoiseCorner(f + float2(1.0, 0.0));
    float c = FafNoiseCorner(f + float2(0.0, 1.0));
    float d = FafNoiseCorner(f + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
)HLSL";

  // The foam of the water surface: `foam` is what the simulation holds (0 .. 1.5, filtered), `xz` the world position, `sumA` the alpha of the shipped
  // wave crest texture. Returns how much of the water pixel is foam. The simulation foam is a grid of half units: a hard threshold on it shows the
  // grid, so the threshold is carried by fractal noise (broad lumps, fine strands and speckle) that has no regular direction.
  constexpr char kWaterFoamLookHlsl[] = R"HLSL(
float FafFoamLace(float2 xz)
{
    float lump = FafNoise2(xz * 0.55 + float2(11.3, 4.7));
    float fine = FafNoise2(xz * 2.3 + float2(3.1, 17.9));
    float strand = 1.0 - abs(2.0 * FafNoise2(xz * 1.25 + float2(21.7, 9.1)) - 1.0); // ridged: thin streaks where the noise crosses 0.5
    return saturate(0.36 * lump + 0.26 * fine + 0.38 * strand * strand);
}

float FafFoamAmount(float foam, float2 xz, float sumA)
{
    float lace = FafFoamLace(xz);
    return smoothstep(0.15, 0.7, foam * (0.30 + 1.0 * lace) + (saturate(sumA * 0.5) - 0.5) * 0.9);
}
)HLSL";
} // namespace moho::water
