// tests/unit/test_clip_sample.cpp
// glTF clip key sampling (mclib/clip_sample.h): STEP, LINEAR and CUBICSPLINE, Assimp-free.
#include "doctest.h"
#include "clip_sample.h"
#include <cmath>

namespace {
struct V3 { float x = 0, y = 0, z = 0; V3() = default; V3(float a, float b, float c) : x(a), y(b), z(c) {} };
struct Q4 { float w = 1, x = 0, y = 0, z = 0; Q4() = default; Q4(float a, float b, float c, float d) : w(a), x(b), y(c), z(d) {} };
struct VKey { double mTime; V3 mValue; int mInterpolation; };
struct QKey { double mTime; Q4 mValue; int mInterpolation; };

using namespace mc2skel::clip;
// Test stand-in for aiQuaternion::Interpolate + Normalize: normalised lerp.
auto nlerp = [](Q4& out, const Q4& a, const Q4& b, float f) {
    out = Q4(a.w + (b.w - a.w) * f, a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f);
    const float l = std::sqrt(out.w * out.w + out.x * out.x + out.y * out.y + out.z * out.z);
    out = Q4(out.w / l, out.x / l, out.y / l, out.z / l);
};
}

TEST_CASE("clip sample: STEP holds the segment-start value")
{
    const VKey k[] = {{0, V3(0, 0, 0), kStep}, {1000, V3(10, 0, 0), kStep}, {2000, V3(20, 0, 0), kStep}};
    CHECK(sampleVec3<VKey, V3>(k, 3, 500, 0.001).x == 0.0f);
    CHECK(sampleVec3<VKey, V3>(k, 3, 999.9, 0.001).x == 0.0f);
    CHECK(sampleVec3<VKey, V3>(k, 3, 1000, 0.001).x == 10.0f);
    CHECK(sampleVec3<VKey, V3>(k, 3, 5000, 0.001).x == 20.0f);
    const QKey q[] = {{0, Q4(1, 0, 0, 0), kStep}, {1000, Q4(0, 1, 0, 0), kStep}};
    CHECK(sampleQuat<QKey, Q4>(q, 2, 700, 0.001, nlerp).w == 1.0f);
}

TEST_CASE("clip sample: LINEAR interpolates and clamps at the end")
{
    const VKey k[] = {{0, V3(0, 0, 0), kLinear}, {1000, V3(10, 20, 30), kLinear}};
    const V3 mid = sampleVec3<VKey, V3>(k, 2, 250, 0.001);
    CHECK(mid.x == doctest::Approx(2.5f));
    CHECK(mid.y == doctest::Approx(5.0f));
    CHECK(mid.z == doctest::Approx(7.5f));
    CHECK(sampleVec3<VKey, V3>(k, 2, 9999, 0.001).z == 30.0f);
    const QKey q[] = {{0, Q4(1, 0, 0, 0), kLinear}, {1000, Q4(0, 1, 0, 0), kLinear}};
    const Q4 h = sampleQuat<QKey, Q4>(q, 2, 500, 0.001, nlerp);
    CHECK(h.w == doctest::Approx(0.70710678f));
    CHECK(h.x == doctest::Approx(0.70710678f));
}

TEST_CASE("clip sample: CUBICSPLINE reads in/value/out triplets and hits the keys")
{
    // Two logical keys (0 -> 10 over 1 s) as [in, value, out] triplets. Zero tangents give
    // a smoothstep: the midpoint is the average and the ends are exact.
    VKey k[6] = {{0, V3(), kCubic}, {0, V3(0, 0, 0), kCubic}, {0, V3(), kCubic},
                 {1000, V3(), kCubic}, {1000, V3(10, 0, 0), kCubic}, {1000, V3(), kCubic}};
    CHECK(sampleVec3<VKey, V3>(k, 6, 0, 0.001).x == doctest::Approx(0.0f));
    CHECK(sampleVec3<VKey, V3>(k, 6, 500, 0.001).x == doctest::Approx(5.0f));
    CHECK(sampleVec3<VKey, V3>(k, 6, 250, 0.001).x == doctest::Approx(1.5625f));  // 10 * (3s^2 - 2s^3), s = 0.25
    CHECK(sampleVec3<VKey, V3>(k, 6, 1000, 0.001).x == doctest::Approx(10.0f));
    // Tangents are per second, scaled by the 1 s segment: an out-tangent of 10/s on key 0
    // lifts the s=0.5 value by h10 * 10 = 0.125 * 10.
    k[2].mValue = V3(10, 0, 0);
    CHECK(sampleVec3<VKey, V3>(k, 6, 500, 0.001).x == doctest::Approx(5.0f + 0.125f * 10.0f));
    // A 2 s segment doubles the tangent contribution (sampled at its midpoint, t = 1000).
    k[3].mTime = k[4].mTime = k[5].mTime = 2000;
    CHECK(sampleVec3<VKey, V3>(k, 6, 1000, 0.001).x == doctest::Approx(5.0f + 0.125f * 20.0f));
}

TEST_CASE("clip sample: CUBICSPLINE quaternion stays unit length")
{
    const QKey q[6] = {{0, Q4(1, 0, 0, 0), kCubic}, {0, Q4(1, 0, 0, 0), kCubic}, {0, Q4(0, 0, 0, 0), kCubic},
                       {1000, Q4(0, 0, 0, 0), kCubic}, {1000, Q4(0, 1, 0, 0), kCubic}, {1000, Q4(0, 0, 0, 0), kCubic}};
    const Q4 h = sampleQuat<QKey, Q4>(q, 6, 500, 0.001, nlerp);
    CHECK(std::sqrt(h.w * h.w + h.x * h.x + h.y * h.y + h.z * h.z) == doctest::Approx(1.0f));
    CHECK(h.w == doctest::Approx(0.70710678f));
}
