#include "doctest.h"
#include "RenderCore/CursorMap.h"
#include <cmath>

TEST_CASE("cursor map: 800x600 menu centre maps to 4K centre") {
    const auto m = mc2::makeCursorMap(800, 600, 0, 0, 3840, 2160, 3840, 2160, 3840, 2160);
    CHECK(std::fabs(m.x(400) - 1920.0f) < 0.01f);
    CHECK(std::fabs(m.y(300) - 1080.0f) < 0.01f);
    CHECK(std::fabs(m.sx - 4.8f) < 0.001f);
    CHECK(std::fabs(m.sy - 3.6f) < 0.001f);
}

TEST_CASE("cursor map: identity when logical == display") {
    const auto m = mc2::makeCursorMap(1920, 1080, 0, 0, 1920, 1080, 1920, 1080, 1920, 1080);
    CHECK(m.x(123.0f) == doctest::Approx(123.0f));
    CHECK(m.y(456.0f) == doctest::Approx(456.0f));
    CHECK(m.sx == doctest::Approx(1.0f));
}

TEST_CASE("cursor map: letterboxed canvas and display != drawable") {
    // 4:3-ish window 2000x1500 hosting a 2000x1125 canvas at y=187; display is half the drawable.
    const auto m = mc2::makeCursorMap(800, 600, 0, 187, 2000, 1125, 2000, 1500, 1000, 750);
    CHECK(m.x(400) == doctest::Approx(500.0f));
    CHECK(m.y(300) == doctest::Approx((187 + 562.5f) * 0.5f));
}
