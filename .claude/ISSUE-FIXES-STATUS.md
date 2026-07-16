# GitHub issue-fix batch — status

## META-FIX-BATCH-2 (recurrence-proofing: design D + design F) — IN PROGRESS 2026-07-16

Plan: `.claude/META-FIX-PLAN-2026-07-16.md` Batch 2; designs in
`.claude/META-FIX-DESIGNS-2026-07-16.json` (cls "CLASS D", "F").
Lane registered (mc2-repo-intel): META-FIX-BATCH-2. slice_preflight PASS for
both D and F symbol sets vs base 994c3776 (same MCP-tool deviation as
Batch 1 — worktree has no repo_query.py). Base HEAD: `c3fbc72b`.

**Commit ledger (per-unit):**
| Unit | Commit | State |
|---|---|---|
| D1 check-res-aspect-contract.py (RULE A modern-dir zero-tolerance, RULE B generated count-ratchet `scripts/res-aspect-baseline.json` (11 files / 84 tokens), RULE C 4 anti-revert pins) + check-contracts.sh rows `res_aspect_contract`/`res_aspect_selftest` | `5b2e8dbc` | DONE — GREEN on HEAD (A: all 7 modern dirs zero; B: ratchet matches baseline; C: all 4 pins found); --self-test RED on all 5 synthetic violations (modern read / ratchet grow / clamp drop / #49 guard / #45 revert) |
| D2 MC2_RES_ASSERT runtime audit — RenderCore/ResolutionContract.h (off/log/fatal, once-per-site dedup) + 3 chokepoints: camera_aspect (camera.cpp setOrthogonal, skip conditions re-derived independently of the #45 override), hud_clamp/drawable_stale/windowed_size #49 oracle (gameosmain.cpp mc2resFrameAudit @ draw_screen, 3-frame grace, desktop-smaller→note), hud_canvas_bounds/hud_roundtrip (flushHUDBatch shrink∘canvas ∘ gos_HudInverseMousePoint identity ≤0.5px). New gos_GetHudResClampEnabled + gos_ProbeDrawableSize (gameos.hpp/gameos_graphics.cpp). Registry: MC2_RES_ASSERT + MC2_FIXTURE_ORDER_LIVENESS kAuxEnvVars entries; run_smoke passthrough +7 (RES_ASSERT, WIDTH, HEIGHT, WINDOWED, FORCE_43, CAMERA_ASPECT_NATIVE, FIXTURE_ORDER_LIVENESS); tier1_env_vars.md batch-2 section; critical_inline_rules.md two-space contract paragraph. Ratchet baseline consciously regenerated 84→90 tokens (the audit's own contract-enforcing reads: camera detail line + gameosmain clamp check/oracle) | (this commit) | CODE DONE — check-env-registry PASS (344 registered); res-aspect checker PASS on regenerated baseline; build+runtime proof pending batch build |

## META-FIX-BATCH-1 (recurrence-proofing: INFRA + design C + design E) — COMPLETE 2026-07-16

**Tier1 verdict: PASS 5/5** (mc2_01/03/10/17/24, 30 s each, exit 0,
`[diag-state] PASS`), run 2026-07-16T10-38-26 via
`A:\Games\mc2-main\scripts\run_smoke.py --exe <lane>\mc2.exe` with
`MC2_SMOKE_REQUIRE_FINGERPRINT=0` (documented cross-worktree waiver; the
`MISMATCH exe=f017c70f expected=994c3776` lines are the expected artifact —
the same binary was fingerprint-VERIFIED `OK` by the worktree runner in the
A/B and file-resolve legs). Same stale `[visual-advisory] FAIL_CHANGED
2026-06-26` record as the Fixer-1/2 runs. Lane exe/manifest = `f017c70f`,
DIRTY=0, shaders in lockstep.

Plan: `.claude/META-FIX-PLAN-2026-07-16.md` Batch 1; designs in
`.claude/META-FIX-DESIGNS-2026-07-16.json` (cls "C", "CLASS E").
Lane registered (mc2-repo-intel): META-FIX-BATCH-1. slice_preflight PASS for
both C and E symbol sets vs base 994c3776 (worktree has no repo_query.py —
used the mc2-repo-intel MCP slice_preflight instead, noted deviation).

**Commit ledger (all landed, per-unit):**
| Unit | Commit | State |
|---|---|---|
| INFRA-1/2/3 (mc2_audit mclib TU + checklib.py + smoke marker-bucket table) | `7058450c` | DONE — pytest tests/smoke 81/81; checklib self-checks pass. mc2_audit diag mirror = registered SINK (mclib must not link GameOS; sink registered after mc2_diag::init in gos_CreateRenderer) |
| Design C (check-split-path-contracts.py RULES 1-4 + stamps + identity shader edits + MC2_SPLIT_PATH_PARITY_TRACE runtime hook) | `25609893` | DONE — checker GREEN on HEAD; --self-test RED on all 5 synthetic violations; env-registry PASS; marker-bucket row `split_path_parity` armed |
| Design E tests (path-permutation matrix) | `469a569b` | DONE — RED-BEFORE BANKED: 21 failing assertions, exactly the fileExists (0==2) + FastFileFind (nullptr) non-canonical cells; File::open/FitIni/Packet cells green pre-fix |
| Design E fix (canonical-key chokepoint + checkers + env promotions) | `26129767` | DONE — GREEN-AFTER: doctest 332/332 cases, 115023/115023 assertions; check-path-normalization + --self-test PASS; check-env-registry PASS (340 registered) |
| docs: gated-layout-macros ledger refresh (checker write side-effect raced the clean build → DIRTY deploy; committed to restore COMMIT-FIRST) | `9031d292` | DONE |
| INFRA-3 marker-bucket red-capability pytest (5 cases) | `cd5f806f` | DONE — 5/5 |
| check-file-resolve-miss --duration 60→30 (run_smoke hard cap; caught on first live run) | `87a44585` | DONE |
| file-resolve MISS benign-probe baseline seed (581 keys, mc2_01) | `f017c70f` | DONE — verify leg PASS unknown=0 |

Final HEAD `f017c70f` == deployed lane exe. pytest tests/smoke 86/86 at HEAD;
all four new check-contracts rows + both self-tests re-verified PASS at HEAD.

**Proof recipe progress (steps 3-5):**
- [x] mc2 clean build (`--clean-first`, RelWithDebInfo) GREEN at `26129767`
      (only the standard FFmpeg DELAYLOAD warnings).
- [x] EditRel + mc2_launcher build GREEN (exit 0).
- [x] Deploy exe+shaders lockstep to `releases/mc2-win64-issuefix-test` —
      manifest src_commit `9031d2927f80`, CLEAN (first deploy attempt carried
      DIRTY=1 because check-gated-layout-macros.py REGENERATED its ledger doc
      mid-build — checker write side-effect, refreshed line numbers committed
      as `9031d292`, relinked, redeployed; deployed gos_terrain.tese +
      terrain_lod_chunk.vert verified to carry the batch edits). NOTE:
      deploy_payload needs `--build-dir build64/RelWithDebInfo` +
      `--allow-new-target` for this non-canonical (pre-existing) lane.
- [x] gos_terrain.tese compile console check: tess-ON leg (artifact
      2026-07-16T10-35-47) logs `[TESS] Terrain material loaded successfully`
      with the batch-edited tese deployed — no silent hot-reload/compile
      failure.
- [x] tier1 5/5 canonical PASS (see verdict block above); lane exe
      relinked+redeployed at `f017c70f` (== HEAD, DIRTY=0). C's shader
      edits confirmed identity: zero shader_error buckets, all missions
      clean, no pixel-change signal.
- [x] scripts/file-resolve-miss-baseline.txt SEEDED (581 benign MISS keys,
      mc2_01 @30s, committed `f017c70f`) and verify leg PASS
      (misses=581 known=581 unknown=0; its smoke_ok=False was only the
      fingerprint hard-gate vs the then-stale exe — MISS verdict unaffected).
      FIRST ATTEMPT FAILED: checker defaulted --duration 60 which run_smoke
      hard-caps at 30 (exit 2 before launch, no trace) — fixed 60->30 in
      `87a44585`. NOTE for successors: the user's shell exports
      MC2_SMOKE_REQUIRE_FINGERPRINT=1 (worktree-runner fingerprint gate is
      HARD by default here) + several MC2_GPU_CULL_*/MC2_STATIC_POP_SPLIT
      vars (dropped by the allowlist, inert); commits between smoke legs
      move HEAD past the deployed exe and trip the gate — batch commits,
      then relink+redeploy, THEN smoke.
- [x] TESS=0 A/B leg with MC2_SPLIT_PATH_PARITY_TRACE=1: **no match=0**;
      both missions emitted `[SPLIT_PATH_PARITY] site=terrain_material_load
      pair=tess_vs_modern_shadow tess=0 shadow=1 match=1` (artifacts
      2026-07-16T10-27-50, mc2_24 PASS; mc2_01 flaked heartbeat_freeze_play
      on a 23.5s cold load — clean PASS on retry 2026-07-16T10-30-33 with
      5.9s warm load + the same match=1 line; exit-1 of the retry was ONLY
      the fingerprint hard-gate vs the tests-only commit, fixed by
      relink+redeploy). INFRA-3 red-capability proven by pytest
      tests/smoke/test_marker_buckets.py (5/5, commit `cd5f806f`).
- [x] Full check-contracts.sh table: **all 4 new rows PASS**
      (split_path_contracts, split_path_selftest, path_normalization,
      path_norm_selftest); table total 46 passed / 9 failed — ALL 9
      (include_firewall, no_raw_gl_from_game, render_contract_gbuf1,
      light_abi_lockstep, pipeline_desc, shader_injectors, texture_owners,
      spirv_artifacts, doc_links) verified PRE-EXISTING by running them in a
      throwaway worktree at pre-batch `85979c1e` — identical FAILs there;
      not touched by this batch (branch debt inherited with the fork).

**Known pre-existing reds (NOT from this batch):** check-doc-links.py exits 1
on 7 stale refs (README modder-beta, cxx17 recon paths, known_issues beta
link, render-perf-snapshot spec paths) — predates batch; full
check-contracts.sh run takes >5 min (not "cheap" anymore), run it with a
long timeout. test_rendercore.cpp stale TerrainTessellation expectation was
pre-existing red in the unit tier — fixed inside `26129767`.

**Deviations from plan (with justification):** (1) mc2AuditLine's
mc2_diag::writeEvent mirror is via registered sink, not direct call — mclib
cannot link GameOS TUs from tools; (2) check-path-normalization.sh Rule-2
"auto-shrink" became explicit --update-baseline + shrink notice — CI checks
must stay read-only; (3) RULE-1 color table gained decal.frag (fs-raw; pairs
terrain_overlay.vert, verified makeProgram site) and neutral-splat auto-pass
— review-verified additions the design's table missed; thin.vert unpackARGB
raw write FROZEN with do-not-copy note (latent R/B swap, sub-visible;
split-path-pairs.md); (4) fileExists strip-ladder asymmetry documented (not
fixed — outside reviewed mechanism); (5) unit-tier gate required un-staling
test_rendercore.cpp:357 (TerrainTessellation killswitch from b346a0a5).


Worktree: `A:/Games/mc2-issue-fixes`, branch `claude/github-issue-fixes-1`,
base nifty `994c3776`. NEVER pushed. Build: RelWithDebInfo, mc2 + EditRel +
mc2_launcher all green. Deploy lane: `A:/Games/mc2-opengl/releases/mc2-win64-issuefix-test`
(cloned from releases/mc2-win64-v0.5.0 minus mods/, payload deployed; Fixer-2
redeployed at `ca9ffaa6`, clean tree).

Reply drafts (NOT posted): `.claude/issue-reply-drafts.md`.

Fixer-1 batch = rows 1-7 (`45747525`..`bb55d6e9`, tier1 5/5 2026-07-16T03-01-35).
Fixer-2 batch = rows 8-12 below (`fdbae140`..`ca9ffaa6`, tier1 5/5 2026-07-16T04-11-00).

| # | Issue | Commit | Fix | Test evidence |
|---|-------|--------|-----|---------------|
| 1 | #42 salvage regression | `45747525` | missiongui.cpp:2109 guard narrowed to `isDisabled() && !getAppearance()` + appearance-guard on the getObjectNameId deref; MC2_SOAK_SALVAGE_TRADEMOVER fixture (default OFF, registered) exercises the REAL recover/setStatus/tradeMover/orderPowerUp flow for the needs-repro AAR half | build green; tier1 (below); hover-cursor behavior check is interactive-only (see Deferrals); fixture probe below |
| 2 | #50 link-up latch | `f659d48f` | objmgr.cpp moverInRect requires `isOnGUI()`; mech.cpp link-up gate keyed on `!isOnGUI()` alone; MC2_LINKUP_TRACE (default OFF, registered) | probe: mc2_08 smoke with MC2_LINKUP_TRACE=1 (below) |
| 3 | #43 AAR blocky icons | `16b8011d` | AssetScale::blitFactorFor — column-extended atlas ⇒ direct index; used at the 3 mechicon.cpp blits (conservative call-site variant); MC2_ASSET_SCALE_SELFTEST case added | selftest run (below); stock = strict no-op (factor already 1.0) |
| 4 | #34 near-clip w/o tess | `b346a0a5` | MC2_TERRAIN_TESS killswitch (default ON, registered); gos_IsModernShadowPathActive() replaces tess predicate at 8 sites (txmmgr 2692/2796, mech3d 3573/3782/4105/4144, gvactor 2149/2574) | killswitch A/B smoke (below) |
| 5 | #49 residual | `ab181cd8` | create_window windowed_hint param (Environment.fullScreen==0) OR-ed into the resizable predicate; MC2_RES_DIAG boot line; editor/text_tool call sites pass false | resizable-flag diag probe (below); drag-resize itself is interactive-only |
| 6 | #8 residual | `1279d0de` | one MC2_LOG_LOGISTICS-gated trace line in appendAvailability's silent open-failure branch (existing var, no new registration) | build green; tier1 (below) |
| 7 | (hygiene) EditRel link | `bb55d6e9` | no-op shim for mc2_save_camera_bookmark (pre-existing gap from 560dfc1d, surfaced by building EditRel; documented shim pattern) | EditRel links |
| 8 | #9 called shots | `fdbae140` | MC2_CALLED_SHOT_FIX (default OFF, registered): S1 calcAttackChance aimed modifiers floored 0.30/0.45/0.50; S2 aimed area-effect (LRM) hit delivers located damage to the called location (weaponbolt deliverAimedAreaHit, SP only, explosion kept cosmetic). S3: COMBAT `hit` events carry `loc`. MC2_AIMED_FIRE_FIXTURE headless probe (registered) | Probe E A/B (below); tier1 (below) |
| 9 | #9 fixture fix | `ca9ffaa6` | fixture origin PLAYER→COMMANDER — orderAttackObject only installs the tac order (and thus the aim) for COMMANDER origin; first probe run proved PLAYER-origin fire is unaimed | Probe E is post-fix |
| 10 | #17 reluctant fire | `e1eeaa05` | MC2_FIRE_DECISION_TRACE (default OFF, registered): `nofire` events in combatDecisionTree (per-gate tallies rd/am/rn/lk/ch/hot + los split + dist) + `firegate` events at fireWeapon's silent non-zero early returns. NO behavior change | Probe F (below) |
| 11 | #10 scripted stalls | `ecda2201` | MC2_BRAIN_FREEZE_TRACE extended: orderState / mainGoal+goalWID / tacOrder[GENERAL].code / pos x,y / LastMoveCalcErr — classifies alarm-loop vs goal-divert vs no-path vs path-not-moving. Var was registered but not allowlisted/documented; both added. NO behavior change | Probe G (below) |
| 12 | #32 data provenance | `ce474176` | docs/data-provenance.md researched + rewritten: mc2_patch.exe identified as Magic's FAN patch (not official); shipped data verified stock at 3 content markers; full source chain + this project's (additive) modifications enumerated; honest unknowns listed | doc-only; research evidence in the doc |

Env-var discipline: MC2_SOAK_SALVAGE_TRADEMOVER + MC2_LINKUP_TRACE registered in
RendererFeatureRegistry.h kAuxEnvVars (Trace); MC2_TERRAIN_TESS registered on the
kFeatureTable TerrainTessellation entry (default-ON killswitch). All documented in
docs/tier1_env_vars.md; all three added to the run_smoke.py env passthrough
allowlist. `sh scripts/check-env-registry.sh` = PASS (no new violations).

## Test evidence

All runs on the deployed lane `releases/mc2-win64-issuefix-test` (exe fingerprint
`bb55d6e9b5ea` = branch HEAD, clean). ONE-mc2 discipline held: zero-count before
every launch, exact-PID kill on timeout, zero-count after (verified each run).

- **Probe A (#50)** — `MC2_LINKUP_TRACE=1 run_smoke --mission mc2_08 --duration 30`
  (worktree runner, `[DEPLOY_FINGERPRINT] OK`): **PASS 1/1**. Trace shows the
  latch precondition exactly as the verdict predicted:
  `[LINKUP] part=513 name=Atlas status=5 onGUI=0 los=0 pos=(661,-363)` (~1/s,
  also auto-parsed into diagnostic JSONL). Artifacts: `tests/smoke/artifacts/2026-07-16T02-55-07/`.
- **Probe B (#34)** — `MC2_TERRAIN_TESS=0 run_smoke --mission mc2_01 --mission mc2_24`:
  **PASS 2/2**. Both missions log
  `[TESS] MC2_TERRAIN_TESS=0 — terrain tessellation material load skipped (kill-switch)`
  AND `gosPostProcess: rendering static shadows (map half-extent=…)` — static
  shadow maps still build with tess off, proving the decouple (pre-fix, the
  txmmgr tess gate would have disabled them). Artifacts: `2026-07-16T02-56-36/`.
- **Probe C (#42a)** — direct lane launch, `MC2_SOAK_AUTOWIN + MC2_SOAK_KILL_ENEMY +
  MC2_SOAK_SALVAGE_TRADEMOVER + MC2_LOG_LOGISTICS_FLOW + MC2_DIAG_TAGS=LOGISTICS`,
  mc2_01: exit 0. Fixture fired through the REAL path:
  `[SOAK] kill-enemy killed=3 first=Urbanmech Prime` →
  `[SOAK] salvage-trademover name=Urbanmech variant=Urbanmech Prime pilot=pmwmeat` →
  `[SOAK] salvage-check items=2` (Phase 2 correctly excludes the now-home-team
  mech). Diagnostic JSONL confirms the traded mech survives
  setMissionCompleted's recovered-mech branch:
  `{site:cull, name:Urbanmech, on_gui:1, cmd_match:1, dead:0, carry_over:1}` and
  `{site:resolve, name:Urbanmech, pilot:Meat, variant:"Urbanmech Prime",
  mech_found:0, pilot_found:1}` with **zero** `mech_lost` events — i.e. the AAR
  half does NOT reproduce for a stock variant on this build; the fixture now
  permanently exercises the suspect branch (getVariant-miss remains the
  mod-variant candidate).
- **Probe D (#43 + #49)** — direct lane launch with options.cfg flipped to
  `FullScreen=FALSE` 1600x900 (backed up + restored), NO `MC2_WINDOWED`, env
  `MC2_ASSET_SCALE_SELFTEST=1 MC2_RES_DIAG=1`: exit 0.
  - #49 residual proven: `[RESOLUTION] Pre-window: 1600x900 fullscreen=0 (from
    options.cfg)` then `[RES_DIAG] window flags: resizable=1 windowed_env=0
    windowed_hint=1` (pre-fix: resizable=0 in this exact config).
  - #43 regression-locked: `[ASSET_SCALE v1] event=selftest_pass
    case=blit_factor_extension`, `selftest_summary passed=5 failed=0`.
- **Tier1 (final gate)** — canonical command via `A:\Games\mc2-main\scripts\run_smoke.py`
  with `--exe` pointed at the lane and `MC2_SMOKE_REQUIRE_FINGERPRINT=0` (the
  documented escape hatch: mc2-main's runner pins the fingerprint to mc2-main
  HEAD, which can never match an external-worktree exe; the same binary was
  fingerprint-VERIFIED by the worktree runner in probes A/B): see verdict below.

**Tier1 verdict: PASS 5/5** (mc2_01 / mc2_03 / mc2_10 / mc2_17 / mc2_24, 30 s
each, exit 0, `[diag-state] PASS`), run 2026-07-16T03-01-35 via
`A:\Games\mc2-main\scripts\run_smoke.py` against the lane exe. The
`[DEPLOY_FINGERPRINT] MISMATCH exe=bb55d6e9 expected=994c3776` lines are the
expected cross-worktree artifact (mc2-main runner pins to mc2-main HEAD);
the same binary was fingerprint-VERIFIED (`OK`) by the worktree runner in
probes A/B. The `[visual-advisory] FAIL_CHANGED generated 2026-06-26` line is
a stale pre-existing advisory record, not from this run.

## Fixer-2 test evidence (rows 8-12, lane exe `ca9ffaa6`, all gates default OFF)

All Fixer-2 runs on the redeployed lane (worktree runner printed
`[DEPLOY_FINGERPRINT] OK: exe sha=ca9ffaa6ba93`). One-mc2 discipline: the
USER's own game (`mc2-win64-0.4c\mc2.exe`, various PIDs) ran concurrently the
whole session — never touched; every probe tracked its OWN exact PID
(Start-Process -PassThru, WaitForExit, exact-PID kill path never needed —
all probes exited by themselves via MC2_SOAK_AUTOWIN or runner timeout).

- **Probe E (#9 A/B)** — direct lane launches (Fixer-1 Probe-C precedent;
  run_smoke hard-caps 30 s, oracle needs ~100 s): mc2_24,
  `MC2_AIMED_FIRE_FIXTURE=1 MC2_SOAK_AUTOWIN=1 MC2_SOAK_WIN_AFTER_SEC=100`,
  `-log weaponfire`, MC2_DIAG_TAGS=+COMBAT; legs identical except
  `MC2_CALLED_SHOT_FIX`. Per-shot ground truth from combat.log:
  - gate OFF: 152 aimed-head shots, **every one clamped at the 5% floor**
    (`chance_dist {5:152}`), hit rate 10/152 (6.6%) — reproduces the issue.
  - gate ON: 77 aimed-head shots at 13-18% (`{13:29, 15:12, 18:36}`), hit
    rate 12/77 (15.6%) — the 0.30 head-modifier floor lifts chances off the
    clamp (base chances on mc2_24 are ~43-60%).
  - JSONL corroboration (`loc` field, home-team dmg>0 hits on ordered
    targets): head share 20.4% → 27.1%; zero-damage cosmetic connects
    31 → 13; missile-cluster-scale (dmg>=4) head hits 5 → 10 (S2).
- **Probe F (#17)** — worktree runner, mc2_24 30 s,
  `MC2_FIRE_DECISION_TRACE=1` + COMBAT tag: **66 `nofire` events** with
  populated tallies (`rd` dominant 66, `rn` 54, `lk` 1; `los:1` on all —
  refusals were recycle/range, NOT LOS) + **16 `firegate` events** (all
  `node_recycle` — the silent per-hotspot ripple-recycle skip the verdict
  predicted). Sample event:
  `{"ev":"nofire","wid":33,"tgt_wid":392,"ord":16,"result":0,"rd":5,"rn":3,"los":1,"dist":117}`.
- **Probe G (#10)** — worktree runner, mc2_16 30 s, `MC2_BRAIN_FREEZE_TRACE=1`:
  **647 extended lines**, all new fields present, e.g.
  `[BRAIN_FREEZE_TRACE] wid=30 ... orderState=0 mainGoal=0 goalWID=0
  genOrder=0 pos=(7445,-4032) lastMoveErr=0`. Artifacts:
  `tests/smoke/artifacts/2026-07-16T04-09-21/`.
- **Gate-off byte-identity**: all four new env vars are static-getenv latches;
  tier1 below ran with every gate unset.

**Fixer-2 tier1 verdict: PASS 5/5** (mc2_01 / mc2_03 / mc2_10 / mc2_17 /
mc2_24, 30 s each, `[diag-state] PASS`), run 2026-07-16T04-11-00 via
`A:\Games\mc2-main\scripts\run_smoke.py --exe <lane>\mc2.exe` with
`MC2_SMOKE_REQUIRE_FINGERPRINT=0` (same documented escape hatch as Fixer-1;
the `[DEPLOY_FINGERPRINT] MISMATCH exe=ca9ffaa6 expected=994c3776` line is the
expected cross-worktree artifact — the same exe was verified `OK` by the
worktree runner in probes F/G). Same stale `[visual-advisory]` record as
Fixer-1's run.

## Fixer-2 deferrals

- #9: gate ships default-OFF; whether to flip default (and/or the exact
  0.30/0.45/0.50 numbers) is an owner call — the A/B harness
  (MC2_AIMED_FIRE_FIXTURE + `-log weaponfire`) makes retuning cheap.
- #9 S2 arrival-path: direct proof is indirect (missile-scale head hits
  doubled; fire-time aiming proven in combat.log; gate-off tier1 clean). A
  dedicated arrival-site trace line was deliberately not added. MP keeps
  stock splash behavior by design (deliverAimedAreaHit returns false under
  MPlayer).
- #10: the verdict's calcTacOrder divert-site traces (turret/gate/wall)
  deferred per dispatch — add once a trace capture shows the divert class is
  live. Actual mc2_15/mc2_16 stall classification (long-form soak) and the
  captured-turret interactive repro remain to be run with the new fields.
- #17: flat-ground reluctance remains NEEDS-REPRO — reporter retest on a
  build >= e7be6175 with `MC2_FIRE_DECISION_TRACE=1` is the next step.
- #32: no byte-level diff of all ~936 patch-touched files; v0.3 zips not
  re-audited (superseded). Both listed as honest unknowns in the doc.

## Deferrals / not verifiable headless

- #42 hover-cursor behavior (info cursor + salvage order on a downed mech):
  interactive click-through per the verdict's test plan step 2 (Ctrl+Alt+Shift+K
  dev hotkey, hover, deploy salvage craft). Not driveable headless.
- #42a full carry-over proof: needs the campaign soak (MC2_BOOT_TO_BAY +
  MC2_SOAK_AUTOWIN across a mission boundary) so Phase 3's load-bay assert fires
  in the NEXT mission; single-mission smoke only proves the fixture fires.
- #50 live E-select repro + reporter confirmation (MC2_LINKUP_TRACE build).
- #49 actual drag-resize + HUD/mouse alignment: interactive.
- #43 MCO-content repro (mods/ content run with MC2_LOG_MECHICON=1) + AAR
  kill-strip capture: the promote-screen capture-settle fix (verdict part B) was
  NOT implemented (out of assigned scope) — noted as follow-up.
- #50 getAppearRadius restore (wider blast radius, also touches issue #17):
  deliberately NOT done per dispatch instructions — follow-up slice.
