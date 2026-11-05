#pragma once
// GameOS/gameos/utils/terrain_daylight.h
// TERRAIN-AMBIENT-FLOOR-2 — the daylight gate, as PURE MATH.
//
// WHY THIS IS A HEADER AND NOT A LAMBDA IN THE UPLOAD PATH.
// The predecessor slice (TERRAIN-AMBIENT-FLOOR-1) was rejected at merge review
// because its daylight fade could not fade out: it scaled by the mission FLAT
// LIGHT, and camera.cpp:2028-2036 deliberately pins the night path to fixed
// night ambient/light values ("DO NOT ever get darker then the night Ambient.
// ... very unplayable in the pitch dark"), so the fade had a hard non-zero
// floor. On mc2_10 (authored LightDirPitch = 180, the sun exactly AT the
// horizon) that produced a +59.1% ground lift on the one map that should have
// gated off hardest. A gate that can be WRONG THIS WAY must be provable
// without a capture, so the whole decision lives here, engine-free, and
// tools/terrain_daylight_harness executes it against every authored mission
// pitch in the shipping data. No window, no mission load, no GPU.
//
// WHICH SIGNAL. Not mission flat light (cannot reach zero, see above). The
// AUTHORED pitch (Camera::dayLightPitch) instead, because it is the one input
// that is correct on every path and cannot be rewritten behind this gate's back.
//
// THE STATE OF THE MOON FOLD, STATED TRUTHFULLY AND UP FRONT so that no reader
// can stop halfway through this block and leave with the wrong model. The fold
// is real code but it is UNREACHABLE IN A MISSION. updateDaylight()'s night
// branch substitutes a moon at the mirrored altitude and writes it back:
//
//     float moonPitch = 180.0f - fabs(180.0f - lightPitch);
//     lightPitch = moonPitch;                       // camera.cpp:2079-2080
//
// and Camera::update() re-derives isNight/nightFactor from lightPitch every
// frame (camera.cpp:2254-2281, its `day2NightTransitionTime` guard commented
// out), so IF the fold ever landed, mc2_05 / mc2_21 (authored 270, sun straight
// DOWN) would read back as a noon sun overhead on a mission the engine had just
// classified as night. IT NEVER LANDS. updateDaylight() NEVER RUNS IN A MISSION:
// activate() is VIRTUAL (mclib/camera.h:1285), the in-mission camera is a
// GameCamera (code/mission.cpp:5108 `eye = new GameCamera`), and
// GameCamera::activate (code/gamecam.cpp:1024) fully overrides the base without
// chaining to it -- its own updateDaylight(true) is commented out at
// gamecam.cpp:1060. All three game-side callers (mechcmd2.cpp:1847,
// logistics.cpp:875, saveload.cpp:1654) dispatch there, and the per-frame call
// site is commented out too (camera.cpp:2238). The only surviving caller is
// Camera::setDaylightFraction (the MC2_UI_COMMAND_FILE sun driver, not a
// default-launch path), and even that RESTORES the sun angle immediately after
// (camera.cpp:1840, setSunPitch) precisely so the fold cannot leak. So the fold
// reaches no downstream reader on ANY path, cold or post-load.
// CORRECTED 2026-07-30 (PROP-BAKE-TIME-1): this block previously said the fold
// was merely skipped on a cold start and "becomes wrong on any later activate()
// taken with the camera already ready+active". That was wrong, and the
// `ready && active` early-exit is not the reason -- the missing chain is.
// RE-VERIFIED AND RESTRUCTURED 2026-07-30 (DAYLIGHT-GATE-RECHECK-1), which also
// found the same false claim still standing in two other files and fixed them:
// mclib/camera.h getDayLightPitch() and GameOS/gameos/gameos_graphics.cpp
// gos_SetTerrainDaylightLevel.
//
// CONSEQUENCE FOR THIS GATE -- the reassuring part, so it is worth being plain
// about. lightPitch keeps its AUTHORED value everywhere, so Camera::update()
// computes lightAltitude = 180-270 = -90 -> isNight = true on mc2_05/mc2_21, and
// the runtime arm is CORRECT rather than lying. The two arms therefore AGREE on
// every shipping mission. The AUTHORED arm alone already returns exactly 0.000
// on all five night maps (mc2_10 pitch 180 -> altitude 0 <= kNightFullDeg), so
// the mc2_10 protection that got the predecessor rejected does NOT depend on the
// runtime arm at all. Measured, not assumed: DAYLIGHT-GATE-RECHECK-1 deleted the
// runtime arm in the live tree (level() -> return a) and all ten mission-derived
// harness tests still passed; only the two synthetic fail-dark tests went red.
// Do NOT conclude from that the runtime arm is spare and can be dropped: the
// reverse substitution (level() -> return r, a "simplification" to the runtime
// signal alone) takes the harness to 4 failures, because it is the arm that
// would go wrong first if the fold were ever revived. Keep the min().
//
// (Adjacent inversion, noted so nobody "fixes" it casually: Camera::update()'s
// guard `if (day2NightTransitionTime <= 0.0f)` is commented out at
// camera.cpp:2254 while its comment still says the flags "have already been set
// in updateDaylight" -- so that block reads as a fallback when it is in fact the
// ONLY producer of isNight/nightFactor. Repairing the comment-vs-code mismatch
// there is a global change to isNight/nightFactor -- runway edge lights, night
// fog dim, prop night branches -- and is emphatically NOT in scope for a terrain
// shading floor.)
//
// The AUTHORED pitch (Camera::dayLightPitch) is never folded and never
// animated: updateDaylight() writes lightPitch and dayLightTime but never
// dayLightPitch. Its bInitialize=true call is Camera::activate()
// (camera.cpp:2824, dead per above) and the per-frame call site is commented out
// (camera.cpp:2238, "WE ARE NEVER TRANSITIONING ANYMORE"). So dayLightPitch is a
// stable, per-mission constant equal to the authored LightDirPitch on all 25
// shipping missions (verified against mc2-srcdata/missions/*.fit), and it is the
// value the engine's OWN day/night classifier looks at before it substitutes
// anything.
//
// Everything here is constexpr-friendly, allocation-free, and side-effect-free.

#include <algorithm>
#include <cmath>

namespace terrain_daylight {

// Mirrors camera.cpp:149-150. Duplicated deliberately: this header must not
// pull in mclib/camera.h (which drags Stuff, gameos, the whole engine) or the
// harness stops being standalone. The duplication is pinned by
// scripts/check-terrain-daylight-constants.py, registered in check-contracts.sh
// as terrain_daylight_consts, whose --self-test injects a drift in each constant
// and requires the check to go red. (This comment previously asserted the pin
// existed before the checker did. It does now.)
constexpr float kNightFullDeg  = 10.0f;   // NIGHT_LIGHT_PITCH  — engine says "night"
constexpr float kNightStartDeg = 25.0f;   // NIGHT_START_PITCH  — engine says "full day"

inline float smoothstep01(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Fold an authored LightDirPitch/DayLightPitch to the engine's own
// "lightAltitude" (camera.cpp:1848-1850 / 2199-2201).
//
// Returns a NEGATIVE altitude when the authored pitch puts the light source
// BELOW the horizon (pitch > 180). That is not an approximation: it is exactly
// how updateDaylight() branches -- `if (180.0f >= lightPitch)` takes the
// daytime arm and everything else takes the `isNight = true; nightFactor = 1`
// arm at camera.cpp:2013-2018 -- before the moon substitution rewrites
// lightPitch and hides the fact from every downstream reader.
inline float authored_altitude_deg(float pitchDeg) {
    float p = std::fmod(pitchDeg, 360.0f);
    if (p < 0.0f) p += 360.0f;
    if (p > 180.0f) return -1.0f;                    // below horizon => night
    return (p > 90.0f) ? (180.0f - p) : p;
}

// Daylight level in [0,1] from the authored pitch alone.
inline float from_authored_pitch(float pitchDeg) {
    const float alt = authored_altitude_deg(pitchDeg);
    if (alt <= kNightFullDeg) return 0.0f;
    return smoothstep01((alt - kNightFullDeg) / (kNightStartDeg - kNightFullDeg));
}

// Daylight level in [0,1] from the engine's runtime night state
// (Camera::isNight / Camera::nightFactor, pushed via gos_SetNightState).
// NOTE ON THE CLAMP ORDER. It is deliberately upper-bound-first, so that a NaN
// nightFactor resolves to FULL NIGHT (gate 0), not full day. The obvious
// `if (!(nf >= 0)) nf = 0;` idiom used elsewhere in this codebase to swallow NaN
// is exactly backwards here: it would turn a garbage night factor into a
// fully-ON brightening gate. The harness test junk_runtime_inputs_cannot_brighten
// caught that on the first run of this file and exists to keep catching it.
inline float from_runtime_night(bool isNight, float nightFactor) {
    if (isNight) return 0.0f;
    if (!(nightFactor <= 1.0f)) nightFactor = 1.0f;  // >1 or NaN -> full night
    nightFactor = (std::max)(nightFactor, 0.0f);
    return 1.0f - nightFactor;
}

// FAIL-DARK COMPOSITION. Two independent signals, and we take the MINIMUM, not
// the product:
//   * min, not product, so a mission sitting in the engine's 10-25 degree dusk
//     band is not faded twice (both signals encode the same ramp there, so a
//     product would square it);
//   * min, not max/or, so if the two ever DISAGREE the brightening gate turns
//     OFF. A visual floor that fails dark is a no-op; one that fails bright is
//     the mc2_10 regression that got the predecessor rejected.
inline float level(float authoredPitchDeg, bool isNight, float nightFactor) {
    const float a = from_authored_pitch(authoredPitchDeg);
    const float r = from_runtime_night(isNight, nightFactor);
    return (a < r) ? a : r;
}

} // namespace terrain_daylight
