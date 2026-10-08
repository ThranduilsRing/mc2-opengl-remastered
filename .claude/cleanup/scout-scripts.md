# Scout: scripts/ and tools/ (top level + scripts/smoke_lib) classification

Scope: `git ls-files` of `scripts/*`, `scripts/smoke_lib/*`, `tools/*` (top level only). 305 files. Read-only scout; nothing edited, built, or run.

## Counts

| class | count |
|---|---|
| WIRED | 227 |
| REFERENCED | 44 |
| XREF-ONLY | 12 |
| ORPHAN | 22 |
| multi-lane flagged (any class) | 59 |

## Method and caveats

- **WIRED**: BFS from the roots `scripts/check-contracts.sh`, `scripts/run_smoke.py`, `scripts/deploy_payload.py`, `tools/build_contract_harnesses.py`, and every CMake file in the repo (basename mention), then transitively through any mention or python import from a wired file. This is generous: a basename mention in a wired file counts, including mentions in comments that were not invocations. Caveats found by hand: `check-flag-registry-drift.sh` is only mentioned in comments and in `manual-checks.allowlist` (documented as advisory, not run), so it was moved to REFERENCED. `check-contracts.sh` runs `pytest scripts/tests/` only, so top-level `scripts/test_*.py` files are not run by any gate.
- **REFERENCED**: not wired, but the basename appears in a live doc: `CLAUDE.md`, `AGENTS.md`, `docs/**`, `.claude/**`, a README, or the allowlists. Most of these are historical recon or status notes, so a doc mention does not mean the script is used.
- **XREF-ONLY** (extra class, not in the spec): not wired, not in any doc, but referenced by other code or tests outside the wired set. Delete only together with the referrer.
- **ORPHAN**: no tracked file other than itself mentions the basename. Grep covered tracked files only. Untracked files (much of `.claude/` is untracked) were not searched.
- **multi-lane?**: heuristic. Yes if the basename matches lane/lease/steer/worktree/hardlink/foreign/new_lane/shared index/coordinat/isolat, or if the body has 5+ hits for lane/lease/steer/worktree/hardlink/foreign-process/shared index/lane parity/register_lane. Expect some false positives on "isolation" and "lanes" wording in comments.
- **lines**: `wc -l`-equivalent line count.
- **referenced-by**: first two referrers (tracked files, self excluded).

## Multi-lane infrastructure to retire (wired ones need code changes, not just deletion)

Wired and multi-lane: `scripts/new_lane.py` (lane creation, 129 hits), `scripts/check-deploy-lane-parity.py`, `scripts/check-hardlink-isolation.py`, `scripts/check-worktree-context.sh`, `scripts/check-json-isolation.sh`, `scripts/smoke_lib/deploy_lease.py`, `scripts/smoke_lib/isolation.py`, `scripts/run_smoke.py` (lane-lock and lease code inside the core smoke runner), `scripts/make-isolated-deploy.py`, `scripts/check-claude-md-pointer.sh`, `scripts/check-capture-determinism.py`. Referenced but multi-lane: `tools/spawn_lane.py`, `tools/test_spawn_lane.py`, `scripts/parse-done-reports.py`, `scripts/bootstrap_worktree_build.py`. Orphan and multi-lane: `scripts/test_deploy_lease.py`, `scripts/test_smoke_lane_lock.py`. Both test wired code, so they are not safe to delete on their own. See the table for the full flag set.

## SAFE TO DELETE

ORPHAN and not multi-lane, and not coupled to wired code (no tracked referrer of any kind):

- `tools/data_ship_census.py` (528 lines)
- `scripts/residency_slim_report.py` (435 lines)
- `scripts/pull_compat_pilot_roster.py` (374 lines)
- `scripts/gltf_fk_oracle.py` (310 lines)
- `scripts/cook_upscaled_assets_mod.py` (301 lines)
- `scripts/slice_gate.py` (299 lines)
- `scripts/build_asset_thumbnails.py` (276 lines)
- `scripts/visual_gallery.py` (249 lines)
- `scripts/nightly_evidence_2_phase2b_3.py` (193 lines)
- `tools/place_marblecliff_mc2_01.py` (162 lines)
- `scripts/nightly_evidence_2.py` (152 lines)
- `tools/place_coastalcliff_mc2_01.py` (147 lines)
- `scripts/run_closeout_tests.bat` (136 lines)
- `tools/blender_bake_coastal_cliff.py` (126 lines)
- `scripts/bt2018_retarget_clips.py` (115 lines)
- `scripts/test_smoke_guard.py` (92 lines)
- `tools/extract_veg_cards.py` (88 lines)
- `scripts/run_vk_nointerop_strict_smoke.py` (71 lines)
- `scripts/cook_magic_icons.py` (57 lines)
- `scripts/run_smoke_local.py` (20 lines)

ORPHAN but multi-lane (not in the safe list above, decide with lane retirement): `scripts/test_deploy_lease.py`, `scripts/test_smoke_lane_lock.py`.

XREF-ONLY groups (delete together, after checking the referrer is also dead): `tools/install_tree_scatter.py` + `tools/pinescatter{1,2,3}_fit_template.txt` (referrer is `tools/terrain_beautify/scatter_tool.py`, outside scope), `tools/install_cliff_dressing.py` + `tools/marblecliff*_fit_template.txt` + `tools/place_marblecliff_mc2_01.py` (ORPHAN), `tools/condition_coastal_cliff_glb.py` + `tools/blender_cmd.py` + `tools/blender_bake_coastal_cliff.py` (ORPHAN), `tools/residency_slim.py` (referenced by `mclib/txmmgr.cpp`, so keep unless that is removed), `scripts/terrain_grounding_audit.py` (its referrer `scripts/terrain_height_audit.py` is REFERENCED), `scripts/whatran.py` (referenced by a test baseline JSON only).

## Biggest ORPHAN files by line count

1. `tools/data_ship_census.py`: 528 lines
2. `scripts/residency_slim_report.py`: 435 lines
3. `scripts/test_deploy_lease.py`: 380 lines (multi-lane)
4. `scripts/pull_compat_pilot_roster.py`: 374 lines
5. `scripts/gltf_fk_oracle.py`: 310 lines
6. `scripts/cook_upscaled_assets_mod.py`: 301 lines
7. `scripts/slice_gate.py`: 299 lines
8. `scripts/build_asset_thumbnails.py`: 276 lines
9. `scripts/visual_gallery.py`: 249 lines
10. `scripts/nightly_evidence_2_phase2b_3.py`: 193 lines
11. `tools/place_marblecliff_mc2_01.py`: 162 lines
12. `scripts/nightly_evidence_2.py`: 152 lines
13. `tools/place_coastalcliff_mc2_01.py`: 147 lines
14. `scripts/run_closeout_tests.bat`: 136 lines
15. `tools/blender_bake_coastal_cliff.py`: 126 lines

## Full table

| path | class | multi-lane? | lines | referenced-by (first 2) |
|---|---|---|---|---|
| `scripts/bake_colormap_ktx2.py` | WIRED | no | 187 | `RenderCore/RendererFeatureRegistry.h`, `docs/terrain-colormap-modernization-debt.md` |
| `scripts/build_release.sh` | WIRED | YES | 348 | `docs/RELEASE-PROCESS.md`, `docs/capture-evidence-retention.md` |
| `scripts/build_review_pack.py` | WIRED | YES | 925 | `docs/review-feedback-schema.md`, `release_assets_src/annotate/review_annotate.css` |
| `scripts/camera_sweep_smoke.py` | WIRED | no | 206 | `docs/background-launch-inventory.md`, `docs/superpowers/strategy/visual-regression-lab-architecture.md` |
| `scripts/capture_baseline.py` | WIRED | no | 576 | `docs/background-launch-inventory.md`, `docs/ibl-plan.md` |
| `scripts/check-ab-kit.py` | WIRED | no | 245 | `scripts/check-deploy-coherence.py`, `scripts/kit-digests.csv` |
| `scripts/check-agents-md.sh` | WIRED | no | 27 | `scripts/check-contracts.sh` |
| `scripts/check-ambient-guard.py` | WIRED | no | 72 | `docs/render-backend-seams/executor-island-recon-1.md`, `docs/render-backend-seams/shadow-observe-2-recon-1.md` |
| `scripts/check-apply-pass-bumped.py` | WIRED | no | 163 | `scripts/check-contracts.sh` |
| `scripts/check-asset-manifest-fixtures.py` | WIRED | no | 91 | `docs/asset-manifest-schema.md`, `docs/trackv-status.md` |
| `scripts/check-asset-manifests.py` | WIRED | no | 66 | `docs/asset-manifest-schema.md`, `docs/model-override-system-recon.md` |
| `scripts/check-asset-pipeline-doc.sh` | WIRED | no | 53 | `docs/asset-pipeline.md`, `scripts/check-contracts.sh` |
| `scripts/check-asset-scale-callers.sh` | WIRED | no | 50 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/check-background-launch.py` | WIRED | no | 554 | `.claude/DESKTOP-ISOLATION-WIRING-1.patch`, `GameOS/gameos/gameos_input.cpp` |
| `scripts/check-beauty-sidecars.py` | WIRED | YES | 764 | `docs/beauty-sidecar-inventory.txt`, `docs/data-provenance.md` |
| `scripts/check-binding-slots.py` | WIRED | no | 364 | `docs/render-backend-seams/binding-slot-occupancy.md`, `docs/render-backend-seams/gpu-buffer-owner-recon-1.md` |
| `scripts/check-brain-fit-schema.py` | WIRED | no | 653 | `docs/render-backend-seams/SCOPE-BRAIN-FIT-SCHEMA-1.md`, `docs/render-backend-seams/brain-arc-implementation-roadmap.md` |
| `scripts/check-brain-opord.py` | WIRED | no | 151 | `scripts/manual-checks.allowlist` |
| `scripts/check-buffer-lifetime-ownership.py` | WIRED | no | 92 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `docs/render-backend-seams/buffer-lifetime-ledger.md` |
| `scripts/check-capture-determinism.py` | WIRED | YES | 364 | `docs/visual-verify.md`, `scripts/check-contracts.sh` |
| `scripts/check-capture-env-declared.py` | WIRED | no | 201 | `scripts/check-contracts.sh` |
| `scripts/check-capture-provenance.py` | WIRED | YES | 1160 | `docs/visual-verify.md`, `scripts/check-contracts.sh` |
| `scripts/check-capture-resolution.py` | WIRED | no | 211 | `scripts/check-contracts.sh`, `scripts/run_visual_capture.py` |
| `scripts/check-claude-md-pointer.sh` | WIRED | YES | 102 | `docs/engine-closure-audit.md`, `docs/superpowers/strategy/ai-agent-operating-manual.md` |
| `scripts/check-colormask-ownership.py` | WIRED | no | 101 | `GameOS/gameos/pipeline_binder.cpp`, `RenderCore/ambient_contract.h` |
| `scripts/check-combat-soak.py` | WIRED | no | 146 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-contract-coverage.py` | WIRED | YES | 930 | `docs/disciplines.md`, `scripts/check-contracts.sh` |
| `scripts/check-contracts.sh` | WIRED | YES | 1476 | `.claude/CEMENT-PATCHY-CAUSE-1-STATUS.md`, `.claude/FIXTURE-GLOW-1-STATUS.md` |
| `scripts/check-counter-honesty.py` | WIRED | no | 645 | `scripts/check-contracts.sh`, `scripts/counter-honesty-allowlist.txt` |
| `scripts/check-cpu-gpu-cogating.py` | WIRED | no | 350 | `RenderCore/RendererFeatureRegistry.h`, `docs/plans/2026-04-10-cpu-terrain-displacement-design.md` |
| `scripts/check-debug-state-json.py` | WIRED | no | 225 | `docs/debug_state_schema.md`, `docs/mcp-render-state.md` |
| `scripts/check-default-build-quiet.py` | WIRED | no | 214 | `RenderCore/RendererFeatureRegistry.h`, `docs/tier1_env_vars.md` |
| `scripts/check-delivery.py` | WIRED | YES | 1952 | `.claude/user-feedback/QUEUE-2026-08-01.md`, `GameOS/gameos/gos_terrain_lod_chunk.cpp` |
| `scripts/check-deploy-coherence.py` | WIRED | YES | 447 | `.claude/skills/mc2-deploy.md`, `cmake/GenerateBuildFingerprint.cmake` |
| `scripts/check-deploy-data-parity.py` | WIRED | YES | 538 | `scripts/check-contracts.sh` |
| `scripts/check-deploy-hygiene.py` | WIRED | no | 132 | `scripts/check-beauty-sidecars.py`, `scripts/check-contract-coverage.py` |
| `scripts/check-deploy-lane-parity.py` | WIRED | YES | 245 | `scripts/check-beauty-sidecars.py`, `scripts/check-contracts.sh` |
| `scripts/check-deploy-target.py` | WIRED | YES | 500 | `docs/dev-velocity.md`, `scripts/check-beauty-sidecars.py` |
| `scripts/check-destroy-invariant.sh` | WIRED | no | 41 | `docs/engine-closure-audit.md`, `docs/tier1_env_vars.md` |
| `scripts/check-disk-space.py` | WIRED | YES | 491 | `docs/capture-evidence-retention.md`, `docs/deploy-isolation.md` |
| `scripts/check-doc-links.py` | WIRED | no | 190 | `.claude/ISSUE-FIXES-STATUS.md`, `scripts/check-contracts.sh` |
| `scripts/check-drawbuffer-ownership.py` | WIRED | no | 126 | `RenderCore/PipelineDesc.h`, `RenderCore/ambient_contract.h` |
| `scripts/check-editor-build.sh` | WIRED | no | 125 | `docs/editor-debug-overlay-phase2.md`, `docs/known_issues.md` |
| `scripts/check-editor-game-render-context-parity.py` | WIRED | no | 154 | `docs/render-backend-seams/editor-game-parity-arc.md`, `scripts/check-contracts.sh` |
| `scripts/check-editor-pass-contract-parity.py` | WIRED | no | 197 | `docs/render-backend-seams/editor-game-parity-arc.md`, `docs/render-backend-seams/editor-pass-contract-parity.json` |
| `scripts/check-env-allowlist.sh` | WIRED | no | 146 | `.claude/VK-ARC-STATE.md`, `docs/tier1_env_vars.md` |
| `scripts/check-env-drive-guard.py` | WIRED | no | 190 | `scripts/check-contracts.sh`, `tools/repo_intel/gate_status.py` |
| `scripts/check-env-registry.sh` | WIRED | YES | 1171 | `.claude/CEMENT-PATCHY-CAUSE-1-STATUS.md`, `.claude/CONE-SPILL-GRADE-1-STATUS.md` |
| `scripts/check-file-resolve-miss.py` | WIRED | no | 174 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/check-flag-registry-truth.py` | WIRED | YES | 290 | `scripts/check-contracts.sh`, `scripts/flag-registry-auto-gen.py` |
| `scripts/check-frame-graph.py` | WIRED | no | 101 | `scripts/check-contract-coverage.py`, `scripts/check-test-tier-coverage.py` |
| `scripts/check-frame-resource-feedback.py` | WIRED | no | 92 | `docs/render-backend-seams/render-pass-attachment-contract-recon-1.md`, `scripts/check-contracts.sh` |
| `scripts/check-gate-default-agreement.py` | WIRED | YES | 1540 | `GameOS/gameos/gos_static_prop_batcher.cpp`, `GameOS/gameos/gos_terrain_lod_chunk.cpp` |
| `scripts/check-gated-layout-macros.allowlist` | WIRED | no | 12 | `docs/render-backend-seams/gated-layout-macro-guard.md`, `scripts/check-gated-layout-macros.py` |
| `scripts/check-gated-layout-macros.py` | WIRED | no | 559 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/VULKAN-NEXT-SLICES.md` |
| `scripts/check-golden-coverage.py` | WIRED | no | 553 | `.claude/GOLDEN-v051-v052-RETIREMENT-CASE.md`, `scripts/check-contracts.sh` |
| `scripts/check-gpu-buffer-owners.py` | WIRED | no | 556 | `RenderCore/GpuBuffer.h`, `docs/testing/golden-scene-manifest.md` |
| `scripts/check-hardlink-isolation.py` | WIRED | YES | 645 | `docs/deploy-isolation.md`, `scripts/check-contracts.sh` |
| `scripts/check-icon-atlases.py` | WIRED | no | 164 | `scripts/manual-checks.allowlist` |
| `scripts/check-include-firewall.allowlist` | WIRED | no | 27 | `.claude/skills/mc2-render-spine-advisor.md`, `EditorBridge/CMakeLists.txt` |
| `scripts/check-include-firewall.sh` | WIRED | no | 168 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/skills/mc2-render-spine-advisor.md` |
| `scripts/check-input-capture-visibility.py` | WIRED | no | 581 | `scripts/check-contract-coverage.py`, `scripts/check-contracts.sh` |
| `scripts/check-json-isolation.sh` | WIRED | YES | 42 | `docs/model-override-mvp-plan.md`, `docs/plans/2026-04-25-mod-profile-launcher-plan-1-foundation.md` |
| `scripts/check-ktx2-probe.py` | WIRED | no | 38 | `scripts/manual-checks.allowlist` |
| `scripts/check-launcher-ascii.py` | WIRED | no | 152 | `scripts/check-contracts.sh`, `tools/mc2_launcher/mc2_launcher.cpp` |
| `scripts/check-light-abi-lockstep.py` | WIRED | no | 131 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `GameOS/gameos/gameos_graphics_light_ssbo.cpp` |
| `scripts/check-light-loop-numlights-bound.py` | WIRED | no | 192 | `docs/active_campaigns.md`, `docs/lighting-divergence-map.md` |
| `scripts/check-light-row-freshness.py` | WIRED | no | 258 | `scripts/check-contracts.sh` |
| `scripts/check-lighting-divergence.py` | WIRED | no | 467 | `RenderCore/RendererFeatureRegistry.h`, `docs/lighting-divergence-map.md` |
| `scripts/check-manifest-schema.py` | WIRED | YES | 162 | `docs/release-checklist.md`, `docs/superpowers/strategy/baseline-a-capture-runbook.md` |
| `scripts/check-material-gpu-mirror.sh` | WIRED | no | 172 | `GameOS/gameos/gos_static_prop_batcher.h`, `RenderCore/MaterialGpu.h` |
| `scripts/check-mdi-submission-ownership.py` | WIRED | no | 76 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `GameOS/gameos/mdi_submit.h` |
| `scripts/check-memory-size.py` | WIRED | no | 95 | `scripts/check-claude-md-pointer.sh`, `scripts/check-contract-coverage.py` |
| `scripts/check-mlr-leaves-gated.sh` | WIRED | no | 49 | `docs/engine-closure-audit.md`, `docs/render-backend-seams/render-contract-index-1.md` |
| `scripts/check-mover-registry-coherence.sh` | WIRED | no | 403 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-multiply-blend.py` | WIRED | no | 81 | `docs/render-backend-seams/pipeline-pass-coverage-ledger.json`, `scripts/check-contracts.sh` |
| `scripts/check-new-gates.py` | WIRED | no | 166 | `docs/render-backend-seams/render-contract-index-1.md`, `scripts/check-contract-coverage.py` |
| `scripts/check-no-raw-gl-from-game.allowlist` | WIRED | no | 26 | `docs/render-api-boundary-recon.md`, `docs/renderworld_arc_status.md` |
| `scripts/check-no-raw-gl-from-game.sh` | WIRED | no | 137 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/skills/mc2-render-spine-advisor.md` |
| `scripts/check-object-mvp-currency.py` | WIRED | no | 163 | `GameOS/gameos/gameos_graphics.cpp`, `GameOS/gameos/gos_mech_batcher.cpp` |
| `scripts/check-oracle-registry.py` | WIRED | YES | 238 | `docs/superpowers/strategy/superpowers-execution-roadmap.md`, `docs/superpowers/strategy/telemetry-oracle-cockpit-architecture.md` |
| `scripts/check-order-gate-coverage.py` | WIRED | no | 270 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-order-liveness.py` | WIRED | no | 374 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-particles-no-cpu-projection.sh` | WIRED | no | 57 | `docs/engine-closure-audit.md`, `docs/render-backend-seams/render-contract-index-1.md` |
| `scripts/check-pass-attachment-contract.py` | WIRED | no | 122 | `GameOS/gameos/render_pass_attachment.h`, `scripts/check-contracts.sh` |
| `scripts/check-pass-coverage.py` | WIRED | no | 222 | `docs/render-backend-seams/HANDOFF-WATER-VISUAL-GATE-2.md`, `docs/render-backend-seams/pipeline-pass-coverage-ledger.json` |
| `scripts/check-path-normalization.sh` | WIRED | no | 206 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-pipeline-desc.py` | WIRED | no | 291 | `docs/render-backend-seams/HANDOFF-WATER-VISUAL-GATE-2.md`, `docs/render-backend-seams/pipeline-state-contract-recon-1.md` |
| `scripts/check-pipeline-key.py` | WIRED | no | 352 | `RenderCore/PipelineRegistry.cpp`, `RenderCore/PipelineRegistry.h` |
| `scripts/check-raw-gl-blendfunc.py` | WIRED | no | 294 | `docs/render-backend-seams/render-contract-index-1.md`, `scripts/check-contracts.sh` |
| `scripts/check-raw-gl-capstone.py` | WIRED | no | 212 | `docs/render-backend-seams/render-contract-index-1.md`, `scripts/check-contracts.sh` |
| `scripts/check-raw-gl-colormask.py` | WIRED | no | 284 | `docs/render-backend-seams/render-contract-index-1.md`, `scripts/check-contracts.sh` |
| `scripts/check-raw-gl-depthfunc.py` | WIRED | no | 270 | `docs/render-backend-seams/render-contract-index-1.md`, `scripts/check-apply-pass-bumped.py` |
| `scripts/check-raw-gl-depthmask.py` | WIRED | no | 272 | `docs/render-backend-seams/render-contract-index-1.md`, `scripts/check-contracts.sh` |
| `scripts/check-raw-gl-fbobind.py` | WIRED | no | 303 | `docs/render-backend-seams/render-contract-index-1.md`, `scripts/check-contracts.sh` |
| `scripts/check-registered-gate-passthrough.py` | WIRED | YES | 272 | `scripts/check-contracts.sh` |
| `scripts/check-render-contract-alpha-classes.py` | WIRED | no | 464 | `scripts/check-contracts.sh`, `shaders/include/render_contract.hglsl` |
| `scripts/check-render-contract-gbuffer1.allowlist` | WIRED | no | 124 | `scripts/check-render-contract-gbuffer1.sh`, `shaders/vulkan/color_prop_tier1.frag` |
| `scripts/check-render-contract-gbuffer1.sh` | WIRED | no | 365 | `.claude/LIGHTING-HEADROOM-AUDIT.md`, `.claude/agents/mc2-render-contract-synthesizer.md` |
| `scripts/check-render-resource-ids.py` | WIRED | no | 137 | `docs/testing/golden-scene-manifest.md`, `scripts/check-contracts.sh` |
| `scripts/check-res-aspect-contract.py` | WIRED | no | 349 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-review-anchor-stability.py` | WIRED | YES | 460 | `docs/review-feedback-schema.md`, `scripts/check-contracts.sh` |
| `scripts/check-sampler-bindings-selftest.py` | WIRED | no | 171 | `scripts/check-contracts.sh` |
| `scripts/check-sampler-bindings.py` | WIRED | no | 591 | `docs/render-backend-seams/frame-graph-tex-latch-recon-1.md`, `docs/render-backend-seams/gpu-texture-resource-manager-seam-recon-1.md` |
| `scripts/check-shader-injectors.py` | WIRED | no | 188 | `docs/render-backend-seams/shader-permutation-inventory.json`, `docs/render-backend-seams/shader-permutation-inventory.md` |
| `scripts/check-shader-package.py` | WIRED | no | 137 | `docs/render-backend-seams/shader-artifact-package-metadata-1.md`, `scripts/check-contracts.sh` |
| `scripts/check-shader-schema.sh` | WIRED | no | 26 | `CMakeLists.txt`, `docs/engine-closure-audit.md` |
| `scripts/check-shader-variant.py` | WIRED | no | 90 | `docs/render-backend-seams/shader-variant-lockstep-1.md`, `docs/render-backend-seams/shader-variant-ownership-recon-1.md` |
| `scripts/check-sim-identity-fieldsets.py` | WIRED | no | 181 | `scripts/check-contracts.sh`, `scripts/check-sim-identity-propagation.py` |
| `scripts/check-sim-identity-propagation.py` | WIRED | no | 422 | `scripts/check-contracts.sh`, `scripts/run_golden_parity.py` |
| `scripts/check-smoke-env-inheritance.py` | WIRED | no | 258 | `scripts/check-contracts.sh`, `scripts/mc2_env_pin.py` |
| `scripts/check-smoke-fingerprint-honesty.py` | WIRED | YES | 435 | `scripts/check-contracts.sh`, `scripts/run_smoke.py` |
| `scripts/check-smoke-matrices.py` | WIRED | no | 202 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-spirv-artifacts.py` | WIRED | no | 150 | `docs/render-backend-seams/offline-shader-variant-build-1.md`, `docs/render-backend-seams/shader-variant-ownership-recon-1.md` |
| `scripts/check-spirv-reflection-contract.py` | WIRED | no | 193 | `docs/render-backend-seams/shader-variant-ownership-recon-1.md`, `scripts/check-contracts.sh` |
| `scripts/check-split-path-contracts.py` | WIRED | no | 791 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-static-prop-family.py` | WIRED | no | 78 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `docs/render-backend-seams/static-prop-family-ledger.md` |
| `scripts/check-stock-mission-data.py` | WIRED | YES | 993 | `docs/data-provenance.md`, `scripts/check-contracts.sh` |
| `scripts/check-terrain-daylight-constants.py` | WIRED | no | 135 | `GameOS/gameos/utils/terrain_daylight.h`, `scripts/check-contracts.sh` |
| `scripts/check-test-tier-coverage.py` | WIRED | no | 362 | `scripts/check-contracts.sh`, `scripts/run-unit-tests.sh` |
| `scripts/check-texture-owners.py` | WIRED | no | 209 | `scripts/check-contracts.sh` |
| `scripts/check-texture-resolution.py` | WIRED | no | 399 | `scripts/manual-checks.allowlist` |
| `scripts/check-tgl-load-callers.sh` | WIRED | no | 237 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-DESIGNS-2026-07-16.json` |
| `scripts/check-toolchain-bom.py` | WIRED | no | 118 | `docs/asset-manifest-schema.md`, `docs/toolchain-bom.md` |
| `scripts/check-unified-projection-retirement.allowlist` | WIRED | no | 25 | `scripts/check-unified-projection-retirement.sh` |
| `scripts/check-unified-projection-retirement.sh` | WIRED | no | 136 | `docs/engine-closure-audit.md`, `docs/observations/2026-05-22-unified-proj-stage-a-shipped.md` |
| `scripts/check-vfx-blend-distinction.py` | WIRED | no | 97 | `docs/VFX-BLACKBODY-1.md`, `docs/VFX-DISTORTION-1.md` |
| `scripts/check-vfx-no-objectid.allowlist` | WIRED | no | 13 | `.claude/skills/mc2-render-spine-advisor.md`, `docs/renderworld_arc_status.md` |
| `scripts/check-vfx-no-objectid.sh` | WIRED | no | 143 | `.claude/skills/mc2-render-spine-advisor.md`, `RenderWorld/RenderWorld.cpp` |
| `scripts/check-visibility-log-schema.sh` | WIRED | no | 58 | `docs/engine-closure-audit.md`, `docs/render-backend-seams/render-contract-index-1.md` |
| `scripts/check-visual-bookmark-honesty.py` | WIRED | YES | 313 | `.claude/PAVED-OVERLAY-CLASS-1-STATUS.md`, `scripts/check-contracts.sh` |
| `scripts/check-vulkan-bindings.allowlist.json` | WIRED | no | 41 | `scripts/check-vulkan-bindings.py` |
| `scripts/check-vulkan-bindings.py` | WIRED | no | 360 | `.claude/VULKAN-NEXT-SLICES.md`, `docs/render-backend-seams/binding-slot-occupancy.md` |
| `scripts/check-worktree-context.sh` | WIRED | YES | 110 | `docs/worktree-discipline.md`, `scripts/check-contract-coverage.py` |
| `scripts/check_brain_fixture_deploy_sync.py` | WIRED | YES | 143 | `docs/render-backend-seams/brain-fsm-1k-a.md`, `scripts/check-contracts.sh` |
| `scripts/check_brain_fixture_manifest.py` | WIRED | no | 126 | `docs/render-backend-seams/brain-fsm-1k-a.md`, `scripts/check-contracts.sh` |
| `scripts/check_brain_relaxed_guard_doc.py` | WIRED | no | 288 | `.claude/TECHSCRIPT-GAP-CLOSURE-1.md`, `code/brain_special_dispatch.cpp` |
| `scripts/check_shader_bom.py` | WIRED | no | 79 | `GameOS/gameos/utils/shader_builder.cpp`, `scripts/run_smoke.py` |
| `scripts/check_vk_nointerop_log.py` | WIRED | no | 490 | `.claude/VK-ARC-STATE.md`, `.claude/VK-RUNG8-LADDER-1.md` |
| `scripts/checklib.py` | WIRED | no | 338 | `.claude/ISSUE-FIXES-STATUS.md`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/counter-honesty-allowlist.txt` | WIRED | no | 139 | `scripts/check-contracts.sh`, `scripts/check-counter-honesty.py` |
| `scripts/counter-honesty-population-inventory.txt` | WIRED | no | 38 | `scripts/check-contracts.sh`, `scripts/check-counter-honesty.py` |
| `scripts/delivery-fixes.json` | WIRED | no | 169 | `scripts/check-contracts.sh`, `scripts/check-delivery.py` |
| `scripts/deploy-editor.sh` | WIRED | no | 117 | `.claude/skills/mc2-deploy.md`, `scripts/check-editor-build.sh` |
| `scripts/deploy_payload.py` | WIRED | YES | 1404 | `.claude/LIGHTING-HEADROOM-AUDIT.md`, `.claude/MEASUREMENT-LEDGER.md` |
| `scripts/exe_fingerprint.py` | WIRED | YES | 347 | `.claude/MEASUREMENT-LEDGER.md`, `scripts/check-capture-provenance.py` |
| `scripts/file-resolve-miss-baseline.txt` | WIRED | no | 585 | `.claude/ISSUE-FIXES-STATUS.md`, `docs/tier1_env_vars.md` |
| `scripts/flag-registry-auto-gen.py` | WIRED | YES | 395 | `docs/flags.auto.yaml`, `mclib/cone_glow.cpp` |
| `scripts/game_auto.py` | WIRED | no | 452 | `docs/background-launch-inventory.md`, `docs/game-auto-tools.md` |
| `scripts/gate-defaults-allowlist.txt` | WIRED | no | 35 | `scripts/check-contracts.sh`, `scripts/check-gate-default-agreement.py` |
| `scripts/golden_compare.py` | WIRED | no | 490 | `docs/testing/golden-scene-parity.md`, `docs/tier1_env_vars.md` |
| `scripts/golden_diff_report.py` | WIRED | no | 351 | `docs/testing/golden-scene-parity.md`, `scripts/run_golden_parity.py` |
| `scripts/golden_scene.py` | WIRED | no | 263 | `docs/render-backend-seams/vulkan-island-template.md`, `docs/testing/golden-scene-manifest.md` |
| `scripts/kit-digests.csv` | WIRED | no | 28 | `scripts/check-deploy-coherence.py`, `scripts/manual-checks.allowlist` |
| `scripts/make-isolated-deploy.py` | WIRED | YES | 618 | `docs/deploy-isolation.md`, `scripts/check-contracts.sh` |
| `scripts/manifest_schema.py` | WIRED | YES | 587 | `docs/active_campaigns.md`, `docs/release-checklist.md` |
| `scripts/manual-checks.allowlist` | WIRED | YES | 50 | `scripts/check-contract-coverage.py`, `scripts/check-contracts.sh` |
| `scripts/mc2_env_pin.py` | WIRED | no | 267 | `scripts/check-background-launch.py`, `scripts/check-capture-env-declared.py` |
| `scripts/mech_shot.py` | WIRED | no | 118 | `.claude/DESKTOP-ISOLATION-WIRING-1.patch`, `docs/background-launch-inventory.md` |
| `scripts/mechbay_preview_harness.py` | WIRED | no | 125 | `.claude/archive-2026-07-05/HANDOFF-mech-preview-fbo.md`, `docs/background-launch-inventory.md` |
| `scripts/new_lane.py` | WIRED | YES | 435 | `docs/dev-velocity.md`, `scripts/check-contract-coverage.py` |
| `scripts/path-open-baseline.txt` | WIRED | no | 20 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/pe_gate_scan.py` | WIRED | no | 156 | `scripts/check-delivery.py`, `scripts/exe_fingerprint.py` |
| `scripts/pipeline_visual_gate.py` | WIRED | no | 347 | `docs/render-backend-seams/HANDOFF-WATER-VISUAL-GATE-2.md`, `docs/render-backend-seams/pipeline-visual-gate-harness-isolation-plan-1.md` |
| `scripts/provenance-known-failures.json` | WIRED | no | 43 | `scripts/check-capture-provenance.py`, `scripts/check-contracts.sh` |
| `scripts/provenance_gate.py` | WIRED | YES | 960 | `scripts/check-contracts.sh`, `tests/visual/goldens/reset-50331d5/golden.json` |
| `scripts/quick_shot.py` | WIRED | no | 94 | `.claude/agents/mc2-terrain-indirect-expert.md`, `docs/background-launch-inventory.md` |
| `scripts/quick_sweep.py` | WIRED | no | 102 | `docs/background-launch-inventory.md`, `scripts/check-background-launch.py` |
| `scripts/read_review_feedback.py` | WIRED | YES | 284 | `docs/review-feedback-schema.md`, `release_assets_src/annotate/review_annotate.js` |
| `scripts/registered-gate-passthrough-debt.txt` | WIRED | no | 91 | `.claude/LIGHTING-HEADROOM-AUDIT.md`, `scripts/check-contract-coverage.py` |
| `scripts/registry_parse.py` | WIRED | no | 598 | `scripts/build_review_pack.py`, `scripts/check-contracts.sh` |
| `scripts/renderdoc_capture.py` | WIRED | no | 366 | `.claude/VK-ARC-STATE.md`, `.claude/VK-RUNG8-LADDER-1.md` |
| `scripts/res-aspect-baseline.json` | WIRED | no | 13 | `.claude/ISSUE-FIXES-STATUS.md`, `scripts/check-res-aspect-contract.py` |
| `scripts/resolve_campaign_config.py` | WIRED | no | 388 | `docs/render-backend-seams/abl-mod-mission-guard-validation-1.md`, `scripts/sweep_campaign.py` |
| `scripts/run-unit-tests.sh` | WIRED | no | 118 | `docs/render-backend-seams/executor-island-recon-1.md`, `scripts/check-contracts.sh` |
| `scripts/run_editor_smoke.py` | WIRED | no | 456 | `docs/background-launch-inventory.md`, `docs/editor-debug-overlay-phase2.md` |
| `scripts/run_golden_parity.py` | WIRED | no | 1436 | `.claude/VK-P8a-STEP0-CASTER-FEED-NOTE.md`, `.claude/VULKAN-NEXT-SLICES.md` |
| `scripts/run_smoke.py` | WIRED | YES | 3574 | `.claude/CEMENT-EDGE-CURVE-1-STATUS.md`, `.claude/FIXTURE-GLOW-1-STATUS.md` |
| `scripts/run_smoke_matrix.py` | WIRED | no | 201 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/run_visual.py` | WIRED | no | 358 | `docs/active_campaigns.md`, `docs/recon/terrain-runtime-api-recon-1.md` |
| `scripts/run_visual_capture.py` | WIRED | YES | 759 | `.claude/DESKTOP-ISOLATION-WIRING-1.patch`, `.claude/PROP-LIGHT-CLASS-4-STATUS.md` |
| `scripts/run_vk_nointerop_strict_visible.py` | WIRED | no | 197 | `.claude/VK-ARC-STATE.md`, `.claude/VK-RUNG8-LADDER-1.md` |
| `scripts/shader_common.py` | WIRED | no | 265 | - |
| `scripts/sim_identity.py` | WIRED | no | 647 | `GameOS/gameos/gos_visual_capture.cpp`, `RenderCore/RendererFeatureRegistry.h` |
| `scripts/smoke_lib/__init__.py` | WIRED | no | 0 | `scripts/check-stock-mission-data.py` |
| `scripts/smoke_lib/baselines.py` | WIRED | no | 29 | `docs/superpowers/strategy/telemetry-oracle-cockpit-architecture.md`, `docs/testing/harness-target-sweep-3.md` |
| `scripts/smoke_lib/cockpit.py` | WIRED | no | 343 | `docs/testing/harness-target-sweep-3.md`, `scripts/manifest_schema.py` |
| `scripts/smoke_lib/crash_evidence.py` | WIRED | no | 162 | `docs/known_issues.md`, `docs/testing/cross-subsystem-audit-recon-1.md` |
| `scripts/smoke_lib/cursor_guard.py` | WIRED | YES | 628 | `scripts/camera_sweep_smoke.py`, `scripts/capture_baseline.py` |
| `scripts/smoke_lib/deploy_lease.py` | WIRED | YES | 419 | `BUILD-WIN.md`, `docs/build-parallel-and-tooling.md` |
| `scripts/smoke_lib/fingerprint.py` | WIRED | YES | 67 | `.claude/MEASUREMENT-LEDGER.md`, `docs/testing/cross-subsystem-audit-recon-1.md` |
| `scripts/smoke_lib/gates.py` | WIRED | no | 111 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/smoke_lib/isolation.py` | WIRED | YES | 828 | `.claude/DESKTOP-ISOLATION-WIRING-1.patch`, `docs/deploy-isolation.md` |
| `scripts/smoke_lib/logparse.py` | WIRED | no | 246 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/smoke_lib/manifest.py` | WIRED | no | 103 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/smoke_lib/nofocus.py` | WIRED | no | 431 | `.claude/DESKTOP-ISOLATION-WIRING-1.patch`, `GameOS/gameos/gameos_input.cpp` |
| `scripts/smoke_lib/preflight_cache.py` | WIRED | YES | 136 | `scripts/run_smoke.py`, `tests/smoke/test_preflight_cache.py` |
| `scripts/smoke_lib/report.py` | WIRED | no | 109 | `docs/active_campaigns.md`, `docs/modernization-roadmap-2026-06-09.md` |
| `scripts/smoke_lib/runner.py` | WIRED | YES | 317 | `.claude/VISIBILITY-SEMANTICS-1-STATUS.md`, `RenderCore/RendererFeatureRegistry.h` |
| `scripts/smoke_with_screenshots.py` | WIRED | no | 276 | `docs/background-launch-inventory.md`, `docs/tier1_env_vars.md` |
| `scripts/split-path-stamps.json` | WIRED | no | 19 | `.claude/META-FIX-DESIGNS-2026-07-16.json`, `.claude/META-FIX-PLAN-2026-07-16.md` |
| `scripts/sweep_campaign.py` | WIRED | no | 111 | `docs/background-launch-inventory.md`, `scripts/mechbay_preview_harness.py` |
| `scripts/telemetry_lift.py` | WIRED | no | 494 | `docs/superpowers/strategy/superpowers-execution-roadmap.md`, `docs/superpowers/strategy/telemetry-oracle-cockpit-architecture.md` |
| `scripts/test_release_zip_install.py` | WIRED | YES | 390 | `docs/RELEASE-PROCESS.md`, `docs/release-checklist.md` |
| `scripts/unregistered-harness-debt.txt` | WIRED | no | 29 | `scripts/check-contract-coverage.py` |
| `scripts/validate_shaders.py` | WIRED | no | 290 | `.claude/FIXTURE-GLOW-1-STATUS.md`, `docs/render-backend-seams/render-contract-index-1.md` |
| `scripts/visual_compare.py` | WIRED | no | 258 | `docs/render-backend-seams/HANDOFF-WATER-VISUAL-GATE-2.md`, `docs/render-backend-seams/pipeline-visual-gate-harness-recon-1.md` |
| `scripts/visual_diff.py` | WIRED | no | 455 | `.claude/VK-ARC-STATE.md`, `docs/background-launch-inventory.md` |
| `scripts/water_visual_diff.py` | WIRED | no | 122 | `docs/background-launch-inventory.md`, `docs/water-rv-arc-recon.md` |
| `scripts/write-deploy-manifest.py` | WIRED | YES | 138 | `.claude/skills/mc2-deploy.md`, `scripts/check-deploy-coherence.py` |
| `tools/build_contract_harnesses.py` | WIRED | no | 164 | `.claude/FIXTURE-GLOW-1-STATUS.md`, `.claude/IBL-NIGHT-SCALE-1-STATUS.md` |
| `tools/build_quiet.py` | WIRED | YES | 73 | `AGENTS.md`, `docs/disciplines.md` |
| `tools/cook_bc6h_hdri.py` | WIRED | no | 337 | `.gitignore`, `docs/testing/deploy-asset-contract-harness-1-recon.md` |
| `tools/debug_view_sweep.py` | WIRED | no | 304 | `GameOS/gameos/gameos_graphics.cpp`, `RenderCore/RendererFeatureRegistry.h` |
| `tools/engine_standalone_stub.cpp` | WIRED | no | 35 | `CMakeLists.txt` |
| `tools/list_free_ordinals.py` | WIRED | no | 99 | `AGENTS.md`, `docs/disciplines.md` |
| `tools/menu_oracle.py` | WIRED | no | 214 | `AGENTS.md`, `docs/background-launch-inventory.md` |
| `tools/pak_append.py` | WIRED | no | 170 | `.claude/TERRAIN-SCATTER-MASK-1-RECON.md`, `scripts/check-stock-mission-data.py` |
| `tools/pak_dump.py` | WIRED | no | 272 | `scripts/check-stock-mission-data.py`, `scripts/predict_cone_pools.py` |
| `tools/perf_snapshot.py` | WIRED | no | 152 | `.claude/VK-ARC-STATE.md`, `docs/background-launch-inventory.md` |
| `tools/run_contract_tests.py` | WIRED | no | 204 | `docs/testing/deploy-asset-contract-harness-1-recon.md`, `docs/testing/dev-efficiency-bootstrap-sweep-1.md` |
| `tools/run_nvidia_readiness_smoke.py` | WIRED | no | 104 | `docs/render-backend-seams/nvidia-readiness-runbook.md`, `scripts/run_smoke.py` |
| `tools/test_pak_append.py` | WIRED | no | 127 | `.claude/TERRAIN-SCATTER-MASK-1-RECON.md`, `scripts/check-stock-mission-data.py` |
| `tools/test_pak_dump.py` | WIRED | no | 56 | `scripts/check-stock-mission-data.py` |
| `tools/tracy_capture_compare.py` | WIRED | no | 961 | `.claude/VK-ARC-STATE.md`, `docs/README.md` |
| `tools/vulkan_shader_probe_main.cpp` | WIRED | no | 65 | `.claude/VULKAN-NEXT-SLICES.md`, `CMakeLists.txt` |
| `tools/witness_smoke.py` | WIRED | no | 149 | `AGENTS.md`, `docs/disciplines.md` |
| `scripts/bootstrap_worktree_build.py` | REFERENCED | YES | 174 | `.claude/skills/mc2-build.md`, `CLAUDE.md` |
| `scripts/check-flag-registry-drift.sh` | REFERENCED | no | 34 | `scripts/check-contracts.sh`, `scripts/check-flag-registry-truth.py` |
| `scripts/deploy-mc2fx-tools.sh` | REFERENCED | no | 110 | `.claude/skills/mc2-deploy.md` |
| `scripts/measure_tile_boundary.py` | REFERENCED | no | 144 | `.claude/user-feedback/QUEUE-2026-08-01.md` |
| `scripts/mech_bone_parity.py` | REFERENCED | no | 115 | `docs/bt2018-skel-1b-gpu-recon.md`, `docs/bt2018-skel-1b-gpu-skinning-recon-2.md` |
| `scripts/object_burial_audit.py` | REFERENCED | YES | 1388 | `.claude/MEASUREMENT-LEDGER.md`, `scripts/water_class_flip_audit.py` |
| `scripts/oracle_report.py` | REFERENCED | no | 127 | `docs/modernization-roadmap-2026-06-09.md`, `docs/perf-budget.md` |
| `scripts/parse-done-reports.py` | REFERENCED | YES | 273 | `docs/LANE_COMPLETION_REPORT.template.md` |
| `scripts/predict_cement_adopt.py` | REFERENCED | no | 259 | `.claude/user-feedback/QUEUE-2026-08-01.md`, `RenderCore/RendererFeatureRegistry.h` |
| `scripts/predict_cone_pools.py` | REFERENCED | no | 489 | `.claude/user-feedback/QUEUE-2026-08-01.md`, `RenderCore/RendererFeatureRegistry.h` |
| `scripts/predict_shore_edge.py` | REFERENCED | no | 388 | `.claude/user-feedback/QUEUE-2026-08-01.md` |
| `scripts/qwen_local.py` | REFERENCED | YES | 422 | `docs/qwen-local-workflow.md` |
| `scripts/render_pass_report.py` | REFERENCED | no | 273 | `docs/active_campaigns.md`, `tests/telemetry/test_render_pass_report.py` |
| `scripts/run_brain_apply_ab.ps1` | REFERENCED | no | 66 | `tools/brain_dispatch_harness/README-v2.md` |
| `scripts/run_editor_playtest_smoke.py` | REFERENCED | no | 395 | `docs/background-launch-inventory.md`, `tests/contract/default_build_quiet_baseline.json` |
| `scripts/run_smoke_asan.py` | REFERENCED | no | 254 | `docs/background-launch-inventory.md`, `docs/testing-strategy.md` |
| `scripts/run_vk_field_check.py` | REFERENCED | no | 354 | `.claude/VK-ARC-STATE.md`, `.claude/VK-RUNG8-LADDER-1.md` |
| `scripts/run_vulkan_probe.py` | REFERENCED | YES | 415 | `.claude/VULKAN-NEXT-SLICES.md`, `docs/render-backend-seams/vulkan-island-template.md` |
| `scripts/smoke_lib/evidence_classifier.py` | REFERENCED | no | 195 | `docs/testing/smoke-evidence-classifier-1.md`, `tests/smoke/test_evidence_classifier.py` |
| `scripts/smoke_lib/oracleparse.py` | REFERENCED | no | 241 | `docs/modernization-roadmap-2026-06-09.md`, `docs/perf-budget.md` |
| `scripts/terrain_height_audit.py` | REFERENCED | no | 460 | `RenderCore/RendererFeatureRegistry.h`, `docs/terrain-height-audit.md` |
| `scripts/test_visual_diff.py` | REFERENCED | no | 10 | `docs/testing/harness-target-sweep-3.md`, `tests/visual/test_visual_diff.py` |
| `scripts/verify_campaign_assets.py` | REFERENCED | no | 156 | `docs/STARTER-GUIDE.md`, `docs/testing/harness-target-sweep-2.md` |
| `scripts/verify_executor_slice.py` | REFERENCED | no | 568 | `docs/build-parallel-and-tooling.md` |
| `scripts/verify_mod_roundtrip.py` | REFERENCED | no | 400 | `docs/active_campaigns.md` |
| `scripts/vk_shadow_stale_discriminator.py` | REFERENCED | no | 160 | `.claude/VK-ARC-STATE.md` |
| `scripts/water_class_flip_audit.py` | REFERENCED | no | 297 | `docs/disciplines.md` |
| `tools/albedo_masked_audit.py` | REFERENCED | YES | 1312 | `.claude/LIGHTING-HEADROOM-AUDIT.md`, `.claude/MEASUREMENT-LEDGER.md` |
| `tools/ase_to_glb.py` | REFERENCED | no | 1024 | `data/tgl/HangarGLB.mcasset.json`, `data/tgl/QuonsetGLB.mcasset.json` |
| `tools/asset_baseline.py` | REFERENCED | no | 268 | `docs/active_campaigns.md`, `docs/asset-pipeline.md` |
| `tools/check_ubo_layout.py` | REFERENCED | no | 199 | `.claude/VK-ARC-STATE.md`, `GameOS/gameos/vulkan_post_fog_native.cpp` |
| `tools/check_vk_shaders.py` | REFERENCED | no | 111 | `.claude/VK-ARC-STATE.md` |
| `tools/classify_materials.py` | REFERENCED | no | 356 | `docs/active_campaigns.md`, `docs/asset-pipeline.md` |
| `tools/fst_repack_drop.py` | REFERENCED | no | 78 | `docs/testing/cross-subsystem-audit-recon-1.md`, `mclib/txmmgr.cpp` |
| `tools/gen_cliff_wall_glb.py` | REFERENCED | no | 327 | `docs/tier1_env_vars.md`, `tools/blender_bake_coastal_cliff.py` |
| `tools/glsl_port_diff.py` | REFERENCED | no | 130 | `.claude/VK-ARC-STATE.md`, `GameOS/gameos/vulkan_post_fog_native.cpp` |
| `tools/make_handoff.py` | REFERENCED | YES | 111 | `docs/testing/cross-subsystem-audit-recon-1.md`, `docs/testing/dev-efficiency-bootstrap-sweep-1.md` |
| `tools/place_cliffwall_mc2_01.py` | REFERENCED | no | 142 | `docs/tier1_env_vars.md`, `tools/blender_bake_coastal_cliff.py` |
| `tools/scenario_runner.py` | REFERENCED | no | 775 | `docs/gameplay-driver.md` |
| `tools/spawn_lane.py` | REFERENCED | YES | 470 | `docs/dev-velocity.md`, `tools/test_spawn_lane.py` |
| `tools/test_spawn_lane.py` | REFERENCED | YES | 155 | `docs/dev-velocity.md` |
| `tools/ui_art_reconstruct.py` | REFERENCED | no | 300 | `RenderCore/RendererFeatureRegistry.h`, `code/controlgui.cpp` |
| `tools/validate_glb.py` | REFERENCED | no | 456 | `docs/active_campaigns.md`, `docs/asset-pipeline.md` |
| `tools/vk_feed_diff.py` | REFERENCED | no | 141 | `.claude/VK-ARC-STATE.md`, `GameOS/gameos/vk_feed_dump.h` |
| `scripts/terrain_grounding_audit.py` | XREF-ONLY | no | 275 | `scripts/terrain_height_audit.py`, `tests/terrain/terrain_grounding_audit_20260528T095135.md` |
| `scripts/whatran.py` | XREF-ONLY | no | 220 | `tests/contract/default_build_quiet_baseline.json` |
| `tools/blender_cmd.py` | XREF-ONLY | no | 29 | `tools/blender_bake_coastal_cliff.py`, `tools/blender_pipeline/asset_edit.py` |
| `tools/condition_coastal_cliff_glb.py` | XREF-ONLY | no | 150 | `tools/blender_bake_coastal_cliff.py` |
| `tools/install_cliff_dressing.py` | XREF-ONLY | no | 196 | `tools/install_tree_scatter.py`, `tools/place_marblecliff_mc2_01.py` |
| `tools/install_tree_scatter.py` | XREF-ONLY | no | 153 | `tools/terrain_beautify/scatter_tool.py` |
| `tools/marblecliff_fit_template.txt` | XREF-ONLY | no | 42 | `tools/install_cliff_dressing.py` |
| `tools/marblecliffscatter_fit_template.txt` | XREF-ONLY | no | 42 | `tools/install_cliff_dressing.py` |
| `tools/pinescatter1_fit_template.txt` | XREF-ONLY | no | 16 | `tools/install_tree_scatter.py` |
| `tools/pinescatter2_fit_template.txt` | XREF-ONLY | no | 16 | `tools/install_tree_scatter.py` |
| `tools/pinescatter3_fit_template.txt` | XREF-ONLY | no | 16 | `tools/install_tree_scatter.py` |
| `tools/residency_slim.py` | XREF-ONLY | no | 181 | `mclib/txmmgr.cpp`, `scripts/residency_slim_report.py` |
| `scripts/bt2018_retarget_clips.py` | ORPHAN | no | 115 | - |
| `scripts/build_asset_thumbnails.py` | ORPHAN | no | 276 | - |
| `scripts/cook_magic_icons.py` | ORPHAN | no | 57 | - |
| `scripts/cook_upscaled_assets_mod.py` | ORPHAN | no | 301 | - |
| `scripts/gltf_fk_oracle.py` | ORPHAN | no | 310 | - |
| `scripts/nightly_evidence_2.py` | ORPHAN | no | 152 | - |
| `scripts/nightly_evidence_2_phase2b_3.py` | ORPHAN | no | 193 | - |
| `scripts/pull_compat_pilot_roster.py` | ORPHAN | no | 374 | - |
| `scripts/residency_slim_report.py` | ORPHAN | no | 435 | - |
| `scripts/run_closeout_tests.bat` | ORPHAN | no | 136 | - |
| `scripts/run_smoke_local.py` | ORPHAN | no | 20 | - |
| `scripts/run_vk_nointerop_strict_smoke.py` | ORPHAN | no | 71 | - |
| `scripts/slice_gate.py` | ORPHAN | no | 299 | - |
| `scripts/test_deploy_lease.py` | ORPHAN | YES | 380 | - |
| `scripts/test_smoke_guard.py` | ORPHAN | no | 92 | - |
| `scripts/test_smoke_lane_lock.py` | ORPHAN | YES | 108 | - |
| `scripts/visual_gallery.py` | ORPHAN | no | 249 | - |
| `tools/blender_bake_coastal_cliff.py` | ORPHAN | no | 126 | - |
| `tools/data_ship_census.py` | ORPHAN | no | 528 | - |
| `tools/extract_veg_cards.py` | ORPHAN | no | 88 | - |
| `tools/place_coastalcliff_mc2_01.py` | ORPHAN | no | 147 | - |
| `tools/place_marblecliff_mc2_01.py` | ORPHAN | no | 162 | - |
