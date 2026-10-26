// Contact sheets of the T7 ops (included by EdT7Test.cpp): `EdT7Test.exe <cases.lua> --lab <dir> [size]` writes gen.png, filt.png, ero.png, ana.png.
// Heights are drawn as shaded relief, masks as grey values; the labels are listed in <dir>/<sheet>.txt (row by row).
#include <cstdint>
#include <fstream>

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

static void PngChunk(std::vector<std::uint8_t>& o, const char* type, const std::vector<std::uint8_t>& data)
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
  PngChunk(out, "IHDR", ihdr);
  PngChunk(out, "IDAT", z);
  PngChunk(out, "IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

struct Sheet
{
  int tile = 200, cols = 6, count = 0;
  std::vector<std::uint8_t> rgb;
  std::vector<std::string> labels;
  Sheet(const int t, const int c, const int rows) : tile(t), cols(c), rgb(static_cast<std::size_t>(t) * c * t * rows * 3, 20) {}
  int Width() const { return tile * cols; }
  int Height() const { return static_cast<int>(rgb.size() / 3) / Width(); }
  void Put(const int ox, const int oy, const int x, const int y, const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
  {
    const std::size_t o = (static_cast<std::size_t>(oy + y) * Width() + ox + x) * 3;
    if (o + 2 < rgb.size()) { rgb[o] = r; rgb[o + 1] = g; rgb[o + 2] = b; }
  }
  /** kind 0 = shaded relief of heights (lo..hi tint), 1 = grey mask lo..hi, 2 = colour (needs g, b grids) */
  void Add(const char* label, const Grid& g, const int kind, float lo, float hi, const Grid* gg = nullptr, const Grid* bb = nullptr)
  {
    const int ox = (count % cols) * tile, oy = (count / cols) * tile;
    ++count;
    labels.push_back(label);
    if (hi <= lo) { const Stats s = Measure(g); lo = s.min; hi = s.max > s.min ? s.max : s.min + 1.0f; }
    for (int y = 0; y < tile; ++y) {
      for (int x = 0; x < tile; ++x) {
        const int sx = x * g.w / tile, sy = y * g.h / tile;
        const float v = g.At(sx, sy);
        const float t = std::max(0.0f, std::min(1.0f, (v - lo) / (hi - lo)));
        if (kind == 1) {
          const std::uint8_t c = static_cast<std::uint8_t>(255.0f * t);
          Put(ox, oy, x, y, c, c, c);
        } else if (kind == 2 && gg && bb) {
          Put(ox, oy, x, y, static_cast<std::uint8_t>(255.0f * std::max(0.0f, std::min(1.0f, v))), static_cast<std::uint8_t>(255.0f * std::max(0.0f, std::min(1.0f, gg->At(sx, sy)))),
              static_cast<std::uint8_t>(255.0f * std::max(0.0f, std::min(1.0f, bb->At(sx, sy)))));
        } else {
          const auto H = [&](const int i, const int j) { return g.At(std::max(0, std::min(g.w - 1, i)), std::max(0, std::min(g.h - 1, j))); };
          const float gx = (H(sx + 1, sy) - H(sx - 1, sy)) / (2.0f * g.cell);
          const float gz = (H(sx, sy + 1) - H(sx, sy - 1)) / (2.0f * g.cell);
          const float shade = std::max(0.15f, std::min(1.2f, 0.7f + (-gx * 0.7f - gz * 0.7f) * 1.8f));
          const float r = (0.25f + 0.55f * t) * shade, gc = (0.35f + 0.45f * t) * shade, b = (0.2f + 0.7f * t * t) * shade;
          Put(ox, oy, x, y, static_cast<std::uint8_t>(255.0f * std::min(1.0f, r * 1.3f)), static_cast<std::uint8_t>(255.0f * std::min(1.0f, gc * 1.3f)),
              static_cast<std::uint8_t>(255.0f * std::min(1.0f, b * 1.3f)));
        }
      }
    }
  }
  void Save(const std::string& dir, const std::string& name)
  {
    WritePng(dir + "/" + name + ".png", Width(), Height(), rgb);
    std::ofstream f(dir + "/" + name + ".txt");
    for (std::size_t i = 0; i < labels.size(); ++i) f << labels[i] << ((i + 1) % cols == 0 ? "\n" : " | ");
  }
};

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

static void Lab(const std::string& dir, const int n)
{
  const std::string sz = "width=" + std::to_string(n) + ",height=" + std::to_string(n) + ",";
  const GridPtr H = LabTerrain(n, 35.0);
  const std::map<std::string, GridPtr> hh = {{"a", H}};
  const Stats hs = Measure(*H);
  const auto G = [&](const std::string& op, const std::string& params, std::map<std::string, GridPtr> in = {}) { return Make(op, params, in); };
  const auto Sc = [&](const GridPtr& g) { return Make("affine", "mul=45,add=0", {{"a", g}}); };
  {
    Sheet s(200, 6, 5);
    s.Add("musgrave ridged", *G("musgrave", sz + "seed=3,scale=110,octaves=7"), 0, 0.0f, 0.5f);
    s.Add("musgrave hybrid", *G("musgrave", sz + "seed=3,scale=110,octaves=7,mode=hybrid,offset=0.7"), 0, 0.0f, 0.0f);
    s.Add("musgrave hetero", *G("musgrave", sz + "seed=3,scale=110,octaves=7,mode=hetero,offset=0.7"), 0, 0.0f, 0.0f);
    s.Add("fbm_damped", *G("fbm_damped", sz + "seed=5,scale=120,octaves=8"), 0, 0.0f, 1.0f);
    s.Add("perlin ref", *G("perlin", sz + "seed=5,scale=120,octaves=8"), 0, 0.0f, 1.0f);
    s.Add("cells f1", *G("cells", sz + "seed=2,scale=60"), 1, 0, 1);
    s.Add("cells f2", *G("cells", sz + "seed=2,scale=60,mode=f2"), 1, 0, 1);
    s.Add("cells f3", *G("cells", sz + "seed=2,scale=60,mode=f3"), 1, 0, 1);
    s.Add("cells edge", *G("cells", sz + "seed=2,scale=60,mode=edge"), 1, 0, 0.4f);
    s.Add("cells smooth", *G("cells", sz + "seed=2,scale=60,mode=smooth,smooth=6"), 1, 0, 1);
    s.Add("cells border", *G("cells", sz + "seed=2,scale=60,mode=border"), 1, 0, 1);
    s.Add("cells plate", *G("cells", sz + "seed=2,scale=60,mode=plate,bevel=0.15,drop=0.4"), 0, 0, 1);
    s.Add("cells facet", *G("cells", sz + "seed=2,scale=60,mode=facet"), 0, 0, 1);
    s.Add("cells manhattan", *G("cells", sz + "seed=2,scale=60,metric=manhattan"), 1, 0, 1);
    s.Add("cells chebyshev", *G("cells", sz + "seed=2,scale=60,metric=chebyshev,mode=f2"), 1, 0, 1);
    s.Add("cells minkowski", *G("cells", sz + "seed=2,scale=60,metric=minkowski,p=0.6"), 1, 0, 1);
    s.Add("cells warped id", *G("cells", sz + "seed=2,scale=50,mode=id,warp=30,warp_scale=80"), 1, 0, 1);
    s.Add("craters", *G("craters", sz + "seed=4,scale=70,density=0.8"), 0, 0, 1);
    s.Add("dunes", *G("dunes", sz + "seed=4,wavelength=45,angle=20"), 0, 0, 1);
    s.Add("dunes sharp", *G("dunes", sz + "seed=4,wavelength=45,angle=70,asym=1.4,ripples=0.3"), 0, 0, 1);
    s.Add("cracks", *G("cracks", sz + "seed=4,scale=70,crack_width=3"), 1, 0, 1);
    s.Add("cracks u", *G("cracks", sz + "seed=4,scale=90,crack_width=5,profile=u,levels=3"), 1, 0, 1);
    s.Add("island height", *G("island", sz + "seed=4"), 0, 0, 1);
    s.Add("island mask", *G("island", sz + "seed=4,output=mask,aspect=1.5,angle=30"), 1, 0, 1);
    s.Add("island rough", *G("island", sz + "seed=9,roughness=0.8,radius=100"), 0, 0, 1);
    s.Add("dwarp fbm", *G("dwarp", "strength=40,size=100", {{"a", Sc(G("voronoi", sz + "seed=2,scale=50,mode=edge"))}}), 1, 0, 0);
    s.Add("dwarp fbm x2", *G("dwarp", "strength=40,size=100,iterations=2", {{"a", Sc(G("voronoi", sz + "seed=2,scale=50,mode=edge"))}}), 1, 0, 0);
    s.Add("dwarp curl", *G("dwarp", "kind=curl,strength=60,size=100", {{"a", Sc(G("voronoi", sz + "seed=2,scale=50,mode=edge"))}}), 1, 0, 0);
    s.Add("dwarp swirl", *G("dwarp", "kind=swirl,strength=120,size=60", {{"a", Sc(G("voronoi", sz + "seed=2,scale=50,mode=edge"))}}), 1, 0, 0);
    s.Add("dwarp ripple", *G("dwarp", "kind=ripple,strength=8,wavelength=40,angle=30", {{"a", Sc(G("voronoi", sz + "seed=2,scale=50,mode=edge"))}}), 1, 0, 0);
    s.Save(dir, "gen");
  }
  {
    Sheet s(200, 6, 5);
    s.Add("terrain", *H, 0, hs.min, hs.max);
    s.Add("bilateral", *G("bilateral", "radius=6,sigma_r=1.5", hh), 0, hs.min, hs.max);
    s.Add("diffuse", *G("diffuse", "iterations=60,kappa=0.25", hh), 0, hs.min, hs.max);
    s.Add("unsharp", *G("unsharp", "radius=4,amount=1.5", hh), 0, hs.min, hs.max);
    s.Add("edge sobel", *G("edge", "mode=sobel,gain=1.5", hh), 1, 0, 1);
    s.Add("edge laplace", *G("edge", "mode=laplace,gain=20,smooth=2", hh), 1, 0, 1);
    s.Add("edge dog", *G("edge", "mode=dog,radius=2,gain=2", hh), 1, 0, 1);
    s.Add("dilate disc", *G("morph", "op=dilate,radius=6,shape=disc", hh), 0, hs.min, hs.max);
    s.Add("erode square", *G("morph", "op=erode,radius=5", hh), 0, hs.min, hs.max);
    s.Add("open", *G("morph", "op=open,radius=5,shape=disc", hh), 0, hs.min, hs.max);
    s.Add("close", *G("morph", "op=close,radius=5,shape=disc", hh), 0, hs.min, hs.max);
    s.Add("gradient", *G("morph", "op=gradient,radius=2,shape=disc", hh), 1, 0, 0);
    s.Add("tophat", *G("morph", "op=tophat,radius=8,shape=disc", hh), 1, 0, 0);
    s.Add("convolve emboss", *G("convolve", "kernel=-2:-1:0;-1:1:1;0:1:2,normalize=0,bias=20", hh), 0, 0, 0);
    s.Add("blur_dir", *G("blur_dir", "angle=30,length=40,shape=tri", hh), 0, hs.min, hs.max);
    s.Add("blur_slope flat", *G("blur_slope", "radius=8,lo=0.2,hi=0.5", hh), 0, hs.min, hs.max);
    s.Add("blur_slope steep", *G("blur_slope", "radius=8,lo=0.2,hi=0.5,mode=steep", hh), 0, hs.min, hs.max);
    s.Add("stratify", *G("stratify", "thickness=2.5,variation=0.5,warp=2,tilt_x=0.02,sharpness=0.8", hh), 0, hs.min, hs.max);
    s.Add("stratify slopes", *G("stratify", "thickness=3,slope_lo=0.2,slope_hi=0.5,sharpness=0.9", hh), 0, hs.min, hs.max);
    s.Save(dir, "filt");
  }
  {
    Sheet s(200, 6, 5);
    s.Add("terrain", *H, 0, hs.min, hs.max);
    std::map<std::string, Grid> ex;
    const GridPtr t = Make("thermal_map", "iterations=40,talus_lo=0.25,talus_hi=1.0", {{"a", H}, {"b", G("perlin", sz + "seed=3,scale=60,octaves=3")}}, nullptr, &ex);
    s.Add("thermal_map", *t, 0, hs.min, hs.max);
    s.Add("  scree", ex["scree"], 1, 0, 0.5f);
    ex.clear();
    const GridPtr rs = Make("erode_rocksoil", "iterations=80,weather=0.02,soil=0.6", hh, nullptr, &ex);
    s.Add("rocksoil", *rs, 0, hs.min, hs.max);
    s.Add("  soil", ex["soil"], 1, 0, 3);
    s.Add("  rock", ex["rock"], 1, 0, 1);
    ex.clear();
    const GridPtr sn = Make("erode_snow", "snowline=30,band=8,amount=3,iterations=40", hh, nullptr, &ex);
    s.Add("snow", *sn, 0, hs.min, hs.max + 4);
    s.Add("  cover", ex["cover"], 1, 0, 1);
    ex.clear();
    const GridPtr wi = Make("erode_wind", "iterations=60,azimuth=20,sand=0.8,abrade=0.03", hh, nullptr, &ex);
    s.Add("wind", *wi, 0, hs.min, hs.max);
    s.Add("  sand", ex["sand"], 1, 0, 2);
    s.Add("  abrasion", ex["abrasion"], 1, 0, 0.5f);
    ex.clear();
    const GridPtr gl = Make("erode_glacier", "iterations=300,ela=25", hh, nullptr, &ex);
    s.Add("glacier", *gl, 0, hs.min, hs.max);
    s.Add("  ice", ex["ice"], 1, 0, 0);
    s.Add("  carve", ex["carve"], 1, 0, 0);
    ex.clear();
    const GridPtr se = Make("settle", "iterations=30,rate=0.5,cap=1", hh, nullptr, &ex);
    s.Add("settle", *se, 0, hs.min, hs.max);
    s.Add("  deposit", ex["deposit"], 1, 0, 1);
    s.Save(dir, "ero");
  }
  {
    Sheet s(200, 6, 4);
    s.Add("terrain", *H, 0, hs.min, hs.max);
    s.Add("occ cavity", *G("occlusion", "radius=30,gain=2", hh), 1, 0, 1);
    s.Add("occ convex", *G("occlusion", "radius=30,mode=convex,gain=2", hh), 1, 0, 1);
    s.Add("occ both", *G("occlusion", "radius=30,mode=both,gain=2", hh), 1, 0, 1);
    s.Add("hillshade", *G("hillshade", "azimuth=225,altitude=35", hh), 1, 0, 1);
    s.Add("hillshade shadows", *G("hillshade", "azimuth=225,altitude=25,shadows=1", hh), 1, 0, 1);
    s.Add("lit", *G("hillshade", "mode=lit,azimuth=225,altitude=30", hh), 1, 0, 1);
    s.Add("shadow", *G("hillshade", "mode=shadow,azimuth=225,altitude=20", hh), 1, 0, 1);
    s.Add("wetness", *G("wetness", "", hh), 1, 0, 1);
    s.Add("twi", *G("wetness", "mode=twi", hh), 1, 0, 0);
    s.Add("catchment", *G("wetness", "mode=area", hh), 1, 0, 1);
    s.Add("rockiness", *G("rockiness", "", hh), 1, 0, 1);
    std::map<std::string, Grid> ex;
    const GridPtr cr = Make("color_ramp", "stops=0:0.1:0.3:0.7:1;0.35:0.8:0.8:0.5:1;0.6:0.3:0.6:0.2:1;1:1:1:1:1,lo=0,hi=1", {{"a", Make("normalize", "lo=0,hi=1", hh)}}, nullptr, &ex);
    s.Add("color ramp", *cr, 2, 0, 1, &ex["g"], &ex["b"]);
    s.Save(dir, "ana");
  }
  std::printf("lab written to %s\n", dir.c_str());
}
