#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "terrain_daylight.h"
#include "../../shaders/include/terrain_cliff_math.hglsl"

#include <cmath>

namespace {
namespace td = terrain_daylight;

namespace {

struct Mission {
    const char* stem;
    float       authoredPitchDeg;
    bool        expectFloorOff;
};

const Mission kMissions[] = {

    {"mc2_05", 270.0f,  true},
    {"mc2_21", 270.0f,  true},
    {"mc2_10", 180.0f,  true},
    {"mc2_19",   0.0f,  true},
    {"mc2_23",   9.0f,  true},

    {"mc2_01",  50.0f,  false},
    {"mc2_03",  46.0f,  false},
    {"mc2_04",  35.0f,  false},
    {"mc2_06",  25.0f,  false},
    {"mc2_07",  34.25f, false},
    {"mc2_08",  37.5f,  false},
    {"mc2_13",  37.0f,  false},
    {"mc2_16",  38.0f,  false},
    {"mc2_18",  33.74f, false},
    {"mc2_20",  44.0f,  false},
    {"mc2_22",  30.0f,  false},
    {"mc2_24",  75.0f,  false},
    {"mc2_demo",91.0f,  false},

    {"mc2_14",  20.0f,  false},
    {"mc2_02",  22.5f,  false},
    {"mc2_09",  22.5f,  false},
    {"mc2_11",  22.5f,  false},
    {"mc2_12",  22.5f,  false},
    {"mc2_15",  22.5f,  false},
    {"mc2_17",  22.5f,  false},
};

void runtime_night_state_for(float authoredPitch, bool* isNight, float* nightFactor) {
    float p = std::fmod(authoredPitch, 360.0f);
    if (p < 0.0f) p += 360.0f;

    float lightPitch = (p > 180.0f) ? (180.0f - std::fabs(180.0f - p)) : p;

    float alt = (lightPitch > 90.0f) ? (180.0f - lightPitch) : lightPitch;
    if (alt < td::kNightFullDeg) { *isNight = true;  *nightFactor = 1.0f; return; }
    *isNight = false;
    *nightFactor = (alt > td::kNightStartDeg)
                 ? 0.0f
                 : (td::kNightStartDeg - lightPitch) / (td::kNightStartDeg - td::kNightFullDeg);
    if (*nightFactor < 0.0f) *nightFactor = 0.0f;
    if (*nightFactor > 1.0f) *nightFactor = 1.0f;
}

float gate_for(float authoredPitch) {
    bool isNight = false; float nf = 0.0f;
    runtime_night_state_for(authoredPitch, &isNight, &nf);
    return td::level(authoredPitch, isNight, nf);
}

}

TEST_CASE("terrain_daylight_harness/mc2_10_horizon_sun_gates_fully_off") {

    CHECK((gate_for(180.0f) == 0.0f));
    CHECK((td::from_authored_pitch(180.0f) == 0.0f));
    return;
}

TEST_CASE("terrain_daylight_harness/below_horizon_gates_off_vs_runtime_day") {

    bool isNight = true; float nf = 1.0f;
    runtime_night_state_for(270.0f, &isNight, &nf);
    CHECK((isNight == false));
    CHECK((nf == 0.0f));
    CHECK((td::from_runtime_night(isNight, nf) == 1.0f));
    CHECK((td::from_authored_pitch(270.0f) == 0.0f));
    CHECK((gate_for(270.0f) == 0.0f));
    return;
}

TEST_CASE("terrain_daylight_harness/runtime_only_gate_is_not_equivalent") {

    for (const Mission& m : kMissions) {
        if (!m.expectFloorOff) continue;
        bool isNight = false; float nf = 0.0f;
        runtime_night_state_for(m.authoredPitchDeg, &isNight, &nf);
        const float runtimeOnly = td::from_runtime_night(isNight, nf);
        const float composed    = td::level(m.authoredPitchDeg, isNight, nf);
        CHECK((composed == 0.0f));
        if (composed != 0.0f) {
            std::fprintf(stderr, "  %s: authored=%.2f composed=%.4f runtimeOnly=%.4f\n",
                         m.stem, m.authoredPitchDeg, composed, runtimeOnly);
        }
    }

    bool n5 = false; float f5 = 0.0f;
    runtime_night_state_for(270.0f, &n5, &f5);
    CHECK((td::from_runtime_night(n5, f5) != td::level(270.0f, n5, f5)));
    return;
}

TEST_CASE("terrain_daylight_harness/every_night_mission_is_byte_identical") {
    for (const Mission& m : kMissions) {
        if (!m.expectFloorOff) continue;
        const float g = gate_for(m.authoredPitchDeg);
        CHECK((g == 0.0f));
        if (g != 0.0f)
            std::fprintf(stderr, "  night mission %s (pitch %.2f) gate=%.6f, expected 0\n",
                         m.stem, m.authoredPitchDeg, g);
    }
    return;
}

TEST_CASE("terrain_daylight_harness/every_daylight_mission_gets_a_floor") {
    for (const Mission& m : kMissions) {
        if (m.expectFloorOff) continue;
        const float g = gate_for(m.authoredPitchDeg);
        CHECK((g > 0.0f));
        CHECK((g <= 1.0f));
        if (!(g > 0.0f))
            std::fprintf(stderr, "  day mission %s (pitch %.2f) gate=%.6f, expected > 0\n",
                         m.stem, m.authoredPitchDeg, g);
    }
    return;
}

TEST_CASE("terrain_daylight_harness/reported_defect_map_gets_full_floor") {

    CHECK((gate_for(46.0f) == 1.0f));
    CHECK((gate_for(50.0f) == 1.0f));
    CHECK((gate_for(75.0f) == 1.0f));
    return;
}

TEST_CASE("terrain_daylight_harness/ramp_matches_engine_night_band") {

    CHECK((td::from_authored_pitch(td::kNightFullDeg)  == 0.0f));
    CHECK((td::from_authored_pitch(td::kNightStartDeg) == 1.0f));
    CHECK((td::from_authored_pitch(9.999f)  == 0.0f));
    CHECK((td::from_authored_pitch(25.001f) == 1.0f));

    float prev = -1.0f;
    for (int deg = 0; deg <= 90; ++deg) {
        const float g = td::from_authored_pitch((float)deg);
        CHECK((g >= 0.0f && g <= 1.0f));
        CHECK((g >= prev));
        prev = g;
    }
    return;
}

TEST_CASE("terrain_daylight_harness/fold_is_symmetric_about_zenith") {

    CHECK((td::from_authored_pitch(91.0f)  == td::from_authored_pitch(89.0f)));
    CHECK((td::from_authored_pitch(170.0f) == td::from_authored_pitch(10.0f)));
    CHECK((td::from_authored_pitch(180.0f) == td::from_authored_pitch(0.0f)));
    CHECK((td::from_authored_pitch(155.0f) == td::from_authored_pitch(25.0f)));
    return;
}

TEST_CASE("terrain_daylight_harness/every_below_horizon_pitch_is_off") {

    for (int deg = 181; deg <= 359; ++deg)
        CHECK((td::from_authored_pitch((float)deg) == 0.0f));

    CHECK((td::from_authored_pitch(630.0f) == 0.0f));
    CHECK((td::from_authored_pitch(-90.0f) == 0.0f));
    CHECK((td::from_authored_pitch(406.0f) == td::from_authored_pitch(46.0f)));
    return;
}

TEST_CASE("terrain_daylight_harness/composition_fails_dark_on_disagreement") {

    CHECK((td::level(46.0f, true,  0.0f) == 0.0f));
    CHECK((td::level(270.0f, false, 0.0f) == 0.0f));
    CHECK((td::level(46.0f,  false, 1.0f) == 0.0f));

    const float dusk = td::level(20.0f, false, (25.0f - 20.0f) / 15.0f);
    const float a = td::from_authored_pitch(20.0f);
    const float r = 1.0f - (25.0f - 20.0f) / 15.0f;
    CHECK((dusk == ((a < r) ? a : r)));
    CHECK((dusk > a * r));
    return;
}

TEST_CASE("terrain_daylight_harness/junk_runtime_inputs_cannot_brighten") {
    const float nan = std::nanf("");
    CHECK((td::from_runtime_night(false, nan) == 0.0f));
    CHECK((td::from_runtime_night(false, -5.0f) == 1.0f));
    CHECK((td::from_runtime_night(false, 99.0f) == 0.0f));
    CHECK((td::level(46.0f, false, nan) == 0.0f));
    return;
}

TEST_CASE("terrain_daylight_harness/gate_never_exceeds_one") {
    for (int deg = -720; deg <= 1080; ++deg) {
        const float g = td::from_authored_pitch((float)deg);
        CHECK((g >= 0.0f && g <= 1.0f));
    }
    return;
}


TEST_CASE("terrain_daylight_harness/cliff_prelit_ambient_is_not_sun_shadowed") {
    CHECK(cliffDirectShare(-0.8f, 0.2f) == 0.0f);
    CHECK(cliffDirectShare(0.0f, 0.2f) == 0.0f);
    CHECK(cliffDirectShare(0.8f, 0.2f) == doctest::Approx(0.8f));
    // A fully occluded sun retains the ambient already present in the burnin.
    for (float nl : {0.0f, 0.2f, 0.8f, 1.0f}) {
        const float sun = cliffDirectShare(nl, 0.2f);
        const float shadowed = 1.0f - sun;
        CHECK(shadowed >= 0.0f);
        CHECK(shadowed <= 1.0f);
        CHECK((shadowed + sun) == doctest::Approx(1.0f));
    }
}

TEST_CASE("terrain_daylight_harness/cliff_delight_is_spatial_bounded_and_hue_preserving") {
    CHECK(cliffDelightGain(0.3f, 0.3f) == 1.0f);
    CHECK(cliffDelightGain(0.05f, 0.05f) == 1.0f);
    CHECK(cliffDelightGain(0.4f, 0.3f) == 1.0f);
    CHECK(cliffDelightGain(0.0f, 0.4f) == 1.0f);
    CHECK(cliffDelightGain(0.01f, 0.4f) == 1.0f);
    CHECK(cliffDelightGain(0.1f, 0.8f) == 2.5f);
    const float gain = cliffDelightGain(0.15f, 0.3f);
    CHECK(gain == doctest::Approx(2.0f));
    CHECK((0.12f * gain) / (0.18f * gain) == doctest::Approx(0.12f / 0.18f));
}

TEST_CASE("terrain_daylight_harness/cliff_filter_preserves_airfield_and_planes") {
    CHECK(cliffFilteredHeight(100.0f, 400.0f, 400.0f, 0.0f) == 100.0f);
    // A slight local feature on gentle terrain is retained byte-exactly.
    CHECK(cliffFilteredHeight(100.0f, 399.0f, 398.0f, 24.0f) == 100.0f);
    // Binomial filtering preserves steep planes (sums are symmetric about c).
    CHECK(cliffFilteredHeight(100.0f, 400.0f, 400.0f, 128.0f) == 100.0f);
}

TEST_CASE("terrain_daylight_harness/rock_reaches_rounded_crest_but_not_flat_top") {
    CHECK(cliffRockWeight(1.0f) == 0.0f);
    CHECK(cliffRockWeight(0.98f) == doctest::Approx(0.0f));
    CHECK(cliffRockWeight(0.95f) > 0.1f);
    CHECK(cliffRockWeight(0.90f) > 0.6f);
    CHECK(cliffRockWeight(0.85f) == doctest::Approx(1.0f));
    CHECK(cliffRockWeight(0.55f) == 1.0f);
}

TEST_CASE("terrain_daylight_harness/cliff_filter_rejects_source_grid_ripple") {
    // An 8-wu ripple with a 128-wu period rides on a steep planar cliff.
    // At 64-wu taps, east/west have opposite ripple phase; north/south agree.
    const float c = 108.0f;
    const float cardinal = (100.0f + 128.0f - 8.0f) + (100.0f - 128.0f - 8.0f) + 2.0f * 108.0f;
    const float diagonal = 4.0f * 92.0f;
    CHECK(cliffFilteredHeight(c, cardinal, diagonal, 144.0f) == doctest::Approx(100.0f));
    CHECK(cliffFilteredHeight(92.0f, 400.0f, 432.0f, 144.0f) == doctest::Approx(100.0f));
}
}
