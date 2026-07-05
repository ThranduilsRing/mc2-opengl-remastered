# STATIC-SCENE-PROXY-SKIP-1 — Scope / Design Recon (2026-06-20)

**Status:** RECON ONLY. No implementation, no commits, no default flip.
**Branch:** `claude/nifty-mendeleev`. Recon ran against HEAD `80844f42` (worktree advanced from `39542ed5` mid-session — shared worktree, concurrent lane). File:line are current-HEAD; re-verify before editing.
**Goal:** Stop *entering/evaluating* Phase 2 `touchSerialCommit` for stable registered statics — the ~600µs/frame residual (FRAME-JOBS-2G) is the `stableLightSkipEligible` EVAL itself, run per object per frame even when it skips the bake. Remove the eval, not just the bake. No persistent proxy records.

---

## THE TARGET (confirmed)
- Phase 2 loop: `code/objmgr.cpp:2382-2387` — `for (long h : s_splitHandles) { ap->touchSerialCommit(); }`.
- Candidate set `s_splitHandles` (`objmgr.cpp:2345-2357`) rebuilt EVERY frame from `s_frameJobsHandles` (`:2271-2281`, walked from `Terrain::objBlockInfo` blocks), filtered by `isTouchSplitSafe()`. Gated `MC2_FRAME_JOBS_TOUCH`.
- `BldgAppearance::touchSerialCommit()` `mclib/bdactor.cpp:4240`; `TreeAppearance::touchSerialCommit()` `:6569`.
- Both stamp `touchSerialCommitFrame = g_mc2FrameCounter` UNCONDITIONALLY at entry (Path-B suppression, FRAME-JOBS-2F) THEN compute the expensive `stableLightSkipEligible` and return. The compute is the waste.

## CUT POINT VERDICT
**Internal early-return inside `touchSerialCommit()`, gated by a NEW sticky per-object cached-skip bit, placed BEFORE the `stableLightSkipEligible` computation** (before bldg `:4261`, before tree `:6594/6605`).

- **NOT** before-loop removal from `s_splitHandles`: that list rebuilds each frame from `objBlockInfo`, and removing entries would also skip `touchWorkerPrepass()` Phase 1 (`Touch()`/`selectActiveLOD()`, `objmgr.cpp:2364-2373`), which MUST still run.
- **MUST still stamp** `touchSerialCommitFrame = g_mc2FrameCounter` on the skip-return path (match `:4246`/`:6573`) or Path B (`objmgr.cpp` terrain loop) re-enters legacy touch.
- New field sits beside existing flags — bldg next to `touchSerialCommitFrame` (`bdactor.h:340`) / `staticReg` (`:261`); **tree must be PER-LOD** `bool[MAX_LODS]` beside `staticReg[MAX_LODS]` (`bdactor.h:638`) or cleared on `selectActiveLOD()` change (`activeLOD` varies per-frame).
- **No persistent proxy records** — the bit is pure derived state on the existing appearance object.

## WHY A STICKY CACHE IS SOUND TODAY (and the latent landmine)
`mc2StaticLightEnvironmentGeneration()` (`bdactor.cpp:2654`) is **hardcoded `return 0`**. So `lastLightEnvGen == currentLightEnvGen` is `0==0` always. **Eligibility has ZERO frame-varying inputs today** — once eligible, an object stays eligible until a discrete state-change event. That is exactly what makes "evaluate once, cache, clear on event" correct. **Trees don't track lightEnvGen at all.**
**LATENT LANDMINE:** if dynamic lighting / time-of-day / destruction-relight is ever added, the dead field produces no bump → every skipped static freezes its lighting. **SKIP-1 is valid ONLY under static mission lighting (MC2's current reality). Document this constraint loudly; do not silently depend on it.**

---

## RE-ADMISSION MATRIX (the correctness core)

A skipped static must re-enter Phase 2 on every state change. The two recon agents reconcile to this: **the clear-set is WIDER than the `invalidateStaticRegistration` sites**, because damage/destruction/LOD-swap do NOT call invalidate — they're detected only at render-time shape-compare (`staticReg.shape != bldgShape`, `:2158`/`:5907`), which SKIP bypasses.

| # | Event | Detect site | Flows through invalidate/latch? | Verdict |
|---|-------|-------------|-------------------------------|---------|
| 1 | Damage shape swap | `setObjStatus` `bdactor.cpp:1572` swap `:1587` (tree `5590/5605`); render detect `:2158` | NO invalidate, NO latch | **SILENT — must handle** |
| 2 | Destruction → rubble | `setObjStatus:1576` | NO | **SILENT — must handle** |
| 3 | Falling / collapse | objmgr gate `objmgr.cpp:2683/2703` tests `!isFalling` (`OBJECT_FLAG_FALLING` on game obj, not appearance) | nothing on appearance | **RE-ADMIT only if SKIP copies `!isFalling` test** |
| 4 | LOD swap | shape reassign `:4555/5094`; render detect `:2158`/`:5907` | latch set only AS CONSEQUENCE of render | **SILENT — must handle** |
| 5 | needsFullBakeNextFrame latch | set `:2177/2209/4052/5920/5945/6509/6892`; clear guarded on valid lightIdx `:4135` | drives IsStaticNow()=false; already in predicate `:4182` | RE-ADMIT (self-healing) |
| 6 | invalidateStaticRegistration | bodies `:4460`/`:6651`; sites `:1760,2099,2105,2126,2159,2178,4472,6651,6982` | tombstones, clears registered | RE-ADMIT via destroy + animation-start (`setGesture:1760`, render-independent) |
| 7 | Light-env change (ToD/dynamic) | `mc2StaticLightEnvironmentGeneration:2654` = **return 0** | dead field | **SILENT (latent) — document constraint** |
| 8 | Recipe-slot recycle | registry `gos_static_prop_registry.cpp:613` | no Appearance back-signal | RE-ADMIT only if (1)/(4) handled; else **corruption** (draws wrong prop) |
| 9 | Tree destruction / LOD | `setObjStatus:5594`; `staticReg[activeLOD]` | same as 1/2/4 + activeLOD-slot mismatch | **SILENT — must handle, per-LOD** |

### DANGER LIST — SKIP-1 MUST explicitly handle
1. **Damage/destruction (`setObjStatus` `1572`/`5590`)** — clear the cached-skip bit on `OBJECT_STATUS_DESTROYED/DISABLED` (and restore-to-NORMAL), OR make `setObjStatus` set `needsFullBakeNextFrame`. Cleanest: clear the bit inside `setObjStatus`.
2. **LOD swap (`4555/5094`, tree activeLOD)** — clear on LOD change, or exclude LOD-eligible types from skip.
3. **Falling** — replicate the existing `!isFalling && !isJustCreated` test (`objmgr.cpp:2683/2703`) at the skip site; it is NOT an appearance flag.
4. **Light-env (dead field)** — document static-lighting-only validity; do not key skip on a real-looking-but-constant generation.
5. **Recipe recycle compounding** — handling (1)+(4) closes it; leaving them open turns staleness into wrong-prop corruption (a skipped, un-invalidated object's `recipeIndex` can be recycled out from under it).

### SAFE / self-healing — NO handling needed
- `needsFullBakeNextFrame` latch (already in `stableLightSkipEligible` predicate `:4182`).
- Object destroy (lifecycle, always runs `:4472/6982`).
- Animation-start (`setGesture:1760` → invalidate, render-independent).

**PRECEDENT:** `MC2_SKIP_STATIC_BUILDINGS`/`MC2_SKIP_STATIC_TREES` (objmgr gate `:2683/2703`, default-off) already skips `update()` with re-admission contract `staticNow && !isFalling && !isJustCreated`. SKIP-1 inherits this exact contract and extends it to the touch-eval. NOTE: that skip is `update()`; touchSerialCommit is a SEPARATE path — SKIP-1 is genuinely additive (update-skip does not remove the 600µs touch eval).

---

## Answers to the 10 questions
1. Loop: `objmgr.cpp:2382-2387`. 2. List: `s_splitHandles` (rebuilt/frame from `objBlockInfo`, `:2271/2345`). 3. Cut: INTERNAL early-return (not before-loop; prepass must run). 4. Skip predicate = `stableLightSkipEligible` (`bdactor.cpp:4263-4269`): registered & recipeIndex≥0 & hasValidStaticLight & lightDataIndex valid & lastLightEnvGen==cur(0) & !needsFullBakeNextFrame; tree `:6605` (no envGen/hasValidStaticLight). 5. Re-admit events = DANGER LIST above. 6. Light gen = FAKE (constant 0). 7. needsFullBakeNextFrame cleared at `:4135` only when `lightDataIndex!=0xFFFFFFFFu` (guard correct). 8. Trees: per-LOD `staticReg[activeLOD]`, no envGen, per-LOD skip bit required. 9. YES — no persistent records; a 1-bit sticky cache + clears in existing+setObjStatus+LOD paths. 10. Diagnostics below.

## v1 DESIGN (no persistent records)
- Add sticky `bool stableSkipCached` (bldg) / `bool stableSkipCached[MAX_LODS]` (tree) beside `touchSerialCommitFrame`/`staticReg`.
- In `touchSerialCommit`: stamp frame; if `MC2_STATIC_PROXY_SKIP` && cached → return immediately (skip eval). Else run existing eval; if `stableLightSkipEligible` true → set cached, return.
- CLEAR sites: all `invalidateStaticRegistration` sites + all `needsFullBakeNextFrame` SET sites + `setObjStatus` (damage/destroy/restore) + LOD-swap sites + tree `selectActiveLOD` change.
- Skip site also tests `!isFalling && !isJustCreated` (mirror objmgr gate).

## DIAGNOSTICS (`[STATIC_PROXY_SKIP v1]`, gate `MC2_STATIC_PROXY_SKIP_TRACE`)
`phase2_candidates_before`, `skipped_stable_static`, `entered_phase2`, `rejected_dirty`, `rejected_needs_full_bake`, `rejected_not_registered`, `rejected_falling`, `rejected_light_gen_mismatch` (will be 0 — dead field; keep for future), `invalidated_damage`, `invalidated_destruction`, `invalidated_falling`, `invalidated_shape_swap`, `estimated_us_saved`. Hook beside `lbssRecordBldg`/`recordStableLightSkipDiag` (`:4270/4438`) and tree `lbssRecordTree` (`:6631`); per-modulo print mirrors `STATIC_PROXY_RECON` block (`:4342-4408`).
**Before/after metric:** `serial_commit_us` at `objmgr.cpp:2398` (gate `MC2_FRAME_JOBS_TRACE`). `MC2_STATIC_PROXY_RECON` already gives `proxy_candidate %` (the target population).

## ACCEPTANCE (later implementation)
- **Gate OFF (`MC2_STATIC_PROXY_SKIP` unset):** tier1 5/5; baseline counters unchanged; byte-identical behavior.
- **Gate ON:** mc2_24 Phase 2 candidates drop sharply (toward proxy_candidate %); `serial_commit_us` drops materially from ~600µs; Path B remains suppressed (double_touch_prevented stable); mc2_10 destruction test passes; destroyed/falling/damaged/LOD-swapped buildings re-enter or invalidate correctly (DANGER LIST verified — watch for stale rubble / frozen falling / wrong-prop after recycle); no stale light/index artifacts.

## HARD BOUNDARIES (from charter)
No building-shadow changes. No static-reg/prewarm changes. No render-registry changes. No persistent recipe-index proxy table. No default-on.

## OPEN DECISIONS for implementation phase
- Confirm the full set of `setObjStatus`/LOD-swap clear sites by instrumenting BEFORE wiring skip (add the `invalidated_*` counters first, observe damage/destroy/LOD fire on mc2_10, THEN add the skip return). Sequence: instrument → verify re-admit fires → enable skip.
- Tree per-LOD bit vs clear-on-activeLOD-change: pick the cheaper; per-LOD bit is safer.
