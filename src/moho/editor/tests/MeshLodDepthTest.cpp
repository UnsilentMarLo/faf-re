// Stand-alone test of the mesh LOD depth (mesh/MeshLodDepth.h; no engine needed).
//   g++ -std=c++20 -O2 -I src/sdk src/sdk/moho/editor/tests/MeshLodDepthTest.cpp -o MeshLodDepthTest
//   MSVC: cl /std:c++20 /EHsc /O2 /I src/sdk MeshLodDepthTest.cpp
//
// The camera's LOD row (GeomCamera3::viewport.r[1]) is slope * (view-space depth) + intercept as a plane (a,b,c,d). MeshRenderer::Batch must
// dot the instance position with that plane. It used to dot it with the camera's orientation quaternion. The test builds the camera the way
// CameraImpl::UpdateCoords does (focus + eye distance + heading + pitch), and checks that
//   * the depth of the focus point is the eye distance times the slope for every heading, also for headings outside (-pi, pi] (the engine's
//     NormalizeQuadrantRelative lets the heading accumulate: -4.71 for east reached by turning left, +1.57 reached by turning right);
//   * the old quaternion formula is NOT: it changes with the heading and flips its sign by a full turn (q and -q are one rotation).
#include <cmath>
#include <cstdio>

#include "moho/mesh/MeshLodDepth.h"

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

struct Vec3 { float x, y, z; };
struct Quat { float w, x, y, z; }; // lane order of Wm3::Quatf's storage: m_afTuple[0..3] = w, x, y, z

static Quat Mul(const Quat& a, const Quat& b)
{
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

// yaw about +Y by heading, then pitch about X: a camera orientation like COORDS_Orient(heading, pitch)
static Quat Orient(float heading, float pitch)
{
  const Quat yaw{std::cos(heading * 0.5f), 0.0f, std::sin(heading * 0.5f), 0.0f};
  const Quat tilt{std::cos(pitch * 0.5f), std::sin(pitch * 0.5f), 0.0f, 0.0f};
  return Mul(yaw, tilt);
}

static Vec3 Rotate(const Quat& q, const Vec3& v)
{
  const Quat p{0.0f, v.x, v.y, v.z};
  const Quat c{q.w, -q.x, -q.y, -q.z};
  const Quat r = Mul(Mul(q, p), c);
  return {r.x, r.y, r.z};
}

int main()
{
  const float pi = 3.14159265f;
  const Vec3 focus{300.0f, 12.0f, 700.0f};
  const float eyeDistance = 150.0f;
  const float pitch = 1.0f;
  const float slope = 1.1f; // 2 * tan(fov / 2)-ish, a LOD scale of 1

  const float headings[] = {0.0f, 1.5708f, -4.7124f, 1.5708f - 2.0f * pi, -pi, pi, 3.0f, 3.0f - 2.0f * pi, 6.0f};
  float firstOld = 0.0f;
  float minOld = 1e30f, maxOld = -1e30f;
  bool first = true;
  for (const float h : headings) {
    const Quat q = Orient(h, pitch);
    const Vec3 forward = Rotate(q, {0.0f, 0.0f, 1.0f}); // the direction the eye looks along (up to the engine's sign convention)
    const Vec3 eye{focus.x - forward.x * eyeDistance, focus.y - forward.y * eyeDistance, focus.z - forward.z * eyeDistance};
    // viewport.r[1] as plane (a,b,c,d): slope * (dot(p - eye, forward)) = slope * forward . p - slope * forward . eye
    const float a = slope * forward.x, b = slope * forward.y, c = slope * forward.z;
    const float d = -slope * (forward.x * eye.x + forward.y * eye.y + forward.z * eye.z);

    const float depth = moho::MeshLodDepth(a, b, c, d, focus.x, focus.y, focus.z);
    CHECK(std::fabs(depth - slope * eyeDistance) < 1e-2f, "heading %.4f: depth of the focus %.4f, expected %.4f", h, depth, slope * eyeDistance);
    // a point one forward step further is exactly slope further away, whatever the heading
    const float further = moho::MeshLodDepth(a, b, c, d, focus.x + forward.x, focus.y + forward.y, focus.z + forward.z);
    CHECK(std::fabs(further - depth - slope) < 1e-2f, "heading %.4f: depth step %.4f", h, further - depth);

    // the old formula: the orientation quaternion's lanes as the plane
    const float old = moho::MeshLodDepth(q.w, q.x, q.y, q.z, focus.x, focus.y, focus.z);
    if (first) { firstOld = old; first = false; }
    minOld = std::fmin(minOld, old);
    maxOld = std::fmax(maxOld, old);
  }
  CHECK(maxOld - minOld > 100.0f, "the old quaternion formula should vary with the heading (%.2f .. %.2f, first %.2f)", minOld, maxOld, firstOld);

  // q and -q (heading and heading + a full turn) are the same camera: the old depth flips sign, the new one does not
  const Quat q0 = Orient(1.5708f, pitch);
  const Quat q1 = Orient(1.5708f - 2.0f * pi, pitch);
  const float o0 = moho::MeshLodDepth(q0.w, q0.x, q0.y, q0.z, 500.0f, 0.0f, 500.0f);
  const float o1 = moho::MeshLodDepth(q1.w, q1.x, q1.y, q1.z, 500.0f, 0.0f, 500.0f);
  CHECK(std::fabs(o0 + o1) < 0.5f && std::fabs(o0) > 10.0f, "old formula: %.2f vs %.2f should be negatives", o0, o1);

  std::printf("%d checks, %d failures (old formula over headings: %.1f .. %.1f)\n", gChecks, gFailures, minOld, maxOld);
  return gFailures ? 1 : 0;
}
