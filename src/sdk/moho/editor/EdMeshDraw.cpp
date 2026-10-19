// Mesh terrain renderer of the editor (docs/projects/editor/MESH_VIEW.md). The shaders and the draw order follow automation/terrainmesh/meshterrain.cpp (the patch
// for the retail exe), which in turn follows SC2's terrain.hlsl: two blended layers, radiosity normal mapping with three baked light masks, sun occlusion in the
// alpha of mask 0, the layer blend in the alpha of mask 1, FA's shadow map for dynamic shadows, FA's water ramp below the water level.
//
// Differences to the patch: the camera and the shadow state come from the engine objects (GeomCamera3, Shadow) instead of being read back from shader constants,
// so the matrices are used as the engine stores them (row vector convention, `row_major` in HLSL). The raw Direct3D 9 device is the gal device's own; every draw is
// wrapped in a state block capture / apply so the engine's state cache stays valid.

#include "moho/editor/EdMeshTerrain.h"

#include <windows.h>

#include <d3d9.h>
#include <d3dx9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "boost/shared_ptr.h"
#include "gpg/gal/backends/d3d9/DeviceD3D9.hpp"
#include "gpg/gal/backends/d3d9/RenderTargetD3D9.hpp"
#include "moho/render/Shadow.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/CD3DRenderTarget.h"
#include "moho/sim/STIMap.h"
#include "moho/terrain/water/CWaterShaderProperties.h"

namespace moho
{
  namespace edmesh
  {
    namespace
    {
      // ---- shaders ---------------------------------------------------------------------------------------------------------------------

      const char kVertexShader[] = R"(
row_major float4x4 ViewProj : register(c0);
row_major float4x4 ShadowMatrix : register(c4);

struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; float3 tangent : TANGENT; float3 binormal : BINORMAL;
               float2 uv : TEXCOORD0; float2 uv2 : TEXCOORD1; };
struct VSOut { float4 pos : POSITION; float4 uv : TEXCOORD0; float4 shadow : TEXCOORD1; float3 world : TEXCOORD2;
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
float4 Params : register(c0);      // x exposure, y 1 when FA's shadow map is bound, z water level, w 1 / depth span
float4 Haze : register(c1);        // x level below which the ground fades out, y 1 / fade span, z water tint, w 1 when in use
float4 VoidColor : register(c2);
float4 ShadowColor : register(c3); // SC2's fill light in shadow; a: how far a dynamic shadow pulls towards it
float4 Material : register(c4);    // x 1: layers blend, y 1: glows, z: sun highlight strength, w: alpha test reference + 1 or -1
float4 SunDirection : register(c5);
float4 SunColor : register(c6);
float4 ViewPosition : register(c7);
struct VSOut { float4 pos : POSITION; float4 uv : TEXCOORD0; float4 shadow : TEXCOORD1; float3 world : TEXCOORD2;
               float3 normal : TEXCOORD3; float3 tangent : TEXCOORD4; float3 binormal : TEXCOORD5; };

sampler DiffuseA : register(s0);
sampler DiffuseB : register(s1);
sampler Light0 : register(s2);
sampler Light1 : register(s3);
sampler Light2 : register(s4);
sampler Shadow : register(s5);
sampler WaterRamp : register(s6);
sampler NormalA : register(s7);
sampler NormalB : register(s8);
sampler Mask : register(s9);
sampler MaskB : register(s10);

float3 Linear(float3 c) { return pow(max(c, 0.0001), 2.2); }

float4 PS(VSOut i) : COLOR {
    float4 l0 = tex2D(Light0, i.uv.zw);
    float4 l1 = tex2D(Light1, i.uv.zw);
    float4 l2 = tex2D(Light2, i.uv.zw);
    float blend = l1.a * Material.x;
    float4 diffuse = lerp(tex2D(DiffuseA, i.uv.xy), tex2D(DiffuseB, i.uv.xy), blend);
    clip(diffuse.a * 255.0 - Material.w);
    float3 albedo = Linear(diffuse.rgb);
    float3 n = normalize(lerp(tex2D(NormalA, i.uv.xy).rgb, tex2D(NormalB, i.uv.xy).rgb, blend) * 2.0 - 1.0);
    float3 dp = saturate(float3(dot(n, float3(0.81649658, 0.0, 0.57735027)),
                                dot(n, float3(-0.40824829, 0.70710678, 0.57735027)),
                                dot(n, float3(-0.40824829, -0.70710678, 0.57735027))));
    float3 light = dp.x * Linear(l0.rgb) + dp.y * Linear(l1.rgb) + dp.z * Linear(l2.rgb);
    float sun = lerp(1.0, tex2D(Shadow, i.shadow.xy / i.shadow.w).g, Params.y);
    float3 shade = min(ShadowColor.rgb, light * 0.4);
    light = lerp(light, shade, (1.0 - sun) * ShadowColor.a * max(l0.a, 0.5));
    float4 mask = lerp(tex2D(Mask, i.uv.xy), tex2D(MaskB, i.uv.xy), blend);
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
    float4 water = tex2D(WaterRamp, float2(depth, 0.5));
    color = lerp(color, water.rgb, water.a * step(i.world.y, Params.z) * Haze.z);
    float haze = saturate((Haze.x - i.world.y) * Haze.y) * Haze.w;
    color = lerp(color, VoidColor.rgb, haze);
    return float4(color, 0.4 * glow * (1.0 - haze));
}
)";

      const char kDepthVertexShader[] = R"(
row_major float4x4 ViewProj : register(c0);
struct VSOut { float4 pos : POSITION; float depth : TEXCOORD0; float2 uv : TEXCOORD1; };
VSOut VS(float3 pos : POSITION, float2 uv : TEXCOORD0) {
    VSOut o;
    o.pos = mul(float4(pos, 1.0), ViewProj);
    o.depth = o.pos.z / o.pos.w;
    o.uv = uv;
    return o;
}
)";

      const char kDepthPixelShader[] = R"(
float4 AlphaTest : register(c0);
sampler Diffuse : register(s0);
float4 PS(float depth : TEXCOORD0, float2 uv : TEXCOORD1) : COLOR {
    clip(tex2D(Diffuse, uv).a * 255.0 - AlphaTest.x);
    return float4(depth, 1.0, 0.0, 1.0);
}
)";

      // SC2's sky: position - map centre + camera, kept just in front of the far plane, alpha blended, unlit.
      const char kSkyVertexShader[] = R"(
row_major float4x4 ViewProj : register(c0);
float4 Eye : register(c4);
float4 Centre : register(c5);
struct VSIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float facing : TEXCOORD1; };
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
sampler Diffuse : register(s0);
float4 PS(float2 uv : TEXCOORD0, float facing : TEXCOORD1) : COLOR {
    clip(facing);
    return tex2D(Diffuse, uv);
}
)";

      // ---- state -----------------------------------------------------------------------------------------------------------------------

      MeshMap gMap;
      GeomCamera3 gCamera;
      bool gHaveCamera = false;
      unsigned gGeneration = 1;
      Mode gMode = Mode::Mesh;
      double gLastDrawMs = 0.0;

      struct GpuPatch
      {
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9* ib = nullptr;
      };

      struct Gpu
      {
        IDirect3DDevice9* device = nullptr;
        unsigned generation = 0;
        unsigned failedGeneration = 0;
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexShader9* depthVs = nullptr;
        IDirect3DPixelShader9* depthPs = nullptr;
        IDirect3DVertexShader9* skyVs = nullptr;
        IDirect3DPixelShader9* skyPs = nullptr;
        IDirect3DVertexDeclaration9* decl = nullptr;
        IDirect3DTexture9* white = nullptr;
        IDirect3DTexture9* gray = nullptr;
        IDirect3DTexture9* flatNormal = nullptr;
        IDirect3DTexture9* ramp = nullptr;
        std::string rampPath;
        std::vector<GpuPatch> patches;
        std::vector<IDirect3DTexture9*> textures;     // material * kSlots + slot
      };
      Gpu gGpu;

      template <class T>
      void Release(T*& p)
      {
        if (p != nullptr) {
          p->Release();
          p = nullptr;
        }
      }

      void ReleaseAll()
      {
        for (GpuPatch& p : gGpu.patches) {
          Release(p.vb);
          Release(p.ib);
        }
        gGpu.patches.clear();
        for (IDirect3DTexture9*& t : gGpu.textures) {
          Release(t);
        }
        gGpu.textures.clear();
        Release(gGpu.vs);
        Release(gGpu.ps);
        Release(gGpu.depthVs);
        Release(gGpu.depthPs);
        Release(gGpu.skyVs);
        Release(gGpu.skyPs);
        Release(gGpu.decl);
        Release(gGpu.white);
        Release(gGpu.gray);
        Release(gGpu.flatNormal);
        Release(gGpu.ramp);
        gGpu.rampPath.clear();
        gGpu.device = nullptr;
        gGpu.generation = 0;
      }

      IDirect3DDevice9* RawDevice()
      {
        CD3DDevice* const device = D3D_GetDevice();
        if (device == nullptr || device->GetGalDevice() == nullptr) {
          return nullptr;
        }
        return static_cast<gpg::gal::DeviceD3D9*>(device->GetGalDevice())->mDevice;
      }

      bool Compile(IDirect3DDevice9* dev, const char* source, const char* profile, IDirect3DVertexShader9** vs, IDirect3DPixelShader9** ps)
      {
        ID3DXBuffer* code = nullptr;
        ID3DXBuffer* errors = nullptr;
        const HRESULT hr = D3DXCompileShader(source, static_cast<UINT>(std::strlen(source)), nullptr, nullptr, "VS", profile, 0, &code, &errors, nullptr);
        if (FAILED(hr) || code == nullptr) {
          std::fprintf(stderr, "[edmesh] shader compile failed: %s\n", errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
          if (errors != nullptr) {
            errors->Release();
          }
          return false;
        }
        const DWORD* bytes = static_cast<const DWORD*>(code->GetBufferPointer());
        const HRESULT made = vs != nullptr ? dev->CreateVertexShader(bytes, vs) : dev->CreatePixelShader(bytes, ps);
        code->Release();
        if (errors != nullptr) {
          errors->Release();
        }
        return SUCCEEDED(made);
      }

      bool CompilePs(IDirect3DDevice9* dev, const char* source, const char* profile, IDirect3DPixelShader9** ps)
      {
        ID3DXBuffer* code = nullptr;
        ID3DXBuffer* errors = nullptr;
        const HRESULT hr = D3DXCompileShader(source, static_cast<UINT>(std::strlen(source)), nullptr, nullptr, "PS", profile, 0, &code, &errors, nullptr);
        if (FAILED(hr) || code == nullptr) {
          std::fprintf(stderr, "[edmesh] pixel shader compile failed: %s\n", errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
          if (errors != nullptr) {
            errors->Release();
          }
          return false;
        }
        const HRESULT made = dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), ps);
        code->Release();
        if (errors != nullptr) {
          errors->Release();
        }
        return SUCCEEDED(made);
      }

      IDirect3DTexture9* LoadTexture(IDirect3DDevice9* dev, const std::string& vfsPath, const bool oneLevel)
      {
        const std::vector<std::uint8_t> bytes = ReadVfs(vfsPath);
        if (bytes.empty()) {
          return nullptr;
        }
        IDirect3DTexture9* tex = nullptr;
        // a full mip chain: the file's levels, missing ones made from the top level (SC2's light masks have one level)
        const HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(dev, bytes.data(), static_cast<UINT>(bytes.size()), D3DX_DEFAULT, D3DX_DEFAULT,
                                                               oneLevel ? 1U : 0U, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0,
                                                               nullptr, nullptr, &tex);
        return SUCCEEDED(hr) ? tex : nullptr;
      }

      IDirect3DTexture9* Solid(IDirect3DDevice9* dev, const std::uint32_t argb)
      {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) {
          return nullptr;
        }
        D3DLOCKED_RECT r;
        if (SUCCEEDED(t->LockRect(0, &r, nullptr, 0))) {
          *static_cast<std::uint32_t*>(r.pBits) = argb;
          t->UnlockRect(0);
        }
        return t;
      }

      bool CreateResources(IDirect3DDevice9* dev, const CWaterShaderProperties* water)
      {
        ReleaseAll();
        gGpu.device = dev;
        if (!Compile(dev, kVertexShader, "vs_3_0", &gGpu.vs, nullptr) || !CompilePs(dev, kPixelShader, "ps_3_0", &gGpu.ps) ||
            !Compile(dev, kDepthVertexShader, "vs_2_0", &gGpu.depthVs, nullptr) || !CompilePs(dev, kDepthPixelShader, "ps_2_0", &gGpu.depthPs) ||
            !Compile(dev, kSkyVertexShader, "vs_2_0", &gGpu.skyVs, nullptr) || !CompilePs(dev, kSkyPixelShader, "ps_2_0", &gGpu.skyPs)) {
          return false;
        }
        const D3DVERTEXELEMENT9 elements[] = {
          {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
          {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
          {0, 36, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0},
          {0, 48, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
          {0, 56, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
          D3DDECL_END()};
        if (FAILED(dev->CreateVertexDeclaration(elements, &gGpu.decl))) {
          return false;
        }
        // alpha 0: as a water ramp white tints nothing, as light mask 0 gray shows the first layer only
        gGpu.white = Solid(dev, 0x00FFFFFFu);
        gGpu.gray = Solid(dev, 0x00C7C7C7u);
        gGpu.flatNormal = Solid(dev, 0xFF8080FFu);
        if (gGpu.white == nullptr || gGpu.gray == nullptr || gGpu.flatNormal == nullptr) {
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
          void* dst = nullptr;
          if (FAILED(dev->CreateVertexBuffer(p.vertexCount * 68, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g.vb, nullptr)) || FAILED(g.vb->Lock(0, 0, &dst, 0))) {
            return false;
          }
          std::memcpy(dst, p.vertices.data(), p.vertexCount * 68ull);
          g.vb->Unlock();
          if (FAILED(dev->CreateIndexBuffer(static_cast<UINT>(p.indices.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g.ib, nullptr)) ||
              FAILED(g.ib->Lock(0, 0, &dst, 0))) {
            return false;
          }
          std::memcpy(dst, p.indices.data(), p.indices.size() * 2);
          g.ib->Unlock();
        }
        gGpu.generation = gGeneration;
        return true;
      }

      bool EnsureResources(IDirect3DDevice9* dev, const CWaterShaderProperties* water)
      {
        if (gGpu.device == dev && gGpu.generation == gGeneration) {
          return true;
        }
        if (gGpu.failedGeneration == gGeneration && gGpu.device == nullptr) {
          return false;
        }
        if (!CreateResources(dev, water)) {
          std::fprintf(stderr, "[edmesh] resources could not be created: the height field stays\n");
          ReleaseAll();
          gGpu.failedGeneration = gGeneration;
          return false;
        }
        return true;
      }

      const Material* MaterialOf(const Patch& p)
      {
        return p.material >= 0 && p.material < static_cast<int>(gMap.materials.size()) ? &gMap.materials[static_cast<std::size_t>(p.material)] : nullptr;
      }

      IDirect3DTexture9* SlotTexture(const Patch& p, const int slot)
      {
        if (p.material < 0 || p.material >= static_cast<int>(gMap.materials.size())) {
          return nullptr;
        }
        return gGpu.textures[static_cast<std::size_t>(p.material) * kSlots + static_cast<std::size_t>(slot)];
      }

      IDirect3DTexture9* ShadowTexture(Shadow* shadow)
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
        return surface != nullptr ? static_cast<gpg::gal::RenderTargetD3D9*>(surface)->GetTexture() : nullptr;
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

      void SetRenderStates(IDirect3DDevice9* dev)
      {
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        dev->SetStreamSourceFreq(0, 1);
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

    // ---- the surface pass -----------------------------------------------------------------------------------------------------------

    bool DrawSurface(const GeomCamera3& camera, Shadow* const shadow, const CWaterShaderProperties* const water)
    {
      if (!MeshWanted()) {
        return false;
      }
      IDirect3DDevice9* const dev = RawDevice();
      if (dev == nullptr || !EnsureResources(dev, water)) {
        return false;
      }
      LARGE_INTEGER t0, t1, freq;
      QueryPerformanceCounter(&t0);
      IDirect3DStateBlock9* saved = nullptr;
      if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) {
        return false;
      }
      const Settings& s = gMap.settings;
      float viewProj[16];
      TerrainViewProj(camera, viewProj);
      float shadowMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      IDirect3DTexture9* const shadowTexture = ShadowTexture(shadow);
      if (shadowTexture != nullptr) {
        std::memcpy(shadowMatrix, &shadow->mCamera.viewProjection, sizeof shadowMatrix);
      }
      const Vector4f& eyeRow = camera.inverseView.r[3];
      const float eye[4] = {eyeRow.x, eyeRow.y, eyeRow.z, 1.0f};

      dev->SetVertexDeclaration(gGpu.decl);
      dev->SetVertexShader(gGpu.vs);
      dev->SetPixelShader(gGpu.ps);
      dev->SetVertexShaderConstantF(0, viewProj, 4);
      dev->SetVertexShaderConstantF(4, shadowMatrix, 4);
      const bool hasWater = s.water > -9000.0f;
      const bool hazeOn = !hasWater && s.haveVoid && s.haze > -9000.0f;
      const float params[4] = {1.0f, shadowTexture != nullptr ? 1.0f : 0.0f, s.water, 1.0f / std::max(1.0f, s.water - s.waterAbyss)};
      const float haze[4] = {s.haze, 1.0f / s.hazeSpan, s.waterTint, hazeOn ? 1.0f : 0.0f};
      const float voidColor[4] = {s.voidColor[0], s.voidColor[1], s.voidColor[2], 0.0f};
      dev->SetPixelShaderConstantF(0, params, 1);
      dev->SetPixelShaderConstantF(1, haze, 1);
      dev->SetPixelShaderConstantF(2, voidColor, 1);
      dev->SetPixelShaderConstantF(3, s.shadowColor, 1);
      dev->SetPixelShaderConstantF(5, s.sunDirection, 1);
      dev->SetPixelShaderConstantF(6, s.sunColor, 1);
      dev->SetPixelShaderConstantF(7, eye, 1);
      SetRenderStates(dev);
      for (DWORD i = 0; i < 11; ++i) {
        // 0-1 tiled layers, 2-4 light masks, 5 shadow map (outside it: in the sun), 6 water ramp, 7-8 normal maps, 9-10 the masks (glow, highlight)
        const bool tiled = i < 2 || i > 6;
        const DWORD address = tiled ? D3DTADDRESS_WRAP : i == 5 ? D3DTADDRESS_BORDER : D3DTADDRESS_CLAMP;
        dev->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(i, D3DSAMP_MIPFILTER, i < 5 || i > 6 ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSU, address);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSV, address);
        dev->SetSamplerState(i, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);
        dev->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, FALSE);
      }
      dev->SetTexture(5, shadowTexture != nullptr ? shadowTexture : gGpu.white);
      dev->SetTexture(6, gGpu.ramp != nullptr ? gGpu.ramp : gGpu.white);

      // slots: manifest order diffuse a, b, mask 0..2, normal a, b, specular a, b -> samplers 0, 1, 2, 3, 4, 7, 8
      static const DWORD kSampler[7] = {0, 1, 2, 3, 4, 7, 8};
      for (std::size_t pi = 0; pi < gMap.patches.size(); ++pi) {
        const Patch& p = gMap.patches[pi];
        const GpuPatch& g = gGpu.patches[pi];
        if (g.vb == nullptr || p.triangleCount == 0 || p.sky) {
          continue;
        }
        const Material* m = MaterialOf(p);
        for (int k = 0; k < 7; ++k) {
          IDirect3DTexture9* t = SlotTexture(p, k);
          if (t == nullptr && (k == 1 || k == 6) && m != nullptr) {
            t = SlotTexture(p, k == 1 ? 0 : 5);   // one layer only: blend it with itself
          }
          dev->SetTexture(kSampler[k], t != nullptr ? t : (k < 2 ? gGpu.white : k < 5 ? gGpu.gray : gGpu.flatNormal));
        }
        IDirect3DTexture9* const maskA = SlotTexture(p, 7);
        IDirect3DTexture9* const maskB = SlotTexture(p, 8);
        const bool glow = m != nullptr && m->glow && maskA != nullptr;
        dev->SetTexture(9, maskA != nullptr ? maskA : gGpu.white);
        dev->SetTexture(10, maskB != nullptr ? maskB : (maskA != nullptr ? maskA : gGpu.white));
        const float material[4] = {m != nullptr && m->blending ? 1.0f : 0.0f, glow ? 1.0f : 0.0f, m != nullptr && m->highlight ? 0.5f : 0.0f,
                                   m != nullptr && m->alphaTest >= 0.0f ? m->alphaTest + 1.0f : -1.0f};
        dev->SetPixelShaderConstantF(4, material, 1);
        dev->SetStreamSource(0, g.vb, 0, 68);
        dev->SetIndices(g.ib);
        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, p.vertexCount, 0, p.triangleCount);
      }

      // SC2's sky, only where nothing else is: painted in the manifest's order without depth writes
      bool anySky = false;
      for (const Patch& p : gMap.patches) {
        anySky = anySky || p.sky;
      }
      if (anySky) {
        const float centre[4] = {s.size * 0.5f, 0.0f, s.size * 0.5f, 0.0f};
        dev->SetVertexDeclaration(gGpu.decl);
        dev->SetVertexShader(gGpu.skyVs);
        dev->SetPixelShader(gGpu.skyPs);
        dev->SetVertexShaderConstantF(0, viewProj, 4);
        dev->SetVertexShaderConstantF(4, eye, 1);
        dev->SetVertexShaderConstantF(5, centre, 1);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);   // alpha is FA's glow: leave it
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        for (std::size_t pi = 0; pi < gMap.patches.size(); ++pi) {
          const Patch& p = gMap.patches[pi];
          const GpuPatch& g = gGpu.patches[pi];
          if (g.vb == nullptr || p.triangleCount == 0 || !p.sky) {
            continue;
          }
          const Material* m = MaterialOf(p);
          IDirect3DTexture9* t = SlotTexture(p, 0);
          dev->SetTexture(0, t != nullptr ? t : gGpu.white);
          dev->SetRenderState(D3DRS_DESTBLEND, m != nullptr && m->additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
          dev->SetStreamSource(0, g.vb, 0, 68);
          dev->SetIndices(g.ib);
          dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, p.vertexCount, 0, p.triangleCount);
        }
      }
      saved->Apply();
      saved->Release();
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
      IDirect3DDevice9* const dev = RawDevice();
      if (dev == nullptr || !EnsureResources(dev, nullptr)) {
        return false;
      }
      IDirect3DStateBlock9* saved = nullptr;
      if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) {
        return false;
      }
      float viewProj[16];
      std::memcpy(viewProj, &lightCamera.viewProjection, sizeof viewProj);
      dev->SetVertexDeclaration(gGpu.decl);
      dev->SetVertexShader(gGpu.depthVs);
      dev->SetPixelShader(gGpu.depthPs);
      dev->SetVertexShaderConstantF(0, viewProj, 4);
      SetRenderStates(dev);
      dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
      dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
      dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
      for (std::size_t pi = 0; pi < gMap.patches.size(); ++pi) {
        const Patch& p = gMap.patches[pi];
        const GpuPatch& g = gGpu.patches[pi];
        if (g.vb == nullptr || p.triangleCount == 0 || p.sky) {
          continue;
        }
        const Material* m = MaterialOf(p);
        IDirect3DTexture9* const diffuse = SlotTexture(p, 0);
        const bool tested = m != nullptr && m->alphaTest >= 0.0f && diffuse != nullptr;
        const float alphaTest[4] = {tested ? m->alphaTest + 1.0f : -1.0f, 0.0f, 0.0f, 0.0f};
        dev->SetPixelShaderConstantF(0, alphaTest, 1);
        dev->SetTexture(0, tested ? diffuse : gGpu.white);
        dev->SetStreamSource(0, g.vb, 0, 68);
        dev->SetIndices(g.ib);
        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, p.vertexCount, 0, p.triangleCount);
      }
      saved->Apply();
      saved->Release();
      return true;
    }
  } // namespace edmesh
} // namespace moho
