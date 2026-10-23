// EDITOR GPU compute: console variables and Lua natives (docs/projects/editor/GPU_COMPUTE.md). The backend itself is EdGpu.cpp, the op registry and the self test EdGpuOps.cpp.
//
// Console variables:
//   ed_gpu_compute      1        field ops that have a GPU version (blur, box ...) run on the GPU when a Direct3D 11 compute device is available; 0 = always the CPU
//   ed_gpu_selftest_first 1      the self test runs once before the first real GPU op of a session; a device that fails it is switched off for the session
//   ed_gpu_min_cells    65536    fields smaller than this stay on the CPU (upload and download cost more than the op)
// Lua natives (all run on the UI thread):
//   ed_gpu_info([probe])        -> { compiledIn, tried, available, enabled, active, failed, selfTestPassed, adapter, vramMB, maxMB, featureLevel, compiler, textures, lastError, lastOpError, minCells, ops = { names } }
//                                  probe (default true) tries to create the device when that has not been tried yet (a few hundred ms once); the perf report passes false
//   ed_gpu_enable(on[, reset])  -> enabled     the same switch as ed_gpu_compute; reset = true forgets a failure and lets the next op create a new device
//   ed_gpu_selftest()           -> { ok, texturesOk, ms, error, parts = { { name, ok, detail, ms }, ... } }     scale + offset, group shared memory reduction, blur against the CPU op, texture sampling
//   ed_gpu_stats([reset])       -> { enabled, ops = { name = { cpu, cpuMs, gpu, gpuMs, fallbacks } }, cpu, cpuMs, gpu, gpuMs, fallbacks }     per-op counters since start (or the last reset)
//
// Threads: the natives run on the UI thread. The D3D9 adapter lookup is done only there. Log lines of the backend can come from any thread that runs an op: they are queued and
// written by the UI thread (the engine log is not known to be thread safe).
#include "moho/editor/EdGpu.h"
#include "moho/editor/EdGpuOps.h"

#include <Windows.h>
#include <d3d9.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/backends/d3d9/DeviceD3D9.hpp"
#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/console/CConCommand.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"
#include "moho/render/d3d/CD3DDevice.h"

namespace moho::editor::gpu
{
  namespace
  {
    const DWORD gMainThread = GetCurrentThreadId();   // static initialisation runs on the main thread

    TConVar<bool> gConCompute("ed_gpu_compute", "Editor: 1 = field ops with a GPU version (blur, box ...) run on a Direct3D 11 compute device when one is available, 0 = always on the CPU (default 1)", &GetPolicy().enabled);
    TConVar<bool> gConSelfTestFirst("ed_gpu_selftest_first", "Editor: 1 = the GPU self test runs once before the first real GPU op of a session, and a device that fails it is switched off (default 1)", &GetPolicy().selfTestFirst);
    TConVar<float> gConMinCells("ed_gpu_min_cells", "Editor: fields with fewer samples than this stay on the CPU even when the GPU is on (default 65536 = 256 x 256)", &GetPolicy().minCells);

    // ---- log -----------------------------------------------------------------------------------------------------------------------------------

    std::mutex gLogMutex;
    std::deque<std::string> gLogLines;

    void QueueLog(const char* const line)
    {
      const std::lock_guard<std::mutex> lock(gLogMutex);
      if (gLogLines.size() < 200) {
        gLogLines.emplace_back(line != nullptr ? line : "");
      }
    }

    /** Writes the queued lines to the engine log; only on the main thread. */
    void DrainLog()
    {
      if (GetCurrentThreadId() != gMainThread) {
        return;
      }
      std::deque<std::string> lines;
      {
        const std::lock_guard<std::mutex> lock(gLogMutex);
        lines.swap(gLogLines);
      }
      for (const std::string& l : lines) {
        gpg::Logf("EDITOR %s", l.c_str());
      }
    }

    // ---- the adapter of the game's Direct3D 9 device ----------------------------------------------------------------------------------------------

    bool FindD3D9Adapter(UINT& vendorId, UINT& deviceId)
    {
      CD3DDevice* const device = D3D_GetDevice();
      if (device == nullptr || device->GetGalDevice() == nullptr) {
        return false;
      }
      IDirect3DDevice9* const dev = static_cast<gpg::gal::DeviceD3D9*>(device->GetGalDevice())->mDevice;
      if (dev == nullptr) {
        return false;
      }
      D3DDEVICE_CREATION_PARAMETERS cp = {};
      if (FAILED(dev->GetCreationParameters(&cp))) {
        return false;
      }
      IDirect3D9* d3d = nullptr;
      if (FAILED(dev->GetDirect3D(&d3d)) || d3d == nullptr) {
        return false;
      }
      D3DADAPTER_IDENTIFIER9 id = {};
      const HRESULT hr = d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id);
      d3d->Release();
      if (FAILED(hr)) {
        return false;
      }
      vendorId = id.VendorId;
      deviceId = id.DeviceId;
      return true;
    }

    /** TryRun's hook and the natives' entry: true once the preferred adapter is known (or cannot be found any more). */
    bool BeforeUse()
    {
      DrainLog();
      if (GetCurrentThreadId() != gMainThread) {
        return false;   // another thread: not allowed to touch the D3D9 device; the next call on the main thread does it
      }
      static int attempts = 0;       // main thread only
      static bool known = false;
      if (known || attempts >= 20) {
        return true;
      }
      ++attempts;
      UINT vendor = 0;
      UINT device = 0;
      if (FindD3D9Adapter(vendor, device)) {
        SetPreferredAdapter(vendor, device);
        known = true;
      }
      return true;
    }

    // ---- Lua helpers ----------------------------------------------------------------------------------------------------------------------------

    void SetNumber(lua_State* const L, const char* const key, const double value)
    {
      lua_pushstring(L, key);
      lua_pushnumber(L, value);
      lua_settable(L, -3);
    }

    void SetString(lua_State* const L, const char* const key, const std::string& value)
    {
      lua_pushstring(L, key);
      lua_pushstring(L, value.c_str());
      lua_settable(L, -3);
    }

    void SetBool(lua_State* const L, const char* const key, const bool value)
    {
      lua_pushstring(L, key);
      lua_pushboolean(L, value ? 1 : 0);
      lua_settable(L, -3);
    }

    // ed_gpu_info([probe])
    int __cdecl cfunc_GpuInfo(lua_State* const L)
    {
      const bool probe = lua_gettop(L) < 1 || lua_toboolean(L, 1) != 0;
      BeforeUse();
      const BackendInfo info = CurrentBackend().Info(probe);
      const Policy& policy = GetPolicy();
      DrainLog();
      lua_newtable(L);
      SetBool(L, "compiledIn", info.compiledIn);
      SetBool(L, "tried", info.tried);
      SetBool(L, "available", info.available);
      SetBool(L, "enabled", policy.enabled);
      SetBool(L, "active", policy.enabled && info.available);
      SetBool(L, "failed", info.failed);
      SetBool(L, "selfTestPassed", SelfTestPassed());
      SetString(L, "adapter", info.adapter);
      SetNumber(L, "vramMB", static_cast<double>(info.vramMB));
      SetNumber(L, "maxMB", static_cast<double>(info.maxBytes >> 20));
      SetString(L, "featureLevel", info.featureLevel);
      SetString(L, "compiler", info.compiler);
      SetBool(L, "textures", info.textures);
      SetString(L, "lastError", info.lastError);
      SetString(L, "lastOpError", LastOpError());
      SetNumber(L, "minCells", policy.minCells);
      lua_pushstring(L, "ops");
      lua_newtable(L);
      int i = 1;
      for (const std::string& n : GpuOpNames()) {
        lua_pushnumber(L, i++);
        lua_pushstring(L, n.c_str());
        lua_settable(L, -3);
      }
      lua_settable(L, -3);
      return 1;
    }

    // ed_gpu_enable(on[, reset]) -> enabled
    int __cdecl cfunc_GpuEnable(lua_State* const L)
    {
      GetPolicy().enabled = lua_toboolean(L, 1) != 0;
      if (lua_toboolean(L, 2) != 0) {
        CurrentBackend().Reset();
        ResetSelfTestState();
      }
      lua_pushboolean(L, GetPolicy().enabled ? 1 : 0);
      return 1;
    }

    // ed_gpu_selftest()
    int __cdecl cfunc_GpuSelfTest(lua_State* const L)
    {
      BeforeUse();
      const SelfTestReport rep = RunSelfTest();
      DrainLog();
      lua_newtable(L);
      SetBool(L, "ok", rep.ok);
      SetBool(L, "texturesOk", rep.texturesOk);
      SetNumber(L, "ms", rep.ms);
      SetString(L, "error", rep.error);
      lua_pushstring(L, "parts");
      lua_newtable(L);
      int i = 1;
      for (const SelfTestPart& p : rep.parts) {
        lua_pushnumber(L, i++);
        lua_newtable(L);
        SetString(L, "name", p.name);
        SetBool(L, "ok", p.ok);
        SetString(L, "detail", p.detail);
        SetNumber(L, "ms", p.ms);
        lua_settable(L, -3);
      }
      lua_settable(L, -3);
      return 1;
    }

    // ed_gpu_stats([reset])
    int __cdecl cfunc_GpuStats(lua_State* const L)
    {
      const std::map<std::string, field::OpStat> stats = field::OpStats(lua_toboolean(L, 1) != 0);
      double cpu = 0, cpuMs = 0, gpu = 0, gpuMs = 0, fallbacks = 0;
      lua_newtable(L);
      SetBool(L, "enabled", GetPolicy().enabled);
      lua_pushstring(L, "ops");
      lua_newtable(L);
      for (const auto& kv : stats) {
        lua_pushstring(L, kv.first.c_str());
        lua_newtable(L);
        SetNumber(L, "cpu", static_cast<double>(kv.second.cpuCalls));
        SetNumber(L, "cpuMs", kv.second.cpuMs);
        SetNumber(L, "gpu", static_cast<double>(kv.second.gpuCalls));
        SetNumber(L, "gpuMs", kv.second.gpuMs);
        SetNumber(L, "fallbacks", static_cast<double>(kv.second.fallbacks));
        lua_settable(L, -3);
        cpu += static_cast<double>(kv.second.cpuCalls);
        cpuMs += kv.second.cpuMs;
        gpu += static_cast<double>(kv.second.gpuCalls);
        gpuMs += kv.second.gpuMs;
        fallbacks += static_cast<double>(kv.second.fallbacks);
      }
      lua_settable(L, -3);
      SetNumber(L, "cpu", cpu);
      SetNumber(L, "cpuMs", cpuMs);
      SetNumber(L, "gpu", gpu);
      SetNumber(L, "gpuMs", gpuMs);
      SetNumber(L, "fallbacks", fallbacks);
      return 1;
    }

    [[nodiscard]] CScrLuaInitFormSet& UserLuaInitSet()
    {
      if (CScrLuaInitFormSet* const set = SCR_FindLuaInitFormSet("User"); set != nullptr) {
        return *set;
      }
      static CScrLuaInitFormSet fallbackSet("User");
      return fallbackSet;
    }

    struct EdGpuLuaBootstrap
    {
      EdGpuLuaBootstrap()
      {
        SetLogSink(&QueueLog);
        SetBeforeUseHook(&BeforeUse);
        Install();   // the field engine's interceptor
        auto& set = UserLuaInitSet();
        (void)new CScrLuaBinder(set, "ed_gpu_info", &cfunc_GpuInfo, nullptr, "<global>",
                                "ed_gpu_info([probe]) -> { available, enabled, active, adapter, vramMB, featureLevel, textures, lastError, lastOpError, ops, ... } (see EdGpuLua.cpp)");
        (void)new CScrLuaBinder(set, "ed_gpu_enable", &cfunc_GpuEnable, nullptr, "<global>", "ed_gpu_enable(on[, reset]) -> enabled; reset = true forgets a failure so the next op creates a new device");
        (void)new CScrLuaBinder(set, "ed_gpu_selftest", &cfunc_GpuSelfTest, nullptr, "<global>", "ed_gpu_selftest() -> { ok, texturesOk, ms, error, parts = { { name, ok, detail, ms } } }");
        (void)new CScrLuaBinder(set, "ed_gpu_stats", &cfunc_GpuStats, nullptr, "<global>", "ed_gpu_stats([reset]) -> { enabled, ops = { name = { cpu, cpuMs, gpu, gpuMs, fallbacks } }, cpu, cpuMs, gpu, gpuMs, fallbacks }");
      }
    };

    const EdGpuLuaBootstrap gEdGpuLuaBootstrap{};
  } // namespace
} // namespace moho::editor::gpu
