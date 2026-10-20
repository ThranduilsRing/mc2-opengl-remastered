// mclib/static_light_row_guard.h
// ZERO-LIGHT-ROW-1 — the static-prop light-row validity contract, as pure code.
//
// WHY THIS HEADER EXISTS
// ----------------------
// A static prop's lighting is read by the vertex shader as
//   ObjectLights ld = light[int(inst.lightDataIndex)];   (shaders/static_prop.vert:714)
// so `lightDataIndex` is the ONLY thing standing between a prop and its light.
// Two distinct failures both end as an all-zero row on screen:
//
//   (1) the index is the "I have no value" sentinel 0xFFFFFFFF. GLSL converts
//       that to int(-1); light[-1] is an out-of-bounds SSBO read, which returns
//       zeros under robust-buffer-access. numLights==0 => ambientTerm and
//       sunColor both stay 0.0 => the prop is lit by nothing.
//   (2) the index is in range but the slot was never baked (numLights_ == 0),
//       which the registry's own flush trace already calls out as the
//       black-billboard mechanism (gos_static_prop_registry.cpp, "if numLights==0
//       here, calc_light returns base_light only ... that's black").
//
// A row of zeros is NOT a plausible lighting result for a lit object. It is a
// MISSING VALUE WEARING THE COSTUME OF A COMPUTED ONE — and every layer between
// its creation and the framebuffer treated it as data. This header makes the
// distinction explicit and, crucially, TESTABLE OFFLINE: production
// (gos_static_prop_registry.cpp) and the contract harness
// (tools/object_light_row_harness) compile the SAME functions, so a harness test
// exercises shipped code rather than a re-implementation of it.
//
// Header-only, dependency-free (same idiom as mclib/object_light_row.h): no
// engine, no GL, no Stuff, no gameos. That is what lets it be proven with the
// game unlaunchable.

#ifndef MC2_STATIC_LIGHT_ROW_GUARD_H
#define MC2_STATIC_LIGHT_ROW_GUARD_H

#include <stdint.h>

namespace static_light_row {

// The "not yet cached / no value" sentinel. Minted at msl.h:346
// (cachedGpuLightIndex_) and at bdactor.h:258 (StaticRegistration::lightDataIndex),
// and written verbatim into a recipe at gos_static_prop_batcher.cpp:9818
// ("sentinel; flush() patches per-frame").
static const uint32_t kNoLightIndex = 0xFFFFFFFFu;

enum class Class {
    kHealthy    = 0,  // a real, baked, in-range row
    kSentinel   = 1,  // index is kNoLightIndex -> int(-1) -> OOB read -> zeros
    kOutOfRange = 2,  // index past the uploaded table -> OOB read -> zeros
    kEmptyRow   = 3   // slot exists but numLights_ == 0 -> reads as zero light
};
using enum Class;

// Mirror of the resolution at gos_static_prop_registry.cpp flush():
//   freshLightIdx = (perInstanceLight && rangeLightIdx != sentinel)
//                       ? rangeLightIdx : multiCacheIdx;
//
// NOTE the shape of this: when the PER-ACTOR value is the sentinel, resolution
// silently falls back to the SHARED per-multi cache. That fallback is why a
// sentinel rarely reaches the GPU on the render path — and also why a prop whose
// own capture failed is lit by whichever SIBLING instance wrote the shared cache
// last. Correct-looking, wrong light. The fallback hides the failure instead of
// reporting it; classify() below is what makes it reportable.
inline uint32_t resolve(bool perInstanceLight,
                        uint32_t rangeLightIdx,
                        uint32_t multiCacheIdx)
{
    return (perInstanceLight && rangeLightIdx != kNoLightIndex) ? rangeLightIdx
                                                                : multiCacheIdx;
}

// Classify a resolved index against the live light table.
//   numLightsAtSlot < 0  => caller could not address the slot (out of range).
//                           This is exactly what MC_TextureManager::peekLightSlot
//                           returns (LightSlotPeek::numLights, "-1 if out of range").
//   numLightsAtSlot == 0 => addressable but empty.
inline Class classify(uint32_t resolvedLightIdx, int numLightsAtSlot)
{
    if (resolvedLightIdx == kNoLightIndex) return kSentinel;
    if (numLightsAtSlot < 0)               return kOutOfRange;
    if (numLightsAtSlot == 0)              return kEmptyRow;
    return kHealthy;
}

// True when the row this index selects will shade the prop with no light at all.
inline bool isZeroRow(uint32_t resolvedLightIdx, int numLightsAtSlot)
{
    return classify(resolvedLightIdx, numLightsAtSlot) != kHealthy;
}

// ── GUARD/FORWARD DIVERGENCE ────────────────────────────────────────────────
// ZERO-LIGHT-ROW-1 follow-up. BldgAppearance::render (bdactor.cpp:2214) and
// TreeAppearance::render (:6860) VALIDATE `shape->getCachedGpuLightIndex()` --
// the shared per-multi cache -- and then FORWARD `staticReg.lightDataIndex` --
// the per-actor captured value -- to markVisibleChecked (:2230 / :6878).
// Those are different variables by design: per-actor capture was introduced
// precisely because the multi cache is last-writer-wins across sibling
// instances. So the guard tests the variable the fix was introduced to stop
// trusting, and never tests the one it introduced.
//
// This classifier names the four states so the divergence can be COUNTED before
// anyone changes what is drawn. Correcting the guard would alter which light row
// some props receive -- a visible change on a default launch -- so the census
// runs first and the behaviour change waits on its number.
enum class Divergence {
    kAgree                 = 0,  // both usable and equal -- nothing to decide
    kDifferBothValid       = 1,  // both usable but different: the per-actor capture
                                 // is doing real work; forwarding it is CORRECT and
                                 // the guard happens to be harmless here
    kForwardedSentinelOnly = 2,  // forwarded is the sentinel, validated is fine.
                                 // GUARD PASSES, then resolve() silently falls back
                                 // to the sibling's row -> prop lit by another actor.
                                 // Wrong light, not a zero row. THE MISSED CASE.
    kValidatedSentinelOnly = 3,  // validated is the sentinel, forwarded is fine.
                                 // GUARD INVALIDATES a prop that HAD a good
                                 // per-actor index -> needless deregistration churn.
    kBothSentinel          = 4    // guard invalidates, and that is correct.
};
using enum Divergence;

inline Divergence classifyGuardForward(uint32_t validatedIdx, uint32_t forwardedIdx)
{
    const bool vBad = (validatedIdx == kNoLightIndex);
    const bool fBad = (forwardedIdx == kNoLightIndex);
    if (vBad && fBad) return kBothSentinel;
    if (fBad)         return kForwardedSentinelOnly;
    if (vBad)         return kValidatedSentinelOnly;
    return (validatedIdx == forwardedIdx) ? kAgree : kDifferBothValid;
}

// True when the CURRENT (guard-validates-the-wrong-variable) code and a
// CORRECTED (guard-validates-what-it-forwards) code would behave differently.
// Exactly the two mixed states: one passes a prop the corrected guard would
// reject, the other rejects a prop the corrected guard would keep. If a run
// reports zero of both, this defect is LATENT rather than live -- a materially
// different report, and the reason the census exists.
inline bool guardCorrectionWouldChangeBehaviour(Divergence d)
{
    return d == kForwardedSentinelOnly || d == kValidatedSentinelOnly;
}

inline const char* divergenceName(Divergence d)
{
    switch (d) {
        case kDifferBothValid:       return "differ_both_valid";
        case kForwardedSentinelOnly: return "forwarded_sentinel_only";
        case kValidatedSentinelOnly: return "validated_sentinel_only";
        case kBothSentinel:          return "both_sentinel";
        default:                     return "agree";
    }
}

inline const char* className(Class c)
{
    switch (c) {
        case kSentinel:   return "sentinel";
        case kOutOfRange: return "out_of_range";
        case kEmptyRow:   return "empty_row";
        default:          return "healthy";
    }
}

} // namespace static_light_row

#endif // MC2_STATIC_LIGHT_ROW_GUARD_H
