#include "doctest.h"
#include <algorithm>

namespace {
using std::min;
using std::max;
// Compile the production shader math itself; no separate CPU implementation.
#include "../../shaders/include/water_shore_lift.hglsl"
}

TEST_CASE("water shore lift: live bake preserves land and object depth") {
    // A sim corner 30 wu above water used to pull water in front of real land.
    CHECK(waterShoreLift(16.0f, 31.0f, true) == 0.0f);
    CHECK(waterShoreLift(16.0f, 200.0f, true) == 0.0f);
    CHECK(waterShoreLift(16.0f, 0.0f, true) == 0.0f);
}

TEST_CASE("water shore lift: sim-only maps retain tile-top cover and cap") {
    CHECK(waterShoreLift(16.0f, 31.0f, false) == 31.0f);
    CHECK(waterShoreLift(16.0f, 0.0f, false) == 16.0f);
    CHECK(waterShoreLift(16.0f, 200.0f, false) == 128.0f);
}
