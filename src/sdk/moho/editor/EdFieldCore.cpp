// EDITOR: the field engine core, see EdFieldCore.h.
#pragma fp_contract(off)

#include "moho/editor/EdFieldCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace moho::editor::field
{
  // ---- params ----------------------------------------------------------------------------------------------------------------------------------

  double Params::N(const char* const key, const double fallback) const
  {
    const auto it = num.find(key);
    // Lua numbers are float32 in the editor: round every number the same way here so a parameter means the same in a Lua call and in the native test
    return it != num.end() ? static_cast<double>(static_cast<float>(it->second)) : fallback;
  }

  std::string Params::S(const char* const key, const char* const fallback) const
  {
    const auto it = str.find(key);
    return it != str.end() ? it->second : std::string(fallback);
  }

  const std::vector<double>* Params::A(const char* const key) const
  {
    const auto it = arr.find(key);
    return it != arr.end() ? &it->second : nullptr;
  }

  // ---- thread pool -----------------------------------------------------------------------------------------------------------------------------

  namespace
  {
    struct Job
    {
      const std::function<void(int)>* fn = nullptr;
      int count = 0;
      std::atomic<int> next{0};
      std::atomic<int> done{0};
    };

    class Pool
    {
    public:
      Pool() { Resize(DefaultThreads()); }

      static int DefaultThreads()
      {
        const unsigned hc = std::thread::hardware_concurrency();
        return static_cast<int>(std::max(1u, std::min(hc == 0 ? 4u : hc, 16u)));
      }

      int Threads() const { return threads_; }

      void Resize(int n)
      {
        n = std::max(1, std::min(n, 64));
        Stop();
        threads_ = n;
        stop_ = false;
        for (int i = 1; i < n; ++i) {
          workers_.emplace_back([this] { WorkerLoop(); });
        }
      }

      void For(const int count, const std::function<void(int)>& fn)
      {
        if (count <= 0) {
          return;
        }
        if (threads_ <= 1 || count == 1) {
          for (int i = 0; i < count; ++i) {
            fn(i);
          }
          return;
        }
        auto job = std::make_shared<Job>();
        job->fn = &fn;
        job->count = count;
        {
          std::lock_guard<std::mutex> lock(m_);
          job_ = job;
          ++gen_;
        }
        cvWork_.notify_all();
        Run(*job);
        std::unique_lock<std::mutex> lock(m_);
        cvDone_.wait(lock, [&] { return job->done.load() >= count; });
        job_.reset();
      }

    private:
      void Run(Job& job)
      {
        for (;;) {
          const int i = job.next.fetch_add(1);
          if (i >= job.count) {
            break;
          }
          (*job.fn)(i);
          if (job.done.fetch_add(1) + 1 == job.count) {
            std::lock_guard<std::mutex> lock(m_);
            cvDone_.notify_all();
          }
        }
      }

      void WorkerLoop()
      {
        std::uint64_t seen = 0;
        for (;;) {
          std::shared_ptr<Job> job;
          {
            std::unique_lock<std::mutex> lock(m_);
            cvWork_.wait(lock, [&] { return stop_ || gen_ != seen; });
            if (stop_) {
              return;
            }
            seen = gen_;
            job = job_;
          }
          if (job) {
            Run(*job);
          }
        }
      }

      void Stop()
      {
        {
          std::lock_guard<std::mutex> lock(m_);
          stop_ = true;
        }
        cvWork_.notify_all();
        for (std::thread& t : workers_) {
          t.join();
        }
        workers_.clear();
      }

      std::mutex m_;
      std::condition_variable cvWork_;
      std::condition_variable cvDone_;
      std::shared_ptr<Job> job_;
      std::uint64_t gen_ = 0;
      bool stop_ = false;
      int threads_ = 1;
      std::vector<std::thread> workers_;
    };

    Pool& ThePool()
    {
      static Pool* const pool = new Pool();   // never destroyed: the process ends with its workers asleep
      return *pool;
    }
  } // namespace

  void SetThreads(const int n) { ThePool().Resize(n); }
  int Threads() { return ThePool().Threads(); }
  void ParallelFor(const int count, const std::function<void(int)>& fn) { ThePool().For(count, fn); }

  namespace
  {
    constexpr float kPi = 3.14159265358979f;
    constexpr int kRowChunk = 8;

    /** f(z0, z1) for fixed chunks of rows. */
    template <class F>
    void Rows(const int h, F&& f)
    {
      const int chunks = (h + kRowChunk - 1) / kRowChunk;
      ParallelFor(chunks, [&](const int c) {
        const int z0 = c * kRowChunk;
        f(z0, std::min(h, z0 + kRowChunk));
      });
    }

    /** g(x, z) for every sample of out. */
    template <class F>
    void Cells(Grid& out, F&& f)
    {
      const int w = out.w;
      Rows(out.h, [&](const int z0, const int z1) {
        for (int z = z0; z < z1; ++z) {
          float* row = out.v.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(w);
          for (int x = 0; x < w; ++x) {
            row[x] = f(x, z);
          }
        }
      });
    }

    inline float Clamp01(const float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    inline float Lerp(const float a, const float b, const float t) { return a + (b - a) * t; }

    inline float SmoothStep(const float e0, const float e1, const float v)
    {
      if (e0 == e1) {
        return v < e0 ? 0.0f : 1.0f;
      }
      const float t = Clamp01((v - e0) / (e1 - e0));
      return t * t * (3.0f - 2.0f * t);
    }

    // ---- input access --------------------------------------------------------------------------------------------------------------------------

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

    /** The output takes its shape from `like`, else `a`, else width / height / cell / ox / oz. */
    bool MakeOut(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* ref = Get(a, "like");
      if (ref == nullptr) {
        ref = Get(a, "a");
      }
      // the ops index their inputs with the output's size: two inputs of different shapes would read or write out of range
      if (const Grid* const inA = Get(a, "a"); inA != nullptr && ref != nullptr && (inA->w != ref->w || inA->h != ref->h)) {
        err = "'like' and 'a' differ in size";
        return false;
      }
      if (ref != nullptr) {
        out = Grid(ref->w, ref->h, ref->cell, ref->ox, ref->oz);
        return true;
      }
      const int w = static_cast<int>(a.p.N("width", 0));
      const int h = static_cast<int>(a.p.N("height", 0));
      if (w < 1 || h < 1 || static_cast<long long>(w) * h > (1LL << 24)) {
        err = "give 'like' or 'a', or width and height (at most 16M samples)";
        return false;
      }
      out = Grid(w, h, static_cast<float>(a.p.N("cell", 1.0)), static_cast<float>(a.p.N("ox", 0.0)), static_cast<float>(a.p.N("oz", 0.0)));
      return true;
    }

    inline double WorldX(const Grid& g, const int x) { return static_cast<double>(g.ox) + static_cast<double>(x) * static_cast<double>(g.cell); }
    inline double WorldZ(const Grid& g, const int z) { return static_cast<double>(g.oz) + static_cast<double>(z) * static_cast<double>(g.cell); }

    // ---- edge modes and sampling ---------------------------------------------------------------------------------------------------------------

    enum Edge { kClamp = 0, kWrap = 1, kMirror = 2, kFill = 3 };

    Edge ParseEdge(const OpArgs& a, const Edge fallback)
    {
      const std::string s = a.p.S("edge", "");
      if (s == "clamp") return kClamp;
      if (s == "wrap") return kWrap;
      if (s == "mirror") return kMirror;
      if (s == "fill") return kFill;
      return fallback;
    }

    inline int Wrap(int i, const int n)
    {
      i %= n;
      return i < 0 ? i + n : i;
    }

    inline int Mirror(int i, const int n)
    {
      if (n == 1) {
        return 0;
      }
      const int period = 2 * (n - 1);
      i = Wrap(i, period);
      return i < n ? i : period - i;
    }

    inline float Fetch(const Grid& g, int ix, int iz, const Edge e, const float fill)
    {
      if (e == kFill) {
        if (ix < 0 || iz < 0 || ix >= g.w || iz >= g.h) {
          return fill;
        }
      } else if (e == kWrap) {
        ix = Wrap(ix, g.w);
        iz = Wrap(iz, g.h);
      } else if (e == kMirror) {
        ix = Mirror(ix, g.w);
        iz = Mirror(iz, g.h);
      } else {
        ix = std::max(0, std::min(g.w - 1, ix));
        iz = std::max(0, std::min(g.h - 1, iz));
      }
      return g.At(ix, iz);
    }

    /** Bilinear sample at fractional sample coordinates. */
    inline float Bilinear(const Grid& g, const double gx, const double gz, const Edge e, const float fill)
    {
      const double fx = std::floor(gx);
      const double fz = std::floor(gz);
      const float tx = static_cast<float>(gx - fx);
      const float tz = static_cast<float>(gz - fz);
      const int ix = static_cast<int>(fx);
      const int iz = static_cast<int>(fz);
      const float a = Fetch(g, ix, iz, e, fill);
      const float b = Fetch(g, ix + 1, iz, e, fill);
      const float c = Fetch(g, ix, iz + 1, e, fill);
      const float d = Fetch(g, ix + 1, iz + 1, e, fill);
      const float top = a + (b - a) * tx;
      const float bottom = c + (d - c) * tx;
      return top + (bottom - top) * tz;
    }

    // ---- noise ---------------------------------------------------------------------------------------------------------------------------------

    inline std::uint32_t Hash32(std::uint32_t x)
    {
      x ^= x >> 16;
      x *= 0x7feb352dU;
      x ^= x >> 15;
      x *= 0x846ca68bU;
      x ^= x >> 16;
      return x;
    }

    inline std::uint32_t Hash2(const int ix, const int iz, const std::uint32_t seed)
    {
      return Hash32(static_cast<std::uint32_t>(ix) * 0x9E3779B1U ^ Hash32(static_cast<std::uint32_t>(iz) + seed * 0x85EBCA6BU + 0x165667B1U));
    }

    inline float Unit(const std::uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }   // [0, 1)

    const float kGrad[8][2] = {{1.0f, 0.0f},         {-1.0f, 0.0f},          {0.0f, 1.0f},           {0.0f, -1.0f},
                               {0.70710678f, 0.70710678f}, {-0.70710678f, 0.70710678f}, {0.70710678f, -0.70710678f}, {-0.70710678f, -0.70710678f}};

    inline float GradDot(const std::uint32_t h, const float x, const float z)
    {
      const float* g = kGrad[h & 7u];
      return g[0] * x + g[1] * z;
    }

    inline float Fade(const float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

    /** Gradient noise, about -1..1. */
    float Perlin(const double x, const double z, const std::uint32_t seed)
    {
      const double fx0 = std::floor(x);
      const double fz0 = std::floor(z);
      const int ix = static_cast<int>(fx0);
      const int iz = static_cast<int>(fz0);
      const float fx = static_cast<float>(x - fx0);
      const float fz = static_cast<float>(z - fz0);
      const float u = Fade(fx);
      const float v = Fade(fz);
      const float g00 = GradDot(Hash2(ix, iz, seed), fx, fz);
      const float g10 = GradDot(Hash2(ix + 1, iz, seed), fx - 1.0f, fz);
      const float g01 = GradDot(Hash2(ix, iz + 1, seed), fx, fz - 1.0f);
      const float g11 = GradDot(Hash2(ix + 1, iz + 1, seed), fx - 1.0f, fz - 1.0f);
      const float a = g00 + (g10 - g00) * u;
      const float b = g01 + (g11 - g01) * u;
      return (a + (b - a) * v) * 1.41421356f;
    }

    constexpr float kSimplexScale = 99.2f;   // calibrated so the extremes of the 8 direction gradient set reach about +-1

    /** 2D simplex noise, about -1..1. */
    float Simplex(const double x, const double z, const std::uint32_t seed)
    {
      const double F2 = 0.36602540378443865;
      const double G2 = 0.21132486540518713;
      const double s = (x + z) * F2;
      const double fi = std::floor(x + s);
      const double fj = std::floor(z + s);
      const int i = static_cast<int>(fi);
      const int j = static_cast<int>(fj);
      const double t = (fi + fj) * G2;
      const float x0 = static_cast<float>(x - (fi - t));
      const float y0 = static_cast<float>(z - (fj - t));
      const int i1 = x0 > y0 ? 1 : 0;
      const int j1 = x0 > y0 ? 0 : 1;
      const float g2 = static_cast<float>(G2);
      const float x1 = x0 - static_cast<float>(i1) + g2;
      const float y1 = y0 - static_cast<float>(j1) + g2;
      const float x2 = x0 - 1.0f + 2.0f * g2;
      const float y2 = y0 - 1.0f + 2.0f * g2;
      float n = 0.0f;
      float t0 = 0.5f - x0 * x0 - y0 * y0;
      if (t0 > 0.0f) {
        t0 *= t0;
        n += t0 * t0 * GradDot(Hash2(i, j, seed), x0, y0);
      }
      float t1 = 0.5f - x1 * x1 - y1 * y1;
      if (t1 > 0.0f) {
        t1 *= t1;
        n += t1 * t1 * GradDot(Hash2(i + i1, j + j1, seed), x1, y1);
      }
      float t2 = 0.5f - x2 * x2 - y2 * y2;
      if (t2 > 0.0f) {
        t2 *= t2;
        n += t2 * t2 * GradDot(Hash2(i + 1, j + 1, seed), x2, y2);
      }
      return n * kSimplexScale;
    }

    struct NoiseParams
    {
      std::uint32_t seed = 1;
      double scale = 100.0;
      int octaves = 5;
      float persistence = 0.5f;
      float lacunarity = 2.0f;
      int basis = 0;                 // 0 perlin, 1 simplex
      double shiftX = 0.0, shiftZ = 0.0;
    };

    NoiseParams ReadNoise(const OpArgs& a, const char* const defaultBasis)
    {
      NoiseParams n;
      n.seed = static_cast<std::uint32_t>(static_cast<std::int64_t>(a.p.N("seed", 1)));
      n.scale = std::max(1e-3, a.p.N("scale", 100.0));
      n.octaves = std::max(1, std::min(16, static_cast<int>(a.p.N("octaves", 5))));
      n.persistence = static_cast<float>(a.p.N("persistence", 0.5));
      n.lacunarity = static_cast<float>(a.p.N("lacunarity", 2.0));
      n.basis = (a.p.S("basis", defaultBasis) == "simplex") ? 1 : 0;
      n.shiftX = a.p.N("shiftx", 0.0);
      n.shiftZ = a.p.N("shiftz", 0.0);
      return n;
    }

    /** kind 0 fbm (rolling), 1 ridged, 2 billow. Result 0..1. */
    float Fbm(const int kind, const NoiseParams& n, const double wx, const double wz)
    {
      float sum = 0.0f;
      float norm = 0.0f;
      float amp = 1.0f;
      double freq = 1.0 / n.scale;
      for (int o = 0; o < n.octaves; ++o) {
        const std::uint32_t seed = Hash32(n.seed * 131U + static_cast<std::uint32_t>(o) * 7919U + 17U);
        const double px = (wx + n.shiftX) * freq;
        const double pz = (wz + n.shiftZ) * freq;
        float s = n.basis == 1 ? Simplex(px, pz, seed) : Perlin(px, pz, seed);
        s = std::max(-1.0f, std::min(1.0f, s));
        float u;
        if (kind == 1) {
          u = 1.0f - std::fabs(s);
          u *= u;
        } else if (kind == 2) {
          u = std::fabs(s);
        } else {
          u = 0.5f * s + 0.5f;
        }
        sum += u * amp;
        norm += amp;
        amp *= n.persistence;
        freq *= static_cast<double>(n.lacunarity);
      }
      return Clamp01(sum / norm);
    }

    bool OpNoise(const OpArgs& a, Grid& out, std::string& err, const int kind, const char* const defaultBasis)
    {
      if (!MakeOut(a, out, err)) {
        return false;
      }
      const NoiseParams n = ReadNoise(a, defaultBasis);
      const Grid& g = out;
      Cells(out, [&](const int x, const int z) { return Fbm(kind, n, WorldX(g, x), WorldZ(g, z)); });
      return true;
    }

    bool OpPerlin(const OpArgs& a, Grid& o, std::string& e) { return OpNoise(a, o, e, 0, "perlin"); }
    bool OpSimplex(const OpArgs& a, Grid& o, std::string& e) { return OpNoise(a, o, e, 0, "simplex"); }
    bool OpRidged(const OpArgs& a, Grid& o, std::string& e) { return OpNoise(a, o, e, 1, "perlin"); }
    bool OpBillow(const OpArgs& a, Grid& o, std::string& e) { return OpNoise(a, o, e, 2, "perlin"); }

    bool OpConst(const OpArgs& a, Grid& out, std::string& err)
    {
      if (!MakeOut(a, out, err)) {
        return false;
      }
      std::fill(out.v.begin(), out.v.end(), static_cast<float>(a.p.N("value", 0.0)));
      return true;
    }

    bool OpWhite(const OpArgs& a, Grid& out, std::string& err)
    {
      if (!MakeOut(a, out, err)) {
        return false;
      }
      const std::uint32_t seed = static_cast<std::uint32_t>(static_cast<std::int64_t>(a.p.N("seed", 1)));
      const Grid& g = out;
      Cells(out, [&](const int x, const int z) {
        return Unit(Hash2(static_cast<int>(std::floor(WorldX(g, x))), static_cast<int>(std::floor(WorldZ(g, z))), seed));
      });
      return true;
    }

    /** Voronoi / cellular: mode f1, f2, edge, id, crack. */
    bool OpVoronoi(const OpArgs& a, Grid& out, std::string& err)
    {
      if (!MakeOut(a, out, err)) {
        return false;
      }
      const double scale = std::max(1e-3, a.p.N("scale", 100.0));
      const float jitter = Clamp01(static_cast<float>(a.p.N("jitter", 1.0)));
      const std::uint32_t seed = static_cast<std::uint32_t>(static_cast<std::int64_t>(a.p.N("seed", 1)));
      const std::string mode = a.p.S("mode", "f1");
      const float width = std::max(1e-4f, static_cast<float>(a.p.N("thickness", 0.1)));
      const Grid& g = out;
      Cells(out, [&](const int x, const int z) {
        const double px = WorldX(g, x) / scale;
        const double pz = WorldZ(g, z) / scale;
        const double fx0 = std::floor(px);
        const double fz0 = std::floor(pz);
        const int cx = static_cast<int>(fx0);
        const int cz = static_cast<int>(fz0);
        const float lx = static_cast<float>(px - fx0);
        const float lz = static_cast<float>(pz - fz0);
        float d1 = 1e9f;
        float d2 = 1e9f;
        std::uint32_t id = 0;
        for (int dz = -1; dz <= 1; ++dz) {
          for (int dx = -1; dx <= 1; ++dx) {
            const std::uint32_t h = Hash2(cx + dx, cz + dz, seed);
            const float jx = 0.5f + (Unit(h) - 0.5f) * jitter;
            const float jz = 0.5f + (Unit(Hash32(h ^ 0x68E31DA4U)) - 0.5f) * jitter;
            const float ex = static_cast<float>(dx) + jx - lx;
            const float ez = static_cast<float>(dz) + jz - lz;
            const float d = ex * ex + ez * ez;
            if (d < d1) {
              d2 = d1;
              d1 = d;
              id = h;
            } else if (d < d2) {
              d2 = d;
            }
          }
        }
        const float f1 = std::sqrt(d1);
        const float f2 = std::sqrt(d2);
        if (mode == "f2") return Clamp01(f2);
        if (mode == "edge") return Clamp01(f2 - f1);
        if (mode == "id") return Unit(Hash32(id ^ 0xA511E9B3U));
        if (mode == "crack") return 1.0f - SmoothStep(0.0f, width, f2 - f1);
        return Clamp01(f1);
      });
      return true;
    }

    /** Linear gradient: angle in degrees (0 = towards +x), lo at the start, hi at the end, spanning the field. */
    bool OpGradient(const OpArgs& a, Grid& out, std::string& err)
    {
      if (!MakeOut(a, out, err)) {
        return false;
      }
      const double ang = a.p.N("angle", 0.0) * 3.14159265358979 / 180.0;
      const double dx = std::cos(ang);
      const double dz = std::sin(ang);
      const Grid& g = out;
      const double cxw = WorldX(g, 0) + 0.5 * (g.w - 1) * g.cell;
      const double czw = WorldZ(g, 0) + 0.5 * (g.h - 1) * g.cell;
      const double extent = std::max(1e-6, (g.w - 1) * g.cell * std::fabs(dx) + (g.h - 1) * g.cell * std::fabs(dz));
      const float lo = static_cast<float>(a.p.N("lo", 0.0));
      const float hi = static_cast<float>(a.p.N("hi", 1.0));
      Cells(out, [&](const int x, const int z) {
        const double t = ((WorldX(g, x) - cxw) * dx + (WorldZ(g, z) - czw) * dz) / extent + 0.5;
        return Lerp(lo, hi, Clamp01(static_cast<float>(t)));
      });
      return true;
    }

    /** Radial distance from a centre (default the middle) in units of `radius` (default half the smaller extent), 0 at the centre, clamped to 0..1 unless clamp = 0. */
    bool OpRadial(const OpArgs& a, Grid& out, std::string& err)
    {
      if (!MakeOut(a, out, err)) {
        return false;
      }
      const Grid& g = out;
      const double cxw = a.p.Has("cx") ? a.p.N("cx", 0) : WorldX(g, 0) + 0.5 * (g.w - 1) * g.cell;
      const double czw = a.p.Has("cz") ? a.p.N("cz", 0) : WorldZ(g, 0) + 0.5 * (g.h - 1) * g.cell;
      const double radius = std::max(1e-6, a.p.Has("radius") ? a.p.N("radius", 1) : 0.5 * std::min(g.w - 1, g.h - 1) * g.cell);
      const bool clamp = a.p.N("clamp", 1) != 0;
      Cells(out, [&](const int x, const int z) {
        const double ex = WorldX(g, x) - cxw;
        const double ez = WorldZ(g, z) - czw;
        const float d = static_cast<float>(std::sqrt(ex * ex + ez * ez) / radius);
        return clamp ? Clamp01(d) : d;
      });
      return true;
    }

    // ---- point filters ---------------------------------------------------------------------------------------------------------------------------

    template <class F>
    bool Point(const OpArgs& a, Grid& out, std::string& err, F&& f)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      Cells(out, [&](const int x, const int z) { return f(src->At(x, z)); });
      return true;
    }

    bool OpClamp(const OpArgs& a, Grid& o, std::string& e)
    {
      const float lo = static_cast<float>(a.p.N("lo", 0.0));
      const float hi = static_cast<float>(a.p.N("hi", 1.0));
      return Point(a, o, e, [&](const float v) { return v < lo ? lo : (v > hi ? hi : v); });
    }

    bool OpInvert(const OpArgs& a, Grid& o, std::string& e)
    {
      const float lo = static_cast<float>(a.p.N("lo", 0.0));
      const float hi = static_cast<float>(a.p.N("hi", 1.0));
      return Point(a, o, e, [&](const float v) { return lo + hi - v; });
    }

    bool OpAbs(const OpArgs& a, Grid& o, std::string& e)
    {
      return Point(a, o, e, [&](const float v) { return std::fabs(v); });
    }

    bool OpGamma(const OpArgs& a, Grid& o, std::string& e)
    {
      const float g = std::max(1e-3f, static_cast<float>(a.p.N("gamma", 1.0)));
      return Point(a, o, e, [&](const float v) { return v <= 0.0f ? 0.0f : std::pow(v, g); });
    }

    /** v * mul + add */
    bool OpAffine(const OpArgs& a, Grid& o, std::string& e)
    {
      const float m = static_cast<float>(a.p.N("mul", 1.0));
      const float b = static_cast<float>(a.p.N("add", 0.0));
      return Point(a, o, e, [&](const float v) { return v * m + b; });
    }

    bool OpSmoothstep(const OpArgs& a, Grid& o, std::string& e)
    {
      const float e0 = static_cast<float>(a.p.N("edge0", 0.0));
      const float e1 = static_cast<float>(a.p.N("edge1", 1.0));
      return Point(a, o, e, [&](const float v) { return SmoothStep(e0, e1, v); });
    }

    bool OpThreshold(const OpArgs& a, Grid& o, std::string& e)
    {
      const float t = static_cast<float>(a.p.N("value", 0.5));
      return Point(a, o, e, [&](const float v) { return v > t ? 1.0f : 0.0f; });
    }

    bool OpNormalize(const OpArgs& a, Grid& o, std::string& e)
    {
      const Grid* src = Need(a, "a", e);
      if (src == nullptr) {
        return false;
      }
      const Stats s = Measure(*src);
      const float lo = static_cast<float>(a.p.N("lo", 0.0));
      const float hi = static_cast<float>(a.p.N("hi", 1.0));
      const float span = s.max - s.min;
      return Point(a, o, e, [&](const float v) { return span > 0.0f ? lo + (hi - lo) * ((v - s.min) / span) : lo; });
    }

    /** Rounds to a multiple of `step` (default 1/128: the height word grid). */
    bool OpQuantize(const OpArgs& a, Grid& o, std::string& e)
    {
      const float step = std::max(1e-6f, static_cast<float>(a.p.N("step", 1.0 / 128.0)));
      return Point(a, o, e, [&](const float v) { return std::floor(v / step + 0.5f) * step; });
    }

    // ---- curves ----------------------------------------------------------------------------------------------------------------------------------

    struct Curve
    {
      std::vector<float> x, y, m;   // points sorted by x, tangents for the smooth mode
      bool smooth = false;

      bool Build(const std::vector<double>& flat, const bool smoothMode, std::string& err)
      {
        const std::size_t n = flat.size() / 2;
        if (n < 2) {
          err = "a curve needs at least two points";
          return false;
        }
        std::vector<std::pair<float, float>> pts;
        for (std::size_t i = 0; i < n; ++i) {
          pts.emplace_back(static_cast<float>(flat[2 * i]), static_cast<float>(flat[2 * i + 1]));
        }
        std::stable_sort(pts.begin(), pts.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
        for (const auto& p : pts) {
          x.push_back(p.first);
          y.push_back(p.second);
        }
        smooth = smoothMode;
        m.assign(n, 0.0f);
        if (smooth) {
          // Fritsch-Carlson monotone cubic tangents
          std::vector<float> d(n - 1);
          for (std::size_t i = 0; i + 1 < n; ++i) {
            const float dx = x[i + 1] - x[i];
            d[i] = dx > 0.0f ? (y[i + 1] - y[i]) / dx : 0.0f;
          }
          m[0] = d[0];
          m[n - 1] = d[n - 2];
          for (std::size_t i = 1; i + 1 < n; ++i) {
            m[i] = (d[i - 1] * d[i] <= 0.0f) ? 0.0f : 0.5f * (d[i - 1] + d[i]);
          }
          for (std::size_t i = 0; i + 1 < n; ++i) {
            if (d[i] == 0.0f) {
              m[i] = 0.0f;
              m[i + 1] = 0.0f;
            } else {
              const float al = m[i] / d[i];
              const float be = m[i + 1] / d[i];
              const float s = al * al + be * be;
              if (s > 9.0f) {
                const float tau = 3.0f / std::sqrt(s);
                m[i] = tau * al * d[i];
                m[i + 1] = tau * be * d[i];
              }
            }
          }
        }
        return true;
      }

      float Eval(const float v) const
      {
        const std::size_t n = x.size();
        if (v <= x[0]) return y[0];
        if (v >= x[n - 1]) return y[n - 1];
        std::size_t i = 0;
        while (i + 2 < n && v > x[i + 1]) {
          ++i;
        }
        const float dx = x[i + 1] - x[i];
        if (dx <= 0.0f) return y[i + 1];
        const float t = (v - x[i]) / dx;
        if (!smooth) {
          return y[i] + (y[i + 1] - y[i]) * t;
        }
        const float t2 = t * t;
        const float t3 = t2 * t;
        return (2.0f * t3 - 3.0f * t2 + 1.0f) * y[i] + (t3 - 2.0f * t2 + t) * dx * m[i] + (-2.0f * t3 + 3.0f * t2) * y[i + 1] + (t3 - t2) * dx * m[i + 1];
      }
    };

    /** points = {x1, y1, x2, y2, ...}; smooth = 1 for a monotone cubic through them, else straight segments. */
    bool OpRemap(const OpArgs& a, Grid& o, std::string& e)
    {
      const std::vector<double>* pts = a.p.A("points");
      if (pts == nullptr) {
        e = "remap needs points";
        return false;
      }
      Curve c;
      if (!c.Build(*pts, a.p.N("smooth", 1) != 0, e)) {
        return false;
      }
      return Point(a, o, e, [&](const float v) { return c.Eval(v); });
    }

    bool OpLevels(const OpArgs& a, Grid& o, std::string& e)
    {
      const float inLo = static_cast<float>(a.p.N("in_lo", 0.0));
      const float inHi = static_cast<float>(a.p.N("in_hi", 1.0));
      const float gamma = std::max(1e-3f, static_cast<float>(a.p.N("gamma", 1.0)));
      const float outLo = static_cast<float>(a.p.N("out_lo", 0.0));
      const float outHi = static_cast<float>(a.p.N("out_hi", 1.0));
      const float span = inHi - inLo;
      return Point(a, o, e, [&](const float v) {
        float t = span != 0.0f ? Clamp01((v - inLo) / span) : (v >= inLo ? 1.0f : 0.0f);
        if (gamma != 1.0f && t > 0.0f) {
          t = std::pow(t, 1.0f / gamma);
        }
        return outLo + (outHi - outLo) * t;
      });
    }

    /** `steps` plateaus over 0..1 (or over lo..hi); sharpness 0 = smooth ramps, 1 = hard steps. */
    bool OpTerrace(const OpArgs& a, Grid& o, std::string& e)
    {
      const float n = std::max(1.0f, static_cast<float>(a.p.N("steps", 6)));
      const float lo = static_cast<float>(a.p.N("lo", 0.0));
      const float hi = static_cast<float>(a.p.N("hi", 1.0));
      const float sharp = Clamp01(static_cast<float>(a.p.N("sharpness", 0.7)));
      const float width = 1.0f - sharp;
      const float span = hi - lo;
      return Point(a, o, e, [&](const float v) {
        if (span == 0.0f) return v;
        const float t = (v - lo) / span * n;
        const float i = std::floor(t);
        const float f = t - i;
        const float shape = width <= 1e-6f ? (f < 0.5f ? 0.0f : 1.0f) : SmoothStep(0.5f - 0.5f * width, 0.5f + 0.5f * width, f);
        return lo + (i + shape) / n * span;
      });
    }

    // ---- blur ------------------------------------------------------------------------------------------------------------------------------------

    void ConvolveH(const Grid& src, Grid& dst, const std::vector<float>& k, const int r)
    {
      const int w = src.w;
      Rows(src.h, [&](const int z0, const int z1) {
        for (int z = z0; z < z1; ++z) {
          const float* in = src.v.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(w);
          float* outRow = dst.v.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(w);
          for (int x = 0; x < w; ++x) {
            float sum = k[0] * in[x];
            if (x >= r && x + r < w) {
              for (int i = 1; i <= r; ++i) {
                sum += k[i] * (in[x - i] + in[x + i]);
              }
            } else {
              for (int i = 1; i <= r; ++i) {
                sum += k[i] * (in[std::max(0, x - i)] + in[std::min(w - 1, x + i)]);
              }
            }
            outRow[x] = sum;
          }
        }
      });
    }

    void ConvolveV(const Grid& src, Grid& dst, const std::vector<float>& k, const int r)
    {
      const int w = src.w;
      const int h = src.h;
      Rows(h, [&](const int z0, const int z1) {
        std::vector<float> acc(static_cast<std::size_t>(w));
        for (int z = z0; z < z1; ++z) {
          const float* mid = src.v.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(w);
          for (int x = 0; x < w; ++x) {
            acc[static_cast<std::size_t>(x)] = k[0] * mid[x];
          }
          for (int i = 1; i <= r; ++i) {
            const float* up = src.v.data() + static_cast<std::size_t>(std::max(0, z - i)) * static_cast<std::size_t>(w);
            const float* dn = src.v.data() + static_cast<std::size_t>(std::min(h - 1, z + i)) * static_cast<std::size_t>(w);
            const float ki = k[i];
            for (int x = 0; x < w; ++x) {
              acc[static_cast<std::size_t>(x)] += ki * (up[x] + dn[x]);
            }
          }
          float* outRow = dst.v.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(w);
          for (int x = 0; x < w; ++x) {
            outRow[x] = acc[static_cast<std::size_t>(x)];
          }
        }
      });
    }

    /** One-sided kernel k[0..r] of a symmetric normalised kernel; gauss (sigma in samples) or box. */
    std::vector<float> MakeKernel(const bool gauss, const double sigmaOrRadius, int& r)
    {
      std::vector<float> k;
      if (gauss) {
        r = std::max(1, static_cast<int>(std::ceil(3.0 * sigmaOrRadius)));
        std::vector<double> d(static_cast<std::size_t>(r) + 1);
        double sum = 0.0;
        for (int i = 0; i <= r; ++i) {
          d[static_cast<std::size_t>(i)] = std::exp(-0.5 * static_cast<double>(i) * static_cast<double>(i) / (sigmaOrRadius * sigmaOrRadius));
          sum += (i == 0 ? 1.0 : 2.0) * d[static_cast<std::size_t>(i)];
        }
        for (double v : d) {
          k.push_back(static_cast<float>(v / sum));
        }
      } else {
        r = std::max(1, static_cast<int>(std::floor(sigmaOrRadius + 0.5)));
        k.assign(static_cast<std::size_t>(r) + 1, static_cast<float>(1.0 / (2.0 * r + 1.0)));
      }
      return k;
    }

    bool OpBlurImpl(const OpArgs& a, Grid& out, std::string& err, const bool gauss)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const double radius = a.p.N("radius", 4.0) / static_cast<double>(src->cell);   // world units -> samples
      if (radius <= 1e-6) {
        out.v = src->v;
        return true;
      }
      int r = 0;
      const std::vector<float> k = MakeKernel(gauss, radius, r);
      Grid tmp(src->w, src->h, src->cell, src->ox, src->oz);
      ConvolveH(*src, tmp, k, r);
      ConvolveV(tmp, out, k, r);
      if (const Grid* mask = Get(a, "mask"); mask != nullptr && mask->SameShape(*src)) {
        Cells(out, [&](const int x, const int z) { return Lerp(src->At(x, z), out.At(x, z), Clamp01(mask->At(x, z))); });
      }
      return true;
    }

    /** radius = sigma in world units. */
    bool OpBlur(const OpArgs& a, Grid& o, std::string& e) { return OpBlurImpl(a, o, e, true); }
    /** radius = half width in world units. */
    bool OpBox(const OpArgs& a, Grid& o, std::string& e) { return OpBlurImpl(a, o, e, false); }

    // ---- geometry --------------------------------------------------------------------------------------------------------------------------------

    /** Displaces lookups of `a` by the fields dx / dz (0..1, 0.5 = no move) times `strength` world units. */
    bool OpWarp(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      const Grid* dx = Need(a, "dx", err);
      const Grid* dz = Need(a, "dz", err);
      if (src == nullptr || dx == nullptr || dz == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      if (!dx->SameShape(*src) || !dz->SameShape(*src)) {
        err = "warp: dx and dz must have the shape of a";
        return false;
      }
      const double strength = a.p.N("strength", 20.0) / static_cast<double>(src->cell);   // samples
      const bool centred = a.p.N("centred", 1) != 0;
      const Edge e = ParseEdge(a, kClamp);
      Cells(out, [&](const int x, const int z) {
        const float ux = dx->At(x, z);
        const float uz = dz->At(x, z);
        const double sx = (centred ? 2.0 * static_cast<double>(ux) - 1.0 : static_cast<double>(ux)) * strength;
        const double sz = (centred ? 2.0 * static_cast<double>(uz) - 1.0 : static_cast<double>(uz)) * strength;
        return Bilinear(*src, static_cast<double>(x) + sx, static_cast<double>(z) + sz, e, 0.0f);
      });
      return true;
    }

    /** Moves the content by (move_x, move_z) world units; default wraps around. */
    bool OpOffset(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const double sx = a.p.N("move_x", 0.0) / static_cast<double>(src->cell);
      const double sz = a.p.N("move_z", 0.0) / static_cast<double>(src->cell);
      const Edge e = ParseEdge(a, kWrap);
      const float fill = static_cast<float>(a.p.N("fill", 0.0));
      const bool whole = sx == std::floor(sx) && sz == std::floor(sz);
      Cells(out, [&](const int x, const int z) {
        if (whole) {
          return Fetch(*src, x - static_cast<int>(sx), z - static_cast<int>(sz), e, fill);
        }
        return Bilinear(*src, static_cast<double>(x) - sx, static_cast<double>(z) - sz, e, fill);
      });
      return true;
    }

    /** Rotates by `angle` degrees (counter-clockwise seen with +z down) about (cx, cz) (default the middle). */
    bool OpRotate(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const double ang = a.p.N("angle", 0.0) * 3.14159265358979 / 180.0;
      const double c = std::cos(ang);
      const double s = std::sin(ang);
      const double cxs = a.p.Has("cx") ? (a.p.N("cx", 0) - src->ox) / src->cell : 0.5 * (src->w - 1);
      const double czs = a.p.Has("cz") ? (a.p.N("cz", 0) - src->oz) / src->cell : 0.5 * (src->h - 1);
      const Edge e = ParseEdge(a, kClamp);
      const float fill = static_cast<float>(a.p.N("fill", 0.0));
      Cells(out, [&](const int x, const int z) {
        const double ex = static_cast<double>(x) - cxs;
        const double ez = static_cast<double>(z) - czs;
        return Bilinear(*src, cxs + c * ex + s * ez, czs - s * ex + c * ez, e, fill);
      });
      return true;
    }

    /** Zooms by `factor` (2 = twice as big) about (cx, cz) (default the middle). */
    bool OpScale(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const double f = std::max(1e-4, a.p.N("factor", 1.0));
      const double cxs = a.p.Has("cx") ? (a.p.N("cx", 0) - src->ox) / src->cell : 0.5 * (src->w - 1);
      const double czs = a.p.Has("cz") ? (a.p.N("cz", 0) - src->oz) / src->cell : 0.5 * (src->h - 1);
      const Edge e = ParseEdge(a, kClamp);
      const float fill = static_cast<float>(a.p.N("fill", 0.0));
      Cells(out, [&](const int x, const int z) {
        return Bilinear(*src, cxs + (static_cast<double>(x) - cxs) / f, czs + (static_cast<double>(z) - czs) / f, e, fill);
      });
      return true;
    }

    /**
     * Symmetry copies (exact sample copies): mode x (left | right), z, xz (four quadrants), rot2 (180 degrees), rot4 (90 degrees, square), diag, adiag (square).
     * side 0 keeps the first half / quadrant as the master, side 1 the other.
     */
    bool OpMirror(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const std::string mode = a.p.S("mode", "x");
      const int side = a.p.N("side", 0) != 0 ? 1 : 0;
      const int w = src->w;
      const int h = src->h;
      if ((mode == "rot4" || mode == "diag" || mode == "adiag") && w != h) {
        err = "mirror " + mode + " needs a square field";
        return false;
      }
      if (mode != "x" && mode != "z" && mode != "xz" && mode != "rot2" && mode != "rot4" && mode != "diag" && mode != "adiag") {
        err = "mirror: unknown mode '" + mode + "'";
        return false;
      }
      Cells(out, [&](const int x, const int z) {
        int sx = x;
        int sz = z;
        if (mode == "x" || mode == "xz") {
          const int mx = w - 1 - x;
          if (side == 0 ? x > mx : x < mx) sx = mx;
        }
        if (mode == "z" || mode == "xz") {
          const int mz = h - 1 - z;
          if (side == 0 ? z > mz : z < mz) sz = mz;
        }
        if (mode == "rot2") {
          const int mx = w - 1 - x;
          const int mz = h - 1 - z;
          const bool master = z < mz || (z == mz && x <= mx);
          if (side == 0 ? !master : (master && !(z == mz && x == mx))) {
            sx = mx;
            sz = mz;
          }
        }
        if (mode == "rot4") {
          // doubled integer coordinates about the centre: U = 2x - (n-1), V = 2z - (n-1); domains D0 = {U>0, V>=0} and its three quarter turns
          const int n1 = w - 1;
          int U = 2 * x - n1;
          int V = 2 * z - n1;
          int turns = 0;
          const auto inD0 = [](const int u, const int v) { return u > 0 && v >= 0; };
          while (turns < 4 && !inD0(U, V)) {
            const int nu = V;           // quarter turn back: (u, v) -> (v, -u)
            const int nv = -U;
            U = nu;
            V = nv;
            ++turns;
          }
          if (turns < 4) {
            sx = (U + n1) / 2;
            sz = (V + n1) / 2;
          }
          if (side != 0) {
            // the master domain is the one after one more turn
            const int nu = -V;
            const int nv = U;
            sx = (nu + n1) / 2;
            sz = (nv + n1) / 2;
          }
        }
        if (mode == "diag") {
          if (side == 0 ? x > z : x < z) {
            sx = z;
            sz = x;
          }
        }
        if (mode == "adiag") {
          const int n1 = w - 1;
          if (side == 0 ? x + z > n1 : x + z < n1) {
            sx = n1 - z;
            sz = n1 - x;
          }
        }
        return src->At(sx, sz);
      });
      return true;
    }

    /** Sub rectangle x0..x1 / z0..z1 (sample indices, inclusive). */
    bool OpCrop(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr) {
        return false;
      }
      int x0 = static_cast<int>(a.p.N("x0", 0));
      int z0 = static_cast<int>(a.p.N("z0", 0));
      int x1 = static_cast<int>(a.p.N("x1", src->w - 1));
      int z1 = static_cast<int>(a.p.N("z1", src->h - 1));
      x0 = std::max(0, x0);
      z0 = std::max(0, z0);
      x1 = std::min(src->w - 1, x1);
      z1 = std::min(src->h - 1, z1);
      if (x0 > x1 || z0 > z1) {
        err = "crop: empty rectangle";
        return false;
      }
      out = Grid(x1 - x0 + 1, z1 - z0 + 1, src->cell, static_cast<float>(WorldX(*src, x0)), static_cast<float>(WorldZ(*src, z0)));
      Cells(out, [&](const int x, const int z) { return src->At(x + x0, z + z0); });
      return true;
    }

    /** New sample count over the same world extent (first and last samples map onto first and last), bilinear. */
    bool OpResize(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr) {
        return false;
      }
      const int w = static_cast<int>(a.p.N("width", src->w));
      const int h = static_cast<int>(a.p.N("height", src->h));
      if (w < 2 || h < 2 || static_cast<long long>(w) * h > (1LL << 24)) {
        err = "resize: bad size";
        return false;
      }
      const float cell = src->w > 1 ? src->cell * static_cast<float>(src->w - 1) / static_cast<float>(w - 1) : src->cell;
      out = Grid(w, h, cell, src->ox, src->oz);
      Cells(out, [&](const int x, const int z) {
        return Bilinear(*src, static_cast<double>(x) * (src->w - 1) / (w - 1), static_cast<double>(z) * (src->h - 1) / (h - 1), kClamp, 0.0f);
      });
      return true;
    }

    // ---- combiners -------------------------------------------------------------------------------------------------------------------------------

    template <class F>
    bool Binary(const OpArgs& a, Grid& out, std::string& err, F&& f)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const Grid* b = Get(a, "b");
      if (b != nullptr && !b->SameShape(*src)) {
        err = "the two inputs differ in size";
        return false;
      }
      const float value = static_cast<float>(a.p.N("value", 0.0));
      Cells(out, [&](const int x, const int z) { return f(src->At(x, z), b != nullptr ? b->At(x, z) : value); });
      return true;
    }

    bool OpMin(const OpArgs& a, Grid& o, std::string& e) { return Binary(a, o, e, [](const float p, const float q) { return p < q ? p : q; }); }
    bool OpMax(const OpArgs& a, Grid& o, std::string& e) { return Binary(a, o, e, [](const float p, const float q) { return p > q ? p : q; }); }
    bool OpAdd(const OpArgs& a, Grid& o, std::string& e) { return Binary(a, o, e, [](const float p, const float q) { return p + q; }); }
    bool OpSub(const OpArgs& a, Grid& o, std::string& e) { return Binary(a, o, e, [](const float p, const float q) { return p - q; }); }
    bool OpMul(const OpArgs& a, Grid& o, std::string& e) { return Binary(a, o, e, [](const float p, const float q) { return p * q; }); }

    /** lerp(a, f(a, b), opacity * mask): mode normal, add, sub, mul, screen, overlay, min, max, diff. */
    bool OpBlend(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      const Grid* b = Need(a, "b", err);
      if (src == nullptr || b == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      if (!b->SameShape(*src)) {
        err = "the two inputs differ in size";
        return false;
      }
      const Grid* mask = Get(a, "mask");
      if (mask != nullptr && !mask->SameShape(*src)) {
        err = "the mask differs in size";
        return false;
      }
      const std::string mode = a.p.S("mode", "normal");
      const float opacity = static_cast<float>(a.p.N("opacity", 1.0));
      Cells(out, [&](const int x, const int z) {
        const float p = src->At(x, z);
        const float q = b->At(x, z);
        float r;
        if (mode == "add") r = p + q;
        else if (mode == "sub") r = p - q;
        else if (mode == "mul") r = p * q;
        else if (mode == "screen") r = 1.0f - (1.0f - p) * (1.0f - q);
        else if (mode == "overlay") r = p < 0.5f ? 2.0f * p * q : 1.0f - 2.0f * (1.0f - p) * (1.0f - q);
        else if (mode == "min") r = p < q ? p : q;
        else if (mode == "max") r = p > q ? p : q;
        else if (mode == "diff") r = std::fabs(p - q);
        else r = q;
        const float t = opacity * (mask != nullptr ? mask->At(x, z) : 1.0f);
        return Lerp(p, r, t);
      });
      return true;
    }

    // ---- analysis --------------------------------------------------------------------------------------------------------------------------------

    /**
     * mode degrees (angle of the central-difference gradient), gradient (its rise per run), rating (the engine's rule for a mobile footprint of `window`
     * samples: the steepest neighbouring difference inside the window, per world unit; 0.75 blocks every land unit) and variation (highest minus lowest
     * sample over the window + 1 skirt: the rule for structures, MaxGroundVariation). `radius` widens the stencil of degrees / gradient (samples).
     */
    bool OpSlope(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const std::string mode = a.p.S("mode", "degrees");
      const int w = src->w;
      const int h = src->h;
      const float cell = src->cell;
      if (mode == "rating" || mode == "variation") {
        const int n = std::max(1, std::min(16, static_cast<int>(a.p.N("window", 1))));
        const bool rating = mode == "rating";
        Cells(out, [&](const int cx, const int cz) {
          if (rating) {
            float maxDelta = 0.0f;
            for (int z = cz; z <= std::min(h - 1, cz + n); ++z) {
              for (int x = cx; x < std::min(w - 1, cx + n); ++x) {
                maxDelta = std::max(maxDelta, std::fabs(src->At(x + 1, z) - src->At(x, z)));
              }
            }
            for (int x = cx; x <= std::min(w - 1, cx + n); ++x) {
              for (int z = cz; z < std::min(h - 1, cz + n); ++z) {
                maxDelta = std::max(maxDelta, std::fabs(src->At(x, z + 1) - src->At(x, z)));
              }
            }
            return maxDelta / cell;
          }
          float lo = src->At(cx, cz);
          float hi = lo;
          for (int z = cz; z <= std::min(h - 1, cz + n); ++z) {
            for (int x = cx; x <= std::min(w - 1, cx + n); ++x) {
              const float v = src->At(x, z);
              lo = std::min(lo, v);
              hi = std::max(hi, v);
            }
          }
          return hi - lo;
        });
        return true;
      }
      if (mode != "degrees" && mode != "gradient") {
        err = "slope: unknown mode '" + mode + "'";
        return false;
      }
      const int n = std::max(1, std::min(16, static_cast<int>(a.p.N("radius", 1))));
      const bool degrees = mode == "degrees";
      Cells(out, [&](const int x, const int z) {
        const int x0 = std::max(0, x - n);
        const int x1 = std::min(w - 1, x + n);
        const int z0 = std::max(0, z - n);
        const int z1 = std::min(h - 1, z + n);
        const float gx = x1 > x0 ? (src->At(x1, z) - src->At(x0, z)) / (static_cast<float>(x1 - x0) * cell) : 0.0f;
        const float gz = z1 > z0 ? (src->At(x, z1) - src->At(x, z0)) / (static_cast<float>(z1 - z0) * cell) : 0.0f;
        const float g = std::sqrt(gx * gx + gz * gz);
        return degrees ? std::atan(g) * (180.0f / kPi) : g;
      });
      return true;
    }

    /** 1 inside lo..hi, falling to 0 over `soft` height units outside. */
    bool OpHeightBand(const OpArgs& a, Grid& o, std::string& e)
    {
      const float lo = static_cast<float>(a.p.N("lo", 0.0));
      const float hi = static_cast<float>(a.p.N("hi", 1.0));
      const float soft = std::max(0.0f, static_cast<float>(a.p.N("soft", 0.0)));
      return Point(a, o, e, [&](const float v) {
        if (v >= lo && v <= hi) return 1.0f;
        if (soft <= 0.0f) return 0.0f;
        return v < lo ? SmoothStep(lo - soft, lo, v) : SmoothStep(hi + soft, hi, v);
      });
    }

    /**
     * Second derivative of the surface over `radius` samples: convex ground (hill tops, ridges) above 0.5, concave (gullies, basins) below, flat 0.5.
     * `gain` scales it; raw = 1 returns the signed value (negative laplacian per unit squared) instead.
     */
    bool OpCurvature(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const int n = std::max(1, std::min(32, static_cast<int>(a.p.N("radius", 1))));
      const float gain = static_cast<float>(a.p.N("gain", 10.0));
      const bool raw = a.p.N("raw", 0) != 0;
      const float d2 = static_cast<float>(n) * static_cast<float>(n) * src->cell * src->cell;
      const int w = src->w;
      const int h = src->h;
      Cells(out, [&](const int x, const int z) {
        const float c = src->At(x, z);
        const float lap = (src->At(std::max(0, x - n), z) + src->At(std::min(w - 1, x + n), z) + src->At(x, std::max(0, z - n)) +
                           src->At(x, std::min(h - 1, z + n)) - 4.0f * c) / d2;
        return raw ? -lap : Clamp01(0.5f - gain * lap);
      });
      return true;
    }

    bool OpWaterBelow(const OpArgs& a, Grid& o, std::string& e)
    {
      const float level = static_cast<float>(a.p.N("level", 0.0));
      const float soft = std::max(0.0f, static_cast<float>(a.p.N("soft", 0.0)));
      return Point(a, o, e, [&](const float v) { return soft > 0.0f ? SmoothStep(level + soft, level, v) : (v <= level ? 1.0f : 0.0f); });
    }

    bool OpWaterAbove(const OpArgs& a, Grid& o, std::string& e)
    {
      const float level = static_cast<float>(a.p.N("level", 0.0));
      const float soft = std::max(0.0f, static_cast<float>(a.p.N("soft", 0.0)));
      return Point(a, o, e, [&](const float v) { return soft > 0.0f ? SmoothStep(level - soft, level, v) : (v > level ? 1.0f : 0.0f); });
    }

    /** Depth of water over the ground (level minus height, 0 on land). */
    bool OpWaterDepth(const OpArgs& a, Grid& o, std::string& e)
    {
      const float level = static_cast<float>(a.p.N("level", 0.0));
      return Point(a, o, e, [&](const float v) { return v < level ? level - v : 0.0f; });
    }

    /** 1 within `width` height units of the water level (a beach band), soft edges over `soft`. */
    bool OpShore(const OpArgs& a, Grid& o, std::string& e)
    {
      const float level = static_cast<float>(a.p.N("level", 0.0));
      const float width = std::max(0.0f, static_cast<float>(a.p.N("width", 1.0)));
      const float soft = std::max(0.0f, static_cast<float>(a.p.N("soft", 0.5)));
      return Point(a, o, e, [&](const float v) {
        const float d = std::fabs(v - level);
        if (d <= width) return 1.0f;
        return soft > 0.0f ? SmoothStep(width + soft, width, d) : 0.0f;
      });
    }

    // exact Euclidean distance transform (Felzenszwalb and Huttenlocher), squared distances in samples
    constexpr double kFar = 1e20;

    void Edt1D(const double* f, const int n, double* d, int* v, double* z)
    {
      int k = 0;
      v[0] = 0;
      z[0] = -1e30;
      z[1] = 1e30;
      for (int q = 1; q < n; ++q) {
        double s = ((f[q] + static_cast<double>(q) * q) - (f[v[k]] + static_cast<double>(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        while (s <= z[k]) {
          --k;
          s = ((f[q] + static_cast<double>(q) * q) - (f[v[k]] + static_cast<double>(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
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
        d[q] = static_cast<double>(q - v[k]) * (q - v[k]) + f[v[k]];
      }
    }

    /** Squared distance (samples) of every sample to the nearest sample where feature(x, z) holds; kFar when there is none. */
    void Edt2D(const int w, const int h, const std::vector<unsigned char>& feature, std::vector<double>& dist)
    {
      std::vector<double> tmp(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
      dist.assign(tmp.size(), 0.0);
      const int colChunks = (w + 31) / 32;
      ParallelFor(colChunks, [&](const int c) {
        const int xa = c * 32;
        const int xb = std::min(w, xa + 32);
        std::vector<double> f(static_cast<std::size_t>(h));
        std::vector<double> d(static_cast<std::size_t>(h));
        std::vector<int> v(static_cast<std::size_t>(h));
        std::vector<double> z(static_cast<std::size_t>(h) + 1);
        for (int x = xa; x < xb; ++x) {
          for (int j = 0; j < h; ++j) {
            f[static_cast<std::size_t>(j)] = feature[static_cast<std::size_t>(j) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] ? 0.0 : kFar;
          }
          Edt1D(f.data(), h, d.data(), v.data(), z.data());
          for (int j = 0; j < h; ++j) {
            tmp[static_cast<std::size_t>(j) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = d[static_cast<std::size_t>(j)];
          }
        }
      });
      Rows(h, [&](const int z0, const int z1) {
        std::vector<double> d(static_cast<std::size_t>(w));
        std::vector<int> v(static_cast<std::size_t>(w));
        std::vector<double> zz(static_cast<std::size_t>(w) + 1);
        for (int j = z0; j < z1; ++j) {
          Edt1D(tmp.data() + static_cast<std::size_t>(j) * static_cast<std::size_t>(w), w, d.data(), v.data(), zz.data());
          for (int i = 0; i < w; ++i) {
            dist[static_cast<std::size_t>(j) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i)] = d[static_cast<std::size_t>(i)];
          }
        }
      });
    }

    /**
     * Distance in world units to the nearest sample above `threshold` (outside, default), from every sample inside to the nearest below it (inside), or
     * both (signed: positive outside). range > 0 divides and clamps to 0..1; `max` caps the distance.
     */
    bool OpDistance(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const float threshold = static_cast<float>(a.p.N("threshold", 0.5));
      const std::string mode = a.p.S("mode", "outside");
      const double range = a.p.N("range", 0.0);
      const double cap = a.p.N("max", 1e9);
      const std::size_t count = src->Count();
      std::vector<unsigned char> feat(count);
      for (std::size_t i = 0; i < count; ++i) {
        feat[i] = src->v[i] > threshold ? 1 : 0;
      }
      std::vector<double> dOut;
      std::vector<double> dIn;
      if (mode != "inside") {
        Edt2D(src->w, src->h, feat, dOut);
      }
      if (mode != "outside") {
        for (std::size_t i = 0; i < count; ++i) {
          feat[i] = feat[i] ? 0 : 1;
        }
        Edt2D(src->w, src->h, feat, dIn);
      }
      const double cell = src->cell;
      const int w = src->w;
      Cells(out, [&](const int x, const int z) {
        const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        const auto dist = [&](const std::vector<double>& d) { return d[i] >= kFar * 0.5 ? cap : std::min(cap, std::sqrt(d[i]) * cell); };
        double r;
        if (mode == "inside") r = dist(dIn);
        else if (mode == "signed") r = dist(dOut) - dist(dIn);
        else r = dist(dOut);
        if (range > 0.0) r = std::min(1.0, std::fabs(r) / range) * (r < 0.0 ? -1.0 : 1.0);
        return static_cast<float>(r);
      });
      return true;
    }

    // ---- thermal erosion -------------------------------------------------------------------------------------------------------------------------

    /**
     * Material slides downhill wherever the drop to a neighbour (8 neighbours) exceeds talus * distance (talus = rise per run; 0.75 is the engine's land
     * limit). Per iteration every cell gives its steepest excess times rate / 2 to its lower neighbours in proportion to their excess; everything is
     * computed from the state at the start of the iteration, so it is order independent and conserves the total height. `mask` scales the rate per cell.
     */
    bool OpThermal(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const Grid* mask = Get(a, "mask");
      if (mask != nullptr && !mask->SameShape(*src)) {
        err = "the mask differs in size";
        return false;
      }
      const int iterations = std::max(0, std::min(100000, static_cast<int>(a.p.N("iterations", 20))));
      const float talus = std::max(0.0f, static_cast<float>(a.p.N("talus", 0.6)));
      const float rate = std::max(0.0f, std::min(1.0f, static_cast<float>(a.p.N("rate", 0.5))));
      const int w = src->w;
      const int h = src->h;
      const float cell = src->cell;
      const float tAxis = talus * cell;
      const float tDiag = talus * cell * 1.41421356f;
      static const int dxs[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
      static const int dzs[8] = {0, 0, -1, 1, -1, -1, 1, 1};
      Grid cur = *src;
      Grid next = *src;
      std::vector<float> share(src->Count(), 0.0f);    // s: fraction of each neighbour's excess that leaves the cell
      std::vector<float> moved(src->Count(), 0.0f);    // total leaving the cell
      for (int it = 0; it < iterations; ++it) {
        Rows(h, [&](const int z0, const int z1) {
          for (int z = z0; z < z1; ++z) {
            for (int x = 0; x < w; ++x) {
              const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
              const float c = cur.v[i];
              float maxEx = 0.0f;
              float sumEx = 0.0f;
              for (int k = 0; k < 8; ++k) {
                const int nx = x + dxs[k];
                const int nz = z + dzs[k];
                if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
                const float ex = (c - cur.At(nx, nz)) - (k < 4 ? tAxis : tDiag);
                if (ex > 0.0f) {
                  sumEx += ex;
                  maxEx = std::max(maxEx, ex);
                }
              }
              float s = 0.0f;
              if (sumEx > 0.0f) {
                float r = rate;
                if (mask != nullptr) r *= Clamp01(mask->v[i]);
                s = r * 0.5f * maxEx / sumEx;
              }
              share[i] = s;
              moved[i] = s * sumEx;
            }
          }
        });
        Rows(h, [&](const int z0, const int z1) {
          for (int z = z0; z < z1; ++z) {
            for (int x = 0; x < w; ++x) {
              const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
              const float c = cur.v[i];
              float gain = 0.0f;
              for (int k = 0; k < 8; ++k) {
                const int nx = x + dxs[k];
                const int nz = z + dzs[k];
                if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
                const std::size_t j = static_cast<std::size_t>(nz) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx);
                const float ex = (cur.v[j] - c) - (k < 4 ? tAxis : tDiag);
                if (ex > 0.0f) gain += share[j] * ex;
              }
              next.v[i] = c - moved[i] + gain;
            }
          }
        });
        std::swap(cur.v, next.v);
      }
      out.v = std::move(cur.v);
      return true;
    }

    // ---- aspect and flow (stage T2) --------------------------------------------------------------------------------------------------------------

    /**
     * Which way the ground faces. mode facing (default): 1 where the ground descends toward `azimuth` (degrees; the direction (cos, sin) in x / z like
     * `gradient`), 0 where it faces away, 0.5 on level ground (the weight fades in over a rise per run of `flat`, default 0.05); degrees: the downhill
     * direction as an angle 0..360 in the same convention (0 where level). `radius` widens the stencil like slope.
     */
    bool OpAspect(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const std::string mode = a.p.S("mode", "facing");
      if (mode != "facing" && mode != "degrees") {
        err = "aspect: unknown mode '" + mode + "'";
        return false;
      }
      const bool degrees = mode == "degrees";
      const double ang = a.p.N("azimuth", 0.0) * 3.14159265358979 / 180.0;
      const float dx = static_cast<float>(std::cos(ang));
      const float dz = static_cast<float>(std::sin(ang));
      const float flat = std::max(1e-6f, static_cast<float>(a.p.N("flat", 0.05)));
      const int n = std::max(1, std::min(16, static_cast<int>(a.p.N("radius", 1))));
      const int w = src->w;
      const int h = src->h;
      const float cell = src->cell;
      Cells(out, [&](const int x, const int z) {
        const int x0 = std::max(0, x - n);
        const int x1 = std::min(w - 1, x + n);
        const int z0 = std::max(0, z - n);
        const int z1 = std::min(h - 1, z + n);
        const float gx = x1 > x0 ? (src->At(x1, z) - src->At(x0, z)) / (static_cast<float>(x1 - x0) * cell) : 0.0f;
        const float gz = z1 > z0 ? (src->At(x, z1) - src->At(x, z0)) / (static_cast<float>(z1 - z0) * cell) : 0.0f;
        const float g = std::sqrt(gx * gx + gz * gz);
        if (degrees) {
          if (g <= 1e-9f) return 0.0f;
          float d = std::atan2(-gz, -gx) * (180.0f / kPi);
          if (d < 0.0f) d += 360.0f;
          return d;
        }
        if (g <= 1e-9f) return 0.5f;
        const float c = (-gx * dx - gz * dz) / g;
        return 0.5f + 0.5f * c * std::min(1.0f, g / flat);
      });
      return true;
    }

    /**
     * Flow accumulation (D8): every cell passes what it collected (its own area plus everything upstream) to its steepest lower neighbour (8 neighbours,
     * ties in neighbour order); cells without a lower neighbour keep it (no pit filling). Cells are visited from the highest down (equal heights by
     * index), so the result is deterministic and does not depend on the thread count. mode cells (the count of cells draining through), area (world
     * units squared) or log (default: clamp01(ln area / ln range), `range` = the catchment area that reads as 1, default 1e6).
     */
    bool OpFlow(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      if (src == nullptr || !MakeOut(a, out, err)) {
        return false;
      }
      const std::string mode = a.p.S("mode", "log");
      if (mode != "cells" && mode != "area" && mode != "log") {
        err = "flow: unknown mode '" + mode + "'";
        return false;
      }
      const int w = src->w;
      const int h = src->h;
      const std::size_t count = src->Count();
      std::vector<std::uint32_t> order(count);
      for (std::size_t i = 0; i < count; ++i) {
        order[i] = static_cast<std::uint32_t>(i);
      }
      const std::vector<float>& hv = src->v;
      std::sort(order.begin(), order.end(), [&hv](const std::uint32_t p, const std::uint32_t q) { return hv[p] != hv[q] ? hv[p] > hv[q] : p < q; });
      std::vector<float> acc(count, 1.0f);
      static const int dxs[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
      static const int dzs[8] = {0, 0, -1, 1, -1, -1, 1, 1};
      for (std::size_t k = 0; k < count; ++k) {
        const std::uint32_t i = order[k];
        const int x = static_cast<int>(i % static_cast<std::uint32_t>(w));
        const int z = static_cast<int>(i / static_cast<std::uint32_t>(w));
        float best = 0.0f;
        std::size_t target = count;
        for (int d = 0; d < 8; ++d) {
          const int nx = x + dxs[d];
          const int nz = z + dzs[d];
          if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
          const std::size_t j = static_cast<std::size_t>(nz) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx);
          const float drop = (hv[i] - hv[j]) / (d < 4 ? 1.0f : 1.41421356f);
          if (drop > best) {
            best = drop;
            target = j;
          }
        }
        if (target < count) {
          acc[target] += acc[i];
        }
      }
      const float cell2 = src->cell * src->cell;
      const double range = std::max(2.0, a.p.N("range", 1.0e6));
      const double invLog = 1.0 / std::log(range);
      Cells(out, [&](const int x, const int z) {
        const float c = acc[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
        if (mode == "cells") return c;
        const float area = c * cell2;
        if (mode == "area") return area;
        return Clamp01(static_cast<float>(std::log(static_cast<double>(area)) * invLog));
      });
      return true;
    }

    /** Reads `a` at the world positions of the samples of `like` (bilinear, clamped at the borders): puts a field of another resolution onto this grid (a paint mask onto the height grid). */
    bool OpResample(const OpArgs& a, Grid& out, std::string& err)
    {
      const Grid* src = Need(a, "a", err);
      const Grid* like = Need(a, "like", err);
      if (src == nullptr || like == nullptr) {
        return false;
      }
      out = Grid(like->w, like->h, like->cell, like->ox, like->oz);      // not MakeOut: `a` and `like` differ in size by design
      const Grid& g = out;
      Cells(out, [&](const int x, const int z) { return SampleWorld(*src, WorldX(g, x), WorldZ(g, z)); });
      return true;
    }

    // ---- registry --------------------------------------------------------------------------------------------------------------------------------

    using OpFn = bool (*)(const OpArgs&, Grid&, std::string&);

    const std::map<std::string, OpFn>& Registry()
    {
      static const std::map<std::string, OpFn> reg = {
        {"const", OpConst},         {"white", OpWhite},           {"perlin", OpPerlin},       {"simplex", OpSimplex},       {"ridged", OpRidged},
        {"billow", OpBillow},       {"voronoi", OpVoronoi},       {"gradient", OpGradient},   {"radial", OpRadial},         {"blur", OpBlur},
        {"box", OpBox},             {"clamp", OpClamp},           {"invert", OpInvert},       {"abs", OpAbs},               {"gamma", OpGamma},
        {"affine", OpAffine},       {"smoothstep", OpSmoothstep}, {"threshold", OpThreshold}, {"normalize", OpNormalize},   {"quantize", OpQuantize},
        {"remap", OpRemap},         {"levels", OpLevels},         {"terrace", OpTerrace},     {"warp", OpWarp},             {"offset", OpOffset},
        {"rotate", OpRotate},       {"scale", OpScale},           {"mirror", OpMirror},       {"crop", OpCrop},             {"resize", OpResize},
        {"min", OpMin},             {"max", OpMax},               {"add", OpAdd},             {"sub", OpSub},               {"mul", OpMul},
        {"blend", OpBlend},         {"slope", OpSlope},           {"height_band", OpHeightBand}, {"curvature", OpCurvature},  {"water_below", OpWaterBelow},
        {"water_above", OpWaterAbove}, {"water_depth", OpWaterDepth}, {"shore", OpShore},     {"distance", OpDistance},     {"thermal", OpThermal},
        {"aspect", OpAspect},       {"flow", OpFlow},           {"resample", OpResample},
      };
      return reg;
    }
  } // namespace

  namespace
  {
    std::map<std::string, ExternalOpFn>& ExternalOps()
    {
      static std::map<std::string, ExternalOpFn> ops;
      return ops;
    }
  } // namespace

  void RegisterOp(const char* const name, const ExternalOpFn fn) { ExternalOps()[name] = fn; }

  bool RunOp(const std::string& name, const OpArgs& args, Grid& out, std::string& err)
  {
    const auto it = Registry().find(name);
    const auto ext = ExternalOps().find(name);
    if (it == Registry().end() && ext == ExternalOps().end()) {
      err = "unknown op '" + name + "'";
      return false;
    }
    try {
      return it != Registry().end() ? it->second(args, out, err) : ext->second(args, out, err);
    } catch (const std::bad_alloc&) {
      err = "out of memory";
      return false;
    }
  }

  const std::vector<std::string>& OpNames()
  {
    static const std::vector<std::string> names = [] {
      std::vector<std::string> n;
      for (const auto& kv : Registry()) {
        n.push_back(kv.first);
      }
      for (const auto& kv : ExternalOps()) {
        n.push_back(kv.first);
      }
      std::sort(n.begin(), n.end());
      return n;
    }();
    return names;
  }

  // ---- analysis helpers ------------------------------------------------------------------------------------------------------------------------

  Stats Measure(const Grid& g)
  {
    Stats s;
    if (g.Count() == 0) {
      return s;
    }
    struct Part
    {
      float lo = 0, hi = 0;
      double sum = 0, sq = 0;
      bool any = false;
    };
    const int chunks = (g.h + kRowChunk - 1) / kRowChunk;
    std::vector<Part> parts(static_cast<std::size_t>(chunks));
    ParallelFor(chunks, [&](const int c) {
      Part& p = parts[static_cast<std::size_t>(c)];
      const int z0 = c * kRowChunk;
      const int z1 = std::min(g.h, z0 + kRowChunk);
      for (int z = z0; z < z1; ++z) {
        for (int x = 0; x < g.w; ++x) {
          const float v = g.At(x, z);
          if (!p.any) {
            p.lo = p.hi = v;
            p.any = true;
          } else {
            p.lo = std::min(p.lo, v);
            p.hi = std::max(p.hi, v);
          }
          p.sum += v;
          p.sq += static_cast<double>(v) * v;
        }
      }
    });
    double sum = 0.0;
    double sq = 0.0;
    bool any = false;
    for (const Part& p : parts) {
      if (!p.any) continue;
      if (!any) {
        s.min = p.lo;
        s.max = p.hi;
        any = true;
      } else {
        s.min = std::min(s.min, p.lo);
        s.max = std::max(s.max, p.hi);
      }
      sum += p.sum;
      sq += p.sq;
    }
    const double n = static_cast<double>(g.Count());
    s.mean = sum / n;
    s.sd = std::sqrt(std::max(0.0, sq / n - s.mean * s.mean));
    return s;
  }

  std::uint64_t Checksum(const Grid& g)
  {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](const std::uint32_t word) {
      for (int i = 0; i < 4; ++i) {
        h ^= (word >> (8 * i)) & 0xFFu;
        h *= 1099511628211ULL;
      }
    };
    mix(static_cast<std::uint32_t>(g.w));
    mix(static_cast<std::uint32_t>(g.h));
    for (const float f : g.v) {
      std::uint32_t bits;
      const float norm = f == 0.0f ? 0.0f : f;
      std::memcpy(&bits, &norm, sizeof(bits));
      mix(bits);
    }
    return h;
  }

  float SampleWorld(const Grid& g, const double wx, const double wz)
  {
    return Bilinear(g, (wx - static_cast<double>(g.ox)) / static_cast<double>(g.cell), (wz - static_cast<double>(g.oz)) / static_cast<double>(g.cell), kClamp, 0.0f);
  }

  void Downsample(const Grid& g, const int pw, const int ph, std::vector<float>& out)
  {
    out.assign(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph), 0.0f);
    if (g.Count() == 0 || pw < 1 || ph < 1) {
      return;
    }
    for (int j = 0; j < ph; ++j) {
      const int z0 = static_cast<int>(static_cast<long long>(j) * g.h / ph);
      const int z1 = std::max(z0 + 1, static_cast<int>(static_cast<long long>(j + 1) * g.h / ph));
      for (int i = 0; i < pw; ++i) {
        const int x0 = static_cast<int>(static_cast<long long>(i) * g.w / pw);
        const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(i + 1) * g.w / pw));
        double sum = 0.0;
        int n = 0;
        for (int z = z0; z < std::min(g.h, z1); ++z) {
          for (int x = x0; x < std::min(g.w, x1); ++x, ++n) {
            sum += g.At(x, z);
          }
        }
        out[static_cast<std::size_t>(j) * static_cast<std::size_t>(pw) + static_cast<std::size_t>(i)] = n > 0 ? static_cast<float>(sum / n) : 0.0f;
      }
    }
  }
} // namespace moho::editor::field
