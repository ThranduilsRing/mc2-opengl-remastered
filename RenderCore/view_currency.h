#include <algorithm>

#pragma once
// RENDER-VIEW-CURRENCY pure kernel (GL-free, host-testable).
//
// Single source of truth for (1) the view-content-epoch DEDUPE predicate and (2) the
// object/mech MVP currency DECISION. The engine (GameOS/gameos/gameos_graphics.cpp,
// gos_object_draw_mvp.h) and the offline unit harness (tests/unit/test_view_currency.cpp)
// both call THESE functions, so the test cannot drift from the shipped logic.
//
// Background (VIEW-EPOCH-DEDUPE-1): object draw may use the terrain dispatch-MVP
// snapshot (depth-matched -> FixB z-fight fix) only when the snapshot belongs to the
// CURRENT view. "Current" is keyed on a semantic VIEW-CONTENT epoch that advances only
// when the published world-to-clip matrix actually changes -- NOT on every publish
// (the early-publish + gamecam double-publish of the same camera must not advance it,
// or the snapshot is false-staled every frame and the z-fight fix silently dies).
namespace mc2 { namespace view_currency {

// True iff the published world-to-clip CONTENT changed beyond `eps` (max abs element
// delta). Redundant same-camera republishes return false (epoch must NOT advance).
// `havePrev == false` (first publish this process) returns true to force one bump.
inline bool viewContentChanged(const float* prev16, const float* cur16,
                               bool havePrev, float eps) {
    if (!havePrev) return true;
    float maxDelta = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float d = cur16[i] - prev16[i];
        if (d < 0.0f) d = -d;
        maxDelta = (std::max)(maxDelta, d);
    }
    return maxDelta > eps;
}

// The object/mech MVP currency decision. Returns true => use the depth-matched
// dispatch snapshot (FixB z-fight fix active); false => caller falls back to the live
// MVP (always a correct projection of current-position geometry). The snapshot is used
// ONLY when: the FixB gate is on, the solid pass is armed, the snapshot pointer is
// valid, AND the snapshot's stamped view-content epoch equals the current view-content
// epoch (i.e. it was produced under THIS view).
inline bool objectMvpUseSnapshot(bool fixBEnabled, bool armed,
                                 long snapshotEpoch, long currentEpoch,
                                 bool snapshotNonNull) {
    return fixBEnabled && armed && snapshotNonNull && (snapshotEpoch == currentEpoch);
}

}} // namespace mc2::view_currency
