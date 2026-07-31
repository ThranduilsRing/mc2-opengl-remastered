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
// WHICH SIGNAL. Not mission flat light (cannot reach zero, see above). Not the
// RUNTIME sun altitude or the RUNTIME night flags either, because both are
// derived from Camera::lightPitch AFTER camera.cpp:2033 has overwritten it:
//
//     float moonPitch = 180.0f - fabs(180.0f - lightPitch);
//     lightPitch = moonPitch;                       // camera.cpp:2032-2033
//
// On a mission that authors a pitch BELOW the horizon, updateDaylight()
// substitutes the moon at the mirrored altitude AND WRITES IT BACK OVER
// lightPitch; Camera::update() (camera.cpp:2196-2223) then re-derives
// isNight/nightFactor from that substituted pitch and concludes "day".
// mc2_05 and mc2_21 author LightDirPitch = 270 (sun straight DOWN): after the
// fold lightPitch = 90, so terrainLightDir.z = sin(90) normalized = +0.707 and
// nightFactor = 0 -- the runtime signals then describe a NOON SUN OVERHEAD on a
// mission the engine itself classified as night and whose light colours it has
// already set to nightLight*.
//
// PRECISION ABOUT WHEN THAT BITES, because it is worth being exact rather than
// alarming: the fold only happens if updateDaylight() actually runs, and IT
// NEVER DOES during a mission. CORRECTED 2026-07-30 (PROP-BAKE-TIME-1) -- this
// paragraph previously said the fold was merely skipped on a cold start and
// "becomes wrong on any later activate() taken with the camera already
// ready+active -- e.g. saveload.cpp:1654 after loading a save". That is not
// right, and the `ready && active` early-exit is not the reason. activate() is
// VIRTUAL (mclib/camera.h:1285), the in-mission camera is a GameCamera
// (code/mission.cpp:5108 `eye = new GameCamera`), and GameCamera::activate
// (code/gamecam.cpp:1024) fully overrides the base without chaining to it -- its
// own updateDaylight(true) is commented out at gamecam.cpp:1060. All three
// game-side callers (mechcmd2.cpp:1847, logistics.cpp:875, saveload.cpp:1654)
// dispatch there, and the per-frame call site is commented out too
// (camera.cpp:2238). So lightPitch keeps its authored value on EVERY path
// including post-load, Camera::update() computes lightAltitude = 180-270 = -90
// -> isNight = true, and the runtime signals are consistently CORRECT rather
// than unreliably right. The only surviving caller of updateDaylight() is
// Camera::setDaylightFraction (the MC2_UI_COMMAND_FILE sun driver), which is not
// a default-launch path.
// The authored pitch is 270 regardless, so this gate does not care either way --
// which is why the correction changes no behaviour here, only what a reader may
// safely cite from this file.
//
// (Root cause of the fold leaking out at all: Camera::update()'s guard
// `if (day2NightTransitionTime <= 0.0f)` is commented out at camera.cpp:2192-
// 2195 while its comment still says the flags "have already been set in
// updateDaylight". Repairing that is a global change to isNight/nightFactor --
// runway edge lights, night fog dim, prop night branches -- and is emphatically
// NOT in scope for a terrain shading floor.)
//
// The AUTHORED pitch (Camera::dayLightPitch) is never folded and never
// animated: updateDaylight() writes lightPitch and dayLightTime but never
// dayLightPitch, and it is only ever called with bInitialize=true from
// Camera::activate() (camera.cpp:2766) because the per-frame call site is
// commented out (camera.cpp:2180, "WE ARE NEVER TRANSITIONING ANYMORE"). So it
// is a stable, per-mission constant, and it is the value the engine's OWN
// day/night classifier looks at before it substitutes anything.
//
// Everything here is constexpr-friendly, allocation-free, and side-effect-free.

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
    if (nightFactor < 0.0f)     nightFactor = 0.0f;
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
