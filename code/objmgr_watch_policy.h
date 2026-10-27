//============================================================================
// objmgr_watch_policy.h — OBJMGR-WATCH-POLICY-EXTRACT-1
//
// Pure, header-only bounds/index policy for the GameObjectManager watch list.
// The watch list (watchList[]) and the save-side watchSave[] are both allocated
// getMaxObjects()+1 entries, so the only valid indices are [0, maxObjects], and
// getMaxObjects() never grows after load. These functions centralize the small
// integer guards that were hand-copied across setWatchID / getByWatchID / Save /
// CopyFrom / Load so production and the contract harness share ONE source of
// truth (no duplicated guard math = no fake-green).
//
// DELIBERATELY boring: no ownership, no allocation, no GameObject semantics, no
// diagnostics, no array access. Each function is a pure predicate or clamp over
// integers. Production call sites still own the array access, the logging, and
// the fail-safe action (drop / invalidate / leave un-watchable).
//============================================================================
#ifndef OBJMGR_WATCH_POLICY_H
#define OBJMGR_WATCH_POLICY_H

#include <cstdint>

namespace mc2watch {

// setWatchID: a new watch id can be assigned iff the next free slot is still
// within the allocation (indices [0, maxObjects]). Exhausted -> caller leaves
// the object un-watchable (no OOB write). [OBJMGR-WATCHID-BOUNDS-1]
inline bool canAssignWatchId(unsigned long nextWatchID, long maxObjects)
{
    return nextWatchID <= static_cast<unsigned long>(maxObjects);
}

// getByWatchID: a watch id resolves iff it is a live, in-range slot. Id 0 is the
// reserved NULL slot; ids >= nextWatchID are unassigned. Caller indexes
// watchList[id] only when this is true.
inline bool isResolvableWatchId(unsigned long watchID, unsigned long nextWatchID)
{
    return (watchID > 0) && (watchID < nextWatchID);
}

// Save: inclusive upper bound for the watch-restore scan. nextWatchID can
// legitimately equal maxObjects+1 after the last in-bounds assign, so the scan
// must never exceed maxObjects (watchList/watchSave are sized maxObjects+1).
// [OBJMGR-WATCHID-BOUNDS-1]
inline long clampSaveLoopLimit(unsigned long nextWatchID, long maxObjects)
{
    return (static_cast<long>(nextWatchID) < maxObjects) ? static_cast<long>(nextWatchID) : maxObjects;
}

// CopyFrom (load): nextWatchId is untrusted on-disk data. getByWatchID reads
// watchList[id] for id < nextWatchID, so a corrupt/cross-version value must be
// clamped to the allocation [0, maxObjects+1]. Negative -> 1 (empty). The caller
// detects a clamp (result != savedNext) to decide whether to emit a diag.
// [WATCHID-LOAD-GUARD-1]
inline unsigned long clampSavedNextWatchId(long savedNext, long maxObjects)
{
    const long cap = maxObjects + 1;
    if (savedNext < 0)   return 1ul;
    if (savedNext > cap) return static_cast<unsigned long>(cap);
    return static_cast<unsigned long>(savedNext);
}

// Load: each watchSave[j] is untrusted on-disk data used to index objList (sized
// maxObjects+1). Valid iff in [0, maxObjects] (boundary maxObjects allowed).
// Invalid -> caller invalidates the watch slot, never dereferences objList.
// [WATCHID-LOAD-GUARD-1]
inline bool isValidWatchSaveIndex(int32_t saveIdx, long maxObjects)
{
    return (saveIdx >= 0) && (static_cast<long>(saveIdx) <= maxObjects);
}

} // namespace mc2watch

#endif // OBJMGR_WATCH_POLICY_H
