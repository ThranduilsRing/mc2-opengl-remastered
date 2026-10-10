<!-- doc-status: current | reviewed:2026-10-11 -->
# OBJMGR-CONTRACT-HARNESS-1 — RECON (REFRESHED) + PLAN

**Status:** RECON REFRESH — no code changes. Supersedes the stale recon cut from
`de681b33`.
**Branch / worktree:** `claude/objmgr-contract-harness-1` @
`A:/Games/mc2-objmgr-harness`, off **current nifty `3da176d4`**.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## ⚠️ Prior recon finding is STALE — the "live OOB" is already fixed

The first recon (off `de681b33`) reported an unguarded
`watchList[j] = objList[watchSave[j]]` OOB at Load. **That was already fixed on
nifty by a parallel session before this recon ran:**

- `e5b49cb8` OBJMGR-WATCHID-BOUNDS-1 — bound `nextWatchID` in `setWatchID`,
  clamp the `Save` loop.
- `8f6eb16c` WATCHID-LOAD-GUARD-1 (merge `7928fa71`) — validate untrusted
  `watchSave[j]` and clamp `nextWatchId` on Load. **Same fix this recon
  proposed.**

My branch predated `8f6eb16c`, so it saw the old code. **No production fix is
needed.** (Process lesson logged below.)

## Current (hardened) invariants — verified on `3da176d4`

| Function | Site | Invariant now enforced |
|---|---|---|
| `setWatchID` | objmgr.cpp:959 | `nextWatchID > maxObjects` → leave un-watchable (no OOB write), cursor unchanged; else `watchList[nextWatchID]=obj; obj->watchID=nextWatchID++`. |
| `getByWatchID` | objmgr.h:500 | `(id>0 && id<nextWatchID) ? watchList[id] : NULL`. |
| `Save` | code/objmgr_misc.cpp:581 | `_watchHi = min(nextWatchID, maxObjects)` — never reads/writes past `maxObjects`. |
| `CopyFrom` (load) | code/objmgr_misc.cpp:552 | clamp untrusted `nextWatchId` to `[0, maxObjects+1]`; OOB → clamp + `WATCHID_LOAD` diag. |
| `Load` restore | objmgr.cpp:5467 | each `watchSave[j]` validated in `[0, maxObjects]` before indexing `objList`; invalid → `watchList[j]=NULL` + diag; loop bounded by `maxObjects`, not any serialized value. |

These are exactly the edge cases that are hard to hit in tier1 smoke (corrupt /
cross-version saves, watch-ID exhaustion under reinforcement churn). They are
**high-value regression-guard targets** — the harness's job is to lock them in
so a future refactor can't silently regress them.

## Revised decision: harness-only, but it needs a small extraction

The arc's hard rule is **no fake-green**: the harness must exercise the *real*
logic, not a copy. Problem: all four pieces are **inline/member code on
`GameObjectManager`** (objmgr.cpp 44 includes; getByWatchID is a member inline in
objmgr.h). There is **no standalone helper today** to link, and instantiating a
`GameObjectManager` pulls ~the whole engine (confirmed in the original recon).

So there are only three honest options:

| Option | No fake-green? | Production touch? | Verdict |
|---|---|---|---|
| A. Harness re-implements the math | ❌ fake-green | none | **Forbidden by the arc.** |
| B. Link `objmgr.cpp` directly | ✅ | none | **Infeasible** (whole engine, >1s, not game-free). |
| C. Extract `code/objmgr_watch_policy.h` (4 tiny pure templated fns), refactor the 4 sites to call it, harness links the header | ✅ real shared code | **yes — small, behavior-preserving** | **Only viable path.** |

The ruling permits a production refactor "if the helper is small and
behavior-preserving." Option C qualifies: 4 pure functions over
`(T** list, unsigned long& next, long maxObj, ...)`, no globals, no behavior
change — just moves existing math into a header both production and harness call.

**But** C still edits `objmgr.cpp/.h`, so it needs **tier1 smoke** as the
integration gate (unlike the tools-only template/shader slices). It is no longer
urgent (no bug to fix) — it is pure test-infrastructure debt-paydown.

## Plan: OBJMGR-CONTRACT-HARNESS-1 (two slices, gated on a go/no-go)

**Decision needed before any code** — is the small behavior-preserving extraction
worth a tier1-gated production touch *purely to enable a regression harness*, or
do we defer objmgr from the harness arc until a future objmgr change forces the
extraction anyway?

If **GO**:

1. **OBJMGR-WATCH-POLICY-EXTRACT-1** (production, tier1-gated)
   - Add `code/objmgr_watch_policy.h` — header-only, templated on `T`,
     `#include <cstdint>` only. Functions:
     `watchAssign(T** list, unsigned long& next, long maxObj, T* obj, unsigned long& outID) -> bool`,
     `watchResolve(T** list, unsigned long next, unsigned long id) -> T*`,
     `watchSaveHi(unsigned long next, long maxObj) -> long`,
     `watchLoadNextClamp(long savedNext, long maxObj) -> unsigned long`,
     `watchLoadIndexValid(int32_t saveIdx, long maxObj) -> bool`.
   - Refactor `setWatchID`/`getByWatchID`/`Save`/`CopyFrom`/`Load` to call them.
     Diag/logging stays in the production methods. **Byte-for-byte behavior.**
   - Gate: tier1 5/5 (the canonical smoke command), `MC2_GL_DEBUG_FATAL` clean.

2. **OBJMGR-CONTRACT-HARNESS-1** (tools-only, no smoke)
   - `tools/objmgr_contract_harness/` standalone CMake, links **only**
     `objmgr_watch_policy.h` (header-only → zero production objects, stays
     game-free). Uses `struct FakeObj { unsigned long watchID; }`.
   - Tests: assign at exact `maxObjects` succeeds; `maxObjects+1` drops + cursor
     unchanged + obj stays un-watchable; resolve rejects `0` / `>=nextWatchID` /
     huge; save-hi never exceeds `maxObjects` when `nextWatchID==maxObjects+1`;
     load-next clamp on negative / `>maxObjects+1`; load-index rejects `<0` /
     `>maxObjects`.
   - Register in `tools/run_contract_tests.py`; full suite stays <1s.

If **NO-GO**: mark objmgr deferred in the arc doc; pick a different
low-entanglement subsystem next (e.g. RENDER-STATE-CONTRACT-HARNESS-1, mock-only,
no production touch).

## Process lesson (recurring)

Branches cut from an older nifty keep rediscovering bugs already fixed by
parallel sessions. **Before claiming a live bug from recon, check current nifty
history:**
```
git log --oneline -G "watchSave|nextWatchID|getByWatchID" -i --since=...
```
This recon's OOB was fixed ~hours earlier on another branch. Always recon against
current HEAD, not the branch base.
