#include "doctest.h"
#include "../../code/hover_name.h"
#include "../../mclib/terrain_ray_pick.h"
#include "../../mclib/contact_visibility.h"
#include "../../mclib/pick_occlusion.h"
#include <filesystem>
#include <fstream>
#include <iterator>

TEST_CASE("contact visibility: resolved quality and visual overrides share one table")
{
    for (int sensorQuality = CONTACT_SENSOR_QUALITY_1; sensorQuality <= CONTACT_SENSOR_QUALITY_4; ++sensorQuality) {
        for (bool visualRangeAndLOS : {false, true}) {
            // This is the contact-system promotion at contact.cpp add/modifyContact.
            const auto state = mc2::contactVisibility(visualRangeAndLOS ? CONTACT_VISUAL : sensorQuality);
            CHECK(state.model == visualRangeAndLOS);
            CHECK(state.bar == visualRangeAndLOS);
            CHECK(state.shadow == visualRangeAndLOS);
            CHECK(state.tag == (visualRangeAndLOS || sensorQuality >= CONTACT_SENSOR_QUALITY_3));
            CHECK(state.blip);
        }
    }
    for (int quality = CONTACT_NONE; quality <= CONTACT_VISUAL; ++quality) {
        // ContactStatus already incorporates visual range/LOS. Outside visual
        // range the sensor system resolves VISUAL back to its sensor quality.
        const auto state = mc2::contactVisibility(quality);
        CHECK(state.model == (quality == CONTACT_VISUAL));
        CHECK(state.bar == state.model);
        CHECK(state.shadow == state.model);
        CHECK(state.tag == (quality >= CONTACT_SENSOR_QUALITY_3));
        CHECK(state.blip == (quality != CONTACT_NONE));
        CHECK(state.sensorLevel() == (state.model ? 0 : quality));
        const auto ownOrRevealed = mc2::contactVisibility(quality, true);
        CHECK(ownOrRevealed.model);
        CHECK(ownOrRevealed.bar);
        CHECK(ownOrRevealed.tag);
        CHECK(ownOrRevealed.blip);
        CHECK(ownOrRevealed.shadow);
    }
    // A visual -> none -> sensor -> visual transition must carry no old level
    // or alpha admission into the next frame.
    auto state = mc2::contactVisibility(CONTACT_VISUAL);
    state = mc2::contactVisibility(CONTACT_NONE);
    CHECK_FALSE(state.model);
    CHECK_FALSE(state.bar);
    CHECK_FALSE(state.shadow);
    state = mc2::contactVisibility(CONTACT_SENSOR_QUALITY_4);
    CHECK_FALSE(state.model);
    CHECK(state.tag);
    state = mc2::contactVisibility(CONTACT_VISUAL);
    CHECK(state.bar);
    CHECK(state.sensorLevel() == 0);
}

TEST_CASE("contact visibility: production consumers read the shared snapshot")
{
    const auto root = std::filesystem::path(MC2_REPO_ROOT);
    const auto source = [&](const char* file) {
        std::ifstream input(root / file);
        REQUIRE(input.good());
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    for (const char* file : {"code/mech_update.cpp", "code/gvehicl_update.cpp"}) {
        const auto text = source(file);
        CHECK(text.find("view.contact = contactView") != std::string::npos);
        CHECK(text.find("appearance->contactView = view.contact") != std::string::npos);
        CHECK(text.find("setSensorLevel(view.contact.sensorLevel())") != std::string::npos);
        CHECK(text.find("setAlphaValue(view.contact.model ? 0xff : 0)") != std::string::npos);
        CHECK(text.find("if (contactView.shadow)") != std::string::npos);
    }
    for (const char* file : {"mclib/mech3d_render.cpp", "mclib/gvactor_render.cpp"}) {
        const auto text = source(file);
        CHECK(text.find("visible && contactView.model") != std::string::npos);
        CHECK(text.find("contactView.bar && (selected & DRAW_BARS)") != std::string::npos);
        CHECK(text.find("contactView.model && (selected & DRAW_BRACKETS)") != std::string::npos);
        CHECK(text.find("contactView.blip && !contactView.model") != std::string::npos);
    }
    CHECK(source("mclib/gvactor_render.cpp").find("contactView.shadow") != std::string::npos);
    CHECK(source("code/missiongui_update.cpp").find("hoverName = contact.tag") != std::string::npos);
    CHECK(source("code/missiongui.cpp").find("g_hoverTipNameID = contact.tag") != std::string::npos);
    CHECK(source("code/objmgr_pick.cpp").find("contactView.blip") != std::string::npos);
    CHECK(source("code/gametacmap.cpp").find("mover->contactView.blip") != std::string::npos);
    CHECK(source("code/controlgui_render.cpp").find("m->contactView.blip") != std::string::npos);
    CHECK(source("code/infowindow.cpp").find("if (pUnit && !pUnit->contactView.model) setUnit(nullptr)") != std::string::npos);
    CHECK(source("code/missiongui_style.cpp").find("getContactStatus(") == std::string::npos);
}

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

TEST_CASE("pick stack: enemy mover under a tree canopy beats the nearer tree and prop")
{
    using mc2::PickRank;
    // Cursor ray enters a huge island-tree box (t=5) and a prop (t=6) before the enemy mech (t=40).
    const PickRank tree{false, 5.0, 4, 10}, prop{false, 6.0, 1, 11}, enemy{true, 40.0, 0, 99};
    CHECK(mc2::pickStackBefore(enemy, tree));
    CHECK(mc2::pickStackBefore(enemy, prop));
    CHECK_FALSE(mc2::pickStackBefore(tree, enemy));
    // Unit in the open (no scenery on the ray): unchanged, mover is the only candidate.
    CHECK_FALSE(mc2::pickStackBefore(enemy, enemy));
    // Friendly and enemy movers: depth then priority, as before.
    const PickRank friendlyNear{true, 20.0, 2, 5}, enemyFar{true, 40.0, 0, 6};
    CHECK(mc2::pickStackBefore(friendlyNear, enemyFar));
    CHECK(mc2::pickStackBefore(PickRank{true, 20.0, 0, 7}, PickRank{true, 20.0, 2, 5}));
    // An ineligible mover (unseen contact) does not outrank a visible static object.
    CHECK(mc2::pickStackBefore(PickRank{false, 6.0, 1, 11}, PickRank{false, 40.0, 0, 3}));
}

TEST_CASE("pick stack: a unit the cursor hits outranks a farther-forgiven-but-nearer one")
{
    using mc2::PickRank;
    // Grazing view: A stands in front (t=100) and is only reached by padded-rect forgiveness; B (t=140) is hit exactly.
    const PickRank forgivenNear{true, 100.0, 0, 1, true}, exactFar{true, 140.0, 2, 2, false};
    CHECK(mc2::pickStackBefore(exactFar, forgivenNear));
    CHECK_FALSE(mc2::pickStackBefore(forgivenNear, exactFar));
    // Two exact (or two forgiven) hits still resolve by depth.
    CHECK(mc2::pickStackBefore(PickRank{true, 100.0, 2, 1, false}, PickRank{true, 140.0, 0, 2, false}));
    CHECK(mc2::pickStackBefore(PickRank{true, 100.0, 2, 1, true}, PickRank{true, 140.0, 0, 2, true}));
    // Scenery stays behind any eligible mover, forgiven or not.
    CHECK(mc2::pickStackBefore(forgivenNear, PickRank{false, 5.0, 1, 3, false}));
}

TEST_CASE("pick occlusion: a mover slightly behind the visual surface at its feet is still pickable")
{
    // Observed on mc2_01: object_t 1318.7 vs terrain_t 1313.1 for a visible enemy under the cursor.
    CHECK_FALSE(mc2::pickBeforeSurface(1318.7, 1313.1));
    const double lo[3] = {-5, -10, 0}, hi[3] = {5, 10, 20};
    const double feet[3] = {0, 0, 1}, ridge[3] = {-20, 0, 1};
    CHECK(mc2::pickMoverBeforeSurface(1318.7, 1313.1, feet, lo, hi));
    CHECK_FALSE(mc2::pickMoverBeforeSurface(1400.0, 1313.1, feet, lo, hi));
    // Regression: the old diagonal slack accepted t=120 behind a ridge at t=100.
    CHECK_FALSE(mc2::pickMoverBeforeSurface(120, 100, ridge, lo, hi));
    CHECK(mc2::pickMoverBeforeSurface(99, 100, ridge, lo, hi));
    const double sideRidge[3] = {0, 11, 1};
    CHECK_FALSE(mc2::pickMoverBeforeSurface(120, 100, sideRidge, lo, hi));
}
