<!-- doc-status: stale | reviewed:2026-10-11 -->
# TREE-OVERRIDE-LOD-MVP-1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Give override tree render shapes a **per-LOD, pre-registered, pre-baked** LOD chain with distance selection, so a heavy override forest draws distance-appropriate geometry (killing the 2.29s `Render.GpuStaticProps` full-detail-at-all-distances cost) **without** resurrecting the black-tree regression class — and only if per-type light-slot cardinality is proven bounded.

**Architecture:** Promote the single `treeRenderShape` to `treeRenderShape[MAX_LODS]`; register one static recipe per populated LOD into the immutable batcher VBO **before `finalizeGeometry`**, each with its own permanent baked light slot; make `staticReg` per-LOD; select the active LOD per-instance by distance and replay the already-registered+baked recipe (lookup, never invalidate/re-register). LOD lives ONLY on render shapes — collision stays stock (dual-shape, Task 0 = CASE A). Auto-generate lower LODs by leaf-card-preserving decimation at import.

**Tech Stack:** C++14 engine (`mclib/bdactor.{h,cpp}`, `GameOS/gameos/gos_static_prop_batcher.*`), VS2022 cmake `RelWithDebInfo`, gltfpack/meshopt (vendored, `.claude/gltfpack_build`), `--validate` runtime gate on `A:/Games/mc2-opengl/mc2-win64-v0.3` mission `mc2_01`.

**Reads (authoritative):** `docs/tree-override-lod-spec.md`, `docs/model-override-lighting-lod-recon.md`, `docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md`, `docs/model-override-mvp-notes.md`.

---

## Hard constraints
No collision changes (collision reads stock `treeShape[]`, LOD-independent) · LOD only on `*RenderShape[]` · all LODs registered+baked before first replay · preserve `needsFullBakeNextFrame` black-tree guard at every active-shape change · no stock-asset/`tgl.fst` mutation · no `git add -A` · no-mod identity stays byte-identical · buildings out of MVP scope (trees only).

## Environment discipline (every runtime task)
- NO process killing; if deploy exe locked, STOP + report. Build `--target mclib` THEN `--target mc2` (VS cmake `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`, `build64`, RelWithDebInfo, 0 errors). Deploy+run from `A:/Games/mc2-opengl/mc2-win64-v0.3`, `-mission mc2_01`, `--validate`. Read logs via PowerShell `Select-String` (UTF-16; bash grep/strings broken). View screenshots (PIL `.tga`→`.png`). Explicit-path commits.

## File Structure
- Modify `mclib/bdactor.h` — `treeRenderShape` → `treeRenderShape[MAX_LODS]`; `getTreeRenderShape(long lod)` real indexing; per-LOD `staticReg[MAX_LODS]` (or a per-LOD tuple struct); `treeRenderShapeLodCount`.
- Modify `mclib/bdactor.cpp` — per-LOD type-load register+bake (TreeAppearanceType init + `TreeAppearance::registerStatic`); distance `currentLOD` selection in `TreeAppearance::update`; per-frame `markVisible`/`recipeIndex`/`lightDataIndex` select active LOD; `needsFullBakeNextFrame` re-arm on switch; M4 abort→mark-LOD-unavailable + clamp.
- Modify `GameOS/gameos/gos_static_prop_batcher.{h,cpp}` — light-slot cardinality instrumentation (Task 0); confirm per-recipe registration handles N LODs.
- Modify `mclib/model_override_registry.{h,cpp}` — parse `lods[]`/`autoLod` (Task 5).
- Modify `mclib/assimp_importer.cpp` or a load-time helper — auto-LOD decimation hook (Task 4) — OR reuse offline gltfpack-generated LOD files (decide in Task 4).
- Create `docs/tier1_env_vars.md` row for `MC2_FORCE_LOD` (Task 6).
- Modify `docs/model-override-mvp-notes.md` — record results.

---

## Task 0 — LIGHT-SLOT CARDINALITY PROOF GATE (M2 — must pass before any LOD code)

**Goal:** Prove that override forest light slots are bounded by ~types×LODs after `addLightDataStructure` content dedup, NOT instances×LODs. If unbounded → STOP and split a separate lighting-ownership slice (do NOT build LOD on a broken assumption).

**Files:** `GameOS/gameos/gos_static_prop_batcher.cpp` (or `mclib/txmmgr.cpp` near `addLightDataStructure`), env-gated instrument only.

- [ ] **Step 1: Instrument unique light-slot consumption (diagnostic, no behavior change)**

Add an env-gated (`MC2_LIGHTSLOT_TRACE`) one-line-per-map summary in `MC_TextureManager::addLightDataStructure` / the static bake path logging, at mission_ready:
```
[LIGHTSLOT v1] override_tree_types=N registered_lod_recipes=R instances=K unique_light_slots=U dedup_hits=H
```
Compute U = total slots in `lightData_` attributable to static props (or total table count delta across the static-prop registration), K = static-prop instance count, R = registered recipe count. Pure counters; no draw/state change.

- [ ] **Step 2: Build mclib+mc2; deploy v0.3; run the current single-LOD lush forest**

Manifest = the existing lush 6-type override (tc1_*, palm1, palms → tree_lush.glb). Run `--validate --frames 20 -mission mc2_01` with `MC2_LIGHTSLOT_TRACE=1`. Read the `[LIGHTSLOT v1]` line via PowerShell.

- [ ] **Step 3: GATE — evaluate cardinality**

PASS if `unique_light_slots` is ~O(types) (small, tens), i.e. does NOT scale with `instances` (K≈150). Record N, R, K, U.
**STOP CONDITION:** if `unique_light_slots ≈ K` (per-instance growth, dedup ineffective) → **HALT this plan**, write `docs/model-override-lighting-ownership-recon.md` noting per-instance light-slot growth, and split a separate lighting-ownership slice. Do NOT proceed to Task 1.

- [ ] **Step 4: Commit the gate result**
```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp docs/model-override-mvp-notes.md
git commit -m "diag(modoverride): light-slot cardinality gate (M2) — [PASS|STOP] U=.. K=.."
```

---

## Task 1 — `treeRenderShape[MAX_LODS]` + real `getTreeRenderShape(lod)` (M1 prereq + minors)

**Files:** `mclib/bdactor.h`, `mclib/bdactor.cpp`

- [ ] **Step 1: Promote the member**

In `TreeAppearanceType` (bdactor.h ~:478): `TG_TypeMultiShapePtr treeRenderShape;` → `TG_TypeMultiShapePtr treeRenderShape[MAX_LODS];` + `long treeRenderShapeLodCount = 0;`. Init all NULL in the type ctor; free all non-null in destroy.

- [ ] **Step 2: Real LOD indexing with stock fallback**

`getTreeRenderShape(long lod)` (bdactor.h ~:518): return `treeRenderShape[lod]` if non-null, else clamp to the highest non-null `treeRenderShape[<=lod]`, else stock `treeShape[lod]`. NEVER return a null. Keep `getTreeCollisionShape` = stock `treeShape[lod]` unchanged.

- [ ] **Step 3: Override import fills index 0 only (behavior unchanged)**

The existing override import (TreeAppearanceType load) writes `treeRenderShape[0]` (was the single member). `treeRenderShapeLodCount=1`. With only LOD0 populated, `getTreeRenderShape(any)` → LOD0 → identical to today.

- [ ] **Step 4: Build + no-mod identity + single-LOD override unchanged**

Build mclib+mc2. Deploy v0.3. Run (a) empty manifest → stock identical; (b) lush 6-type override → renders as before (view screenshot, compare to `.claude/lush_apron_tree_zoom.png`), exit 0, 0 GL errors.

- [ ] **Step 5: Commit**
```bash
git add mclib/bdactor.h mclib/bdactor.cpp
git commit -m "feat(modoverride): treeRenderShape[MAX_LODS] + real getTreeRenderShape(lod) indexing (LOD0 only; no behavior change)"
```

---

## Task 2 — Per-LOD static registration + `staticReg[MAX_LODS]` (M1 + M4)

**Files:** `mclib/bdactor.cpp`, `mclib/bdactor.h`

- [ ] **Step 1: Make `staticReg` per-LOD**

`TreeAppearance::staticReg` (single struct) → `staticReg[MAX_LODS]` (each `{registered, recipeIndex, lightDataIndex, ...}`). Add `activeLOD` (per-instance, default 0).

- [ ] **Step 2: Register every populated LOD before finalize (M4)**

In `TreeAppearance::registerStatic` (bdactor.cpp ~:4796): loop `lod` over populated `treeRenderShape[lod]`; for each, register the recipe and capture `staticReg[lod].recipeIndex`. **M4:** if `buildRecipe` MISSes for a LOD (the `:4874` abort), mark `staticReg[lod].registered=false` (LOD unavailable) and CONTINUE to other LODs — do NOT abort the whole instance or half-populate. Ensure all LOD geometry is in `s_typeIndex` before `finalizeGeometry` (same pre-finalize pass that registers LOD0 today; register all LODs there).

- [ ] **Step 3: Per-frame replay uses the active LOD tuple (M1)**

The per-frame `markVisible`/`touch`/`mc2CacheOrBakeStaticGpuLight` (bdactor.cpp ~:1422-1423, ~:2336-2340, ~:4226) must select `staticReg[activeLOD]`'s recipeIndex + lightDataIndex. With `activeLOD` pinned 0 (no distance switch yet), behavior == today.

- [ ] **Step 4: Pre-bake all LODs (safety)**

At mission-load registration, re-arm `needsFullBakeNextFrame` so every registered LOD recipe gets its `lightData_` populated + permanent baked slot before any replay (recon §1; `mc2CacheOrBakeStaticGpuLight` :1903). Assert (env-gated) every `staticReg[lod].registered` LOD has a baked slot before first `markVisible`.

- [ ] **Step 5: Build + verify baseline unchanged**

activeLOD pinned 0 → forest renders as Task 1; no-mod identity holds; exit 0, 0 GL errors. The `[LIGHTSLOT v1]` count unchanged (still LOD0 only).

- [ ] **Step 6: Commit**
```bash
git add mclib/bdactor.h mclib/bdactor.cpp
git commit -m "feat(modoverride): per-LOD staticReg[] + register/bake all LODs pre-finalize (M1/M4; activeLOD pinned 0)"
```

---

## Task 3 — Auto-LOD generation at import (the lower LODs)

**Files:** decide: (a) offline — generate LOD1/LOD2 `.glb` via gltfpack and reference via manifest `lods[]`; or (b) load-time decimation hook. **MVP = (a) offline** (simplest, deterministic, no runtime decimation cost): the manifest supplies LOD1 (and optional LOD2) source files; importer loads each into `treeRenderShape[lod]`.

- [ ] **Step 1: Generate a decimated LOD1 asset**

Use gltfpack (`.claude/gltfpack_build/Release/gltfpack.exe -kn`) and/or the leaf-card-preserving thinner to produce `tree_lush_lod1.glb` (~50-100k tris, canopy preserved) from the LOD0 source. Record tri counts. Deploy to v0.3 `data/model_overrides/source/trees/`.

- [ ] **Step 2: Import per-LOD with textures + bounds**

In the override import, for each manifest LOD source: `ImportGeometryFromFile` → `treeRenderShape[lod]`, `LoadOverrideRenderShapeTextures` (per LOD; shared LOD0 textures for decimated LODs), vertex-tight mesh-local AABB per LOD (existing ComputeBoundingBox path). `treeRenderShapeLodCount = populated count`.

- [ ] **Step 3: Build + verify both LODs register + bake (still activeLOD=0)**

`[LIGHTSLOT v1]` now shows `registered_lod_recipes` = types×LODs; `unique_light_slots` still bounded (re-confirm M2 gate holds with LODs present). exit 0, 0 GL errors. activeLOD still 0 → visual unchanged.

- [ ] **Step 4: Commit**
```bash
git add mclib/bdactor.cpp data/model_overrides/source/trees/tree_lush_lod1.glb mclib/model_override_registry.cpp
git commit -m "feat(modoverride): import per-LOD override meshes (LOD1 decimated) + per-LOD textures/bounds"
```

---

## Task 4 — Manifest `lods[]` / `autoLod` parse + validation

**Files:** `mclib/model_override_registry.{h,cpp}`, `tests/model_override/`

- [ ] **Step 1: Failing unit test for `lods[]` parse/validation**

Add fixtures + test blocks: valid `lods[]` (ascending lod+distance), reject non-ascending, reject missing LOD0, reject unsafe per-LOD `source` (reuse `isSafeSource`), `autoLod` default true. (Follows the existing registry test pattern — red first, do not commit red.)

- [ ] **Step 2: Implement parse**

Extend `ModelOverrideRecord` with `lods` (vector of `{lod, sourceRelPath, distance}`) + `autoLod`. Validate per Step 1. Lazy-load consumers (bdactor) read these.

- [ ] **Step 3: Green — unit test passes**

`ctest -R model_override_tests` → ALL TESTS PASSED. Single green commit (test+impl+fixtures).

- [ ] **Step 4: Commit**
```bash
git add mclib/model_override_registry.h mclib/model_override_registry.cpp tests/model_override/
git commit -m "feat(modoverride): manifest lods[]/autoLod parse + validation (+tests)"
```

---

## Task 5 — Distance-driven `currentLOD` selection (the perf win) + safety guard (M3 minors)

**Files:** `mclib/bdactor.cpp`, `docs/tier1_env_vars.md`

- [ ] **Step 1: Distance selection in `TreeAppearance::update`**

Compute `activeLOD` from camera distance vs `lodDistance[]` (loaded at `:3634`), clamped to the highest LOD with `staticReg[lod].registered && baked` (M4 clamp — never select an unavailable/unbaked LOD). Default to 0 if none.

- [ ] **Step 2: `MC2_FORCE_LOD` runtime gate (M3)**

`getenv("MC2_FORCE_LOD")` → if set (0..N), override `activeLOD` to that value (clamped to available). Runtime, not `#ifdef`. Add a row to `docs/tier1_env_vars.md`.

- [ ] **Step 3: Re-arm safety on switch (black-tree guard)**

When `activeLOD` changes for an instance, set `needsFullBakeNextFrame=true` (belt-and-suspenders; the pre-bake from Task 2 already covers it, but the guard is mandatory per spec §2). Verify the switch is a recipe lookup — NO `invalidateStaticRegistration`, NO re-register.

- [ ] **Step 4: Build + the perf win + black-tree gate**

Run forest with default distance LOD: (a) Tracy/`Render.GpuStaticProps` GPU zone drops materially vs Task 3 baseline (capture before/after; aim toward stock-tree-class ms). (b) `MC2_FORCE_LOD=0..N` each: view screenshots — trees render at each LOD, **no black trees**, no GL errors. (c) zero-light-slot counter (Task 2 assert) == 0 across all forced LODs + a camera dolly crossing LOD bands. (d) collision/`cellsCovered` identical to stock at every LOD (footprint diff == 0). (e) no-mod identity holds.

- [ ] **Step 5: Commit**
```bash
git add mclib/bdactor.cpp docs/tier1_env_vars.md
git commit -m "feat(modoverride): distance-driven tree LOD selection + MC2_FORCE_LOD gate + black-tree re-arm guard (the perf win)"
```

---

## Task 6 — Regression gate, perf capture, docs

- [ ] **Step 1: Black-tree regression gate wired**

A repeatable check (script or smoke case): mission load + `MC2_FORCE_LOD` sweep + dolly; assert `zero_light_slot_emitted == 0` and no all-black tree (sample tree pixels / the zero-light counter). Document the command.

- [ ] **Step 2: Perf before/after recorded**

Record `Render.GpuStaticProps` GPU zone: no-LOD baseline (Task 3, activeLOD=0) vs distance-LOD (Task 5). Record pool peaks (stay low). Put numbers in notes.

- [ ] **Step 3: Update docs**

`docs/model-override-mvp-notes.md`: LOD MVP result + the M2 cardinality verdict. Add the model-override LOD row to `docs/asset-pipeline.md` (§7).

- [ ] **Step 4: Commit**
```bash
git add docs/model-override-mvp-notes.md docs/asset-pipeline.md tests/smoke/run_smoke.py
git commit -m "test+docs(modoverride): black-tree regression gate + LOD perf capture + asset-pipeline row"
```

---

## Validation summary (the gates that matter)
- **M2 cardinality (Task 0, blocking):** `unique_light_slots` bounded ~types×LODs, not ×instances. STOP+split if not.
- **Perf win (Task 5):** `Render.GpuStaticProps` GPU zone drops materially with distance LOD vs full-detail-everywhere.
- **Black-tree (Tasks 5/6, mandatory):** `zero_light_slot_emitted==0` across all LODs + switches; no black trees.
- **Correctness:** no-mod byte-identical; collision/`cellsCovered` identical at all LODs; 0 GL errors; +0 destroys; per-LOD bounds enclose mesh, cull works per active LOD.
- **No phantom gates:** `MC2_FORCE_LOD` implemented + in `tier1_env_vars.md`.

## Self-review (spec coverage + advisor majors)
- M1 → Tasks 1+2 (`treeRenderShape[MAX_LODS]`, `staticReg[MAX_LODS]`, active-LOD tuple in markVisible/recipeIndex/lightDataIndex). ✓
- M2 → Task 0 blocking gate + re-confirm in Task 3; explicit STOP→split. ✓
- M3 → Task 5 `MC2_FORCE_LOD` getenv + tier1_env_vars.md row. ✓
- M4 → Task 2 Step 2 (register all LODs pre-finalize; MISS→mark-unavailable, no half-populate) + Task 5 clamp to highest available baked LOD. ✓
- Minors → Task 1 (`getTreeRenderShape(lod)` real index + stock fallback), Task 5 (re-arm guard), collision untouched throughout, black-tree gate mandatory (Task 6). ✓
- Stop conditions (spec §8) preserved: Task 0 STOP; M4 LOD-unavailable clamp; collision-leak BLOCK; pre-finalize timing in Task 2.
- Type consistency: `staticReg[MAX_LODS]`, `treeRenderShape[MAX_LODS]`, `getTreeRenderShape(lod)`, `activeLOD` used consistently across tasks.
- Buildings explicitly deferred (trees-only MVP).

---

## Plan-review revisions (render-spine review 2026-06-03 → REVISE→folded; these SUPERSEDE the text above where they conflict)

**M-A (load-bearing — recipe/light must key on the render LOD shape, not `treeShape`):** Today `registerStatic` builds the recipe via `buildRecipeFromShape` over the per-instance `treeShape` (the render-instance, currently CreateFrom'd from LOD0) and captures `lightDataIndex` from `treeShape`. Per-LOD selection that only swaps `staticReg[lod].recipeIndex` while the recipe geometry stays LOD0 yields **no perf win** and breaks the `IsStaticNow` `shape==treeShape` key. **Required:** in Task 2, each LOD's recipe AND `lightDataIndex` must be built from `getTreeRenderShape(lod)` geometry (per-LOD `buildRecipeFromShape` / cached light index). At runtime, the active-LOD switch (Task 5) must also repoint the per-instance render-instance to the active LOD's shape (re-`CreateFrom` from `getTreeRenderShape(activeLOD)`) with the `needsFullBakeNextFrame=true` re-arm, OR draw purely via the active LOD's pre-registered recipe — whichever the executor confirms actually swaps the drawn VBO geometry. `treeShape`'s collision role is unaffected (collision reads stock `treeShape[]`, separate).

**M-B (stale line refs — fix before editing):** `TreeAppearance::registerStatic` is at **bdactor.cpp:4806** (`:4796` is `invalidateStaticRegistration`). Tree per-frame `markVisible` is at **:4226** (the `:1422` cite is the **Building** path — do not edit it for trees). `buildRecipe` MISS abort `return;` at `:4874` is correct.

**M-C (avoid 3× LOD0 upload):** The geometry-register loop `for (i<MAX_LODS) registerMultiShape(getTreeRenderShape(i))` (bdactor.cpp ~:4821-4822) currently uploads the single override shape `MAX_LODS` times. After Task 1 promotes `treeRenderShape[]`, change the loop to iterate **only populated LODs** (`i < treeRenderShapeLodCount`), registering each distinct LOD shape once — no duplicate type registration / slot inflation.

**Minors folded:**
- m1: `lodDistance[]` at `:3634` already has a `*=5.0f` zoom-push for stock LOD bands — Task 5 must decide whether render-LOD bands reuse the scaled thresholds or define their own; state it.
- m2: Task 0 `[LIGHTSLOT v1]` line fires at **mission_ready only** (one line/map), never per-frame.
- m3: Task 0 commit add-list must match the file actually instrumented (`txmmgr.cpp` if the counter lives by `addLightDataStructure`, else `gos_static_prop_batcher.cpp`).

**Verdict after fold:** render-spine review was EXECUTE-ready contingent on M-A/M-B/M-C; with these folded the plan is execution-ready. Re-grounding confirmed: `treeRenderShape`:478, `getTreeRenderShape`:518 (discards lod today ✓), `mc2CacheOrBakeStaticGpuLight`:1903, `addLightDataStructure`:1278 (+dedup :1286), `lodDistance`:3634, `currentLOD=0`:3834, `MAX_LODS=3`, `finalizeGeometry` one-shot — all verified.
