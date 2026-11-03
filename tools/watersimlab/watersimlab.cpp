// Headless lab of the water simulation (docs/LIGHTING_V2.md, "Water lab").
//
// It runs the REAL compute shader of the game (the HLSL raw string of src/moho/terrain/water/WaterSimulation.cpp is read from the source at start)
// on a software D3D11 device (WARP: the CPU, so a running game is not disturbed), over a synthetic coast, with the real distance-to-shore field
// (src/moho/terrain/water/ShoreField.h), and writes captures in the format of the in-game recorder (tools/waterrec.py reads them; tools/watersimlab.py
// measures and renders them). No game, no window, no GPU.
//
//   watersimlab --src <WaterSimulation.cpp> --out <dir> [--terrain coast|beach|bay] [--n 512] [--seconds 40] [--every 0.5] [--from 10] [--swell 100]
//               [--shorewave 100] [--ship]
//
// The window is n x n cells of half a unit at the world origin (n = 512: 256 x 256 units), fixed (no scrolling).

#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/moho/terrain/water/ShoreField.h"

using Microsoft::WRL::ComPtr;

namespace
{
  constexpr float kCell = 0.5f;
  constexpr float kStep = 1.0f / 90.0f;
  constexpr float kElevation = 25.0f; // world units: the sea level of the synthetic maps
  constexpr float kMargin = 0.1f;     // the land margin of the game

  struct Constants
  {
    float a[4], b[4], c[4], d[4], e[4], f[4];
  };

  std::string ReadFile(const std::string& path)
  {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  // ---- synthetic terrain -----------------------------------------------------------------------------------------------------------
  float Hash2(int x, int z)
  {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFF) / 16777215.0f;
  }

  float ValueNoise(float x, float z)
  {
    const int ix = static_cast<int>(std::floor(x));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(ix);
    const float fz = z - static_cast<float>(iz);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sz = fz * fz * (3.0f - 2.0f * fz);
    const float a = Hash2(ix, iz), b = Hash2(ix + 1, iz), c = Hash2(ix, iz + 1), d = Hash2(ix + 1, iz + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sz;
  }

  // The ground of a synthetic map relative to the sea level, world units (positive is land). `s` is the signed distance from the coast, positive in
  // the water.
  float GroundAt(const std::string& terrain, const float x, const float z)
  {
    float s = 0.0f;
    float slope = 0.10f;
    if (terrain == "beach") {
      s = 140.0f - x; // a straight beach, to the right of x = 140
      slope = 0.05f;
    } else if (terrain == "bay") {
      const float dx = x - 190.0f;
      const float dz = z - 128.0f;
      s = std::sqrt(dx * dx + dz * dz) - 70.0f; // a round bay: land outside, water inside
      s = -s;                                   // (water inside the circle)
      slope = 0.12f;
    } else { // "coast": a wandering coast, a headland, an island, slopes from a flat beach to a cliff
      const float coast = 150.0f + 22.0f * std::sin(z * 0.035f) + 10.0f * std::sin(z * 0.11f + 1.0f) + 8.0f * (ValueNoise(z * 0.05f, 3.0f) - 0.5f);
      const float shore = coast - x;
      const float ix = x - 60.0f;
      const float iz = z - 70.0f;
      const float island = std::sqrt(ix * ix + iz * iz) - 18.0f;
      s = std::min(shore, island);
      slope = 0.07f + 0.13f * (0.5f + 0.5f * std::sin(z * 0.02f + 0.5f));
    }
    float h;
    if (s > 0.0f) {
      h = -std::min(slope * s + 0.25f * std::sqrt(s) * 0.3f, 14.0f); // the sea bed
    } else {
      h = 0.3f * (-s) + 0.1f;                                         // the land
    }
    // ground bumps: a sea bed and a beach are not smooth
    h += 0.30f * (ValueNoise(x * 0.9f, z * 0.9f) - 0.5f) + 0.5f * (ValueNoise(x * 0.12f, z * 0.12f) - 0.5f);
    return h;
  }

  struct Args
  {
    std::string src, out = "out", terrain = "coast";
    int n = 512;
    float seconds = 40.0f, every = 0.5f, from = 10.0f;
    int swell = 100, shoreWave = 100;
    int look = 4;       // pixels per world unit of the look images (0: none)
    float suma = 0.5f;  // the alpha of the shipped crest texture the foam threshold sees (a pixel of the game has a value of its own)
  };
} // namespace

int main(int argc, char** argv)
{
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (key == "--src") args.src = next();
    else if (key == "--out") args.out = next();
    else if (key == "--terrain") args.terrain = next();
    else if (key == "--n") args.n = std::atoi(next().c_str());
    else if (key == "--seconds") args.seconds = static_cast<float>(std::atof(next().c_str()));
    else if (key == "--every") args.every = static_cast<float>(std::atof(next().c_str()));
    else if (key == "--from") args.from = static_cast<float>(std::atof(next().c_str()));
    else if (key == "--swell") args.swell = std::atoi(next().c_str());
    else if (key == "--shorewave") args.shoreWave = std::atoi(next().c_str());
    else if (key == "--look") args.look = std::atoi(next().c_str());
    else if (key == "--suma") args.suma = static_cast<float>(std::atof(next().c_str()));
  }
  if (args.src.empty()) {
    std::fprintf(stderr, "usage: watersimlab --src <WaterSimulation.cpp> --out <dir> [--terrain coast|beach|bay] [--n 512] [--seconds 40] [--every 0.5] [--from 10]\n");
    return 2;
  }
  const int n = args.n;

  // ---- the shader: the HLSL raw string of the game ---------------------------------------------------------------------------------
  const std::string cpp = ReadFile(args.src);
  const std::size_t a = cpp.find("R\"HLSL(");
  const std::size_t b = a == std::string::npos ? a : cpp.find(")HLSL\"", a);
  if (a == std::string::npos || b == std::string::npos) {
    std::fprintf(stderr, "no HLSL raw string in %s\n", args.src.c_str());
    return 2;
  }
  // the shared noise comes first, as in the game (SimSource in WaterSimulation.cpp); the foam look is the shared code of the pixel shader
  const std::string common = ReadFile((std::filesystem::path(args.src).parent_path() / "WaterShaderCommon.h").string());
  const auto rawString = [&](const std::size_t which) {
    std::size_t from = 0;
    for (std::size_t i = 0; i <= which; ++i) {
      from = common.find("R\"HLSL(", i == 0 ? 0 : from + 1);
    }
    const std::size_t end = common.find(")HLSL\"", from);
    return common.substr(from + 7, end - (from + 7));
  };
  const std::string noiseHlsl = rawString(0);
  const std::string foamHlsl = rawString(1);
  const std::string hlsl = noiseHlsl + cpp.substr(a + 7, b - (a + 7));

  // ---- the terrain, the land mask and the distance to the shore ----------------------------------------------------------------------
  const std::uint32_t hw = static_cast<std::uint32_t>(n / 2 + 4); // height samples cover the window (one a world unit)
  const std::uint32_t hh = hw;
  std::vector<std::uint16_t> raw(static_cast<std::size_t>(hw) * hh);
  for (std::uint32_t z = 0; z < hh; ++z) {
    for (std::uint32_t x = 0; x < hw; ++x) {
      const float h = GroundAt(args.terrain, static_cast<float>(x), static_cast<float>(z));
      raw[static_cast<std::size_t>(z) * hw + x] = static_cast<std::uint16_t>(std::min(std::max((kElevation + h) * 128.0f, 0.0f), 65535.0f));
    }
  }
  const float levelRaw = (kElevation - kMargin) * 128.0f;
  const std::vector<float> shore = moho::water::ComputeShoreDistance(
    [&](const std::uint32_t x, const std::uint32_t z) { return raw[static_cast<std::size_t>(z) * hw + x]; }, hw, hh, levelRaw);
  std::vector<std::uint32_t> packed((raw.size() + 1U) / 2U, 0U);
  for (std::size_t i = 0; i < raw.size(); ++i) {
    packed[i >> 1U] |= static_cast<std::uint32_t>(raw[i]) << ((i & 1U) * 16U);
  }

  // ---- the device -------------------------------------------------------------------------------------------------------------------
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level{};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context))) {
    std::fprintf(stderr, "no WARP device\n");
    return 3;
  }
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), "WaterSimulation.hlsl", nullptr, nullptr, "SimCS", "cs_5_0", 0, 0, &code, &errors))) {
    std::fprintf(stderr, "shader: %s\n", errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    return 4;
  }
  ComPtr<ID3D11ComputeShader> cs;
  device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);

  struct Buf
  {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
  };
  const auto makeBuffer = [&](const UINT stride, const UINT count, const void* init, const bool writable) {
    Buf r;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = stride * count;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (writable ? D3D11_BIND_UNORDERED_ACCESS : 0U);
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = stride;
    std::vector<std::uint8_t> zero;
    if (init == nullptr) {
      zero.assign(d.ByteWidth, 0);
      init = zero.data();
    }
    D3D11_SUBRESOURCE_DATA data{init, 0, 0};
    device->CreateBuffer(&d, &data, &r.buffer);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0;
    sv.Buffer.NumElements = count;
    device->CreateShaderResourceView(r.buffer.Get(), &sv, &r.srv);
    if (writable) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
      uv.Format = DXGI_FORMAT_UNKNOWN;
      uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      uv.Buffer.FirstElement = 0;
      uv.Buffer.NumElements = count;
      device->CreateUnorderedAccessView(r.buffer.Get(), &uv, &r.uav);
    }
    return r;
  };
  const UINT cells = static_cast<UINT>(n) * static_cast<UINT>(n);
  Buf state[2] = {makeBuffer(16, cells, nullptr, true), makeBuffer(16, cells, nullptr, true)};
  Buf sources = makeBuffer(16, 8, nullptr, false);
  Buf heights = makeBuffer(4, static_cast<UINT>(packed.size()), packed.data(), false);
  Buf shoreBuf = makeBuffer(4, static_cast<UINT>(shore.size()), shore.data(), false);
  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(Constants);
  cbd.Usage = D3D11_USAGE_DEFAULT;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ComPtr<ID3D11Buffer> constantBuffer;
  device->CreateBuffer(&cbd, nullptr, &constantBuffer);
  D3D11_BUFFER_DESC sd{};
  sd.ByteWidth = 16U * cells;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  sd.StructureByteStride = 16;
  ComPtr<ID3D11Buffer> staging;
  device->CreateBuffer(&sd, nullptr, &staging);

  // ---- the look pass: the foam as the water pixel shader shows it (shared code), over a plain blue sea, top down -------------------------------
  const int lookSize = args.look > 0 ? static_cast<int>(static_cast<float>(args.look) * static_cast<float>(n) * kCell) : 0;
  ComPtr<ID3D11ComputeShader> lookCs;
  Buf lookOut;
  ComPtr<ID3D11Buffer> lookStaging, lookConstants;
  if (lookSize > 0) {
    const std::string lookSrc = noiseHlsl + foamHlsl + R"HLSL(
cbuffer LookConstants : register(b0)
{
    float4 lA;   // pixels per unit, cells, cell size, unused
    float4 lB;   // alpha of the crest texture, time, unused, unused
};
StructuredBuffer<float4> State : register(t0);   // height, last height, foam, land
RWStructuredBuffer<uint> Pixels : register(u0);

float4 Cell(int2 c)
{
    int n = (int)lA.y;
    return State[clamp(c.y, 0, n - 1) * n + clamp(c.x, 0, n - 1)];
}

// bilinear sample of the state at a world position
float4 Sample(float2 xz)
{
    float2 g = xz / lA.z - 0.5;
    int2 i = (int2)floor(g);
    float2 f = g - (float2)i;
    return lerp(lerp(Cell(i), Cell(i + int2(1, 0)), f.x), lerp(Cell(i + int2(0, 1)), Cell(i + int2(1, 1)), f.x), f.y);
}

[numthreads(8, 8, 1)]
void LookCS(uint3 id : SV_DispatchThreadID)
{
    int size = (int)(lA.x * lA.y * lA.z);
    if ((int)id.x >= size || (int)id.y >= size)
    {
        return;
    }
    float2 xz = ((float2)id.xy + 0.5) / lA.x;
    float4 here = Sample(xz);
    float3 color;
    if (here.w > 0.5)
    {
        color = float3(0.33, 0.35, 0.24) * (0.8 + 0.4 * FafNoise2(xz * 0.8));   // land
    }
    else
    {
        // the sea: a plain blue, lit by the slope of the waves
        float e = lA.z;
        float hx = Sample(xz + float2(e, 0)).x - Sample(xz - float2(e, 0)).x;
        float hz = Sample(xz + float2(0, e)).x - Sample(xz - float2(0, e)).x;
        float3 N = normalize(float3(-hx * 5.0 / e, 1.0, -hz * 5.0 / e));
        float sun = saturate(dot(N, normalize(float3(0.4, 0.8, 0.3))));
        color = float3(0.04, 0.20, 0.42) * (0.55 + 0.9 * sun);
        // the foam: four taps on the diagonals a bit more than a cell out, as FafWaterFoam does in the game
        float o = 0.45;
        float foam = 0.25 * (Sample(xz + float2(o, o)).z + Sample(xz + float2(-o, o)).z + Sample(xz + float2(o, -o)).z + Sample(xz + float2(-o, -o)).z);
        float amount = FafFoamAmount(foam, xz, lB.x);
        float3 foamColor = float3(0.95, 0.98, 1.0) * (0.8 + 0.2 * sun);
        color = lerp(color, foamColor, amount * 0.92);
    }
    uint3 q = (uint3)(saturate(color) * 255.0 + 0.5);
    Pixels[id.y * (uint)size + id.x] = q.x | (q.y << 8) | (q.z << 16);
}
)HLSL";
    ComPtr<ID3DBlob> lookCode, lookErrors;
    if (FAILED(D3DCompile(lookSrc.data(), lookSrc.size(), "Look.hlsl", nullptr, nullptr, "LookCS", "cs_5_0", 0, 0, &lookCode, &lookErrors))) {
      std::fprintf(stderr, "look shader: %s\n", lookErrors != nullptr ? static_cast<const char*>(lookErrors->GetBufferPointer()) : "?");
      return 4;
    }
    device->CreateComputeShader(lookCode->GetBufferPointer(), lookCode->GetBufferSize(), nullptr, &lookCs);
    lookOut = makeBuffer(4, static_cast<UINT>(lookSize) * static_cast<UINT>(lookSize), nullptr, true);
    D3D11_BUFFER_DESC lsd{};
    lsd.ByteWidth = 4U * static_cast<UINT>(lookSize) * static_cast<UINT>(lookSize);
    lsd.Usage = D3D11_USAGE_STAGING;
    lsd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    lsd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    lsd.StructureByteStride = 4;
    device->CreateBuffer(&lsd, nullptr, &lookStaging);
    D3D11_BUFFER_DESC lcd{};
    lcd.ByteWidth = 32;
    lcd.Usage = D3D11_USAGE_DEFAULT;
    lcd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&lcd, nullptr, &lookConstants);
  }

  std::filesystem::create_directories(args.out);
  const float half = 0.5f * static_cast<float>(n) * kCell;
  int current = 0;
  int captureIndex = 0;
  float nextCapture = args.from;
  const int totalSteps = static_cast<int>(args.seconds / kStep);
  std::printf("watersimlab: terrain %s, %d x %d cells, %d steps (%.1f s), shader of the game, WARP\n", args.terrain.c_str(), n, n, totalSteps, args.seconds);
  for (int step = 0; step < totalSteps; ++step) {
    const float time = static_cast<float>(step) * kStep;
    Constants k{};
    k.a[0] = 0; k.a[1] = 0; k.a[2] = 0; k.a[3] = 0;                                    // origin and previous origin (cells)
    k.b[0] = kCell; k.b[1] = static_cast<float>(n); k.b[2] = 0.0f; k.b[3] = kStep;     // cell, cells, sources, step seconds
    k.c[0] = 0.0031f; k.c[1] = 0.997f; k.c[2] = half; k.c[3] = half;                   // wave speed term, damping, centre
    k.d[0] = half; k.d[1] = 0.01f * static_cast<float>(args.shoreWave);               // half extent, shore wave factor
    k.e[0] = static_cast<float>(hw); k.e[1] = static_cast<float>(hh); k.e[2] = levelRaw; k.e[3] = 1.0f; // heights, level, land mask on
    k.f[0] = time; k.f[1] = 0.01f * static_cast<float>(args.swell); k.f[2] = 0.994f; k.f[3] = 1.0f;     // time, swell, foam kept, surf
    context->UpdateSubresource(constantBuffer.Get(), 0, nullptr, &k, 0, 0);
    ID3D11ShaderResourceView* srvs[4] = {state[current].srv.Get(), sources.srv.Get(), heights.srv.Get(), shoreBuf.srv.Get()};
    ID3D11UnorderedAccessView* uavs[1] = {state[current ^ 1].uav.Get()};
    context->CSSetShader(cs.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
    context->CSSetShaderResources(0, 4, srvs);
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context->Dispatch(static_cast<UINT>(n / 8), static_cast<UINT>(n / 8), 1);
    ID3D11ShaderResourceView* noSrv[4] = {};
    ID3D11UnorderedAccessView* noUav[1] = {};
    context->CSSetShaderResources(0, 4, noSrv);
    context->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    current ^= 1;

    if (time >= nextCapture) {
      nextCapture += args.every;
      context->CopyResource(staging.Get(), state[current].buffer.Get());
      D3D11_MAPPED_SUBRESOURCE map{};
      if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        const float* cellsData = static_cast<const float*>(map.pData);
        char name[512];
        std::snprintf(name, sizeof(name), "%s/rec_%05d.bin", args.out.c_str(), captureIndex++);
        if (FILE* const f = std::fopen(name, "wb")) {
          const std::uint32_t header[4] = {0x31435257u, static_cast<std::uint32_t>(n), 0u, 0u};
          const float info[12] = {kCell, 0.0f, 0.0f, time, static_cast<float>(step), 1.0f, 1.0f, 1.0f, kStep, half, half, kElevation};
          std::fwrite(header, sizeof(header), 1, f);
          std::fwrite(info, sizeof(info), 1, f);
          std::vector<float> height(cells), foam(cells);
          std::vector<std::uint8_t> land(cells);
          for (UINT i = 0; i < cells; ++i) {
            height[i] = cellsData[i * 4U];
            foam[i] = cellsData[i * 4U + 2U];
            land[i] = cellsData[i * 4U + 3U] > 0.5f ? 1U : 0U;
          }
          std::fwrite(height.data(), 4, cells, f);
          std::fwrite(foam.data(), 4, cells, f);
          std::fwrite(land.data(), 1, cells, f);
          std::fclose(f);
        }
        context->Unmap(staging.Get(), 0);
      }
      if (lookSize > 0) {
        const float lc[8] = {static_cast<float>(args.look), static_cast<float>(n), kCell, 0.0f, args.suma, time, 0.0f, 0.0f};
        context->UpdateSubresource(lookConstants.Get(), 0, nullptr, lc, 0, 0);
        ID3D11ShaderResourceView* lookSrv[1] = {state[current].srv.Get()};
        ID3D11UnorderedAccessView* lookUav[1] = {lookOut.uav.Get()};
        context->CSSetShader(lookCs.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, lookConstants.GetAddressOf());
        context->CSSetShaderResources(0, 1, lookSrv);
        context->CSSetUnorderedAccessViews(0, 1, lookUav, nullptr);
        const UINT groups = (static_cast<UINT>(lookSize) + 7U) / 8U;
        context->Dispatch(groups, groups, 1);
        ID3D11ShaderResourceView* noLook[1] = {};
        ID3D11UnorderedAccessView* noLookUav[1] = {};
        context->CSSetShaderResources(0, 1, noLook);
        context->CSSetUnorderedAccessViews(0, 1, noLookUav, nullptr);
        context->CopyResource(lookStaging.Get(), lookOut.buffer.Get());
        D3D11_MAPPED_SUBRESOURCE lm{};
        if (SUCCEEDED(context->Map(lookStaging.Get(), 0, D3D11_MAP_READ, 0, &lm))) {
          const std::uint32_t* px = static_cast<const std::uint32_t*>(lm.pData);
          char bmpName[512];
          std::snprintf(bmpName, sizeof(bmpName), "%s/look_%05d.bmp", args.out.c_str(), captureIndex - 1);
          if (FILE* const f = std::fopen(bmpName, "wb")) {
            const std::uint32_t rowBytes = static_cast<std::uint32_t>(lookSize) * 3U;
            const std::uint32_t pad = (4U - rowBytes % 4U) % 4U;
            const std::uint32_t dataBytes = (rowBytes + pad) * static_cast<std::uint32_t>(lookSize);
            std::uint8_t header[54] = {'B', 'M'};
            const std::uint32_t fileBytes = 54U + dataBytes;
            std::memcpy(header + 2, &fileBytes, 4);
            const std::uint32_t offset = 54U, dib = 40U;
            const std::uint16_t planes = 1U, bits = 24U;
            std::memcpy(header + 10, &offset, 4);
            std::memcpy(header + 14, &dib, 4);
            const std::int32_t w = lookSize, h = lookSize;
            std::memcpy(header + 18, &w, 4);
            std::memcpy(header + 22, &h, 4);
            std::memcpy(header + 26, &planes, 2);
            std::memcpy(header + 28, &bits, 2);
            std::memcpy(header + 34, &dataBytes, 4);
            std::fwrite(header, 1, 54, f);
            std::vector<std::uint8_t> row(rowBytes + pad, 0);
            for (int y = lookSize - 1; y >= 0; --y) { // BMP rows run bottom up
              for (int x = 0; x < lookSize; ++x) {
                const std::uint32_t c = px[static_cast<std::size_t>(y) * static_cast<std::size_t>(lookSize) + static_cast<std::size_t>(x)];
                row[static_cast<std::size_t>(x) * 3U + 0U] = static_cast<std::uint8_t>((c >> 16) & 255U);
                row[static_cast<std::size_t>(x) * 3U + 1U] = static_cast<std::uint8_t>((c >> 8) & 255U);
                row[static_cast<std::size_t>(x) * 3U + 2U] = static_cast<std::uint8_t>(c & 255U);
              }
              std::fwrite(row.data(), 1, row.size(), f);
            }
            std::fclose(f);
          }
          context->Unmap(lookStaging.Get(), 0);
        }
      }
    }
  }
  std::printf("watersimlab: %d captures in %s\n", captureIndex, args.out.c_str());
  return 0;
}

