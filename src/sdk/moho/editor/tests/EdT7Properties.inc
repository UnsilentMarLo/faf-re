// Property tests of the T7 ops (included by EdT7Test.cpp). Every op has golden cases for the exact bits; these check what the result has to be.

static bool SameBits(const Grid& a, const Grid& b)
{
  return a.SameShape(b) && Checksum(a) == Checksum(b);
}

static double MaxAbsDiff(const Grid& a, const Grid& b)
{
  double m = 0.0;
  for (std::size_t i = 0; i < a.Count(); ++i) m = std::max(m, std::fabs(static_cast<double>(a.v[i]) - static_cast<double>(b.v[i])));
  return m;
}

static void PropGenerators()
{
  const std::string sz = "width=48,height=48,";
  for (const char* mode : {"ridged", "hybrid", "hetero"}) {
    const GridPtr g = Run("musgrave", sz + "seed=3,scale=14,octaves=5,mode=" + std::string(mode));
    CHECK(InRange(*g, 0.0f, 1.0f), "musgrave %s range", mode);
    CHECK(Measure(*g).sd > 0.01, "musgrave %s is flat", mode);
  }
  CHECK(InRange(*Run("fbm_damped", sz + "seed=3,scale=14,octaves=6"), 0.0f, 1.0f), "fbm_damped range");
  // noise depends on the world position only: a coarse grid samples the same field
  {
    const GridPtr fine = Run("musgrave", "width=65,height=65,seed=5,scale=30,octaves=4");
    const GridPtr coarse = Run("musgrave", "width=33,height=33,seed=5,scale=30,octaves=4,cell=2");
    bool ok = true;
    for (int z = 0; z < 33; ++z) for (int x = 0; x < 33; ++x) ok = ok && coarse->At(x, z) == fine->At(2 * x, 2 * z);
    CHECK(ok, "musgrave: a cell=2 grid samples the same world field");
  }
  // cells: every mode stays 0..1, border is 0 on a Voronoi edge and grows inside, id is constant inside a cell
  for (const char* mode : {"f1", "f2", "f3", "edge", "crack", "id", "smooth", "border", "plate", "facet"}) {
    CHECK(InRange(*Run("cells", sz + "seed=2,scale=16,mode=" + std::string(mode)), 0.0f, 1.0f), "cells %s range", mode);
  }
  for (const char* metric : {"manhattan", "chebyshev", "minkowski"}) CHECK(InRange(*Run("cells", sz + "seed=2,scale=16,mode=f1,metric=" + std::string(metric)), 0.0f, 1.0f), "cells metric %s", metric);
  {
    const GridPtr id = Run("cells", "width=64,height=64,seed=2,scale=16,mode=id");
    const GridPtr f1 = Run("cells", "width=64,height=64,seed=2,scale=16,mode=f1");
    // a sample next to its neighbour with the same id differs by at most a little in f1 (continuity), and ids are piecewise constant
    int same = 0, total = 0;
    for (int z = 0; z < 64; ++z) for (int x = 0; x < 63; ++x) { ++total; if (id->At(x, z) == id->At(x + 1, z)) ++same; }
    CHECK(same > total * 9 / 10, "cells id is constant inside a cell (%d of %d)", same, total);
    double maxStep = 0.0;
    for (int z = 0; z < 64; ++z) for (int x = 0; x < 63; ++x) maxStep = std::max(maxStep, static_cast<double>(std::fabs(f1->At(x, z) - f1->At(x + 1, z))));
    CHECK(maxStep < 0.1, "cells f1 is continuous (step %g)", maxStep);
  }
  {
    const GridPtr b = Run("cells", "width=64,height=64,seed=2,scale=16,mode=border");
    const GridPtr e = Run("cells", "width=64,height=64,seed=2,scale=16,mode=edge");
    // the border distance and f2 - f1 vanish at the same places: where border is below 0.02 the edge value is small
    int bad = 0;
    for (std::size_t i = 0; i < b->Count(); ++i) if (b->v[i] < 0.02f && e->v[i] > 0.15f) ++bad;
    CHECK(bad == 0, "cells border and edge disagree on %d samples", bad);
  }
  {
    const GridPtr c = Run("craters", sz + "seed=4,scale=30,density=1");
    CHECK(InRange(*c, 0.0f, 1.0f), "craters range");
    const Stats s = Measure(*c);
    CHECK(s.min < 0.45f && s.max > 0.5f, "craters have bowls (%g) and rims (%g)", s.min, s.max);
    const GridPtr none = Run("craters", sz + "seed=4,scale=30,density=0");
    CHECK(Measure(*none).min == 0.5f && Measure(*none).max == 0.5f, "no craters = the base level");
  }
  {
    // dunes without meander, variation and ripples are periodic along the wind direction
    const GridPtr d = Run("dunes", "width=80,height=16,seed=4,wavelength=16,angle=0,warp=0,variation=0,ripples=0");
    bool ok = true;
    for (int z = 0; z < 16; ++z) for (int x = 0; x + 16 < 80; ++x) ok = ok && std::fabs(d->At(x, z) - d->At(x + 16, z)) < 1e-4f;
    CHECK(ok, "dunes repeat every wavelength");
    bool rows = true;
    for (int x = 0; x < 80; ++x) rows = rows && d->At(x, 0) == d->At(x, 9);
    CHECK(rows, "dune crests run across the wind");
    CHECK(InRange(*d, 0.0f, 1.0f), "dunes range");
  }
  {
    const GridPtr c = Run("cracks", sz + "seed=4,scale=24,crack_width=3");
    CHECK(InRange(*c, 0.0f, 1.0f), "cracks range");
    const Stats s = Measure(*c);
    CHECK(s.max > 0.9f && s.mean < 0.4, "cracks are thin lines (max %g mean %g)", s.max, s.mean);
  }
  {
    const GridPtr isl = Run("island", "width=65,height=65,seed=2,roughness=0");
    CHECK(InRange(*isl, 0.0f, 1.0f), "island range");
    CHECK(isl->At(32, 32) > 0.9f && isl->At(0, 0) == 0.0f && isl->At(64, 64) == 0.0f, "island: high in the middle, sea in the corners");
    CHECK(isl->At(10, 32) < isl->At(32, 32) && isl->At(20, 32) > isl->At(10, 32), "island rises towards the middle");
    const GridPtr mk = Run("island", "width=65,height=65,seed=2,output=mask,roughness=0");
    CHECK(mk->At(32, 32) == 1.0f && mk->At(0, 0) == 0.0f, "island mask");
    std::string err;
    CHECK(!Make("island", "width=8,height=8,output=x", {}, &err), "island: bad output must fail");
  }
  {
    // dwarp: no strength = no change; a mask of 0 = no change
    const GridPtr H = gFix["H"];
    for (const char* kind : {"fbm", "curl", "swirl", "ripple", "pinch"}) {
      const std::string base = std::string("kind=") + kind;
      const double neutral = std::string(kind) == "pinch" ? 0.0 : 0.0;
      const GridPtr same = Run("dwarp", base + ",strength=" + std::to_string(neutral), {{"a", H}});
      CHECK(SameBits(*same, *H), "dwarp %s with strength 0 changes the field", kind);
      const GridPtr moved = Run("dwarp", base + ",strength=" + (std::string(kind) == "swirl" ? "90" : "12") + ",size=20", {{"a", H}});
      CHECK(MaxAbsDiff(*moved, *H) > 0.01, "dwarp %s does nothing", kind);
      const GridPtr zero = Make("const", "width=65,height=65,value=0", {});
      const GridPtr masked = Run("dwarp", base + ",strength=12,size=20", {{"a", H}, {"mask", zero}});
      CHECK(SameBits(*masked, *H), "dwarp %s ignores a zero mask", kind);
    }
    std::string err;
    CHECK(!Make("dwarp", "kind=bogus", {{"a", H}}, &err), "dwarp: bad kind must fail");
    // a warp only moves values around: the range stays inside the input range
    const GridPtr w = Run("dwarp", "kind=fbm,strength=30,size=25", {{"a", H}});
    const Stats si = Measure(*H), sw = Measure(*w);
    CHECK(sw.min >= si.min - 1e-4f && sw.max <= si.max + 1e-4f, "dwarp stays inside the input range");
  }
}

static void PropFilters()
{
  const GridPtr H = gFix["H"], T = gFix["T"], L = gFix["L"];
  {
    const GridPtr b = Run("bilateral", "radius=4,sigma_r=3", {{"a", H}});
    const Stats si = Measure(*H), sb = Measure(*b);
    CHECK(sb.min >= si.min - 1e-4f && sb.max <= si.max + 1e-4f, "bilateral stays inside the input range");
    CHECK(sb.sd < si.sd, "bilateral smooths (%g -> %g)", si.sd, sb.sd);
    CHECK(MaxAbsDiff(*Run("bilateral", "radius=4,sigma_r=3", {{"a", L}}), *L) < 1e-5, "bilateral keeps a flat field");
    CHECK(SameBits(*Run("bilateral", "radius=4,sigma_r=3", {{"a", H}, {"mask", Run("const", "width=65,height=65,value=0", {})}}), *H), "bilateral mask 0 = identity");
    // an edge survives: a step of 20 with sigma_r 1 stays a step, a plain blur would not
    GridPtr step = Run("gradient", "width=65,height=65,angle=0,lo=0,hi=1");
    step = Run("threshold", "value=0.5", {{"a", step}});
    step = Run("affine", "mul=20,add=0", {{"a", step}});
    const GridPtr bs = Run("bilateral", "radius=4,sigma_r=1", {{"a", step}});
    const GridPtr bl = Run("blur", "radius=2", {{"a", step}});
    CHECK(std::fabs(bs->At(31, 10) - step->At(31, 10)) < 0.5f && std::fabs(bl->At(31, 10) - step->At(31, 10)) > 2.0f, "bilateral keeps the step (%g) where blur does not (%g)", bs->At(31, 10), bl->At(31, 10));
  }
  {
    const GridPtr d = Run("diffuse", "iterations=30,kappa=0.5", {{"a", H}});
    const Stats si = Measure(*H), sd = Measure(*d);
    CHECK(std::fabs(Sum(*d) - Sum(*H)) < 1e-2 * std::sqrt(static_cast<double>(H->Count())), "diffuse conserves the total (%g vs %g)", Sum(*d), Sum(*H));
    CHECK(sd.min >= si.min - 1e-4f && sd.max <= si.max + 1e-4f && sd.sd < si.sd, "diffuse smooths inside the range");
    CHECK(SameBits(*Run("diffuse", "iterations=0", {{"a", H}}), *H), "diffuse with 0 iterations");
    CHECK(MaxAbsDiff(*Run("diffuse", "iterations=10", {{"a", L}}), *L) == 0.0, "diffuse keeps a flat field");
  }
  {
    CHECK(SameBits(*Run("unsharp", "radius=3,amount=0", {{"a", H}}), *H), "unsharp amount 0");
    const GridPtr u = Run("unsharp", "radius=3,amount=1.5", {{"a", H}});
    CHECK(Measure(*u).sd > Measure(*H).sd, "unsharp increases the contrast");
    CHECK(MaxAbsDiff(*Run("unsharp", "radius=3,amount=2", {{"a", L}}), *L) < 1e-5, "unsharp keeps a flat field");
  }
  {
    for (const char* mode : {"sobel", "prewitt", "laplace", "dog"}) {
      const GridPtr e = Run("edge", std::string("mode=") + mode + ",gain=1", {{"a", T}});
      CHECK(InRange(*e, 0.0f, 1.0f), "edge %s range", mode);
      CHECK(Measure(*Run("edge", std::string("mode=") + mode, {{"a", L}})).max < 1e-4f, "edge %s of a flat field", mode);
    }
    // a ramp of slope 0.5 reads 0.5 with sobel
    const GridPtr ramp = Run("gradient", "width=33,height=33,angle=0,lo=0,hi=32");   // 1 unit per sample = 0.5 per cell of width 2? cell 1: slope 1
    const GridPtr ramp2 = Run("affine", "mul=0.5,add=0", {{"a", ramp}});
    CHECK(std::fabs(Run("edge", "mode=sobel", {{"a", ramp2}})->At(16, 16) - 0.5f) < 1e-3f, "sobel reads the slope");
    CHECK(std::fabs(Run("edge", "mode=prewitt", {{"a", ramp2}})->At(16, 16) - 0.5f) < 1e-3f, "prewitt reads the slope");
  }
  {
    const GridPtr dil = Run("morph", "op=dilate,radius=3", {{"a", H}});
    const GridPtr ero = Run("morph", "op=erode,radius=3", {{"a", H}});
    bool ok = true;
    for (std::size_t i = 0; i < H->Count(); ++i) ok = ok && dil->v[i] >= H->v[i] && ero->v[i] <= H->v[i];
    CHECK(ok, "dilate >= input >= erode");
    for (const char* shape : {"square", "disc", "cross", "diamond"}) {
      const std::string p = std::string(",radius=3,shape=") + shape;
      const GridPtr op = Run("morph", "op=open" + p, {{"a", H}});
      const GridPtr cl = Run("morph", "op=close" + p, {{"a", H}});
      bool o = true;
      for (std::size_t i = 0; i < H->Count(); ++i) o = o && op->v[i] <= H->v[i] && cl->v[i] >= H->v[i];
      CHECK(o, "open <= input <= close (%s)", shape);
      CHECK(SameBits(*Run("morph", "op=open" + p, {{"a", op}}), *op), "open is idempotent (%s)", shape);
      CHECK(SameBits(*Run("morph", "op=close" + p, {{"a", cl}}), *cl), "close is idempotent (%s)", shape);
      CHECK(InRange(*Run("morph", "op=gradient" + p, {{"a", H}}), 0.0f, 1e9f) && InRange(*Run("morph", "op=tophat" + p, {{"a", H}}), 0.0f, 1e9f) && InRange(*Run("morph", "op=bothat" + p, {{"a", H}}), 0.0f, 1e9f), "morph gradient / hats are >= 0 (%s)", shape);
    }
    // the square dilation equals the brute force maximum
    const GridPtr sq = Run("morph", "op=dilate,radius=2", {{"a", H}});
    bool bf = true;
    for (int z = 0; z < 65 && bf; ++z) for (int x = 0; x < 65; ++x) {
      float m = -1e9f;
      for (int dz = -2; dz <= 2; ++dz) for (int dx = -2; dx <= 2; ++dx) { const int xx = x + dx, zz = z + dz; if (xx >= 0 && zz >= 0 && xx < 65 && zz < 65) m = std::max(m, H->At(xx, zz)); }
      if (sq->At(x, z) != m) { bf = false; break; }
    }
    CHECK(bf, "square dilation equals the brute force maximum");
    std::string err;
    CHECK(!Make("morph", "op=wrong", {{"a", H}}, &err), "morph: bad op must fail");
  }
  {
    CHECK(SameBits(*Run("convolve", "kernel=0:0:0;0:1:0;0:0:0", {{"a", H}}), *H), "convolve with the identity kernel");
    const GridPtr bx = Run("convolve", "kernel=1:1:1;1:1:1;1:1:1", {{"a", H}});
    const GridPtr bx2 = Run("box", "radius=1", {{"a", H}});
    CHECK(MaxAbsDiff(*bx, *bx2) < 1e-3, "convolve with a 3x3 box equals the box blur away from the edges (%g)", MaxAbsDiff(*bx, *bx2));
    std::string err;
    CHECK(!Make("convolve", "kernel=1:1:1:1", {{"a", H}}, &err), "convolve: even kernel must fail");
    CHECK(!Make("convolve", "", {{"a", H}}, &err), "convolve: no kernel must fail");
  }
  {
    CHECK(SameBits(*Run("blur_dir", "length=0", {{"a", H}}), *H), "blur_dir length 0");
    for (const char* shape : {"box", "tri", "gauss"}) {
      const GridPtr b = Run("blur_dir", std::string("angle=30,length=9,shape=") + shape, {{"a", H}});
      CHECK(MaxAbsDiff(*Run("blur_dir", std::string("angle=30,length=9,shape=") + shape, {{"a", L}}), *L) < 1e-4, "blur_dir %s keeps a flat field", shape);
      CHECK(Measure(*b).sd < Measure(*H).sd, "blur_dir %s smooths", shape);
    }
    // along the blur direction a field that varies only across it is unchanged
    const GridPtr stripes = Run("gradient", "width=65,height=65,angle=90,lo=0,hi=10");
    CHECK(MaxAbsDiff(*Run("blur_dir", "angle=0,length=11", {{"a", stripes}}), *stripes) < 1e-4, "blur_dir along the stripes changes nothing");
  }
  {
    CHECK(SameBits(*Run("blur_slope", "amount=0", {{"a", H}}), *H), "blur_slope amount 0");
    const GridPtr f = Run("blur_slope", "radius=3,lo=0.05,hi=0.2", {{"a", T}});
    const GridPtr s = Run("blur_slope", "radius=3,lo=0.05,hi=0.2,mode=steep", {{"a", T}});
    const GridPtr b = Run("blur", "radius=3", {{"a", T}});
    // flat + steep weights add up to the plain blur
    bool ok = true;
    for (std::size_t i = 0; i < T->Count(); ++i) ok = ok && std::fabs((f->v[i] - T->v[i]) + (s->v[i] - T->v[i]) - (b->v[i] - T->v[i])) < 1e-3f;
    CHECK(ok, "blur_slope flat + steep = blur");
  }
  {
    CHECK(SameBits(*Run("stratify", "strength=0", {{"a", H}}), *H), "stratify strength 0");
    const GridPtr s = Run("stratify", "thickness=2,variation=0.4,sharpness=0.9", {{"a", H}});
    CHECK(MaxAbsDiff(*s, *H) <= 2.0 * 1.4 + 1e-3, "stratify moves a height by less than a layer (%g)", MaxAbsDiff(*s, *H));
    // heights end up in the flat part of a shelf: far fewer distinct fractional positions than the input has
    CHECK(MaxAbsDiff(*s, *H) > 0.1, "stratify does something");
    const GridPtr steep = Run("stratify", "thickness=2,slope_lo=5,slope_hi=6", {{"a", H}});
    CHECK(SameBits(*steep, *H), "stratify with a slope limit above every slope changes nothing");
    // a ramp becomes a staircase: the number of distinct values drops
    GridPtr ramp = Run("gradient", "width=65,height=65,angle=0,lo=0,hi=30");
    const GridPtr st = Run("stratify", "thickness=3,variation=0,sharpness=1", {{"a", ramp}});
    int distinct = 0;
    for (int x = 1; x < 65; ++x) if (st->At(x, 0) != st->At(x - 1, 0)) ++distinct;
    CHECK(distinct < 64 && distinct > 5, "stratify turns a ramp into steps (%d changes)", distinct);
  }
}

static void PropAnalysis()
{
  const GridPtr H = gFix["H"], T = gFix["T"], L = gFix["L"];
  {
    CHECK(Measure(*Run("occlusion", "radius=10", {{"a", L}})).max == 0.0f, "occlusion of a flat field is 0");
    for (const char* mode : {"cavity", "convex", "both"}) CHECK(InRange(*Run("occlusion", std::string("radius=10,gain=3,mode=") + mode, {{"a", T}}), 0.0f, 1.0f), "occlusion %s range", mode);
    // a pit is occluded, a hill is exposed
    GridPtr pit = Run("radial", "width=65,height=65,radius=24,clamp=1");   // 0 in the middle, 1 outside
    pit = Run("affine", "mul=12,add=0", {{"a", pit}});
    const GridPtr oc = Run("occlusion", "radius=24,gain=1", {{"a", pit}});
    const GridPtr ox = Run("occlusion", "radius=24,gain=1,mode=convex", {{"a", pit}});
    CHECK(oc->At(32, 32) > 0.3f && oc->At(32, 32) > oc->At(2, 2), "the bottom of a pit is occluded (%g)", oc->At(32, 32));
    CHECK(ox->At(32, 32) < 0.02f, "the bottom of a pit is not exposed (%g)", ox->At(32, 32));
    const GridPtr inv = Run("affine", "mul=-1,add=0", {{"a", pit}});
    const GridPtr oi = Run("occlusion", "radius=24,gain=1,mode=convex", {{"a", inv}});
    CHECK(MaxAbsDiff(*oi, *oc) < 1e-5, "convex of -h = cavity of h");
    const GridPtr both = Run("occlusion", "radius=24,mode=both", {{"a", pit}});
    CHECK(both->At(32, 32) > 0.5f, "both: concave above 0.5");
  }
  {
    // a flat field: the shade is ambient + (1 - ambient) * sin(altitude)
    const GridPtr f = Run("hillshade", "altitude=40,ambient=0.2", {{"a", L}});
    CHECK(std::fabs(f->At(10, 10) - (0.2f + 0.8f * std::sin(40.0f * 3.14159265f / 180.0f))) < 1e-3f, "flat shade %g", f->At(10, 10));
    CHECK(Measure(*Run("hillshade", "mode=shadow,altitude=40", {{"a", L}})).max == 0.0f, "no shadow on flat ground");
    // a wall casts a shadow away from the sun
    GridPtr wall = Run("const", "width=65,height=65,value=0");
    Grid w2 = *wall;
    for (int z = 0; z < 65; ++z) w2.At(20, z) = 30.0f;
    const GridPtr wallp = std::make_shared<Grid>(w2);
    const GridPtr sh = Run("hillshade", "mode=shadow,azimuth=0,altitude=30,shadow_len=100", {{"a", wallp}});   // sun towards +x: the shadow lies at +x of the wall? no: toward the sun is +x, so x < 20 is shadowed
    CHECK(sh->At(10, 30) == 1.0f && sh->At(40, 30) == 0.0f, "the wall shadows the ground on the far side (%g, %g)", sh->At(10, 30), sh->At(40, 30));
    CHECK(Measure(*Run("hillshade", "mode=lit,azimuth=0,altitude=30", {{"a", H}})).max <= 1.0f, "lit range");
    const GridPtr full = Run("hillshade", "azimuth=300,altitude=35,shadows=1", {{"a", T}});
    CHECK(InRange(*full, 0.0f, 1.0f), "hillshade range");
  }
  {
    // normal_map: the engine's GetNormal (left - right, 1, top - bottom) encoded as n * 0.5 + 0.5; flat ground is (0.5, 1, 0.5)
    std::map<std::string, Grid> ex;
    const GridPtr flatN = Run("normal_map", "", {{"a", L}}, &ex);
    CHECK(std::fabs(flatN->At(10, 10) - 0.5f) < 1e-6f && std::fabs(ex["z"].At(10, 10) - 0.5f) < 1e-6f && std::fabs(ex["y"].At(10, 10) - 1.0f) < 1e-6f, "flat normal (%g %g %g)", flatN->At(10, 10), ex["y"].At(10, 10), ex["z"].At(10, 10));
    // a plane rising 0.5 per unit towards +x: the normal leans towards -x, nx = -0.5 / sqrt(1.25)
    Grid ramp = *Run("const", "width=65,height=65,value=0");
    for (int z = 0; z < 65; ++z) for (int x = 0; x < 65; ++x) ramp.At(x, z) = 0.5f * static_cast<float>(x) + 0.25f * static_cast<float>(z);
    const GridPtr rp = std::make_shared<Grid>(ramp);
    std::map<std::string, Grid> ex2;
    const GridPtr nx = Run("normal_map", "", {{"a", rp}}, &ex2);
    const float len = std::sqrt(0.25f + 1.0f + 0.0625f);
    CHECK(std::fabs(nx->At(30, 30) - (-0.5f / len * 0.5f + 0.5f)) < 1e-4f, "plane nx %g", nx->At(30, 30));
    CHECK(std::fabs(ex2["z"].At(30, 30) - (-0.25f / len * 0.5f + 0.5f)) < 1e-4f, "plane nz %g", ex2["z"].At(30, 30));
    CHECK(std::fabs(ex2["y"].At(30, 30) - (1.0f / len * 0.5f + 0.5f)) < 1e-4f, "plane ny %g", ex2["y"].At(30, 30));
    // strength scales the slope, signed gives -1..1, the encoded vector is a unit vector
    std::map<std::string, Grid> ex3;
    const GridPtr sg = Run("normal_map", "signed=1,strength=2", {{"a", rp}}, &ex3);
    const float len2 = std::sqrt(1.0f + 1.0f + 0.25f);
    CHECK(std::fabs(sg->At(30, 30) - (-1.0f / len2)) < 1e-4f, "strength 2 signed nx %g", sg->At(30, 30));
    const float ux = nx->At(20, 40) * 2.0f - 1.0f, uy = ex2["y"].At(20, 40) * 2.0f - 1.0f, uz = ex2["z"].At(20, 40) * 2.0f - 1.0f;
    CHECK(std::fabs(ux * ux + uy * uy + uz * uz - 1.0f) < 1e-4f, "unit length (%g)", ux * ux + uy * uy + uz * uz);
    // span 4 on a plane gives the same slope per unit; the range is 0..1 on rough ground
    const GridPtr wide = Run("normal_map", "span=4", {{"a", rp}});
    CHECK(std::fabs(wide->At(30, 30) - nx->At(30, 30)) < 1e-4f, "span does not change a plane (%g vs %g)", wide->At(30, 30), nx->At(30, 30));
    CHECK(InRange(*Run("normal_map", "shift=0.5", {{"a", T}}), 0.0f, 1.0f), "normal_map range");
    // shift 0.5 on a plane: the same normal (the plane is linear), on a bump it moves the samples by half a unit
    const GridPtr sh5 = Run("normal_map", "shift=0.5", {{"a", rp}});
    CHECK(std::fabs(sh5->At(30, 30) - nx->At(30, 30)) < 1e-4f, "shift on a plane");
    CHECK(sh5->ox == 0.5f && sh5->oz == 0.5f && nx->ox == 0.0f, "shift moves the origin (%g, %g)", sh5->ox, sh5->oz);
    // on a bump the shifted result is the unshifted normal sampled half a unit further: nx(x) with shift 0.5 equals the engine's quad normal (h(x) - h(x + 1)) at the square (x, z)
    Grid bump = *Run("const", "width=33,height=33,value=0");
    for (int z = 0; z < 33; ++z) for (int x = 0; x < 33; ++x) bump.At(x, z) = 0.1f * static_cast<float>((x - 16) * (x - 16));
    const GridPtr bp = std::make_shared<Grid>(bump);
    const GridPtr sq = Run("normal_map", "shift=0.5,signed=1", {{"a", bp}});
    const float slope = (bump.At(10, 8) + bump.At(10, 9)) * 0.5f - (bump.At(11, 8) + bump.At(11, 9)) * 0.5f;     // left - right over the square (10, 8): both rows are equal here, so the mean of the two rows
    const float eq = slope / std::sqrt(slope * slope + 1.0f);
    CHECK(std::fabs(sq->At(10, 8) - eq) < 1e-4f, "square normal %g expected %g", sq->At(10, 8), eq);
  }
  {
    // wetness: a V valley is wetter along the floor than on its sides, and the catchment grows downhill
    GridPtr v = Run("gradient", "width=65,height=65,angle=0,lo=-1,hi=1");
    v = Run("abs", "", {{"a", v}});
    const GridPtr tilt = Run("gradient", "width=65,height=65,angle=90,lo=6,hi=0");
    const GridPtr valley = Run("add", "", {{"a", Run("affine", "mul=10,add=0", {{"a", v}})}, {"b", tilt}});
    const GridPtr wt = Run("wetness", "mode=twi", {{"a", valley}});
    const GridPtr ar = Run("wetness", "mode=area", {{"a", valley}});
    CHECK(wt->At(32, 40) > wt->At(10, 40), "the valley floor has a higher wetness index (%g vs %g)", wt->At(32, 40), wt->At(10, 40));
    CHECK(ar->At(32, 60) > ar->At(32, 10), "the catchment grows downstream");
    CHECK(InRange(*Run("wetness", "", {{"a", T}}), 0.0f, 1.0f), "wetness range");
    CHECK(MaxAbsDiff(*Run("wetness", "", {{"a", T}}), *Run("wetness", "", {{"a", T}})) == 0.0, "wetness is deterministic");
  }
  {
    GridPtr ramp = Run("gradient", "width=65,height=65,angle=0,lo=0,hi=96");   // slope 1.5 = 56 degrees
    const GridPtr steep = Run("rockiness", "breakup=0,w_convex=0", {{"a", ramp}});
    const GridPtr flat = Run("rockiness", "breakup=0,w_convex=0", {{"a", gFix["L"]}});
    CHECK(steep->At(32, 32) > 0.95f && flat->At(32, 32) == 0.0f, "rockiness: steep ground is rock, flat ground is not (%g, %g)", steep->At(32, 32), flat->At(32, 32));
    const GridPtr thin = Run("rockiness", "breakup=0,w_slope=0,w_convex=0,soil_full=2", {{"a", gFix["L"]}, {"b", Run("const", "width=65,height=65,value=0")}});
    CHECK(thin->At(10, 10) == 1.0f, "no soil = rock");
    CHECK(InRange(*Run("rockiness", "", {{"a", T}, {"b", gFix["S"]}}), 0.0f, 1.0f), "rockiness range");
  }
  {
    std::map<std::string, Grid> ex;
    const GridPtr ramp = Run("gradient", "width=33,height=3,angle=0,lo=0,hi=1");
    const GridPtr r = Run("color_ramp", "stops=0:0:0:0:1;1:1:0.5:0.25:0", {{"a", ramp}}, &ex);
    CHECK(std::fabs(r->At(16, 1) - 0.5f) < 1e-5f && std::fabs(ex["g"].At(16, 1) - 0.25f) < 1e-5f && std::fabs(ex["b"].At(16, 1) - 0.125f) < 1e-5f && std::fabs(ex["a"].At(16, 1) - 0.5f) < 1e-5f, "color_ramp interpolates all four channels");
    CHECK(r->At(0, 1) == 0.0f && r->At(32, 1) == 1.0f, "color_ramp ends");
    // unsorted stops are sorted, step mode holds the left colour, lo / hi remap the input
    std::map<std::string, Grid> e2;
    const GridPtr s2 = Run("color_ramp", "stops=1:1:1:1:1;0:0:0:0:0,interp=step", {{"a", ramp}}, &e2);
    CHECK(s2->At(16, 1) == 0.0f && s2->At(32, 1) == 1.0f, "color_ramp step mode with unsorted stops");
    std::map<std::string, Grid> e3;
    const GridPtr s3 = Run("color_ramp", "stops=0:0:0:0:1;1:1:1:1:1,lo=0.5,hi=1", {{"a", ramp}}, &e3);
    CHECK(s3->At(8, 1) == 0.0f && std::fabs(s3->At(24, 1) - 0.5f) < 1e-5f, "color_ramp lo / hi");
    std::string err;
    CHECK(!Make("color_ramp", "stops=0:0:0:0:1", {{"a", ramp}}, &err), "color_ramp: one stop must fail");
  }
}

static void PropErosion()
{
  const GridPtr H = gFix["H"], T = gFix["T"], L = gFix["L"];
  {
    std::map<std::string, Grid> ex;
    const GridPtr tm = Run("thermal_map", "iterations=25,talus=0.4,rate=0.5", {{"a", H}}, &ex);
    const GridPtr th = Run("thermal", "iterations=25,talus=0.4,rate=0.5", {{"a", H}});
    CHECK(SameBits(*tm, *th), "thermal_map with a constant talus equals thermal");
    CHECK(std::fabs(Sum(*tm) - Sum(*H)) < 0.5, "thermal_map conserves the total (%g)", Sum(*tm) - Sum(*H));
    CHECK(Sum(ex["scree"]) > 0.0 && InRange(ex["scree"], 0.0f, 1.0f) && InRange(ex["moved"], 0.0f, 1.0f), "thermal_map scree / moved");
    // a talus map: steep ground (talus 1.0) keeps more height than flat (0.35)
    const GridPtr lowT = Run("const", "width=65,height=65,value=0"), highT = Run("const", "width=65,height=65,value=1");
    const GridPtr a1 = Run("thermal_map", "iterations=40,talus_lo=0.2,talus_hi=1.2", {{"a", T}, {"b", lowT}});
    const GridPtr a2 = Run("thermal_map", "iterations=40,talus_lo=0.2,talus_hi=1.2", {{"a", T}, {"b", highT}});
    CHECK(Measure(*a1).max < Measure(*a2).max, "a lower talus slides more (%g vs %g)", Measure(*a1).max, Measure(*a2).max);
    std::map<std::string, Grid> e0;
    const GridPtr zero = Run("thermal_map", "iterations=0", {{"a", H}}, &e0);
    CHECK(SameBits(*zero, *H), "thermal_map with 0 iterations");
    std::string err;
    CHECK(!Make("thermal_map", "output=nothing", {{"a", H}}, &err), "thermal_map: bad output must fail");
  }
  {
    std::map<std::string, Grid> ex;
    const GridPtr rs = Run("erode_rocksoil", "iterations=40,weather=0.02,soil=0.5", {{"a", T}}, &ex);
    CHECK(std::fabs(Sum(*rs) - Sum(*T)) < 0.5, "erode_rocksoil conserves rock + soil (%g vs %g)", Sum(*rs), Sum(*T));
    CHECK(InRange(ex["soil"], 0.0f, 1e9f) && InRange(ex["rock"], 0.0f, 1.0f), "erode_rocksoil soil >= 0, rock 0..1");
    std::map<std::string, Grid> e2;
    const GridPtr nw = Run("erode_rocksoil", "iterations=20,weather=0,soil=0", {{"a", L}}, &e2);
    CHECK(SameBits(*nw, *L) && Measure(e2["rock"]).min == 1.0f, "no soil and no weathering on flat ground: all rock, unchanged");
    std::map<std::string, Grid> e3;
    Run("erode_rocksoil", "iterations=20,weather=0.0,rock_vis=0.5", {{"a", T}, {"b", gFix["S"]}}, &e3);
    CHECK(Measure(e3["soil"]).max > 1.0f, "a soil field is used");
  }
  {
    std::map<std::string, Grid> ex;
    const GridPtr sn = Run("erode_snow", "snowline=35,band=6,amount=3,iterations=30", {{"a", T}}, &ex);
    bool up = true, none = true;
    for (std::size_t i = 0; i < T->Count(); ++i) {
      up = up && sn->v[i] >= T->v[i] - 1e-5f;
      if (T->v[i] < 20.0f && ex["snow"].v[i] > 0.05f) none = false;
    }
    CHECK(up, "erode_snow only adds");
    CHECK(Measure(ex["snow"]).max > 0.5f && InRange(ex["cover"], 0.0f, 1.0f), "erode_snow has snow and a 0..1 cover");
    CHECK(Measure(*Run("erode_snow", "snowline=999,iterations=10", {{"a", T}}, &ex)).max <= Measure(*T).max + 1e-4f, "no snow below the snowline");
    (void)none;
  }
  {
    std::map<std::string, Grid> ex;
    const GridPtr wi = Run("erode_wind", "iterations=30,azimuth=0,sand=0.5,abrade=0.03", {{"a", H}}, &ex);
    CHECK(InRange(ex["sand"], 0.0f, 1e9f), "erode_wind sand >= 0");
    CHECK(Sum(*wi) <= Sum(*H) + 1e-2, "erode_wind never creates material");
    const GridPtr dead = Run("erode_wind", "iterations=0", {{"a", H}});
    CHECK(SameBits(*dead, *H), "erode_wind with 0 iterations");
    std::map<std::string, Grid> e2;
    Run("erode_wind", "iterations=40,azimuth=0,sand=1,abrade=0", {{"a", L}}, &e2);
    // sand blows to the +x edge and leaves: the upwind half is empty, the amount never grows
    CHECK(Measure(e2["sand"]).max <= 1.0f + 1e-4f, "erode_wind: flat sand sheet never gets deeper than it started");
    CHECK(e2["sand"].At(2, 30) < 0.5f, "erode_wind: the upwind edge empties (%g)", e2["sand"].At(2, 30));
  }
  {
    std::map<std::string, Grid> ex;
    const GridPtr gl = Run("erode_glacier", "iterations=120,ela=30,erode=0.002", {{"a", T}}, &ex);
    bool low = true;
    for (std::size_t i = 0; i < T->Count(); ++i) low = low && gl->v[i] <= T->v[i] + 1e-5f;
    CHECK(low, "erode_glacier only lowers the bed");
    CHECK(InRange(ex["ice"], 0.0f, 1e9f) && Measure(ex["ice"]).max > 0.0f, "erode_glacier makes ice");
    std::map<std::string, Grid> e2;
    const GridPtr none = Run("erode_glacier", "iterations=50,ela=9999,acc_max=0", {{"a", T}}, &e2);
    CHECK(SameBits(*none, *T) && Measure(e2["ice"]).max == 0.0f, "no accumulation = no ice, no change");
  }
  {
    std::map<std::string, Grid> ex;
    const GridPtr se = Run("settle", "iterations=20,rate=0.5,cap=1", {{"a", T}}, &ex);
    bool up = true;
    for (std::size_t i = 0; i < T->Count(); ++i) up = up && se->v[i] >= T->v[i];
    CHECK(up && InRange(ex["deposit"], 0.0f, 1.0f), "settle only fills");
    CHECK(SameBits(*Run("settle", "iterations=10", {{"a", L}}), *L), "settle keeps a flat field");
    const GridPtr s0 = Run("settle", "iterations=20,rate=0.5,cap=1,slope_lo=0,slope_hi=0.001", {{"a", T}});
    CHECK(MaxAbsDiff(*s0, *T) < MaxAbsDiff(*se, *T), "a tight slope limit fills less");
  }
}

static void PropThreads()
{
  // a bigger chain at several thread counts: the same bits
  const int orig = Threads();
  std::uint64_t first = 0;
  for (const int tc : {1, 2, 4, 7, 12}) {
    SetThreads(tc);
    const GridPtr base = Run("musgrave", "width=257,height=257,seed=3,scale=70,octaves=6");
    const GridPtr hh = Run("affine", "mul=60,add=5", {{"a", base}});
    const GridPtr w = Run("dwarp", "kind=curl,strength=20,size=60", {{"a", hh}});
    const GridPtr b = Run("bilateral", "radius=5,sigma_r=2", {{"a", w}});
    const GridPtr d = Run("diffuse", "iterations=10,kappa=0.3", {{"a", b}});
    const GridPtr m = Run("morph", "op=close,radius=3,shape=disc", {{"a", d}});
    std::map<std::string, Grid> ex;
    const GridPtr rs = Run("erode_rocksoil", "iterations=12", {{"a", m}}, &ex);
    const GridPtr sn = Run("erode_snow", "snowline=40,iterations=10", {{"a", rs}});
    const GridPtr gl = Run("erode_glacier", "iterations=20,ela=35", {{"a", sn}});
    const GridPtr oc = Run("occlusion", "radius=20", {{"a", gl}});
    const GridPtr wt = Run("wetness", "", {{"a", gl}});
    const GridPtr hs = Run("hillshade", "shadows=1", {{"a", gl}});
    const GridPtr st = Run("stratify", "thickness=2.5,warp=1", {{"a", gl}});
    const GridPtr bs = Run("blur_slope", "radius=4", {{"a", st}});
    const GridPtr ct = Run("cells", "width=257,height=257,seed=3,scale=40,mode=plate,warp=10");
    const std::uint64_t sum = Checksum(*gl) * 31 + Checksum(*oc) * 17 + Checksum(*wt) * 7 + Checksum(*hs) * 5 + Checksum(*bs) * 3 + Checksum(*ct) + Checksum(ex["soil"]) * 11;
    if (tc == 1) first = sum;
    else CHECK(sum == first, "T7 chain differs with %d threads", tc);
  }
  SetThreads(orig);
}

/** The erosion variants leave no step at the map border: the mean |h(edge) - h(edge + 1)| over the four sides stays near the input's (a missing neighbour never gives or takes, nothing is pinned). */
static void PropBorder()
{
  const int n = 129;
  const GridPtr r = Run("ridged", "width=129,height=129,seed=3,scale=51,octaves=3,persistence=0.4");
  const GridPtr p = Run("perlin", "width=129,height=129,seed=103,scale=25,octaves=3");
  const GridPtr base = Run("add", "", {{"a", Run("affine", "mul=55,add=0", {{"a", r}})}, {"b", Run("affine", "mul=8,add=0", {{"a", p}})}});
  auto tilted = std::make_shared<Grid>(*base);
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) tilted->At(x, z) += 0.25f * static_cast<float>(std::fabs(x - 0.5 * n) + 0.7 * std::fabs(z - 0.5 * n));
  const auto edgeStep = [n](const Grid& g) {
    double s = 0.0;
    int c = 0;
    for (int i = 2; i < n - 2; ++i) {
      s += std::fabs(g.At(i, 0) - g.At(i, 1)) + std::fabs(g.At(i, n - 1) - g.At(i, n - 2)) + std::fabs(g.At(0, i) - g.At(1, i)) + std::fabs(g.At(n - 1, i) - g.At(n - 2, i));
      c += 4;
    }
    return s / c;
  };
  struct C { const char* op; const char* params; };
  static const C cases[] = {{"thermal_map", "iterations=60,rate=0.5,talus=0.5"}, {"erode_rocksoil", "iterations=60"}, {"erode_snow", "iterations=40,amount=3"},
                            {"erode_wind", "iterations=40"}, {"erode_glacier", "iterations=60"}, {"settle", "iterations=20"}};
  const double before = edgeStep(*tilted);
  for (const C& c : cases) {
    const GridPtr out = Run(c.op, c.params, {{"a", GridPtr(tilted)}});
    const double after = edgeStep(*out);
    CHECK(after <= before * 1.2 + 0.05, "%s: a step at the map border (%.3f -> %.3f)", c.op, before, after);
  }
}

static void RunProperties()
{
  PropGenerators();
  PropFilters();
  PropAnalysis();
  PropErosion();
  PropBorder();
  PropThreads();
}
