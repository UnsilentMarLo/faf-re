// Mesh terrain renderer of the editor (docs/projects/editor/MESH_VIEW.md). The shaders and the draw order follow automation/terrainmesh/meshterrain.cpp (the patch
// for the retail exe), which in turn follows SC2's terrain.hlsl: two blended layers, radiosity normal mapping with three baked light masks, sun occlusion in the
// alpha of mask 0, the layer blend in the alpha of mask 1, FA's shadow map for dynamic shadows, FA's water ramp below the water level.
//
// Differences to the patch: the camera and the shadow state come from the engine objects (GeomCamera3, Shadow) instead of being read back from shader constants,
// so the matrices are used as the engine stores them (row vector convention, `row_major` in HLSL). The raw Direct3D 11 device and its immediate context are the gal
// device's own; every draw saves the pipeline state it changes and puts it back, so the engine's state stays valid. The shaders are shader model 4 (the patch's are 3).

#include "moho/editor/EdMeshTerrain.h"
#include "moho/editor/EdD3D11Device.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3dx11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "boost/shared_ptr.h"
#include "gpg/gal/backends/d3d11/DeviceD3D11.h"
#include "gpg/gal/backends/d3d11/RenderTargetD3D11.h"
#include "moho/render/Shadow.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/misc/StartupHelpers.h"
#include "moho/render/WRenViewport.h"
#include "moho/render/d3d/CD3DRenderTarget.h"
#include "moho/sim/STIMap.h"
#include "moho/terrain/water/CWaterShaderProperties.h"

namespace moho
{
  namespace edmesh
  {
    namespace
    {
      using Microsoft::WRL::ComPtr;

      // ---- shaders ---------------------------------------------------------------------------------------------------------------------
      // Constant buffers: b0 of the vertex shader and b0 of the pixel shader, filled from the arrays below (the old c# registers, in the same order).

      const char kVertexShader[] = R"(
cbuffer VSConst : register(b0) { row_major float4x4 ViewProj; row_major float4x4 ShadowMatrix; };

struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; float3 tangent : TANGENT; float3 binormal : BINORMAL;
               float2 uv : TEXCOORD0; float2 uv2 : TEXCOORD1; };
struct VSOut { float4 pos : SV_Position; float4 uv : TEXCOORD0; float4 shadow : TEXCOORD1; float3 world : TEXCOORD2;
               float3 normal : TEXCOORD3; float3 tangent : TEXCOORD4; float3 binormal : TEXCOORD5; };

VSOut VS(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), ViewProj);
    o.uv = float4(i.uv, i.uv2);
    float4 s = mul(float4(i.pos, 1.0), ShadowMatrix);
    o.shadow = float4((s.x + s.w) * 0.5, (s.w - s.y) * 0.5, 0.0, s.w);
    o.world = i.pos;
    o.normal = i.normal;
    o.tangent = i.tangent;
    o.binormal = i.binormal;
    return o;
}
)";

      const char kPixelShader[] = R"(
cbuffer PSConst : register(b0) {
    float4 Params;      // x exposure, y 1 when FA's shadow map is bound, z water level, w 1 / depth span
    float4 Haze;        // x level below which the ground fades out, y 1 / fade span, z water tint, w 1 when in use
    float4 VoidColor;
    float4 ShadowColor; // SC2's fill light in shadow; a: how far a dynamic shadow pulls towards it
    float4 Material;    // x 1: layers blend, y 1: glows, z: sun highlight strength, w: alpha test reference + 1 or -1
    float4 SunDirection;
    float4 SunColor;
    float4 ViewPosition;
};
struct VSOut { float4 pos : SV_Position; float4 uv : TEXCOORD0; float4 shadow : TEXCOORD1; float3 world : TEXCOORD2;
               float3 normal : TEXCOORD3; float3 tangent : TEXCOORD4; float3 binormal : TEXCOORD5; };

Texture2D DiffuseA : register(t0);
Texture2D DiffuseB : register(t1);
Texture2D Light0 : register(t2);
Texture2D Light1 : register(t3);
Texture2D Light2 : register(t4);
Texture2D Shadow : register(t5);
Texture2D WaterRamp : register(t6);
Texture2D NormalA : register(t7);
Texture2D NormalB : register(t8);
Texture2D Mask : register(t9);
Texture2D MaskB : register(t10);
SamplerState S0 : register(s0);
SamplerState S1 : register(s1);
SamplerState S2 : register(s2);
SamplerState S3 : register(s3);
SamplerState S4 : register(s4);
SamplerState S5 : register(s5);
SamplerState S6 : register(s6);
SamplerState S7 : register(s7);
SamplerState S8 : register(s8);
SamplerState S9 : register(s9);
SamplerState S10 : register(s10);

float3 Linear(float3 c) { return pow(max(c, 0.0001), 2.2); }

float4 PS(VSOut i) : SV_Target {
    float4 l0 = Light0.Sample(S2, i.uv.zw);
    float4 l1 = Light1.Sample(S3, i.uv.zw);
    float4 l2 = Light2.Sample(S4, i.uv.zw);
    float blend = l1.a * Material.x;
    float4 diffuse = lerp(DiffuseA.Sample(S0, i.uv.xy), DiffuseB.Sample(S1, i.uv.xy), blend);
    clip(diffuse.a * 255.0 - Material.w);
    float3 albedo = Linear(diffuse.rgb);
    float3 n = normalize(lerp(NormalA.Sample(S7, i.uv.xy).rgb, NormalB.Sample(S8, i.uv.xy).rgb, blend) * 2.0 - 1.0);
    float3 dp = saturate(float3(dot(n, float3(0.81649658, 0.0, 0.57735027)),
                                dot(n, float3(-0.40824829, 0.70710678, 0.57735027)),
                                dot(n, float3(-0.40824829, -0.70710678, 0.57735027))));
    float3 light = dp.x * Linear(l0.rgb) + dp.y * Linear(l1.rgb) + dp.z * Linear(l2.rgb);
    float sun = lerp(1.0, Shadow.Sample(S5, i.shadow.xy / i.shadow.w).g, Params.y);
    float3 shade = min(ShadowColor.rgb, light * 0.4);
    light = lerp(light, shade, (1.0 - sun) * ShadowColor.a * max(l0.a, 0.5));
    float4 mask = lerp(Mask.Sample(S9, i.uv.xy), MaskB.Sample(S10, i.uv.xy), blend);
    float glow = mask.g * Material.y;
    light = lerp(light, 1.0, glow);
    float3 world = normalize(n.x * i.tangent + n.y * i.binormal + n.z * i.normal);
    float3 view = normalize(ViewPosition.xyz - i.world);
    float3 reflection = -reflect(view, world);
    float3 highlight = Material.z * SunColor.rgb * mask.b *
                       pow(max(0.000001, dot(SunDirection.xyz, reflection)), 20.0) * sun * l0.a;
    float3 color = pow((albedo * light + highlight) * Params.x, 1.0 / 2.2);
    color = min(color, 0.7) + 0.3 * (1.0 - exp(max(color - 0.7, 0.0) / -0.3));
    float depth = saturate((Params.z - i.world.y) * Params.w);
    float4 water = WaterRamp.Sample(S6, float2(depth, 0.5));
    color = lerp(color, water.rgb, water.a * step(i.world.y, Params.z) * Haze.z);
    float haze = saturate((Haze.x - i.world.y) * Haze.y) * Haze.w;
    color = lerp(color, VoidColor.rgb, haze);
    return float4(color, 0.4 * glow * (1.0 - haze));
}
)";

      const char kDepthVertexShader[] = R"(
cbuffer VSConst : register(b0) { row_major float4x4 ViewProj; };
struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; float3 tangent : TANGENT; float3 binormal : BINORMAL;
               float2 uv : TEXCOORD0; float2 uv2 : TEXCOORD1; };
struct VSOut { float4 pos : SV_Position; float depth : TEXCOORD0; float2 uv : TEXCOORD1; };
VSOut VS(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), ViewProj);
    o.depth = o.pos.z / o.pos.w;
    o.uv = i.uv;
    return o;
}
)";

      const char kDepthPixelShader[] = R"(
cbuffer PSConst : register(b0) { float4 AlphaTest; };
Texture2D Diffuse : register(t0);
SamplerState S0 : register(s0);
float4 PS(float4 pos : SV_Position, float depth : TEXCOORD0, float2 uv : TEXCOORD1) : SV_Target {
    clip(Diffuse.Sample(S0, uv).a * 255.0 - AlphaTest.x);
    return float4(depth, 1.0, 0.0, 1.0);
}
)";

      // SC2's sky: position - map centre + camera, kept just in front of the far plane, alpha blended, unlit.
      const char kSkyVertexShader[] = R"(
cbuffer VSConst : register(b0) { row_major float4x4 ViewProj; float4 Eye; float4 Centre; };
struct VSIn { float3 pos : POSITION; float3 normal : NORMAL; float3 tangent : TANGENT; float3 binormal : BINORMAL;
              float2 uv : TEXCOORD0; float2 uv2 : TEXCOORD1; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float facing : TEXCOORD1; };
VSOut VS(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos - Centre.xyz + Eye.xyz, 1.0), ViewProj);
    o.pos.z = o.pos.w * 0.99999;
    o.uv = i.uv;
    o.facing = dot(i.normal, Centre.xyz - i.pos);
    return o;
}
)";

      const char kSkyPixelShader[] = R"(
Texture2D Diffuse : register(t0);
SamplerState S0 : register(s0);
float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0, float facing : TEXCOORD1) : SV_Target {
    clip(facing);
    return Diffuse.Sample(S0, uv);
}
)";

      // The void fill: one huge rectangle drawn at the far plane, so it only shows where nothing else was drawn (water to the horizon, or the colour of a canyon map's void).
      const char kFillVertexShader[] = R"(
cbuffer VSConst : register(b0) { row_major float4x4 ViewProj; };
float4 VS(float3 pos : POSITION) : SV_Position {
    float4 p = mul(float4(pos, 1.0), ViewProj);
    p.z = p.w * 0.99999;
    return p;
}
)";

      const char kFillPixelShader[] = R"(
cbuffer PSConst : register(b0) { float4 Fill; };   // rgb colour, w 1: take the deep end of FA's water ramp instead
Texture2D WaterRamp : register(t6);
SamplerState S6 : register(s6);
float4 PS(float4 pos : SV_Position) : SV_Target {
    return float4(lerp(Fill.rgb, WaterRamp.Sample(S6, float2(0.97, 0.5)).rgb, Fill.w), 0.0);
}
)";

      // ---- state -----------------------------------------------------------------------------------------------------------------------

      MeshMap gMap;
      GeomCamera3 gCamera;
      bool gHaveCamera = false;
      unsigned gGeneration = 1;
      Mode gMode = Mode::Mesh;
      double gLastDrawMs = 0.0;

      constexpr UINT kVertexStride = 68;
      constexpr int kSamplerCount = 11;

      struct GpuPatch
      {
        ComPtr<ID3D11Buffer> vb;
        ComPtr<ID3D11Buffer> ib;
      };

      struct Gpu
      {
        ID3D11Device* device = nullptr;
        unsigned generation = 0;
        unsigned failedGeneration = 0;
        ComPtr<ID3D11VertexShader> vs, depthVs, skyVs, fillVs;
        ComPtr<ID3D11PixelShader> ps, depthPs, skyPs, fillPs;
        ComPtr<ID3D11InputLayout> layout, depthLayout, skyLayout, fillLayout;
        ComPtr<ID3D11Buffer> vsConst;            // 128 bytes: the vertex shaders' b0
        ComPtr<ID3D11Buffer> psConst;            // 128 bytes: the pixel shaders' b0
        ComPtr<ID3D11Buffer> fillVb;             // the void fill's four vertices
        ComPtr<ID3D11SamplerState> samplers[kSamplerCount];
        ComPtr<ID3D11SamplerState> skySampler;   // slot 0 wraps
        ComPtr<ID3D11RasterizerState> raster, rasterBiased;
        ComPtr<ID3D11DepthStencilState> depthWrite, depthTest;
        ComPtr<ID3D11BlendState> opaque, skyAlpha, skyAdditive;
        ComPtr<ID3D11ShaderResourceView> white, gray, flatNormal, ramp;
        std::string rampPath;
        std::vector<GpuPatch> patches;
        std::vector<ComPtr<ID3D11ShaderResourceView>> textures;   // material * kSlots + slot
      };
      Gpu gGpu;

      void ReleaseAll()
      {
        gGpu = Gpu();
      }

      ID3D11Device* RawDevice()
      {
        CD3DDevice* const device = D3D_GetDevice();
        if (device == nullptr || device->GetGalDevice() == nullptr) {
          return nullptr;
        }
        return moho::editor::D3D11RawDevice(device->GetGalDevice());
      }

      ID3D11DeviceContext* RawContext()
      {
        CD3DDevice* const device = D3D_GetDevice();
        if (device == nullptr || device->GetGalDevice() == nullptr) {
          return nullptr;
        }
        return moho::editor::D3D11RawContext(device->GetGalDevice());
      }

      using CompileFn = decltype(&::D3DCompile);
      using CreateTextureFn = decltype(&::D3DX11CreateTextureFromMemory);

      CompileFn Compiler()
      {
        static const CompileFn fn = [] {
          HMODULE module = ::LoadLibraryA("d3dcompiler_47.dll");
          return module != nullptr ? reinterpret_cast<CompileFn>(::GetProcAddress(module, "D3DCompile")) : nullptr;
        }();
        return fn;
      }

      CreateTextureFn TextureLoader()
      {
        static const CreateTextureFn fn = [] {
          HMODULE module = ::LoadLibraryA("d3dx11_43.dll");
          return module != nullptr ? reinterpret_cast<CreateTextureFn>(::GetProcAddress(module, "D3DX11CreateTextureFromMemory")) : nullptr;
        }();
        return fn;
      }

      bool CompileShader(const char* const source, const char* const entry, const char* const profile, ComPtr<ID3DBlob>& code)
      {
        const CompileFn compile = Compiler();
        if (compile == nullptr) {
          std::fprintf(stderr, "[edmesh] d3dcompiler_47.dll is missing\n");
          return false;
        }
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = compile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile, 0, 0, &code, &errors);
        if (FAILED(hr) || !code) {
          std::fprintf(stderr, "[edmesh] shader compile failed (%s): %s\n", profile, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
          return false;
        }
        return true;
      }

      bool MakeVertexShader(ID3D11Device* const dev, const char* const source, ComPtr<ID3D11VertexShader>& vs, ComPtr<ID3D11InputLayout>& layout, const bool fullLayout)
      {
        ComPtr<ID3DBlob> code;
        if (!CompileShader(source, "VS", "vs_4_0", code) || FAILED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs))) {
          return false;
        }
        static const D3D11_INPUT_ELEMENT_DESC kElements[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        static const D3D11_INPUT_ELEMENT_DESC kFillElements[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        const HRESULT hr = fullLayout ? dev->CreateInputLayout(kElements, 6, code->GetBufferPointer(), code->GetBufferSize(), &layout)
                                      : dev->CreateInputLayout(kFillElements, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout);
        return SUCCEEDED(hr);
      }

      bool MakePixelShader(ID3D11Device* const dev, const char* const source, ComPtr<ID3D11PixelShader>& ps)
      {
        ComPtr<ID3DBlob> code;
        return CompileShader(source, "PS", "ps_4_0", code) && SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
      }

      ComPtr<ID3D11ShaderResourceView> LoadTexture(ID3D11Device* const dev, const std::string& vfsPath, const bool oneLevel)
      {
        const std::vector<std::uint8_t> bytes = ReadVfs(vfsPath);
        const CreateTextureFn load = TextureLoader();
        if (bytes.empty() || load == nullptr) {
          return nullptr;
        }
        // a full mip chain: the file's levels, missing ones made from the top level (SC2's light masks have one level)
        D3DX11_IMAGE_LOAD_INFO info;
        info.MipLevels = oneLevel ? 1U : 0U;
        info.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Resource> resource;
        if (FAILED(load(dev, bytes.data(), bytes.size(), &info, nullptr, &resource, nullptr)) || !resource) {
          return nullptr;
        }
        ComPtr<ID3D11ShaderResourceView> view;
        return SUCCEEDED(dev->CreateShaderResourceView(resource.Get(), nullptr, &view)) ? view : nullptr;
      }

      ComPtr<ID3D11ShaderResourceView> Solid(ID3D11Device* const dev, const std::uint32_t argb)
      {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // A8R8G8B8 as a 32 bit value
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data = {&argb, 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(dev->CreateTexture2D(&desc, &data, &texture)) || FAILED(dev->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
          return nullptr;
        }
        return view;
      }

      ComPtr<ID3D11Buffer> MakeBuffer(ID3D11Device* const dev, const void* const data, const UINT bytes, const UINT bindFlags)
      {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = bytes;
        desc.BindFlags = bindFlags;
        desc.Usage = data != nullptr && bindFlags != D3D11_BIND_CONSTANT_BUFFER ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
        const D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        return SUCCEEDED(dev->CreateBuffer(&desc, data != nullptr ? &init : nullptr, &buffer)) ? buffer : nullptr;
      }

      bool CreateStates(ID3D11Device* const dev)
      {
        for (int i = 0; i < kSamplerCount; ++i) {
          // 0-1 tiled layers, 2-4 light masks, 5 shadow map (outside it: in the sun), 6 water ramp, 7-8 normal maps, 9-10 the masks (glow, highlight)
          const bool tiled = i < 2 || i > 6;
          const D3D11_TEXTURE_ADDRESS_MODE address = tiled ? D3D11_TEXTURE_ADDRESS_WRAP : i == 5 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
          const bool mips = i < 5 || i > 6;
          D3D11_SAMPLER_DESC desc{};
          desc.Filter = mips ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
          desc.AddressU = desc.AddressV = desc.AddressW = address;
          desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 1.0f;
          desc.MaxLOD = mips ? D3D11_FLOAT32_MAX : 0.0f;
          desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
          if (FAILED(dev->CreateSamplerState(&desc, &gGpu.samplers[i]))) {
            return false;
          }
        }
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&raster, &gGpu.raster))) {
          return false;
        }
        // both surfaces: the mesh is pulled a little towards the camera (the former depth bias of -0.002 is about -0.002 * 2^24 steps of a 24 bit buffer, slope scale -4)
        raster.DepthBias = -33554;
        raster.SlopeScaledDepthBias = -4.0f;
        if (FAILED(dev->CreateRasterizerState(&raster, &gGpu.rasterBiased))) {
          return false;
        }
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        if (FAILED(dev->CreateDepthStencilState(&depth, &gGpu.depthWrite))) {
          return false;
        }
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(dev->CreateDepthStencilState(&depth, &gGpu.depthTest))) {
          return false;
        }
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&blend, &gGpu.opaque))) {
          return false;
        }
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].RenderTargetWriteMask = 0x7;   // alpha is FA's glow: leave it
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        if (FAILED(dev->CreateBlendState(&blend, &gGpu.skyAlpha))) {
          return false;
        }
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        return SUCCEEDED(dev->CreateBlendState(&blend, &gGpu.skyAdditive));
      }

      bool CreateResources(ID3D11Device* const dev, const CWaterShaderProperties* water)
      {
        ReleaseAll();
        gGpu.device = dev;
        if (!MakeVertexShader(dev, kVertexShader, gGpu.vs, gGpu.layout, true) || !MakePixelShader(dev, kPixelShader, gGpu.ps) ||
            !MakeVertexShader(dev, kDepthVertexShader, gGpu.depthVs, gGpu.depthLayout, true) || !MakePixelShader(dev, kDepthPixelShader, gGpu.depthPs) ||
            !MakeVertexShader(dev, kSkyVertexShader, gGpu.skyVs, gGpu.skyLayout, true) || !MakePixelShader(dev, kSkyPixelShader, gGpu.skyPs) ||
            !MakeVertexShader(dev, kFillVertexShader, gGpu.fillVs, gGpu.fillLayout, false) || !MakePixelShader(dev, kFillPixelShader, gGpu.fillPs)) {
          return false;
        }
        gGpu.vsConst = MakeBuffer(dev, nullptr, 128, D3D11_BIND_CONSTANT_BUFFER);
        gGpu.psConst = MakeBuffer(dev, nullptr, 128, D3D11_BIND_CONSTANT_BUFFER);
        gGpu.fillVb = MakeBuffer(dev, nullptr, 48, D3D11_BIND_VERTEX_BUFFER);
        if (!gGpu.vsConst || !gGpu.psConst || !gGpu.fillVb || !CreateStates(dev)) {
          return false;
        }
        // the sky wraps its texture on slot 0
        {
          D3D11_SAMPLER_DESC desc{};
          desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
          desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
          desc.MaxLOD = D3D11_FLOAT32_MAX;
          desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
          if (FAILED(dev->CreateSamplerState(&desc, &gGpu.skySampler))) {
            return false;
          }
        }
        // alpha 0: as a water ramp white tints nothing, as light mask 0 gray shows the first layer only
        gGpu.white = Solid(dev, 0x00FFFFFFu);
        gGpu.gray = Solid(dev, 0x00C7C7C7u);
        gGpu.flatNormal = Solid(dev, 0xFF8080FFu);
        if (!gGpu.white || !gGpu.gray || !gGpu.flatNormal) {
          return false;
        }
        const MeshMap& map = gMap;
        gGpu.textures.assign(map.materials.size() * kSlots, nullptr);
        for (std::size_t m = 0; m < map.materials.size(); ++m) {
          for (int k = 0; k < kSlots; ++k) {
            const std::string& file = map.materials[m].files[k];
            if (!file.empty()) {
              gGpu.textures[m * kSlots + static_cast<std::size_t>(k)] = LoadTexture(dev, map.dir + "/" + file, false);
            }
          }
        }
        if (water != nullptr && water->mWaterRamp.size() > 0) {
          gGpu.rampPath = water->mWaterRamp.c_str();
          gGpu.ramp = LoadTexture(dev, gGpu.rampPath, true);
        }
        gGpu.patches.assign(map.patches.size(), GpuPatch());
        for (std::size_t i = 0; i < map.patches.size(); ++i) {
          const Patch& p = map.patches[i];
          GpuPatch& g = gGpu.patches[i];
          g.vb = MakeBuffer(dev, p.vertices.data(), static_cast<UINT>(p.vertexCount * 68ull), D3D11_BIND_VERTEX_BUFFER);
          g.ib = MakeBuffer(dev, p.indices.data(), static_cast<UINT>(p.indices.size() * 2), D3D11_BIND_INDEX_BUFFER);
          if (!g.vb || !g.ib) {
            return false;
          }
        }
        gGpu.generation = gGeneration;
        return true;
      }

      bool EnsureResources(ID3D11Device* const dev, const CWaterShaderProperties* water)
      {
        if (gGpu.device == dev && gGpu.generation == gGeneration) {
          return true;
        }
        if (gGpu.failedGeneration == gGeneration && gGpu.device == nullptr) {
          return false;
        }
        if (!CreateResources(dev, water)) {
          std::fprintf(stderr, "[edmesh] resources could not be created: the height field stays\n");
          const unsigned failed = gGeneration;
          ReleaseAll();
          gGpu.failedGeneration = failed;
          return false;
        }
        return true;
      }

      const Material* MaterialOf(const Patch& p)
      {
        return p.material >= 0 && p.material < static_cast<int>(gMap.materials.size()) ? &gMap.materials[static_cast<std::size_t>(p.material)] : nullptr;
      }

      ID3D11ShaderResourceView* SlotTexture(const Patch& p, const int slot)
      {
        if (p.material < 0 || p.material >= static_cast<int>(gMap.materials.size())) {
          return nullptr;
        }
        return gGpu.textures[static_cast<std::size_t>(p.material) * kSlots + static_cast<std::size_t>(slot)].Get();
      }

      ID3D11ShaderResourceView* ShadowTexture(Shadow* shadow)
      {
        if (shadow == nullptr || !shadow->mShadowCameraValid) {
          return nullptr;
        }
        const boost::shared_ptr<CD3DRenderTarget> target = shadow->GetShadowTexture();
        if (!target) {
          return nullptr;
        }
        CD3DRenderTarget::SurfaceHandle handle;
        target->GetSurface(handle);
        gpg::gal::RenderTarget* const surface = handle.get();
        return surface != nullptr ? static_cast<gpg::gal::RenderTargetD3D11*>(surface)->GetShaderResourceView() : nullptr;
      }

      /// clip = ((p * View) * (s, s, s, 1)) * Proj with s = TerrainErrorScale (terrain.fx): FA pulls distant terrain slightly towards the camera, so the mesh and the
      /// depth buffer agree with units and decals at every zoom.
      void TerrainViewProj(const GeomCamera3& camera, float out[16])
      {
        constexpr double kErrorScale = 1.0005;
        float view[16], proj[16];
        std::memcpy(view, &camera.view, sizeof view);
        std::memcpy(proj, &camera.projection, sizeof proj);
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
              sum += static_cast<double>(view[r * 4 + k]) * (k < 3 ? kErrorScale : 1.0) * proj[k * 4 + c];
            }
            out[r * 4 + c] = static_cast<float>(sum);
          }
        }
      }

      // The pipeline state a draw changes, put back afterwards (the engine's own state stays valid).
      struct SavedState
      {
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11Buffer> vb, ib, vsCb, psCb;
        UINT stride = 0, vbOffset = 0, ibOffset = 0;
        DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11DepthStencilState> depth;
        UINT stencilRef = 0;
        ComPtr<ID3D11BlendState> blend;
        float blendFactor[4] = {};
        UINT sampleMask = 0xFFFFFFFFu;
        ComPtr<ID3D11ShaderResourceView> srv[kSamplerCount];
        ComPtr<ID3D11SamplerState> sampler[kSamplerCount];

        void Capture(ID3D11DeviceContext* const ctx)
        {
          ctx->IAGetInputLayout(&layout);
          ctx->IAGetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &vbOffset);
          ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
          ctx->IAGetPrimitiveTopology(&topology);
          ctx->VSGetShader(&vs, nullptr, nullptr);
          ctx->PSGetShader(&ps, nullptr, nullptr);
          ctx->VSGetConstantBuffers(0, 1, vsCb.GetAddressOf());
          ctx->PSGetConstantBuffers(0, 1, psCb.GetAddressOf());
          ctx->RSGetState(&raster);
          ctx->OMGetDepthStencilState(&depth, &stencilRef);
          ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
          ID3D11ShaderResourceView* views[kSamplerCount] = {};
          ID3D11SamplerState* samplers[kSamplerCount] = {};
          ctx->PSGetShaderResources(0, kSamplerCount, views);
          ctx->PSGetSamplers(0, kSamplerCount, samplers);
          for (int i = 0; i < kSamplerCount; ++i) {
            srv[i].Attach(views[i]);
            sampler[i].Attach(samplers[i]);
          }
        }

        void Restore(ID3D11DeviceContext* const ctx)
        {
          ctx->IASetInputLayout(layout.Get());
          ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &vbOffset);
          ctx->IASetIndexBuffer(ib.Get(), ibFormat, ibOffset);
          ctx->IASetPrimitiveTopology(topology);
          ctx->VSSetShader(vs.Get(), nullptr, 0);
          ctx->PSSetShader(ps.Get(), nullptr, 0);
          ctx->VSSetConstantBuffers(0, 1, vsCb.GetAddressOf());
          ctx->PSSetConstantBuffers(0, 1, psCb.GetAddressOf());
          ctx->RSSetState(raster.Get());
          ctx->OMSetDepthStencilState(depth.Get(), stencilRef);
          ctx->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
          ID3D11ShaderResourceView* views[kSamplerCount];
          ID3D11SamplerState* samplers[kSamplerCount];
          for (int i = 0; i < kSamplerCount; ++i) {
            views[i] = srv[i].Get();
            samplers[i] = sampler[i].Get();
          }
          ctx->PSSetShaderResources(0, kSamplerCount, views);
          ctx->PSSetSamplers(0, kSamplerCount, samplers);
        }
      };

      void SetTexture(ID3D11DeviceContext* const ctx, const UINT slot, ID3D11ShaderResourceView* const view)
      {
        ctx->PSSetShaderResources(slot, 1, &view);
      }

      void DrawPatch(ID3D11DeviceContext* const ctx, const GpuPatch& g, const Patch& p)
      {
        const UINT stride = kVertexStride;
        const UINT offset = 0;
        ctx->IASetVertexBuffers(0, 1, g.vb.GetAddressOf(), &stride, &offset);
        ctx->IASetIndexBuffer(g.ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        ctx->DrawIndexed(p.triangleCount * 3, 0, 0);
      }
    } // namespace

    // ---- public state ---------------------------------------------------------------------------------------------------------------

    MeshMap& Map()
    {
      return gMap;
    }

    unsigned Generation()
    {
      return gGeneration;
    }

    void MarkChanged()
    {
      ++gGeneration;
    }

    Mode GetMode()
    {
      return gMode;
    }

    void SetMode(const Mode mode)
    {
      gMode = mode;
    }

    bool MeshWanted()
    {
      return gMode != Mode::HeightField && gMap.Loaded();
    }

    bool HeightFieldWanted()
    {
      return gMode != Mode::Mesh || !gMap.Loaded();
    }

    double LastDrawMs()
    {
      return gLastDrawMs;
    }

    void NoteCamera(const GeomCamera3& camera)
    {
      gCamera = camera;
      gHaveCamera = true;
    }

    bool CameraRay(const float px, const float py, const float vw, const float vh, float origin[3], float dir[3])
    {
      if (!gHaveCamera || vw <= 0.0f || vh <= 0.0f) {
        return false;
      }
      const GeomLine3 line = gCamera.Unproject(Wm3::Vector2f{px, py}, 0.0f, vw, vh, 0.0f);
      origin[0] = line.pos.x;
      origin[1] = line.pos.y;
      origin[2] = line.pos.z;
      dir[0] = line.dir.x;
      dir[1] = line.dir.y;
      dir[2] = line.dir.z;
      return true;
    }

    void ReleaseGpu()
    {
      ReleaseAll();
    }

    void ApplyHeightBands()
    {
      float low = 0.0f, high = 0.0f;
      if (!gMap.Loaded() || !gMap.HeightRange(32.0f, low, high)) {
        return;
      }
      // (the engine's names are swapped: `patch_maxMapHeight` holds the bottom of the band, `patch_minMapHeight` its top, see Sim_Create_exxt)
      moho::patch_maxMapHeight = std::min(moho::patch_maxMapHeight, low - 5.0f);
      moho::patch_minMapHeight = std::max(moho::patch_minMapHeight, high + 30.0f);
      if (CD3DDevice* const device = D3D_GetDevice(); device != nullptr) {
        if (WRenViewport* const viewport = device->GetViewport(); viewport != nullptr) {
          viewport->mRangeRenderer.Init();
          viewport->mVisionRenderer.Init();
        }
      }
    }

    // ---- the surface pass -----------------------------------------------------------------------------------------------------------

    bool DrawSurface(const GeomCamera3& camera, Shadow* const shadow, const CWaterShaderProperties* const water)
    {
      if (!MeshWanted()) {
        return false;
      }
      ID3D11Device* const dev = RawDevice();
      ID3D11DeviceContext* const ctx = RawContext();
      if (dev == nullptr || ctx == nullptr || !EnsureResources(dev, water)) {
        return false;
      }
      LARGE_INTEGER t0, t1, freq;
      QueryPerformanceCounter(&t0);
      SavedState saved;
      saved.Capture(ctx);

      const Settings& s = gMap.settings;
      float vsConst[32];   // ViewProj, ShadowMatrix
      TerrainViewProj(camera, vsConst);
      float* const shadowMatrix = vsConst + 16;
      static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      std::memcpy(shadowMatrix, kIdentity, sizeof kIdentity);
      ID3D11ShaderResourceView* const shadowTexture = ShadowTexture(shadow);
      if (shadowTexture != nullptr) {
        std::memcpy(shadowMatrix, &shadow->mCamera.viewProjection, 16 * sizeof(float));
      }
      const Vector4f& eyeRow = camera.inverseView.r[3];
      const float eye[4] = {eyeRow.x, eyeRow.y, eyeRow.z, 1.0f};

      const bool hasWater = s.water > -9000.0f;
      const bool hazeOn = !hasWater && s.haveVoid && s.haze > -9000.0f;
      float psConst[32] = {};   // c0..c7 of the pixel shader: params, haze, void colour, shadow colour, material, sun direction, sun colour, eye
      const float params[4] = {1.0f, shadowTexture != nullptr ? 1.0f : 0.0f, s.water, 1.0f / std::max(1.0f, s.water - s.waterAbyss)};
      const float haze[4] = {s.haze, 1.0f / s.hazeSpan, s.waterTint, hazeOn ? 1.0f : 0.0f};
      const float voidColor[4] = {s.voidColor[0], s.voidColor[1], s.voidColor[2], 0.0f};
      std::memcpy(psConst + 0, params, 16);
      std::memcpy(psConst + 4, haze, 16);
      std::memcpy(psConst + 8, voidColor, 16);
      std::memcpy(psConst + 12, s.shadowColor, 16);
      std::memcpy(psConst + 20, s.sunDirection, 16);
      std::memcpy(psConst + 24, s.sunColor, 16);
      std::memcpy(psConst + 28, eye, 16);

      ctx->UpdateSubresource(gGpu.vsConst.Get(), 0, nullptr, vsConst, 0, 0);
      ctx->IASetInputLayout(gGpu.layout.Get());
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetShader(gGpu.vs.Get(), nullptr, 0);
      ctx->PSSetShader(gGpu.ps.Get(), nullptr, 0);
      ctx->VSSetConstantBuffers(0, 1, gGpu.vsConst.GetAddressOf());
      ctx->PSSetConstantBuffers(0, 1, gGpu.psConst.GetAddressOf());
      // both surfaces: the mesh is pulled a little towards the camera, so the height field shows only where it lies above the mesh (where the sim's ground and
      // the visible ground disagree) instead of the two z-fighting over every flat part
      ctx->RSSetState(gMode == Mode::Both ? gGpu.rasterBiased.Get() : gGpu.raster.Get());
      ctx->OMSetDepthStencilState(gGpu.depthWrite.Get(), 0);
      ctx->OMSetBlendState(gGpu.opaque.Get(), nullptr, 0xFFFFFFFFu);
      ID3D11SamplerState* samplers[kSamplerCount];
      for (int i = 0; i < kSamplerCount; ++i) {
        samplers[i] = gGpu.samplers[i].Get();
      }
      ctx->PSSetSamplers(0, kSamplerCount, samplers);
      SetTexture(ctx, 5, shadowTexture != nullptr ? shadowTexture : gGpu.white.Get());
      SetTexture(ctx, 6, gGpu.ramp ? gGpu.ramp.Get() : gGpu.white.Get());

      // slots: manifest order diffuse a, b, mask 0..2, normal a, b, specular a, b -> samplers 0, 1, 2, 3, 4, 7, 8
      static const UINT kSampler[7] = {0, 1, 2, 3, 4, 7, 8};
      for (std::size_t pi = 0; pi < gMap.patches.size(); ++pi) {
        const Patch& p = gMap.patches[pi];
        const GpuPatch& g = gGpu.patches[pi];
        if (!g.vb || p.triangleCount == 0 || p.sky) {
          continue;
        }
        const Material* m = MaterialOf(p);
        for (int k = 0; k < 7; ++k) {
          ID3D11ShaderResourceView* t = SlotTexture(p, k);
          if (t == nullptr && (k == 1 || k == 6) && m != nullptr) {
            t = SlotTexture(p, k == 1 ? 0 : 5);   // one layer only: blend it with itself
          }
          SetTexture(ctx, kSampler[k], t != nullptr ? t : (k < 2 ? gGpu.white.Get() : k < 5 ? gGpu.gray.Get() : gGpu.flatNormal.Get()));
        }
        ID3D11ShaderResourceView* const maskA = SlotTexture(p, 7);
        ID3D11ShaderResourceView* const maskB = SlotTexture(p, 8);
        const bool glow = m != nullptr && m->glow && maskA != nullptr;
        SetTexture(ctx, 9, maskA != nullptr ? maskA : gGpu.white.Get());
        SetTexture(ctx, 10, maskB != nullptr ? maskB : (maskA != nullptr ? maskA : gGpu.white.Get()));
        const float material[4] = {m != nullptr && m->blending ? 1.0f : 0.0f, glow ? 1.0f : 0.0f, m != nullptr && m->highlight ? 0.5f : 0.0f,
                                   m != nullptr && m->alphaTest >= 0.0f ? m->alphaTest + 1.0f : -1.0f};
        std::memcpy(psConst + 16, material, 16);
        ctx->UpdateSubresource(gGpu.psConst.Get(), 0, nullptr, psConst, 0, 0);
        DrawPatch(ctx, g, p);
      }

      // where no ground was drawn: water to the horizon, or the haze a canyon map fades into (the quad sits at the far plane)
      if ((hasWater && gGpu.ramp) || hazeOn) {
        const float y = hasWater ? s.water : s.haze - s.hazeSpan, lo = -8.0f * s.size, hi = 9.0f * s.size;
        const float quad[4][3] = {{lo, y, lo}, {hi, y, lo}, {lo, y, hi}, {hi, y, hi}};
        const float fill[4] = {s.voidColor[0], s.voidColor[1], s.voidColor[2], hasWater && gGpu.ramp ? 1.0f : 0.0f};
        ctx->UpdateSubresource(gGpu.fillVb.Get(), 0, nullptr, quad, 0, 0);
        std::memcpy(psConst, fill, 16);
        ctx->UpdateSubresource(gGpu.psConst.Get(), 0, nullptr, psConst, 0, 0);
        const UINT stride = sizeof quad[0];
        const UINT offset = 0;
        ctx->IASetInputLayout(gGpu.fillLayout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->IASetVertexBuffers(0, 1, gGpu.fillVb.GetAddressOf(), &stride, &offset);
        ctx->VSSetShader(gGpu.fillVs.Get(), nullptr, 0);
        ctx->PSSetShader(gGpu.fillPs.Get(), nullptr, 0);
        ctx->OMSetDepthStencilState(gGpu.depthTest.Get(), 0);
        ctx->Draw(4, 0);
      }

      // SC2's sky, only where nothing else is: painted in the manifest's order without depth writes
      bool anySky = false;
      for (const Patch& p : gMap.patches) {
        anySky = anySky || p.sky;
      }
      if (anySky) {
        float skyConst[32] = {};   // ViewProj, Eye, Centre
        std::memcpy(skyConst, vsConst, 16 * sizeof(float));
        std::memcpy(skyConst + 16, eye, 16);
        const float centre[4] = {s.size * 0.5f, 0.0f, s.size * 0.5f, 0.0f};
        std::memcpy(skyConst + 20, centre, 16);
        ctx->UpdateSubresource(gGpu.vsConst.Get(), 0, nullptr, skyConst, 0, 0);
        ctx->IASetInputLayout(gGpu.skyLayout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(gGpu.skyVs.Get(), nullptr, 0);
        ctx->PSSetShader(gGpu.skyPs.Get(), nullptr, 0);
        ctx->OMSetDepthStencilState(gGpu.depthTest.Get(), 0);
        ID3D11SamplerState* const skySampler = gGpu.skySampler.Get();
        ctx->PSSetSamplers(0, 1, &skySampler);
        for (std::size_t pi = 0; pi < gMap.patches.size(); ++pi) {
          const Patch& p = gMap.patches[pi];
          const GpuPatch& g = gGpu.patches[pi];
          if (!g.vb || p.triangleCount == 0 || !p.sky) {
            continue;
          }
          const Material* m = MaterialOf(p);
          ID3D11ShaderResourceView* const t = SlotTexture(p, 0);
          SetTexture(ctx, 0, t != nullptr ? t : gGpu.white.Get());
          ctx->OMSetBlendState(m != nullptr && m->additive ? gGpu.skyAdditive.Get() : gGpu.skyAlpha.Get(), nullptr, 0xFFFFFFFFu);
          DrawPatch(ctx, g, p);
        }
      }
      saved.Restore(ctx);
      QueryPerformanceCounter(&t1);
      QueryPerformanceFrequency(&freq);
      gLastDrawMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
      return true;
    }

    // ---- the shadow map -------------------------------------------------------------------------------------------------------------

    bool DrawDepth(const GeomCamera3& lightCamera)
    {
      if (!MeshWanted()) {
        return false;
      }
      ID3D11Device* const dev = RawDevice();
      ID3D11DeviceContext* const ctx = RawContext();
      if (dev == nullptr || ctx == nullptr || !EnsureResources(dev, nullptr)) {
        return false;
      }
      SavedState saved;
      saved.Capture(ctx);
      float viewProj[32] = {};
      std::memcpy(viewProj, &lightCamera.viewProjection, 16 * sizeof(float));
      ctx->UpdateSubresource(gGpu.vsConst.Get(), 0, nullptr, viewProj, 0, 0);
      ctx->IASetInputLayout(gGpu.depthLayout.Get());
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetShader(gGpu.depthVs.Get(), nullptr, 0);
      ctx->PSSetShader(gGpu.depthPs.Get(), nullptr, 0);
      ctx->VSSetConstantBuffers(0, 1, gGpu.vsConst.GetAddressOf());
      ctx->PSSetConstantBuffers(0, 1, gGpu.psConst.GetAddressOf());
      ctx->RSSetState(gGpu.raster.Get());
      ctx->OMSetDepthStencilState(gGpu.depthWrite.Get(), 0);
      ctx->OMSetBlendState(gGpu.opaque.Get(), nullptr, 0xFFFFFFFFu);
      ID3D11SamplerState* const sampler = gGpu.samplers[0].Get();   // tiled, linear
      ctx->PSSetSamplers(0, 1, &sampler);
      for (std::size_t pi = 0; pi < gMap.patches.size(); ++pi) {
        const Patch& p = gMap.patches[pi];
        const GpuPatch& g = gGpu.patches[pi];
        if (!g.vb || p.triangleCount == 0 || p.sky) {
          continue;
        }
        const Material* m = MaterialOf(p);
        ID3D11ShaderResourceView* const diffuse = SlotTexture(p, 0);
        const bool tested = m != nullptr && m->alphaTest >= 0.0f && diffuse != nullptr;
        float alphaTest[32] = {};
        alphaTest[0] = tested ? m->alphaTest + 1.0f : -1.0f;
        ctx->UpdateSubresource(gGpu.psConst.Get(), 0, nullptr, alphaTest, 0, 0);
        SetTexture(ctx, 0, tested ? diffuse : gGpu.white.Get());
        DrawPatch(ctx, g, p);
      }
      saved.Restore(ctx);
      return true;
    }
  } // namespace edmesh
} // namespace moho
