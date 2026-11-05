#pragma once
//---------------------------------------------------------------------------
// Track A1 — object admission predicate (clip-space frustum).
//
// This is the modern bool decision used by Camera::projectForObjectAdmission
// when MC2_OBJECT_ADMISSION_PREDICATE=modern, AND the function the trace
// system uses to compute its homogClipFull candidate predicate. Both
// callers must use the same function — drift would invalidate the
// dual-run parity gate.
//
// Spec: docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md
// Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
//---------------------------------------------------------------------------

// Full Stuff headers needed for ModernClipResult (struct members must be complete types).
// Include the umbrella header so the Stuff type hierarchy is initialized in correct order
// (stuff.hpp -> point3d.hpp -> vector3d.hpp; stuff.hpp -> vector4d.hpp).
#include "mc2_czstring.h"
#include "stuff/stuff.hpp"
#include "math/mc2_xform.h"

// Mode probed at startup from MC2_OBJECT_ADMISSION_PREDICATE env var.
enum class ObjectAdmissionPredicateMode {
    Legacy,    // default: bool comes from projectZ's screen-rect test
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe (idempotent). Called lazily from objectAdmissionPredicateMode().
void objectAdmissionPredicate_init();

// Read the active mode. Lazily initializes — startup ordering is non-load-bearing.
ObjectAdmissionPredicateMode objectAdmissionPredicateMode();

// The predicate itself. Returns true iff the homogeneous clip-space point is
// inside the canonical clip volume:
//   rawClip.w >  0
//   |rawClip.x| <= rawClip.w   (left/right planes)
//   |rawClip.y| <= rawClip.w   (top/bottom planes)
//   0 <= rawClip.z <= rawClip.w (near/far planes; MC2 uses D3D-style [0,w])
//
// Note: rawClip is `xformCoords` from inside Camera::projectZ — the post-multiply,
// pre-divide clip-space coord. Always use the value captured via
// LegacyProjectionResult::rawClip; do NOT pass the post-divide screen vector.
bool clipSpaceFrustumAdmit(const Stuff::Vector4D& rawClip);

// Sphere-aware admit. Pads the frustum test by worldRadius*projScale so an
// object whose center has crossed a plane (notably the near plane) is still
// admitted while its bounding sphere is in view. projScale = max xyz-norm of
// the view-projection matrix rows/cols that produce clip.x/clip.y (see the
// definition comment + gpu_cull.comp). Lockstep GLSL: gpu_cull_predicate.glsl
// clipSpaceFrustumAdmitSphere. Used by Camera::projectForObjectAdmissionSphere
// ([LOW-CAMERA-OBJECT-CULL-1 / FIX-3]).
bool clipSpaceFrustumAdmitSphere(const Stuff::Vector4D& rawClip, float worldRadius,
                                 float projScale);

// Self-test entry point. Runs the unit cases listed in
// docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
// Task 3. Prints `[OBJECT_ADMISSION v1] event=selftest_pass|fail case=<name>`
// per case. Returns the number of failures (0 = all pass). Caller is
// expected to fail loudly on non-zero return — see Task 4.
int objectAdmissionPredicate_selftest();

//---------------------------------------------------------------------------
// F4 projectZ-bypass support.
// Used by the 5 Modern-default wrappers (object, effect, lighting_shadow,
// debug_overlay, selection_picking) when MC2_PROJECTZ_BYPASS_MODE != Off,
// to compute clip space directly from worldToClipGL() without routing
// through projectZ.
//---------------------------------------------------------------------------

struct ModernClipResult {
    Stuff::Vector4D clip;  // post-axisSwap, post-kPixelHomogToGLNDC: GL-NDC convention
    bool            admit; // standard clip-volume test: -w<=x,y<=w AND 0<=z<=w
};

// GL-NDC convention clipSpaceFrustumAdmit. Mirrors clipSpaceFrustumAdmit
// but for the GL-NDC clip range (clip.xy in [-w,w]; clip.z in [0,w]).
// Distinct from clipSpaceFrustumAdmit which assumes D3D-pixel-homog clip.
bool clipSpaceFrustumAdmitGL(const Stuff::Vector4D& clipGL);

// 3-state bypass mode read from env MC2_PROJECTZ_BYPASS_MODE.
enum class ProjectZBypassMode {
    Off,     // current behavior: Modern routes through projectZ + clipSpaceFrustumAdmit(rawClip). Default.
    Compare, // run BOTH paths; log disagreements; return Legacy result (no behavior change).
    Bypass,  // use ONLY projectModernClipGL; skip projectZ entirely.
};

// Reads env MC2_PROJECTZ_BYPASS_MODE. Default = Off. Values: Off / Compare / Bypass.
ProjectZBypassMode projectZBypassMode();

// Logs a disagreement between Legacy and Bypass at low rate (first ~64 per
// wrapper per process). Wrapper-name is one of: object, effect,
// lighting_shadow, debug_overlay, selection_picking.
void logProjectZBypassDisagreement(mc2::czstring wrapper,
                                   const glm::vec3& world,
                                   const Stuff::Vector4D& legacyRawClip,
                                   bool legacyAdmit,
                                   const Stuff::Vector4D& bypassClipGL,
                                   bool bypassAdmit);

// F5 T1: rate-limited (first ~64 events) screen.xy parity logger for
// selection_picking Compare mode. Tracks legacy-vs-bypass screen pixel
// disagreements >1px.
void logSelectionPickingScreenDelta(const glm::vec3& world,
                                    const Stuff::Vector4D& legacyScreen,
                                    float bypassScreenX, float bypassScreenY,
                                    float dxPx, float dyPx);

// F5 T2: rate-limited (first ~64 events) screen.xy parity logger for
// projectForScreenXY Compare mode. Tracks legacy-vs-bypass screen pixel
// disagreements >1px.
void logScreenXYScreenDelta(const glm::vec3& world,
                            const Stuff::Vector4D& legacyScreen,
                            float bypassScreenX, float bypassScreenY,
                            float dxPx, float dyPx);

//---------------------------------------------------------------------------
// Track A2 - effect admission mode (sibling of the Track A1 object mode).
// Same clipSpaceFrustumAdmit predicate; separate env flag so the two slices
// can be flipped on independently during soak.
//---------------------------------------------------------------------------

enum class EffectAdmissionPredicateMode {
    Legacy,    // default: bool comes from projectZ's screen-rect test
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe. Idempotent. Called lazily from effectAdmissionPredicateMode().
void effectAdmissionPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
EffectAdmissionPredicateMode effectAdmissionPredicateMode();

//---------------------------------------------------------------------------
// Track A1 - lighting/shadow predicate mode (sibling of object + effect modes).
// Same clipSpaceFrustumAdmit predicate; separate env flag for independent soak.
// Default: Legacy (lighting/shadow activation gating is low-risk; flip after
// broader canary coverage of object/effect modes).
//---------------------------------------------------------------------------

enum class LightingShadowPredicateMode {
    Legacy,    // bool comes from projectZ's screen-rect test
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe. Idempotent. Called lazily from lightingShadowPredicateMode().
void lightingShadowPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
LightingShadowPredicateMode lightingShadowPredicateMode();

//---------------------------------------------------------------------------
// Track A1 - debug overlay predicate mode (sibling of lighting/shadow mode).
// Same clipSpaceFrustumAdmit predicate; separate env flag for independent soak.
// Default: Legacy (debug overlay draws are lab-only; low-risk flip candidate).
//---------------------------------------------------------------------------

enum class DebugOverlayPredicateMode {
    Legacy,    // bool comes from legacy projectZ screen-rect acceptance
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe (idempotent). Reads env MC2_DEBUG_OVERLAY_PREDICATE_MODE
// (Legacy / Modern). Default = Legacy.
void debugOverlayPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
DebugOverlayPredicateMode debugOverlayPredicateMode();

//---------------------------------------------------------------------------
// Track A1 - selection picking predicate mode (sibling of lighting/shadow mode).
// Same clipSpaceFrustumAdmit predicate; separate env flag for independent soak.
// Default: Legacy (picking bool is discarded by callers; screen.xy still read).
//---------------------------------------------------------------------------

enum class SelectionPickingPredicateMode {
    Legacy,    // bool comes from legacy projectZ screen-rect acceptance
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe (idempotent). Reads env MC2_SELECTION_PICKING_PREDICATE_MODE
// (Legacy / Modern). Default = Legacy.
void selectionPickingPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
SelectionPickingPredicateMode selectionPickingPredicateMode();

//---------------------------------------------------------------------------
// F5 T2 - screen-XY predicate mode for projectForScreenXY.
// 16 callers consume screen.xy for HUD/UI positioning; pixel parity is
// load-bearing. Default: Legacy. Flip to Modern only after user-validated
// HUD parity on mc2_10 + tier1.
// Env var: MC2_SCREENXY_PREDICATE_MODE (Modern flips; default Legacy).
//---------------------------------------------------------------------------

enum class ScreenXYPredicateMode {
    Legacy,    // screen.xy from legacy projectZ
    Modern,    // screen.xy via projectModernClipGL + viewport remap (same as F5 T1)
};

// One-time probe (idempotent). Reads env MC2_SCREENXY_PREDICATE_MODE
// (Modern flips; default Legacy).
void screenXYPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
ScreenXYPredicateMode screenXYPredicateMode();
