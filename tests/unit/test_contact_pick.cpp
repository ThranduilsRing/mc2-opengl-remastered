#include "doctest.h"
#include "../../code/hover_name.h"
#include "../../mclib/terrain_ray_pick.h"

TEST_CASE("contact pick: sensor naming never leaks DEMO or visual identification")
{
    for (bool vehicle : {false, true}) {
        for (int status = CONTACT_NONE; status <= CONTACT_VISUAL; ++status) {
            CHECK(mc2::hoverNameId(status, 55, IDS_DEMO_MSG, vehicle) != IDS_DEMO_MSG);
            if (status < CONTACT_SENSOR_QUALITY_3)
                CHECK(mc2::hoverNameId(status, 55, 1234, vehicle) == 0);
        }
        CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_4, 55, 1234, vehicle) == 1234);
        CHECK(mc2::hoverNameId(CONTACT_VISUAL, 55, 1234, vehicle) == 1234);
        CHECK(mc2::hoverNameId(CONTACT_VISUAL, 55, -1, vehicle) == 0);
    }
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 39, 1234) == IDS_SENSOR_LIGHT_MECH);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 40, 1234) == IDS_SENSOR_MEDIUM_MECH);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 64, 1234) == IDS_SENSOR_MEDIUM_MECH);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 65, 1234) == IDS_SENSOR_HEAVY_MECH);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 79, 1234) == IDS_SENSOR_HEAVY_MECH);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 80, 1234) == IDS_SENSOR_ASSAULT_MECH);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 40, 1234, true) == IDS_SENSOR_LIGHT_VEHICLE);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 41, 1234, true) == IDS_SENSOR_MEDIUM_VEHICLE);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 65, 1234, true) == IDS_SENSOR_MEDIUM_VEHICLE);
    CHECK(mc2::hoverNameId(CONTACT_SENSOR_QUALITY_3, 66, 1234, true) == IDS_SENSOR_HEAVY_VEHICLE);
}

TEST_CASE("contact pick: first ridge wins over the fixed point far valley")
{
    // Eye at (0,10), ray z=10-x. A triangular ridge at x=3 occludes
    // the flat valley x=10. The old plane iteration starts at z=0 and
    // converges immediately on x=10, missing the visible ridge entirely.
    auto height = [](double x, double) { return std::max(0.0, 8.0-8.0*std::abs(x-3.0)); };
    double oldX = 0, plane = 0;
    for (int i = 0; i < 3; ++i) {
        oldX = 10-plane;
        const double e = height(oldX, 0);
        if (std::abs(e-plane) < 1) break;
        plane = e;
    }
    CHECK(oldX == 10);
    const double root2 = std::sqrt(2.0);
    const double origin[3] = {0, 0, 10}, dir[3] = {1/root2, 0, -1/root2};
    const double lo[2] = {0, -1}, hi[2] = {12, 1};
    double t = -1;
    REQUIRE(mc2::firstTerrainHit(origin, dir, lo, hi, 0.125, height, t));
    CHECK(t/root2 == doctest::Approx(26.0/9.0));
    CHECK(t/root2 < oldX);
    // Entry from outside, reverse horizontal direction, and an off-map ray.
    const double outside[3] = {-2, 0, 12};
    REQUIRE(mc2::firstTerrainHit(outside, dir, lo, hi, 0.125, height, t));
    CHECK(-2+t/root2 == doctest::Approx(26.0/9.0));
    const double reverseOrigin[3] = {12, 0, 10}, reverseDir[3] = {-1/root2, 0, -1/root2};
    REQUIRE(mc2::firstTerrainHit(reverseOrigin, reverseDir, lo, hi, 0.125, height, t));
    CHECK(12-t/root2 > 3);
    const double miss[3] = {0, 2, 10};
    CHECK_FALSE(mc2::firstTerrainHit(miss, dir, lo, hi, 0.125, height, t));
    const double vertical[3] = {0, 0, -1};
    REQUIRE(mc2::firstTerrainHit(origin, vertical, lo, hi, 0.125, height, t));
    CHECK(t == doctest::Approx(10));
}
