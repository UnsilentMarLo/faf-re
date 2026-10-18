// EDITOR: the field engine core (docs/projects/editor/TERRAIN_GEN_DESIGN.md, stage T1). Engine independent (no moho types), so it is tested on its
// own (editor/tests/EdFieldTest.cpp, scripts/ed_native_tests.py).
//
// A Grid is a float32 raster with a world placement: sample (x, z) lies at world position (ox + x * cell, oz + z * cell). Heights are world units
// (the document's words / 128), masks are 0..1. Ops are pure functions of their input grids and parameters; every output sample is computed
// from the inputs only (no accumulation across threads), and reductions combine fixed row chunks in chunk order, so the result is bit identical
// for any thread count. Noise is a function of (lattice position, seed) only.
//
// Compile flags that matter for the golden checksums: no fast-math, no FMA contraction (the .cpp pins fp_contract off).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace moho::editor::field
{
  struct Grid
  {
    int w = 0;
    int h = 0;
    float cell = 1.0f;   // world units between neighbouring samples
    float ox = 0.0f;     // world position of sample (0, 0)
    float oz = 0.0f;
    std::vector<float> v;

    Grid() = default;
    Grid(int width, int height, float cellSize, float x0, float z0, float fill = 0.0f)
      : w(width), h(height), cell(cellSize), ox(x0), oz(z0), v(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill)
    {
    }
    [[nodiscard]] std::size_t Count() const noexcept { return static_cast<std::size_t>(w) * static_cast<std::size_t>(h); }
    [[nodiscard]] float At(int x, int z) const { return v[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
    float& At(int x, int z) { return v[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
    [[nodiscard]] bool SameShape(const Grid& o) const noexcept { return w == o.w && h == o.h; }
  };

  using GridPtr = std::shared_ptr<const Grid>;

  /** The parameters of an op: numbers, strings and number arrays by name (Lua tables are flattened into these). */
  struct Params
  {
    std::map<std::string, double> num;
    std::map<std::string, std::string> str;
    std::map<std::string, std::vector<double>> arr;

    [[nodiscard]] double N(const char* key, double fallback) const;
    [[nodiscard]] bool Has(const char* key) const { return num.count(key) != 0; }
    [[nodiscard]] std::string S(const char* key, const char* fallback) const;
    [[nodiscard]] const std::vector<double>* A(const char* key) const;
  };

  struct OpArgs
  {
    Params p;
    std::map<std::string, GridPtr> in;   // named input grids: a, b, mask, dx, dz, like
    std::map<std::string, Grid>* extra = nullptr;   // when set, ops with more than one result (erosion: wear, deposit, flow ...) store the others here by name
  };

  /** Ops that live in their own files (EdErosion.cpp) register themselves here at start-up; RunOp / OpNames know them like the built-in ones. */
  using ExternalOpFn = bool (*)(const OpArgs&, Grid&, std::string&);
  void RegisterOp(const char* name, ExternalOpFn fn);

  /** Runs op `name`. false with `err` set on a bad op name, missing input or parameter. */
  bool RunOp(const std::string& name, const OpArgs& args, Grid& out, std::string& err);

  /** Names of all ops (sorted). */
  [[nodiscard]] const std::vector<std::string>& OpNames();

  // ---- thread pool -----------------------------------------------------------------------------------------------------------------------------
  /** Number of worker threads including the caller (1 = everything runs inline). Changing it never changes any result. */
  void SetThreads(int n);
  [[nodiscard]] int Threads();
  /** Runs fn(i) for i in [0, count); i is handed out dynamically, fn must only write data that belongs to i. */
  void ParallelFor(int count, const std::function<void(int)>& fn);

  // ---- analysis helpers ------------------------------------------------------------------------------------------------------------------------
  struct Stats
  {
    float min = 0, max = 0;
    double mean = 0, sd = 0;
  };
  [[nodiscard]] Stats Measure(const Grid& g);
  /** FNV-1a over the sample bit patterns (-0 is read as +0), the dimensions are part of the hash. */
  [[nodiscard]] std::uint64_t Checksum(const Grid& g);
  /** Bilinear sample at world position (clamped to the grid). */
  [[nodiscard]] float SampleWorld(const Grid& g, double wx, double wz);
  /** Box-average downsample to pw x ph values (the preview thumbnail). */
  void Downsample(const Grid& g, int pw, int ph, std::vector<float>& out);
} // namespace moho::editor::field
