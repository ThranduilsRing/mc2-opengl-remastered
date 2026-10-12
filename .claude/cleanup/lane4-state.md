<!-- doc-status: current | reviewed:2026-10-12 -->
# Lane 4 state (harness cull, wave 1; worktree A:/Games/mc2-lane4, branch claude/cull-wave1, tag pre-cull-2026-10-11)

Sessions: 2026-10-11 night (batch 0, cut by the weekly limit) and 2026-10-12 (resumed; nifty merged at 61888cadd; batches 1-4).
Plan: `harness-cull-audit.md` section 6; rulings in `HANDOFF-LOG.md` 2026-10-12 08:28/08:37. Nothing pushed. This lane never builds or
launches the game; its proofs are the board, pytest and doctest. The orchestrator merges with `lane_merge.py <sha>` (lane 1 adds board rows
on nifty; this lane only removed rows, so the merge of `scripts/check-contracts.sh` is additive on both sides).

## Batches done (one commit each, pathspec-staged; every tarball under E:/mc2-archive-2026-10/docs/ with a .sha256 beside it)

| Batch | sha | files | lines deleted | tarball | what |
|---|---|---:|---:|---|---|
| 0a | 619b71ea1 | 44 | 5,981 | cull-wave1-cleanup-stale-2026-10-11.tar.gz | .claude/cleanup stale docs + finished one-shots (pass3-comments, split_mover, warn_table, nonascii_literals, probe/) |
| 0b | 87673f2f7 | 43 | 26,779 | cull-wave1-claude-root-2026-10-11.tar.gz | .claude root arc evidence (STATUS/RECON/patches, user-feedback, maintenance-rules.json) |
| 0c | e0905a2dd | 82 | 13,540 | cull-wave1-docs-2026-10-11.tar.gz | docs/ stale + zero-inbound docs + orphan data |
| 0d | a417a2c47 | 53 | 9,727 | cull-wave1-phase-proofs-2026-10-11.tar.gz | tools/phase/proofs |
| 1 | 933eb80c8 | 14 | 1,031 | cull-wave1-1-2026-10-12.tar.gz | evidence_classifier + oracleparse (+tests), cmake/sdl2, shader_deploy hook (deleted, see below) |
| 2 | f43e8b66b | 75 | 11,260 | cull-wave1-2-2026-10-12.tar.gz + cull-wave1-2b-2026-10-12.tar.gz | 12 manual-only checks + allowlist rows, one-off measurement scripts, zero-caller tools, 2 orphaned docs |
| 3 | 31e7ddab1 | 32 | 6,758 | cull-wave1-3-2026-10-12.tar.gz | golden-parity / sim-identity chain, visual_compare, capture_baseline, smoke_with_screenshots, 3 docs |
| 4 | 29f7ee6b9 | 61 | 17,846 | cull-wave1-4-2026-10-12.tar.gz | gate default-agreement grader, gate_codemod + 14 generated plans, gate_conflicts + ledger + baseline |

Totals over the eight commits (numstat): 92,922 lines deleted, 117 inserted (cross-reference rewrites), 404 files. By tree: docs 26,928;
.claude root 26,780; tools 18,131; scripts 13,533; .claude/cleanup 5,979; golden_floors 822; tests 412; cmake 311; hooks 22.

## Board rows: 170 before (nifty 2dc34f9c9) -> 162 after
Removed: visual_compare, sim_identity_fieldsets, sim_identity_gate_selftest, sim_identity_propagation, sim_identity_propagation_selftest
(batch 3); gate_default_agreement, gate_default_agreement_selftest, gate_reader_conflicts (batch 4). The 12 manual-only checks of batch 2 had
no rows (manual-checks.allowlist only; it now holds check-ab-kit.py and check-worktree-context.sh).

## Proof state at 29f7ee6b9
`bash scripts/check-contracts.sh`: 158/162 green. The 4 reds PREDATE this lane and are untouched by it:
- `gl_shader_variants` + `tools_tests` (scripts/test_gl_shader_variants.py): `scripts/gl_shader_variants.json` is stale against the C++
  after lane 2's gos_postprocess.cpp line moves (a223b807c, merged into nifty before this session); lane 1/2 regenerate with
  `py -3 scripts/extract_gl_shader_variants.py`. Not this lane's file.
- `doc_links`: 4 refs to `docs/beta/README_modder_beta.md`, `docs/beta/known_issues.md`, `docs/micro_scene_spec.md` (README.md:47,
  docs/README.md:13,43, docs/known_issues.md:44) -- those files were already absent at pre-cull-2026-10-11.
- `manifest_schema`: its corpus is the machine-local `tests/smoke/artifacts/*/manifest.json` (gitignored smoke output); this worktree never
  ran a smoke, so it scans 0 and refuses an empty pass. Green in mc2-main.
- Flake seen once (batch-4 board, under the parallel runner): `tests/smoke/test_runner.py::test_run_fault_crash_no_summary`; passes 2/2 alone.
pytest: scripts/tests 591 passed (batch 2 re-run); tests/smoke + tests/visual 78 passed (batch 3); `tools/sim_determinism.py selftest` ok;
`gate_gen --check` consistent; `gate_default_classifier --selftest` 34 cases. Doctest/harness build NOT run: no harness CMakeLists or
tests/unit file changed in any batch (build_contract_harnesses.py is for C++ changes).
Known pre-existing quirk: `py -3 -m pytest scripts tools tests` fails at collection on duplicate basenames (test_manifest.py,
test_visual_diff.py) on the untouched tree too; the board rows scripts_tests/tools_tests are the pytest proof.

## Decisions taken inside the batches (the audit said otherwise)
- `.claude/hooks/shader_deploy.py`: DELETED, not re-pointed. deploy_payload.py deploys the whole shaders/ tree in lockstep with the exe; a
  per-edit copy into one hardcoded install bypassed COMMIT->BUILD->DEPLOY and never fired since the rename. OWNER ACTION: remove the PostToolUse
  entry (`python3 "A:/Games/mc2-main/.claude/hooks/shader_deploy.py"`, lines ~19-23) from the untracked A:/Games/mc2-main/.claude/settings.local.json.
- `tests/visual/baselines/{README.md,presets.json}` moved from batch 1 to batch 3 (capture_baseline.py read them until it went).
- MCP `run_capture_baseline` KEPT: it wraps run_smoke.py, not capture_baseline.py (the audit's "repoint or drop" rested on a wrong caller).
- `gate_reader_conflicts`: no assertion folded into `gate_gen --check`; the cxx17 ratchet's LG19 idiom already pins raw MC2_ getenv reads
  (baseline sum 3), so the fold would duplicate it.
- C++ comments that still name deleted scripts were left (this lane does not touch C++): predict_cone_pools.py (bdactor_update.cpp:286,
  cone_glow.cpp:166), terrain_height_audit.py (mapdata.cpp:2435), check-order-liveness.py (mission_update.cpp:161), check-ambient-guard.py
  (render_contract.cpp:985), check-file-resolve-miss.py (file.cpp:969), scenario_runner (missiongui_uicmd*.cpp), debug_view_sweep.py
  (gameos_graphics_material_internal.h:799, missiongui_update.cpp:99), check-gate-default-agreement.py (5 sites). Comment pass (ruling 6).
- `scripts/run_smoke.py` VISUAL advisory block (lines ~1712-1740) reads `tests/visual/compare/visual_compare_report.json`, which nothing
  produces now; harmless, left so no batch touched a runner. Wave 2.

## Skipped (kept), with the reason each
- AB-kit / review-pack / user-feedback chain (batch 5): owner ruling SKIP this wave. (`.claude/user-feedback` went with batch 0b before the
  ruling; it is in cull-wave1-claude-root-2026-10-11.tar.gz -- restore from the tarball or git if the loop is revived.)
- VK binding/pipeline ledgers (13 rows): KEEP until the Vulkan default flip (ruling).
- ~35 render-contract grep rows: KEEP as the cheap floor (ruling).
- Cook/content tools incl. tools/examples + tests/examples, classify_materials.py, validate_glb.py, asset_baseline.py, docs/baseline-flea.json,
  scripts/bt2018_retarget_clips.py (imported by tools/bt2018_import/*): KEEP, never delete (ruling 2026-10-12 morning).
- tools/mc2mod + tests/mc2mod: the modder packager that docs/modding/examples/* document 28 times; mod stack KEEP-parked (ruling). The prompt
  listed it under ruling 2026-10-09 #5, but it is not a measurement one-off; verify_mod_roundtrip.py (its only harness helper) was deleted.
- scripts/verify_campaign_assets.py: user-facing post-import check in docs/STARTER-GUIDE.md (campaign importer = KEEP).
- scripts/run_brain_apply_ab.ps1: the brain_dispatch_harness README names it as the runner (brain harness = KEEP).
- tools/dev/apply_edit.py + test: built by lane 1 on 2026-10-11 (f471bcb04), named in lane1-state.md. Live.
- tools/comment_guard_needles.txt: read by tools/comment_guard.py (EXTERNAL_NEEDLES_REL) and tested. Live.
- scripts/gate_default_classifier.py + scripts/gate_idioms.py + row gate_classifier_selftest: scripts/check-delivery.py imports the
  classifier (line 276). Deleting them = rework check-delivery to read Gates/gate_table.json (wave 2, not a cull).
- tools/gate_table.seed.json shrink: NOT done. gate_table_gen.py reads defaultDisagreement/conflict (line 345) and gl_vk_feature_coverage.py
  reads default/registryKind (lines 62-67, 443-452); both need more than the name. Lane 2's amnesty edits the file too.
- `.claude/VULKAN-NEXT-SLICES.md` still narrates run_golden_parity's backend-compare (interop era); one backticked path was rewritten, the
  rest is history. GL-vs-VK parity is tools/visual_verify/ab.py.
- Not run (needs a game launch): the audit's batch-3 proof `capture_ab.py` on mc2_01_saved_205644 + mc2_10_saved_204523, viewed.

## Wave 2 (exact batches, in order; each provable with the board + pytest)
1. Merges with tests: raw-GL rows (6) -> `check-no-raw-gl-from-game.sh`; `contract_coverage`+`test_tier_coverage` (+selftests) -> one pytest
   in scripts/tests; `check-disk-space.py` -> `deploy_payload.py` (shutil.disk_usage + threshold) and drop row disk_guard_selftest.
2. check-delivery.py reads gate defaults from Gates/gate_table.json; then delete gate_default_classifier.py, gate_idioms.py, row
   gate_classifier_selftest (and the test_gate.cpp comment that cites the probe order).
3. 16 registered C++ contract harnesses -> doctest cases in tests/unit (one binary); delete 18 per-harness CMakeLists,
   contract_harness_common, run_contract_tests.py. C++ lane; proof `build_contract_harnesses.py --run --unit`.
4. Board reshaping: every `--selftest`/`--self-test` row -> pytest in scripts/tests (49 rows -> scripts_tests); prove 3 by injected defects.
5. run_smoke.py VISUAL advisory block removal; `tests/unit/printf_corpus*` after C++23 step 3 closes; `mlr_leaves_gated` /
   `unified_projection_retirement` / `particles_no_cpu_projection` with MLR retirement; VK ledgers -> validation layers at the flip;
   AB-kit chain if the owner re-rules; MCP `binding_index`/`commit_plan` (audit §3) if the owner confirms.
6. Auditor-requested tooling (cheap): `check-contracts.sh --list`; a tracked callers.json regenerated by a board row (this lane's census
   script is the prototype: grep every basename AND stem, never pipe through `head` -- a SIGPIPE truncated one census and hid the
   test_strict_runner_lazy.py caller until the board found it).
