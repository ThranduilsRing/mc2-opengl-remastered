// Projection of a debug segment to window pixels (the Vulkan-run debug overlay).
#include "doctest.h"

#include "debug_line_project.h"

#include <limits>

using DebugRenderer::ScreenSegment;
using DebugRenderer::projectSegment;

namespace {
// Column-major identity: clip = (x, y, z, 1).
const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}

TEST_CASE("debug line project: NDC corners land on window corners, y runs down") {
    const float a[3] = {-1.0f, 1.0f, 0.0f};
    const float b[3] = { 1.0f, -1.0f, 0.0f};
    ScreenSegment s{};
    REQUIRE(projectSegment(kIdentity, a, b, 800.0f, 600.0f, &s));
    CHECK(s.x0 == doctest::Approx(0.0f));
    CHECK(s.y0 == doctest::Approx(0.0f));
    CHECK(s.x1 == doctest::Approx(800.0f));
    CHECK(s.y1 == doctest::Approx(600.0f));
}

TEST_CASE("debug line project: a translation sits in the matrix's fourth column") {
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0.5f,0,0,1};
    const float a[3] = {0, 0, 0};
    ScreenSegment s{};
    REQUIRE(projectSegment(m, a, a, 100.0f, 100.0f, &s));
    CHECK(s.x0 == doctest::Approx(75.0f));   // ndc 0.5 -> 0.75 of the width
    CHECK(s.y0 == doctest::Approx(50.0f));
}

TEST_CASE("debug line project: a segment behind the camera is dropped, one crossing the near plane is cut") {
    // w = z: points with z <= 0 are behind.
    const float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,1, 0,0,0,0};
    const float behindA[3] = {0, 0, -2}, behindB[3] = {0, 0, -1}, front[3] = {0, 0, 2};
    ScreenSegment s{};
    CHECK_FALSE(projectSegment(m, behindA, behindB, 100.0f, 100.0f, &s));
    CHECK(projectSegment(m, behindA, front, 100.0f, 100.0f, &s));
    CHECK(std::isfinite(s.x0));
    CHECK(std::isfinite(s.y0));
}

TEST_CASE("debug line project: non-finite input and a bad target are refused") {
    const float a[3] = {std::numeric_limits<float>::quiet_NaN(), 0, 0}, b[3] = {0, 0, 0};
    ScreenSegment s{};
    CHECK_FALSE(projectSegment(kIdentity, a, b, 100.0f, 100.0f, &s));
    CHECK_FALSE(projectSegment(kIdentity, b, b, 0.0f, 100.0f, &s));
    CHECK_FALSE(projectSegment(nullptr, b, b, 100.0f, 100.0f, &s));
}
