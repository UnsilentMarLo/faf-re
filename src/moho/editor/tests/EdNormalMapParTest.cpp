// Stand-alone test of the parallel normal map update (CWldTerrainRes::UpdateNormalMap in moho/sim/CWldMap.cpp, ed_par_normalmap).
//   links: moho/editor/EdFieldCore.cpp (the worker pool) and moho/render/textures/DXTCodec.cpp (the engine's own DXT block encoders)
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/EdNormalMapParTest.cpp src/sdk/moho/editor/EdFieldCore.cpp src/sdk/moho/render/textures/DXTCodec.cpp -o EdNormalMapParTest -pthread
//
// The engine function needs a CHeightField and a D3D texture; this test carries a MODEL of it (the same loops in the same order, GetElevation / GetNormal / EncodeNormalLaneByte copied
// from moho/sim/STIMap.cpp and CWldMap.cpp, the encoders linked from the engine) and proves what the change claims: the bytes written by the batched, pool-parallel loop are
// identical to the serial loop, for any thread count, any rectangle (aligned or not, clipped by the field edge) and any batch size, and rows never write outside their own span.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <random>
#include <vector>

#include "moho/editor/EdFieldCore.h"
#include "moho/render/textures/DXTCodec.h"

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    ++gChecks;                                         \
    if (!(cond)) {                                     \
      ++gFailures;                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

namespace
{
  struct Field
  {
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> data;

    static int Clamp(int v, int hi) { return std::max(0, std::min(v, hi)); }

    // CHeightField::GetElevation
    float GetElevation(const float x, const float z) const
    {
      const int fx = static_cast<int>(std::floor(static_cast<double>(x)));
      const int fz = static_cast<int>(std::floor(static_cast<double>(z)));
      const float fracX = x - static_cast<float>(fx);
      const float fracZ = z - static_cast<float>(fz);
      const int wMax = width - 1;
      const int hMax = height - 1;
      const int x0 = Clamp(fx, wMax);
      const int x1 = Clamp(fx + 1, wMax);
      const int z0 = Clamp(fz, hMax);
      const int z1 = Clamp(fz + 1, hMax);
      const float h00 = static_cast<float>(data[x0 + z0 * width]) * 0.0078125f;
      const float h01 = static_cast<float>(data[x0 + z1 * width]) * 0.0078125f;
      const float h10 = static_cast<float>(data[x1 + z0 * width]) * 0.0078125f;
      const float h11 = static_cast<float>(data[x1 + z1 * width]) * 0.0078125f;
      const double row0 = static_cast<double>(h00) + (static_cast<double>(h01) - h00) * fracZ;
      const double row1 = static_cast<double>(h10) + (static_cast<double>(h11) - h10) * fracZ;
      return static_cast<float>(row0 + (row1 - row0) * fracX);
    }

    // CHeightField::GetNormal (x and z lanes)
    void GetNormal(const float x, const float z, float& nx, float& nz) const
    {
      const float top = GetElevation(x, z - 0.5f);
      const float left = GetElevation(x - 0.5f, z);
      const float right = GetElevation(x + 0.5f, z);
      const float bottom = GetElevation(x, z + 0.5f);
      float vx = left - right;
      float vy = 1.0f;
      float vz = top - bottom;
      const float len = std::sqrt(vx * vx + vy * vy + vz * vz);
      vx /= len;
      vy /= len;
      vz /= len;
      (void)vy;
      nx = vx;
      nz = vz;
    }
  };

  std::uint8_t EncodeLane(const float lane)
  {
    int encoded = static_cast<int>((lane + 1.0f) * 128.0f);
    encoded = std::max(0, std::min(encoded, 255));
    return static_cast<std::uint8_t>(encoded);
  }

  struct Rect
  {
    int x0, z0, x1, z1;
  };

  int AlignDown4(const int v) { return v & ~3; }
  int AlignUp4(const int v) { return (v + 3) & ~3; }

  // One tile of CWldTerrainRes::UpdateNormalMap. `parallel` selects the new batched path (the engine's `blockCount >= threshold && enabled` test is the caller's).
  void UpdateTile(const Field& field, const Rect lockRect, const int tileXBase, const int tileZBase, std::uint8_t* const locked, const std::size_t pitch, const bool parallel,
                  const int batchRows, int* const ticks)
  {
    const auto encodeBlockRow = [&](const int localZ, std::uint8_t* const rowBytes) {
      auto* out = reinterpret_cast<std::uint64_t*>(rowBytes);
      for (int localX = lockRect.x0; localX < lockRect.x1; localX += 4) {
        std::uint8_t nx[16];
        std::uint8_t nz[16];
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            float fx = 0;
            float fz = 0;
            field.GetNormal(static_cast<float>(tileXBase + localX + c) + 0.5f, static_cast<float>(tileZBase + localZ + r) + 0.5f, fx, fz);
            nx[r * 4 + c] = EncodeLane(fx);
            nz[r * 4 + c] = EncodeLane(fz);
          }
        }
        std::uint64_t a = moho::DXT_EncodeAlphaBlock(nx, 1, 4);
        std::uint64_t g = moho::DXT_EncodeGreenBlock(nz);
        std::memcpy(out, &a, 8);
        std::memcpy(out + 1, &g, 8);
        out += 2;
      }
    };

    const int blockRows = (lockRect.z1 - lockRect.z0 + 3) / 4;
    if (parallel) {
      for (int start = 0; start < blockRows; start += batchRows) {
        const int end = std::min(blockRows, start + batchRows);
        moho::editor::field::ParallelFor(end - start, [&](const int index) {
          const int row = start + index;
          encodeBlockRow(lockRect.z0 + row * 4, locked + static_cast<std::size_t>(row) * pitch);
        });
        for (int row = start; row < end; ++row) {
          ++*ticks;
        }
      }
    } else {
      std::uint8_t* rowBytes = locked;
      for (int localZ = lockRect.z0; localZ < lockRect.z1; localZ += 4) {
        encodeBlockRow(localZ, rowBytes);
        ++*ticks;
        rowBytes += pitch;
      }
    }
  }

  // The whole function: tiles, clipping, locking (a fresh zeroed texture per call), for a rectangle `rect` (x1 / z1 exclusive, like the engine).
  std::vector<std::uint8_t> UpdateMap(const Field& field, const int tileW, const int tileH, const Rect rect, const bool parallel, const int batchRows, int* const ticks)
  {
    const int tilesX = (field.width - 1 + tileW - 1) / tileW;
    const int tilesY = (field.height - 1 + tileH - 1) / tileH;
    const std::size_t pitch = static_cast<std::size_t>(tileW / 4) * 16;
    const std::size_t tileBytes = pitch * static_cast<std::size_t>(tileH / 4);
    std::vector<std::uint8_t> texels(tileBytes * static_cast<std::size_t>(tilesX * tilesY), 0xCD);   // 0xCD: what an untouched byte looks like
    std::size_t tileIndex = 0;
    for (int tz = 0; tz < field.height - 1; tz += tileH) {
      for (int tx = 0; tx < field.width - 1; tx += tileW, ++tileIndex) {
        const int cx0 = std::max(tx, rect.x0);
        const int cx1 = std::min(tx + tileW, rect.x1);
        const int cz0 = std::max(tz, rect.z0);
        const int cz1 = std::min(tz + tileH, rect.z1);
        if (cx0 >= cx1 || cz0 >= cz1) {
          continue;
        }
        Rect lock{AlignDown4(cx0 - tx), AlignDown4(cz0 - tz), AlignUp4(cx1 - tx), AlignUp4(cz1 - tz)};
        // the engine's LockRect hands out the memory of the locked rectangle only: rows start at lock.z0, columns at lock.x0
        std::uint8_t* tileBase = texels.data() + tileIndex * tileBytes;
        std::uint8_t* locked = tileBase + static_cast<std::size_t>(lock.z0 / 4) * pitch + static_cast<std::size_t>(lock.x0 / 4) * 16;
        UpdateTile(field, lock, tx, tz, locked, pitch, parallel, batchRows, ticks);
      }
    }
    return texels;
  }

  Field MakeField(const int w, const int h, const unsigned seed)
  {
    Field f;
    f.width = w;
    f.height = h;
    f.data.resize(static_cast<std::size_t>(w) * h);
    std::mt19937 rng(seed);
    for (int z = 0; z < h; ++z) {
      for (int x = 0; x < w; ++x) {
        const double smooth = 6000.0 + 2500.0 * std::sin(x * 0.07) * std::cos(z * 0.05);
        f.data[static_cast<std::size_t>(z) * w + x] = static_cast<std::uint16_t>(smooth + static_cast<double>(rng() % 400));
      }
    }
    return f;
  }
} // namespace

int main(const int argc, char** const argv)
{
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
    // timing of the model on this machine (not a check): a 2049 x 2049 field, whole map
    const Field field = MakeField(2049, 2049, 1);
    const Rect rect{0, 0, 2048, 2048};
    const auto time = [&](const bool parallel) {
      int ticks = 0;
      const auto t0 = std::chrono::steady_clock::now();
      const std::vector<std::uint8_t> out = UpdateMap(field, 2048, 2048, rect, parallel, 64, &ticks);
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() + 0.0 * static_cast<double>(out.size());
    };
    moho::editor::field::SetThreads(1);
    const double serial = time(false);
    std::printf("serial %.0f ms\n", serial);
    for (const int t : {2, 4, 8, 16}) {
      moho::editor::field::SetThreads(t);
      std::printf("%d threads %.0f ms\n", t, time(true));
    }
    return 0;
  }
  const int origThreads = moho::editor::field::Threads();
  std::mt19937 rng(12345);

  // sizes: one tile, several tiles (tile 64), a field edge that is not a multiple of 4
  struct Case
  {
    int w, h, tile;
  };
  const Case cases[] = {{129, 129, 128}, {257, 193, 64}, {200, 131, 64}, {513, 513, 512}};
  for (const Case& c : cases) {
    const Field field = MakeField(c.w, c.h, 7u + static_cast<unsigned>(c.w));
    const int tileW = c.tile;
    const int tileH = c.tile;
    for (int round = 0; round < 6; ++round) {
      Rect rect{0, 0, field.width - 1, field.height - 1};
      if (round > 0) {
        rect.x0 = static_cast<int>(rng() % static_cast<unsigned>(field.width - 2));
        rect.z0 = static_cast<int>(rng() % static_cast<unsigned>(field.height - 2));
        rect.x1 = std::min(field.width - 1, rect.x0 + 1 + static_cast<int>(rng() % 150u));
        rect.z1 = std::min(field.height - 1, rect.z0 + 1 + static_cast<int>(rng() % 150u));
      }
      int serialTicks = 0;
      const std::vector<std::uint8_t> serial = UpdateMap(field, tileW, tileH, rect, false, 1, &serialTicks);
      for (const int threads : {1, 2, 3, 8}) {
        for (const int batch : {1, 7, 64}) {
          moho::editor::field::SetThreads(threads);
          int parTicks = 0;
          const std::vector<std::uint8_t> par = UpdateMap(field, tileW, tileH, rect, true, batch, &parTicks);
          CHECK(par == serial, "case %dx%d tile %d round %d threads %d batch %d: bytes differ", c.w, c.h, c.tile, round, threads, batch);
          CHECK(parTicks == serialTicks, "ticks %d vs %d (one per block row either way)", parTicks, serialTicks);
        }
      }
    }
  }

  // an untouched byte stays untouched: rows write only inside their rectangle (0xCD marks the texture before the update)
  {
    const Field field = MakeField(257, 257, 99);
    int ticks = 0;
    moho::editor::field::SetThreads(4);
    const Rect rect{40, 52, 100, 120};
    const std::vector<std::uint8_t> par = UpdateMap(field, 256, 256, rect, true, 3, &ticks);
    std::size_t touched = 0;
    for (const std::uint8_t b : par) {
      touched += b != 0xCD ? 1u : 0u;
    }
    const std::size_t expected = static_cast<std::size_t>((AlignUp4(100) - AlignDown4(40)) / 4) * 16 * static_cast<std::size_t>((AlignUp4(120) - AlignDown4(52)) / 4);
    CHECK(touched <= expected && touched > expected / 2, "touched %zu bytes, the blocks of the rectangle hold %zu", touched, expected);
  }

  moho::editor::field::SetThreads(origThreads);
  std::printf("EdNormalMapParTest: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
