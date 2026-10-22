// Stand-alone test and lab of the erosion suite (no engine needed): build + run with `python scripts/ed_native_tests.py`.
//   EdErosionTest.exe <cases.lua> [--update]          golden cases (editor/runtime/lua/editor/selftest_erosion_cases.lua), thread independence, property tests
//   EdErosionTest.exe <cases.lua> --lab <outdir> [size] [preset]   runs the presets on a size x size mountain field, prints statistics and timings, writes PNG pictures
//   EdErosionTest.exe <cases.lua> --bench             timings on 1025 x 1025
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "moho/editor/EdErosion.h"
#include "moho/editor/EdFieldCore.h"

using namespace moho::editor::field;
namespace er = moho::editor::field::erosion;

static int gFailures = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      ++gFailures;                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static std::string Hex(const std::uint64_t v)
{
  char b[32];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

static double Now()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::vector<std::string> Split(const std::string& s, const char sep)
{
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

static void ParseParams(const std::string& text, Params& p)
{
  if (text.empty()) return;
  for (const std::string& kv : Split(text, ',')) {
    const std::size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
    char* end = nullptr;
    const double n = std::strtod(val.c_str(), &end);
    if (!val.empty() && end != nullptr && *end == '\0') p.num[key] = n;
    else p.str[key] = val;
  }
}

static GridPtr Make(const std::string& op, const std::string& params, const std::map<std::string, GridPtr>& in, std::map<std::string, Grid>* extra = nullptr)
{
  OpArgs a;
  ParseParams(params, a.p);
  a.in = in;
  a.extra = extra;
  auto g = std::make_shared<Grid>();
  std::string e;
  if (!RunOp(op, a, *g, e)) {
    std::printf("op %s(%s) failed: %s\n", op.c_str(), params.c_str(), e.c_str());
    return nullptr;
  }
  return g;
}

/** A mountain field: ridged noise under a broad uplift, in world units (cell 1). */
static GridPtr Mountains(const int n, const int seed = 3, const char* ampR = "mul=55,add=0", const char* ampP = "mul=8,add=0")
{
  char buf[200], buf2[200];
  std::snprintf(buf, sizeof(buf), "width=%d,height=%d,seed=%d,scale=%d,octaves=3,persistence=0.4", n, n, seed, std::max(24, n * 2 / 5));
  std::snprintf(buf2, sizeof(buf2), "width=%d,height=%d,seed=%d,scale=%d,octaves=3", n, n, seed + 100, std::max(12, n / 5));
  const GridPtr r = Make("ridged", buf, {});
  const GridPtr p = Make("perlin", buf2, {});
  const GridPtr rr = Make("affine", ampR, {{"a", r}});
  const GridPtr pp = Make("affine", ampP, {{"a", p}});
  return Make("add", "", {{"a", rr}, {"b", pp}});
}

/** The lab terrain: ridged fractal noise made of perlin (1 - |2p - 1|, without the lattice seams of the ridged op) with slopes like the stock maps (mean gradient 0.3), range about 10 .. 40. */
static GridPtr LabTerrain(const int n, const double amplitude)
{
  char buf[200];
  std::snprintf(buf, sizeof(buf), "width=%d,height=%d,seed=21,scale=%d,octaves=5,persistence=0.5", n, n, std::max(32, n / 2));
  const GridPtr p = Make("perlin", buf, {});
  const GridPtr a = Make("affine", "mul=2,add=-1", {{"a", p}});
  const GridPtr b = Make("abs", "", {{"a", a}});
  const GridPtr c = Make("affine", "mul=-1,add=1", {{"a", b}});
  std::snprintf(buf, sizeof(buf), "mul=%g,add=10", amplitude);
  return Make("affine", buf, {{"a", c}});
}

// ---- PNG output (stored deflate blocks) ----------------------------------------------------------------------------------------------------------

static std::uint32_t Crc(const std::uint8_t* d, const std::size_t n, std::uint32_t c = 0xFFFFFFFFu)
{
  static std::uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    init = true;
  }
  for (std::size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c;
}

static void Put32(std::vector<std::uint8_t>& o, const std::uint32_t v)
{
  o.push_back(static_cast<std::uint8_t>(v >> 24));
  o.push_back(static_cast<std::uint8_t>(v >> 16));
  o.push_back(static_cast<std::uint8_t>(v >> 8));
  o.push_back(static_cast<std::uint8_t>(v));
}

static void Chunk(std::vector<std::uint8_t>& o, const char* type, const std::vector<std::uint8_t>& data)
{
  Put32(o, static_cast<std::uint32_t>(data.size()));
  std::vector<std::uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  o.insert(o.end(), body.begin(), body.end());
  Put32(o, ~Crc(body.data(), body.size()));
}

static void WritePng(const std::string& path, const int w, const int h, const std::vector<std::uint8_t>& rgb)
{
  std::vector<std::uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + static_cast<std::size_t>(y) * w * 3, rgb.begin() + static_cast<std::size_t>(y + 1) * w * 3);
  }
  std::vector<std::uint8_t> z = {0x78, 0x01};
  std::size_t pos = 0;
  while (pos < raw.size()) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
    z.push_back(pos + n >= raw.size() ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(n));
    z.push_back(static_cast<std::uint8_t>(n >> 8));
    z.push_back(static_cast<std::uint8_t>(~n));
    z.push_back(static_cast<std::uint8_t>((~n) >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  std::uint32_t a = 1, b = 0;
  for (const std::uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  Put32(z, (b << 16) | a);
  std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> ihdr;
  Put32(ihdr, w);
  Put32(ihdr, h);
  ihdr.push_back(8);
  ihdr.push_back(2);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

/** Shaded relief with a hypsometric tint; the picture is scaled to at most 1024 wide by box sampling. */
static void DumpRelief(const std::string& path, const Grid& g, const float lo, const float hi)
{
  const int step = std::max(1, (g.w + 1023) / 1024);
  const int up = g.w <= 300 ? 3 : 1;
  const int pw = g.w / step * up, ph = g.h / step * up;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(pw) * ph * 3);
  for (int y = 0; y < ph; ++y) {
    for (int x = 0; x < pw; ++x) {
      const int sx = x / up * step, sy = y / up * step;
      const auto H = [&](const int i, const int j) { return g.At(std::max(0, std::min(g.w - 1, i)), std::max(0, std::min(g.h - 1, j))); };
      const float gx = (H(sx + 1, sy) - H(sx - 1, sy)) / (2.0f * g.cell);
      const float gz = (H(sx, sy + 1) - H(sx, sy - 1)) / (2.0f * g.cell);
      // light from the north west
      const float shade = std::max(0.15f, std::min(1.2f, 0.7f + (-gx * 0.7f - gz * 0.7f) * 1.8f));
      const float t = std::max(0.0f, std::min(1.0f, (g.At(sx, sy) - lo) / std::max(1e-6f, hi - lo)));
      const float r = (0.25f + 0.55f * t) * shade, gg = (0.35f + 0.45f * t) * shade, b = (0.2f + 0.7f * t * t) * shade;
      const std::size_t o = (static_cast<std::size_t>(y) * pw + x) * 3;
      rgb[o] = static_cast<std::uint8_t>(255.0f * std::min(1.0f, r * 1.3f));
      rgb[o + 1] = static_cast<std::uint8_t>(255.0f * std::min(1.0f, gg * 1.3f));
      rgb[o + 2] = static_cast<std::uint8_t>(255.0f * std::min(1.0f, b * 1.3f));
    }
  }
  WritePng(path, pw, ph, rgb);
}

static void DumpMask(const std::string& path, const Grid& g, float hi = -1.0f, const bool logScale = false)
{
  const Stats s = Measure(g);
  if (hi <= 0.0f) hi = static_cast<float>(s.mean + 3.0 * s.sd);
  if (hi <= 1e-9f) hi = 1.0f;
  const int step = std::max(1, (g.w + 1023) / 1024);
  const int pw = g.w / step, ph = g.h / step;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(pw) * ph * 3);
  for (int y = 0; y < ph; ++y) {
    for (int x = 0; x < pw; ++x) {
      float t = g.At(x * step, y * step) / hi;
      if (logScale) t = std::log1p(g.At(x * step, y * step)) / std::log1p(hi);
      t = std::max(0.0f, std::min(1.0f, t));
      const std::uint8_t v = static_cast<std::uint8_t>(255.0f * t);
      const std::size_t o = (static_cast<std::size_t>(y) * pw + x) * 3;
      rgb[o] = rgb[o + 1] = rgb[o + 2] = v;
    }
  }
  WritePng(path, pw, ph, rgb);
}

// ---- presets (the same numbers as erosion.lua) ----------------------------------------------------------------------------------------------------

struct Preset
{
  const char* name;
  const char* op;
  const char* params;
};

static const Preset kPresets[] = {
  {"hydraulic", "erode_hydraulic", "iterations=300"},
  {"droplet", "erode_droplet", "density=1.0"},
  {"stream", "erode_stream", "iterations=12"},
  // the presets of erosion.lua (keep in sync)
  {"mountain", "erode_droplet", "density=4,lifetime=70,inertia=0.3,capacity=8,erode=0.6,deposit=0.15,gravity=6,thermal=0.4,thermal_steps=3,talus=0.9"},
  {"hills", "erode_hydraulic", "iterations=150,rain=0.004,erosion=0.08,deposition=0.08,thermal=0.3,talus=0.6"},
  {"canyon", "erode_stream", "iterations=14,k=1.5,max_cut=0.7,diffusion=0.05,widen=2,talus=1.2,area_min=40"},
  {"alpine", "erode_droplet", "density=3,lifetime=80,inertia=0.2,capacity=6,erode=0.5,deposit=0.05,gravity=8,thermal=0.6,thermal_steps=6,talus=0.7"},
  {"desert", "erode_hydraulic", "iterations=120,rain=0.002,evaporation=0.05,capacity=0.5,deposition=0.2,thermal=0.6,talus=0.35"},
  {"river", "erode_stream", "iterations=20,k=0.5,m=0.5,diffusion=0.3,widen=2,area_min=60,talus=0.5"},
};

// ---- tests ------------------------------------------------------------------------------------------------------------------------------------------

struct Case
{
  std::string label, op, params, inputs, golden;
};

static std::vector<std::string> gLines;
static std::vector<Case> gCases;
static std::size_t gBegin = 0, gEnd = 0;
static std::map<std::string, GridPtr> gFix;

static bool LoadCases(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    gLines.push_back(line);
  }
  bool inside = false;
  for (std::size_t i = 0; i < gLines.size(); ++i) {
    const std::string& l = gLines[i];
    if (!inside) {
      if (l.find("Cases = [[") == 0) {
        inside = true;
        gBegin = i + 1;
      }
      continue;
    }
    if (l.find("]]") == 0) {
      gEnd = i;
      break;
    }
    if (l.empty() || l[0] == '#') continue;
    const std::vector<std::string> f = Split(l, '|');
    if (f.size() < 5) continue;
    gCases.push_back({f[0], f[1], f[2], f[3], f[4]});
  }
  return !gCases.empty();
}

static void BuildFixtures()
{
  // H: a 65 x 65 mountain field, R: a rock hardness mask, F: a flat plate
  gFix["H"] = Mountains(65, 11);
  gFix["R"] = Make("perlin", "width=65,height=65,seed=4,scale=14,octaves=3", {});
  gFix["F"] = Make("const", "width=33,height=33,value=10", {});
}

static std::map<std::string, GridPtr> InputsOf(const std::string& spec)
{
  std::map<std::string, GridPtr> in;
  for (const std::string& kv : Split(spec, ',')) {
    const std::size_t eq = kv.find('=');
    if (eq != std::string::npos) in[kv.substr(0, eq)] = gFix[kv.substr(eq + 1)];
  }
  return in;
}

static void TestGoldens(const bool update)
{
  const int orig = Threads();
  const int counts[] = {1, 2, 3, 5, 8};
  std::vector<std::string> now;
  for (const int tc : counts) {
    SetThreads(tc);
    BuildFixtures();
    for (std::size_t i = 0; i < gCases.size(); ++i) {
      const Case& c = gCases[i];
      const GridPtr g = Make(c.op, c.params, InputsOf(c.inputs));
      if (!g) {
        CHECK(false, "case %s failed", c.label.c_str());
        if (tc == counts[0]) now.push_back("0");
        continue;
      }
      const std::string hex = Hex(Checksum(*g));
      if (tc == counts[0]) now.push_back(hex);
      else CHECK(hex == now[i], "case %s differs with %d threads (%s vs %s)", c.label.c_str(), tc, hex.c_str(), now[i].c_str());
    }
  }
  SetThreads(orig);
  int wrong = 0;
  for (std::size_t i = 0; i < gCases.size(); ++i) {
    if (gCases[i].golden != now[i]) {
      ++wrong;
      if (!update) std::printf("golden %s: file %s, now %s\n", gCases[i].label.c_str(), gCases[i].golden.c_str(), now[i].c_str());
    }
  }
  if (update) {
    // rewrite the golden column in the file lines
    std::size_t ci = 0;
    for (std::size_t i = gBegin; i < gEnd; ++i) {
      std::string& l = gLines[i];
      if (l.empty() || l[0] == '#') continue;
      const std::vector<std::string> f = Split(l, '|');
      if (f.size() < 5) continue;
      l = f[0] + "|" + f[1] + "|" + f[2] + "|" + f[3] + "|" + now[ci++];
    }
  } else {
    CHECK(wrong == 0, "%d golden checksums differ (run with --update after checking the change)", wrong);
  }
  std::printf("goldens: %zu cases, %d differ%s\n", gCases.size(), wrong, update ? " (updated)" : "");
}

static double Sum(const std::vector<float>& v)
{
  double s = 0.0;
  for (const float x : v) s += x;
  return s;
}

static void TestProperties()
{
  SetThreads(4);
  // 1. a flat plate stays (nearly) flat for every solver (the droplets may carve the faint ramp that lets water leave a flat)
  const GridPtr flat = Make("const", "width=48,height=48,value=10", {});
  for (const Preset& p : kPresets) {
    const GridPtr r = Make(p.op, p.params, {{"a", flat}});
    const Stats s = Measure(*r);
    CHECK(r && std::fabs(s.min - 10.0f) < 0.1f && std::fabs(s.max - 10.0f) < 0.1f, "%s: a flat plate changed (%f .. %f)", p.name, s.min, s.max);
  }
  // 2. mass: the hydraulic solver with closed edges and settling keeps the total height (rain evaporates, sediment settles)
  {
    const GridPtr H = Mountains(97, 5);
    std::map<std::string, Grid> extra;
    const GridPtr r = Make("erode_hydraulic", "iterations=120,edge=closed,settle=1", {{"a", H}}, &extra);
    const double before = Sum(H->v), after = Sum(r->v);
    CHECK(std::fabs(before - after) / before < 2e-3, "hydraulic closed: mass changed %g -> %g", before, after);
    CHECK(extra.count("wear") && extra.count("deposit") && extra.count("flow") && extra.count("water") && extra.count("sediment"), "extra outputs missing");
    const Stats w = Measure(extra["wear"]), d = Measure(extra["deposit"]);
    CHECK(w.min >= 0.0f && d.min >= 0.0f, "wear / deposit negative");
    CHECK(w.max > 0.0f, "no wear at all");
    // wear - deposit = height lost
    double net = 0.0;
    for (std::size_t i = 0; i < H->v.size(); ++i) net += static_cast<double>(extra["wear"].v[i]) - extra["deposit"].v[i] - (H->v[i] - r->v[i]);
    CHECK(std::fabs(net) / before < 2e-3, "wear - deposit does not match the height change (%g)", net);
    // the border is left alone with pinned edges
    const GridPtr p = Make("erode_hydraulic", "iterations=40,edge=pinned", {{"a", H}});
    bool same = true;
    for (int x = 0; x < H->w; ++x) {
      same = same && p->At(x, 0) == H->At(x, 0) && p->At(x, H->h - 1) == H->At(x, H->h - 1);
    }
    CHECK(same, "the pinned border changed");
  }
  // 3. erosion lowers the high ground and does not run away
  {
    const GridPtr H = Mountains(97, 6);
    for (const Preset& pr : kPresets) {
      const GridPtr r = Make(pr.op, pr.params, {{"a", H}});
      const Stats a = Measure(*H), b = Measure(*r);
      CHECK(b.max <= a.max + 0.5f, "%s raised the peak (%f -> %f)", pr.name, a.max, b.max);
      CHECK(b.min >= a.min - 1.0f, "%s dug %f below the lowest point", pr.name, a.min - b.min);
      CHECK(std::isfinite(b.mean) && std::isfinite(b.sd), "%s produced non-finite values", pr.name);
      CHECK(r->v != H->v, "%s changed nothing", pr.name);
    }
  }
  // 4. erodibility mask 0 protects the terrain
  {
    const GridPtr H = Mountains(65, 7);
    const GridPtr zero = Make("const", "width=65,height=65,value=0", {});
    const GridPtr r = Make("erode_hydraulic", "iterations=60,settle=0", {{"a", H}, {"mask", zero}});
    // no dissolving: heights only change by deposit of nothing -> identical
    CHECK(r->v == H->v, "a zero erodibility mask still changed the terrain");
  }
  // 5. the brush session: deterministic, bounded, only the window changes
  {
    const int n = 129;
    const GridPtr H = Mountains(n, 8);
    std::vector<std::uint16_t> words(H->Count());
    for (std::size_t i = 0; i < words.size(); ++i) words[i] = static_cast<std::uint16_t>(std::floor(H->v[i] * 128.0f + 0.5f));
    auto run = [&](const int threads) {
      SetThreads(threads);
      er::ErodeSession s;
      s.Begin(n, n, words.data(), er::HydroParams());
      std::vector<std::uint16_t> cur = words;
      for (int k = 0; k < 6; ++k) {
        er::BrushDab d;
        d.cx = 50.0 + k * 3.0;
        d.cz = 60.0;
        d.radius = 14;
        d.iterations = 10;
        int x0, z0, x1, z1;
        std::vector<std::uint16_t> w;
        CHECK(s.Dab(d, x0, z0, x1, z1, w), "dab failed");
        for (int z = z0; z <= z1; ++z)
          for (int x = x0; x <= x1; ++x) cur[static_cast<std::size_t>(z) * n + x] = w[static_cast<std::size_t>(z - z0) * (x1 - x0 + 1) + (x - x0)];
      }
      return cur;
    };
    const std::vector<std::uint16_t> a = run(1), b = run(7);
    CHECK(a == b, "the brush session depends on the thread count");
    int changed = 0, outside = 0;
    for (int z = 0; z < n; ++z) {
      for (int x = 0; x < n; ++x) {
        const std::size_t i = static_cast<std::size_t>(z) * n + x;
        if (a[i] != words[i]) {
          ++changed;
          if (x < 20 || x > 90 || z < 30 || z > 90) ++outside;
        }
      }
    }
    CHECK(changed > 0, "the brush changed nothing");
    CHECK(outside == 0, "the brush changed %d samples far outside its window", outside);
  }

  std::printf("properties done\n");
}


// ---- the map boundary ---------------------------------------------------------------------------------------------------------------------------

/** Mean |h(edge) - h(edge + 1)| over the four sides (corners left out): a vertical wall or lip at the border shows up as a jump of this step. */
static double EdgeStep(const Grid& g)
{
  double s = 0.0;
  int n = 0;
  for (int i = 2; i < g.w - 2; ++i) {
    s += std::fabs(g.At(i, 0) - g.At(i, 1)) + std::fabs(g.At(i, g.h - 1) - g.At(i, g.h - 2));
    n += 2;
  }
  for (int j = 2; j < g.h - 2; ++j) {
    s += std::fabs(g.At(0, j) - g.At(1, j)) + std::fabs(g.At(g.w - 1, j) - g.At(g.w - 2, j));
    n += 2;
  }
  return s / n;
}

/** Mean change |after - before| of the outermost cells over the mean change of the cells 8 .. 12 in from the border: a ring that is left out (or hit twice as hard) breaks this. */
static double EdgeChangeRatio(const Grid& before, const Grid& after)
{
  double edge = 0.0, inner = 0.0;
  int ne = 0, ni = 0;
  for (int z = 0; z < before.h; ++z) {
    for (int x = 0; x < before.w; ++x) {
      const int d = std::min(std::min(x, before.w - 1 - x), std::min(z, before.h - 1 - z));
      const double c = std::fabs(static_cast<double>(after.At(x, z)) - before.At(x, z));
      if (d == 0) { edge += c; ++ne; }
      else if (d >= 8 && d <= 12) { inner += c; ++ni; }
    }
  }
  return (edge / ne) / std::max(1e-9, inner / ni);
}

/** A hilly field with a tilt, so water and droplets run towards and over the borders. */
static GridPtr Tilted(const int n, const int seed, const double tilt)
{
  const GridPtr M = Mountains(n, seed);
  auto g = std::make_shared<Grid>(*M);
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) g->At(x, z) += static_cast<float>(tilt * (std::fabs(x - 0.5 * n) + 0.7 * std::fabs(z - 0.5 * n)));
  return g;
}

static void TestBoundary()
{
  SetThreads(4);
  struct Case { const char* name; const char* op; const char* params; };
  static const Case cases[] = {
    {"hydraulic", "erode_hydraulic", "iterations=200,rain=0.006,erosion=0.12"},
    {"hydraulic thermal", "erode_hydraulic", "iterations=120,thermal=0.5,talus=0.6"},
    {"droplet", "erode_droplet", "density=3,lifetime=60,seed=4"},
    {"droplet thermal", "erode_droplet", "density=3,lifetime=40,seed=5,thermal=0.5,thermal_steps=10"},
    {"stream", "erode_stream", "iterations=12,uplift=0.2"},
  };
  for (const double tilt : {0.0, 0.25, -0.25}) {
    const GridPtr H = Tilted(129, 12, tilt);
    const double before = EdgeStep(*H);
    for (const Case& c : cases) {
      const GridPtr r = Make(c.op, c.params, {{"a", H}});
      const double after = EdgeStep(*r);
      const double ratio = EdgeChangeRatio(*H, *r);
      std::printf("boundary tilt %+.2f %-18s edge step %.3f -> %.3f, edge change / inner change %.2f\n", tilt, c.name, before, after, ratio);
      CHECK(after <= before * 1.2 + 0.05, "%s (tilt %g): a step at the border: %.3f -> %.3f", c.name, tilt, before, after);
      CHECK(ratio > 0.25 && ratio < 2.5, "%s (tilt %g): the border cells change %.2f times as much as the inside (a pinned ring changes 0, a clipped brush 2 and more)", c.name, tilt, ratio);
    }
  }
}

/** The local brushes (erode, rain, deposit) at the map border: the stroke runs along the left and top edge of a tilted field. */
static void TestBrushBoundary()
{
  SetThreads(2);
  const int n = 97;
  const GridPtr H = Tilted(n, 13, 0.3);
  std::vector<std::uint16_t> words(H->Count());
  for (std::size_t i = 0; i < words.size(); ++i) words[i] = static_cast<std::uint16_t>(std::floor(H->v[i] * 128.0f + 0.5f));
  for (int mode = 0; mode < 3; ++mode) {
    er::ErodeSession s;
    er::HydroParams hp;   // the strokes of the editor use rates like these (erosionbrush.lua)
    hp.rain = 0.012f;
    hp.erosion = 0.3f;
    hp.deposition = 0.1f;
    hp.thermal = 0.3f;
    s.Begin(n, n, words.data(), hp);
    Grid cur = *H;
    auto put = [&](const int x0, const int z0, const int x1, const int z1, const std::vector<std::uint16_t>& w) {
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) cur.At(x, z) = static_cast<float>(w[static_cast<std::size_t>(z - z0) * (x1 - x0 + 1) + (x - x0)]) / 128.0f;
    };
    for (int k = 0; k < 10; ++k) {
      for (int side = 0; side < 2; ++side) {
        er::BrushDab d;
        d.mode = mode;
        d.radius = 12;
        d.cx = side == 0 ? 2.0 : 10.0 + k * 7.0;
        d.cz = side == 0 ? 10.0 + k * 7.0 : 2.0;
        d.iterations = 24;
        d.amount = 1.5;
        int x0, z0, x1, z1;
        std::vector<std::uint16_t> w;
        if (s.Dab(d, x0, z0, x1, z1, w)) put(x0, z0, x1, z1, w);
      }
    }
    int x0, z0, x1, z1;
    std::vector<std::uint16_t> w;
    if (s.Finish(x0, z0, x1, z1, w)) put(x0, z0, x1, z1, w);
    // the left column x = 0 and the top row z = 0, compared with the step the field had
    double b = 0.0, a = 0.0;
    int cnt = 0;
    for (int i = 2; i < n - 2; ++i) {
      b += std::fabs(H->At(0, i) - H->At(1, i)) + std::fabs(H->At(i, 0) - H->At(i, 1));
      a += std::fabs(cur.At(0, i) - cur.At(1, i)) + std::fabs(cur.At(i, 0) - cur.At(i, 1));
      cnt += 2;
    }
    b /= cnt;
    a /= cnt;
    std::printf("brush mode %d: edge step %.3f -> %.3f, edge change / inner change %.2f\n", mode, b, a, EdgeChangeRatio(*H, cur));
    CHECK(a <= b * 1.2 + 0.05, "brush mode %d: a step at the map border: %.3f -> %.3f", mode, b, a);
  }
}


// ---- variation, height preservation, conservation ---------------------------------------------------------------------------------------------

/** How unevenly the erosion is spread over the map: the coefficient of variation of the mean |change| of 8 x 8 blocks (0 = every block is changed alike, the evenly spaced look of uniform rain and identical droplets). */
static double RegionalVariation(const Grid& before, const Grid& after, double* meanChange = nullptr)
{
  const int B = 8, margin = 16;      // the blocks cover the map without its border strip (edge effects are not what is measured here)
  const int iw = before.w - 2 * margin, ih = before.h - 2 * margin;
  double sum[B][B] = {}, cnt[B][B] = {};
  double all = 0.0;
  for (int z = margin; z < before.h - margin; ++z) {
    for (int x = margin; x < before.w - margin; ++x) {
      const double c = std::fabs(static_cast<double>(after.At(x, z)) - before.At(x, z));
      sum[(z - margin) * B / ih][(x - margin) * B / iw] += c;
      cnt[(z - margin) * B / ih][(x - margin) * B / iw] += 1.0;
      all += c;
    }
  }
  double m = 0.0, m2 = 0.0;
  for (int j = 0; j < B; ++j)
    for (int i = 0; i < B; ++i) {
      const double v = sum[j][i] / cnt[j][i];
      m += v;
      m2 += v * v;
    }
  m /= B * B;
  m2 /= B * B;
  if (meanChange != nullptr) *meanChange = all / static_cast<double>(iw * ih);
  return std::sqrt(std::max(0.0, m2 - m * m)) / std::max(1e-9, m);
}

static double Total(const Grid& g) { return Sum(g.v); }

/** A noisy hillside: a tilted plane with gentle noise on it (what uniform rain and identical droplets turn into evenly spaced parallel rills). */
static GridPtr Hillside(const int n, const int seed)
{
  char buf[160];
  std::snprintf(buf, sizeof(buf), "width=%d,height=%d,seed=%d,scale=%d,octaves=4,persistence=0.5", n, n, seed, std::max(16, n / 3));
  const GridPtr p = Make("perlin", buf, {});
  auto g = std::make_shared<Grid>(*p);
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) g->At(x, z) = 1.5f * p->At(x, z) + 0.35f * static_cast<float>(x) + 0.2f * static_cast<float>(z) + 20.0f;
  return g;
}

static void TestVariation()
{
  SetThreads(4);
  const GridPtr M = Mountains(129, 21);
  const GridPtr H = Hillside(129, 21);
  struct Case { const char* name; const char* op; const char* base; const char* vary; };
  static const Case cases[] = {
    {"hydraulic", "erode_hydraulic", "iterations=200", "vary=0.9,vary_scale=40,rain_vary=0.8"},
    {"droplet", "erode_droplet", "density=2,seed=3", "vary=0.9,vary_scale=40,rain_vary=0.8,drop_vary=0.8"},
    {"stream", "erode_stream", "iterations=8,k=0.5", "vary=0.9,vary_scale=40,rain_vary=0.8,jitter=0.5"},
  };
  for (const Case& c : cases) {
    const GridPtr plain = Make(c.op, c.base, {{"a", H}});
    const GridPtr varied = Make(c.op, (std::string(c.base) + "," + c.vary).c_str(), {{"a", H}});
    double m0 = 0.0, m1 = 0.0;
    const double v0 = RegionalVariation(*H, *plain, &m0), v1 = RegionalVariation(*H, *varied, &m1);
    std::printf("variation %-10s regional variation %.3f -> %.3f (mean change %.3f -> %.3f)\n", c.name, v0, v1, m0, m1);
    CHECK(v1 > v0 * 1.08, "%s: the variation options do not make the erosion more uneven (%.3f -> %.3f)", c.name, v0, v1);
    CHECK(varied->v != plain->v, "%s: the variation options changed nothing", c.name);
    // thread independence of the new paths
    SetThreads(1);
    const GridPtr again = Make(c.op, (std::string(c.base) + "," + c.vary).c_str(), {{"a", H}});
    SetThreads(4);
    CHECK(again->v == varied->v, "%s: the variation depends on the thread count", c.name);
    // a different seed gives a different pattern
    const GridPtr other = Make(c.op, (std::string(c.base) + "," + c.vary + ",seed=77").c_str(), {{"a", H}});
    CHECK(other->v != varied->v, "%s: the seed does not change the variation", c.name);
  }
  // keep range, max change (on the mountain field)
  const Stats hs = Measure(*M);
  for (const Case& c : cases) {
    const GridPtr kept = Make(c.op, (std::string(c.base) + ",keep_range=1").c_str(), {{"a", M}});
    const Stats ks = Measure(*kept);
    const auto pct = [](std::vector<float> v, const double q) {
      std::sort(v.begin(), v.end());
      return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
    };
    const float span = hs.max - hs.min;
    CHECK(ks.min >= hs.min - 1e-3f && ks.max <= hs.max + 1e-3f, "%s: keep_range left the input range: %f .. %f (input %f .. %f)", c.name, ks.min, ks.max, hs.min, hs.max);
    CHECK(std::fabs(pct(kept->v, 0.01) - pct(M->v, 0.01)) < 0.04f * span && std::fabs(pct(kept->v, 0.99) - pct(M->v, 0.99)) < 0.04f * span,
          "%s: keep_range: the 1%% / 99%% heights are %f / %f, the input's %f / %f", c.name, pct(kept->v, 0.01), pct(kept->v, 0.99), pct(M->v, 0.01), pct(M->v, 0.99));
    const GridPtr capped = Make(c.op, (std::string(c.base) + ",max_change=1.5").c_str(), {{"a", M}});
    double worst = 0.0;
    for (std::size_t i = 0; i < M->v.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(capped->v[i]) - M->v[i]));
    CHECK(worst <= 1.5 + 1e-3, "%s: max_change 1.5 left a change of %f", c.name, worst);
    const GridPtr plain = Make(c.op, c.base, {{"a", M}});
    double worstPlain = 0.0;
    for (std::size_t i = 0; i < M->v.size(); ++i) worstPlain = std::max(worstPlain, std::fabs(static_cast<double>(plain->v[i]) - M->v[i]));
    std::printf("range %-10s plain %.2f .. %.2f (max change %.2f), kept %.2f .. %.2f, input %.2f .. %.2f\n", c.name, Measure(*plain).min, Measure(*plain).max, worstPlain, ks.min, ks.max, hs.min, hs.max);
  }
  // conserve: the volume the run lost comes back
  for (const Case& c : cases) {
    const GridPtr plain = Make(c.op, c.base, {{"a", M}});
    const GridPtr kept = Make(c.op, (std::string(c.base) + ",conserve=1").c_str(), {{"a", M}});
    const double t0 = Total(*M), t1 = Total(*plain), t2 = Total(*kept);
    std::printf("volume %-10s lost %.4f%% -> %.6f%%\n", c.name, 100.0 * (t0 - t1) / t0, 100.0 * (t0 - t2) / t0);
    CHECK(std::fabs(t0 - t2) / t0 < 1e-3, "%s: conserve=1 left a volume change of %g", c.name, (t0 - t2) / t0);
  }
  // protect: the summits keep more of their height
  for (const Case& c : cases) {
    const GridPtr plain = Make(c.op, c.base, {{"a", M}});
    const GridPtr prot = Make(c.op, (std::string(c.base) + ",protect=1").c_str(), {{"a", M}});
    std::vector<float> sorted = M->v;
    std::sort(sorted.begin(), sorted.end());
    const float top = sorted[sorted.size() * 95 / 100];
    double d0 = 0.0, d1 = 0.0;
    int cnt = 0;
    for (std::size_t i = 0; i < M->v.size(); ++i) {
      if (M->v[i] < top) continue;
      d0 += M->v[i] - plain->v[i];
      d1 += M->v[i] - prot->v[i];
      ++cnt;
    }
    std::printf("protect %-10s the top 5%% lose %.3f -> %.3f on average\n", c.name, d0 / cnt, d1 / cnt);
    CHECK(d1 <= d0 * 0.8 + 1e-3, "%s: protect=1 does not spare the summits (%.3f -> %.3f)", c.name, d0 / cnt, d1 / cnt);
  }
}

static void TestSolverExtras()
{
  SetThreads(4);
  // droplets on a real slope: with the old speed rule (accel 0) the speed fell to 0 on the first steps and (almost) nothing was dug
  {
    const GridPtr slope = Hillside(129, 5);
    const GridPtr fixedRule = Make("erode_droplet", "density=2,seed=3", {{"a", slope}});
    const GridPtr oldRule = Make("erode_droplet", "density=2,seed=3,accel=0", {{"a", slope}});
    double mf = 0.0, mo = 0.0;
    RegionalVariation(*slope, *fixedRule, &mf);
    RegionalVariation(*slope, *oldRule, &mo);
    std::printf("droplet speed rule: mean change accel=1 %.4f, accel=0 %.4f\n", mf, mo);
    CHECK(mf > 20.0 * mo && mf > 0.05, "droplets still dig (almost) nothing on a slope: %.4f vs the old rule %.4f", mf, mo);
  }
  // the local brush with hard / soft ground: same pattern for every dab, differs from the plain brush
  {
    const int n = 97;
    const GridPtr B = Mountains(n, 30);
    std::vector<std::uint16_t> words(B->Count());
    for (std::size_t i = 0; i < words.size(); ++i) words[i] = static_cast<std::uint16_t>(std::floor(B->v[i] * 128.0f + 0.5f));
    auto stroke = [&](const float vary) {
      er::HydroParams hp;
      hp.rain = 0.012f;
      hp.erosion = 0.3f;
      hp.vary = vary;
      er::ErodeSession s;
      s.Begin(n, n, words.data(), hp);
      std::vector<std::uint16_t> cur = words;
      for (int k = 0; k < 8; ++k) {
        er::BrushDab d;
        d.cx = 30.0 + k * 5.0;
        d.cz = 48.0;
        d.radius = 12;
        d.iterations = 16;
        int x0, z0, x1, z1;
        std::vector<std::uint16_t> w;
        if (s.Dab(d, x0, z0, x1, z1, w))
          for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) cur[static_cast<std::size_t>(z) * n + x] = w[static_cast<std::size_t>(z - z0) * (x1 - x0 + 1) + (x - x0)];
      }
      return cur;
    };
    const std::vector<std::uint16_t> plain = stroke(0.0f), varied = stroke(0.9f), again = stroke(0.9f);
    CHECK(varied == again, "the brush with variation is not repeatable");
    CHECK(varied != plain, "the brush variation changed nothing");
  }
}

static void Lab(const std::string& dir, const int n, const std::string& only, const std::string& extraParams)
{
  const GridPtr H = LabTerrain(n, 60.0 * n / 513.0);
  const Stats hs = Measure(*H);
  std::printf("terrain %dx%d  min %.1f max %.1f mean %.1f sd %.1f\n", n, n, hs.min, hs.max, hs.mean, hs.sd);
  DumpRelief(dir + "/base.png", *H, hs.min, hs.max);
  for (const Preset& p : kPresets) {
    if (!only.empty() && only != p.name) continue;
    std::map<std::string, Grid> extra;
    const double t0 = Now();
    const GridPtr r = Make(p.op, (std::string(p.params) + (extraParams.empty() ? "" : "," + extraParams)).c_str(), {{"a", H}}, &extra);
    const double dt = Now() - t0;
    const Stats s = Measure(*r);
    std::printf("%-10s %.3f s   min %.1f max %.1f mean %.2f sd %.2f   wear %.3g deposit %.3g\n", p.name, dt, s.min, s.max, s.mean, s.sd, Measure(extra["wear"]).mean,
                Measure(extra["deposit"]).mean);
    {
      std::size_t im = 0;
      for (std::size_t i = 0; i < r->v.size(); ++i) if (r->v[i] < r->v[im]) im = i;
      const int mx = static_cast<int>(im % r->w), mz = static_cast<int>(im / r->w);
      std::printf("   lowest at (%d,%d) = %.2f, was %.2f%c", mx, mz, r->v[im], H->v[im], 10);
    }
    DumpRelief(dir + "/" + p.name + ".png", *r, hs.min, hs.max);
    DumpMask(dir + "/" + p.name + "_wear.png", extra["wear"]);
    DumpMask(dir + "/" + p.name + "_deposit.png", extra["deposit"]);
    DumpMask(dir + "/" + p.name + "_flow.png", extra["flow"], -1.0f, true);
  }
}


/** How well does a coarse run (every step-th sample, parameters scaled like erosion.lua's OpParams) predict the full resolution run? Prints the correlation of the height changes. */
static void ScaleStudy(const std::string& name, const int n, const int step, const std::string& extra)
{
  const GridPtr H = LabTerrain(n, 60.0 * n / 513.0);
  const Preset* pr = nullptr;
  for (const Preset& p : kPresets) if (name == p.name) pr = &p;
  if (pr == nullptr) { std::printf("no such preset%c", 10); return; }
  const GridPtr full = Make(pr->op, pr->params, {{"a", H}});
  // coarse terrain
  const int cn = (n - 1) / step + 1;
  auto coarse = std::make_shared<Grid>(cn, cn, static_cast<float>(step), 0.0f, 0.0f);
  for (int z = 0; z < cn; ++z) for (int x = 0; x < cn; ++x) coarse->At(x, z) = H->At(x * step, z * step);
  std::string params = pr->params;
  // scaled parameters
  Params parsed;
  ParseParams(params, parsed);
  auto scaled = [&](const char* key, double def, double div, double lo) {
    const double v = parsed.num.count(key) ? parsed.num[key] : def;
    char b[64];
    std::snprintf(b, sizeof(b), ",%s=%g", key, std::max(lo, std::floor(v / div + 0.5)));
    params += b;
  };
  if (std::string(pr->op) == "erode_hydraulic") scaled("iterations", 200, step, 10);
  if (std::string(pr->op) == "erode_droplet") { scaled("lifetime", 30, step, 5); scaled("radius", 3, step, 1); }
  if (!extra.empty()) params += "," + extra;
  const GridPtr cr = Make(pr->op, params, {{"a", coarse}});
  // coarse diff -> full resolution by bilinear resize
  auto diffc = std::make_shared<Grid>(*cr);
  for (std::size_t i = 0; i < diffc->v.size(); ++i) diffc->v[i] = cr->v[i] - coarse->v[i];
  const GridPtr up = Make("resize", ("width=" + std::to_string(n) + ",height=" + std::to_string(n)).c_str(), {{"a", diffc}});
  double sf = 0, su = 0, sff = 0, suu = 0, sfu = 0;
  const std::size_t cnt = H->v.size();
  for (std::size_t i = 0; i < cnt; ++i) {
    const double df = full->v[i] - H->v[i], du = up->v[i];
    sf += df; su += du; sff += df * df; suu += du * du; sfu += df * du;
  }
  const double mf = sf / cnt, mu = su / cnt;
  const double vf = sff / cnt - mf * mf, vu = suu / cnt - mu * mu, cv = sfu / cnt - mf * mu;
  std::printf("scale study %s n=%d step=%d: full diff mean %.3f rms %.3f | coarse diff mean %.3f rms %.3f | correlation %.3f%c", pr->name, n, step, mf, std::sqrt(sff / cnt), mu,
              std::sqrt(suu / cnt), cv / std::sqrt(std::max(1e-12, vf * vu)), 10);
}

int main(const int argc, char** argv)
{
  bool update = false, bench = false;
  std::string labDir;
  int labSize = 257;
  std::string labOnly, labParams;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--update") update = true;
    else if (a == "--bench") bench = true;
    else if (a == "--scale" && i + 3 < argc) {
      ScaleStudy(argv[i + 1], std::atoi(argv[i + 2]), std::atoi(argv[i + 3]), i + 4 < argc ? argv[i + 4] : "");
      return 0;
    }
    else if (a == "--lab" && i + 1 < argc) {
      labDir = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-') labSize = std::atoi(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] != '-') labOnly = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-') labParams = argv[++i];
    }
  }
  if (!labDir.empty()) {
    Lab(labDir, labSize, labOnly, labParams);
    return 0;
  }
  if (bench) {
    const GridPtr H = Mountains(1025, 3);
    for (const Preset& p : kPresets) {
      const double t0 = Now();
      std::map<std::string, Grid> extra;
      Make(p.op, p.params, {{"a", H}}, &extra);
      std::printf("bench %-10s %.3f s (%d threads)\n", p.name, Now() - t0, Threads());
    }
    return 0;
  }
  if (argc < 2 || !LoadCases(argv[1])) {
    std::printf("cannot read the cases file\n");
    return 1;
  }
  TestGoldens(update);
  TestProperties();
  TestBoundary();
  TestBrushBoundary();
  TestVariation();
  TestSolverExtras();
  if (update && gFailures == 0) {
    std::ofstream out(argv[1], std::ios::binary);
    for (const std::string& l : gLines) out << l << "\n";
  }
  std::printf("%s (%d failures)\n", gFailures == 0 ? "EdErosionTest PASS" : "EdErosionTest FAIL", gFailures);
  return gFailures == 0 ? 0 : 1;
}
