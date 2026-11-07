// Temporal inputs (docs/plans/2026-11-06-temporal-aa.md slice B): jitter sequence, neutral path
// with no consumer, jitter application, mip bias hook, and the motion-vector algebra that
// motion_vectors.frag mirrors. Offline, no Vulkan.
#include "doctest.h"
#include "RenderCore/TemporalInputs.h"
#include <cmath>

namespace T = RenderCore::temporal;

namespace {
void identity(float* m) { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
}

TEST_CASE("temporal: no consumer means zero jitter and the existing mip bias") {
    REQUIRE(T::consumerCount().load() == 0);
    float j[2] = {1, 1};
    T::frameJitterClip(7, 1280, 720, 1920, j);
    CHECK(j[0] == 0.0f);
    CHECK(j[1] == 0.0f);
    CHECK(T::mipBias(0.5f, false) == doctest::Approx(-1.0f));
    CHECK(T::mipBias(0.5f, true) == doctest::Approx(-2.0f));
}

TEST_CASE("temporal: Halton jitter stays inside half a pixel and cycles over the phase count") {
    CHECK(T::phaseCount(1920, 1920) == 8u);
    CHECK(T::phaseCount(1280, 1920) == 18u);   // 8 * 2.25
    for (uint32_t f = 0; f < 40; ++f) {
        float px[2];
        T::jitterPixels(f, 8, px);
        CHECK(px[0] >= -0.5f);  CHECK(px[0] < 0.5f);
        CHECK(px[1] >= -0.5f);  CHECK(px[1] < 0.5f);
    }
    float a[2], b[2];
    T::jitterPixels(3, 8, a);
    T::jitterPixels(11, 8, b);
    CHECK(a[0] == b[0]);
    CHECK(a[1] == b[1]);
}

TEST_CASE("temporal: active consumer publishes 2 * px / extent and the matrix offset matches it") {
    T::consumerCount().fetch_add(1);
    float j[2];
    T::frameJitterClip(0, 1000, 500, 1000, j);
    T::consumerCount().fetch_sub(1);
    float px[2];
    T::jitterPixels(0, 8, px);
    CHECK(j[0] == doctest::Approx(2.0f * px[0] / 1000.0f));
    CHECK(j[1] == doctest::Approx(2.0f * px[1] / 500.0f));

    // clip.xy += J * clip.w for a perspective-like row 3.
    float m[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, -1, 0};   // w = -z (row-major row 3)
    T::applyJitterRowMajor(m, j);
    CHECK(m[0 * 4 + 2] == doctest::Approx(j[0] * -1.0f));
    CHECK(m[1 * 4 + 2] == doctest::Approx(j[1] * -1.0f));
    CHECK(m[3 * 4 + 2] == -1.0f);
}

TEST_CASE("temporal: motion vector algebra") {
    float rep[16];
    identity(rep);
    const float uv[2] = {0.3f, 0.6f};
    float mv[2];

    SUBCASE("stationary camera with alternating jitter gives zero") {
        for (float s : {-1.0f, 1.0f}) {
            const float j[2] = {0.0013f * s, -0.0007f * s};
            T::motionVector(rep, uv, 0.4f, j, mv);
            CHECK(mv[0] == doctest::Approx(0.0f).epsilon(1e-6));
            CHECK(mv[1] == doctest::Approx(0.0f).epsilon(1e-6));
        }
    }
    SUBCASE("pan of 0.2 clip units in x and -0.1 in y is 0.1 / -0.05 UV, jitter independent") {
        rep[3 * 4 + 0] = 0.2f;    // column 3 = translation (column-major)
        rep[3 * 4 + 1] = -0.1f;
        for (float s : {0.0f, 1.0f}) {
            const float j[2] = {0.002f * s, 0.001f * s};
            T::motionVector(rep, uv, 0.4f, j, mv);
            CHECK(mv[0] == doctest::Approx(0.1f).epsilon(1e-5));
            CHECK(mv[1] == doctest::Approx(-0.05f).epsilon(1e-5));
        }
    }
    SUBCASE("zoom about the centre scales with distance from it") {
        rep[0] = rep[5] = 0.9f;
        const float j[2] = {0, 0};
        T::motionVector(rep, uv, 0.4f, j, mv);
        CHECK(mv[0] == doctest::Approx(0.5f * 0.9f * (uv[0] * 2 - 1) + 0.5f - uv[0]).epsilon(1e-5));
    }
    SUBCASE("sky is zero") {
        rep[3 * 4 + 0] = 0.5f;
        const float j[2] = {0, 0};
        T::motionVector(rep, uv, 0.0f, j, mv);
        CHECK(mv[0] == 0.0f);
        CHECK(mv[1] == 0.0f);
    }
}
