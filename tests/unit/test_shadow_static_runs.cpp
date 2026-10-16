// tests/unit/test_shadow_static_runs.cpp
// The VK static shadow map's building runs: a map without static props has empty type tables, so every registry building is dropped and nothing
// is drawn; with tables, one run per type present and the out-of-range types counted.
#include "doctest.h"
#include "gameos/shadow_static_building_runs.h"

using mc2vk::planShadowBuildingRuns;

TEST_CASE("static shadow runs: no static-prop geometry draws no building") {
    const uint32_t types[] = {0, 0, 3, 7};
    uint32_t dropped = 99;
    const auto runs = planShadowBuildingRuns(types, 0, dropped);
    CHECK(runs.empty());
    CHECK(dropped == 4);
}

TEST_CASE("static shadow runs: no building instances") {
    uint32_t dropped = 99;
    const auto runs = planShadowBuildingRuns({}, 5, dropped);
    CHECK(runs.empty());
    CHECK(dropped == 0);
}

TEST_CASE("static shadow runs: one run per type, types past the table dropped") {
    const uint32_t types[] = {1, 1, 1, 2, 4, 4, 9};
    uint32_t dropped = 0;
    const auto runs = planShadowBuildingRuns(types, 5, dropped);
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].typeID == 1); CHECK(runs[0].firstInstance == 0); CHECK(runs[0].count == 3);
    CHECK(runs[1].typeID == 2); CHECK(runs[1].firstInstance == 3); CHECK(runs[1].count == 1);
    CHECK(runs[2].typeID == 4); CHECK(runs[2].firstInstance == 4); CHECK(runs[2].count == 2);
    CHECK(dropped == 1);
}
