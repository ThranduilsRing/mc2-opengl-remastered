#include "doctest.h"
#include "../../code/hover_name.h"
#include "../../mclib/terrain_ray_pick.h"
#include "../../mclib/pick_occlusion.h"

TEST_CASE("contact pick: mover in front of a building")
{
    const double origin[3] = {0, 0, 10}, ray[3] = {0, 0, -1};
    const double mechLo[3] = {-1, -1, 6}, mechHi[3] = {1, 1, 8};
    const double buildingLo[3] = {-3, -3, 0}, buildingHi[3] = {3, 3, 5};
    double mechT, buildingT;
    REQUIRE(mc2::pickBoxHit(origin, ray, mechLo, mechHi, mechT));
    REQUIRE(mc2::pickBoxHit(origin, ray, buildingLo, buildingHi, buildingT));
    REQUIRE(mc2::pickBeforeSurface(mechT, 10));
    CHECK(mc2::pickCandidateBefore(true, mechT, 2, false, buildingT, 1));
    CHECK_FALSE(mc2::pickCandidateBefore(false, buildingT, 1, true, mechT, 2));
}

TEST_CASE("contact pick: mover partly behind a building still wins overlap")
{
    const double origin[3] = {0, 0, 10}, ray[3] = {0, 0, -1};
    const double mechLo[3] = {-2, -1, 1}, mechHi[3] = {1, 1, 3};
    const double buildingLo[3] = {-1, -3, 4}, buildingHi[3] = {3, 3, 6};
    double mechT, buildingT;
    REQUIRE(mc2::pickBoxHit(origin, ray, mechLo, mechHi, mechT));
    REQUIRE(mc2::pickBoxHit(origin, ray, buildingLo, buildingHi, buildingT));
    REQUIRE(buildingT < mechT);
    CHECK(mc2::pickCandidateBefore(true, mechT, 2, false, buildingT, 1));
    // Within a class, foreground depth remains stronger than enemy priority.
    CHECK_FALSE(mc2::pickCandidateBefore(true, mechT, 0, true, buildingT, 2));
    CHECK_FALSE(mc2::pickCandidateBefore(false, mechT, 0, false, buildingT, 2));
}

TEST_CASE("contact pick: near miss beside a mech keeps mover forgiveness")
{
    // Cursor lies in the padded rectangle, just outside the actual bounds.
    const double miss[3] = {1.1, 0, 10}, center[3] = {0, 0, 10}, ray[3] = {0, 0, -1};
    const double lo[3] = {-1, -1, 0}, hi[3] = {1, 1, 4};
    auto exact = [&](double& t) { return mc2::pickBoxHit(miss, ray, lo, hi, t); };
    auto forgiven = [&](double& t) { return mc2::pickBoxHit(center, ray, lo, hi, t); };
    double t;
    REQUIRE_FALSE(exact(t));
    const auto hit = mc2::pickDepthWithForgiveness(true, exact, forgiven);
    REQUIRE(hit.has_value());
    t = *hit;
    CHECK(t == doctest::Approx(6));
    CHECK(mc2::pickBeforeSurface(t, 10));
    CHECK_FALSE(mc2::pickDepthWithForgiveness(false, exact, forgiven).has_value());
    // Forgiveness cannot resurrect a mover behind a hill.
    CHECK_FALSE(mc2::pickBeforeSurface(t, 5));
    bool usedFallback = false;
    CHECK(mc2::pickDepthWithForgiveness(true, forgiven,
        [&](double&) { usedFallback = true; return false; }).has_value());
    CHECK_FALSE(usedFallback);
}

TEST_CASE("pick occlusion: tower behind a hill is rejected before ranking")
{
    const double r = std::sqrt(2.0);
    const double origin[3] = {0, 0, 10}, direction[3] = {1/r, 0, -1/r};
    const double mapLo[2] = {0, -1}, mapHi[2] = {12, 1};
    auto ridge = [](double x, double) { return std::max(0.0, 8.0-8.0*std::abs(x-3.0)); };
    double terrainT = 0;
    REQUIRE(mc2::firstTerrainHit(origin, direction, mapLo, mapHi, 0.125, ridge, terrainT));
    const double rearLo[3] = {7, -0.5, 0}, rearHi[3] = {9, 0.5, 4};
    const double frontLo[3] = {1, -0.5, 7}, frontHi[3] = {2, 0.5, 10};
    double rearT = 0, frontT = 0;
    REQUIRE(mc2::pickBoxHit(origin, direction, rearLo, rearHi, rearT));
    REQUIRE(mc2::pickBoxHit(origin, direction, frontLo, frontHi, frontT));
    CHECK_FALSE(mc2::pickBeforeSurface(rearT, terrainT));
    CHECK(mc2::pickBeforeSurface(frontT, terrainT));
    CHECK_FALSE(mc2::pickBeforeSurface(rearT, frontT));
    CHECK(mc2::pickBeforeSurface(frontT, rearT));
    CHECK_FALSE(mc2::pickBeforeSurface(-1, terrainT));
    CHECK_FALSE(mc2::pickBeforeSurface(std::numeric_limits<double>::quiet_NaN(), terrainT));
}

TEST_CASE("pick occlusion: box intersection rejects misses and behind-eye boxes")
{
    const double origin[3] = {0, 0, 10}, down[3] = {0, 0, -1};
    const double lo[3] = {-1, -1, 0}, hi[3] = {1, 1, 4};
    double t = -1;
    REQUIRE(mc2::pickBoxHit(origin, down, lo, hi, t));
    CHECK(t == doctest::Approx(6));
    REQUIRE(mc2::pickBoxHit(origin, down, hi, lo, t));
    CHECK(t == doctest::Approx(6));
    const double up[3] = {0, 0, 1}, miss[3] = {2, 0, 10};
    CHECK_FALSE(mc2::pickBoxHit(origin, up, lo, hi, t));
    CHECK_FALSE(mc2::pickBoxHit(miss, down, lo, hi, t));
}

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
