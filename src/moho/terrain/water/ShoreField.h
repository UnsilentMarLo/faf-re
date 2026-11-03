#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// The distance to the shore (docs/LIGHTING_V2.md, "Shore waves"): for every height sample of the map the distance in world units to the nearest
// land sample. The shore waves of the water simulation run in it. Pure functions on plain arrays (no engine types), so that the headless lab of the
// water simulation (tools/watersimlab) computes exactly what the game does.
namespace moho::water
{
  // Exact euclidean distance transform of one line (Felzenszwalb and Huttenlocher): d[q] = min over p of f[p] + (q - p)^2.
  inline void ShoreDistanceLine(const std::vector<double>& f, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z)
  {
    const int n = static_cast<int>(f.size());
    int k = 0;
    v[0] = 0;
    z[0] = -1e30;
    z[1] = 1e30;
    for (int q = 1; q < n; ++q) {
      double s = 0.0;
      for (;;) {
        const int p = v[k];
        s = ((f[q] + static_cast<double>(q) * q) - (f[p] + static_cast<double>(p) * p)) / (2.0 * q - 2.0 * p);
        if (s <= z[k] && k > 0) {
          --k;
        } else {
          break;
        }
      }
      if (s <= z[k]) {
        v[k] = q; // k == 0: the new parabola is lower everywhere the old one was
        continue;
      }
      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
      while (z[k + 1] < q) {
        ++k;
      }
      const double dq = static_cast<double>(q - v[k]);
      d[q] = dq * dq + f[v[k]];
    }
  }

  // `rawAt(x, z)` is the raw height of a sample (any callable returning a number). Land is ground above `level` (raw height units). The result is
  // smoothed over thirteen samples in both directions (twice a box of 6.5 units: the contours of a distance field have cusps where the shores of a bay or two
  // islands meet, and crests that follow them show V shapes) so that neither those nor the staircase of a one-unit coast show in the contours the waves follow; 1e4
  // where the map has no land.
  template <class RawAt>
  std::vector<float> ComputeShoreDistance(RawAt&& rawAt, const std::uint32_t width, const std::uint32_t height, const float level)
  {
    const double kInf = 1e12;
    std::vector<double> grid(static_cast<std::size_t>(width) * height);
    for (std::uint32_t z = 0; z < height; ++z) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const float raw = static_cast<float>(rawAt(x, z));
        grid[static_cast<std::size_t>(z) * width + x] = raw > level ? 0.0 : kInf;
      }
    }
    const std::size_t longest = std::max(width, height);
    std::vector<double> f(longest), d(longest), z(longest + 1U);
    std::vector<int> v(longest);
    for (std::uint32_t x = 0; x < width; ++x) { // columns
      f.assign(height, 0.0);
      d.assign(height, 0.0);
      for (std::uint32_t j = 0; j < height; ++j) {
        f[j] = grid[static_cast<std::size_t>(j) * width + x];
      }
      ShoreDistanceLine(f, d, v, z);
      for (std::uint32_t j = 0; j < height; ++j) {
        grid[static_cast<std::size_t>(j) * width + x] = d[j];
      }
    }
    for (std::uint32_t j = 0; j < height; ++j) { // rows
      f.assign(width, 0.0);
      d.assign(width, 0.0);
      for (std::uint32_t x = 0; x < width; ++x) {
        f[x] = grid[static_cast<std::size_t>(j) * width + x];
      }
      ShoreDistanceLine(f, d, v, z);
      for (std::uint32_t x = 0; x < width; ++x) {
        grid[static_cast<std::size_t>(j) * width + x] = d[x];
      }
    }
    std::vector<float> dist(grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i) {
      dist[i] = static_cast<float>(std::min(std::sqrt(grid[i]), 1e4));
    }
    std::vector<float> tmp(dist.size());
    constexpr int kRadius = 6;
    for (int pass = 0; pass < 4; ++pass) {
      for (std::uint32_t j = 0; j < height; ++j) {
        for (std::uint32_t x = 0; x < width; ++x) {
          float sum = 0.0f;
          for (int o = -kRadius; o <= kRadius; ++o) {
            const std::uint32_t xx = (pass & 1) == 0 ? static_cast<std::uint32_t>(std::min(std::max(static_cast<int>(x) + o, 0), static_cast<int>(width) - 1)) : x;
            const std::uint32_t jj = (pass & 1) == 1 ? static_cast<std::uint32_t>(std::min(std::max(static_cast<int>(j) + o, 0), static_cast<int>(height) - 1)) : j;
            sum += dist[static_cast<std::size_t>(jj) * width + xx];
          }
          tmp[static_cast<std::size_t>(j) * width + x] = sum / static_cast<float>(2 * kRadius + 1);
        }
      }
      dist.swap(tmp);
    }
    return dist;
  }
} // namespace moho::water
