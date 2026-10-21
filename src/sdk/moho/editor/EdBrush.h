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
// Units: heights are words, 128 per world unit; a dab at (cx, cz) in sample coordinates with a radius in samples; words are clamped to 0..65535.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace moho::editor::brush
{
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
     * terrain.lua Noise(): a hash of (x, z, stroke) in -1..1. Every intermediate stays below 2^24, so it is exact in the game's 32-bit float Lua numbers
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
        const float weight = profile.At(distance);
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
          float t = dir * detail::Noise(x, z, dab.seed);
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
