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

enum Class {
    kHealthy    = 0,  // a real, baked, in-range row
    kSentinel   = 1,  // index is kNoLightIndex -> int(-1) -> OOB read -> zeros
    kOutOfRange = 2,  // index past the uploaded table -> OOB read -> zeros
    kEmptyRow   = 3   // slot exists but numLights_ == 0 -> reads as zero light
};

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
