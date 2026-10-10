<!-- doc-status: current | reviewed:2026-10-11 -->
# FRAME-SKIP-SAFETY-AUDIT-1

## Executive Summary

**The Rule:** Per-frame skip fast-paths (continue, return, early-exit branches) that avoid expensive work MUST preserve the cheap liveness stamps that downstream consumers require. Example: the R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 fix (commit 07a1f8ac) added `mc2R2bTouchStaticLiveness()` stamping inside the skip to prevent stale `cachedFrame_` — the registry flush was dropping trees because their shapes' frame stamps weren't refreshed when update() was skipped.

**Scope:** This audit inventories every per-frame skip site in the update/touch/render/submit paths and classifies each by whether it preserves required liveness and frame contracts.

---

## Skip Sites Classification

Sorted by file, with notation: **file:line | owner | gate var | skipped work | skipped stamp | consumer | STATUS**.

### code/objmgr.cpp

| Line | Owner | Gate Var | Skipped Work | Skipped Stamp | Consumer | Status |
|------|-------|----------|--------------|---------------|----------|--------|
| 2229 | GOM.framesSinceActive sweep | — | skip loop iteration if objList[i]==NULL | (none) | framesSinceActive accumulator | SAFE |
| 2328 | GOM.terrainBlock loop | — | skip block if !objBlockInfo[].active | (none) | terrain block processing | SAFE |
| 2333 | terrainObj inner loop | — | skip obj if !objList[] exists | (none) | object processing | SAFE |
| 2334 | terrainObj inner loop | — | skip obj if !objVertexActive[] | (none) | vertex culling | SAFE |
| 2352 | FRAME-JOBS-1 prepass | — | skip obj if !objList[] or !appearance | (none) | bounds prepass | SAFE |
| 2354 | FRAME-JOBS-1 prepass | — | skip appearance if !ap | (none) | bounds computation | SAFE |
| 2360 | FRAME-JOBS-1 prepass | — | skip obj if !isRecalcBoundsWorkerSafe() | (none) | worker pool — non-whitelisted types deferred | SAFE_WITH_STAMP |
| 2409 | FRAME-JOBS-2C touch prepass | MC2_FRAME_JOBS_TOUCH | skip obj if !isTouchSplitSafe() | (none) | split phase touch — non-qualifying types use legacy path | SAFE |
| 2411 | FRAME-JOBS-2C touch prepass | MC2_FRAME_JOBS_TOUCH | skip appearance if !ap || !isTouchSplitSafe() | (none) | split phase touch | SAFE |
| 2753 | **R2B-STATIC-NATURAL-TOUCH-PRESERVE-1** | MC2_SKIP_STATIC_TREES (default-ON) | **skip update() for static trees at turn>=3** | **cachedFrame_ stamp (FIXED: stamped via mc2R2bTouchStaticLiveness if MC2_R2B_TOUCH_PRESERVE=1, default-ON)** | GpuStaticPropRegistry::flush() drops stale multi-shapes | **SAFE_WITH_STAMP** — the stamping fn (lines 222–241) preserves the critical shape liveness; commit 07a1f8ac + gate=1 default-ON |
| 2788 | R2B-STATIC-NATURAL (buildings) | MC2_SKIP_STATIC_TREES (default-ON) | **skip update() for static Pine buildings at turn>=3, staticNow=true** | **cachedFrame_ stamp (FIXED: same mc2R2bTouchStaticLiveness stamp path as trees, lines 784–786)** | registry flush | **SAFE_WITH_STAMP** — same fix as tree path; power-supply propagation still runs inline (lines 2774–2779) |
| 3272 | GOM terrain loop | — | skip if frame is "skip this building" boundary | (none) | static-buildings skip predicate | SAFE |
| 3283 | GOM terrain loop | — | skip iteration | (none) | building enumeration | SAFE |
| 3830 | GOM terrain loop | — | skip branch in campaign-logic | (none) | campaign state | SAFE |
| 4360 | mover update loop | — | skip dead movers | (none) | mover processing | SAFE |

### mclib/bdactor.cpp (BldgAppearance)

| Line | Owner | Gate Var | Skipped Work | Skipped Stamp | Consumer | Status |
|------|-------|----------|--------------|---------------|----------|--------|
| 4259 | BldgAppearance::touchSerialCommit | — | early return if !bldgShape | (none) | shape touchSerialCommit | SAFE |
| 4440 | **BldgAppearance::touchSerialCommit (LIGHTBRIDGE-STABLE-SKIP-WIRE-1)** | MC2_LIGHTBRIDGE_STABLE_SKIP (default-OFF) | **skip EmitBakedGpuLightData + getCachedGpuLightIndex + staticReg writes if stableLightSkipEligible** | **cachedGpuLightIndex_ (NOT skipped — slot is permanent per INVARIANT Q1–Q4: baked slots [0..s_staticLightHighWater) persist frame-to-frame; prior frame's repoint index stays valid)** | BldgAppearance::touch() Path B (legacy) reads the same slot; renderer consumes cachedGpuLightIndex_ | **SAFE_WITH_STAMP** — the slot index itself doesn't need refresh (baked static prefix is immutable); BUT the `touchSerialCommitFrame` stamp (line 4246) is set BEFORE the skip so Path B touch() (line 6525–6526) early-returns correctly; double-touch prevented |
| 6524 | BldgAppearance::touch (Path B, terrain loop) | — | early return if touchSerialCommitFrame==g_mc2FrameCounter | (none) | legacy shape touch prepass | SAFE_WITH_STAMP — stamp is set in touchSerialCommit (4246) BEFORE stable-skip, so Path B correctly skips when Phase 2 already ran |

### mclib/bdactor.cpp (TreeAppearance)

| Line | Owner | Gate Var | Skipped Work | Skipped Stamp | Consumer | Status |
|------|-------|----------|--------------|---------------|----------|--------|
| 6521 | TreeAppearance::touch (Path B, terrain loop) | MC2_FRAME_JOBS_TOUCH_DIAG (diag-only) | early return if touchSerialCommitFrame==g_mc2FrameCounter | (none) | legacy tree touch | SAFE_WITH_STAMP — mirrors BldgAppearance; stamp set in touchSerialCommit (6573) |
| 6526 | TreeAppearance::touch (Path B) | — | early return if touchSerialCommitFrame==g_mc2FrameCounter | (none) | legacy tree touch prepass | SAFE_WITH_STAMP — same stamp contract as BldgAppearance |
| 6586 | TreeAppearance::touchSerialCommit | — | early return if !treeShape | (none) | shape touchSerialCommit | SAFE |
| 6633 | **TreeAppearance::touchSerialCommit (LIGHTBRIDGE-STABLE-SKIP-WIRE-1)** | MC2_LIGHTBRIDGE_STABLE_SKIP (default-OFF) | **skip EmitBakedGpuLightData if (registered && indexValid && !forceFullCommit)** | **staticReg[activeLOD].lightDataIndex (NOT skipped — same INVARIANT as BldgAppearance: baked slots are permanent)** | TreeAppearance::touch() Path B; renderer | **SAFE_WITH_STAMP** — `touchSerialCommitFrame` stamp (line 6573) set unconditionally BEFORE the skip; Path B touch() (line 6525) early-returns when stamp matches |

### GameOS/gameos/gos_static_prop_registry.cpp

| Line | Owner | Gate Var | Skipped Work | Skipped Stamp | Consumer | Status |
|------|-------|----------|--------------|---------------|----------|--------|
| 408 | registryFlush (RecipeRange loop) | — | skip if pinsReleased (tombstone guard) | (none) | static draw emission | SAFE |
| 582, 669, 719 | registry API returns | — | skip if !s_enabled | (none) | gating functions | SAFE |
| 670 | registry accessors | — | skip if index out of bounds | (none) | range access | SAFE |
| 672 | registryFlush loop | — | skip if rng.count==0 (tombstone) | (none) | recipe emission | SAFE |
| 826, 849 | buildStaticInstanceRanges prepass | — | skip if recipe index OOB or slot >= S | (none) | instance table build | SAFE |
| 922 | registryFlush loop | — | skip if arng.count==0 OR !arng.multi (tombstone guard) | (none) | range iteration | SAFE |
| 976 | **registryFlush main loop (R2B-STATIC-NATURAL regression guard)** | — | **skip (continue) if rng.count==0 OR !rng.multi (tombstone guard)** | **cachedFrame_ (NOT stamped at skip; but note: a TRUE stale frame here is a BUG — it means a registered prop's multi-shape cachedFrame_ fell behind currentFrame, indicating update/touch skipped the stamp. This skip itself is SAFE; the BUG is upstream in the update/touch logic that should have stamped the frame.)** | static draw list | **SAFE (but detector of upstream bugs)** — lines 987–1036 classify the cachedFrame_ mismatch and surface it as `s_diag_ranges_stale_after_drawn` + `[STATIC_PROP_REGISTRY]...stale_frame_drop_after_drawn` stderr; gate MC2_STATIC_STALE_DROP_FATAL for CI |
| 1036 | registryFlush loop | — | **skip (continue) if rng.multi->getCachedFrame()!=currentFrame** | cachedFrame_ (NOT refreshed — the skip detects the staleness; underlying bug is in update/touch) | static draw list | **NEEDS_TRACE** — this is the key consumer-boundary counter: `s_diag_ranges_stale_after_drawn` counts registered props whose frame stamp drifted. Rising/persistent counts for on-screen typeIDs = upstream stamping bug (the R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 class). Diagnostic: `MC2_STATIC_STALE_DROP_FATAL` aborts on regression. |
| 1778, 1779, 1781, 1801, 1802, 1803, 1805 | shadow-draw recipe loop | — | skip if tombstone, wrong population, OOB | (none) | shadow draw submission | SAFE |

### code/mission.cpp (frame loop entry point)

| Line | Owner | Gate Var | Skipped Work | Skipped Stamp | Consumer | Status |
|------|-------|----------|--------------|---------------|----------|--------|
| (inferred from MEMORY.md) | substrate_frameBegin() gate | — | moved to Mission::update() before the updateAppearancesOnly/update branch so it fires every frame including pause | (none) | GPU substrate ring-slot state | **NEEDS_TRACE** (behavioral, not a skip site) — ensures GPU compute cull buffer is reset every frame, preventing pause-smear bug |

---

## NEEDS_TRACE Shortlist

Ranked by consumer-boundary impact and confidence that instrumentation will catch real regressions:

### 1. **GpuStaticPropRegistry::flush() — stale_after_drawn counter** (Priority: CRITICAL)
   - **Counter Name:** `s_diag_ranges_stale_after_drawn` (already exists, gos_static_prop_registry.cpp:1012)
   - **Consumer Boundary:** registry flush loop (line 987–1036) vs. update/touch stamping paths (code/objmgr_update.cpp:746, mclib/bdactor_update.cpp:820, 6573)
   - **Why Instrument:** This counter directly detects the R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 regression class. If a skip path fails to stamp cachedFrame_, the registry drop happens silently unless this counter is monitored. **Rising or persistent per-frame counts for on-screen typeIDs = upstream stamping bug.**
   - **Deployment:** Wire into smoke CI: alert if `stale_after_drawn` > 0 for any on-screen mission (mc2_01, mc2_10, mc2_24). Current status: implemented (stderr emit + `MC2_STATIC_STALE_DROP_FATAL` gate for CI).
   - **Related Env Vars:** `MC2_STATIC_STALE_DROP_FATAL` (abort on regression), `MC2_SKIP_STATIC_TREES_DIAG` (legacy per-type summary), `MC2_R2B_STATIC_NATURAL_TRACE` (R2B skip path trace)

### 2. **BldgAppearance::touchSerialCommit() — double-touch prevention** (Priority: HIGH)
   - **Counter Name:** `s_touchEntryDiag.legacy_skipped` (already exists, bdactor.cpp:2879–2896)
   - **Consumer Boundary:** touchSerialCommit() (line 4246 stamp) vs. Path B touch() (line 6524–6526 stamp-check early-return)
   - **Why Instrument:** Verifies that FRAME-JOBS-2F stamp-unification (Phase 2 marks frame, Path B checks stamp) is working. Counter should be ~100% of Path B touch() calls on missions running FRAME_JOBS_TOUCH (split enabled). If counter stays 0, the stamp logic is broken and Path B will double-run work.
   - **Deployment:** Wire into smoke CI: assert `legacy_skipped / total_Path_B_calls > 0.95` when `MC2_FRAME_JOBS_TOUCH=1`. Currently printed every 300 frames when `MC2_FRAME_JOBS_TOUCH_DIAG=1`.
   - **Related Env Vars:** `MC2_FRAME_JOBS_TOUCH` (gate), `MC2_FRAME_JOBS_TOUCH_DIAG` (diagnostic)

### 3. **BldgAppearance::touchSerialCommit() — stableLightSkip eligibility** (Priority: MEDIUM)
   - **Counter Name:** `s_stableLightSkipDiag.skip` (already exists, bdactor.cpp:2745–2766, printed at `s_lbssDiag`)
   - **Consumer Boundary:** touchSerialCommit() stableLightSkipEligible check (line 4263–4269) vs. ResubmitCachedGpuLightData() call (line 4452)
   - **Why Instrument:** Tracks how often the stable-skip fires (eligible && armed). Should be high on stable missions (mc2_10 ~99% at steady state). If it drops unexpectedly (needsFullBakeNextFrame spike, registration loss), the building will re-submit every frame instead of using cached slots, visible as perf regression.
   - **Deployment:** Wire into smoke perf regression gates: if `skip_rate < baseline_skip_rate * 0.8` (where baseline = mission average), flag for investigation. Printed every 300 frames when `MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1`.
   - **Related Env Vars:** `MC2_LIGHTBRIDGE_STABLE_SKIP` (enable), `MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG` (diagnostic)

### 4. **GpuStaticPropRegistry::flush() — proxy candidate classification** (Priority: MEDIUM-LOW)
   - **Counter Name:** `g_spr_proxyCandidate`, `g_spr_rejNoStaticReg`, etc. (already exist, mclib/bdactor_internal.h:44–62)
   - **Consumer Boundary:** touchSerialCommit() stableLightSkipEligible evaluation vs. STATIC-SCENE-PROXY-RECON classification (bdactor.cpp:4287–4409, 6596–6620)
   - **Why Instrument:** RECON-only (no behavior change). Identifies buildings/trees stable enough to skip touch() entirely. Rejection breakdown (no_shape, no_static_reg, bad_light_idx, needs_full_bake) reveals structural issues (e.g., mc2_24 72% rej_no_static_reg = off-screen buildings never registered). Baseline mission profiles guide future STATIC-SCENE-PROXY optimization slices.
   - **Deployment:** Archive per-mission baseline profiles (recon=1 soak runs). Compare new baseline after changes to detect registration/stability regressions early.
   - **Related Env Vars:** `MC2_STATIC_PROXY_RECON` (enable), `MC2_STATIC_REG_COVERAGE` (sub-classify rejections)

### 5. **code/objmgr.cpp — FRAME-JOBS-1 prepass skip rate** (Priority: LOW)
   - **Counter Name:** `frameJobsPrePassCount` (already instrumented, line 2340, printed per-frame when `MC2_FRAME_JOBS=1` and trace enabled)
   - **Consumer Boundary:** parallelForRange() (line 2346) candidate filter vs. isRecalcBoundsWorkerSafe() gate (line 2355)
   - **Why Instrument:** Tracks how many objects are whitelisted for parallel recalcBounds. Should be stable per mission. If it drops (appearance type disqualified), prepass perf gains diminish. Diagnostic only; low priority.
   - **Deployment:** Smoke trace (`MC2_FRAME_JOBS=1 + MC2_FRAME_JOBS_TRACE=1`) captures this. Manual review for appearance-type changes.
   - **Related Env Vars:** `MC2_FRAME_JOBS`, `MC2_FRAME_JOBS_BATCH`, `MC2_FRAME_JOBS_TRACE`

---

## Key Findings

1. **R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 is the historical regression class:** The skip at code/objmgr_update.cpp:749 (continue for static trees) and 2788 (continue for static buildings) now preserve cachedFrame_ via mc2R2bTouchStaticLiveness() when MC2_R2B_TOUCH_PRESERVE=1 (default-ON). Regression signature: GpuStaticPropRegistry::flush() detects stale multi-shapes at line 987 and emits `[STATIC_PROP_REGISTRY]...stale_frame_drop_after_drawn`. The counter `s_diag_ranges_stale_after_drawn` is the regression detector.

2. **FRAME-JOBS-2F split-phase introduces new stamp contract:** touchSerialCommit() sets touchSerialCommitFrame unconditionally (mclib/bdactor_update.cpp:820, 6573) BEFORE the stable-skip return. This allows Path B touch() to stamp-check and early-return correctly, preventing double-work. The contract is verified by `s_touchEntryDiag.legacy_skipped` counter (printed when `MC2_FRAME_JOBS_TOUCH_DIAG=1`).

3. **LIGHTBRIDGE-STABLE-SKIP-WIRE-1 preserves slot liveness via INVARIANT:** Baked static-prop light slots [0..s_staticLightHighWater) persist frame-to-frame; skipping EmitBakedGpuLightData doesn't stale them because the GPU buffer upload keeps them valid (renderLists() always covers the range). The cached slot index from prior frame remains correct.

4. **Most skip sites are trivial gate-checks (NULL, bounds, conditions):** Only the following are load-bearing per-frame skips that warrant regression tests:
   - code/objmgr_update.cpp:749, 2788 (R2B-STATIC-NATURAL) — instrumented by `s_diag_ranges_stale_after_drawn`
   - bdactor.cpp:4440, 6633 (LIGHTBRIDGE-STABLE-SKIP) — instrumented by `s_stableLightSkipDiag.skip`
   - bdactor.cpp:6521, 6526 (touch Path B early-return) — instrumented by `s_touchEntryDiag.legacy_skipped`
   - gos_static_prop_registry.cpp:976 (cachedFrame stale check) — detector counter `s_diag_ranges_stale_after_drawn`

---

## Audit Metadata

- **Scan Date:** 2026-06-22
- **Worktree:** A:/Games/mc2-frame-currentness-guards (branch: claude/frame-currentness-guards-1)
- **Baseline:** commit 07a1f8ac (R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 fix) + verified default-ON gates
- **Coverage:** mclib/{bdactor.cpp, appear.h}, code/{objmgr.cpp, mission.cpp}, GameOS/gameos/gos_static_prop_registry.cpp, GameAdapters/, RenderWorld/
- **Classification Scheme:**
  - **SAFE:** skip affects no consumer; early-exit is correct gate or boundary check
  - **SAFE_WITH_STAMP:** skip preserves all required liveness stamps (frame, light index, bounds, etc.)
  - **BUG_FIXED:** skip was a bug, now guarded by fix commit (cite commit hash)
  - **NEEDS_TRACE:** cannot prove safe from code alone; instrumentation counter required (see NEEDS_TRACE section)
  - **DEAD:** unreachable or default-off and unused
