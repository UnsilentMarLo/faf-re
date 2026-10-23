// EDITOR GPU compute: the registry of GPU implementations of field ops, the policy that decides when to use them, the statistics and the self test (EdGpu.h has the device side,
// docs/projects/editor/GPU_COMPUTE.md the whole picture). Plain C++, no Direct3D: tested with the CPU emulator (tests/EdGpuTest.cpp).
//
// How an op gets to the GPU: RunOp (EdFieldCore.cpp) asks TryRun first. TryRun runs the GPU version only when every condition holds, and on ANY other outcome returns false so the
// CPU op runs as before:
//   policy.enabled (ed_gpu_compute)   the backend may work (Backend::MayWork)   the op has a registered GpuOpFn   the field has at least policy.minCells samples (ed_gpu_min_cells)
//   the backend is not busy with another thread's op (never waits)   the GpuOpFn returns Done (Unsupported = parameters it does not handle, Failed = an error: both fall back).
#pragma once

#include <string>
#include <vector>

#include "moho/editor/EdFieldCore.h"
#include "moho/editor/EdGpu.h"

namespace moho::editor::gpu
{
  enum class OpResult
  {
    Done,          // `out` is complete
    Unsupported,   // these inputs / parameters are not handled on the GPU: the CPU op runs (nothing is counted as a failure)
    Failed,        // something went wrong on the device (`err` says what): the CPU op runs, the fall-back is counted and the reason kept (LastOpError)
  };

  /** A GPU version of CPU op `name`: same parameters, same result (within float tolerance). Must leave `out` untouched unless it returns Done. May throw nothing the caller cannot take (TryRun catches). */
  using GpuOpFn = OpResult (*)(Executor& ex, const field::OpArgs& args, field::Grid& out, std::string& err);

  /** Console variables ed_gpu_compute / ed_gpu_min_cells (EdGpuLua.cpp binds them to these). */
  struct Policy
  {
    bool enabled = true;           // ed_gpu_compute
    float minCells = 65536.0f;     // ed_gpu_min_cells: smaller fields stay on the CPU (upload + download cost more than they save)
    bool selfTestFirst = true;     // ed_gpu_selftest_first: the self test runs once before the first real op of a session; a device that fails it is switched off (the CPU does everything)
  };
  [[nodiscard]] Policy& GetPolicy();

  /** Registers the GPU version of the CPU op `name` (once per name; a second call replaces the first). Static initialisers of op files call this. */
  void RegisterGpuOp(const char* name, GpuOpFn fn);
  [[nodiscard]] std::vector<std::string> GpuOpNames();

  /** Samples the op will produce, from its inputs (`a`, else `like`, else width * height). */
  [[nodiscard]] double CellsOf(const field::OpArgs& args);

  /** The interceptor RunOp consults (see the header comment). Thread safe. */
  bool TryRun(const std::string& name, const field::OpArgs& args, field::Grid& out);
  /** Installs TryRun as the field engine's interceptor (EdGpuLua.cpp does this at start-up; the test does it explicitly). */
  void Install();

  /**
   * A function TryRun calls before it uses the backend for the first time, again on every later call until it returns true (EdGpuLua.cpp: finds the adapter the game's Direct3D 9
   * device runs on, which is only safe on the main thread; false = "not yet / not on this thread").
   */
  using BeforeUseFn = bool (*)();
  void SetBeforeUseHook(BeforeUseFn fn);

  /** True once the self test has passed on the current device (set by RunSelfTest() and by TryRun's first-use test). */
  [[nodiscard]] bool SelfTestPassed();
  /** Forget it (after a device reset). */
  void ResetSelfTestState();

  /** The reason of the last Failed op or refused lease ("blur: ..."), empty if none. */
  [[nodiscard]] std::string LastOpError();
  void SetLastOpError(const std::string& text);

  // ---- self test -----------------------------------------------------------------------------------------------------------------------------

  struct SelfTestPart
  {
    std::string name;
    bool ok = false;
    std::string detail;
    double ms = 0.0;
  };

  struct SelfTestReport
  {
    bool ok = false;            // every required part passed
    bool texturesOk = false;    // the texture part (optional: a device without it still serves buffer kernels)
    double ms = 0.0;
    std::string error;          // why nothing ran (no device ...)
    std::vector<SelfTestPart> parts;
  };

  /** Runs the self test on `ex`: scale + offset on 64 x 64, a group shared memory reduction, the blur op against the CPU op, a texture sample against the CPU. */
  [[nodiscard]] SelfTestReport RunSelfTest(Executor& ex);
  /** The same on the process backend (takes the lease, waits for it; a device failure marks the backend failed). */
  [[nodiscard]] SelfTestReport RunSelfTest();
} // namespace moho::editor::gpu
