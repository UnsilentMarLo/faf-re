#include "moho/render/lights/LightField.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/GpuDriven.h"
#include "gpg/gal/OutputContext.hpp"
#include "gpg/gal/RenderTarget.hpp"
#include "gpg/gal/RenderTargetContext.hpp"
#include "gpg/gal/RenderProfiler.h"
#include "moho/console/CConCommand.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/lights/LightCollector.h"
#include "moho/sim/STIMap.h"

namespace moho::lights
{
  int ren_DynLightsMode = 0;

  static TConVar<int> gTConVar_ren_DynLightsMode(
    "ren_DynLightsMode",
    "How the dynamic lights reach the shaders: 0 the light grid (every pixel loops over the lights of its cell), 1 the irradiance field "
    "(a compute pass lights a world-fixed field once, the terrain and mesh shaders sample it). docs/LIGHTING_V2.md.",
    &ren_DynLightsMode
  );

  namespace
  {
    using namespace gpg::gal;

    constexpr std::uint32_t kCells = 512;       // cells of the field per side
    constexpr std::uint32_t kLayers = 4;        // heights above the terrain
    constexpr std::uint32_t kTile = 8;          // cells per side of a thread group
    constexpr std::uint32_t kHeightSamples = 129; // terrain height samples per side (bilinear in the shader)
    constexpr std::uint32_t kAtlasFormat = 9U;  // gal render target format: RGBA16F (DxgiFormats.cpp)
    constexpr float kLayerHeights[kLayers] = {0.5f, 3.0f, 10.0f, 30.0f};
    constexpr float kCellLadder[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};

    struct FieldConstants
    {
      float origin[4]; // originX, originZ, cellSize, lightCount
      float dims[4];   // cells, layers, height samples per side, world units per height sample
      float layerH[4];
      float misc[4];
    };

    // One source for the three programs: the compute pass, the fullscreen vertex shader and the copy into the atlas.
    constexpr char kSource[] = R"HLSL(
cbuffer FieldConstants : register(b0)
{
    float4 gOrigin;   // originX, originZ, cellSize, lightCount
    float4 gDims;     // cells, layers, height samples per side, world units per height sample
    float4 gLayerH;
    float4 gMisc;
};

StructuredBuffer<float4> Lights : register(t0);   // three float4 per light: LightGrid.h's data layout
StructuredBuffer<float> Heights : register(t1);
RWStructuredBuffer<float4> Field : register(u0);

#define CHUNK 1024
groupshared uint sList[CHUNK];
groupshared uint sCount;

float GroundAt(float2 xz)
{
    float n = gDims.z;
    float2 g = clamp((xz - gOrigin.xy) / gDims.w, 0.0, n - 1.001);
    uint2 i = (uint2)g;
    float2 f = g - (float2)i;
    uint w = (uint)n;
    float h00 = Heights[i.y * w + i.x];
    float h10 = Heights[i.y * w + i.x + 1];
    float h01 = Heights[(i.y + 1) * w + i.x];
    float h11 = Heights[(i.y + 1) * w + i.x + 1];
    return lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
}

[numthreads(8, 8, 1)]
void FieldCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    uint cells = (uint)gDims.x;
    uint layer = gid.z;
    uint2 cell = gid.xy * 8 + tid.xy;
    float cs = gOrigin.z;
    float2 xz = gOrigin.xy + ((float2)cell + 0.5) * cs;
    float ground = GroundAt(xz);
    float3 p = float3(xz.x, ground + gLayerH[layer], xz.y);
    float2 tileMin = gOrigin.xy + (float2)(gid.xy * 8) * cs;
    float2 tileMax = tileMin + 8.0 * cs;

    float3 E = float3(0, 0, 0);
    float3 D = float3(0, 0, 0);
    float W = 0.0;
    uint total = (uint)gOrigin.w;
    for (uint basis = 0; basis < total; basis += CHUNK)
    {
        if (index == 0)
        {
            sCount = 0;
        }
        GroupMemoryBarrierWithGroupSync();
        uint last = min(basis + CHUNK, total);
        for (uint i = basis + index; i < last; i += 64)
        {
            // the light's footprint (its segment grown by the radius) against the tile, in the ground plane
            float4 a = Lights[i * 3];
            float4 c = Lights[i * 3 + 2];
            float r = abs(a.w);
            float3 e = a.w < 0.0 ? a.xyz : c.xyz;   // a spotlight stores its direction in c: its footprint is the circle around the position
            float2 lo = min(a.xz, e.xz) - r;
            float2 hi = max(a.xz, e.xz) + r;
            if (hi.x >= tileMin.x && hi.y >= tileMin.y && lo.x <= tileMax.x && lo.y <= tileMax.y)
            {
                uint slot;
                InterlockedAdd(sCount, 1, slot);
                sList[slot] = i;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        uint count = sCount;
        for (uint k = 0; k < count; ++k)
        {
            uint li = sList[k];
            float4 a = Lights[li * 3];
            float4 b = Lights[li * 3 + 1];
            float4 c = Lights[li * 3 + 2];
            bool spot = a.w < 0.0;
            float r = max(abs(a.w), 0.001);
            float3 seg = spot ? float3(0, 0, 0) : c.xyz - a.xyz;
            float t = saturate(dot(p - a.xyz, seg) / max(dot(seg, seg), 0.0001));
            float3 toLight = a.xyz + seg * t - p;
            float atten = saturate(1.0 - dot(toLight, toLight) / (r * r));
            atten *= 0.35 + 0.65 * atten;
            if (spot)
            {
                float cd = dot(-normalize(toLight), c.xyz);
                atten *= saturate((cd - c.w) / max((1.0 - c.w) * 0.5, 0.0001));
            }
            if (atten > 0.002)
            {
                float3 colour = b.rgb * atten;
                float lum = dot(colour, float3(0.3, 0.59, 0.11));
                float len = length(toLight);
                E += colour;
                D += toLight * (lum / max(len, 0.001));
                W += lum;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    uint at = ((layer * cells + cell.y) * cells + cell.x) * 2;
    Field[at] = float4(E, ground);
    Field[at + 1] = float4(D, W);
}

StructuredBuffer<float4> FieldIn : register(t0);

float4 FieldVS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

float4 FieldCopyPS(float4 position : SV_Position) : SV_Target
{
    uint cells = (uint)gDims.x;
    uint2 p = (uint2)position.xy;
    uint layer = p.x / cells;
    uint cx = p.x - layer * cells;
    uint part = p.y / cells;
    uint cy = p.y - part * cells;
    return FieldIn[((layer * cells + cy) * cells + cx) * 2 + part];
}
)HLSL";

    struct State
    {
      FieldFrame frame;
      FieldStats stats;
      boost::shared_ptr<GpuBuffer> lightBuffer;
      boost::shared_ptr<GpuBuffer> heightBuffer;
      boost::shared_ptr<GpuBuffer> fieldBuffer;
      boost::shared_ptr<GpuProgram> compute;
      boost::shared_ptr<GpuProgram> copy;
      Device* device = nullptr;
      bool failed = false;
      bool loggedRefusal = false;
      // what the height buffer holds
      float heightOriginX = 0.0f;
      float heightOriginZ = 0.0f;
      float heightCell = 0.0f;
      std::uint32_t heightAge = 1000U;
    };

    State& GetState()
    {
      static State* const state = new State();
      return *state;
    }

    bool EnsureResources(State& s, Device& device)
    {
      if (s.device != &device) {
        s = State();
        s.device = &device;
      }
      if (s.failed) {
        return false;
      }
      if (s.compute == nullptr || s.copy == nullptr) {
        GpuProgramDesc desc;
        desc.name = "LightFieldCompute";
        desc.source = kSource;
        desc.sourceBytes = sizeof(kSource) - 1U;
        desc.computeEntry = "FieldCS";
        s.compute = device.CreateGpuProgram(desc);
        GpuProgramDesc draw;
        draw.name = "LightFieldCopy";
        draw.source = kSource;
        draw.sourceBytes = sizeof(kSource) - 1U;
        draw.vertexEntry = "FieldVS";
        draw.pixelEntry = "FieldCopyPS";
        draw.blend = GpuBlend::Opaque;
        draw.sampler = GpuSampler::PointClamp;
        s.copy = device.CreateGpuProgram(draw);
        if (s.compute == nullptr || s.copy == nullptr) {
          gpg::Warnf("Light field disabled: its GPU programs did not compile");
          s.failed = true;
          return false;
        }
      }
      if (s.lightBuffer == nullptr) {
        GpuBufferDesc lights;
        lights.name = "LightFieldLights";
        lights.stride = 16U;
        lights.capacity = static_cast<std::uint32_t>(kMaxGridLights * kTexelsPerLight);
        s.lightBuffer = device.CreateGpuBuffer(lights);
        GpuBufferDesc heights;
        heights.name = "LightFieldHeights";
        heights.stride = 4U;
        heights.capacity = kHeightSamples * kHeightSamples;
        s.heightBuffer = device.CreateGpuBuffer(heights);
        GpuBufferDesc field;
        field.name = "LightFieldCells";
        field.stride = 16U;
        field.capacity = kCells * kCells * kLayers * 2U;
        field.shaderWrite = true;
        s.fieldBuffer = device.CreateGpuBuffer(field);
        if (s.lightBuffer == nullptr || s.heightBuffer == nullptr || s.fieldBuffer == nullptr) {
          gpg::Warnf("Light field disabled: its GPU buffers could not be created");
          s.failed = true;
          return false;
        }
      }
      if (s.frame.atlas == nullptr) {
        const RenderTargetContext context(kCells * kLayers, kCells * 2U, kAtlasFormat);
        s.frame.atlas = device.CreateRenderTarget(&context);
        if (s.frame.atlas == nullptr) {
          gpg::Warnf("Light field disabled: no %ux%u RGBA16F render target", kCells * kLayers, kCells * 2U);
          s.failed = true;
          return false;
        }
        gpg::Logf("Light field: %ux%u RGBA16F atlas, %u cells x %u layers, %zu light slots", kCells * kLayers, kCells * 2U, kCells, kLayers, kMaxGridLights);
      }
      return true;
    }

    // The ground rectangle the view sees: the corners' rays against the terrain (the camera's own centre ray when a corner looks past the horizon).
    bool ViewGround(const GeomCamera3& camera, const void* terrain, const ElevationFunction elevation, float (&minXZ)[2], float (&maxXZ)[2])
    {
      const float w = camera.viewport.r[3].z;
      const float h = camera.viewport.r[3].w;
      if (!(w > 0.0f && h > 0.0f)) {
        return false;
      }
      const float corners[5][2] = {{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}, {w * 0.5f, h * 0.5f}};
      bool any = false;
      for (const auto& corner : corners) {
        const GeomLine3 ray = camera.Unproject(Wm3::Vector2f(corner[0], corner[1]));
        float t = 1.0f;
        float lastX = ray.pos.x;
        float lastZ = ray.pos.z;
        bool hit = false;
        for (; t < 6000.0f; t += 6.0f) {
          const float x = ray.pos.x + ray.dir.x * t;
          const float y = ray.pos.y + ray.dir.y * t;
          const float z = ray.pos.z + ray.dir.z * t;
          lastX = x;
          lastZ = z;
          if (y <= elevation(terrain, x, z)) {
            hit = true;
            break;
          }
        }
        if (!hit && !any) {
          continue; // a corner above the horizon contributes the far point of its ray
        }
        if (!any) {
          minXZ[0] = maxXZ[0] = lastX;
          minXZ[1] = maxXZ[1] = lastZ;
          any = true;
        } else {
          minXZ[0] = std::min(minXZ[0], lastX);
          maxXZ[0] = std::max(maxXZ[0], lastX);
          minXZ[1] = std::min(minXZ[1], lastZ);
          maxXZ[1] = std::max(maxXZ[1], lastZ);
        }
      }
      return any;
    }
  } // namespace

  bool UpdateField(const GeomCamera3& camera, const LightGridImage& image, const void* terrain, const ElevationFunction elevation)
  {
    State& s = GetState();
    s.frame.params2[0] = 0.0f;
    s.frame.valid = false;
    if (ren_DynLightsMode != 1 || image.lightCount == 0 || terrain == nullptr || elevation == nullptr) {
      return false;
    }
    Device* const device = Device::GetInstance();
    if (device == nullptr) {
      return false;
    }
    const auto refuse = [&](const char* const reason) {
      if (!s.loggedRefusal) {
        s.loggedRefusal = true;
        gpg::Warnf("Light field is not available: %s; the light grid is used", reason);
      }
      return false;
    };
    const std::uint32_t features = device->GetGpuDrivenFeatures();
    if ((features & kGpuFeatureVertexPulling) == 0U || (features & kGpuFeatureCompute) == 0U) {
      return refuse("this render API has no GPU programs with compute");
    }
    if (!EnsureResources(s, *device)) {
      return false;
    }

    float minXZ[2];
    float maxXZ[2];
    if (!ViewGround(camera, terrain, elevation, minXZ, maxXZ)) {
      return false;
    }
    // The cell size from a ladder (the field does not shimmer when the zoom changes a little); the origin snaps to whole thread groups.
    const float margin = 24.0f;
    const float extent = std::max(maxXZ[0] - minXZ[0], maxXZ[1] - minXZ[1]) + 2.0f * margin;
    float cellSize = kCellLadder[sizeof(kCellLadder) / sizeof(kCellLadder[0]) - 1];
    for (const float candidate : kCellLadder) {
      if (static_cast<float>(kCells - 2U * kTile) * candidate >= extent) {
        cellSize = candidate;
        break;
      }
    }
    const float tileSpan = static_cast<float>(kTile) * cellSize;
    const float centreX = 0.5f * (minXZ[0] + maxXZ[0]);
    const float centreZ = 0.5f * (minXZ[1] + maxXZ[1]);
    const float originX = std::floor((centreX - 0.5f * static_cast<float>(kCells) * cellSize) / tileSpan) * tileSpan;
    const float originZ = std::floor((centreZ - 0.5f * static_cast<float>(kCells) * cellSize) / tileSpan) * tileSpan;

    // Terrain heights of the window, on a coarse lattice (the lights sit above a smooth surface; the shader interpolates).
    const float heightStep = static_cast<float>(kCells) * cellSize / static_cast<float>(kHeightSamples - 1U);
    if (originX != s.heightOriginX || originZ != s.heightOriginZ || cellSize != s.heightCell || ++s.heightAge > 180U) {
      std::vector<float> heights(static_cast<std::size_t>(kHeightSamples) * kHeightSamples);
      for (std::uint32_t z = 0; z < kHeightSamples; ++z) {
        for (std::uint32_t x = 0; x < kHeightSamples; ++x) {
          heights[static_cast<std::size_t>(z) * kHeightSamples + x] =
            elevation(terrain, originX + static_cast<float>(x) * heightStep, originZ + static_cast<float>(z) * heightStep);
        }
      }
      device->UpdateGpuBuffer(*s.heightBuffer, 0U, kHeightSamples * kHeightSamples, heights.data());
      s.heightOriginX = originX;
      s.heightOriginZ = originZ;
      s.heightCell = cellSize;
      s.heightAge = 0U;
    }

    const std::uint32_t lightCount = std::min<std::uint32_t>(image.lightCount, static_cast<std::uint32_t>(kMaxGridLights));
    device->UpdateGpuBuffer(*s.lightBuffer, 0U, lightCount * static_cast<std::uint32_t>(kTexelsPerLight), image.data.data());

    FieldConstants constants{};
    constants.origin[0] = originX;
    constants.origin[1] = originZ;
    constants.origin[2] = cellSize;
    constants.origin[3] = static_cast<float>(lightCount);
    constants.dims[0] = static_cast<float>(kCells);
    constants.dims[1] = static_cast<float>(kLayers);
    constants.dims[2] = static_cast<float>(kHeightSamples);
    constants.dims[3] = heightStep;
    std::memcpy(constants.layerH, kLayerHeights, sizeof(constants.layerH));

    OutputContext saved;
    device->GetContext(&saved);
    D3DVIEWPORT9 savedViewport{};
    device->GetViewport(&savedViewport);
    {
      const RenderProfileScope profile("LightField");
      GpuDispatch dispatch;
      dispatch.program = s.compute.get();
      dispatch.bindings.constants = &constants;
      dispatch.bindings.constantBytes = sizeof(constants);
      dispatch.bindings.readBuffers[0] = s.lightBuffer.get();
      dispatch.bindings.readBuffers[1] = s.heightBuffer.get();
      dispatch.bindings.writeBuffers[0] = s.fieldBuffer.get();
      dispatch.groupsX = kCells / kTile;
      dispatch.groupsY = kCells / kTile;
      dispatch.groupsZ = kLayers;
      device->DispatchGpuProgram(dispatch);

      const boost::shared_ptr<DepthStencilTarget> noDepth;
      const OutputContext target(s.frame.atlas, noDepth);
      device->ClearTarget(&target);
      D3DVIEWPORT9 viewport{};
      viewport.Width = kCells * kLayers;
      viewport.Height = kCells * 2U;
      viewport.MaxZ = 1.0f;
      device->SetViewport(&viewport);
      GpuDrawCall call;
      call.program = s.copy.get();
      call.bindings.constants = &constants;
      call.bindings.constantBytes = sizeof(constants);
      call.bindings.readBuffers[0] = s.fieldBuffer.get();
      call.vertexCountPerInstance = 3U;
      call.instanceCount = 1U;
      device->DrawGpuProgram(call);
    }
    device->ClearTarget(&saved);
    device->SetViewport(&savedViewport);

    FieldFrame& f = s.frame;
    f.params[0] = originX;
    f.params[1] = originZ;
    f.params[2] = 1.0f / cellSize;
    f.params[3] = static_cast<float>(kCells);
    f.params2[0] = 1.0f;
    f.params2[1] = static_cast<float>(kLayers);
    f.params2[2] = 1.0f / static_cast<float>(kCells * kLayers);
    f.params2[3] = 1.0f / static_cast<float>(kCells * 2U);
    std::memcpy(f.layerHeights, kLayerHeights, sizeof(f.layerHeights));
    f.valid = true;
    ++s.stats.frames;
    s.stats.cellSize = cellSize;
    s.stats.cells = kCells;
    s.stats.lights = lightCount;
    return true;
  }

  const FieldFrame& CurrentField()
  {
    return GetState().frame;
  }

  void ReleaseField()
  {
    State& s = GetState();
    s = State();
  }

  const FieldStats& GetFieldStats()
  {
    return GetState().stats;
  }
} // namespace moho::lights
