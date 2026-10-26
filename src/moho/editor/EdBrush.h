// EDITOR: the math of one round terrain brush dab (raise, lower, plateau, flatten, smooth, noise) on a grid of 16-bit height words.
//
// This is a port of the sample loop of editor/runtime/lua/editor/terrain.lua (DabOne, Weight, Noise, BoxBlur) so a dab costs one native call
// instead of a few ten thousand Lua operations. It is a header-only pure function (no engine types) so it can be tested on its own
// (moho/editor/tests/EdBrushTest.cpp); the Lua native and the document write are EdBrush.cpp.
//
// The game's Lua numbers are 32-bit floats and every Lua operation rounds its result to a float. The code below keeps that: one statement per
// operation, `float` locals, so a dab gives the same words as the Lua loop (the self test compares the two when the native exists). Do not
// "simplify" an expression into a different operation order, and build with /fp:precise (the default), not /fp:fast.
//
// The noise mode is smooth value noise on a lattice (terrain.lua ValueNoise / SmoothNoise): the lattice values come from a seeded integer hash whose
// intermediates stay below 2^24, so the Lua VM's floats and these integers agree exactly.
//
// Units: heights are words, 128 per world unit; a dab at (cx, cz) in sample coordinates with a radius in samples; words are clamped to 0..65535.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace moho::editor::brush
{
  /**
   * A brush shape mask (editor/runtime/lua/editor/brushes.lua): an n x n grid of weights 0..1 over the unit square -1..1 (row major, z outer, x fastest: sample (i, j) lies at
   * u = -1 + 2 i / (n - 1), v = -1 + 2 j / (n - 1)), uploaded once per shape by ed_brush_shape_set and referred to by a handle number. Lua rasterises procedural shapes and loads
   * image masks into it; a dab turns it by an angle and reads it bilinearly (ShapeSampler), exactly as brushes.lua Weight does for the Lua loop.
   */
  struct Shape
  {
    int n = 0;
    std::vector<float> values;
  };

  /** The shapes uploaded so far, by handle (one instance for the whole exe: an inline function's static is shared by every translation unit). */
  [[nodiscard]] inline std::map<int, Shape>& ShapeRegistry()
  {
    static std::map<int, Shape> shapes;
    return shapes;
  }

  constexpr int kMaxShapeSize = 512;
  constexpr std::size_t kMaxShapes = 4096;

  /** Stores (or replaces) the shape `handle`; false for a bad size or a full registry. */
  [[nodiscard]] inline bool SetShape(const int handle, const int n, std::vector<float>&& values)
  {
    if (handle <= 0 || n < 2 || n > kMaxShapeSize || values.size() != static_cast<std::size_t>(n) * static_cast<std::size_t>(n)) {
      return false;
    }
    std::map<int, Shape>& shapes = ShapeRegistry();
    if (shapes.find(handle) == shapes.end() && shapes.size() >= kMaxShapes) {
      return false;
    }
    Shape& s = shapes[handle];
    s.n = n;
    s.values = std::move(values);
    return true;
  }

  /** The shape of a handle, or null. */
  [[nodiscard]] inline const Shape* FindShape(const int handle)
  {
    const std::map<int, Shape>& shapes = ShapeRegistry();
    const auto it = shapes.find(handle);
    return it == shapes.end() ? nullptr : &it->second;
  }

  /**
   * brushes.lua Weight() on a shape grid: the mask value at the offset (dx, dz) from the centre of a brush of radius r, the mask turned by `angle` degrees. One operation per statement
   * (float roundings as in the Lua VM), the same order as the Lua source:
   *   u = (dx c + dz s) / r,  v = (-dx s + dz c) / r,  0 beyond u^2 + v^2 > 1.6,  bilinear read of the grid, 0 outside it.
   */
  struct ShapeSampler
  {
    const Shape* shape = nullptr;
    float c = 1.0f;
    float s = 0.0f;

    ShapeSampler() = default;
    ShapeSampler(const Shape* const sh, const float angleDegrees) : shape(sh)
    {
      // math.rad(angle) = angle * (pi / 180) in double, stored as a float; cos / sin of that float, stored as floats
      const float a = static_cast<float>(static_cast<double>(angleDegrees) * (3.14159265358979323846 / 180.0));
      c = static_cast<float>(std::cos(static_cast<double>(a)));
      s = static_cast<float>(std::sin(static_cast<double>(a)));
    }

    [[nodiscard]] float At(const float dx, const float dz, const float r) const
    {
      float p = dx * c;
      float q = dz * s;
      float u = p + q;
      u = u / r;
      const float ndx = -dx;
      p = ndx * s;
      q = dz * c;
      float v = p + q;
      v = v / r;
      const float uu = u * u;
      const float vv = v * v;
      if (uu + vv > 1.6f) {
        return 0.0f;
      }
      const int n = shape->n;
      const float top = static_cast<float>(n - 1);
      float fx = u + 1.0f;
      fx = fx * 0.5f;
      fx = fx * top;
      float fz = v + 1.0f;
      fz = fz * 0.5f;
      fz = fz * top;
      if (fx < 0.0f || fz < 0.0f || fx > top || fz > top) {
        return 0.0f;
      }
      const int x0 = std::min(n - 2, static_cast<int>(std::floor(fx)));
      const int z0 = std::min(n - 2, static_cast<int>(std::floor(fz)));
      const float tx = fx - static_cast<float>(x0);
      const float tz = fz - static_cast<float>(z0);
      const float* const g = shape->values.data();
      const float a00 = g[static_cast<std::size_t>(z0) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x0)];
      const float a10 = g[static_cast<std::size_t>(z0) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x0 + 1)];
      const float a01 = g[static_cast<std::size_t>(z0 + 1) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x0)];
      const float a11 = g[static_cast<std::size_t>(z0 + 1) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x0 + 1)];
      const float ix = 1.0f - tx;
      const float iz = 1.0f - tz;
      float t0 = a00 * ix;
      float t1 = a10 * tx;
      const float topRow = t0 + t1;
      t0 = a01 * ix;
      t1 = a11 * tx;
      const float bottomRow = t0 + t1;
      t0 = topRow * iz;
      t1 = bottomRow * tz;
      return t0 + t1;
    }
  };

  enum class Mode
  {
    Raise,
    Lower,
    Plateau,
    Flatten,
    Smooth,
    Noise,
  };

  /** Mode from the name terrain.lua uses; false for a mode this file does not do (the Lua path keeps it). */
  [[nodiscard]] inline bool ModeFromName(const char* const name, Mode& out)
  {
    if (name == nullptr) {
      return false;
    }
    struct Entry
    {
      const char* name;
      Mode mode;
    };
    static const Entry kModes[] = {
      {"raise", Mode::Raise},   {"lower", Mode::Lower},   {"plateau", Mode::Plateau},
      {"flatten", Mode::Flatten}, {"smooth", Mode::Smooth}, {"noise", Mode::Noise},
    };
    for (const Entry& e : kModes) {
      if (std::strcmp(name, e.name) == 0) {
        out = e.mode;
        return true;
      }
    }
    return false;
  }

  /** One dab. Everything is in the units of terrain.lua: the strength is world units per dab for raise / lower / noise, a blend share for the others. */
  struct Dab
  {
    float cx = 0.0f;         // centre in sample coordinates
    float cz = 0.0f;
    float radius = 1.0f;     // samples
    Mode mode = Mode::Raise;
    float strength = 1.0f;
    int dir = 1;             // -1 inverts the brush
    float falloff = 1.0f;    // 0..1, the share of the radius the edge falls off over
    float level = -1.0f;     // flatten / plateau target in world units; < 0 = the ground under the centre of the dab
    int seed = 0;            // noise: the stroke id
    float noiseScale = 8.0f;      // noise: lattice cell size in samples (>= 1)
    int noiseOctaves = 2;         // noise: 1..4 layers, each one with half the cell size
    float noiseRoughness = 0.5f;  // noise: the share of the amplitude the next octave keeps (0..1)
    const Shape* shape = nullptr;  // optional brush shape mask: replaces the round falloff profile (falloff is then unused)
    float shapeAngle = 0.0f;       // degrees the mask is turned (brushes.lua Angle + the stroke's random turn)
  };

  /** Inclusive sample window of a dab (what the Lua path read: one sample beyond the brush on every side), clamped to a width x height grid. */
  struct Window
  {
    int x0 = 0;
    int z0 = 0;
    int x1 = -1;
    int z1 = -1;
    [[nodiscard]] bool Empty() const { return x1 < x0 || z1 < z0; }
    [[nodiscard]] int Width() const { return Empty() ? 0 : x1 - x0 + 1; }
    [[nodiscard]] int Height() const { return Empty() ? 0 : z1 - z0 + 1; }
    [[nodiscard]] std::size_t Count() const
    {
      return static_cast<std::size_t>(Width()) * static_cast<std::size_t>(Height());
    }
  };

  struct Result
  {
    int changed = 0;         // samples whose word changed
    float level = -1.0f;     // the flatten / plateau level used, world units (-1 for the other modes)
    bool usedLevel = false;
  };

  namespace detail
  {
    constexpr int kWord = 128;
    constexpr float kPi = 3.14159265f;

    /**
     * terrain.lua Hash(): a hash of (x, z, stroke) in -1..1 (the value of one lattice point of the smooth noise below). Every intermediate stays below 2^24, so it is exact in the game's 32-bit float Lua numbers
     * and in these integers alike (the first version multiplied by 374761393 and took math.mod: in 32-bit floats that product lost its low bits and
     * the hash was -1 nearly everywhere). Keep it the same as the Lua function.
     */
    [[nodiscard]] inline float Noise(const int x, const int z, const int seed)
    {
      const int a = ((x % 4093) * 1103 + 17) % 4093;
      const int b = ((z % 4091) * 1999 + 31) % 4091;
      const int c = ((seed % 4079) * 997 + 5) % 4079;
      int n = (a * 4001 + b) % 4093;
      n = (n * n + c) % 4093;
      n = (n * n + a) % 4093;
      n = (n * n + b) % 4093;
      float v = static_cast<float>(n) / 4093.0f;
      v = v * 2.0f;
      v = v - 1.0f;
      return v;
    }

    /**
     * terrain.lua ValueNoise(): smooth noise in -1..1 at the sample (x, z). The lattice has one random value (the hash above, per lattice point) every
     * `cell` samples; the value at a point is the bilinear blend of the four corners with a smoothstep weight, so it is continuous and has no
     * per-sample speckle. It depends on the world position only, so the dabs of one stroke join into continuous bumps. One operation per statement
     * (float roundings as in the Lua VM).
     */
    [[nodiscard]] inline float ValueNoise(const int x, const int z, const float cell, const int seed)
    {
      const float fx = static_cast<float>(x) / cell;
      const float fz = static_cast<float>(z) / cell;
      const float flx = std::floor(fx);
      const float flz = std::floor(fz);
      float tx = fx - flx;
      float tz = fz - flz;
      float bx = 2.0f * tx;
      bx = 3.0f - bx;
      tx = (tx * tx) * bx;
      float bz = 2.0f * tz;
      bz = 3.0f - bz;
      tz = (tz * tz) * bz;
      const int ix = static_cast<int>(flx);
      const int iz = static_cast<int>(flz);
      const float v00 = Noise(ix, iz, seed);
      const float v10 = Noise(ix + 1, iz, seed);
      const float v01 = Noise(ix, iz + 1, seed);
      const float v11 = Noise(ix + 1, iz + 1, seed);
      float top = v10 - v00;
      top = top * tx;
      top = v00 + top;
      float bottom = v11 - v01;
      bottom = bottom * tx;
      bottom = v01 + bottom;
      float r = bottom - top;
      r = r * tz;
      return top + r;
    }

    /**
     * terrain.lua SmoothNoise(): up to four octaves of ValueNoise, each with half the cell size of the one before and `roughness` times its amplitude,
     * divided by the sum of the amplitudes so the result stays in -1..1. Octaves whose cell would be under one sample are left out.
     */
    [[nodiscard]] inline float SmoothNoise(const int x, const int z, const int seed, const float scale, const int octaves, const float roughness)
    {
      float cell = scale;
      if (cell < 1.0f) {
        cell = 1.0f;
      }
      int count = octaves;
      if (count < 1) {
        count = 1;
      } else if (count > 4) {
        count = 4;
      }
      float rough = roughness;
      if (rough < 0.0f) {
        rough = 0.0f;
      } else if (rough > 1.0f) {
        rough = 1.0f;
      }
      float total = 0.0f;
      float amp = 1.0f;
      float norm = 0.0f;
      for (int o = 0; o < count; ++o) {
        if (o > 0 && cell < 1.0f) {
          break;
        }
        float v = ValueNoise(x, z, cell, seed + o * 1013);
        v = v * amp;
        total = total + v;
        norm = norm + amp;
        amp = amp * rough;
        cell = cell / 2.0f;
      }
      return total / norm;
    }

    /** terrain.lua Weight() for the round brush: 0 outside the radius, 1 inside the flat part, a cosine edge between. */
    struct Profile
    {
      float radius = 1.0f;
      float inner = 0.0f;
      float width = 1.0f;

      Profile(const float r, const float falloff)
      {
        radius = r;
        float f = falloff;
        if (f < 0.0f) {
          f = 0.0f;
        } else if (f > 1.0f) {
          f = 1.0f;
        }
        const float scaled = r * f;
        const float wide = std::max(scaled, 1.0f);
        width = std::min(r, wide);
        inner = r - width;
      }

      [[nodiscard]] float At(const float distance) const
      {
        if (distance >= radius) {
          return 0.0f;
        }
        if (distance <= inner) {
          return 1.0f;
        }
        float t = distance - inner;
        t = kPi * t;
        t = t / width;
        const float c = static_cast<float>(std::cos(static_cast<double>(t)));
        const float s = 1.0f + c;
        return 0.5f * s;
      }
    };

    /** terrain.lua BoxBlur(): a box blur of a w x h grid with half width kr, edge samples repeated, running sums. */
    inline void BoxBlur(const std::vector<float>& src, const int w, const int h, const int kr, std::vector<float>& out)
    {
      const float n = static_cast<float>(2 * kr + 1);
      std::vector<float> tmp(src.size());
      out.assign(src.size(), 0.0f);
      for (int z = 0; z < h; ++z) {
        const std::size_t row = static_cast<std::size_t>(z) * static_cast<std::size_t>(w);
        float sum = 0.0f;
        for (int k = -kr; k <= kr; ++k) {
          sum = sum + src[row + static_cast<std::size_t>(std::min(std::max(k, 0), w - 1))];
        }
        for (int x = 0; x < w; ++x) {
          tmp[row + static_cast<std::size_t>(x)] = sum / n;
          sum = sum + src[row + static_cast<std::size_t>(std::min(x + kr + 1, w - 1))];
          sum = sum - src[row + static_cast<std::size_t>(std::max(x - kr, 0))];
        }
      }
      for (int x = 0; x < w; ++x) {
        float sum = 0.0f;
        for (int k = -kr; k <= kr; ++k) {
          sum = sum + tmp[static_cast<std::size_t>(std::min(std::max(k, 0), h - 1)) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
        }
        for (int z = 0; z < h; ++z) {
          out[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = sum / n;
          sum = sum + tmp[static_cast<std::size_t>(std::min(z + kr + 1, h - 1)) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
          sum = sum - tmp[static_cast<std::size_t>(std::max(z - kr, 0)) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
        }
      }
    }

    [[nodiscard]] inline std::uint16_t ToWord(const float value)
    {
      const float v = std::floor(value + 0.5f);
      if (v < 0.0f) {
        return 0;
      }
      if (v > 65535.0f) {
        return 65535;
      }
      return static_cast<std::uint16_t>(v);
    }
  } // namespace detail

  /** The window a dab reads and may write, clamped to the grid (empty when the brush is off the map). */
  [[nodiscard]] inline Window WindowOf(const Dab& dab, const int gridWidth, const int gridHeight)
  {
    const float lo_x = dab.cx - dab.radius;
    const float lo_z = dab.cz - dab.radius;
    const float hi_x = dab.cx + dab.radius;
    const float hi_z = dab.cz + dab.radius;
    Window w;
    w.x0 = std::max(0, static_cast<int>(std::floor(lo_x)) - 1);
    w.z0 = std::max(0, static_cast<int>(std::floor(lo_z)) - 1);
    w.x1 = std::min(gridWidth - 1, static_cast<int>(std::ceil(hi_x)) + 1);
    w.z1 = std::min(gridHeight - 1, static_cast<int>(std::ceil(hi_z)) + 1);
    return w;
  }

  /**
   * Applies a dab to the window `win` of a grid. `before` is the grid's content over the window (row major, x fastest); `after` receives the new
   * content (same size; samples the brush does not reach are copied). Returns the number of changed samples and the level used.
   */
  inline Result ApplyWindow(const Dab& dab, const Window& win, const std::uint16_t* const before, std::uint16_t* const after)
  {
    Result result;
    if (win.Empty()) {
      return result;
    }
    const int w = win.Width();
    const int h = win.Height();
    const std::size_t count = win.Count();
    std::memcpy(after, before, count * sizeof(std::uint16_t));

    const float r = dab.radius;
    if (!(r > 0.0f)) {
      return result;
    }
    const float dir = dab.dir < 0 ? -1.0f : 1.0f;
    const float strength = dab.strength;

    // flatten / plateau: the level is the ground under the dab centre unless the stroke already has one (world units; words / 128 is exact)
    float levelWord = 0.0f;
    if (dab.mode == Mode::Flatten || dab.mode == Mode::Plateau) {
      float level = dab.level;
      if (!(level >= 0.0f)) {
        const int cxi = std::min(std::max(static_cast<int>(std::floor(dab.cx + 0.5f)), win.x0), win.x1);
        const int czi = std::min(std::max(static_cast<int>(std::floor(dab.cz + 0.5f)), win.z0), win.z1);
        const float centre = static_cast<float>(before[static_cast<std::size_t>(czi - win.z0) * static_cast<std::size_t>(w) + static_cast<std::size_t>(cxi - win.x0)]);
        level = centre / static_cast<float>(detail::kWord);
      }
      result.level = level;
      result.usedLevel = true;
      levelWord = level * static_cast<float>(detail::kWord);
    }

    // smooth: the blur widens with the strength (3 x 3 at low strength, up to a quarter of the radius, at most 6 samples), two passes
    std::vector<float> blurred;
    if (dab.mode == Mode::Smooth) {
      std::vector<float> src(count);
      for (std::size_t i = 0; i < count; ++i) {
        src[i] = static_cast<float>(before[i]);
      }
      float s = std::fabs(strength) / 10.0f;
      s = std::min(1.0f, s);
      const float quarter = r * 0.25f;
      float room = std::min(quarter, 6.0f);
      room = room - 1.0f;
      room = std::max(0.0f, room);
      float spread = s * room;
      spread = spread + 0.5f;
      const int kr = 1 + static_cast<int>(std::floor(spread));
      std::vector<float> pass;
      detail::BoxBlur(src, w, h, kr, pass);
      detail::BoxBlur(pass, w, h, kr, blurred);
    }

    const detail::Profile profile(r, dab.falloff);
    const bool shaped = dab.shape != nullptr && dab.shape->n >= 2;
    const ShapeSampler sampler(shaped ? dab.shape : nullptr, dab.shapeAngle);
    const float word = static_cast<float>(detail::kWord);
    // squared distance beyond which the weight is surely 0 (a loose bound: the exact test is the float distance in Profile::At)
    const float farD2 = r * r * 1.001f + 1.0f;
    int changed = 0;
    for (int z = win.z0; z <= win.z1; ++z) {
      const float dz = static_cast<float>(z) - dab.cz;
      const float dz2 = dz * dz;
      const std::size_t row = static_cast<std::size_t>(z - win.z0) * static_cast<std::size_t>(w);
      for (int x = win.x0; x <= win.x1; ++x) {
        const float dx = static_cast<float>(x) - dab.cx;
        const float dx2 = dx * dx;
        const float d2 = dx2 + dz2;
        if (d2 > farD2) {
          continue;
        }
        const float distance = static_cast<float>(std::sqrt(static_cast<double>(d2)));
        // a shape mask replaces the falloff profile; the brush is still cut at the radius (terrain.lua Weight)
        const float weight = shaped ? (distance >= r ? 0.0f : sampler.At(dx, dz, r)) : profile.At(distance);
        if (!(weight > 0.0f)) {
          continue;
        }
        const std::size_t i = row + static_cast<std::size_t>(x - win.x0);
        const float old = static_cast<float>(before[i]);
        float value = old;
        switch (dab.mode) {
        case Mode::Raise: {
          float t = dir * strength;
          t = t * weight;
          t = t * word;
          value = old + t;
          break;
        }
        case Mode::Lower: {
          float t = dir * strength;
          t = t * weight;
          t = t * word;
          value = old - t;
          break;
        }
        case Mode::Plateau: {
          float s = strength * 0.5f;
          s = s * weight;
          float k = std::min(1.0f, s);
          k = k * dir;
          float pull = levelWord - old;
          pull = pull * k;
          value = old + pull;
          break;
        }
        case Mode::Flatten: {
          float s = strength * 0.2f;
          s = s * weight;
          float k = std::min(1.0f, s);
          k = k * dir;
          float pull = levelWord - old;
          pull = pull * k;
          value = old + pull;
          break;
        }
        case Mode::Smooth: {
          float s = strength * 0.2f;
          s = s * weight;
          float k = std::min(1.0f, s);
          k = k * dir;
          float pull = blurred[i] - old;
          pull = pull * k;
          value = old + pull;
          break;
        }
        case Mode::Noise: {
          float t = dir * detail::SmoothNoise(x, z, dab.seed, dab.noiseScale, dab.noiseOctaves, dab.noiseRoughness);
          t = t * strength;
          t = t * weight;
          t = t * word;
          value = old + t;
          break;
        }
        }
        const std::uint16_t out = detail::ToWord(value);
        if (out != before[i]) {
          after[i] = out;
          ++changed;
        }
      }
    }
    result.changed = changed;
    return result;
  }

  /**
   * The weights a shaped brush gives the samples x0..x1 / z0..z1 (row major, z outer) for a brush of radius r at (cx, cz): what terrain.lua Weight returns per sample (0 at and beyond the
   * radius, else the mask). The erosion brush takes such a list for its window (ed_erode_dab, opts.weights).
   */
  inline void FillShapeWeights(
    const Shape& shape, const float angleDegrees, const float cx, const float cz, const float r, const int x0, const int z0, const int x1, const int z1, std::vector<float>& out
  )
  {
    out.clear();
    if (x1 < x0 || z1 < z0 || !(r > 0.0f) || shape.n < 2) {
      return;
    }
    out.reserve(static_cast<std::size_t>(x1 - x0 + 1) * static_cast<std::size_t>(z1 - z0 + 1));
    const ShapeSampler sampler(&shape, angleDegrees);
    for (int z = z0; z <= z1; ++z) {
      const float dz = static_cast<float>(z) - cz;
      const float dz2 = dz * dz;
      for (int x = x0; x <= x1; ++x) {
        const float dx = static_cast<float>(x) - cx;
        const float dx2 = dx * dx;
        const float d2 = dx2 + dz2;
        const float distance = static_cast<float>(std::sqrt(static_cast<double>(d2)));
        out.push_back(distance >= r ? 0.0f : sampler.At(dx, dz, r));
      }
    }
  }

  /** A whole dab on a grid of width x height words: the window, the new window content and the result. Used by the tests. */
  inline Result ComputeDab(
    const std::uint16_t* const grid, const int gridWidth, const int gridHeight, const Dab& dab, Window& window, std::vector<std::uint16_t>& after
  )
  {
    window = WindowOf(dab, gridWidth, gridHeight);
    after.clear();
    if (window.Empty()) {
      return Result();
    }
    std::vector<std::uint16_t> before(window.Count());
    for (int z = window.z0; z <= window.z1; ++z) {
      std::memcpy(
        before.data() + static_cast<std::size_t>(z - window.z0) * static_cast<std::size_t>(window.Width()),
        grid + static_cast<std::size_t>(z) * static_cast<std::size_t>(gridWidth) + static_cast<std::size_t>(window.x0),
        static_cast<std::size_t>(window.Width()) * sizeof(std::uint16_t)
      );
    }
    after.resize(window.Count());
    return ApplyWindow(dab, window, before.data(), after.data());
  }
} // namespace moho::editor::brush
