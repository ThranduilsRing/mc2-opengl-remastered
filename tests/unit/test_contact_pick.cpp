#include "doctest.h"
#include "../../code/hover_name.h"
#include "../../mclib/terrain_ray_pick.h"
#include "../../mclib/contact_visibility.h"
#include <filesystem>
#include <fstream>
#include <iterator>

TEST_CASE("contact visibility: resolved quality and visual overrides share one table")
{
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
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
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
