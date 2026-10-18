// EDITOR: Supreme Commander specific ops of the field engine (docs/projects/editor/TERRAIN_GEN_DESIGN.md, stage T6). Engine independent like EdFieldCore; registered through RegisterOp.
//
//   pad_flatten  a = heights. Base pads and resource rings: every pad is a flat square core (edges snapped outward to the build grid, so whole cells are level) with a smooth skirt.
//                `pads` = 6 numbers per pad: x, z, half (half size of the core, world units), skirt (width of the blend beyond the core), y (absolute target height, used when
//                group == -1), group. group 0: the pad is levelled to its own ground (mean | min | max of the core, `level` 0 | 1 | 2); group k >= 1: all pads of the group share ONE
//                height (the same statistic over all their cores: a mass cluster on one level); group -1: y. Pads apply in list order (a later pad wins where they overlap).
//                `grid` (default 1) is the build grid in world units, `snap` (default 1) 0 keeps the exact rectangle, `word` (default 128) quantises targets to 1 / word units (0 = off),
//                `strength` 0..1, `output` height | weight (the largest pad weight, 0..1) | core (1 inside any core).
//   symmetrize   a = any field. The master wedge of the map is copied onto the rest by the symmetry group: mode mirror_x | mirror_z | mirror_xz | diag | adiag | diag_both | rot2 | rot4 |
//                rotn (n = `n`, any N-fold turn). Centre cx, cz (world; default the middle of the field), `side` / `sector` picks the master wedge (0 .. group size - 1; for the mirror modes
//                side 0 is the left / upper half or quadrant like the `mirror` op), `angle` (degrees) turns the wedge start of rotn. `combine` copy (the master wedge, default) | avg |
//                max | min over all images; `nearest` 1 reads the closest sample instead of interpolating (codes such as terrain types); `seam` (world units) blends copy towards avg near the wedge borders (soft borders for turns). Every output is a function of the input and the
//                group alone, so the result is invariant under the group exactly when the centre lies on a lattice point (or half lattice) and the group is made of quarter turns / mirrors.
//   slope_limit  a = heights. Makes every neighbouring pair of samples differ by at most (limit - margin) * cell: the engine's pathing rule (the steepest neighbouring difference over a
//                footprint window, 0.75 blocks land units) then holds for every unit class. Exact in two sweeps: `lower` = the highest Lipschitz surface below the input (cuts peaks),
//                `raise` = the lowest above it (fills valleys), `both` blends the two with `balance` (0 raise .. 1 lower). `mask` (0..1) blends the result into the input, `strength` too.
//   reach        a = passable mask (>= 0.5 passable), optional b = cost multiplier (>= 1). Travel distance (world units, 8 neighbours without corner cutting) from the sources
//                `src` (x, z pairs, world; snapped to the nearest passable sample within `snap` samples, default 4). Not reachable: `unreach` (default -1).
//   regions      a = passable mask. Connected regions of passable samples (connectivity 4 | 8, default 8 without corner cutting): mode label (1..N in raster order, 0 blocked, default) |
//                size (area of the region in world units squared).
//
// Every output sample is computed from the inputs alone (the sweeps and the search are sequential and ordered), so the results are bit identical for any thread count.
#pragma fp_contract(off)

#include "moho/editor/EdFieldCore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>

namespace moho::editor::field
{
  namespace
  {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kChunk = 8;

    inline double F32(const double v) { return static_cast<double>(static_cast<float>(v)); }
    inline double Clamp01(const double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
    inline double SmoothStep(const double t) { const double c = Clamp01(t); return c * c * (3.0 - 2.0 * c); }

    const Grid* Get(const OpArgs& a, const char* const key)
    {
      const auto it = a.in.find(key);
      return it != a.in.end() ? it->second.get() : nullptr;
    }

    const Grid* Need(const OpArgs& a, const char* const key, std::string& err)
    {
      const Grid* g = Get(a, key);
      if (g == nullptr) {
        err = std::string("missing input '") + key + "'";
      }
      return g;
    }

    template <class F>
    void Rows(const int h, F&& f)
    {
      const int chunks = (h + kChunk - 1) / kChunk;
      ParallelFor(chunks, [&](const int c) {
        const int z0 = c * kChunk;
        f(z0, std::min(h, z0 + kChunk));
      });
    }

    inline double WX(const Grid& g, const int x) { return static_cast<double>(g.ox) + static_cast<double>(x) * static_cast<double>(g.cell); }
    inline double WZ(const Grid& g, const int z) { return static_cast<double>(g.oz) + static_cast<double>(z) * static_cast<double>(g.cell); }

    // ---- pad_flatten ----------------------------------------------------------------------------------------------------------------------------

    struct Pad
    {
      double x0, z0, x1, z1;   // the snapped core rectangle (world)
      double skirt;
      double target;
      double y;
      int group;
      double bx0, bz0, bx1, bz1;   // bounding box including the skirt
    };

    double Weight(const Pad& p, const double wx, const double wz)
    {
      const double dx = std::max(std::max(p.x0 - wx, wx - p.x1), 0.0);
      const double dz = std::max(std::max(p.z0 - wz, wz - p.z1), 0.0);
      if (dx == 0.0 && dz == 0.0) return 1.0;
      if (p.skirt <= 0.0) return 0.0;
      const double d = std::sqrt(dx * dx + dz * dz);
      if (d >= p.skirt) return 0.0;
      return 1.0 - SmoothStep(d / p.skirt);
    }

    bool OpPadFlatten(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr) return false;
      const std::vector<double>* v = a.p.A("pads");
      if (v == nullptr || v->size() % 6 != 0) {
        err = "'pads' needs 6 numbers per pad (x, z, half, skirt, y, group)";
        return false;
      }
      const std::string output = a.p.S("output", "height");
      if (output != "height" && output != "weight" && output != "core") {
        err = "output must be height, weight or core";
        return false;
      }
      const double grid = std::max(1e-6, a.p.N("grid", 1.0));
      const bool snap = a.p.N("snap", 1.0) != 0.0;
      const double word = a.p.N("word", 128.0);
      const int level = static_cast<int>(a.p.N("level", 0.0));
      const double strength = Clamp01(a.p.N("strength", 1.0));
      std::vector<Pad> pads(v->size() / 6);
      for (std::size_t i = 0; i < pads.size(); ++i) {
        const double* q = v->data() + i * 6;
        Pad& p = pads[i];
        const double x = F32(q[0]), z = F32(q[1]);
        const double half = std::max(0.0, F32(q[2]));
        p.skirt = std::max(0.0, F32(q[3]));
        p.y = F32(q[4]);
        p.group = static_cast<int>(std::lround(q[5]));
        p.x0 = x - half;
        p.x1 = x + half;
        p.z0 = z - half;
        p.z1 = z + half;
        if (snap) {
          p.x0 = std::floor(p.x0 / grid + 1e-9) * grid;
          p.z0 = std::floor(p.z0 / grid + 1e-9) * grid;
          p.x1 = std::ceil(p.x1 / grid - 1e-9) * grid;
          p.z1 = std::ceil(p.z1 / grid - 1e-9) * grid;
          if (p.x1 <= p.x0) p.x1 = p.x0 + grid;
          if (p.z1 <= p.z0) p.z1 = p.z0 + grid;
        }
        p.bx0 = p.x0 - p.skirt;
        p.bx1 = p.x1 + p.skirt;
        p.bz0 = p.z0 - p.skirt;
        p.bz1 = p.z1 + p.skirt;
        p.target = 0.0;
      }
      // targets: statistic of the core samples of the INPUT, one value per pad or per group (sequential, fixed order)
      struct Acc { double sum = 0.0, lo = 1e30, hi = -1e30; long long n = 0; };
      const auto Core = [&](const Pad& p, Acc& acc) {
        const int ix0 = std::max(0, static_cast<int>(std::ceil((p.x0 - src->ox) / src->cell - 1e-9)));
        const int ix1 = std::min(src->w - 1, static_cast<int>(std::floor((p.x1 - src->ox) / src->cell + 1e-9)));
        const int iz0 = std::max(0, static_cast<int>(std::ceil((p.z0 - src->oz) / src->cell - 1e-9)));
        const int iz1 = std::min(src->h - 1, static_cast<int>(std::floor((p.z1 - src->oz) / src->cell + 1e-9)));
        for (int z = iz0; z <= iz1; ++z) {
          for (int x = ix0; x <= ix1; ++x) {
            const double h = src->At(x, z);
            acc.sum += h;
            acc.lo = std::min(acc.lo, h);
            acc.hi = std::max(acc.hi, h);
            ++acc.n;
          }
        }
      };
      const auto Value = [&](const Acc& acc, const Pad& p) {
        if (acc.n == 0) return static_cast<double>(SampleWorld(*src, 0.5 * (p.x0 + p.x1), 0.5 * (p.z0 + p.z1)));
        return level == 1 ? acc.lo : (level == 2 ? acc.hi : acc.sum / static_cast<double>(acc.n));
      };
      std::map<int, Acc> groups;
      for (const Pad& p : pads) {
        if (p.group >= 1) Core(p, groups[p.group]);
      }
      for (Pad& p : pads) {
        if (p.group == -1) {
          p.target = p.y;
        } else if (p.group >= 1) {
          p.target = Value(groups[p.group], p);
        } else {
          Acc acc;
          Core(p, acc);
          p.target = Value(acc, p);
        }
        if (word > 0.0) p.target = std::floor(p.target * word + 0.5) / word;
        p.target = F32(p.target);
      }
      out = *src;
      const int w = src->w;
      const int h = src->h;
      Rows(h, [&](const int z0, const int z1) {
        for (int z = z0; z < z1; ++z) {
          const double wz = WZ(*src, z);
          for (int x = 0; x < w; ++x) {
            const double wx = WX(*src, x);
            double height = src->At(x, z);
            double wmax = 0.0;
            bool core = false;
            for (const Pad& p : pads) {
              if (wx < p.bx0 || wx > p.bx1 || wz < p.bz0 || wz > p.bz1) continue;
              const double wgt = Weight(p, wx, wz);
              if (wgt <= 0.0) continue;
              wmax = std::max(wmax, wgt);
              if (wgt >= 1.0) core = true;
              const double s = wgt * strength;
              height = (s >= 1.0) ? p.target : height + (p.target - height) * s;
            }
            float r;
            if (output == "weight") r = static_cast<float>(wmax * strength);
            else if (output == "core") r = core ? 1.0f : 0.0f;
            else r = static_cast<float>(height);
            out.At(x, z) = r;
          }
        }
      });
      return true;
    }

    // ---- symmetrize -----------------------------------------------------------------------------------------------------------------------------

    struct Mat
    {
      double a, b, c, d;   // (u, v) -> (a u + b v, c u + d v)
    };

    inline double Snap(const double v)
    {
      if (std::fabs(v) < 1e-9) return 0.0;
      if (std::fabs(v - 1.0) < 1e-9) return 1.0;
      if (std::fabs(v + 1.0) < 1e-9) return -1.0;
      return v;
    }

    Mat Rot(const double ang)
    {
      return {Snap(std::cos(ang)), Snap(-std::sin(ang)), Snap(std::sin(ang)), Snap(std::cos(ang))};
    }

    /** Reflection across the line through the origin at angle `ang`. */
    Mat Refl(const double ang)
    {
      return {Snap(std::cos(2.0 * ang)), Snap(std::sin(2.0 * ang)), Snap(std::sin(2.0 * ang)), Snap(-std::cos(2.0 * ang))};
    }

    struct Group
    {
      std::vector<Mat> m;
      double a0 = 0.0;      // start angle of wedge 0
      double width = 0.0;   // wedge width
    };

    bool BuildGroup(const std::string& mode, const OpArgs& a, Group& g, std::string& err)
    {
      int n = 1;
      bool mirrored = false;
      double axis = 0.0;   // for dihedral groups: the angle of a mirror line = start of wedge 0
      double a0 = 0.0;
      if (mode == "mirror_x") { n = 1; mirrored = true; axis = kPi / 2.0; }
      else if (mode == "mirror_z") { n = 1; mirrored = true; axis = kPi; }
      else if (mode == "mirror_xz") { n = 2; mirrored = true; axis = kPi; }
      else if (mode == "diag") { n = 1; mirrored = true; axis = kPi / 4.0; }
      else if (mode == "adiag") { n = 1; mirrored = true; axis = 3.0 * kPi / 4.0; }
      else if (mode == "diag_both") { n = 2; mirrored = true; axis = kPi / 4.0; }
      else if (mode == "rot2") { n = 2; a0 = kPi; }
      else if (mode == "rot4") { n = 4; a0 = kPi; }
      else if (mode == "rotn") {
        n = static_cast<int>(std::lround(a.p.N("n", 3.0)));
        if (n < 2 || n > 16) { err = "symmetrize: rotn needs 2 <= n <= 16"; return false; }
        a0 = a.p.N("angle", 0.0) * kPi / 180.0;
      } else {
        err = "symmetrize: unknown mode '" + mode + "'";
        return false;
      }
      for (int k = 0; k < n; ++k) {
        g.m.push_back(Rot(2.0 * kPi * k / n));
      }
      if (mirrored) {
        // mirror lines at axis + k * pi / n
        for (int k = 0; k < n; ++k) {
          g.m.push_back(Refl(axis + kPi * k / n));
        }
        g.a0 = axis;
        g.width = kPi / n;
      } else {
        g.a0 = a0;
        g.width = 2.0 * kPi / n;
      }
      return true;
    }

    float Bilinear(const Grid& g, const double sx, const double sz, const bool nearest = false)
    {
      if (nearest) {
        const int ix = static_cast<int>(std::max(0.0, std::min(static_cast<double>(g.w - 1), std::floor(sx + 0.5))));
        const int iz = static_cast<int>(std::max(0.0, std::min(static_cast<double>(g.h - 1), std::floor(sz + 0.5))));
        return g.At(ix, iz);
      }
      const double cx = std::max(0.0, std::min(static_cast<double>(g.w - 1), sx));
      const double cz = std::max(0.0, std::min(static_cast<double>(g.h - 1), sz));
      const int x0 = static_cast<int>(std::floor(cx));
      const int z0 = static_cast<int>(std::floor(cz));
      const int x1 = std::min(g.w - 1, x0 + 1);
      const int z1 = std::min(g.h - 1, z0 + 1);
      const double fx = cx - x0, fz = cz - z0;
      if (fx == 0.0 && fz == 0.0) return g.At(x0, z0);
      const double top = g.At(x0, z0) * (1.0 - fx) + g.At(x1, z0) * fx;
      const double bot = g.At(x0, z1) * (1.0 - fx) + g.At(x1, z1) * fx;
      return static_cast<float>(top * (1.0 - fz) + bot * fz);
    }

    bool OpSymmetrize(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr) return false;
      Group grp;
      const std::string mode = a.p.S("mode", "mirror_x");
      if (!BuildGroup(mode, a, grp, err)) return false;
      const std::string combine = a.p.S("combine", "copy");
      if (combine != "copy" && combine != "avg" && combine != "max" && combine != "min") {
        err = "symmetrize: combine must be copy, avg, max or min";
        return false;
      }
      const int count = static_cast<int>(grp.m.size());
      int wedge = static_cast<int>(std::lround(a.p.Has("sector") ? a.p.N("sector", 0.0) : a.p.N("side", 0.0)));
      wedge = ((wedge % count) + count) % count;
      const double cx = a.p.Has("cx") ? F32(a.p.N("cx", 0.0)) : static_cast<double>(src->ox) + 0.5 * (src->w - 1) * static_cast<double>(src->cell);
      const double cz = a.p.Has("cz") ? F32(a.p.N("cz", 0.0)) : static_cast<double>(src->oz) + 0.5 * (src->h - 1) * static_cast<double>(src->cell);
      const double seam = std::max(0.0, a.p.N("seam", 0.0));
      const bool nearest = a.p.N("nearest", 0.0) != 0.0;
      const double start = grp.a0 + wedge * grp.width;
      out = *src;
      const double inv = 1.0 / static_cast<double>(src->cell);
      Rows(src->h, [&](const int z0, const int z1) {
        for (int z = z0; z < z1; ++z) {
          for (int x = 0; x < src->w; ++x) {
            const double u = WX(*src, x) - cx;
            const double v = WZ(*src, z) - cz;
            const double r = std::sqrt(u * u + v * v);
            // angle inside the wedge system: phi in [0, 2 pi) from the start of wedge 0
            double phi = std::atan2(v, u) - grp.a0;
            phi = std::fmod(phi, 2.0 * kPi);
            if (phi < 0.0) phi += 2.0 * kPi;
            if (r == 0.0) phi = 0.0;
            // the exact axes: phi = k * width up to rounding belongs to the wedge that starts there
            const double kw = phi / grp.width;
            int k = static_cast<int>(std::floor(kw + 1e-9));
            const double within = (kw - k) * grp.width;   // angle from the start of wedge k
            const bool master = (k == wedge) || (count > 1 && std::fabs(kw - std::round(kw)) < 1e-9 && static_cast<int>(std::lround(kw)) == wedge);
            float result;
            const auto Image = [&](const Mat& m) {
              const double qu = m.a * u + m.b * v;
              const double qv = m.c * u + m.d * v;
              return Bilinear(*src, (cx + qu - src->ox) * inv, (cz + qv - src->oz) * inv, nearest);
            };
            if (combine == "copy") {
              if (master) {
                result = src->At(x, z);
              } else {
                // the image of p inside the master wedge. A point on the closing border of the wedge has an image on the opening border (turns); where it has none (mirrors) the
                // candidates on the closing border are all that exist and the one with the smallest (z, x) is taken, so every point of an orbit picks the same one
                float best = src->At(x, z);
                bool found = false;
                bool haveEnd = false;
                double endV = 0.0, endU = 0.0;
                for (const Mat& m : grp.m) {
                  const double qu = m.a * u + m.b * v;
                  const double qv = m.c * u + m.d * v;
                  const double qr = std::sqrt(qu * qu + qv * qv);
                  double ph = std::atan2(qv, qu) - grp.a0;
                  ph = std::fmod(ph, 2.0 * kPi);
                  if (ph < 0.0) ph += 2.0 * kPi;
                  if (qr == 0.0) ph = 0.0;
                  const double kq = ph / grp.width;
                  const int kk = static_cast<int>(std::floor(kq + 1e-9));
                  if (kk == wedge || (std::fabs(kq - std::round(kq)) < 1e-9 && static_cast<int>(std::lround(kq)) == wedge)) {
                    best = Image(m);
                    found = true;
                    break;
                  }
                  if (std::fabs(kq - std::round(kq)) < 1e-9 && (static_cast<int>(std::lround(kq)) - 1 - wedge) % count == 0) {
                    const double ru = std::round(qu * 1e6) / 1e6, rv = std::round(qv * 1e6) / 1e6;
                    if (!haveEnd || rv < endV || (rv == endV && ru < endU)) {
                      haveEnd = true;
                      endV = rv;
                      endU = ru;
                      best = Image(m);
                    }
                  }
                }
                result = (found || haveEnd) ? best : src->At(x, z);
              }
              if (seam > 0.0 && r > 0.0) {
                // distance to the nearest wedge border, blended towards the average of all images
                const double dphi = std::min(within, grp.width - within);
                const double dist = dphi >= kPi / 2.0 ? r : r * std::sin(dphi);
                if (dist < seam) {
                  double sum = 0.0;
                  for (const Mat& m : grp.m) sum += Image(m);
                  const double avg = sum / count;
                  const double t = SmoothStep(dist / seam);
                  result = static_cast<float>(avg + (static_cast<double>(result) - avg) * t);
                }
              }
            } else {
              double acc = combine == "avg" ? 0.0 : static_cast<double>(Image(grp.m[0]));
              for (int i = 0; i < count; ++i) {
                const double val = Image(grp.m[static_cast<std::size_t>(i)]);
                if (combine == "avg") acc += val;
                else if (combine == "max") acc = std::max(acc, val);
                else acc = std::min(acc, val);
              }
              result = static_cast<float>(combine == "avg" ? acc / count : acc);
            }
            out.At(x, z) = result;
          }
        }
      });
      return true;
    }

    // ---- slope_limit ----------------------------------------------------------------------------------------------------------------------------

    bool OpSlopeLimit(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr) return false;
      const std::string mode = a.p.S("mode", "both");
      if (mode != "lower" && mode != "raise" && mode != "both") {
        err = "slope_limit: mode must be lower, raise or both";
        return false;
      }
      const double limit = a.p.N("limit", 0.75);
      const double margin = std::max(0.0, a.p.N("margin", 0.03));
      const double balance = Clamp01(a.p.N("balance", 0.5));
      const double strength = Clamp01(a.p.N("strength", 1.0));
      const Grid* mask = Get(a, "mask");
      if (mask != nullptr && !mask->SameShape(*src)) {
        err = "'mask' must have the shape of 'a'";
        return false;
      }
      const double L = std::max(0.0, (limit - margin)) * static_cast<double>(src->cell);
      const int w = src->w;
      const int h = src->h;
      const std::size_t n = src->Count();
      const auto Envelope = [&](const bool lower) {
        std::vector<double> g(n);
        for (std::size_t i = 0; i < n; ++i) g[i] = src->v[i];
        const double sgn = lower ? 1.0 : -1.0;   // raise = lower envelope of the negated field, negated back
        if (!lower) for (std::size_t i = 0; i < n; ++i) g[i] = -g[i];
        (void)sgn;
        for (int z = 0; z < h; ++z) {
          for (int x = 0; x < w; ++x) {
            double& c = g[static_cast<std::size_t>(z) * w + x];
            if (x > 0) c = std::min(c, g[static_cast<std::size_t>(z) * w + x - 1] + L);
            if (z > 0) c = std::min(c, g[static_cast<std::size_t>(z - 1) * w + x] + L);
          }
        }
        for (int z = h - 1; z >= 0; --z) {
          for (int x = w - 1; x >= 0; --x) {
            double& c = g[static_cast<std::size_t>(z) * w + x];
            if (x < w - 1) c = std::min(c, g[static_cast<std::size_t>(z) * w + x + 1] + L);
            if (z < h - 1) c = std::min(c, g[static_cast<std::size_t>(z + 1) * w + x] + L);
          }
        }
        if (!lower) for (std::size_t i = 0; i < n; ++i) g[i] = -g[i];
        return g;
      };
      std::vector<double> lo, hi;
      if (mode != "raise") lo = Envelope(true);
      if (mode != "lower") hi = Envelope(false);
      out = *src;
      for (std::size_t i = 0; i < n; ++i) {
        double r;
        if (mode == "lower") r = lo[i];
        else if (mode == "raise") r = hi[i];
        else r = hi[i] + (lo[i] - hi[i]) * balance;
        double s = strength;
        if (mask != nullptr) s *= Clamp01(mask->v[i]);
        out.v[i] = static_cast<float>(static_cast<double>(src->v[i]) + (r - static_cast<double>(src->v[i])) * s);
      }
      return true;
    }

    // ---- reach and regions ----------------------------------------------------------------------------------------------------------------------

    inline bool Pass(const Grid& m, const int x, const int z) { return m.At(x, z) >= 0.5f; }

    bool OpReach(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* pass = Need(a, "a", err);
      if (pass == nullptr) return false;
      const Grid* cost = Get(a, "b");
      if (cost != nullptr && !cost->SameShape(*pass)) {
        err = "reach: 'b' must have the shape of 'a'";
        return false;
      }
      const std::vector<double>* src = a.p.A("src");
      if (src == nullptr || src->empty() || src->size() % 2 != 0) {
        err = "reach: 'src' needs x, z pairs";
        return false;
      }
      const int w = pass->w;
      const int h = pass->h;
      const float unreach = static_cast<float>(a.p.N("unreach", -1.0));
      const int snapR = std::max(0, static_cast<int>(a.p.N("snap", 4.0)));
      const double cell = pass->cell;
      out = Grid(w, h, pass->cell, pass->ox, pass->oz, unreach);
      std::vector<double> dist(pass->Count(), std::numeric_limits<double>::infinity());
      using Item = std::pair<double, std::size_t>;
      std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
      for (std::size_t i = 0; i + 1 < src->size(); i += 2) {
        const double fx = ((*src)[i] - pass->ox) / cell;
        const double fz = ((*src)[i + 1] - pass->oz) / cell;
        const int cx = static_cast<int>(std::lround(fx));
        const int cz = static_cast<int>(std::lround(fz));
        int bx = -1, bz = -1;
        double bestD = 1e30;
        for (int dz = -snapR; dz <= snapR; ++dz) {
          for (int dx = -snapR; dx <= snapR; ++dx) {
            const int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= w || z >= h || !Pass(*pass, x, z)) continue;
            const double d = (x - fx) * (x - fx) + (z - fz) * (z - fz);
            if (d < bestD) { bestD = d; bx = x; bz = z; }
          }
        }
        if (bx >= 0) {
          const std::size_t id = static_cast<std::size_t>(bz) * w + bx;
          if (dist[id] > 0.0) { dist[id] = 0.0; pq.push({0.0, id}); }
        }
      }
      static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1};
      static const int dzs[8] = {0, 0, 1, -1, 1, -1, 1, -1};
      while (!pq.empty()) {
        const Item it = pq.top();
        pq.pop();
        if (it.first > dist[it.second]) continue;
        const int x = static_cast<int>(it.second % static_cast<std::size_t>(w));
        const int z = static_cast<int>(it.second / static_cast<std::size_t>(w));
        for (int k = 0; k < 8; ++k) {
          const int nx = x + dxs[k], nz = z + dzs[k];
          if (nx < 0 || nz < 0 || nx >= w || nz >= h || !Pass(*pass, nx, nz)) continue;
          if (k >= 4 && (!Pass(*pass, x + dxs[k], z) || !Pass(*pass, x, z + dzs[k]))) continue;   // no corner cutting
          double step = (k < 4 ? 1.0 : 1.4142135623730951) * cell;
          const std::size_t nid = static_cast<std::size_t>(nz) * w + nx;
          if (cost != nullptr) step *= 0.5 * (std::max(1.0, static_cast<double>(cost->v[it.second])) + std::max(1.0, static_cast<double>(cost->v[nid])));
          const double nd = it.first + step;
          if (nd < dist[nid]) { dist[nid] = nd; pq.push({nd, nid}); }
        }
      }
      for (std::size_t i = 0; i < dist.size(); ++i) {
        if (dist[i] != std::numeric_limits<double>::infinity()) out.v[i] = static_cast<float>(dist[i]);
      }
      return true;
    }

    bool OpRegions(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* pass = Need(a, "a", err);
      if (pass == nullptr) return false;
      const std::string mode = a.p.S("mode", "label");
      if (mode != "label" && mode != "size") {
        err = "regions: mode must be label or size";
        return false;
      }
      const int conn = a.p.N("connectivity", 8.0) == 4.0 ? 4 : 8;
      const int w = pass->w;
      const int h = pass->h;
      std::vector<int> label(pass->Count(), 0);
      std::vector<std::size_t> sizes(1, 0);
      std::vector<std::size_t> stack;
      int next = 0;
      static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1};
      static const int dzs[8] = {0, 0, 1, -1, 1, -1, 1, -1};
      for (int z = 0; z < h; ++z) {
        for (int x = 0; x < w; ++x) {
          const std::size_t id = static_cast<std::size_t>(z) * w + x;
          if (label[id] != 0 || !Pass(*pass, x, z)) continue;
          ++next;
          sizes.push_back(0);
          label[id] = next;
          stack.assign(1, id);
          while (!stack.empty()) {
            const std::size_t cur = stack.back();
            stack.pop_back();
            ++sizes[static_cast<std::size_t>(next)];
            const int cx = static_cast<int>(cur % static_cast<std::size_t>(w));
            const int cz = static_cast<int>(cur / static_cast<std::size_t>(w));
            for (int k = 0; k < conn; ++k) {
              const int nx = cx + dxs[k], nz = cz + dzs[k];
              if (nx < 0 || nz < 0 || nx >= w || nz >= h || !Pass(*pass, nx, nz)) continue;
              if (k >= 4 && (!Pass(*pass, cx + dxs[k], cz) || !Pass(*pass, cx, cz + dzs[k]))) continue;
              const std::size_t nid = static_cast<std::size_t>(nz) * w + nx;
              if (label[nid] != 0) continue;
              label[nid] = next;
              stack.push_back(nid);
            }
          }
        }
      }
      out = Grid(w, h, pass->cell, pass->ox, pass->oz, 0.0f);
      const double area = static_cast<double>(pass->cell) * static_cast<double>(pass->cell);
      for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] == 0) continue;
        out.v[i] = mode == "label" ? static_cast<float>(label[i]) : static_cast<float>(static_cast<double>(sizes[static_cast<std::size_t>(label[i])]) * area);
      }
      return true;
    }

    /** Registers the ops with the field engine at start-up (EdFieldCore.cpp: RegisterOp). */
    struct ScOpRegistrar
    {
      ScOpRegistrar()
      {
        RegisterOp("pad_flatten", &OpPadFlatten);
        RegisterOp("symmetrize", &OpSymmetrize);
        RegisterOp("slope_limit", &OpSlopeLimit);
        RegisterOp("reach", &OpReach);
        RegisterOp("regions", &OpRegions);
      }
    };
    const ScOpRegistrar gScOpRegistrar{};
  } // namespace
} // namespace moho::editor::field
