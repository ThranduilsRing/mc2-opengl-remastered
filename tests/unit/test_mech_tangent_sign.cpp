// tests/unit/test_mech_tangent_sign.cpp
// Mirror fixture for mechTangentSign: a quad and its UV-mirrored twin must get opposite handedness,
// and a degenerate triangle must stay +1.
#include "doctest.h"
#include "../../GameOS/gameos/mech_tangent_sign.h"

namespace {
// Face tangent/bitangent from positions + uvs, exactly as gos_mech_batcher_geometry.cpp derives them.
float signFor(const float p0[3], const float p1[3], const float p2[3],
              const float uv0[2], const float uv1[2], const float uv2[2], const float n[3])
{
    float e1[3], e2[3];
    for (int i = 0; i < 3; ++i) { e1[i] = p1[i] - p0[i]; e2[i] = p2[i] - p0[i]; }
    const float du1 = uv1[0] - uv0[0], dv1 = uv1[1] - uv0[1];
    const float du2 = uv2[0] - uv0[0], dv2 = uv2[1] - uv0[1];
    const float r = 1.0f / (du1 * dv2 - du2 * dv1);
    float t[3], b[3];
    for (int i = 0; i < 3; ++i) {
        t[i] = r * (dv2 * e1[i] - dv1 * e2[i]);
        b[i] = r * (du1 * e2[i] - du2 * e1[i]);
    }
    return mechTangentSign(t, b, n);
}
}

TEST_CASE("mechTangentSign: mirrored UV island flips handedness")
{
    const float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 0}, p2[3] = {0, 1, 0};
    const float n[3] = {0, 0, 1};
    const float a0[2] = {0, 0}, a1[2] = {1, 0}, a2[2] = {0, 1};       // u along +x, v along +y
    const float m0[2] = {1, 0}, m1[2] = {0, 0}, m2[2] = {1, 1};       // u mirrored
    const float s = signFor(p0, p1, p2, a0, a1, a2, n);
    const float sm = signFor(p0, p1, p2, m0, m1, m2, n);
    CHECK(s == 1.0f);
    CHECK(sm == -1.0f);
}

TEST_CASE("mechTangentSign: zero normal or zero bitangent stays +1")
{
    const float z[3] = {0, 0, 0}, t[3] = {1, 0, 0}, n[3] = {0, 0, 1};
    CHECK(mechTangentSign(t, z, n) == 1.0f);
    CHECK(mechTangentSign(t, t, z) == 1.0f);
}
