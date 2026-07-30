// tools/terrain_daylight_harness/terrain_daylight_harness.cpp
// TERRAIN-AMBIENT-FLOOR-2 — the daylight gate, executed offline.
//
// WHY THIS HARNESS EXISTS. The predecessor slice (TERRAIN-AMBIENT-FLOOR-1) was
// rejected at merge review because its daylight fade engaged on mc2_10 -- the
// one shipping mission that authors LightDirPitch = 180, the sun exactly at the
// horizon -- and lifted the ground +59.1%. The fade could not fade out: it was
// keyed on the mission flat light, which camera.cpp:2028-2036 deliberately
// floors at the night ambient so it never reaches zero.
//
// The replacement gate must therefore be provable, and a visual capture is the
// wrong instrument for proving it: it costs a launch, it measures one pose on
// one map, and this project has already shipped a capture set whose manifests
// named a commit that was never in the binary. So the entire decision lives in
// GameOS/gameos/utils/terrain_daylight.h as pure math, and this harness runs it
// against the AUTHORED sun pitch of every shipping mission. No window, no
// mission load, no GL, no engine.
//
// The pitch table below is transcribed from the shipping mission camera blocks
// (data/missions/<stem>.fit, `f LightDirPitch` / `f DayLightPitch`); regenerate
// with:  grep -a "LightDirPitch" <srcdata>/missions/*.fit
//
// Build (standalone, no build64/ cache, no deps):
//   cmake -S tools/terrain_daylight_harness -B build64-h-terrain_daylight_harness -A x64
//   cmake --build build64-h-terrain_daylight_harness --config RelWithDebInfo

#include "contract_harness.h"
#include "terrain_daylight.h"

#include <cmath>

using namespace contract_harness;
namespace td = terrain_daylight;

namespace {

struct Mission {
    const char* stem;
    float       authoredPitchDeg;
    bool        expectFloorOff;   // true => gate MUST be exactly 0.0 (byte-identical)
};

// Every campaign mission that authors a camera light block. expectFloorOff is
// derived from the ENGINE's own daylight classifier, not from taste:
// updateDaylight() takes its night arm when the authored pitch is > 180, and
// sets isNight when the folded altitude is < NIGHT_LIGHT_PITCH (10 deg).
const Mission kMissions[] = {
    // --- night / below-horizon: the floor MUST be fully off ---------------
    {"mc2_05", 270.0f,  true},   // sun straight DOWN; engine substitutes a moon at 90
    {"mc2_21", 270.0f,  true},   // same
    {"mc2_10", 180.0f,  true},   // sun AT the horizon -- the rejection case
    {"mc2_19",   0.0f,  true},   // sun AT the horizon, other end
    {"mc2_23",   9.0f,  true},   // 9 deg < NIGHT_LIGHT_PITCH -> engine says night
    // --- daylight: the floor must be fully on -----------------------------
    {"mc2_01",  50.0f,  false},
    {"mc2_03",  46.0f,  false},  // the reported defect
    {"mc2_04",  35.0f,  false},
    {"mc2_06",  25.0f,  false},  // exactly NIGHT_START_PITCH
    {"mc2_07",  34.25f, false},
    {"mc2_08",  37.5f,  false},
    {"mc2_13",  37.0f,  false},
    {"mc2_16",  38.0f,  false},
    {"mc2_18",  33.74f, false},
    {"mc2_20",  44.0f,  false},
    {"mc2_22",  30.0f,  false},
    {"mc2_24",  75.0f,  false},
    {"mc2_demo",91.0f,  false},  // just past zenith; folds to 89, still full day
    // --- dusk band 10..25 deg: partial, and strictly between --------------
    {"mc2_14",  20.0f,  false},
    {"mc2_02",  22.5f,  false},
    {"mc2_09",  22.5f,  false},
    {"mc2_11",  22.5f,  false},
    {"mc2_12",  22.5f,  false},
    {"mc2_15",  22.5f,  false},
    {"mc2_17",  22.5f,  false},
};

// Runtime night state as Camera::update() would report it for an authored
// pitch, INCLUDING the camera.cpp:2033 moon fold. This is the adversary: it is
// the signal the two previously-proposed gates keyed on, and the table below
// shows exactly where it lies.
void runtime_night_state_for(float authoredPitch, bool* isNight, float* nightFactor) {
    float p = std::fmod(authoredPitch, 360.0f);
    if (p < 0.0f) p += 360.0f;
    // camera.cpp:2032-2033 -- updateDaylight() folds a below-horizon sun into a
    // mirrored MOON pitch and writes it back over lightPitch.
    float lightPitch = (p > 180.0f) ? (180.0f - std::fabs(180.0f - p)) : p;
    // camera.cpp:2199-2223 -- Camera::update() then re-derives the flags from it.
    float alt = (lightPitch > 90.0f) ? (180.0f - lightPitch) : lightPitch;
    if (alt < td::kNightFullDeg) { *isNight = true;  *nightFactor = 1.0f; return; }
    *isNight = false;
    *nightFactor = (alt > td::kNightStartDeg)
                 ? 0.0f
                 : (td::kNightStartDeg - lightPitch) / (td::kNightStartDeg - td::kNightFullDeg);
    if (*nightFactor < 0.0f) *nightFactor = 0.0f;   // gos_SetNightState clamps
    if (*nightFactor > 1.0f) *nightFactor = 1.0f;
}

float gate_for(float authoredPitch) {
    bool isNight = false; float nf = 0.0f;
    runtime_night_state_for(authoredPitch, &isNight, &nf);
    return td::level(authoredPitch, isNight, nf);
}

} // namespace

// ---------------------------------------------------------------------------
// The rejection case, isolated.
// ---------------------------------------------------------------------------

static bool test_mc2_10_horizon_sun_gates_fully_off(TestCtx& t) {
    // LightDirPitch = 180. The predecessor lifted this map's ground +59.1% and
    // halved its dark fraction. The gate must be EXACTLY zero, not merely small:
    // the frag multiplies by it, so 0.0 is a byte-identical no-op.
    CH_CHECK(t, gate_for(180.0f) == 0.0f);
    CH_CHECK(t, td::from_authored_pitch(180.0f) == 0.0f);
    return t.failures == 0;
}

static bool test_below_horizon_missions_gate_off_despite_runtime_saying_day(TestCtx& t) {
    // mc2_05 / mc2_21 author pitch 270 (sun straight down). The engine folds
    // that to a MOON at 90 deg and Camera::update() then reports isNight=false,
    // nightFactor=0 -- i.e. "noon, sun overhead". Prove BOTH: that the runtime
    // signal really does say day (so this is not a straw man), and that the
    // authored-pitch gate still shuts the floor off.
    bool isNight = true; float nf = 1.0f;
    runtime_night_state_for(270.0f, &isNight, &nf);
    CH_CHECK(t, isNight == false);          // the adversary really does say "day"
    CH_CHECK(t, nf == 0.0f);                //   ... with zero night factor
    CH_CHECK(t, td::from_runtime_night(isNight, nf) == 1.0f);  // a runtime-only gate = FULL ON
    CH_CHECK(t, td::from_authored_pitch(270.0f) == 0.0f);      // authored gate = OFF
    CH_CHECK(t, gate_for(270.0f) == 0.0f);                     // min() => OFF
    return t.failures == 0;
}

static bool test_runtime_only_gate_would_fail_but_composed_gate_does_not(TestCtx& t) {
    // Regression fence for the whole design decision: if anyone later "simplifies"
    // level() to use the runtime night state alone, these two lines diverge and
    // this test goes red.
    for (const Mission& m : kMissions) {
        if (!m.expectFloorOff) continue;
        bool isNight = false; float nf = 0.0f;
        runtime_night_state_for(m.authoredPitchDeg, &isNight, &nf);
        const float runtimeOnly = td::from_runtime_night(isNight, nf);
        const float composed    = td::level(m.authoredPitchDeg, isNight, nf);
        CH_CHECK(t, composed == 0.0f);
        if (composed != 0.0f) {
            std::fprintf(stderr, "  %s: authored=%.2f composed=%.4f runtimeOnly=%.4f\n",
                         m.stem, m.authoredPitchDeg, composed, runtimeOnly);
        }
    }
    // And specifically: for the 270-deg missions the runtime-only signal is 1.0,
    // so the two are NOT interchangeable.
    bool n5 = false; float f5 = 0.0f;
    runtime_night_state_for(270.0f, &n5, &f5);
    CH_CHECK(t, td::from_runtime_night(n5, f5) != td::level(270.0f, n5, f5));
    return t.failures == 0;
}

// ---------------------------------------------------------------------------
// Whole shipping mission set.
// ---------------------------------------------------------------------------

static bool test_every_night_mission_is_byte_identical(TestCtx& t) {
    for (const Mission& m : kMissions) {
        if (!m.expectFloorOff) continue;
        const float g = gate_for(m.authoredPitchDeg);
        CH_CHECK(t, g == 0.0f);
        if (g != 0.0f)
            std::fprintf(stderr, "  night mission %s (pitch %.2f) gate=%.6f, expected 0\n",
                         m.stem, m.authoredPitchDeg, g);
    }
    return t.failures == 0;
}

static bool test_every_daylight_mission_gets_a_nonzero_floor(TestCtx& t) {
    for (const Mission& m : kMissions) {
        if (m.expectFloorOff) continue;
        const float g = gate_for(m.authoredPitchDeg);
        CH_CHECK(t, g > 0.0f);
        CH_CHECK(t, g <= 1.0f);
        if (!(g > 0.0f))
            std::fprintf(stderr, "  day mission %s (pitch %.2f) gate=%.6f, expected > 0\n",
                         m.stem, m.authoredPitchDeg, g);
    }
    return t.failures == 0;
}

static bool test_the_reported_defect_map_gets_the_full_floor(TestCtx& t) {
    // mc2_03, authored 46 deg -- the map in the user's screenshot. Well above
    // NIGHT_START_PITCH, so the gate must be saturated, not merely nonzero.
    CH_CHECK(t, gate_for(46.0f) == 1.0f);
    CH_CHECK(t, gate_for(50.0f) == 1.0f);   // mc2_01
    CH_CHECK(t, gate_for(75.0f) == 1.0f);   // mc2_24
    return t.failures == 0;
}

// ---------------------------------------------------------------------------
// Shape of the ramp.
// ---------------------------------------------------------------------------

static bool test_ramp_matches_engine_night_band_and_is_monotone(TestCtx& t) {
    // Endpoints pinned to the engine's own constants.
    CH_CHECK(t, td::from_authored_pitch(td::kNightFullDeg)  == 0.0f);   // 10 deg
    CH_CHECK(t, td::from_authored_pitch(td::kNightStartDeg) == 1.0f);   // 25 deg
    CH_CHECK(t, td::from_authored_pitch(9.999f)  == 0.0f);
    CH_CHECK(t, td::from_authored_pitch(25.001f) == 1.0f);
    // Strictly non-decreasing across the whole authored range, and never out of
    // [0,1] -- a gate that overshoots would over-brighten.
    float prev = -1.0f;
    for (int deg = 0; deg <= 90; ++deg) {
        const float g = td::from_authored_pitch((float)deg);
        CH_CHECK(t, g >= 0.0f && g <= 1.0f);
        CH_CHECK(t, g >= prev);
        prev = g;
    }
    return t.failures == 0;
}

static bool test_fold_is_symmetric_about_zenith(TestCtx& t) {
    // 91 deg must behave like 89 deg (mc2_demo), 170 like 10, 180 like 0.
    CH_CHECK(t, td::from_authored_pitch(91.0f)  == td::from_authored_pitch(89.0f));
    CH_CHECK(t, td::from_authored_pitch(170.0f) == td::from_authored_pitch(10.0f));
    CH_CHECK(t, td::from_authored_pitch(180.0f) == td::from_authored_pitch(0.0f));
    CH_CHECK(t, td::from_authored_pitch(155.0f) == td::from_authored_pitch(25.0f));
    return t.failures == 0;
}

static bool test_every_below_horizon_pitch_is_off(TestCtx& t) {
    // The whole lower half-circle, not just the two authored values.
    for (int deg = 181; deg <= 359; ++deg)
        CH_CHECK(t, td::from_authored_pitch((float)deg) == 0.0f);
    // Wrap-around and negatives normalize the same way.
    CH_CHECK(t, td::from_authored_pitch(630.0f) == 0.0f);   // 630 - 360 = 270
    CH_CHECK(t, td::from_authored_pitch(-90.0f) == 0.0f);   // -> 270
    CH_CHECK(t, td::from_authored_pitch(406.0f) == td::from_authored_pitch(46.0f));
    return t.failures == 0;
}

// ---------------------------------------------------------------------------
// Fail-dark and hygiene.
// ---------------------------------------------------------------------------

static bool test_composition_fails_dark_on_disagreement(TestCtx& t) {
    // min(), not product and not max(): if either signal says night, the floor
    // is off. A visual floor that fails dark is a no-op; one that fails bright
    // is the regression that got the predecessor rejected.
    CH_CHECK(t, td::level(46.0f, /*isNight*/true,  0.0f) == 0.0f);  // runtime vetoes
    CH_CHECK(t, td::level(270.0f, /*isNight*/false, 0.0f) == 0.0f); // authored vetoes
    CH_CHECK(t, td::level(46.0f,  false, 1.0f) == 0.0f);            // nightFactor vetoes
    // And it is not a PRODUCT: in the dusk band both signals encode the same
    // ramp, so a product would square it.
    const float dusk = td::level(20.0f, false, (25.0f - 20.0f) / 15.0f);
    const float a = td::from_authored_pitch(20.0f);
    const float r = 1.0f - (25.0f - 20.0f) / 15.0f;
    CH_CHECK(t, dusk == ((a < r) ? a : r));
    CH_CHECK(t, dusk > a * r);       // strictly greater than the squared version
    return t.failures == 0;
}

static bool test_junk_runtime_inputs_cannot_brighten(TestCtx& t) {
    const float nan = std::nanf("");
    CH_CHECK(t, td::from_runtime_night(false, nan) == 0.0f);      // NaN -> full night
    CH_CHECK(t, td::from_runtime_night(false, -5.0f) == 1.0f);    // clamps low
    CH_CHECK(t, td::from_runtime_night(false, 99.0f) == 0.0f);    // clamps high
    CH_CHECK(t, td::level(46.0f, false, nan) == 0.0f);
    return t.failures == 0;
}

static bool test_gate_never_exceeds_one(TestCtx& t) {
    for (int deg = -720; deg <= 1080; ++deg) {
        const float g = td::from_authored_pitch((float)deg);
        CH_CHECK(t, g >= 0.0f && g <= 1.0f);
    }
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("terrain_daylight_harness");
    h.add("mc2_10_horizon_sun_gates_fully_off",        test_mc2_10_horizon_sun_gates_fully_off);
    h.add("below_horizon_gates_off_vs_runtime_day",    test_below_horizon_missions_gate_off_despite_runtime_saying_day);
    h.add("runtime_only_gate_is_not_equivalent",       test_runtime_only_gate_would_fail_but_composed_gate_does_not);
    h.add("every_night_mission_is_byte_identical",     test_every_night_mission_is_byte_identical);
    h.add("every_daylight_mission_gets_a_floor",       test_every_daylight_mission_gets_a_nonzero_floor);
    h.add("reported_defect_map_gets_full_floor",       test_the_reported_defect_map_gets_the_full_floor);
    h.add("ramp_matches_engine_night_band",            test_ramp_matches_engine_night_band_and_is_monotone);
    h.add("fold_is_symmetric_about_zenith",            test_fold_is_symmetric_about_zenith);
    h.add("every_below_horizon_pitch_is_off",          test_every_below_horizon_pitch_is_off);
    h.add("composition_fails_dark_on_disagreement",    test_composition_fails_dark_on_disagreement);
    h.add("junk_runtime_inputs_cannot_brighten",       test_junk_runtime_inputs_cannot_brighten);
    h.add("gate_never_exceeds_one",                    test_gate_never_exceeds_one);
    return h.run(argc, argv);
}
