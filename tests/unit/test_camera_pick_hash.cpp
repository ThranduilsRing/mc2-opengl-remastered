#include "doctest.h"
#include "camera_pick_hash.h"

namespace {
mc2cam::PickInputs base() { return {10.f, 20.f, 3.f, 45.f, 60.f, 40.f, 500.f, 1.f, 2.f, 1920.f, 1080.f}; }
}

TEST_CASE("pick hash: zoom (cameraAltitude) alone changes it") {
    auto a = base(), b = base();
    b.altitude += 1.0f;   // one grid step is 0.25
    CHECK(mc2cam::pickInputHash(a) != mc2cam::pickInputHash(b));
}

TEST_CASE("pick hash: eased look-at shift alone changes it") {
    auto a = base(), bx = base(), by = base();
    bx.shiftX += 1.0f;
    by.shiftY += 1.0f;
    CHECK(mc2cam::pickInputHash(a) != mc2cam::pickInputHash(bx));
    CHECK(mc2cam::pickInputHash(a) != mc2cam::pickInputHash(by));
}

TEST_CASE("pick hash: sub-grid altitude drift keeps it") {
    auto a = base(), b = base();
    b.altitude += 0.01f;
    CHECK(mc2cam::pickInputHash(a) == mc2cam::pickInputHash(b));
}
