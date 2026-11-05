// mclib/object_admission_predicate.cpp
#include "mc2_czstring.h"
#include "object_admission_predicate.h"
#include "Gates/gate.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "stuff/stuff.hpp"   // Stuff::Vector4D
#include "math/mc2_xform.h" // glm::vec3 (for logProjectZBypassDisagreement)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "mc2fmt.h"

namespace {

bool             s_initialized = false;
ObjectAdmissionPredicateMode s_mode = ObjectAdmissionPredicateMode::Legacy;

const char* modeLabel(ObjectAdmissionPredicateMode m) {
    return (m == ObjectAdmissionPredicateMode::Modern) ? "modern" : "legacy";
}

} // namespace

void objectAdmissionPredicate_init() {
    if (s_initialized) return;
    if (mc2::gate::is<"MC2_OBJECT_ADMISSION_PREDICATE", "legacy">()) {
        s_mode = ObjectAdmissionPredicateMode::Legacy;
    } else {
        s_mode = ObjectAdmissionPredicateMode::Modern;  // default
    }
    s_initialized = true;
    mc2::fmt::print_out("[INSTR v1] object_admission_mode={}\n", mc2::fmt::safe(modeLabel(s_mode)));
    std::fflush(stdout);
}

ObjectAdmissionPredicateMode objectAdmissionPredicateMode() {
    // Lazy init — startup ordering is non-load-bearing.
    objectAdmissionPredicate_init();
    return s_mode;
}

namespace {

bool                         s_effectInitialized = false;
EffectAdmissionPredicateMode s_effectMode = EffectAdmissionPredicateMode::Legacy;

const char* effectModeLabel(EffectAdmissionPredicateMode m) {
    return (m == EffectAdmissionPredicateMode::Modern) ? "modern" : "legacy";
}

} // namespace

void effectAdmissionPredicate_init() {
    if (s_effectInitialized) return;
    if (mc2::gate::is<"MC2_EFFECT_ADMISSION_PREDICATE", "legacy">()) {
        s_effectMode = EffectAdmissionPredicateMode::Legacy;
    } else {
        s_effectMode = EffectAdmissionPredicateMode::Modern;  // default
    }
    s_effectInitialized = true;
    mc2::fmt::print_out("[INSTR v1] effect_admission_mode={}\n", mc2::fmt::safe(effectModeLabel(s_effectMode)));
    std::fflush(stdout);
}

EffectAdmissionPredicateMode effectAdmissionPredicateMode() {
    // Lazy init - startup ordering is non-load-bearing.
    effectAdmissionPredicate_init();
    return s_effectMode;
}

namespace {

bool                           s_lshadowInitialized = false;
LightingShadowPredicateMode    s_lshadowMode = LightingShadowPredicateMode::Modern;

const char* lshadowModeLabel(LightingShadowPredicateMode m) {
    return (m == LightingShadowPredicateMode::Modern) ? "modern" : "legacy";
}

} // namespace

void lightingShadowPredicate_init() {
    if (s_lshadowInitialized) return;
    if (mc2::gate::is<"MC2_LIGHTING_SHADOW_PREDICATE_MODE", "Legacy">()) {
        s_lshadowMode = LightingShadowPredicateMode::Legacy;
    } else {
        s_lshadowMode = LightingShadowPredicateMode::Modern;  // default
    }
    s_lshadowInitialized = true;
    MC2_DIAGF("[OBJECT_ADMISSION_PREDICATE v1] event=mode_select wrapper=lighting_shadow mode=%s\n",
                lshadowModeLabel(s_lshadowMode));
    std::fflush(stdout);
}

LightingShadowPredicateMode lightingShadowPredicateMode() {
    // Lazy init - startup ordering is non-load-bearing.
    lightingShadowPredicate_init();
    return s_lshadowMode;
}

namespace {

bool                        s_dbgoverlayInitialized = false;
DebugOverlayPredicateMode   s_dbgoverlayMode = DebugOverlayPredicateMode::Modern;

const char* dbgoverlayModeLabel(DebugOverlayPredicateMode m) {
    return (m == DebugOverlayPredicateMode::Modern) ? "modern" : "legacy";
}

} // namespace

void debugOverlayPredicate_init() {
    if (s_dbgoverlayInitialized) return;
    if (mc2::gate::is<"MC2_DEBUG_OVERLAY_PREDICATE_MODE", "Legacy">()) {
        s_dbgoverlayMode = DebugOverlayPredicateMode::Legacy;
    } else {
        s_dbgoverlayMode = DebugOverlayPredicateMode::Modern;  // default
    }
    s_dbgoverlayInitialized = true;
    MC2_DIAGF("[OBJECT_ADMISSION_PREDICATE v1] event=mode_select wrapper=debug_overlay mode=%s\n",
                dbgoverlayModeLabel(s_dbgoverlayMode));
    std::fflush(stdout);
}

DebugOverlayPredicateMode debugOverlayPredicateMode() {
    // Lazy init - startup ordering is non-load-bearing.
    debugOverlayPredicate_init();
    return s_dbgoverlayMode;
}

namespace {

bool                          s_selfpickInitialized = false;
SelectionPickingPredicateMode s_selfpickMode = SelectionPickingPredicateMode::Modern;

const char* selfpickModeLabel(SelectionPickingPredicateMode m) {
    return (m == SelectionPickingPredicateMode::Modern) ? "modern" : "legacy";
}

} // namespace

void selectionPickingPredicate_init() {
    if (s_selfpickInitialized) return;
    if (mc2::gate::is<"MC2_SELECTION_PICKING_PREDICATE_MODE", "Legacy">()) {
        s_selfpickMode = SelectionPickingPredicateMode::Legacy;
    } else {
        s_selfpickMode = SelectionPickingPredicateMode::Modern;  // default
    }
    s_selfpickInitialized = true;
    MC2_DIAGF("[OBJECT_ADMISSION_PREDICATE v1] event=mode_select wrapper=selection_picking mode=%s\n",
                selfpickModeLabel(s_selfpickMode));
    std::fflush(stdout);
}

SelectionPickingPredicateMode selectionPickingPredicateMode() {
    // Lazy init - startup ordering is non-load-bearing.
    selectionPickingPredicate_init();
    return s_selfpickMode;
}

bool clipSpaceFrustumAdmitGL(const Stuff::Vector4D& clipGL) {
    // Standard GL clip-volume test. Same epsilon as clipSpaceFrustumAdmit
    // for behind-camera rejection.
    if (clipGL.w <= 1e-4f) return false;  // behind camera or degenerate
    if (clipGL.x < -clipGL.w || clipGL.x > clipGL.w) return false;
    if (clipGL.y < -clipGL.w || clipGL.y > clipGL.w) return false;
    if (clipGL.z < 0.0f      || clipGL.z > clipGL.w) return false;
    return true;
}

namespace {

bool               s_bypassInitialized = false;
ProjectZBypassMode s_bypassMode = ProjectZBypassMode::Bypass;

} // namespace

ProjectZBypassMode projectZBypassMode() {
    if (!s_bypassInitialized) {
        if (mc2::gate::is<"MC2_PROJECTZ_BYPASS_MODE", "Off">()) {
            s_bypassMode = ProjectZBypassMode::Off;
        } else if (mc2::gate::is<"MC2_PROJECTZ_BYPASS_MODE", "Compare">()) {
            s_bypassMode = ProjectZBypassMode::Compare;
        } else {
            // Default: Bypass. Explicit "Bypass" or unset both land here.
            s_bypassMode = ProjectZBypassMode::Bypass;
        }
        s_bypassInitialized = true;
        const char* label = (s_bypassMode == ProjectZBypassMode::Compare) ? "compare"
                          : (s_bypassMode == ProjectZBypassMode::Bypass)  ? "bypass"
                          :                                                  "off";
        MC2_DIAGF("[OBJECT_ADMISSION_PREDICATE v1] event=mode_select wrapper=projectz_bypass mode=%s\n", label);
        std::fflush(stdout);
    }
    return s_bypassMode;
}

void logProjectZBypassDisagreement(mc2::czstring wrapper,
                                   const glm::vec3& world,
                                   const Stuff::Vector4D& legacyRawClip,
                                   bool legacyAdmit,
                                   const Stuff::Vector4D& bypassClipGL,
                                   bool bypassAdmit)
{
    // 5 wrapper slots indexed by wrapper name hash (stable within process).
    // Simple approach: compare against the known 5 names.
    static int s_counts[5] = {0, 0, 0, 0, 0};
    static const char* const s_names[5] = {
        "object", "effect", "lighting_shadow", "debug_overlay", "selection_picking"
    };
    int idx = -1;
    for (int i = 0; i < 5; ++i) {
        if (std::strcmp(wrapper, s_names[i]) == 0) { idx = i; break; }
    }
    if (idx < 0) idx = 0;  // unknown wrapper: slot 0
    if (s_counts[idx] >= 64) return;
    ++s_counts[idx];
    MC2_DIAGF("[OBJECT_ADMISSION_PREDICATE v1] event=disagree wrapper=%s "
                "world=(%.3f,%.3f,%.3f) "
                "legacyRawClip=(%.4f,%.4f,%.4f,%.4f) legacyAdmit=%d "
                "bypassClipGL=(%.4f,%.4f,%.4f,%.4f) bypassAdmit=%d\n",
                wrapper,
                world.x, world.y, world.z,
                legacyRawClip.x, legacyRawClip.y, legacyRawClip.z, legacyRawClip.w,
                legacyAdmit ? 1 : 0,
                bypassClipGL.x, bypassClipGL.y, bypassClipGL.z, bypassClipGL.w,
                bypassAdmit ? 1 : 0);
    std::fflush(stdout);
}

void logSelectionPickingScreenDelta(const glm::vec3& world,
                                    const Stuff::Vector4D& legacyScreen,
                                    float bypassScreenX, float bypassScreenY,
                                    float dxPx, float dyPx)
{
    // F5 T1: rate-limited to first ~64 events (single global counter for this logger).
    static int s_count = 0;
    if (s_count >= 64) return;
    ++s_count;
    MC2_DIAGF("[OBJECT_ADMISSION_PREDICATE v1] event=picking_screen_delta "
                "world=(%.3f,%.3f,%.3f) "
                "legacyScreen=(%.2f,%.2f) "
                "bypassScreen=(%.2f,%.2f) "
                "dxPx=%.3f dyPx=%.3f\n",
                world.x, world.y, world.z,
                legacyScreen.x, legacyScreen.y,
                bypassScreenX, bypassScreenY,
                dxPx, dyPx);
    std::fflush(stdout);
}

namespace {

bool                s_sxyInitialized = false;
ScreenXYPredicateMode s_sxyMode = ScreenXYPredicateMode::Legacy;

const char* sxyModeLabel(ScreenXYPredicateMode m) {
    return (m == ScreenXYPredicateMode::Modern) ? "modern" : "legacy";
}

} // namespace

void screenXYPredicate_init() {
    if (s_sxyInitialized) return;
    if (mc2::gate::is<"MC2_SCREENXY_PREDICATE_MODE", "Legacy">()) {
        s_sxyMode = ScreenXYPredicateMode::Legacy;  // env-revert only
    } else {
        s_sxyMode = ScreenXYPredicateMode::Modern;  // default: Modern
    }
    s_sxyInitialized = true;
    MC2_DIAGF("[OBJECT_ADMISSION_PREDICATE v1] event=mode_select wrapper=screenxy mode=%s\n",
                sxyModeLabel(s_sxyMode));
    std::fflush(stdout);
}

ScreenXYPredicateMode screenXYPredicateMode() {
    // Lazy init - startup ordering is non-load-bearing.
    screenXYPredicate_init();
    return s_sxyMode;
}

void logScreenXYScreenDelta(const glm::vec3& world,
                            const Stuff::Vector4D& legacyScreen,
                            float bypassScreenX, float bypassScreenY,
                            float dxPx, float dyPx)
{
    // F5 T2: rate-limited to first ~64 events (single global counter for this logger).
    static int s_count = 0;
    if (s_count >= 64) return;
    ++s_count;
    MC2_DIAGF("[OBJECT_ADMISSION_PREDICATE v1] event=screenxy_screen_delta "
                "world=(%.3f,%.3f,%.3f) "
                "legacyScreen=(%.2f,%.2f) "
                "bypassScreen=(%.2f,%.2f) "
                "dxPx=%.3f dyPx=%.3f\n",
                world.x, world.y, world.z,
                legacyScreen.x, legacyScreen.y,
                bypassScreenX, bypassScreenY,
                dxPx, dyPx);
    std::fflush(stdout);
}

bool clipSpaceFrustumAdmit(const Stuff::Vector4D& rawClip) {
    // IMPORTANT — MC2 clip.w sign convention (see memory/clip_w_sign_trap.md):
    // MC2's Stuff worldToClip matrix produces clip.w of EITHER sign for visible
    // vertices. The TES uses abs(clip.w) for this reason (terrain_tes_projection.md).
    // clip.w <= 0 does NOT mean "behind camera" — do not use sign(clip.w) as a
    // front-test.
    //
    // We normalize the clip vector so w > 0 before applying the standard frustum test
    // (the GPU's homogeneous clipper does this implicitly). Multiplying by sign(w) flips
    // the inequalities consistently for all components.
    //
    // Lockstep GLSL version: shaders/gpu_cull_predicate.glsl.
    const float s  = (rawClip.w < 0.0f) ? -1.0f : 1.0f;
    const float cx = rawClip.x * s;
    const float cy = rawClip.y * s;
    const float cz = rawClip.z * s;
    const float cw = rawClip.w * s;  // always >= 0 after this
    // Degenerate point (clip.w == 0) cannot be projected.
    if (cw < 1e-5f) return false;
    // Test x, y: NDC x/w ∈ [-1, 1].
    if (cx < -cw || cx > cw) return false;
    if (cy < -cw || cy > cw) return false;
    // D3D-style [0, w] depth range: NDC z/w ∈ [0, 1].
    if (cz < 0.0f || cz > cw) return false;
    return true;
}

// 2026-05-10 — sphere-aware admit. Lockstep with shaders/gpu_cull_predicate.glsl
// `clipSpaceFrustumAdmitSphere`. projScale converts world-space radius to
// clip-space half-extent: for perspective M = P*V (V orthonormal, P diagonal),
// |row_i(M)_xyz| = P_ii, so projScale = max(|row0|, |row1|) of viewProj gives
// the correct depth-independent factor. Caller must supply projScale from the
// current view-projection matrix (see gpu_cull.comp for the GLSL derivation).
bool clipSpaceFrustumAdmitSphere(const Stuff::Vector4D& rawClip, float worldRadius,
                                  float projScale) {
    const float s  = (rawClip.w < 0.0f) ? -1.0f : 1.0f;
    const float cx = rawClip.x * s;
    const float cy = rawClip.y * s;
    const float cz = rawClip.z * s;
    const float cw = rawClip.w * s;
    if (cw < 1e-5f) {
        return worldRadius > 0.0f;
    }
    const float tol = worldRadius * projScale;
    if (cx < -cw - tol || cx > cw + tol) return false;
    if (cy < -cw - tol || cy > cw + tol) return false;
    if (cz < -tol      || cz > cw + tol) return false;
    return true;
}

int objectAdmissionPredicate_selftest() {
    int fails = 0;

    auto runCase = [&](mc2::czstring name, const Stuff::Vector4D& rawClip, bool expected) {
        bool actual = clipSpaceFrustumAdmit(rawClip);
        const char* result = (actual == expected) ? "pass" : "fail";
        if (actual != expected) ++fails;
        mc2::fmt::print_out("[OBJECT_ADMISSION v1] event=selftest_{} case={} expected={:d} actual={:d} rawClip=({:.3f},{:.3f},{:.3f},{:.3f})\n", mc2::fmt::safe(result), mc2::fmt::safe(name), mc2::fmt::as_i32(expected ? 1 : 0), mc2::fmt::as_i32(actual ? 1 : 0), rawClip.x, rawClip.y, rawClip.z, rawClip.w);
    };

    // Center of frustum, w=1, point fully inside.
    Stuff::Vector4D clip;
    clip.x =  0.0f; clip.y =  0.0f; clip.z = 0.5f; clip.w = 1.0f;
    runCase("center_inside", clip, true);

    // MC2 clip.w sign convention: clip.w may be negative for visible objects
    // (see clip_w_sign_trap.md, terrain_tes_projection.md). With sign-normalization
    // a point at the center of the frustum with w=-1 must have z=-0.5 (so NDC z =
    // z/w = -0.5/-1 = 0.5, inside D3D [0,1] range). After flipping: cz=0.5, cw=1 → admit.
    clip.x =  0.0f; clip.y =  0.0f; clip.z = -0.5f; clip.w = -1.0f;
    runCase("neg_w_center_inside", clip, true);

    // Point outside frustum with negative w: after flip x=1.5 > cw=1 → rejected.
    clip.x = -1.5f; clip.y = 0.0f; clip.z = -0.5f; clip.w = -1.0f;
    runCase("neg_w_left_outside", clip, false);

    // Beyond left clip plane (positive w).
    clip.x = -1.5f; clip.y = 0.0f; clip.z = 0.5f; clip.w = 1.0f;
    runCase("left_outside", clip, false);

    // On left clip plane (x == -w): admitted by `<=` boundary semantics.
    clip.x = -1.0f; clip.y = 0.0f; clip.z = 0.5f; clip.w = 1.0f;
    runCase("left_edge_inclusive", clip, true);

    // Past far plane (z > w).
    clip.x =  0.0f; clip.y =  0.0f; clip.z = 1.5f; clip.w = 1.0f;
    runCase("past_far", clip, false);

    // Closer than near plane (z < 0).
    clip.x = 0.0f; clip.y = 0.0f; clip.z = -0.1f; clip.w = 1.0f;
    runCase("before_near", clip, false);

    // w == 0: pathological / degenerate. Reject (cannot represent a finite point).
    clip.x = 0.0f; clip.y = 0.0f; clip.z = 0.0f; clip.w = 0.0f;
    runCase("w_zero_degenerate", clip, false);

    // Far corner of frustum: x=w, y=w, z=w. All on inclusive boundaries.
    clip.x = 1.0f; clip.y = 1.0f; clip.z = 1.0f; clip.w = 1.0f;
    runCase("far_corner_inclusive", clip, true);

    return fails;
}
