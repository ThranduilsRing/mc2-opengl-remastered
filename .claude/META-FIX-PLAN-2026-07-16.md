# MC2 Recurrence-Proofing — Merged Implementation Plan
Worktree: `A:/Games/mc2-issue-fixes` (branch `claude/github-issue-fixes-1`). All six designs are adversarially reviewed with anchors re-verified against this worktree; treat cited line numbers as hints, match on **symbols/sentinels, never frozen lines**.

## Priority ranking — (protection value × confidence) / effort

| Rank | Design | Value | Confidence | Effort | Score rationale |
|---|---|---|---|---|---|
| 1 | **C** split-path divergence | High (#24/#34 classes, silent visual drift) | Very high (20+ anchors verified, explicit day-one-green plan) | Low-med (1 checker + 2 identity shader edits) | Pure static-first; nothing behavioral changes |
| 2 | **E** file-path fragility | High (silent content loss; `fileExists` bug is CONFIRMED live at file.cpp:853) | High (game-free unit-testable; permutation cell proven red pre-fix) | Medium | Fixes a real bug + locks the class |
| 3 | **D** resolution/aspect | High (#45/#49 + clamp merge-drop class) | High (4 pinned anchors; layer 3 explicitly cuttable) | Medium | Checker + gated audit; matrix is nightly-only |
| 4 | **F** silent orders | Med-high (three issue classes → red gates) | Med-high (refutations fixed; behavioral legs stochastic, need deployed exe) | Medium | Static chokepoint is cheap; fixture is ~70 lines |
| 5 | **A** registry coherence | High (#50 class) | Medium (save/load path unverified; tier1 may not hit LINKUP-PENDING) | Med-high (new accessors + audit across many game TUs) | Largest game-code surface after B |
| 6 | **B** silent asset-load | Medium (provenance corrected: mostly disambiguation + adjacent classes, not #50/#43 prevention) | Medium (placeholder-on-GPU-path unverified; needs 24-map baseline before arming) | High (~17 call sites, harness, placeholder) | Ship last; arming gates are observational-first |

Batches follow the ranking: **Batch 1 = infra + C + E**, **Batch 2 = D + F**, **Batch 3 = A + B**. Grouping is also forced by file compatibility: A, B, F all touch `code/mission.cpp` and A+F both touch `code/mech.cpp` — F lands in batch 2, A+B land together in batch 3 (one agent, sequential edits, one build).

---

## Shared infrastructure — build ONCE in Batch 1

### INFRA-1: gated audit emitter (`mclib/mc2_audit.h` / `mc2_audit.cpp`, new)
Every design emits gated, machine-parseable stdout marker lines. One helper, in **mclib** so mc2 / EditRel / mc2_launcher all link:
- `bool mc2AuditLatch(const char* envVar)` — static-getenv latch (call-site `static bool on = ...`), gate-off path is one bool test, byte-identical unset (proven pattern, ISSUE-FIXES-STATUS.md:123-124).
- `void mc2AuditLine(const char* line)` — writes one stdout line AND mirrors to `mc2_diag::writeEvent` when diag is active. **Does not impose a grammar** — each design's reviewed line format (`[MOVER_COHERENCE] VIOLATION rule=...`, `[ASSET_LOAD v1] event=...`, `[SPLIT_PATH_PARITY]`, `[APPEAR_AUDIT v1]`, `[ORDER_FIXTURE] ...`, `ORDER_LIVENESS_SUMMARY ...`) is passed pre-formatted; the reviewed grammars are contract, do not "normalize" them.
- `bool mc2AuditOnce(const char* tag, uint64_t key)` — bounded dedup set (cap 64, kLogLineCap precedent mc2_verify.cpp:30) for per-(entity,rule) / per-path rate limiting.
Consumers: A (MOVER_COHERENCE), B (ASSET_LOAD/APPEAR_AUDIT), C (SPLIT_PATH_PARITY trace), D (RES_ASSERT log mode), F (fixture lines).

### INFRA-2: checker skeleton (`scripts/checklib.py`, new)
Shared python module for the three new .py checkers (C, D, F) — the .sh checkers (A, B, E) follow the existing `check-asset-scale-callers.sh` precedent and need no new infra. Provides:
- comment stripping (`//` + `/* */`) — **mandatory before any return-scan** (F's refutation (b): four commented `return(4)`s in mech.cpp:8199-8231);
- whitespace-normalization pre-pass for regex rules (C);
- brace-matched function-body extraction (F; same technique as check-*-ownership.py family);
- anchor-pin assertion helper that matches **symbol + shape**, reports file+found-line;
- baseline-JSON ratchet: `load / compare / --update-baseline` (D Rule B; E's count ratchet may reuse or stay .sh per its design — implementer's choice, contract identical);
- `--self-test` scaffold that injects a synthetic violation and asserts nonzero exit (house pattern check-contracts.sh:126);
- exit convention 0/1/2 per check-combat-soak.py:20-22.

### INFRA-3: table-driven smoke marker buckets (`scripts/smoke_lib/` + `run_smoke.py`)
Generalize the VEG-SMOKE-FLOOR clone-per-feature pattern once: a registry table `MARKER_BUCKETS = [(regex, bucket_name, severity, manifest_override_key)]` where severity ∈ {red, advisory}, wired into the existing scan (logparse.py:140 counting, gates.py:67-68 red bucket, manifest.py:33/81-84 per-mission override, hard-fail promotion run_smoke.py:2133-2150). Later batches add **one row each** instead of re-cloning:
- A: `\[MOVER_COHERENCE\] VIOLATION` → red; `ADVISORY` → advisory.
- B: `event=(parse_fail|required_missing|appr_type_fail)` → red bucket `asset_load_fail` (NOT bare `file_missing`); `icon_oob` and `appear_audit` buckets registered but **disarmed** until their baselines pass.
- F: `ORDER_LIVENESS_SUMMARY` uses the VEG summary-parser path (run_smoke.py:66-127) — absence-is-failure only when `MC2_FIXTURE_ORDER_LIVENESS` is set (activation-flag pattern :1094-1095).

### INFRA-4: env-var registration recipe (checklist, applied per batch)
Every new gate does all four steps or `check-env-registry.sh` / ENV-DROP bites: (1) `kAuxEnvVars` entry, RendererFeatureRegistry.h table at :599, `EnvVarKind::Trace` default false (MC2_LINKUP_TRACE shape) — exception: `MC2_ASSET_FAILLOUD` is a default-ON killswitch (MC2_TERRAIN_TESS shape); (2) `docs/tier1_env_vars.md` row; (3) `run_smoke.py` Popen passthrough allowlist (~:1161-1410); (4) `sh scripts/check-env-registry.sh` PASS. Full roster: Batch 1 — `MC2_SPLIT_PATH_PARITY_TRACE`; promote `MC2_LOG_FILE_RESOLVE` + `MC2_RESOLVE_TRACE_FILE` to kAuxEnvVars. Batch 2 — `MC2_RES_ASSERT`, `MC2_FIXTURE_ORDER_LIVENESS`; passthrough-only additions `MC2_WIDTH/MC2_HEIGHT/MC2_WINDOWED/MC2_FORCE_43/MC2_CAMERA_ASPECT_NATIVE`. Batch 3 — `MC2_MOVER_COHERENCE_AUDIT`, `MC2_ASSET_FAILLOUD`, `MC2_ICON_FAILLOUD`, `MC2_APPEAR_AUDIT` (+optional `MC2_ASSET_TRACE`).

### INFRA-5: nightly-oracle checker convention (doc note, not code)
`check-file-resolve-miss.py` (E) and `check-order-liveness.py` (F) both clone check-combat-soak.py exactly (oracle table, JSONL counting, stale-proof unlink :66-70, env assembly :72-79). Keep as clones per the reviewed designs; do not force a shared module — divergent oracle logic dominates.

### Cross-batch rules
- `check-contracts.sh`, `RendererFeatureRegistry.h`, `run_smoke.py`, `docs/tier1_env_vars.md` are touched by every batch — **append-only edits, sequential batches, never parallel agents on these files**.
- Build discipline: `/mc2-build` skill, RelWithDebInfo only; load-bearing changes (mclib/file.cpp in B1; forcegroupbar.h/controlgui.h header changes in B3) → `--clean-first`. Shaders deploy in lockstep with exe (B1 touches shaders). Deploy via `scripts/deploy_payload.py` only, commit-first-then-build-then-deploy.
- Smoke: canonical tier1 command verbatim, never `--kill-existing`/`--with-menu-canary`/`--duration >30`.
- Before each batch's code slice: `repo_query.py slice-preflight` on its symbols (mandatory per docs/disciplines.md).

---

## Batch 1 — Shared infra + Design C (split paths) + Design E (path fragility)

**Designs included:** INFRA-1/2/3/4, C in full, E in full.
**Why compatible:** zero engine-file overlap (C = shaders + checker; E = mclib file layer + unit tests); shared files are the append-only quartet; one build + one tier1 cycle covers both.

**Files:**
- Infra: `mclib/mc2_audit.h/.cpp` (new), `scripts/checklib.py` (new), `scripts/smoke_lib/logparse.py`, `scripts/smoke_lib/gates.py`, `scripts/smoke_lib/manifest.py`, `scripts/run_smoke.py`
- C: `scripts/check-split-path-contracts.py` (new), `scripts/split-path-stamps.json` (new), `shaders/terrain_lod_chunk.vert` (LOCKSTEP-MIRROR marker on :16 local const), `shaders/gos_terrain.tese` (no-op `clip.z += TERRAIN_DEPTH_FUDGE * clip.w` after :133), `docs/render-backend-seams/split-path-pairs.md` (new)
- E: `mclib/file.cpp` (fileExists normalization; File::open branch to `fst_normalize_loose_key_inplace`), `mclib/fastfile.cpp` (callee-side case+slash normalize before elfHash :78, same buffer to name compare :83), `mclib/fst_hash.h/.cpp` (in-place variant), `code/saveload.cpp` (delete dead elfHash decl :223), `tests/unit/test_path_permutations.cpp` (new), `tests/unit/CMakeLists.txt`, `scripts/check-path-normalization.sh` (new), `scripts/path-open-baseline.txt` (new, generated not hand-written), `scripts/check-file-resolve-miss.py` (new, nightly), `scripts/check-env-registry.sh`
- Shared quartet: `scripts/check-contracts.sh` (+2 run_check rows + self-tests), `RenderCore/RendererFeatureRegistry.h`, `docs/tier1_env_vars.md`, `scripts/run_smoke.py` (passthrough)

**Proof recipe:**
1. `sh scripts/check-contracts.sh` — both new checkers GREEN on current HEAD (C's known day-one false positives resolved via allowlist/intersection/island-only entries; E's Rule-1 baseline clean after saveload.cpp decl removal) AND `--self-test` legs prove each can fail.
2. Unit tier: `py -3 tools/build_contract_harnesses.py --unit-only --run` — permutation matrix. **Red-before/green-after gate:** run test_path_permutations against pre-fix file.cpp first; FST-only × backslash × fileExists cell MUST fail, then pass post-fix. FitIniFile × FST-only × backslash cell = issue-#8 regression lock.
3. Build `--clean-first` (file.cpp is load-bearing), deploy **exe+shaders together**, console check that `gos_terrain.tese` still compiles (hot-reload fails silently).
4. Tier1 5/5 (canonical command). C's shader edits are identity transforms; any pixel change = stop.
5. Nightly wiring smoke-tested once: one mission with `MC2_LOG_FILE_RESOLVE=2` + `MC2_RESOLVE_TRACE_FILE`, then `check-file-resolve-miss.py` against the seeded benign-probe baseline; one A/B leg `run_smoke --mission mc2_01 --mission mc2_24` with `MC2_TERRAIN_TESS=0` (Probe-B recipe) + `MC2_SPLIT_PATH_PARITY_TRACE=1`, verify no `match=0` events.

---

## Batch 2 — Design D (resolution/aspect) + Design F (silent orders)

**Designs included:** D in full (layer 3 harness cuttable if effort overruns), F in full.
**Why compatible:** D lives in GameOS/gameos*, mclib/camera.cpp, RenderCore; F lives in code/mech.cpp, code/mission.cpp, scripts. No engine-file overlap; shared quartet appended by one agent.

**Files:**
- D: `scripts/check-res-aspect-contract.py` (new, + generated baseline JSON — regenerate on this batch's base commit via `--update-baseline`), `RenderCore/ResolutionContract.h` (new), `mclib/camera.cpp` (audit after :2730), `GameOS/gameos/gameosmain.cpp` (frame audit near :531 with 3-frame grace latch + #49 windowed oracle), `GameOS/gameos/gameos_graphics.cpp` (canvas roundtrip audit at flushHUDBatch :7563; new `gos_GetHudResClampEnabled`), `GameOS/include/gameos.hpp` (decl beside :2962), `GameOS/gameos/gameos_graphics_params.cpp` (lift computeUiCanvas16x9 :61-75), `mclib/camera_frustum_math.h`, `tools/camera_frustum_harness/camera_frustum_harness.cpp`, `tests/smoke/matrices/resolution.json` (new; res_force43 pairs FORCE_43 with `MC2_CAMERA_ASPECT_NATIVE=0` — never ship the red-by-design native pairing), `docs/critical_inline_rules.md` (two-space contract paragraph)
- F: `scripts/check-order-gate-coverage.py` (new; uses checklib comment-strip + brace-match), `scripts/check-order-liveness.py` (new, 3 legs incl. #9 head-share ≥15% @ n≥30), `code/mech.cpp` (FIREGATE-REGION-END sentinel after :8291; `fireGateTrace("range_mp")` at :8248 OR KNOWN_UNCOVERED entry), `code/mission.cpp` (merged `MC2_FIXTURE_ORDER_LIVENESS` fixture, ~70 lines, soak namespace :640-651, trademover-block shape, COMMANDER origin, dead-unit exemption), `docs/ai-order-triage.md` (new), INDEX-SMOKE-TEST.md pointer line
- Shared quartet: check-contracts.sh (+2 run_check), RendererFeatureRegistry.h (2 gates), run_smoke.py (ORDER_LIVENESS_SUMMARY row in INFRA-3 table + 6 passthrough vars), tier1_env_vars.md

**Proof recipe:**
1. `check-contracts.sh` green on HEAD day one: D's four Rule-C anchors (boot clamp gameosmain.cpp:1355, SetScreenMode clamp :8119-8122, #49 keep-window guard :5608-5615, #45 drawable-aspect override camera.cpp:2729) all found; F's checker green because the range_mp gap is traced-or-declared **in the same slice**. Self-tests fail synthetically.
2. Build + deploy; tier1 5/5 byte-identical (all gates default-OFF, static latches).
3. Harness: `py -3 tools/build_contract_harnesses.py --run` — canvas math cases at 1024x768/1920x1080/2560x1080/1280x1024/800x600 + remap∘inverse identity including hud-scale composition.
4. `py -3 scripts/run_smoke_matrix.py resolution` — 5 entries × mc2_01+mc2_24, `MC2_RES_ASSERT=fatal`; verify check-smoke-matrices.py validates the JSON first; res_windowed is the machine oracle for #49. Screenshots land in artifacts as human evidence.
5. `py -3 scripts/check-order-liveness.py --exe <lane>/mc2.exe --duration 90` — attack leg (mc2_24), move+stall leg (mc2_16, empty brain-freeze stream = exit 2 never silent pass), effectiveness leg (#9: aimed-HEAD share). Record the mc2_16 baseline that later promotes frozen-signature (c) from WARN.
6. Historical replay spot-check: revert #45 fix locally → chokepoint (i) trips at res_16x9; revert #49 guard → res_windowed oracle trips; delete nofire emitter → order_gate_coverage red. Restore.

---

## Batch 3 — Design A (registry coherence) + Design B (asset-load)

**Designs included:** A in full (layer-3 chokepoint asserts optional slice 2), B in full (Parts 0-5; arming of icon_oob/appear_audit buckets deferred behind their baselines).
**Why together:** both have heavy `code/mission.cpp` surface (A: audit fn beside bridgeEmitMoverState :554 + pairing sites; B: mission_ready hook ~:4811) — one agent edits sequentially, one build+smoke cycle. Lowest-ranked designs land last so earlier protection ships even if this batch slips.

**Files:**
- A: `scripts/check-mover-registry-coherence.sh` (new; S1 write allowlist incl. member-write pattern, S2 pairing windows, S3 three HARD pins + advisory WARNs, S4 #42b guard shape — **sole defense, never demote**, S5 link-up gate shape), `code/mission.cpp` (auditMoverCoherence, I1-I5 + I1b bar-full carve-out + I2 player-origin scoping), `code/forcegroupbar.h/.cpp` (`hasMover` accessor), `code/controlgui.h/.cpp` (`rosterHasMover`), `code/missiongui.h` (passthrough), `code/mech.cpp`, `code/objmgr.cpp`, `code/missiongui.cpp`, `code/gametacmap.cpp` (reference anchors for checker)
- B: `mclib/tgl_load_guard.h/.cpp` (new), `mclib/msl.cpp` (Part 0 loader-internal classification chokepoint), `mclib/mech3d.cpp`, `mclib/bdactor.cpp` (incl. import path via loadBuildingImportSourceWithSidecarAxis :336), `mclib/gvactor.cpp`, `mclib/genactor.cpp` (~17 Required-slot sites — grep-verified list in design, includes the 5 draft omissions), `mclib/apprtype.h/.cpp` (baseShapeLoadFailed + getAppearance backstop), `code/icon_atlas_cell.h` (cellForIndexChecked returning **legacy cell** on OOB), `code/logisticsmechicon.cpp`, `code/mechlistbox.cpp`, `code/mechicon.cpp` (iconTileChecked ×3), `code/mission.cpp` (APPEAR_AUDIT hook), `scripts/check-tgl-load-callers.sh` (new, TGL-LOAD-EXEMPT/VERIFIED tags), `tools/icon_atlas_harness/`
- Shared quartet: check-contracts.sh (+2 run_check), RendererFeatureRegistry.h (4-5 gates), run_smoke.py (force-set `MC2_MOVER_COHERENCE_AUDIT=1` for --tier runs + INFRA-3 bucket rows), tier1_env_vars.md

**Proof recipe:**
1. `check-contracts.sh` green on HEAD: A's S1-S5 anchors verified present (mech.cpp:5977 `if (!isOnGUI())`, missiongui.cpp:2116 narrow guard, the 8 write sites); B's checker green after exempt tags on optional sites. Self-tests red synthetically (e.g. temporarily widen the :2116 guard → S4 fires).
2. Build `--clean-first` (header changes: forcegroupbar.h, controlgui.h, apprtype.h → class-layout relink rule), deploy.
3. Tier1 5/5 with `MC2_MOVER_COHERENCE_AUDIT=1` force-set: zero `[MOVER_COHERENCE] VIOLATION`, summary event present (diag-state liveness WARN otherwise); zero `asset_load_fail` bucket hits (always-on parse_fail/required_missing lines prove clean stock load).
4. Placeholder probe (closes B's biggest unverified): break one stock base shape in a scratch copy, launch, confirm magenta box **visible on the GPU batcher path** (real registered 4x4 texture, not numTextures=0) + one `required_missing` line + red smoke. Restore.
5. Icon harness: validate cellForIndexChecked row bound against 256/512/1024-wide atlas geometries; only then arm the `icon_oob` red bucket.
6. `MC2_APPEAR_AUDIT=1` observational run across all 24 maps → failed=0 baseline → only then add to tier1 allowlist and arm the `appear_audit` bucket.
7. Nightly add-ons: mc2_08 (LINKUP-PENDING row, #50 signature — pre-fix state must trip I5 then I2) + `MC2_SOAK_SALVAGE_TRADEMOVER` (trade row).

---

## Residual risks the merged plan does NOT close (carried forward verbatim from reviews)
Historical #42b has **static-only** defense (A/S4); 1 Hz audit sampling misses sub-second self-healing drift (A); MPlayer registries unaudited (A); RULE 3 freezes tokens not semantics — re-derived tess predicates evade it (C); stamp fatigue degrades RULE 4 (C); code//editor//gui/ 800-space exemption leaves new game-logic hardcodes unblocked (D); FORCE_43+native-aspect distortion defect is filed separately, not fixed here (D); loose-only sidecar readers are outside the canonical-key contract by design (E); partial regressions (half fire rate, 0.20 head share) stay green (F); B is diagnostic/adjacent-class protection, not #50/#43 prevention. Nightly-only legs (mc2_08, mc2_16, resolution matrix, MISS audit, TESS A/B) protect nothing if the nightly doesn't run — wire them into the existing nightly beside check-combat-soak.py as part of Batch 2/3 closeout.