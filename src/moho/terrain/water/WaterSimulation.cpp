#include "moho/terrain/water/WaterSimulation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "boost/shared_ptr.h"
#include "gpg/core/utils/Logging.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/EffectVariable.hpp"
#include "gpg/gal/GpuDriven.h"
#include "gpg/gal/OutputContext.hpp"
#include "gpg/gal/RenderProfiler.h"
#include "gpg/gal/RenderTarget.hpp"
#include "gpg/gal/RenderTargetContext.hpp"
#include "moho/console/CConCommand.h"
#include "moho/render/camera/GeomCamera3.h"
#include "moho/render/d3d/ShaderVar.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"

namespace moho::water
{
  int ren_WaterSim = 1;
  static int ren_WaterSimStrength = 100;
  static int ren_WaterGrid = 1;
  static int ren_WaterDisplace = 100;
  static int ren_WaterSimTest = 0;

  static TConVar<int> gTConVar_ren_WaterSim(
    "ren_WaterSim",
    "Water that moves when units move in it and things hit it (a GPU wave simulation bends the water normals): 1 (the default) on, 0 the shipped water. "
    "Takes effect at the next map load for the shader patch; docs/LIGHTING_V2.md.",
    &ren_WaterSim
  );
  static TConVar<int> gTConVar_ren_WaterSimTest(
    "ren_WaterSimTest", "Test source of the water simulation: every N rendered frames a ring starts at the water the middle of the view looks at; 0 (the default) none.",
    &ren_WaterSimTest
  );
  static TConVar<int> gTConVar_ren_WaterGrid(
    "ren_WaterGrid", "Draw the water as a projected grid whose vertices the wave simulation lifts (needs ren_WaterSim): 1 (the default) on, 0 the flat quad of the shipped game.",
    &ren_WaterGrid
  );
  static TConVar<int> gTConVar_ren_WaterDisplace(
    "ren_WaterDisplace", "How far the wave simulation lifts the water geometry, in percent of the simulated height (default 100).", &ren_WaterDisplace
  );
  static TConVar<int> gTConVar_ren_WaterSimStrength(
    "ren_WaterSimStrength", "How strongly the waves bend the water surface, in percent (default 100).", &ren_WaterSimStrength
  );

  namespace
  {
    using namespace gpg::gal;

    constexpr std::uint32_t kN = 1024;            // cells per side
    constexpr float kCell = 1.0f;                 // world units per cell
    constexpr std::uint32_t kTile = 8;
    constexpr std::uint32_t kMaxSources = 1024;
    constexpr float kLandMargin = 0.1f;           // ground within this of the water level counts as land
    constexpr float kStepSeconds = 1.0f / 90.0f;
    constexpr int kMaxSteps = 6;
    constexpr std::uint32_t kFormat = 9U;         // RGBA16F render target (DxgiFormats.cpp)

    struct Source
    {
      float x, z, radius, strength;
    };

    struct SimConstants
    {
      float a[4]; // origin x, z (cells, a multiple of 8), previous origin x, z
      float b[4]; // cell size, cells, source count, step seconds
      float c[4]; // wave speed term, damping per step, centre x, z (world)
      float d[4]; // half extent (world), unused
      float e[4]; // map height samples x, z, water level in raw height units, 1 when the land mask is on
    };

    constexpr char kSource[] = R"HLSL(
cbuffer SimConstants : register(b0)
{
    float4 sA;
    float4 sB;
    float4 sC;
    float4 sD;
    float4 sE;
};

StructuredBuffer<float2> Cur : register(t0);
StructuredBuffer<float4> Src : register(t1);
StructuredBuffer<uint> Heights : register(t2);   // the map's raw uint16 heights, two to a word (the land mask)
RWStructuredBuffer<float2> Nxt : register(u0);

groupshared uint sList[256];
groupshared uint sCount;

[numthreads(8, 8, 1)]
void SimCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    int n = (int)sB.y;
    int2 cell = int2(gid.xy * 8 + tid.xy);
    int2 o = int2(sA.xy);
    int2 op = int2(sA.zw);
    int2 w = o + ((cell - o) % n + n) % n;       // the world cell this texel holds in the window of this frame
    int2 wp = op + ((cell - op) % n + n) % n;    // ... and in the window of the last
    float2 wpos = ((float2)w + 0.5) * sB.x;

    int2 tc = int2(gid.xy * 8);
    int2 tw = o + ((tc - o) % n + n) % n;
    float2 tileMin = (float2)tw * sB.x;
    float2 tileMax = tileMin + 8.0 * sB.x;

    float add = 0.0;
    uint total = (uint)sB.z;
    for (uint basis = 0; basis < total; basis += 256)
    {
        if (index == 0)
        {
            sCount = 0;
        }
        GroupMemoryBarrierWithGroupSync();
        uint last = min(basis + 256, total);
        for (uint i = basis + index; i < last; i += 64)
        {
            float4 s = Src[i];
            if (s.x + s.z >= tileMin.x && s.x - s.z <= tileMax.x && s.y + s.z >= tileMin.y && s.y - s.z <= tileMax.y)
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
            float4 s = Src[sList[k]];
            float2 d = wpos - s.xy;
            float f = 1.0 - dot(d, d) / (s.z * s.z);
            if (f > 0.0)
            {
                add += s.w * sB.w * f * f;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    uint idx = (uint)(cell.y * n + cell.x);
    if (sE.w > 0.5)
    {
        // land is a wall: a cell whose ground is above the water holds no wave, so nothing that happens ashore moves the sea and a wave dies at the beach
        int2 sc = clamp(w, int2(0, 0), int2(sE.xy) - 1);
        uint hi = (uint)sc.y * (uint)sE.x + (uint)sc.x;
        uint raw = (Heights[hi >> 1] >> ((hi & 1) * 16)) & 0xFFFF;
        if ((float)raw > sE.z)
        {
            Nxt[idx] = float2(0.0, 0.0);
            return;
        }
    }
    float2 cur = Cur[idx];
    if (w.x != wp.x || w.y != wp.y)
    {
        cur = float2(0.0, 0.0);   // a cell the window has just moved onto holds what the other side of the window left: start it calm
    }
    float hl = Cur[cell.y * n + (cell.x + n - 1) % n].x;
    float hr = Cur[cell.y * n + (cell.x + 1) % n].x;
    float hu = Cur[((cell.y + n - 1) % n) * n + cell.x].x;
    float hd = Cur[((cell.y + 1) % n) * n + cell.x].x;
    float lap = hl + hr + hu + hd - 4.0 * cur.x;
    float dmax = max(abs(wpos.x - sC.z), abs(wpos.y - sC.w)) / sD.x;
    float edge = smoothstep(0.8, 1.0, dmax);   // the waves are absorbed at the border of the window
    float hn = (2.0 * cur.x - cur.y + sC.x * lap) * sC.y * (1.0 - 0.3 * edge) + add;
    hn = clamp(hn, -6.0, 6.0);
    Nxt[idx] = float2(hn, cur.x);
}

float4 SimVS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

// slope (x, z) and height of the surface, for the water pixel shader
float4 SimCopyPS(float4 position : SV_Position) : SV_Target
{
    int n = (int)sB.y;
    int2 c = int2(position.xy);
    float h = Cur[c.y * n + c.x].x;
    float hl = Cur[c.y * n + (c.x + n - 1) % n].x;
    float hr = Cur[c.y * n + (c.x + 1) % n].x;
    float hu = Cur[((c.y + n - 1) % n) * n + c.x].x;
    float hd = Cur[((c.y + 1) % n) * n + c.x].x;
    return float4((hr - hl) * 0.5 / sB.x, (hd - hu) * 0.5 / sB.x, h, 0.0);
}
)HLSL";

    // ---- the water effect: variables, bound by name ------------------------------------------------------------------------------
    struct EffectVars
    {
      ShaderVar texture{};
      ShaderVar params{};
      ShaderVar params2{};
      ShaderVar gridInverse{};
      ShaderVar gridParams{};
      ShaderVar gridParams2{};
      EffectVars()
      {
        RegisterShaderVar("waterGridInv", &gridInverse, "water2");
        RegisterShaderVar("waterGridParams", &gridParams, "water2");
        RegisterShaderVar("waterGridParams2", &gridParams2, "water2");
        RegisterShaderVar("waterSim", &texture, "water2");
        RegisterShaderVar("waterSimParams", &params, "water2");
        RegisterShaderVar("waterSimParams2", &params2, "water2");
      }
    };

    struct State
    {
      EffectVars vars;
      Device* device = nullptr;
      boost::shared_ptr<GpuBuffer> state[2];
      boost::shared_ptr<GpuBuffer> sources;
      boost::shared_ptr<GpuBuffer> heights;
      std::uint64_t heightRevision = ~0ull;
      std::uint32_t heightAge = 1000U; // frames since the land mask went up
      std::uint32_t heightWidth = 0U;
      std::uint32_t heightHeight = 0U;
      const CHeightField* field = nullptr;
      boost::shared_ptr<GpuProgram> step;
      boost::shared_ptr<GpuProgram> copy;
      boost::shared_ptr<RenderTarget> texture;
      std::vector<Source> pending;
      bool failed = false;
      bool loggedRefusal = false;
      bool hasHistory = false;
      int originX = 0;
      int originZ = 0;
      float centreX = 0.0f;
      float centreZ = 0.0f;
      std::uint32_t current = 0U;
      float accumulator = 0.0f;
      std::chrono::steady_clock::time_point last{};
      bool haveLast = false;
      float waterElevation = 0.0f;
      bool waterEnabled = false;
    };

    State& GetState()
    {
      static State* const state = new State(); // the effect unlinks its shader vars on its own schedule: never destroyed
      return *state;
    }

    bool ActiveMapWater(float& elevation)
    {
      CWldSession* const session = WLD_GetActiveSession();
      const STIMap* const map = session != nullptr ? session->GetSTIMap() : nullptr;
      if (map == nullptr || !map->IsWaterEnabled()) {
        return false;
      }
      elevation = map->GetWaterElevation();
      return true;
    }

    void BindOff(State& s)
    {
      const float off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      if (s.vars.params2.Exists()) {
        s.vars.params2.mEffectVariable->SetVector(off);
      }
      if (s.vars.gridParams.Exists()) {
        s.vars.gridParams.mEffectVariable->SetVector(off);
      }
    }

    bool EnsureResources(State& s, Device& device)
    {
      if (s.device != &device) {
        s.state[0].reset();
        s.state[1].reset();
        s.sources.reset();
        s.heights.reset();
        s.heightRevision = ~0ull;
        s.step.reset();
        s.copy.reset();
        s.texture.reset();
        s.failed = false;
        s.hasHistory = false;
        s.device = &device;
      }
      if (s.failed) {
        return false;
      }
      if (s.step == nullptr || s.copy == nullptr) {
        GpuProgramDesc desc;
        desc.name = "WaterSimStep";
        desc.source = kSource;
        desc.sourceBytes = sizeof(kSource) - 1U;
        desc.computeEntry = "SimCS";
        s.step = device.CreateGpuProgram(desc);
        GpuProgramDesc draw;
        draw.name = "WaterSimCopy";
        draw.source = kSource;
        draw.sourceBytes = sizeof(kSource) - 1U;
        draw.vertexEntry = "SimVS";
        draw.pixelEntry = "SimCopyPS";
        draw.blend = GpuBlend::Opaque;
        draw.sampler = GpuSampler::PointClamp;
        s.copy = device.CreateGpuProgram(draw);
        if (s.step == nullptr || s.copy == nullptr) {
          gpg::Warnf("Water simulation disabled: its GPU programs did not compile");
          s.failed = true;
          return false;
        }
      }
      if (s.state[0] == nullptr) {
        GpuBufferDesc buffer;
        buffer.name = "WaterSimState";
        buffer.stride = 8U;
        buffer.capacity = kN * kN;
        buffer.shaderWrite = true;
        s.state[0] = device.CreateGpuBuffer(buffer);
        s.state[1] = device.CreateGpuBuffer(buffer);
        GpuBufferDesc sources;
        sources.name = "WaterSimSources";
        sources.stride = 16U;
        sources.capacity = kMaxSources;
        s.sources = device.CreateGpuBuffer(sources);
        if (s.state[0] == nullptr || s.state[1] == nullptr || s.sources == nullptr) {
          gpg::Warnf("Water simulation disabled: its GPU buffers could not be created");
          s.failed = true;
          return false;
        }
        const std::vector<float> zeros(static_cast<std::size_t>(kN) * kN * 2U, 0.0f);
        device.UpdateGpuBuffer(*s.state[0], 0U, kN * kN, zeros.data());
        device.UpdateGpuBuffer(*s.state[1], 0U, kN * kN, zeros.data());
        s.hasHistory = false;
      }
      if (s.texture == nullptr) {
        const RenderTargetContext context(kN, kN, kFormat);
        s.texture = device.CreateRenderTarget(&context);
        if (s.texture == nullptr) {
          gpg::Warnf("Water simulation disabled: no %ux%u RGBA16F render target", kN, kN);
          s.failed = true;
          return false;
        }
        gpg::Logf("Water simulation: %ux%u cells of %.1f units, RGBA16F result", kN, kN, kCell);
      }
      return true;
    }
  } // namespace

  void SimAddSource(const float x, const float z, const float radius, const float strength)
  {
    State& s = GetState();
    if (ren_WaterSim == 0 || !(radius > 0.05f) || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(strength) || s.pending.size() >= kMaxSources) {
      return;
    }
    // only over water: a flash, a glow or a unit ashore (at the height of the sea on a low beach) must not move the sea
    if (s.field != nullptr && s.waterEnabled) {
      const float cx = std::min(std::max(x, 0.0f), static_cast<float>(s.field->Width() - 1));
      const float cz = std::min(std::max(z, 0.0f), static_cast<float>(s.field->Height() - 1));
      if (s.field->GetElevation(cx, cz) > s.waterElevation - kLandMargin) {
        return;
      }
    }
    s.pending.push_back(Source{x, z, std::min(radius, 40.0f), strength});
  }

  bool SimWaterLevel(float& elevation)
  {
    State& s = GetState();
    if (ren_WaterSim == 0) {
      return false;
    }
    elevation = s.waterElevation;
    return s.waterEnabled;
  }

  bool SimWanted()
  {
    return ren_WaterSim != 0;
  }

  void StepWaterSimulation(const GeomCamera3& camera)
  {
    State& s = GetState();
    float elevation = 0.0f;
    s.waterEnabled = ren_WaterSim != 0 && ActiveMapWater(elevation);
    s.waterElevation = elevation;
    {
      CWldSession* const session = WLD_GetActiveSession();
      const STIMap* const map = session != nullptr ? session->GetSTIMap() : nullptr;
      s.field = map != nullptr ? map->GetHeightField() : nullptr;
    }
    if (!s.waterEnabled) {
      s.pending.clear();
      BindOff(s);
      return;
    }
    Device* const device = Device::GetInstance();
    if (device == nullptr) {
      return;
    }
    const auto refuse = [&](const char* const reason) {
      if (!s.loggedRefusal) {
        s.loggedRefusal = true;
        gpg::Warnf("Water simulation is not available: %s", reason);
      }
      s.pending.clear();
      BindOff(s);
    };
    const std::uint32_t features = device->GetGpuDrivenFeatures();
    if ((features & kGpuFeatureVertexPulling) == 0U || (features & kGpuFeatureCompute) == 0U) {
      refuse("this render API has no GPU programs with compute");
      return;
    }
    if (!EnsureResources(s, *device)) {
      s.pending.clear();
      BindOff(s);
      return;
    }

    // the world view only: the minimap and the other small views call the render too, and each would pull the window to its own camera
    {
      const float viewWidth = camera.viewport.r[3].z;
      const float viewHeight = camera.viewport.r[3].w;
      if (!(viewWidth * viewHeight >= 200000.0f)) {
        return; // keeps the binding of the last frame
      }
    }

    // the land mask: the map's raw heights go to the GPU once (and again when they change), two samples to a word
    bool landMask = false;
    if (s.field != nullptr && s.field->Width() >= 2 && s.field->Height() >= 2) {
      const std::uint32_t hw = static_cast<std::uint32_t>(s.field->Width());
      const std::uint32_t hh = static_cast<std::uint32_t>(s.field->Height());
      const std::uint64_t revision = HeightFieldRevision();
      // A new revision comes with every building that flattens ground: the land mask is refreshed at most every 90 frames.
      if (s.heightAge < 1000000U) {
        ++s.heightAge;
      }
      if (s.heights == nullptr || (s.heightRevision != revision && s.heightAge >= 90U) || s.heightWidth != hw || s.heightHeight != hh) {
        s.heightAge = 0U;
        const std::size_t samples = static_cast<std::size_t>(hw) * hh;
        const std::uint32_t words = static_cast<std::uint32_t>((samples + 1U) / 2U);
        if (s.heights == nullptr || s.heights->Desc().capacity != words) {
          GpuBufferDesc desc;
          desc.name = "WaterSimHeights";
          desc.stride = 4U;
          desc.capacity = words;
          s.heights = device->CreateGpuBuffer(desc);
        }
        if (s.heights != nullptr) {
          std::vector<std::uint32_t> packed(words, 0U);
          for (std::uint32_t z = 0; z < hh; ++z) {
            for (std::uint32_t x = 0; x < hw; ++x) {
              const std::size_t index = static_cast<std::size_t>(z) * hw + x;
              packed[index >> 1U] |= static_cast<std::uint32_t>(s.field->GetHeightAt(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z))) << ((index & 1U) * 16U);
            }
          }
          device->UpdateGpuBuffer(*s.heights, 0U, words, packed.data());
          s.heightRevision = revision;
          s.heightWidth = hw;
          s.heightHeight = hh;
          gpg::Logf("Water simulation: land mask of %ux%u height samples uploaded", hw, hh);
        }
      }
      landMask = s.heights != nullptr && s.heightWidth == hw && s.heightHeight == hh;
    }

    // real time between frames, for the fixed steps
    const auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;
    if (s.haveLast) {
      dt = std::chrono::duration<float>(now - s.last).count();
    }
    s.last = now;
    s.haveLast = true;
    dt = std::min(std::max(dt, 0.0f), 0.1f);

    // the window follows the point of the water the middle of the view looks at
    const float w = camera.viewport.r[3].z;
    const float h = camera.viewport.r[3].w;
    const GeomLine3 ray = camera.Unproject(Wm3::Vector2f(w * 0.5f, h * 0.5f));
    float centreX = s.centreX;
    float centreZ = s.centreZ;
    if (ray.dir.y < -0.01f) {
      const float t = (elevation - ray.pos.y) / ray.dir.y;
      if (t > 0.0f && t < 6000.0f) {
        centreX = ray.pos.x + ray.dir.x * t;
        centreZ = ray.pos.z + ray.dir.z * t;
      }
    }
    const float tileWorld = static_cast<float>(kTile) * kCell;
    const int originX = static_cast<int>(std::floor(centreX / tileWorld)) * static_cast<int>(kTile) - static_cast<int>(kN / 2U);
    const int originZ = static_cast<int>(std::floor(centreZ / tileWorld)) * static_cast<int>(kTile) - static_cast<int>(kN / 2U);
    if (!s.hasHistory) {
      s.originX = originX;
      s.originZ = originZ;
    }

    static int sTestCounter = 0;
    if (ren_WaterSimTest > 0 && (++sTestCounter % ren_WaterSimTest) == 0) {
      SimAddSource(centreX, centreZ, 5.0f, 40.0f);
    }

    s.accumulator += dt;
    int steps = std::min(kMaxSteps, static_cast<int>(s.accumulator / kStepSeconds));
    s.accumulator -= static_cast<float>(steps) * kStepSeconds;
    if (s.accumulator > kStepSeconds * 2.0f) {
      s.accumulator = 0.0f; // never run behind
    }

    static int sLogCounter = 0;
    static std::size_t sPeakSources = 0;
    sPeakSources = std::max(sPeakSources, s.pending.size());
    if ((++sLogCounter % 240) == 1) {
      gpg::Logf("Water simulation: %d steps, up to %u sources a frame, window centre (%.0f, %.0f), water %.1f", steps, static_cast<unsigned>(sPeakSources), centreX,
                centreZ, elevation);
      sPeakSources = 0;
    }
    OutputContext saved;
    device->GetContext(&saved);
    D3DVIEWPORT9 savedViewport{};
    device->GetViewport(&savedViewport);
    {
      const RenderProfileScope profile("WaterSim");
      SimConstants constants{};
      constants.b[0] = kCell;
      constants.b[1] = static_cast<float>(kN);
      constants.b[3] = kStepSeconds;
      constants.c[0] = 0.22f; // wave speed term: sqrt(0.22) cells a step, about 40 units a second
      constants.c[1] = 0.99f; // a ring is gone within a few dozen units, not across the map
      constants.e[0] = static_cast<float>(s.heightWidth);
      constants.e[1] = static_cast<float>(s.heightHeight);
      constants.e[2] = (elevation - kLandMargin) * 128.0f; // raw height units (1 / 128 world unit)
      constants.e[3] = landMask ? 1.0f : 0.0f;
      constants.c[2] = centreX;
      constants.c[3] = centreZ;
      constants.d[0] = 0.5f * static_cast<float>(kN) * kCell;
      for (int step = 0; step < steps; ++step) {
        const std::uint32_t sourceCount = step == 0 ? static_cast<std::uint32_t>(s.pending.size()) : 0U;
        if (sourceCount > 0U) {
          device->UpdateGpuBuffer(*s.sources, 0U, sourceCount, s.pending.data());
        }
        constants.a[0] = static_cast<float>(originX);
        constants.a[1] = static_cast<float>(originZ);
        constants.a[2] = static_cast<float>(step == 0 ? s.originX : originX);
        constants.a[3] = static_cast<float>(step == 0 ? s.originZ : originZ);
        constants.b[2] = static_cast<float>(sourceCount);
        GpuDispatch dispatch;
        dispatch.program = s.step.get();
        dispatch.bindings.constants = &constants;
        dispatch.bindings.constantBytes = sizeof(constants);
        dispatch.bindings.readBuffers[0] = s.state[s.current].get();
        dispatch.bindings.readBuffers[1] = s.sources.get();
        dispatch.bindings.readBuffers[2] = landMask ? s.heights.get() : s.sources.get();
        dispatch.bindings.writeBuffers[0] = s.state[s.current ^ 1U].get();
        dispatch.groupsX = kN / kTile;
        dispatch.groupsY = kN / kTile;
        dispatch.groupsZ = 1U;
        device->DispatchGpuProgram(dispatch);
        s.current ^= 1U;
        if (step == 0) {
          s.pending.clear();
        }
      }
      if (steps > 0) {
        s.originX = originX;
        s.originZ = originZ;
        s.hasHistory = true;
      }

      // the result: slope and height of the current state
      const boost::shared_ptr<DepthStencilTarget> noDepth;
      const OutputContext target(s.texture, noDepth);
      device->ClearTarget(&target);
      D3DVIEWPORT9 viewport{};
      viewport.Width = kN;
      viewport.Height = kN;
      viewport.MaxZ = 1.0f;
      device->SetViewport(&viewport);
      constants.b[2] = 0.0f;
      GpuDrawCall call;
      call.program = s.copy.get();
      call.bindings.constants = &constants;
      call.bindings.constantBytes = sizeof(constants);
      call.bindings.readBuffers[0] = s.state[s.current].get();
      call.vertexCountPerInstance = 3U;
      call.instanceCount = 1U;
      device->DrawGpuProgram(call);
    }
    device->ClearTarget(&saved);
    device->SetViewport(&savedViewport);
    s.centreX = centreX;
    s.centreZ = centreZ;
    s.pending.clear();

    if (s.vars.params2.Exists()) {
      const float strength = 0.01f * static_cast<float>(ren_WaterSimStrength);
      const float params[4] = {1.0f / (static_cast<float>(kN) * kCell), centreX, centreZ, 0.5f * static_cast<float>(kN) * kCell};
      const float params2[4] = {1.0f, strength, 0.12f, 0.0f};
      if (s.vars.texture.Exists()) {
        s.vars.texture.mEffectVariable->SetRenderTarget(s.texture);
      }
      if (s.vars.params.Exists()) {
        s.vars.params.mEffectVariable->SetVector(params);
      }
      s.vars.params2.mEffectVariable->SetVector(params2);
    }
  }

  bool BindWaterGrid(const GeomCamera3& camera, const float mapExtentX, const float mapExtentZ)
  {
    State& s = GetState();
    const bool on = ren_WaterSim != 0 && ren_WaterGrid != 0 && s.waterEnabled && s.texture != nullptr && s.vars.gridParams.Exists() && s.vars.gridInverse.Exists() &&
                    mapExtentX > 1.0f && mapExtentZ > 1.0f;
    const float off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!on) {
      if (s.vars.gridParams.Exists()) {
        s.vars.gridParams.mEffectVariable->SetVector(off);
      }
      return false;
    }
    s.vars.gridInverse.SetMatrix4x4(&camera.inverseViewProjection);
    const float params[4] = {1.0f, 12000.0f, mapExtentX, mapExtentZ};
    s.vars.gridParams.mEffectVariable->SetVector(params);
    if (s.vars.gridParams2.Exists()) {
      const float params2[4] = {0.01f * static_cast<float>(ren_WaterDisplace), 0.0f, 0.0f, 0.0f};
      s.vars.gridParams2.mEffectVariable->SetVector(params2);
    }
    return true;
  }

  void ReleaseWaterSimulation()
  {
    State& s = GetState();
    if (s.vars.texture.mEffectVariable && s.vars.texture.Exists()) {
      s.vars.texture.mEffectVariable->SetRenderTarget(boost::shared_ptr<RenderTarget>());
    }
    s.state[0].reset();
    s.state[1].reset();
    s.sources.reset();
    s.heights.reset();
    s.heightRevision = ~0ull;
    s.field = nullptr;
    s.step.reset();
    s.copy.reset();
    s.texture.reset();
    s.device = nullptr;
    s.hasHistory = false;
    s.failed = false;
  }

  // ------------------------------------------------------------------------------------------------ the effect patch
  namespace
  {
    constexpr const char* kDeclarations = R"HLSL(
// ---- FAF water simulation (WaterSimulation.cpp, docs/LIGHTING_V2.md): ripples of the GPU wave simulation ----
texture waterSim;
sampler2D waterSimSampler = sampler_state
{
    Texture   = (waterSim);
    MipFilter = NONE;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    AddressU  = WRAP;
    AddressV  = WRAP;
};
float4 waterSimParams;    // 1 / (cell size x cells), centre x, centre z, half extent
float4 waterSimParams2;   // x: 1 = on, y: how much the slope bends the normal, z: how much height makes foam

// slope x, slope z, height, weight (0 outside the window the simulation follows)
float4 FafWaterRipple(float2 xz)
{
    float4 r = float4(0, 0, 0, 0);
    if (waterSimParams2.x > 0.5)
    {
        float2 d = abs(xz - waterSimParams.yz);
        float weight = saturate((waterSimParams.w - max(d.x, d.y)) / (waterSimParams.w * 0.15));
        float4 t = tex2Dlod(waterSimSampler, float4(xz * waterSimParams.x, 0, 0));
        r = float4(t.x, t.y, t.z, weight);
    }
    return r;
}

// The projected grid: the vertex's uv is its place on the screen; its position is where that screen ray meets the water plane (limited to
// the map), lifted by the height of the wave simulation.
float4x4 waterGridInv;     // inverse view projection of the camera
float4 waterGridParams;    // x: 1 = projected grid, y: far distance, z, w: extent of the map in x and z
float4 waterGridParams2;   // x: how much of the simulated height lifts the geometry

float3 FafWaterGridPosition(float2 uv, float elevation)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 pn = mul(float4(ndc, 0.0, 1.0), waterGridInv);
    float4 pf = mul(float4(ndc, 1.0, 1.0), waterGridInv);
    float3 a = pn.xyz / pn.w;
    float3 d = normalize(pf.xyz / pf.w - a);
    float t = d.y < -0.0005 ? (elevation - a.y) / d.y : waterGridParams.y;
    t = clamp(t, 0.0, waterGridParams.y);
    float3 p = a + d * t;
    p.xz = clamp(p.xz, float2(0.0, 0.0), waterGridParams.zw);
    float4 ripple = FafWaterRipple(p.xz);
    p.y = elevation + ripple.z * ripple.w * waterGridParams2.x;
    return p;
}

)HLSL";
  } // namespace

  SimPatchResult PatchWaterSimulation(std::string& source)
  {
    SimPatchResult result;
    const auto count = [&](const std::string& needle) {
      std::size_t n = 0;
      for (std::size_t p = source.find(needle); p != std::string::npos; p = source.find(needle, p + needle.size())) {
        ++n;
      }
      return n;
    };
    const std::string shader = "float4 HighFidelityPS(";
    const std::string vertexShader = "VS_OUTPUT WaterVS(";
    const std::string elevation = "inPos.y = WaterElevation;";
    const std::string crest = "float waveCrest = saturate( sum.a - waveCrestThreshold );";
    const std::string normal = "N = normalize(N.xzy);";
    if (count(shader) != 1 || count(crest) != 1 || count(normal) != 1 || count(vertexShader) != 1 || count(elevation) != 1 ||
        source.find("FafWaterRipple") != std::string::npos || source.find(vertexShader) > source.find(shader)) {
      result.error = "HighFidelityPS, WaterVS, their anchor lines not found exactly once in the expected order";
      return result;
    }
    std::string text = source;
    const std::size_t normalAt = text.find(normal);
    text.insert(
      normalAt + normal.size(),
      "\n    N = normalize(N + float3(-simRipple.x, 0.0, -simRipple.y) * (simRipple.w * waterSimParams2.y));"
    );
    const std::size_t crestAt = text.find(crest);
    if (crestAt > normalAt) {
      result.error = "the wave crest line comes after the normal line";
      return result;
    }
    text.insert(
      crestAt + crest.size(),
      "\n    float4 simRipple = FafWaterRipple(ViewPosition.xz - inV.mViewVec.xz);"
      "\n    waveCrest = saturate(waveCrest + simRipple.w * saturate(abs(simRipple.z) * waterSimParams2.z));"
    );
    // the vertex shader: the projected grid (the input uv is the screen position of the vertex) or the flat quad
    const std::size_t elevationAt = text.find(elevation);
    text.replace(
      elevationAt, elevation.size(),
      "if (waterGridParams.x > 0.5)\n"
      "\t{\n"
      "\t\tinPos.xyz = FafWaterGridPosition(inTexUV, WaterElevation);\n"
      "\t\tinTexUV = inPos.xz / waterGridParams.zw;\n"
      "\t}\n"
      "\telse\n"
      "\t{\n"
      "\t\tinPos.y = WaterElevation;\n"
      "\t}"
    );
    text.insert(text.find(vertexShader), kDeclarations);
    source = std::move(text);
    result.patched = true;
    return result;
  }
} // namespace moho::water
