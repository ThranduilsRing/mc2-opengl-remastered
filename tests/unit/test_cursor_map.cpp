#include "doctest.h"
#include "RenderCore/CursorMap.h"
#include "RenderCore/CursorImage.h"
#include <array>
#include <cmath>

TEST_CASE("cursor image: magenta becomes alpha zero and other pixels survive") {
    std::array<uint8_t, 20> pixels{255,0,255,255, 255,0,254,255, 20,30,40,128,
                                  255,0,255,0, 40,50,60,0};
    mc2::keyCursorPixels(pixels);
    CHECK(pixels == std::array<uint8_t,20>{0,0,0,0, 255,0,254,255, 20,30,40,128,
                                         0,0,0,0, 0,0,0,0});
}

TEST_CASE("cursor image: every stock and mod shape maps to its created cursor") {
    std::array<unsigned long, 512> counts{};
    counts.fill(1);
    counts[21] = 4;
    counts[43] = 0; // A nonanimated shape still has one image.
    mc2::CursorShapes<int> cursors;
    int created = 0;
    cursors.create(counts, [&](size_t shape, size_t frame) {
        ++created;
        return static_cast<int>(shape * 10 + frame + 1);
    });
    CHECK(created == 515);
    for (long shape = 0; shape < 512; ++shape)
        CHECK(cursors.get(shape, 0) == shape * 10 + 1);
    CHECK(cursors.get(21, 3) == 214);
    CHECK(cursors.get(21, 4) == 211);
    CHECK(cursors.get(-1, 0) == 0);
    CHECK(cursors.get(512, 0) == 0);
    cursors.create(counts, [](size_t, size_t) { return 0; });
    CHECK(cursors.get(21, 0) == 0); // Software fallback on creation failure.
}

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
