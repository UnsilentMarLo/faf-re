// EDITOR: the erosion suite of the field engine (docs/projects/editor/TERRAIN_GEN_DESIGN.md, stage T3). Engine independent, tested on its own
// (editor/tests/EdErosionTest.cpp, scripts/ed_native_tests.py).
//
//   ops (registered in the field engine's op table, see EdFieldCore.h):
//     erode_hydraulic   grid erosion on the pipe / shallow water model (Mei et al.): water, suspended sediment and outflow fluxes per cell; capacity from slope and speed;
//                       erosion, deposition, evaporation; thermal slides coupled in every few iterations
//     erode_droplet     particle erosion (Beyer / Lague): droplets with inertia run downhill, pick up and drop sediment; tiled, bit identical for any thread count
//     erode_stream      stream power incision on a drainage network (pit filled flow routing, implicit solver): river valleys with branching channels
//   every op returns the new height (`output=height`, the default) or, with `output=` wear | deposit | flow | water | sediment, one of the other results (the editor asks for all at
//   once through OpArgs::extra). Inputs: a (the height field), mask (erodibility 0..1, optional), like.
//
//   ErodeSession: the local erosion brush. It keeps a float copy of the map heights and the water / sediment / flux of the whole map for the length of a stroke; every dab runs a few
//   iterations of the hydraulic kernels on a window around the brush (border fixed), weighted by the brush falloff, so water keeps flowing from dab to dab.
#pragma once

#include <cstdint>
#include <vector>

#include "moho/editor/EdFieldCore.h"

namespace moho::editor::field::erosion
{
  struct HydroParams
  {
    int iterations = 200;
    float rain = 0.006f;          // water depth added to every cell per iteration (world units)
    float evaporation = 0.02f;    // fraction of the water that evaporates per iteration
    float gain = 0.08f;           // pipe gain: outflow flux grows by gain * (water surface difference) per iteration (<= 0.25 keeps the pipes stable)
    float capacity = 1.0f;        // sediment capacity = capacity * sin(slope) * speed * depth factor
    float erosion = 0.12f;        // share of the missing sediment that is dissolved per iteration
    float deposition = 0.06f;      // share of the surplus sediment that settles per iteration
    float depth_max = 0.05f;      // water depth (world units) at which the capacity saturates (shallow water carries less)
    float min_slope = 0.02f;      // sin(slope) floor so flat ground still erodes a little
    float max_speed = 1.0f;       // cells per iteration
    float cut_limit = 0.5f;       // one step may remove at most this share of the drop to the lowest neighbour (0 = unlimited): erosion never digs pits
    float talus = 0.8f;           // thermal slide limit (rise per run)
    float thermal = 0.0f;         // thermal rate 0..1 per slide step (0 = no thermal coupling)
    int thermal_every = 2;        // a thermal step after this many hydraulic iterations
    bool pin_border = true;       // the outermost ring keeps its height and drains water (false: closed edges)
    bool settle = true;           // at the end the remaining sediment settles where it is (conserves mass)
    bool track = true;            // keep the wear, deposit and flow masks (off: the brush and previews do not need them)
  };

  /** The state of the grid solver on a rectangle. Arrays are row major w * h; empty optional arrays mean 1. */
  struct HydroState
  {
    int w = 0;
    int h = 0;
    float cell = 1.0f;
    std::vector<float> t;         // terrain height (world units)
    std::vector<float> d;         // water depth
    std::vector<float> s;         // suspended sediment (as depth of terrain)
    std::vector<float> f[4];      // outflow flux to -x, +x, -z, +z
    std::vector<float> rain_rate; // per cell factor of the rain (brush weight); empty = 1
    std::vector<float> erode_rate;// per cell factor of erosion and thermal slides (brush weight); empty = 1
    std::vector<float> rock;      // per cell erodibility; empty = 1
    std::vector<float> wear;      // accumulated erosion (>= 0), optional
    std::vector<float> dep;       // accumulated deposition (>= 0), optional
    std::vector<float> flow;      // accumulated discharge (speed * depth), optional

    void Allocate(int width, int height, float cellSize);
  };

  /** Runs `iterations` steps. Deterministic for any thread count. */
  void RunHydro(HydroState& st, const HydroParams& p, int iterations);

  /** One thermal slide step on t (see the `thermal` op); `rate` per cell optional (empty = scalar), the border ring is left alone when pinBorder. */
  void ThermalStep(std::vector<float>& t, int w, int h, float cell, float talus, float rate, const std::vector<float>& rateMask, bool pinBorder);

  // ---- the local erosion brush --------------------------------------------------------------------------------------------------------------

  struct BrushDab
  {
    double cx = 0, cz = 0;        // sample coordinates of the centre
    double radius = 12;           // samples
    double falloff = 1.0;         // like the other brushes: share of the radius the edge falls off over
    int iterations = 8;
    double strength = 1.0;        // scales the dissolving, rain and thermal rates
    int mode = 0;                 // 0 erode, 1 rain (water only under the brush, erodes downstream), 2 deposit (sediment dumped under the brush)
    double margin = -1;           // extra window around the brush (samples); < 0: automatic
    double amount = 0.5;          // deposit mode: sediment depth dumped at the centre per dab
    std::vector<float> weights;   // optional brush weights for the window (row major, window size); empty = round falloff
  };

  class ErodeSession
  {
  public:
    /** Starts a stroke on a map of w x h samples; `words` are the height words (1/128 world unit). */
    void Begin(int w, int h, const std::uint16_t* words, const HydroParams& params);
    [[nodiscard]] bool Active() const { return active_; }
    /** One dab. On success `x0..z1` is the inclusive window and `words` its new height words (row major); returns false when nothing was done. */
    bool Dab(const BrushDab& dab, int& x0, int& z0, int& x1, int& z1, std::vector<std::uint16_t>& words);
    /** Settles the remaining sediment into the terrain; returns the changed window like Dab. */
    bool Finish(int& x0, int& z0, int& x1, int& z1, std::vector<std::uint16_t>& words);
    void End();
    [[nodiscard]] HydroParams& Params() { return params_; }

  private:
    bool active_ = false;
    int w_ = 0, h_ = 0;
    HydroParams params_;
    std::vector<float> t_;                    // float copy of the heights (world units)
    std::vector<float> d_, s_, f_[4];         // water, sediment, fluxes of the whole map
    int dx0_ = 0, dz0_ = 0, dx1_ = -1, dz1_ = -1;   // dirty rectangle (where state may be non-zero)
  };
} // namespace moho::editor::field::erosion
