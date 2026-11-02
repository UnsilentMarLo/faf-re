#include "moho/terrain/water/WaterSimulation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
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
#include "gpg/core/containers/FastVector.h"
#include "moho/render/d3d/CD3DDevice.h"
#include "moho/render/d3d/ShaderVar.h"
#include "moho/vision/VisionDB.h"
#include "moho/sim/CArmyImpl.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/STIMap.h"

namespace moho::water
{
  int ren_WaterSim = 1;
  static int ren_WaterSimStrength = 100;
  static int ren_WaterGrid = 1;
  static int ren_WaterDisplace = 100;
  static int ren_WaterSimTest = 0;
  static int ren_WaterSimLog = 0;
  static int ren_WaterSwell = 100;
  static int ren_WaterSolid = 25;
  static int ren_WaterSimWake = 200;
  static int ren_WaterSimImpact = 100;
  static int ren_WaterRec = 0;
  static int ren_WaterRecEvery = 6;

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
  static TConVar<int> gTConVar_ren_WaterSimLog(
    "ren_WaterSimLog", "Diagnostic: every N rendered frames the highest and lowest wave height of the simulation is read back and logged (stalls the GPU); 0 (the default) off.",
    &ren_WaterSimLog
  );
  static TConVar<int> gTConVar_ren_WaterSimWake(
    "ren_WaterSimWake", "How strongly ships push the water, in percent of the base wake (default 200).", &ren_WaterSimWake
  );
  static TConVar<int> gTConVar_ren_WaterRec(
    "ren_WaterRec",
    "Recorder: write the next N captures of the wave simulation (height, foam, land, hulls, sources around the action) to the folder of FAF_WATERREC_DIR "
    "(default logs/waterrec); tools/waterrec.py reads them. 0 (the default) off.",
    &ren_WaterRec
  );
  static TConVar<int> gTConVar_ren_WaterRecEvery(
    "ren_WaterRecEvery", "Recorder: one capture every N rendered frames (default 6).", &ren_WaterRecEvery
  );
  static TConVar<int> gTConVar_ren_WaterSimImpact(
    "ren_WaterSimImpact", "How strongly impacts and explosions push the water, in percent of the tuned splash (default 100).", &ren_WaterSimImpact
  );
  static TConVar<int> gTConVar_ren_WaterSwell(
    "ren_WaterSwell", "The wind sea: height of the constant swell that lifts the water and breaks on the beaches, in percent (default 100, 0 off).", &ren_WaterSwell
  );
  static TConVar<int> gTConVar_ren_WaterSolid(
    "ren_WaterSolid", "How much of the colour of the water covers the sea bed, at least, in percent: more is a more solid sea, less see-through (default 25).", &ren_WaterSolid
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
    constexpr float kCell = 0.5f;                 // world units per cell
    constexpr std::uint32_t kTile = 8;
    constexpr std::uint32_t kMaxSources = 4096;
    constexpr float kLandMargin = 0.1f;           // ground within this of the water level counts as land
    constexpr float kStepSeconds = 1.0f / 90.0f;
    constexpr int kMaxSteps = 6;
    constexpr std::uint32_t kFormat = 9U;         // RGBA16F render target (DxgiFormats.cpp)

    struct Source
    {
      float x, z, radius, strength; // the push (see SimAddSource)
      float foam, a, b, c;          // foam thrown per frame, unused
    };

    struct HullRecord
    {
      float x, z, hx, hz, speed, halfLength, halfBeam, radius;
    };

    struct SimConstants
    {
      float a[4]; // origin x, z (cells, a multiple of 8), previous origin x, z
      float b[4]; // cell size, cells, source count, step seconds
      float c[4]; // wave speed term, damping per step, centre x, z (world)
      float d[4]; // half extent (world), unused
      float e[4]; // map height samples x, z, water level in raw height units, 1 when the land mask is on
      float f[4]; // time (s), swell scale, foam kept per step, shore surf
    };

    constexpr char kSource[] = R"HLSL(
cbuffer SimConstants : register(b0)
{
    float4 sA;
    float4 sB;
    float4 sC;
    float4 sD;
    float4 sE;
    float4 sF;   // time (s), swell scale, foam kept per step, shore surf
};

StructuredBuffer<float4> Cur : register(t0);     // height, last height, foam, 1 on land
StructuredBuffer<float4> Src : register(t1);     // two float4 per source: x, z, radius, push; foam, 0, 0, 0
StructuredBuffer<uint> Heights : register(t2);   // the raw uint16 heights of the map, two to a word (the land mask)
RWStructuredBuffer<float4> Nxt : register(u0);

groupshared uint sList[256];
groupshared uint sCount;

static const float4 kSwellWave[8] = { float4(0.955, 0.296, 1.047, 0.0477), float4(0.814, 0.581, 1.396, 0.0358), float4(0.989, -0.149, 1.745, 0.0286), float4(0.582, 0.813, 2.094, 0.0239), float4(0.999, 0.050, 2.618, 0.0191), float4(0.697, 0.717, 3.142, 0.0159), float4(0.900, -0.435, 3.927, 0.0127), float4(0.362, 0.932, 4.833, 0.0103) };
static const float kSwellSpeed[8] = { 1.36, 1.68, 1.92, 2.09, 2.49, 2.83, 3.34, 3.87 };   // rad / s: phase speeds of about one unit a second

// the wind sea: eight short travelling waves (direction x, z, wave number, height) with sharp crests (1.273 - 2 |sin(theta / 2)| has a cusp at
// the crest, flat troughs and no mean). It bends the normals and drives the surf; it does not lift the mesh (a coarse grid shows it as facets).
float FafSwellHeight(float2 p, float t)
{
    float h = 0.0;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float4 w = kSwellWave[i];
        h += w.w * (1.273 - 2.0 * abs(sin(0.5 * (dot(w.xy, p) * w.z - kSwellSpeed[i] * t + (float)i * 1.7))));
    }
    return h;
}

float2 FafSwellSlope(float2 p, float t)
{
    float2 s = float2(0.0, 0.0);
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float4 w = kSwellWave[i];
        float th = 0.5 * (dot(w.xy, p) * w.z - kSwellSpeed[i] * t + (float)i * 1.7);
        s += w.xy * (-w.w * w.z * cos(th) * (sin(th) >= 0.0 ? 1.0 : -1.0));
    }
    return s;
}

uint Hash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

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
    float foamAdd = 0.0;
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
            float4 s = Src[i * 2];
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
            float4 s = Src[sList[k] * 2];
            float4 s1 = Src[sList[k] * 2 + 1];
            float2 d = wpos - s.xy;
            float f = 1.0 - dot(d, d) / (s.z * s.z);
            if (f > 0.0)
            {
                add += s.w * sB.w * f * f;
                foamAdd += s1.x * f;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    uint idx = (uint)(cell.y * n + cell.x);
    float groundDepth = 1000.0;
    if (sE.w > 0.5)
    {
        // land is a wall: a cell whose ground is above the water holds no wave, so nothing that happens ashore moves the sea and a wave dies at the beach
        int2 sc = clamp((int2)floor(wpos), int2(0, 0), int2(sE.xy) - 1);
        uint hi = (uint)sc.y * (uint)sE.x + (uint)sc.x;
        uint raw = (Heights[hi >> 1] >> ((hi & 1) * 16)) & 0xFFFF;
        if ((float)raw > sE.z)
        {
            Nxt[idx] = float4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        groundDepth = ((float)sE.z - (float)raw) / 128.0;   // how deep the water is here (to the margin above the beach)
    }
    float4 cur = Cur[idx];
    if (w.x != wp.x || w.y != wp.y)
    {
        cur = float4(0.0, 0.0, 0.0, 0.0);   // a cell the window has just moved onto holds what the other side of the window left: start it calm
    }
    float4 L = Cur[cell.y * n + (cell.x + n - 1) % n];
    float4 R = Cur[cell.y * n + (cell.x + 1) % n];
    float4 U = Cur[((cell.y + n - 1) % n) * n + cell.x];
    float4 D = Cur[((cell.y + 1) % n) * n + cell.x];
    // land reflects: a neighbour on land counts as the height of this cell
    float hl = L.w > 0.5 ? cur.x : L.x;
    float hr = R.w > 0.5 ? cur.x : R.x;
    float hu = U.w > 0.5 ? cur.x : U.x;
    float hd = D.w > 0.5 ? cur.x : D.x;
    float lap = hl + hr + hu + hd - 4.0 * cur.x;
    // viscosity: the laplacian of the velocity (height minus last height) damps the short waves, the ringing of a splash, and leaves the long ones
    float vel = cur.x - cur.y;
    float vl = L.w > 0.5 ? vel : L.x - L.y;
    float vr = R.w > 0.5 ? vel : R.x - R.y;
    float vu = U.w > 0.5 ? vel : U.x - U.y;
    float vd = D.w > 0.5 ? vel : D.x - D.y;
    float lapv = vl + vr + vu + vd - 4.0 * vel;
    float dmax = max(abs(wpos.x - sC.z), abs(wpos.y - sC.w)) / sD.x;
    float edge = smoothstep(0.8, 1.0, dmax);   // the waves are absorbed at the border of the window
    float hn = (2.0 * cur.x - cur.y + sC.x * lap + 0.10 * lapv) * sC.y * (1.0 - 0.3 * edge) + add;
    // a trough never goes deeper than half of the water: in the shallows the lifted grid would dip below the sea bed and show the sand in streaks
    hn = clamp(hn, -min(1.5, 0.5 * groundDepth), 1.5);

    // foam: spreads a little, fades, is thrown by the sources (noisy: a propeller wash is not a smooth band) and by the surf at the beach
    float fl = L.w > 0.5 ? cur.z : L.z;
    float fr = R.w > 0.5 ? cur.z : R.z;
    float fu = U.w > 0.5 ? cur.z : U.z;
    float fd = D.w > 0.5 ? cur.z : D.z;
    float foam = lerp(cur.z, 0.25 * (fl + fr + fu + fd), 0.12) * sF.z;
    uint seed = Hash((uint)cell.y * 2654435761u + (uint)cell.x * 40503u + (uint)(sF.x * 90.0) * 9973u);
    float noise = 0.3 + 1.4 * (float)(seed & 0xFFFFu) * (1.0 / 65536.0);
    foam += foamAdd * noise;
    float ring2 = max(max(Cur[cell.y * n + (cell.x + n - 2) % n].w, Cur[cell.y * n + (cell.x + 2) % n].w),
                      max(Cur[((cell.y + n - 2) % n) * n + cell.x].w, Cur[((cell.y + 2) % n) * n + cell.x].w));
    float shore = max(max(max(L.w, R.w), max(U.w, D.w)), 0.6 * ring2);
    float crest = saturate(FafSwellHeight(wpos, sF.x) * sF.y * 6.0 + 0.1);
    foam += shore * (saturate(abs(hn) * 5.0 - 0.1) * 0.10 + crest * crest * 0.035 * sF.w);
    foam += saturate(abs(hn) * 6.0 - 0.9) * 0.004;
    foam = clamp(foam, 0.0, 1.5);
    Nxt[idx] = float4(hn, cur.x, foam, 0.0);
}

float4 SimVS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

// slope (x, z), height and foam of the surface, for the water pixel shader
float4 SimCopyPS(float4 position : SV_Position) : SV_Target
{
    int n = (int)sB.y;
    int2 c = int2(position.xy);
    float4 here = Cur[c.y * n + c.x];
    float hl = Cur[c.y * n + (c.x + n - 1) % n].x;
    float hr = Cur[c.y * n + (c.x + 1) % n].x;
    float hu = Cur[((c.y + n - 1) % n) * n + c.x].x;
    float hd = Cur[((c.y + 1) % n) * n + c.x].x;
    return float4((hr - hl) * 0.5 / sB.x, (hd - hu) * 0.5 / sB.x, here.x, here.z);
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
      float time = 0.0f;
      int lastTick = -1;                                   // the game tick of the last frame that saw one come
      std::chrono::steady_clock::time_point lastTickWall{}; // when
      bool haveTick = false;
      float gameSpeed = 1.0f;                              // game ticks over real time, in units of ten a second (smoothed)
      float sourceWeight = 1.0f;                           // game time of the last frame over one step of the simulation
      std::vector<HullRecord> recHulls;                    // the hulls the feeds saw this frame (recorder)
      std::vector<Source> recSources;                      // the sources of this frame (recorder)
      int recFrame = 0;
      int recIndex = 0;
      int lastSteps = 0;
      float lastDt = 0.0f;
    };

    State& GetState()
    {
      static State* const state = new State(); // the effect unlinks its shader vars on its own schedule: never destroyed
      return *state;
    }

    // Whether the local player sees this place now: a circle of the vision of the session's focus army (allies included) holds it. An observer
    // (no focus army) sees all. What happens in the fog must not move the sea the player looks at: the waves would tell.
    bool PlayerSees(const float x, const float z)
    {
      CWldSession* const session = WLD_GetActiveSession();
      if (session == nullptr || session->FocusArmy < 0) {
        return true;
      }
      static const Wm3::Vector2f kAxes[2] = {Wm3::Vector2f{1.0f, 0.0f}, Wm3::Vector2f{0.0f, 1.0f}};
      Wm3::Box2f box;
      box.Center.x = x;
      box.Center.y = z;
      box.Axis[0] = kAxes[0];
      box.Axis[1] = kAxes[1];
      box.Extent[0] = 0.5f;
      box.Extent[1] = 0.5f;
      gpg::fastvector_n<Wm3::Circle2f, VisionDB::kVisibleCircleInlineCapacity> circles;
      session->mVisionDb.CollectVisibleCircles(circles, box, std::min(std::max(REN_GetSimDeltaSeconds(), 0.0f), 1.0f));
      for (std::size_t i = 0; i < circles.size(); ++i) {
        const float dx = x - circles[i].Center.x;
        const float dz = z - circles[i].Center.y;
        if (dx * dx + dz * dz <= circles[i].Radius * circles[i].Radius) {
          return true;
        }
      }
      return false;
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
        buffer.stride = 16U;
        buffer.capacity = kN * kN;
        buffer.shaderWrite = true;
        s.state[0] = device.CreateGpuBuffer(buffer);
        s.state[1] = device.CreateGpuBuffer(buffer);
        GpuBufferDesc sources;
        sources.name = "WaterSimSources";
        sources.stride = 16U;
        sources.capacity = kMaxSources * 2U; // two float4 a source
        s.sources = device.CreateGpuBuffer(sources);
        if (s.state[0] == nullptr || s.state[1] == nullptr || s.sources == nullptr) {
          gpg::Warnf("Water simulation disabled: its GPU buffers could not be created");
          s.failed = true;
          return false;
        }
        const std::vector<float> zeros(static_cast<std::size_t>(kN) * kN * 4U, 0.0f);
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

  void SimAddSource(const float x, const float z, const float radius, const float strength, const float foam, const bool shown)
  {
    State& s = GetState();
    if (ren_WaterSim == 0 || !(radius > 0.05f) || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(strength) || s.pending.size() >= kMaxSources) {
      return;
    }
    if (!shown && !PlayerSees(x, z)) {
      if (ren_WaterSimLog > 0) {
        gpg::Logf("WaterSim source in the fog at (%.1f %.1f): dropped", x, z);
      }
      return; // fog: no waves for what the player cannot see
    }
    // only over water: a flash, a glow or a unit ashore (at the height of the sea on a low beach) must not move the sea
    if (s.field != nullptr && s.waterEnabled) {
      const float cx = std::min(std::max(x, 0.0f), static_cast<float>(s.field->Width() - 1));
      const float cz = std::min(std::max(z, 0.0f), static_cast<float>(s.field->Height() - 1));
      if (s.field->GetElevation(cx, cz) > s.waterElevation - kLandMargin) {
        return;
      }
    }
    s.pending.push_back(Source{x, z, std::min(radius, 40.0f), strength, foam, 0.0f, 0.0f, 0.0f});
  }

  bool SimIsOwnSide(const CArmyImpl* const army)
  {
    CWldSession* const session = WLD_GetActiveSession();
    if (army == nullptr || session == nullptr || session->FocusArmy < 0) {
      return false;
    }
    return !const_cast<CArmyImpl*>(army)->IsEnemy(static_cast<std::uint32_t>(session->FocusArmy));
  }

  void SimImpact(const float x, const float z, const float size, const float scale, const bool shown)
  {
    const float s = std::max(size, 0.5f);
    if (ren_WaterSimLog > 0) {
      gpg::Logf("WaterSim impact: at (%.1f %.1f), size %.2f, scale %.2f", x, z, s, scale);
    }
    const float radius = std::min(std::max(0.7f * s, 0.8f), 18.0f);
    const float hump = std::min(std::max(0.03f * s, 0.03f), 0.5f) * scale * SimImpactScale();
    const float foam = std::min(0.03f + 0.01f * s, 0.15f);
    // The push of one frame is added to the height of the step, which leaves the water with a velocity that the (slow) waves hardly stop: a
    // twentieth of the wanted height is pushed (measured; docs/LIGHTING_V2.md). A splash is a cluster of lumps of different size, place and
    // strength, some pulling down: a ring from one perfect disc looks like a drop in a bath.
    static std::uint32_t sSeed = 2463534242u;
    const auto random = [&]() {
      sSeed = sSeed * 1664525u + 1013904223u;
      return static_cast<float>(sSeed >> 8) * (1.0f / 16777216.0f);
    };
    const int parts = 4 + std::min(static_cast<int>(s / 4.0f), 4);
    for (int part = 0; part < parts; ++part) {
      const float angle = random() * 6.2831853f;
      const float distance = radius * (0.1f + 0.7f * random());
      const float lump = radius * (0.35f + 0.25f * random());
      const float push = hump * 90.0f * 0.05f * (0.5f + 0.7f * random()) * (part % 4 == 3 ? -0.6f : 1.0f);
      SimAddSource(x + std::cos(angle) * distance, z + std::sin(angle) * distance, lump, push, foam * (0.4f + random()) / static_cast<float>(parts) * 2.0f, shown);
    }
  }

  void SimUnitDeath(const float x, const float y, const float z, const float mass, const bool shown)
  {
    float elevation = 0.0f;
    if (ren_WaterSimLog > 0) {
      gpg::Logf("WaterSim death: at (%.1f %.1f %.1f), cost %.0f", x, y, z, mass);
    }
    if (!SimWaterLevel(elevation) || y - elevation > 3.0f || !(mass > 0.0f)) {
      return;
    }
    // a hull below the surface (a submarine) is felt less
    // The size follows the square root of the cost, on the scale of a shell's (the cube root of its damage): a hull of cost 300 sinks like a shell of
    // 15 damage, 2,250 like one of 300, a battleship of 10,000 like one of 2,700.
    const float size = 0.14f * std::sqrt(std::max(mass, 20.0f));
    SimImpact(x, z, size, y < elevation - 3.0f ? 0.5f : 1.0f, shown);
  }

  bool SimDiagnostics()
  {
    return ren_WaterSimLog > 0;
  }

  float SimSourceWeight()
  {
    return GetState().sourceWeight;
  }

  bool SimRecording()
  {
    return ren_WaterRec > 0;
  }

  void SimRecordHull(const float x, const float z, const float hx, const float hz, const float speed, const float halfLength, const float halfBeam, const float radius)
  {
    if (ren_WaterRec > 0) {
      GetState().recHulls.push_back(HullRecord{x, z, hx, hz, speed, halfLength, halfBeam, radius});
    }
  }

  float SimWakeScale()
  {
    return 0.01f * static_cast<float>(std::max(ren_WaterSimWake, 0));
  }

  float SimImpactScale()
  {
    return 0.01f * static_cast<float>(std::max(ren_WaterSimImpact, 0));
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
    // The water keeps the time of the game, not the wall: it stops when the game is paused (no tick for a third of a second) and runs as fast as
    // the game does (the speed from the ticks that came over the real time since the last).
    {
      const int tick = REN_GetGameTick();
      if (!s.haveTick || tick != s.lastTick) {
        if (s.haveTick && tick > s.lastTick) {
          const float wall = std::chrono::duration<float>(now - s.lastTickWall).count();
          if (wall > 0.0005f) {
            const float rate = std::min(static_cast<float>(tick - s.lastTick) / wall * 0.1f, 8.0f);
            s.gameSpeed += (rate - s.gameSpeed) * 0.3f;
          }
        }
        s.lastTick = tick;
        s.lastTickWall = now;
        s.haveTick = true;
      }
      const bool paused = std::chrono::duration<float>(now - s.lastTickWall).count() > 0.35f;
      dt = paused ? 0.0f : dt * std::min(std::max(s.gameSpeed, 0.1f), 6.0f);
      dt = std::min(dt, 0.1f);
    }
    s.time += dt;
    // What a moving hull pushes in is meant per step of the simulation (90 a second), but the feeds run once per frame: at 200 frames a second the
    // sources of two frames land in one step and a wake grows to a slab of foam and thick ridges, at 30 it is thin, and while the game is paused the
    // pending list filled up and burst on the next frame. The feeds weigh their continuous sources with the game time of the last frame.
    s.sourceWeight = std::min(dt / kStepSeconds, 4.0f);

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

    if (ren_WaterRec > 0) {
      s.recSources = s.pending;
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
      // The waves of a hull travel at about 2.5 units a second (the term is (speed x step / cell)^2); a wake is a few dozen units long and fades in
      // seven seconds.
      constants.c[0] = 0.0031f;
      constants.c[1] = 0.997f;
      constants.f[0] = s.time;
      constants.f[1] = 0.01f * static_cast<float>(ren_WaterSwell);
      constants.f[2] = 0.994f;  // foam kept per step: gone in a couple of seconds
      constants.f[3] = 1.0f;    // surf at the beach
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
          device->UpdateGpuBuffer(*s.sources, 0U, sourceCount * 2U, s.pending.data());
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

      s.lastSteps = steps;
      s.lastDt = dt;
      if (ren_WaterRec > 0 && ++s.recFrame >= std::max(ren_WaterRecEvery, 1)) {
        s.recFrame = 0;
        // a window of 256 x 256 cells (128 units) around the first hull of the frame, else around where the view looks
        constexpr int kCrop = 256;
        float fx = centreX;
        float fz = centreZ;
        if (!s.recHulls.empty()) {
          fx = s.recHulls[0].x;
          fz = s.recHulls[0].z;
        }
        int cx0 = static_cast<int>(std::floor(fx / kCell)) - kCrop / 2;
        int cz0 = static_cast<int>(std::floor(fz / kCell)) - kCrop / 2;
        cx0 = std::min(std::max(cx0, originX), originX + static_cast<int>(kN) - kCrop);
        cz0 = std::min(std::max(cz0, originZ), originZ + static_cast<int>(kN) - kCrop);
        std::vector<float> all(static_cast<std::size_t>(kN) * kN * 4U);
        if (device->ReadGpuBuffer(*s.state[s.current], 0U, kN * kN, all.data())) {
          const char* const dirEnv = std::getenv("FAF_WATERREC_DIR");
          const std::string dir = dirEnv != nullptr ? dirEnv : "C:/ProgramData/FAForever/logs/waterrec";
          std::error_code ec;
          std::filesystem::create_directories(dir, ec);
          char name[512];
          std::snprintf(name, sizeof(name), "%s/rec_%05d.bin", dir.c_str(), s.recIndex++);
          if (FILE* const f = std::fopen(name, "wb")) {
            const std::uint32_t header[4] = {0x31435257u /* WRC1 */, static_cast<std::uint32_t>(kCrop), static_cast<std::uint32_t>(s.recHulls.size()),
                                             static_cast<std::uint32_t>(std::min<std::size_t>(s.recSources.size(), 8192))};
            const float info[12] = {kCell, static_cast<float>(cx0) * kCell, static_cast<float>(cz0) * kCell, s.time, static_cast<float>(REN_GetGameTick()), s.gameSpeed,
                                    s.sourceWeight, static_cast<float>(steps), dt, centreX, centreZ, elevation};
            std::fwrite(header, sizeof(header), 1, f);
            std::fwrite(info, sizeof(info), 1, f);
            for (const HullRecord& h : s.recHulls) {
              std::fwrite(&h, sizeof(h), 1, f);
            }
            for (std::size_t i = 0; i < header[3]; ++i) {
              std::fwrite(&s.recSources[i], sizeof(Source), 1, f);
            }
            std::vector<float> height(static_cast<std::size_t>(kCrop) * kCrop);
            std::vector<float> foam(height.size());
            std::vector<std::uint8_t> land(height.size());
            const int n = static_cast<int>(kN);
            for (int j = 0; j < kCrop; ++j) {
              for (int i = 0; i < kCrop; ++i) {
                const int bx = ((cx0 + i) % n + n) % n;
                const int bz = ((cz0 + j) % n + n) % n;
                const std::size_t src = (static_cast<std::size_t>(bz) * kN + static_cast<std::size_t>(bx)) * 4U;
                const std::size_t dst = static_cast<std::size_t>(j) * kCrop + static_cast<std::size_t>(i);
                height[dst] = all[src];
                foam[dst] = all[src + 2U];
                land[dst] = all[src + 3U] > 0.5f ? 1U : 0U;
              }
            }
            std::fwrite(height.data(), sizeof(float), height.size(), f);
            std::fwrite(foam.data(), sizeof(float), foam.size(), f);
            std::fwrite(land.data(), 1, land.size(), f);
            std::fclose(f);
          }
        }
        --ren_WaterRec;
      }
      s.recHulls.clear();

      static int sSimLogCounter = 0;
      if (ren_WaterSimLog > 0 && (++sSimLogCounter % ren_WaterSimLog) == 0) {
        std::vector<float> state(static_cast<std::size_t>(kN) * kN * 4U);
        if (device->ReadGpuBuffer(*s.state[s.current], 0U, kN * kN, state.data())) {
          float hi = 0.0f;
          float lo = 0.0f;
          float foamMax = 0.0f;
          std::size_t foamy = 0;
          double sum = 0.0;
          std::size_t moving = 0;
          for (std::size_t i = 0; i < static_cast<std::size_t>(kN) * kN; ++i) {
            const float v = state[i * 4U];
            foamMax = std::max(foamMax, state[i * 4U + 2U]);
            foamy += state[i * 4U + 2U] > 0.2f ? 1U : 0U;
            hi = std::max(hi, v);
            lo = std::min(lo, v);
            sum += std::fabs(v);
            moving += std::fabs(v) > 0.02f ? 1U : 0U;
          }
          gpg::Logf("Water simulation: wave height %.3f .. %.3f, mean |h| %.5f, %u cells above 0.02, foam up to %.2f, %u cells above 0.2", lo, hi, sum / (static_cast<double>(kN) * kN),
                    static_cast<unsigned>(moving), foamMax, static_cast<unsigned>(foamy));
        }
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
      const float strength = 0.2f * static_cast<float>(ren_WaterSimStrength); // the slope of a hull wave is small: bend the normal hard
      const float params[4] = {1.0f / (static_cast<float>(kN) * kCell), centreX, centreZ, 0.5f * static_cast<float>(kN) * kCell};
      const float params2[4] = {1.0f, strength, 0.12f, 0.01f * static_cast<float>(ren_WaterSolid)};
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
      const float params2[4] = {0.01f * static_cast<float>(ren_WaterDisplace), 0.01f * static_cast<float>(ren_WaterSwell), 0.0f, 0.0f};
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
float4 waterSimParams2;   // x: 1 = on, y: how much the slope bends the normal, z: how much height makes foam, w: the least colour of the water over the bed

static const float4 kSwellWave[8] = { float4(0.955, 0.296, 1.047, 0.0477), float4(0.814, 0.581, 1.396, 0.0358), float4(0.989, -0.149, 1.745, 0.0286), float4(0.582, 0.813, 2.094, 0.0239), float4(0.999, 0.050, 2.618, 0.0191), float4(0.697, 0.717, 3.142, 0.0159), float4(0.900, -0.435, 3.927, 0.0127), float4(0.362, 0.932, 4.833, 0.0103) };
static const float kSwellSpeed[8] = { 1.36, 1.68, 1.92, 2.09, 2.49, 2.83, 3.34, 3.87 };   // rad / s: phase speeds of about one unit a second

// the wind sea: eight short travelling waves (direction x, z, wave number, height) with sharp crests (1.273 - 2 |sin(theta / 2)| has a cusp at
// the crest, flat troughs and no mean). It bends the normals and drives the surf; it does not lift the mesh (a coarse grid shows it as facets).
float FafSwellHeight(float2 p, float t)
{
    float h = 0.0;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float4 w = kSwellWave[i];
        h += w.w * (1.273 - 2.0 * abs(sin(0.5 * (dot(w.xy, p) * w.z - kSwellSpeed[i] * t + (float)i * 1.7))));
    }
    return h;
}

float2 FafSwellSlope(float2 p, float t)
{
    float2 s = float2(0.0, 0.0);
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float4 w = kSwellWave[i];
        float th = 0.5 * (dot(w.xy, p) * w.z - kSwellSpeed[i] * t + (float)i * 1.7);
        s += w.xy * (-w.w * w.z * cos(th) * (sin(th) >= 0.0 ? 1.0 : -1.0));
    }
    return s;
}

// the foam of the simulation at a world position (0 outside its window)
float FafWaterFoam(float2 xz)
{
    float f = 0.0;
    if (waterSimParams2.x > 0.5)
    {
        float2 d = abs(xz - waterSimParams.yz);
        float weight = saturate((waterSimParams.w - max(d.x, d.y)) / (waterSimParams.w * 0.15));
        f = tex2Dlod(waterSimSampler, float4(xz * waterSimParams.x, 0, 0)).w * weight;
    }
    return f;
}

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
float4 waterGridParams2;   // x: how much of the simulated height lifts the geometry, y: the swell

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
      "\n    // the tilt the waves add to the normal is limited: a steep one (a hull's hump, a splash) swings the reflection and the refraction across the"
      "\n    // screen and paints the hull's colours around the ship in smears"
      "\n    float2 fafBend = float2(-simRipple.x, -simRipple.y) * (simRipple.w * waterSimParams2.y);"
      "\n    fafBend *= min(1.0, 0.3 / max(length(fafBend), 0.0001));"
      "\n    N = normalize(N + float3(fafBend.x, 0.0, fafBend.y));"
      "\n    // sharper highlights, and a slow, large scale undulation from two low frequency reads of the shipped normal maps (noise: no lattice)"
      "\n    N = normalize(float3(N.x * 1.45, N.y, N.z * 1.45));"
      "\n    float2 fafUv = ViewPosition.xz - inV.mViewVec.xz;"
      "\n    float4 fafU0 = tex2D(NormalSampler1, (fafUv + normal2Movement * Time * 0.6 + 17.0) * normalRepeatRate.y * 0.4);"
      "\n    float4 fafU1 = tex2D(NormalSampler2, (fafUv - normal3Movement * Time * 4.0) * normalRepeatRate.z * 0.7);"
      "\n    float3 fafU = (fafU0.xyz + fafU1.xyz) - 1.0;"
      "\n    N = normalize(N + float3(fafU.x, 0.0, fafU.y) * waterGridParams2.y * 0.9);"
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
    // The foam over the colour of the water, and a least colour of the water over the sea bed (a more solid sea). Anchors of the shipped
    // shader; a shader that has them changed is left as it is.
    const std::string crestMix = "water = lerp(water, waveCrestColor, (1 - waterTexture.a) * (1 - waterTexture.b) * waveCrest);";
    if (count(crestMix) == 1) {
      const std::size_t at = text.find(crestMix) + crestMix.size();
      text.insert(
        at,
        "\n    float fafFoam = FafWaterFoam(ViewPosition.xz - inV.mViewVec.xz);"
        "\n    float fafFoamAmount = smoothstep(0.25, 0.6, fafFoam * 0.8 + (saturate(sum.a * 0.5) - 0.5) * 0.9);"
        "\n    float3 fafFoamColor = float3(0.95, 0.98, 1.0) * (0.8 + 0.2 * saturate(dot(N, SunDirection)));"
        "\n    water = lerp(water, fafFoamColor, fafFoamAmount * 0.92);"
      );
    }
    const std::string colourFactor = "float factor = clamp(waterDepth, waterLerp.x, waterLerp.y);";
    if (count(colourFactor) == 1) {
      text.replace(text.find(colourFactor), colourFactor.size(), "float factor = max(clamp(waterDepth, waterLerp.x, waterLerp.y), waterSimParams2.w);");
    }
    text.insert(text.find(vertexShader), kDeclarations);
    source = std::move(text);
    result.patched = true;
    return result;
  }
} // namespace moho::water
