// EDITOR: the erosion suite of the field engine (docs/projects/editor/TERRAIN_GEN_DESIGN.md, stage T3). Engine independent, tested on its own
// (editor/tests/EdErosionTest.cpp, scripts/ed_native_tests.py).
//
//   ops (registered in the field engine's op table, see EdFieldCore.h):
//     erode_hydraulic   grid erosion on the pipe / shallow water model (Mei et al.): water, suspended sediment and outflow fluxes per cell; capacity from slope and speed;
//                       erosion, deposition, evaporation; thermal slides coupled in every few iterations
//     erode_droplet     particle erosion (Beyer / Lague): droplets with inertia run downhill, pick up and drop sediment; tiled, bit identical for any thread count
//     erode_stream      stream power incision on a drainage network (pit filled flow routing, implicit solver): river valleys with branching channels
//   edge (erode_hydraulic): open (default) = the edge cells are ordinary cells, water and sediment run out over the map edge, and the outermost cells take the change of their inner neighbour
//     (no step is left at the border); closed = reflecting walls; pinned = the outer ring keeps its height (the old behaviour, which left a wall of untouched ground at the border).
//     The droplets reflect their digging brush at the map edge; the stream solver raises the edge cells with the uplift. The local brush is not a grid solve: see ErodeSession.
//   settings every erosion op takes (all off by default except where noted): vary / vary_scale (hard and soft ground: noise on the erodibility), rain_vary (wetter and drier patches: hydraulic
//     rain, droplet release probability, stream catchment), protect (ridges and summits are harder, and slide less), seed (of these patterns), conserve (0..1: puts the lost volume back where
//     sediment settles), keep_range (1: stretch the result back to the input's height range), max_change (units: no sample moves further than this); droplets: drop_vary (every droplet its own
//     size / life / inertia / load), accel (1 = speeds up downhill, the default; 0 = the old rule that slowed droplets down on slopes); stream: jitter (flow routed over ground + noise).
//   every op returns the new height (`output=height`, the default) or, with `output=` wear | deposit | flow | water | sediment, one of the other results (the editor asks for all at
//   once through OpArgs::extra). Inputs: a (the height field), mask (erodibility 0..1, optional), like.
//
//   ErodeSession: the local erosion brush. It keeps a float copy of the map heights for the length of a stroke; every dab releases water droplets (Beyer / Lague) at random positions inside
//   the brush, they run downhill on the bilinear surface with inertia, dig where they are fast on a slope and settle where they slow down. Everything a droplet digs or settles is
//   multiplied by the brush weight of the sample, so the change is exactly zero outside the brush and follows its falloff inside (rain brush: digs on to a wider reach). The grid solver is not
//   used any more: its four flow directions drew a diamond and parallel rills, and its water state stood still outside the dab's window.
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
    bool pin_border = true;       // the sides in pin_sides keep their height and drain water (the other sides: open_edge or closed walls)
    int pin_sides = 15;           // bit 0 / 1 = the -x / +x column, bit 2 / 3 = the -z / +z row of the rectangle
    bool open_edge = false;       // a side that is not pinned lets water and sediment run out over the edge (the edge cells are ordinary cells); false: a closed wall
    bool settle = true;           // at the end the remaining sediment settles where it is (conserves mass)
    bool track = true;            // keep the wear, deposit and flow masks (off: the brush and previews do not need them)
    float vary = 0.0f;            // brush only: 0..1, hard and soft ground under the brush (noise on the erodibility and the release of droplets), the same pattern for every dab of a map
    float vary_scale = 40.0f;     // feature size of that pattern (samples)
    float conserve = 0.7f;        // brush only: 0..1, the share of the sediment that runs out of the brush that is put back inside it (valleys, the foot of the slope): the mountain keeps its mass
    float inertia = 0.3f;         // brush only: how much a droplet keeps its direction instead of following the slope (0 straight down the steepest way, 0.9 hardly turns)
    std::uint32_t seed = 1;       // brush only: seed of the droplet positions (the stroke's first dab position is mixed in, so a stroke is repeatable)
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
    std::vector<float> thermal_rate;   // per cell factor of the thermal slides only; empty = erode_rate (the brush weights)
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
    double margin = -1;           // extra window around the brush (samples); < 0: BrushDefaultMargin. Only the rain brush uses it: it digs up to 0.9 * margin past the radius, the other brushes never touch a sample
                                  // outside the radius
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
    /** Nothing is held back between dabs any more (a droplet settles its own load), so this always returns false; kept for the callers that settled the water state of the old brush. */
    bool Finish(int& x0, int& z0, int& x1, int& z1, std::vector<std::uint16_t>& words);
    void End();
    [[nodiscard]] HydroParams& Params() { return params_; }

  private:
    bool active_ = false;
    int w_ = 0, h_ = 0;
    HydroParams params_;
    std::vector<float> t_;                    // float copy of the heights (world units)
    std::uint64_t dabs_ = 0;                  // dabs of this stroke so far (every dab has its own droplet streams)
    std::uint32_t seed_ = 0;
  };

  /** The inclusive window ErodeSession::Dab works on (and that `weights` must be sized for): the brush plus its margin, one sample more, clamped to the map. */
  void BrushWindow(const BrushDab& dab, int w, int h, int& x0, int& z0, int& x1, int& z1);
  /** The margin a dab with margin < 0 uses: 0 for erode and deposit, 0.8 * radius + 3 for rain. */
  double BrushDefaultMargin(int mode, double radius);
} // namespace moho::editor::field::erosion
