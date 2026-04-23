# Stability Tier 1 Instrumentation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add observability for the three MC2 silent-failure modes (TGL pool exhaustion, object destruction, OpenGL errors) without changing any game behavior.

**Architecture:** Three env-gated `[SUBSYS vN]` loggers + one always-on monotonic summary + one startup banner + one invariant-enforcement script. No new subsystems, no behavior changes, no speculative fixes. All three loggers follow the existing `MC2_DEBUG_SHADOW_COLLECT` pattern (`getenv` gate, file-scope static bool, grep-friendly one-line format).

**Tech Stack:** C++17 MSVC, no test framework, verification via `/mc2-build-deploy` + in-game mission load + log grep. Build config is always `RelWithDebInfo` (Release crashes on GL_INVALID_ENUM — one of the bugs this instrumentation is meant to help diagnose).

**Spec:** [docs/superpowers/specs/2026-04-23-stability-tier1-instrumentation-design.md](../specs/2026-04-23-stability-tier1-instrumentation-design.md)

**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/stability-tier1`
**Branch:** `claude/stability-tier1` (based on `claude/nifty-mendeleev`)

---

## File Inventory

### Created
- `scripts/check-destroy-invariant.sh` — enforcement script for the `setExists(false)` invariant (commit 4).

### Modified
**Commit 1 (TGL pool trace):**
- `mclib/tgl.h` — pool method signatures gain `const char* caller, const void* shape = nullptr`; `MC2_TGL_GET_*` macros; per-pool counter fields on `TG_Pool` base; `recordNull` declaration; frame-end drain function.
- `mclib/tgl.cpp` — pool method bodies call `recordNull` on NULL; `drainTglPoolStats()` implementation; monotonic summary every 600 frames + on shutdown.
- `mclib/tgl.cpp:1667-1675` — convert six direct pool calls in `TG_Shape::TransformShape` to the `MC2_TGL_GET_*` macros.
- `GameOS/gameos/gameos.cpp` (or wherever frame-end lives — grep for `SwapBuffers` / `gos_DrawQuad` frame-end path) — one call site to `drainTglPoolStats()` just before buffer swap.

**Commit 2 (destroy wrapper, no conversions yet):**
- `code/gameobj.h` — new members `framesSinceActive` (uint8_t), `lastUpdateRet` (int32_t, default -1); `destroy(const char*, const char*, int)` method declaration; `MC2_DESTROY(obj, reason)` macro; reasons taxonomy comment block (filled by commit 3).
- `code/gameobj.cpp` — `GameObject::destroy` implementation with early field snapshot, double-destroy handling, env-gated log print; `getObjectClassName(ObjectClass)` switch-table helper.
- `code/objmgr.cpp` — single composite-active-decision snippet that updates `framesSinceActive` each frame, placed in `GameObjectManager::update()` (the per-object sweep loop).
- `code/*.cpp` (subset) — sites where `update()` is called and its return value observed: cache into `obj->lastUpdateRet` immediately. Identified by grep during task.

**Commit 3 (34 conversions + dedup + taxonomy):**
- `code/objmgr.cpp` — 26 literal conversions.
- `code/ablmc2.cpp` — 2 literal conversions.
- `code/collsn.cpp` — 2 literal conversions.
- `code/gvehicl.cpp` — 1 literal conversion.
- `code/mission.cpp` — 2 literal conversions.
- `code/weaponbolt.cpp` — 1 literal conversion.
- 4 non-literal `setExists(<expr>)` sites — review, convert where `<expr>` can be false, leave with documented reason otherwise.
- `code/gameobj.h` — fill in the taxonomy comment block above `destroy()` with final canonical reason strings.

**Commit 4 (GL drain + banner + script + CLAUDE.md):**
- `GameOS/gameos/gos_validate.h` — `drainGLErrors(const char* pass)` declaration.
- `GameOS/gameos/gos_validate.cpp` — `drainGLErrors` implementation with per-pass counters, 30-frame suppression, GL-enum switch.
- `GameOS/gameos/gameos_graphics.cpp` — insert drain calls after the four render-pass functions (`shadow_static`, `shadow_dynamic`, `terrain`, `objects_3d` — exact locations by grep during task).
- `mclib/txmmgr.cpp` — insert drain call in the object render list flush.
- `GameOS/gameos/gos_postprocess.cpp` — insert drain call after post-process composite (`post_process`).
- `GameOS/gameos/gameosmain.cpp` — insert drain calls for `hud` and `frame` in the frame-end path (alongside `drainTglPoolStats()` from Task 5); insert `[INSTR v1]` startup banner as the first statement inside `main()` (line 463).
- `scripts/check-destroy-invariant.sh` — new file.
- `CLAUDE.md` — append env vars, invariant check, schema-version grep pattern to the existing "Debug Instrumentation Rule" block.

---

## Verification Model (MC2-specific)

MC2 has no unit-test suite. The verification loop for every task is:

1. Edit code.
2. Run `/mc2-build` (or `cmake --build build64 --config RelWithDebInfo -- /m`). Expected: clean compile, no warnings on touched files.
3. For task groups ending in "build + deploy": run `/mc2-build-deploy` (builds then copies `mc2.exe` + shaders to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`).
4. For task groups ending in "in-game verify": close any running `mc2.exe`, launch the deployed exe, load mission `mis0101`, play 30 seconds at spawn, quit, then `grep -E '\[TGL_POOL|\[DESTROY|\[GL_ERROR|\[INSTR' mc2.log` (log path is where MC2 writes stdout — on Windows typically `A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.log` or the console window if run from a terminal).

If `/mc2-build` and `/mc2-deploy` skills exist in `.claude/skills/`, prefer them. Otherwise the raw commands are in the skill files and in `CLAUDE.md`.

---

## Task 0: Capture pre-change perf baseline

**Files:** none (measurement only).

- [ ] **Step 1: Confirm baseline build is deployable.**

Close any running `mc2.exe`. Run `/mc2-build-deploy` with the current `claude/stability-tier1` HEAD unmodified. Expected: build succeeds, deploy completes, `diff -q` of deployed shaders against source reports no differences.

- [ ] **Step 2: Launch deployed exe, load mis0101.**

Open `A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe`. Navigate to `mis0101` from main menu. When mission loaded and camera is at spawn position, take note of the exact camera position (or leave the mouse untouched so it stays at the default spawn position for reproducibility).

- [ ] **Step 3: Capture 30s Tracy trace.**

Open Tracy profiler GUI, connect to the running MC2 process. Start capture, play 30 seconds (camera stationary at spawn). Save trace to `docs/superpowers/plans/baseline-mis0101-pre.tracy`.

If Tracy is not running or does not connect: note the issue, eyeball median/P99 frame time from the in-game FPS counter for 30 seconds and record in `docs/superpowers/plans/baseline-mis0101-pre.txt`. Document why Tracy wasn't available. Halt and escalate before proceeding if no measurement at all is possible.

- [ ] **Step 4: Record baseline numbers.**

In a new file `docs/superpowers/plans/baseline-mis0101-pre.md`, record: median, P99, P99.9 frame time (ms). Commit the file.

```bash
git add docs/superpowers/plans/baseline-mis0101-pre.md docs/superpowers/plans/baseline-mis0101-pre.tracy 2>/dev/null
git commit -m "perf: capture mis0101 baseline pre-instrumentation

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Commit 1: TGL Pool Exhaustion Trace + Monotonic Summary

### Task 1: Extend pool method signatures

**Files:**
- Modify: `mclib/tgl.h` (class declarations for `TG_VertexPool`, `TG_ColorPool`, `TG_FacePool`, `TG_ShadowPool`, `TG_TrianglePool`, and their common base if one exists)

- [ ] **Step 1: Locate the five pool method declarations.**

Run:
```bash
grep -n 'getVerticesFromPool\|getColorsFromPool\|getFacesFromPool\|getShadowsFromPool\|getTrianglesFromPool' mclib/tgl.h
```

Expected: 5 declarations around line 1020-1080.

- [ ] **Step 2: Add `caller` and `shape` parameters to each declaration.**

For each of the five methods, change the declaration to accept the new parameters. Example for the vertex pool:

```cpp
// BEFORE (mclib/tgl.h near line 1022)
gos_VERTEX * getVerticesFromPool (DWORD numRequested);

// AFTER
gos_VERTEX * getVerticesFromPool (DWORD numRequested, const char* caller, const void* shape = nullptr);
```

Apply the same transformation to `getColorsFromPool`, `getFacesFromPool`, `getShadowsFromPool`, `getTrianglesFromPool`. Preserve the return types and any surrounding comments.

- [ ] **Step 3: Add per-pool counter fields and `recordNull` to the pool base class (or, if no common base, to each pool).**

Add the following fields after the existing `numVertices` / `totalVertices` members (or equivalent) in the pool class(es):

```cpp
// Tier-1 instrumentation — §2 of the stability spec.
// All accessed only from the render thread; no locks required.
struct TGL_NullSnapshot {
    const char* caller;      // __FUNCTION__ of the first NULL-returning call this frame
    const void* shape;       // shape pointer if passed, else nullptr
    DWORD       numRequested;
    DWORD       numVertices_at_failure;
    DWORD       totalVertices;
};
DWORD            nullCountThisFrame = 0;
uint64_t         nullCountMonotonic = 0;
TGL_NullSnapshot firstNullSnapshot  = {};

void recordNull(const char* caller, DWORD numRequested, const void* shape);
void resetFrameCounters(); // called by frame-end drain after emitting log line
```

If the five pools inherit from a common `TG_Pool` base, add these to the base. If they're independent classes, add to each (five copies — acceptable; this is a small engine, not a framework).

### Task 2: Implement `recordNull` and pool method bodies

**Files:**
- Modify: `mclib/tgl.cpp` — pool method implementations (around line 1667 or wherever the NULL branch lives)

- [ ] **Step 1: Find the current NULL branch in each pool method.**

Run:
```bash
grep -n 'return NULL\|return nullptr\|return 0;' mclib/tgl.cpp | head -20
```

Locate the NULL return for each of the five pools. Existing pattern is roughly:

```cpp
gos_VERTEX* TG_VertexPool::getVerticesFromPool(DWORD n) {
    numVertices += n;
    if (numVertices < totalVertices) { result = nextVertex; nextVertex += n; }
    return result;  // NULL if exhausted
}
```

- [ ] **Step 2: Update each pool method to accept the new params and record on NULL.**

Transformation for the vertex pool (apply equivalent to the other four):

```cpp
gos_VERTEX* TG_VertexPool::getVerticesFromPool(DWORD n, const char* caller, const void* shape) {
    gos_VERTEX* result = nullptr;
    if (numVertices + n < totalVertices) {
        result = nextVertex;
        nextVertex += n;
        numVertices += n;
    }
    if (!result) {
        recordNull(caller, n, shape);
    }
    return result;
}
```

Note: preserve existing behavior (increment order, comparison style) as closely as possible. The only behavior change is the `recordNull` call on NULL — no other semantics shift.

- [ ] **Step 3: Implement `recordNull` once per pool class (or on the base).**

```cpp
void TG_Pool::recordNull(const char* caller, DWORD numRequested, const void* shape) {
    if (nullCountThisFrame == 0) {
        firstNullSnapshot.caller                 = caller;
        firstNullSnapshot.shape                  = shape;
        firstNullSnapshot.numRequested           = numRequested;
        firstNullSnapshot.numVertices_at_failure = numVertices;
        firstNullSnapshot.totalVertices          = totalVertices;
    }
    nullCountThisFrame++;
    nullCountMonotonic++;
}

void TG_Pool::resetFrameCounters() {
    nullCountThisFrame = 0;
    firstNullSnapshot  = {};
    // nullCountMonotonic intentionally NOT reset
}
```

(If no common base exists, copy-paste into each of the five pool .cpp blocks.)

### Task 3: Define `MC2_TGL_GET_*` macros

**Files:**
- Modify: `mclib/tgl.h` (near top of file, after includes)

- [ ] **Step 1: Add macros under the pool class declarations.**

Append at an appropriate spot in `mclib/tgl.h` (after the pool classes, outside any namespace):

```cpp
// -- Tier-1 instrumentation macros (stability spec §2.1) --------------------
#define MC2_TGL_GET_VERTS(pool, n)     (pool)->getVerticesFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_COLORS(pool, n)    (pool)->getColorsFromPool((n),  __FUNCTION__)
#define MC2_TGL_GET_FACES(pool, n)     (pool)->getFacesFromPool((n),   __FUNCTION__)
#define MC2_TGL_GET_SHADOW(pool, n)    (pool)->getShadowsFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_TRIANGLES(pool, n) (pool)->getTrianglesFromPool((n), __FUNCTION__)

// Shape-aware form — pass the caller's shape pointer so the first-null snapshot
// carries shape identity. Use at TG_Shape::TransformShape (mclib/tgl.cpp:1667).
#define MC2_TGL_GET_VERTS_FOR_SHAPE(pool, n, shape) \
    (pool)->getVerticesFromPool((n), __FUNCTION__, (shape))
```

### Task 4: Convert TGL call sites to macros

**Files:**
- Modify: `mclib/tgl.cpp` (lines around 1667-1675 per earlier grep)

- [ ] **Step 1: Convert the six call sites in `TG_Shape::TransformShape`.**

```cpp
// BEFORE (mclib/tgl.cpp:1667)
listOfVertices       = vertexPool->getVerticesFromPool(numVertices);
listOfColors         = colorPool->getColorsFromPool(numVertices);
listOfShadowTVertices = shadowPool->getShadowsFromPool(numVertices);
listOfTriangles      = trianglePool->getTrianglesFromPool(numTriangles);
listOfVisibleFaces   = facePool->getFacesFromPool(numTriangles);
listOfVisibleShadows = facePool->getFacesFromPool(numTriangles);

// AFTER
listOfVertices       = MC2_TGL_GET_VERTS_FOR_SHAPE(vertexPool, numVertices, this);
listOfColors         = MC2_TGL_GET_COLORS(colorPool, numVertices);
listOfShadowTVertices = MC2_TGL_GET_SHADOW(shadowPool, numVertices);
listOfTriangles      = MC2_TGL_GET_TRIANGLES(trianglePool, numTriangles);
listOfVisibleFaces   = MC2_TGL_GET_FACES(facePool, numTriangles);
listOfVisibleShadows = MC2_TGL_GET_FACES(facePool, numTriangles);
```

The vertex-pool line uses the shape-aware variant so the snapshot captures `this` (the `TG_Shape*`).

- [ ] **Step 2: Confirm no other TGL pool call sites.**

```bash
grep -rn 'getVerticesFromPool\|getColorsFromPool\|getFacesFromPool\|getShadowsFromPool\|getTrianglesFromPool' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h' | grep -v 'tgl.h' | grep -v 'MC2_TGL_GET'
```

Expected: no results (all six moved, no other callers).

If unexpected results appear: convert each remaining site to the appropriate macro.

### Task 5: Frame-end drain + monotonic summary + canonical frame counter

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` — define the canonical `g_mc2FrameCounter` at TU scope; increment it in the frame-end path.
- Modify: `mclib/tgl.cpp` — add `drainTglPoolStats()` and the monotonic summary emission at the bottom of the file. Declares `extern uint32_t g_mc2FrameCounter` at the top.
- Modify: `mclib/tgl.h` — expose `drainTglPoolStats()` as a free function.
- Modify: `GameOS/gameos/gameosmain.cpp` — one call to `drainTglPoolStats()` at the frame-end site (identified below).

**Canonical frame counter decision (locked in here for reuse by Tasks 12, 21):**

One global `uint32_t g_mc2FrameCounter`, defined in `GameOS/gameos/gameosmain.cpp` at file scope, `extern`'d from every other TU that needs it. Incremented exactly once per frame at the frame-end site, just before `drainTglPoolStats()`. This is the single source of truth for the `frame=` field in all three `[SUBSYS vN]` log lines.

- [ ] **Step 0: Define the canonical frame counter.**

In `GameOS/gameos/gameosmain.cpp` near the top (file-scope, outside any namespace):

```cpp
// Tier-1 instrumentation (stability spec §5.1): single source of truth for
// the frame=... field used by TGL, DESTROY, and GL_ERROR log lines.
// Declared extern from mclib/tgl.cpp, code/gameobj.cpp, GameOS/gameos/gos_validate.cpp.
uint32_t g_mc2FrameCounter = 0;
```

- [ ] **Step 1: Implement `drainTglPoolStats()` in `mclib/tgl.cpp`.**

```cpp
// At file scope near the bottom of mclib/tgl.cpp:
#include <stdlib.h>  // getenv
#include <stdio.h>   // printf, fflush

static const bool s_tglPoolTrace = (getenv("MC2_TGL_POOL_TRACE") != nullptr);
extern uint32_t   g_mc2FrameCounter;  // defined in GameOS/gameos/gameosmain.cpp (Task 5 step 0)

static const char* poolName(int idx) {
    switch (idx) {
        case 0: return "vertex";
        case 1: return "color";
        case 2: return "face";
        case 3: return "shadow";
        case 4: return "triangle";
        default: return "?";
    }
}

void drainTglPoolStats() {
    // Frame counter is incremented at the call site in gameosmain.cpp,
    // immediately before drainTglPoolStats(). Do NOT increment here.

    // Assumes external pointers to the five pools exist — replace with the
    // actual pool accessors / globals used by mission.cpp init.
    extern TG_VertexPool*   vertexPool;
    extern TG_ColorPool*    colorPool;
    extern TG_FacePool*     facePool;
    extern TG_ShadowPool*   shadowPool;
    extern TG_TrianglePool* trianglePool;
    TG_Pool* pools[5] = { vertexPool, colorPool, facePool, shadowPool, trianglePool };

    // Per-frame print (env-gated) — one line per pool with nulls this frame.
    if (s_tglPoolTrace) {
        for (int i = 0; i < 5; i++) {
            TG_Pool* p = pools[i];
            if (!p || p->nullCountThisFrame == 0) continue;
            printf("[TGL_POOL v1] frame=%u pool=%s nulls=%u first_caller=%s shape=%p req=%u high_water=%u/%u mono_total=%llu\n",
                (unsigned)g_mc2FrameCounter,
                poolName(i),
                (unsigned)p->nullCountThisFrame,
                p->firstNullSnapshot.caller ? p->firstNullSnapshot.caller : "?",
                p->firstNullSnapshot.shape,
                (unsigned)p->firstNullSnapshot.numRequested,
                (unsigned)p->firstNullSnapshot.numVertices_at_failure,
                (unsigned)p->firstNullSnapshot.totalVertices,
                (unsigned long long)p->nullCountMonotonic);
            fflush(stdout);
        }
    }

    // Monotonic summary every 600 frames (always, not env-gated).
    if ((g_mc2FrameCounter > 0) && ((g_mc2FrameCounter % 600) == 0)) {
        printf("[TGL_POOL v1] summary mono_total={vertex:%llu, color:%llu, face:%llu, shadow:%llu, triangle:%llu} since=process_start\n",
            (unsigned long long)(pools[0] ? pools[0]->nullCountMonotonic : 0),
            (unsigned long long)(pools[1] ? pools[1]->nullCountMonotonic : 0),
            (unsigned long long)(pools[2] ? pools[2]->nullCountMonotonic : 0),
            (unsigned long long)(pools[3] ? pools[3]->nullCountMonotonic : 0),
            (unsigned long long)(pools[4] ? pools[4]->nullCountMonotonic : 0));
        fflush(stdout);
    }

    // Reset per-frame counters on all five pools.
    for (int i = 0; i < 5; i++) if (pools[i]) pools[i]->resetFrameCounters();
}

// On shutdown — emit final summary. Call this from mission teardown or process-exit path.
void drainTglPoolStatsOnShutdown() {
    extern TG_VertexPool*   vertexPool;
    extern TG_ColorPool*    colorPool;
    extern TG_FacePool*     facePool;
    extern TG_ShadowPool*   shadowPool;
    extern TG_TrianglePool* trianglePool;
    TG_Pool* pools[5] = { vertexPool, colorPool, facePool, shadowPool, trianglePool };
    printf("[TGL_POOL v1] summary mono_total={vertex:%llu, color:%llu, face:%llu, shadow:%llu, triangle:%llu} since=process_start (shutdown)\n",
        (unsigned long long)(pools[0] ? pools[0]->nullCountMonotonic : 0),
        (unsigned long long)(pools[1] ? pools[1]->nullCountMonotonic : 0),
        (unsigned long long)(pools[2] ? pools[2]->nullCountMonotonic : 0),
        (unsigned long long)(pools[3] ? pools[3]->nullCountMonotonic : 0),
        (unsigned long long)(pools[4] ? pools[4]->nullCountMonotonic : 0));
    fflush(stdout);
}
```

**Note on pool accessors:** MC2 may expose the five pools as globals (`extern` as shown) or as members of a Mission struct. Check `mission.cpp` line ~3097 (where the pools are initialized per the spec reference) to confirm the actual access path and adjust the `extern` lines accordingly. If they live on `Mission`, change to `mission->vertexPool` etc. and accept a Mission pointer parameter.

- [ ] **Step 2: Expose both functions from `mclib/tgl.h`.**

```cpp
// mclib/tgl.h, near the macros from Task 3:
void drainTglPoolStats();
void drainTglPoolStatsOnShutdown();
```

- [ ] **Step 3: Wire up the frame-end call in `GameOS/gameos/gameosmain.cpp`.**

Open `GameOS/gameos/gameosmain.cpp`. The `main()` function starts at line 463. Find the per-frame loop inside it — there is exactly one. Look for the place where the engine signals end-of-frame (typically a call to `SwapBuffers`, `SDL_GL_SwapWindow`, `wglSwapBuffers`, or a function named `gos_RenderDoneFrame`/equivalent). If unsure, grep:

```bash
grep -n 'SwapBuffers\|SwapWindow\|RenderDone\|gos_DrawQuad\|buffer.*swap' GameOS/gameos/gameosmain.cpp | head -10
```

Immediately before that swap call, insert:

```cpp
#include "tgl.h"  // at top of file if not already included

// ... inside the per-frame loop, just before the buffer-swap call ...
g_mc2FrameCounter++;     // canonical frame counter (Step 0 defined the global)
drainTglPoolStats();     // emits per-frame and monotonic log lines, then resets per-frame counters
// ... existing SwapBuffers/SwapWindow call follows ...
```

- [ ] **Step 4: Wire up the shutdown call.**

Find mission teardown (`mission->destroy` / `Mission::~Mission`) or the process-exit path. Add:

```cpp
drainTglPoolStatsOnShutdown();
```

If the spec's "process shutdown" moment is hard to locate, mission-end is acceptable (one summary per mission rather than one per process — still surfaces the data).

### Task 6: Build, deploy, in-game verify TGL instrumentation

- [ ] **Step 1: Build.**

Run `/mc2-build` (or `cmake --build build64 --config RelWithDebInfo -- /m`). Expected: clean compile.

If compile fails: fix per compiler output (most likely a signature mismatch or a missing `#include`).

- [ ] **Step 2: Deploy.**

Close any running `mc2.exe`. Run `/mc2-deploy`. Expected: `mc2.exe` + shaders copied to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`, `diff -q` reports no differences.

- [ ] **Step 3: In-game verify with trace gate OFF.**

Set env without the trace: launch `mc2.exe` from a console (so stdout is captured), load `mis0101`, play 30 seconds, quit. Grep the log:

```bash
grep -E '\[TGL_POOL' mc2.log  # or wherever stdout is captured
```

Expected: only `[TGL_POOL v1] summary` lines (one every ~600 frames = every ~3s at 200fps = up to 10 lines in 30s). No per-frame `nulls=` lines.

- [ ] **Step 4: In-game verify with trace gate ON.**

From a console:
```bash
SET MC2_TGL_POOL_TRACE=1
mc2.exe
```

Load `mis0101`, play 30 seconds. Quit. Grep the log.

Expected: if pool exhaustion occurs, one `[TGL_POOL v1] frame=... nulls=...` line per broken frame per pool. On the default pool sizes (500K vertex etc.) exhaustion is unlikely on a standard mission; a clean log here means "instrumentation wired up and silent in the happy path" — that is the success condition.

If no lines appear at all (no summary either): the `drainTglPoolStats` call is not being reached. Verify the frame-end hook location.

### Task 7: Commit 1

- [ ] **Step 1: Review diff.**

```bash
git diff --stat
```

Expected: changes to `mclib/tgl.h`, `mclib/tgl.cpp`, and one `GameOS/gameos/*.cpp` file.

- [ ] **Step 2: Commit.**

```bash
git add mclib/tgl.h mclib/tgl.cpp GameOS/gameos/gameosmain.cpp
git commit -m "$(cat <<'EOF'
feat(instr): TGL pool exhaustion trace + monotonic summary

Adds one-line-per-broken-frame print (env-gated MC2_TGL_POOL_TRACE) and
always-on monotonic summary every 600 frames + on shutdown. No game
behavior change; failure mode for "shapes just vanished" is now
diagnosable from stdout.

Part 1 of 4 for stability tier-1 instrumentation. See
docs/superpowers/specs/2026-04-23-stability-tier1-instrumentation-design.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Commit 2: GameObject::destroy Wrapper + Helper + Counter (no site conversions)

### Task 8: Add `framesSinceActive` + `lastUpdateRet` fields to `GameObject`

**Files:**
- Modify: `code/gameobj.h` around line 340 (existing `drawFlags` / member block, near other small-int fields for alignment)

- [ ] **Step 1: Add fields to `GameObject` class body.**

Insert after `drawFlags` (line 341 area) — group with similar-sized members for layout sanity:

```cpp
// -- Tier-1 instrumentation: destruction visibility (stability spec §3.3-3.4) --
// framesSinceActive: zero if the composite active decision (inView ||
// canBeSeen || objBlockInfo.active) held this frame, saturating-increment
// otherwise. Updated from one site in GameObjectManager::update — see §3.3.
// lastUpdateRet: cache of most recent update() return value, -1 sentinel if
// update() has never been called. Cached at update() return sites — see §3.4.
uint8_t  framesSinceActive = 0;
int32_t  lastUpdateRet     = -1;  // spec said int8_t; corrected: update() returns long
```

- [ ] **Step 2: Verify compile (no logic yet, just the layout change).**

Run `/mc2-build`. Expected: clean compile. If the implicit member initializers hit an issue with MSVC's ancient constructor-list pattern, move the defaults into the constructor/init method instead.

### Task 9: Add three virtual accessors on `GameObject` for log/sweep consumption

**Context — why this task exists.** Verification during plan-writing showed:

- `inView` is declared on subclasses only (e.g. `Actor`, `Artillery`), **not** on `GameObject` base.
- `canBeSeen()` is a method on `Appearance`, reached via `obj->appearance->canBeSeen()`.
- `objBlockInfo` is a static array on `Terrain` (`Terrain::objBlockInfo[blockNumber].active`), not a per-object pointer.

The destroy log and the frame-active sweep both need one uniform "active-this-frame" answer per object. Plan commits to **three virtual getters on `GameObject` base with safe defaults**; subclasses override where they have a direct field. This is the only cross-subsystem interface change in the whole plan, and it's small.

**Files:**
- Modify: `code/gameobj.h` — add three `virtual bool` methods with defaults on `GameObject`.
- Modify: `code/actor.h` (or wherever `Actor` lives) and similarly for any subclass with a direct `inView` field — override `inView_instr()` to return the field.

- [ ] **Step 1: Add the three virtual accessors to `GameObject` in `code/gameobj.h`.**

Inside the `GameObject` class body, near `setExists` (line 898 area):

```cpp
// -- Tier-1 instrumentation accessors (stability spec §3.3-3.4) --
// Three composite-active-decision getters. Safe defaults: appearance-based
// visibility for can_be_seen, false for the rest. Subclasses override where
// they have a direct field.
virtual bool inView_instr() const { return false; }
virtual bool canBeSeen_instr() const {
    return appearance ? appearance->canBeSeen() : false;
}
virtual bool blockActive_instr() const { return false; }
```

(Names have an `_instr` suffix so they're grep-distinguishable from any existing `canBeSeen`/`inView` accessors, and so renaming or removing them in a future spec is surgical.)

- [ ] **Step 2: Override `inView_instr()` in subclasses that have a direct `inView` field.**

From the grep in plan-writing, these subclasses have `inView` as a direct member:
- `Actor` (code/actor.h)
- `Artillery` (code/artlry.h:187, 212)

In each header, inside the class body, add:

```cpp
bool inView_instr() const override { return inView != 0; }
```

Other subclasses (`Mech`, `GroundVehicle`, `Building`, `Gate`, etc.) fall through to the base default, which returns `false`. That's acceptable — the log field will read `in_view=0` for objects that don't own that notion, which is correct information.

- [ ] **Step 3: (Deliberately left out — no `blockActive_instr()` override.)**

Computing `Terrain::objBlockInfo[obj->block_number].active` correctly requires an obj→block lookup that is not a one-line accessor on every subclass. The default `false` is accepted; if follow-up specs want this signal, a dedicated pass can thread block-number access through the subclasses. For this spec, `block_active=0` across the board is consistent, honest, and cheap.

### Task 9b: Single-site `framesSinceActive` update in the per-frame sweep

**Files:**
- Modify: `code/objmgr.cpp` — one snippet in the per-frame object sweep

- [ ] **Step 1: Locate the per-frame sweep.**

```bash
grep -n 'GameObjectManager::update\|for.*objList\|for.*numObjects' code/objmgr.cpp | head -20
```

Look for the single loop that iterates every `GameObject*` once per frame. In MC2 this is typically `GameObjectManager::update()` or an equivalent sweep.

- [ ] **Step 2: Insert the composite-active-decision snippet.**

Inside the loop body, before any cull-driven early-continue:

```cpp
// Tier-1 instrumentation (stability spec §3.3): single source of truth for
// framesSinceActive. Uses the three virtual accessors added in Task 9.
bool activeThisFrame_instr =
       obj->inView_instr()
    || obj->canBeSeen_instr()
    || obj->blockActive_instr();
if (activeThisFrame_instr) {
    obj->framesSinceActive = 0;
} else if (obj->framesSinceActive < 255) {
    obj->framesSinceActive++;
}
```

No further adaptation needed — the accessors encapsulate subclass-specific access.

### Task 10: Cache `lastUpdateRet` at update() return sites

**Scope callout (explicit — this is the largest edit-surface in the plan):**

`lastUpdateRet` is a spec-required field (§3.4) that needs writes at every site where `GameObject::update()` or its subclass overrides are called and observed. This grows the commit's edit surface beyond `code/gameobj.{h,cpp}` + `objmgr.cpp`: expect 3-10 additional files touched. The alternative (don't cache; leave `last_update_ret=-1` always) was rejected during spec review because it removes the log field's diagnostic value for exactly the "stale update" bug class this instrumentation targets.

Each site gets a one-line addition (`obj->lastUpdateRet = (int32_t)obj->update();`). No surrounding code is restructured. The "no behavior changes" premise holds because the cache is a write-only field read only by the log path.


**Files:**
- Modify: sites where `obj->update()` is called and the return value observed

- [ ] **Step 1: Locate update() callers.**

```bash
grep -rn '->update\s*(\s*)' code/ --include='*.cpp' | grep -v '_update\|updateGeometry\|updateSelection\|updateDebugWindow' | head -30
```

Filter for call sites that capture the return value (pattern `x = obj->update()` or `if (obj->update()`).

- [ ] **Step 2: Update each call site to cache the return value.**

At each site:

```cpp
// BEFORE
if (obj->update() != NO_ERR) { ... }

// AFTER
long updateRet_instr = obj->update();
obj->lastUpdateRet   = (int32_t)updateRet_instr;
if (updateRet_instr != NO_ERR) { ... }
```

For sites that discard the return value, simply cache:

```cpp
// BEFORE
obj->update();

// AFTER
obj->lastUpdateRet = (int32_t)obj->update();
```

Expected number of sites: likely 3-10. If grep returns more, apply the same transformation to all of them. Keep the implementer's edit surface limited to the one-line additions — do not restructure surrounding code.

### Task 11: Add `getObjectClassName` helper

**Files:**
- Modify: `code/gameobj.h` (declaration), `code/gameobj.cpp` (definition)

- [ ] **Step 1: Add declaration near the top of `code/gameobj.h`.**

Find the `ObjectClass` enum (grep `enum.*ObjectClass\|ObjectClass {` in `code/*.h`). Immediately after the enum, add:

```cpp
// Stringify an ObjectClass enumerator for log output. Returns a static
// string; never NULL. Unknown enum → "UNKNOWN".
const char* getObjectClassName(ObjectClass kind);
```

- [ ] **Step 2: Add definition in `code/gameobj.cpp`.**

Near other free helpers at the top of the .cpp file:

```cpp
const char* getObjectClassName(ObjectClass kind) {
    switch (kind) {
        case BATTLEMECH:     return "BattleMech";
        case GROUNDVEHICLE:  return "GroundVehicle";
        case ELEMENTAL:      return "Elemental";
        case BUILDING:       return "Building";
        case TREEBUILDING:   return "TreeBuilding";
        case GATE:           return "Gate";
        case TURRET:         return "Turret";
        case TERRAINOBJECT:  return "TerrainObject";
        case ARTILLERY:      return "Artillery";
        case WEAPONBOLT:     return "WeaponBolt";
        case EXPLOSION:      return "Explosion";
        case LIGHT:          return "Light";
        // Add every other enumerator from the ObjectClass enum — do not
        // omit any, or the default branch hides real cases.
        default:             return "UNKNOWN";
    }
}
```

Implementer: open the `ObjectClass` enum and ensure every case is covered. If an enumerator name differs from the above (common in older codebases — e.g., `MECH` vs `BATTLEMECH`), use the actual enum spelling.

### Task 12: Add `GameObject::destroy` + `MC2_DESTROY` macro

**Files:**
- Modify: `code/gameobj.h` (method declaration, macro, taxonomy comment placeholder)
- Modify: `code/gameobj.cpp` (method definition)

- [ ] **Step 1: Declare method + macro in `code/gameobj.h`.**

Inside the `GameObject` class body, near `setExists` (line 898 area):

```cpp
// -- Tier-1 instrumentation: destruction wrapper (stability spec §3.1-3.2) --
// Canonical destruction reasons (keep in sync with MC2_DESTROY call sites):
//   <placeholder — filled in commit 3 after reason-string dedup>
//
// Null-pointer contract: caller must pass non-null obj; same contract as
// direct setExists(false) today. Wrapper does not null-check.
void destroy(const char* reason, const char* file, int line);
```

And outside the class body, near the bottom of the header:

```cpp
// Use this macro at every site that currently calls setExists(false).
// Commit 3 converts all 34 literal sites.
#define MC2_DESTROY(obj, reason) (obj)->destroy((reason), __FILE__, __LINE__)
```

- [ ] **Step 2: Implement the method in `code/gameobj.cpp`.**

Near other `GameObject` method definitions:

```cpp
#include <stdlib.h>  // getenv
#include <stdio.h>   // printf, fflush

static const bool s_destroyTrace = (getenv("MC2_DESTROY_TRACE") != nullptr);

extern uint32_t g_mc2FrameCounter;  // defined by TGL drain (Task 5) or a shared frame counter.
// If no shared counter exists, declare one locally in this file and increment
// alongside the TGL drain — cheap, and this instrumentation needs a frame tag.

void GameObject::destroy(const char* reason, const char* file, int line) {
    // 1. Snapshot all log fields FIRST, before any other logic runs (spec §3.2).
    const int         snap_exists_was      = getExists() ? 1 : 0;
    const ObjectClass snap_kind            = objectClass;
    const int         snap_in_view         = inView_instr()     ? 1 : 0;
    const int         snap_can_be_seen     = canBeSeen_instr()  ? 1 : 0;
    const int         snap_block_active    = blockActive_instr()? 1 : 0;
    const int         snap_frames_inactive = (int)framesSinceActive;
    const int32_t     snap_last_update_ret = lastUpdateRet;

    // 2. Double-destroy: log-and-return without re-calling setExists.
    if (snap_exists_was == 0) {
        if (s_destroyTrace) {
            printf("[DESTROY v1] frame=%u obj=%p kind=%s reason=%s file=%s line=%d exists_was=0 in_view=%d can_be_seen=%d block_active=%d frames_since_active=%d last_update_ret=%d\n",
                g_mc2FrameCounter, (void*)this, getObjectClassName(snap_kind), reason,
                file, line, snap_in_view, snap_can_be_seen, snap_block_active,
                snap_frames_inactive, (int)snap_last_update_ret);
            fflush(stdout);
        }
        return;
    }

    // 3. Real destruction — unchanged existing behavior.
    setExists(false);

    // 4. Env-gated log line.
    if (s_destroyTrace) {
        printf("[DESTROY v1] frame=%u obj=%p kind=%s reason=%s file=%s line=%d exists_was=1 in_view=%d can_be_seen=%d block_active=%d frames_since_active=%d last_update_ret=%d\n",
            g_mc2FrameCounter, (void*)this, getObjectClassName(snap_kind), reason,
            file, line, snap_in_view, snap_can_be_seen, snap_block_active,
            snap_frames_inactive, (int)snap_last_update_ret);
        fflush(stdout);
    }
}
```

**Concrete bindings (no placeholders):**
- The three accessors (`inView_instr`, `canBeSeen_instr`, `blockActive_instr`) are the virtual getters added in Task 9 — method calls are defined, defaults are safe, subclass overrides live in Actor/Artillery.
- `g_mc2FrameCounter` is the canonical global defined in Task 5 (`GameOS/gameos/gameosmain.cpp`, `extern uint32_t g_mc2FrameCounter`). Declared in `code/gameobj.cpp` with `extern "C" uint32_t g_mc2FrameCounter;` near the top.
- `ObjectClass` is defined in `code/gameobj.h`, so `#include "gameobj.h"` at the top of `code/gameobj.cpp` (already included) covers visibility.

### Task 13: Build, deploy, verify wrapper compiles + is silent

- [ ] **Step 1: Build.**

Run `/mc2-build`. Expected: clean compile.

Most likely failure: a subclass override of one of the three `*_instr()` accessors added in Task 9 has a typo or a wrong field name. Fix per compiler message; the accessors themselves have safe defaults, so base-class build is always green.

- [ ] **Step 2: Deploy + launch.**

Run `/mc2-build-deploy`. Close any running `mc2.exe`. Launch. Load `mis0101`. Play 30 seconds.

**With `MC2_DESTROY_TRACE` unset** (default): zero `[DESTROY]` lines in the log — correct, because no call sites have been converted yet (commit 3 does that).

**With `MC2_DESTROY_TRACE=1`**: still zero `[DESTROY]` lines — same reason. This is the "wrapper is wired but dormant" state. That's the success condition for commit 2.

- [ ] **Step 3: Confirm no regression.**

Play 2 minutes of mission gameplay (kill a mech, watch buildings destroyed, let mission cleanup run). Expected: same visual behavior as pre-commit. No crashes, no new visual artifacts.

### Task 14: Commit 2

```bash
git add code/gameobj.h code/gameobj.cpp code/objmgr.cpp code/actor.h code/artlry.h  # + any update-call-site files from Task 10
git commit -m "$(cat <<'EOF'
feat(instr): GameObject::destroy wrapper + helper + frames-active counter

Adds GameObject::destroy(reason, file, line), MC2_DESTROY macro,
getObjectClassName helper, framesSinceActive counter (single-site update
in objmgr sweep), and lastUpdateRet cache. Zero call-site conversions
in this commit — dormant until commit 3 converts the 34 literal
setExists(false) sites.

Part 2 of 4 for stability tier-1 instrumentation. See
docs/superpowers/specs/2026-04-23-stability-tier1-instrumentation-design.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Commit 3: Convert 34 setExists(false) Sites + Dedup Taxonomy

### Task 15: Convert 26 sites in `code/objmgr.cpp`

**Files:**
- Modify: `code/objmgr.cpp`

- [ ] **Step 1: List every site.**

```bash
grep -n 'setExists\s*(\s*false' code/objmgr.cpp
```

Expected: 26 hits.

- [ ] **Step 2: Convert each site to `MC2_DESTROY`.**

For each site, read ±10 lines of context and choose a short literal reason. Draft list (implementer adjusts based on actual context):

```cpp
// Examples (the exact lines will differ — use surrounding context to pick the reason):
// BEFORE
obj->setExists(false);

// AFTER (mission cleanup loop)
MC2_DESTROY(obj, "mission_cleanup");

// AFTER (update returned failure)
MC2_DESTROY(obj, "update_false");

// AFTER (damage model zeroed hit points)
MC2_DESTROY(obj, "killed_by_damage");

// AFTER (object removed because pilot ejected)
MC2_DESTROY(obj, "pilot_ejected");
```

Preserve any surrounding code exactly — only replace the single `setExists(false)` call with `MC2_DESTROY(obj, "<reason>")`.

- [ ] **Step 3: Verify 26 conversions.**

```bash
grep -c 'MC2_DESTROY' code/objmgr.cpp
```

Expected: 26.

```bash
grep -n 'setExists\s*(\s*false' code/objmgr.cpp
```

Expected: 0 (all converted).

### Task 16: Convert remaining 8 literal sites in other files

**Files:**
- Modify: `code/ablmc2.cpp` (2 sites)
- Modify: `code/collsn.cpp` (2 sites)
- Modify: `code/gvehicl.cpp` (1 site)
- Modify: `code/mission.cpp` (2 sites)
- Modify: `code/weaponbolt.cpp` (1 site)

- [ ] **Step 1: Confirm counts per file.**

```bash
for f in code/ablmc2.cpp code/collsn.cpp code/gvehicl.cpp code/mission.cpp code/weaponbolt.cpp; do
    echo -n "$f: "
    grep -c 'setExists\s*(\s*false' $f
done
```

Expected: 2, 2, 1, 2, 1 = 8 total.

- [ ] **Step 2: Convert each site.**

Same pattern as Task 15. Read surrounding context, pick reason literal, replace.

Likely reasons by file (implementer confirms):
- `ablmc2.cpp` — ABL script-driven destruction. Candidates: `"abl_script_remove"`, `"abl_explicit_destroy"`.
- `collsn.cpp` — collision-driven. Candidates: `"collision_removed"`.
- `gvehicl.cpp` — vehicle-specific. Candidate: `"vehicle_destroyed"`.
- `mission.cpp` — mission teardown. Candidate: `"mission_teardown"`.
- `weaponbolt.cpp` — projectile lifecycle end. Candidate: `"bolt_expired"` or `"bolt_impact"`.

- [ ] **Step 3: Verify zero literal sites remain.**

```bash
grep -rn 'setExists\s*(\s*false' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h'
```

Expected: exactly one result — `code/gameobj.cpp:<linenum>: setExists(false);` (the internal call inside `GameObject::destroy`). Zero elsewhere.

### Task 17: Review 4 non-literal `setExists(<expr>)` sites

**Files:**
- Modify: varies (4 sites spread across the codebase)

- [ ] **Step 1: List the 4 sites.**

```bash
grep -rn 'setExists\s*(' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h' | grep -v '(\s*true' | grep -v '(\s*false'
```

Expected: 4 hits.

- [ ] **Step 2: Inspect each site.**

For each hit: read the expression passed to `setExists`. Categorize:

1. **Always-true in practice:** leave unchanged. Add a one-line comment `// non-literal setExists: <expr> is always true here (<why>)`.
2. **Can be false at runtime:** convert to:

```cpp
// BEFORE
obj->setExists(someCondition);

// AFTER
if (someCondition) {
    obj->setExists(true);
} else {
    MC2_DESTROY(obj, "<reason_matching_condition>");
}
```

- [ ] **Step 3: Document the 4 decisions.**

In the eventual commit message, list each site + decision (converted / left with comment). Implementer keeps a running note as they go.

### Task 18: Dedup reason strings + fill taxonomy comment block

**Files:**
- Modify: `code/gameobj.h` (taxonomy comment block placeholder from Task 12)

- [ ] **Step 1: Extract the reason-string set.**

```bash
grep -rhn 'MC2_DESTROY' code/ mclib/ GameOS/ --include='*.cpp' | grep -oP 'MC2_DESTROY\s*\([^,]+,\s*"\K[^"]+' | sort -u
```

Expected: the set of unique reason literals used across all 34+ conversions.

- [ ] **Step 2: Collapse near-duplicates.**

Manually review the set. If `update_false` and `update_returned_false` both appear, pick one canonical form (e.g., `update_false`) and grep-replace the losing variant across all files:

```bash
# Example — only run if duplicates actually exist
grep -rln 'update_returned_false' code/ | xargs sed -i 's/update_returned_false/update_false/g'
```

Repeat for any other near-duplicates.

- [ ] **Step 3: Re-extract and fill the taxonomy comment block.**

```bash
grep -rhn 'MC2_DESTROY' code/ --include='*.cpp' | grep -oP 'MC2_DESTROY\s*\([^,]+,\s*"\K[^"]+' | sort -u
```

Copy the final list into `code/gameobj.h`, replacing the `<placeholder>` line from Task 12:

```cpp
// Canonical destruction reasons (keep in sync with MC2_DESTROY call sites):
//   abl_script_remove    — ABL script explicitly requested object removal
//   bolt_expired         — weapon bolt lifetime elapsed
//   bolt_impact          — weapon bolt hit target and was consumed
//   collision_removed    — removed by collision resolution
//   killed_by_damage     — damage model zeroed hit points
//   mission_cleanup      — removed during mission cleanup sweep
//   mission_teardown     — mission ending, global teardown
//   pilot_ejected        — pilot ejected; chassis removed
//   update_false         — update() returned non-NO_ERR
//   vehicle_destroyed    — ground vehicle destroyed
//   ... (final list produced by dedup)
```

(Adjust entries based on the actual final set.)

### Task 19: Build, deploy, verify `[DESTROY]` fires in-game

- [ ] **Step 1: Build + deploy.**

Run `/mc2-build-deploy`. Expected: clean compile, clean deploy.

- [ ] **Step 2: Launch with trace gate ON.**

From a console:
```bash
SET MC2_DESTROY_TRACE=1
mc2.exe
```

Load `mis0101`. Play until at least one mech is killed (attack an enemy, or let the enemy attack you). Watch for combat destruction.

- [ ] **Step 3: Grep log for destroy events.**

```bash
grep '\[DESTROY v1\]' mc2.log | head -20
```

Expected: at minimum one line per destroyed object during the mission. Example expected line:

```
[DESTROY v1] frame=1234 obj=0x2a3f1800 kind=BattleMech reason=killed_by_damage file=objmgr.cpp line=1421 exists_was=1 in_view=1 can_be_seen=1 block_active=1 frames_since_active=0 last_update_ret=0
```

Fields to sanity-check:
- `kind=<ObjectClass>` is not `UNKNOWN` (helper covers all enumerators).
- `reason=<literal>` matches one of the canonical taxonomy entries.
- `frames_since_active` is mostly 0 for freshly-killed combat units; non-zero for cleanup-sweep destructions (that's the whole point of the field).
- `last_update_ret` is populated (not always `-1`).

If any field is systematically `-1` or `0` when it shouldn't be: the cache site from Task 10 or the sweep from Task 9 is missing a case. Investigate before commit.

- [ ] **Step 4: Mission-end teardown check.**

Abort the mission (or win it) to trigger teardown. Grep again:

```bash
grep '\[DESTROY v1\]' mc2.log | wc -l
```

Expected: a burst of lines at mission end with `reason=mission_cleanup` / `mission_teardown`. Confirms the cleanup path is traced.

- [ ] **Step 5: Invariant spot-check.**

```bash
grep -rn 'setExists\s*(\s*false' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h'
```

Expected: exactly one line — the internal call inside `GameObject::destroy`.

### Task 20: Commit 3

```bash
git add code/objmgr.cpp code/ablmc2.cpp code/collsn.cpp code/gvehicl.cpp code/mission.cpp code/weaponbolt.cpp code/gameobj.h
# Plus any non-literal site files from Task 17
git commit -m "$(cat <<'EOF'
feat(instr): convert 34 setExists(false) sites to MC2_DESTROY + taxonomy

Converts all 34 literal setExists(false) call sites to MC2_DESTROY(obj,
reason), reviews 4 non-literal setExists(<expr>) sites, dedupes the
reason strings, and commits the canonical taxonomy as a comment block
above destroy() in code/gameobj.h.

Invariant: grep for setExists(false) outside GameObject::destroy now
returns zero hits.

Part 3 of 4 for stability tier-1 instrumentation. See
docs/superpowers/specs/2026-04-23-stability-tier1-instrumentation-design.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Commit 4: GL Error Drain + Startup Banner + Script + CLAUDE.md

### Task 21: Implement `drainGLErrors` in `gos_validate.cpp`

**Files:**
- Modify: `GameOS/gameos/gos_validate.h` — add declaration
- Modify: `GameOS/gameos/gos_validate.cpp` — add implementation

- [ ] **Step 1: Add declaration in `gos_validate.h`.**

Near the existing `validateRecordShaderError` declaration:

```cpp
// Tier-1 instrumentation: drain the GL error queue at a render-pass
// boundary. Consumes pending errors (see spec §4.3). Always safe to call
// — if there are no errors, it's a single glGetError() returning GL_NO_ERROR.
void drainGLErrors(const char* pass);
```

- [ ] **Step 2: Add implementation in `gos_validate.cpp`.**

At the bottom of the file (before the final `}` of any namespace if present):

```cpp
// -- Tier-1 instrumentation: GL error drain (stability spec §4) -------------

#include <string.h>

static const bool s_glErrorDrainSilent = (getenv("MC2_GL_ERROR_DRAIN_SILENT") != nullptr);

// Seven known pass names (spec §4.1). Keep in the same order as the pass table.
enum GlDrainPass { GLP_SHADOW_STATIC = 0, GLP_SHADOW_DYNAMIC, GLP_TERRAIN, GLP_OBJECTS_3D, GLP_POST_PROCESS, GLP_HUD, GLP_FRAME, GLP_COUNT };

struct GlPassState {
    const char*  name;
    uint64_t     monoCount;         // monotonic total since process start
    uint32_t     lastPrintFrame;    // frame of most recent first-error print
    uint32_t     suppressedCount;   // errors accumulated during suppression window
    bool         inSuppression;
};

static GlPassState s_glPassState[GLP_COUNT] = {
    {"shadow_static",  0, 0, 0, false},
    {"shadow_dynamic", 0, 0, 0, false},
    {"terrain",        0, 0, 0, false},
    {"objects_3d",     0, 0, 0, false},
    {"post_process",   0, 0, 0, false},
    {"hud",            0, 0, 0, false},
    {"frame",          0, 0, 0, false},
};

extern uint32_t g_mc2FrameCounter;  // defined in GameOS/gameos/gameosmain.cpp (Task 5 step 0)

static int passIndex(const char* name) {
    for (int i = 0; i < GLP_COUNT; i++) {
        if (strcmp(name, s_glPassState[i].name) == 0) return i;
    }
    return -1;
}

static const char* glErrorName(GLenum e) {
    switch (e) {
        case GL_INVALID_ENUM:                  return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:                 return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:             return "GL_INVALID_OPERATION";
        case GL_STACK_OVERFLOW:                return "GL_STACK_OVERFLOW";
        case GL_STACK_UNDERFLOW:               return "GL_STACK_UNDERFLOW";
        case GL_OUT_OF_MEMORY:                 return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
#ifdef GL_CONTEXT_LOST
        case GL_CONTEXT_LOST:                  return "GL_CONTEXT_LOST";
#endif
        default:                               return "UNKNOWN";
    }
}

void drainGLErrors(const char* pass) {
    int pi = passIndex(pass);
    if (pi < 0) return;  // unknown pass name — drop silently rather than crash
    GlPassState& st = s_glPassState[pi];

    uint32_t errorsThisFrame = 0;
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        errorsThisFrame++;
        st.monoCount++;

        // Print only the first error per (pass, frame), and only outside
        // the 30-frame suppression window.
        bool shouldPrint =
               !s_glErrorDrainSilent
            && errorsThisFrame == 1
            && (g_mc2FrameCounter - st.lastPrintFrame) >= 30;

        if (shouldPrint) {
            // If we're exiting a suppression window, emit a summary line first.
            if (st.inSuppression && st.suppressedCount > 0) {
                printf("[GL_ERROR v1] pass=%s suppressed frames=%u count_in_window=%u\n",
                    st.name,
                    (unsigned)(g_mc2FrameCounter - st.lastPrintFrame),
                    (unsigned)st.suppressedCount);
                fflush(stdout);
            }
            st.inSuppression   = false;
            st.suppressedCount = 0;

            printf("[GL_ERROR v1] frame=%u pass=%s code=%s(0x%04X) mono_count=%llu\n",
                g_mc2FrameCounter, st.name, glErrorName(err), (unsigned)err,
                (unsigned long long)st.monoCount);
            fflush(stdout);
            st.lastPrintFrame = g_mc2FrameCounter;
        } else if (!s_glErrorDrainSilent && errorsThisFrame == 1) {
            // First error this frame, but still in suppression window.
            st.inSuppression = true;
            st.suppressedCount++;
        }
    }
}
```

**Implementation notes:**
- `g_mc2FrameCounter` is defined once in `GameOS/gameos/gameosmain.cpp` (Task 5 step 0); this TU uses `extern`.
- If `GL_CONTEXT_LOST` isn't defined (pre-GL 4.5 headers), the `#ifdef` guard keeps compile clean.
- `unknown pass name` silent-drop is deliberate: a misspelled pass name at a call site should not crash the renderer.

### Task 22: Insert pass-boundary drain calls at 7 sites

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` — `shadow_static`, `shadow_dynamic`, `terrain`, `objects_3d`
- Modify: `mclib/txmmgr.cpp` — alternate site for `objects_3d` if the render-list flush is here rather than in `gameos_graphics.cpp`
- Modify: `GameOS/gameos/gos_postprocess.cpp` — `post_process`
- Modify: the frame-end file from Task 5 — `hud`, `frame`

- [ ] **Step 1: Identify the end of each render pass.**

```bash
grep -n 'renderShadows\|DrawShadowStatic\|DrawShadowDynamic\|drawTerrain\|renderTerrain\|renderLists\|renderObjects\|postProcess\|drawHUD\|renderHUD' GameOS/gameos/gameos_graphics.cpp GameOS/gameos/gos_postprocess.cpp mclib/txmmgr.cpp
```

Implementer reads each candidate function, confirms the logical "end of pass," and inserts the drain call as the last statement before the function returns.

- [ ] **Step 2: Add the include to each modified file.**

```cpp
#include "gos_validate.h"
```

(if not already present; check first).

- [ ] **Step 3: Insert drain calls.**

Example — end of a render-pass function:

```cpp
void renderShadowsStatic(...) {
    // ... existing body ...

    drainGLErrors("shadow_static");  // <- add before return
}
```

Apply equivalent calls for:
- `drainGLErrors("shadow_dynamic")` at the end of dynamic mech shadow render.
- `drainGLErrors("terrain")` after terrain draw (after overlay split).
- `drainGLErrors("objects_3d")` after `renderLists` flush or equivalent.
- `drainGLErrors("post_process")` after bloom/FXAA/screen-shadow composite.
- `drainGLErrors("hud")` after HUD/overlay render.
- `drainGLErrors("frame")` as the very last call before `SwapBuffers` / equivalent, in the same frame-end file as `drainTglPoolStats`.

- [ ] **Step 4: Verify all 7 call sites exist.**

```bash
grep -rn 'drainGLErrors(' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h' | grep -v 'gos_validate'
```

Expected: exactly 7 hits, one per pass name.

### Task 23: Startup banner at process/logger init

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` — `main()` starts at line 463 per plan-writing grep. Insert the banner as the **first printf** inside `main()`, before any other engine init.

- [ ] **Step 1: Open `GameOS/gameos/gameosmain.cpp` and find `main()`.**

```bash
grep -n 'int main' GameOS/gameos/gameosmain.cpp
```

Expected: one hit at or near line 463.

- [ ] **Step 2: Insert the banner as the first statement inside `main()`.**

```cpp
// Tier-1 instrumentation: one-line banner so every log file is
// self-describing about which traces are enabled.
{
    const bool tgl     = (getenv("MC2_TGL_POOL_TRACE")       != nullptr);
    const bool destr   = (getenv("MC2_DESTROY_TRACE")        != nullptr);
    // GL_ERROR is default-on; the env var suppresses it.
    const bool glprint = (getenv("MC2_GL_ERROR_DRAIN_SILENT") == nullptr);
    const char* build  =
#ifdef MC2_BUILD_HASH
        MC2_BUILD_HASH
#else
        "UNKNOWN"
#endif
        ;
    printf("[INSTR v1] enabled: tgl_pool=%d destroy=%d gl_error_print=%d build=%s\n",
        tgl ? 1 : 0, destr ? 1 : 0, glprint ? 1 : 0, build);
    fflush(stdout);
}
```

### Task 24: `scripts/check-destroy-invariant.sh`

**Files:**
- Create: `scripts/check-destroy-invariant.sh`

- [ ] **Step 1: Write the script.**

```bash
#!/bin/sh
# scripts/check-destroy-invariant.sh
# Enforces: all setExists(false) call sites outside GameObject::destroy
# are violations (stability spec §3.8).
#
# Portability notes:
#  - Uses `grep -E` (POSIX extended regex) with `[[:space:]]*` instead of
#    `\s`. Plain `grep` / BRE does NOT interpret `\s` as whitespace.
#  - Tested against GNU grep (MSYS2/Git-Bash on Windows) and POSIX grep.

set -e
violations=0

# Literal: setExists(false) must only appear inside GameObject::destroy
# (code/gameobj.cpp).
lits=$(grep -rEn 'setExists[[:space:]]*\([[:space:]]*false' code/ mclib/ GameOS/ \
    --include='*.cpp' --include='*.h' \
    | grep -v 'code/gameobj.cpp' || true)
if [ -n "$lits" ]; then
    echo "[INVARIANT] literal setExists(false) outside GameObject::destroy:"
    echo "$lits"
    violations=1
fi

# Non-literal: setExists(<expr>) where expr is not true/false literal —
# flag for manual review. Does not fail the check by itself.
nonlit=$(grep -rEn 'setExists[[:space:]]*\(' code/ mclib/ GameOS/ \
    --include='*.cpp' --include='*.h' \
    | grep -Ev 'setExists[[:space:]]*\([[:space:]]*(true|false)' \
    | grep -v 'code/gameobj.cpp' || true)
if [ -n "$nonlit" ]; then
    echo "[INVARIANT] non-literal setExists(<expr>) — manual review required:"
    echo "$nonlit"
fi

if [ $violations -eq 0 ]; then
    echo "OK"
fi
exit $violations
```

**Test the script before committing** (new sub-step below).

- [ ] **Step 2: Make executable + test with a synthetic violation.**

```bash
chmod +x scripts/check-destroy-invariant.sh
```

Quick self-test (to confirm the regex actually catches violations):

```bash
# Create a synthetic violation in a scratch location, confirm script flags it.
echo 'obj->setExists( false );' > /tmp/bogus.cpp  # MSYS2: use C:/temp/bogus.cpp
# Run the script against a temp tree that includes the file.
# Expected: exit 1, violation printed.
# Clean up afterwards.
```

If the script does NOT flag the synthetic violation, the regex is broken — fix before committing.

- [ ] **Step 3: Run against the current tree.**

```bash
sh scripts/check-destroy-invariant.sh
```

Expected output:
```
[INVARIANT] non-literal setExists(<expr>) — manual review required:
<4 non-literal sites from Task 17, listed>
OK
```

Exit code 0 (non-literal flags don't fail the check). If exit 1: a literal site was missed in Task 15/16 — go back and convert it.

### Task 25: CLAUDE.md updates

**Files:**
- Modify: `CLAUDE.md` (worktree root — `A:/Games/mc2-opengl-src/.claude/worktrees/stability-tier1/CLAUDE.md`)

- [ ] **Step 1: Append a new section after "Debug Instrumentation Rule".**

```markdown
## Tier-1 Instrumentation Env Vars

Three env-gated loggers, one always-on summary, one checked-in invariant script.

- `MC2_TGL_POOL_TRACE=1` — per-frame `[TGL_POOL v1]` print when any pool returns NULL. Default off; the monotonic `[TGL_POOL v1] summary` line emits every 600 frames + on shutdown regardless.
- `MC2_DESTROY_TRACE=1` — per-destruction `[DESTROY v1]` line with cull/lifecycle snapshot. Default off.
- `MC2_GL_ERROR_DRAIN_SILENT=1` — suppresses `[GL_ERROR v1]` first-error prints. **Default is PRINT-ON** — a fresh operator sees GL errors with no setup. Drain loop always runs; only the print is gated.

Startup banner `[INSTR v1] enabled: ...` appears at the very start of every log file. If it's missing, instrumentation wasn't wired up (or was wired too late).

Schema-version grep pattern: `\[SUBSYS v[0-9]+\]`. Future format changes bump the version; no backward-compat shims.

Before any commit that touches object lifecycle:
```bash
sh scripts/check-destroy-invariant.sh
```

Exit 0 = no literal `setExists(false)` outside `GameObject::destroy`. Non-literal sites are flagged for manual review; the script does not fail on them.
```

### Task 26: Build, deploy, verify GL errors captured

- [ ] **Step 1: Build.**

Run `/mc2-build`. Expected: clean compile. Most likely failure is `g_mc2FrameCounter` visibility — resolve by exposing it (see Task 21 note).

- [ ] **Step 2: Deploy + launch with default env (drain default-on).**

```bash
mc2.exe
```

Load `mis0101`. Play 30 seconds. Quit. Grep:

```bash
grep '\[GL_ERROR v1\]' mc2.log
grep '\[INSTR v1\]' mc2.log
```

Expected:
- `[INSTR v1]` line at the top of the log.
- `[GL_ERROR v1]` lines **may** appear — this is exactly what the instrumentation is for. If errors appear, that's **signal**, not a regression. Record the output in the commit message; it's the input to the follow-up spec that fixes the Release `GL_INVALID_ENUM` crash.
- If suppression fires, `[GL_ERROR v1] pass=<X> suppressed frames=30 count_in_window=N` lines appear after the first burst.

- [ ] **Step 3: Deploy + launch with silent flag set.**

```bash
SET MC2_GL_ERROR_DRAIN_SILENT=1
mc2.exe
```

Grep again. Expected:
- `[INSTR v1]` line with `gl_error_print=0`.
- Zero `[GL_ERROR v1]` lines.
- The drain loop still runs (counters still accumulate), but nothing is printed.

### Task 27: Post-change perf capture + comparison

**Files:** none (measurement + doc update).

- [ ] **Step 1: Capture post-change perf baseline.**

Close `mc2.exe`. Launch with all three gates **OFF** (unset `MC2_TGL_POOL_TRACE`, unset `MC2_DESTROY_TRACE`, set `MC2_GL_ERROR_DRAIN_SILENT=1`). Load `mis0101`, camera at spawn, 30-second Tracy capture.

Save trace to `docs/superpowers/plans/baseline-mis0101-post.tracy`.

- [ ] **Step 2: Compute deltas.**

In `docs/superpowers/plans/baseline-mis0101-post.md`, record median / P99 / P99.9 frame time. Compute deltas vs. `baseline-mis0101-pre.md` from Task 0.

- [ ] **Step 3: Check thresholds.**

- Median within 2%? If yes, pass.
- P99 within 5%?
- P99.9 within 10%?

If any threshold is missed: investigate. Most likely culprit is an unintended hot-path print or a missing env gate. Do not proceed to commit until deltas are within spec.

- [ ] **Step 4: Commit the perf record.**

```bash
git add docs/superpowers/plans/baseline-mis0101-post.md docs/superpowers/plans/baseline-mis0101-post.tracy 2>/dev/null
git commit -m "perf: capture mis0101 baseline post-instrumentation

Deltas vs pre (Task 0):
  median:  <X>% (target within 2%)
  P99:     <X>% (target within 5%)
  P99.9:   <X>% (target within 10%)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

### Task 28: Commit 4

```bash
git add GameOS/gameos/gos_validate.h GameOS/gameos/gos_validate.cpp GameOS/gameos/gameos_graphics.cpp mclib/txmmgr.cpp GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gameos.cpp scripts/check-destroy-invariant.sh CLAUDE.md
git commit -m "$(cat <<'EOF'
feat(instr): GL error drain at 7 pass boundaries + startup banner

Adds drainGLErrors(pass) with 30-frame suppression, inserted at
shadow_static, shadow_dynamic, terrain, objects_3d, post_process, hud,
and frame pass boundaries. Default-on print (MC2_GL_ERROR_DRAIN_SILENT
suppresses); drain loop always runs. Startup [INSTR v1] banner at
process init. scripts/check-destroy-invariant.sh enforces the
setExists(false) invariant. CLAUDE.md documents env vars + schema
grep pattern.

Part 4 of 4 for stability tier-1 instrumentation. See
docs/superpowers/specs/2026-04-23-stability-tier1-instrumentation-design.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Final Verification

### Task 29: Full spec-success-criteria audit

- [ ] **Step 1: Invariant check.**

```bash
sh scripts/check-destroy-invariant.sh
```

Expected: only non-literal flags, exit 0.

- [ ] **Step 2: Full grep sweep for stale `setExists(false)`.**

```bash
grep -rn 'setExists\s*(' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h' | grep -v '(\s*true' | grep -v 'code/gameobj.cpp'
```

Expected: only the 4 documented non-literal sites from Task 17.

- [ ] **Step 3: Log-size sanity with all gates ON.**

```bash
SET MC2_TGL_POOL_TRACE=1
SET MC2_DESTROY_TRACE=1
# (leave MC2_GL_ERROR_DRAIN_SILENT unset — default-on print)
mc2.exe > mc2-full-trace.log 2>&1
```

Load `mis0101`, play 30s, quit. Check log size:

```bash
ls -la mc2-full-trace.log
```

Expected: under 10MB. If larger: a pass is firing a lot of GL errors — that is signal, file it as a discovered bug, do not suppress the instrumentation.

- [ ] **Step 4: Banner sanity.**

```bash
head -5 mc2-full-trace.log
```

Expected: `[INSTR v1] enabled: tgl_pool=1 destroy=1 gl_error_print=1 build=<hash-or-UNKNOWN>` visible.

- [ ] **Step 5: Grep-friendly format check.**

```bash
# Every instrumentation line must start with [NAME vN].
grep -cE '^\[(TGL_POOL|DESTROY|GL_ERROR|INSTR) v[0-9]+\]' mc2-full-trace.log
# And no multi-line / wrapped trace entries slipped in — every line that
# starts with '[' must parse as a tagged line.
grep -E '^\[' mc2-full-trace.log | grep -vE '^\[(TGL_POOL|DESTROY|GL_ERROR|INSTR) v[0-9]+\]'
```

Expected: first command returns a non-zero count. Second command returns nothing (no unrecognized `[...]`-prefixed lines).

- [ ] **Step 6: Perf criteria met.**

Re-read `docs/superpowers/plans/baseline-mis0101-post.md`. Confirm all three thresholds pass.

- [ ] **Step 7: Mark spec status.**

Update the spec's status line:

```markdown
# Stability Tier 1 — Silent-Failure Instrumentation

**Date:** 2026-04-23
**Status:** Implemented. See commits on `claude/stability-tier1`:
  - <hash> — TGL pool trace
  - <hash> — destroy wrapper
  - <hash> — 34 conversions + taxonomy
  - <hash> — GL drain + banner + script
```

Commit:

```bash
git add docs/superpowers/specs/2026-04-23-stability-tier1-instrumentation-design.md
git commit -m "docs(spec): mark stability tier 1 instrumentation as implemented

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Rollback Plan

Each of the 4 feature commits is bisectable standalone. If a regression shows up mid-session:

1. `git bisect start claude/stability-tier1 <baseline-pre-commit>`
2. `git bisect run /mc2-build-deploy && <smoke-test-script>`
3. The offending commit identifies which of: pool trace / destroy wrapper / conversions / GL drain broke things.

If the break is in **commit 3 (conversions)**, the wrapper itself from **commit 2** is proven working — the bug is a specific site's reason string or a missed edge case. Grep the reasons list, correlate with the failing mission state.

If the break is in **commit 4 (GL drain)**, the pass-boundary call might be in the wrong place, or `glGetError()` ownership is clobbering something downstream. Re-audit the 5 existing `glGetError` callers (spec §4 audit table).

---

## Key Handoff Notes

- **Shared frame counter — decided.** Canonical location is `GameOS/gameos/gameosmain.cpp`: `uint32_t g_mc2FrameCounter = 0;` at file scope. Incremented once per frame immediately before `drainTglPoolStats()` in the frame-end path. Every other TU that needs it (`mclib/tgl.cpp`, `code/gameobj.cpp`, `GameOS/gameos/gos_validate.cpp`) uses `extern uint32_t g_mc2FrameCounter;`. Locked in Task 5 step 0 — no later task re-invents this.
- **Pool accessors — verify at Task 5 step 1.** `drainTglPoolStats()` uses `extern TG_VertexPool* vertexPool` etc. If MC2 actually owns the pools as `Mission*` members (read `code/mission.cpp` near line 3097), change the `extern` lines to `mission->vertexPool` and accept a `Mission*` parameter. A wrong pool-access pattern breaks at link time, not silently.
- **Accessor strategy — decided.** `inView_instr()`, `canBeSeen_instr()`, `blockActive_instr()` — three virtual getters added to `GameObject` in Task 9 with safe defaults. Subclass overrides in `code/actor.h` and `code/artlry.h`. No mid-implementation interface decision remains.
- **update() callers — grep-driven.** Task 10 depends on grepping `->update()` across `code/`. Expected 3-10 sites. Task 10's explicit scope callout documents this as the largest edit-surface in the plan.
- **Never push to origin.** Per `CLAUDE.md`: all work is local. `claude/stability-tier1` stays on the local repo.

---

## Self-Review Summary

**Spec coverage:** every section of the spec maps to tasks:
- §1 scope — Task 0 baseline + Task 27 post-measurement + Task 29 final audit.
- §2 TGL — Tasks 1-6 + commit 7.
- §3 destroy — Tasks 8-13 + commit 14 + Tasks 15-18 + commit 20.
- §4 GL drain — Tasks 21-22 + commit 28.
- §5 cross-cutting (banner, thread-safety, schema) — Tasks 23, 25; thread-safety is assumption-only, doc'd in spec and CLAUDE.md.
- §5.5 CLAUDE.md updates — Task 25.
- Invariant script — Task 24.

**Placeholder scan:** no "TBD"/"TODO"/"fill in later." Where the implementer must make a local decision (pool accessor path, subclass accessor style, frame-end file location), the plan flags it explicitly with a `grep` command to find the answer and a decision rubric — not a placeholder.

**Type consistency:** `lastUpdateRet` is `int32_t` throughout (plan corrects the spec's `int8_t` given `update()` returns `long`). `framesSinceActive` is `uint8_t` throughout. `MC2_DESTROY` macro signature is `(obj, reason)` across all tasks. `drainGLErrors(const char*)` signature consistent across Tasks 21-22.
