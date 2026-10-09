# Pass 2 memory audit (report-only)

Coverage: 395 memory files linked from INDEX-MECH, INDEX-BUILD*, INDEX-MISSION-DATA, INDEX-SMOKE-TEST, INDEX-PROCESS* (8 index files). Checks run mechanically over full text by script; no memory files were edited.

Caveat: 'worktree/lane' matches include generic words; obsolete = worktree/lane mention AND >=50% of repo paths missing.

| memory file | problem type | detail |
|---|---|---|
| CONTRACT-HARNESS-ARC-1.md | missing path | 3/24 missing, e.g. mech3d.cpp/txmmgr.h/golden-sets.json, docs/testing/harness-target-sweep-3.md, docs/testing/ibl-hdri-asset-pack-manifest-1.md |
| CONTRACT-HARNESS-ARC-1.md | worktree/lane/lease ref | 2 lines, e.g. "- **DEPLOY-ASSET-CONTRACT-HARNESS-1** (commit `5dbea67c`, merged nifty `97b80c1a`) — **MILESTONE: first Python" |
| HANDOFF-2026-07-02-session-end.md | missing path | 3/3 missing, e.g. .claude/MORNING-VIEWING-README.md, .claude/FRAMEBUDGET-2-LEDGER-1.md, .claude/ADVERSARIAL-SYNTHESIS-FINAL.md |
| HANDOFF-2026-07-02-session-end.md | missing MC2_ name | MC2_TERRAIN_ |
| HANDOFF-2026-07-02-session-end.md | worktree/lane/lease ref | 8 lines, e.g. "- Arc branch `claude/controlmap-sample-1` (worktree A:/Games/mc2-controlmap-sample-1)," |
| HANDOFF-2026-07-02-truth-first-arc.md | missing path | 5/21 missing, e.g. docs/G2-GL-FRAME-PROFILE-RUNBOOK.md, .claude/MESHOPT-COOK-1.md, .claude/VK-GL-INTEROP-SPIKE-1.md |
| HANDOFF-2026-07-02-truth-first-arc.md | worktree/lane/lease ref | 17 lines, e.g. "5. Build identity untrustworthy = deploy slot churned across lanes + terrain build stamped `build=UNKNOWN`." |
| HANDOFF-2026-07-02-truth-first-arc.md | OPEN/TODO/unfixed claim | 3 lines, e.g. "## P4 ACCEPTANCE AUDIT (2026-07-02, advisor-ordered) — ACCEPTED w/ 1 non-blocking TODO" |
| HANDOFF-2026-07-03-cliff-mesh-decal.md | missing path | 5/17 missing, e.g. tools/cliff_conform/RUNTIME_FRAME_MISMATCH.md, scripts/check-cliff-assets-tracked.sh, .claude/SHORELINE-FOG-NEXT-SLICES.md |
| HANDOFF-2026-07-03-cliff-mesh-decal.md | missing MC2_ name | MC2_CLIFF_FRAME_DUMP, MC2_TERRAIN_DECAL_REPLACE, MC2_TERRAIN_SHORELINE_MESH, MC2_TERRAIN_SHORELINE_MORPH |
| HANDOFF-2026-07-03-cliff-mesh-decal.md | worktree/lane/lease ref | 4 lines, e.g. "- **Isolated worktree now**: all cliff work in `A:/Games/mc2-cliff` (branch `claude/cliff-1`, lane `CLIFF-MESH" |
| HANDOFF-2026-07-03-cliff-mesh-decal.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "- **★OPEN — the weld's clean fix is blocked on 1 calibration**: offline weld PASSES offline but FLOATS in-engi" |
| HANDOFF-2026-07-03-truth-first-resume.md | missing path | 1/1 missing, e.g. docs/G2-GL-FRAME-PROFILE-RUNBOOK.md |
| HANDOFF-2026-07-03-truth-first-resume.md | worktree/lane/lease ref | 1 lines, e.g. "- Canonical worktree = nifty-mendeleev. Confirm: `mc2-repo-intel` **canonical_tip** + **lane_status** + **depl" |
| HANDOFF-2026-07-03-vulkan-interop-seam.md | missing path | 5/5 missing, e.g. .claude/WORKTREE-GUARD.md, .claude/VK-ARC-ROADMAP.md, .claude/VK-P14-RECON.md |
| HANDOFF-2026-07-03-vulkan-interop-seam.md | missing MC2_ name | MC2_VK_SHARED_BUFFER_SELFTEST |
| HANDOFF-2026-07-03-vulkan-interop-seam.md | worktree/lane/lease ref | 5 lines, e.g. "**Branch `claude/vk-interop-bridge-1`** (worktree `A:/Games/mc2-vk-interop-bridge`, off L6 lane)." |
| HANDOFF-2026-07-03-world-surface-fidelity-arc.md | missing MC2_ name | MC2_EXPOSURE, MC2_HDR_AUTOEXPOSURE, MC2_TERRAIN_CEMENT_LIGHTING, MC2_TERRAIN_CLIFF_DETAIL_V2 |
| HANDOFF-2026-07-03-world-surface-fidelity-arc.md | worktree/lane/lease ref | 6 lines, e.g. "`git worktree add A:/Games/mc2-<name> -b claude/<name> claude/cliff-dress-1`" |
| HANDOFF-2026-07-03-world-surface-fidelity-arc.md | OPEN/TODO/unfixed claim | 4 lines, e.g. "**OPEN (next session):**" |
| HANDOFF-2026-07-04-vk-arc-resume.md | missing path | 3/3 missing, e.g. .claude/WORKTREE-GUARD.md, .claude/VK-ARC-ROADMAP.md, mc2-win64-v0.4c/tracy-vk-capture.bat |
| HANDOFF-2026-07-04-vk-arc-resume.md | worktree/lane/lease ref | 3 lines, e.g. "- Branch `claude/vk-interop-bridge-1`. Worktree `A:/Games/mc2-vk-interop-bridge`. HEAD `ecbdf98c` (25 commits," |
| HANDOFF-2026-07-04-vk-p14-native-endgame.md | missing path | 4/10 missing, e.g. .claude/WORKTREE-GUARD.md, .claude/VK-P10-TERRAIN-SHARED-GBUFFER-RECON.md, .claude/VK-P14b-PRESENT-TAIL-RECON.md |
| HANDOFF-2026-07-04-vk-p14-native-endgame.md | missing MC2_ name | MC2_VK_PROP_COLOR_INTEROP |
| HANDOFF-2026-07-04-vk-p14-native-endgame.md | worktree/lane/lease ref | 3 lines, e.g. "**Worktree** `A:/Games/mc2-vk-interop-bridge`, branch `claude/vk-interop-bridge-1`, **HEAD `cf78e9b3`**" |
| HANDOFF-2026-07-06-fable-close.md | missing path | 3/20 missing, e.g. mc2-main/AGENTS.md, .claude/VK-FLIP-EVIDENCE-1.md, .claude/VK-FULL-OWNERSHIP-RECON-1.md |
| HANDOFF-2026-07-06-fable-close.md | worktree/lane/lease ref | 7 lines, e.g. "- **Code canonical:** `A:/Games/mc2-main` (branch `claude/nifty-mendeleev`; local branch `main` is kept force-" |
| HANDOFF-2026-07-22-release-hold-automation.md | missing path | 1/5 missing, e.g. verify_verdicts/mc2_10_saved_204523/VERDICT.md |
| HANDOFF-2026-07-22-release-hold-automation.md | worktree/lane/lease ref | 3 lines, e.g. "~15s game runs, own-PID kills, lane-check before deploys/launches; subagents not" |
| HANDOFF-2026-08-01-review-feedback-fixes.md | worktree/lane/lease ref | 1 lines, e.g. "# 2026-08-01 — FEEDBACK-0801-QUEUE-1 (worktree A:/Games/mc2-main, branch claude/nifty-mendeleev)" |
| HANDOFF-2026-10-08-vk-parity.md | worktree/lane/lease ref | 1 lines, e.g. "5. Queued cleanup: delete 9 terrain gates (TERRAIN_LIGHTING_V1/V2, NORMALS_FROM_HEIGHT, CLIFF_DISPLACE, AMBIEN" |
| HANDOFF_2026_05_29_trackv_vfx_payoff_shipped.md | missing MC2_ name | MC2_VFX_BLOOM_SCALE |
| HANDOFF_2026_05_29_trackv_vfx_payoff_shipped.md | worktree/lane/lease ref | 2 lines, e.g. "Branch `claude/trackv-vfx-payoff-1`, worktree `A:/Games/mc2-trackv-vfx-payoff`," |
| HANDOFF_2026_05_29_vfx_originals_arc_phase1_cardcloud.md | worktree/lane/lease ref | 2 lines, e.g. "Worktree `claude/track-rv-VFX` (rebased onto nifty HEAD; lane deploy =" |
| HANDOFF_2026_05_31_perf_gom_readback_id_cache.md | missing path | 2/5 missing, e.g. gvehicl.cpp/mech.cpp, mech3d.cpp/gvactor.cpp |
| HANDOFF_2026_05_31_perf_gom_readback_id_cache.md | worktree/lane/lease ref | 1 lines, e.g. "PERF-GPU-CULL-READBACK-ID-CACHE-1 SHIPPED. Commit `ef03162a` on branch `claude/perf-gpu-cull-readback-id-cache" |
| HANDOFF_2026_06_02_readback_shadow_copy_and_elimination.md | worktree/lane/lease ref | 3 lines, e.g. "## Key file locations (nifty-mendeleev worktree)" |
| HANDOFF_2026_06_04_asset_viewer_mod_workbench_mvp.md | missing path | 2/5 missing, e.g. nlohmann/json.h, 3rdparty/include/nlohmann/json.h |
| HANDOFF_2026_06_04_asset_viewer_mod_workbench_mvp.md | worktree/lane/lease ref | 2 lines, e.g. "**Branch** `claude/asset-viewer-mod-workbench-recon-1` (worktree `A:/Games/mc2-asset-viewer-mod-workbench`, of" |
| HANDOFF_2026_06_04_asset_viewer_mod_workbench_mvp.md | OPEN/TODO/unfixed claim | 2 lines, e.g. "- **UV double-flip bug** — a "non-discriminating UV smoke assertion" finding, when strengthened, exposed that " |
| HANDOFF_2026_06_04_foliage_impostor_pivot.md | missing path | 4/5 missing, e.g. .claude/cook_stock_impostor.py, .claude/foliage_crop.py, .claude/tree_export_impostor.py |
| HANDOFF_2026_06_04_foliage_impostor_pivot.md | worktree/lane/lease ref | 2 lines, e.g. "- Engine impostor system + distance-select + shadow-skip + instrumentation all COMMITTED on `claude/model-over" |
| HANDOFF_2026_06_04_foliage_impostor_pivot.md | OPEN/TODO/unfixed claim | 2 lines, e.g. "## OPEN for fresh session (priority order)" |
| HANDOFF_2026_06_04_foliage_prepass_and_nifty_merge.md | missing path | 4/5 missing, e.g. docs/superpowers/specs/2026-06-04-foliage-depth-prepass-overdraw-design.md, docs/superpowers/plans/2026-06-04-foliage-staticprop-depth-prepass.md, .claude/tree_lod_bake.py |
| HANDOFF_2026_06_04_foliage_prepass_and_nifty_merge.md | worktree/lane/lease ref | 18 lines, e.g. "description: Session handoff — nifty merged into override-foliage branch + depth-prepass overdraw meta-fix imp" |
| HANDOFF_2026_06_04_gpucull_oracle_corrected_metafix.md | missing path | 3/3 missing, e.g. docs/superpowers/specs/2026-06-04-frozen-static-cull-records-meta-fix-spec.md, docs/superpowers/specs/2026-06-04-gpu-scene-model-recon.md, docs/superpowers/plans/2026-06-04-m2a-population-split.md |
| HANDOFF_2026_06_04_gpucull_oracle_corrected_metafix.md | missing MC2_ name | MC2_STATIC_PROP_VISIBLEIDS_DRAW |
| HANDOFF_2026_06_04_gpucull_oracle_corrected_metafix.md | missing symbol | compute_getVisibleIdsBuf |
| HANDOFF_2026_06_04_gpucull_oracle_corrected_metafix.md | worktree/lane/lease ref | 3 lines, e.g. "Branch `claude/perf-gpucull-ownership-1` (worktree `A:/Games/mc2-perf-gpucull-ownership`," |
| HANDOFF_2026_06_05_editor_mapgen_imgui_crash.md | worktree/lane/lease ref | 1 lines, e.g. "**Worktree:** `A:/Games/mc2-main/`" |
| HANDOFF_2026_06_05_editor_usability_overhaul.md | missing path | 1/11 missing, e.g. mc2-win64-v0.4/run-editor.bat |
| HANDOFF_2026_06_05_editor_usability_overhaul.md | worktree/lane/lease ref | 1 lines, e.g. "**Branch:** `claude/nifty-mendeleev` (canonical worktree `A:/Games/mc2-main/`)." |
| HANDOFF_2026_06_05_trackg_cook_m2a_merge_projection_metafix.md | missing path | 2/3 missing, e.g. docs/superpowers/specs/2026-06-05-projection-angle-disappear-meta-fix-recon.md, run_forcelod0/1.bat |
| HANDOFF_2026_06_05_trackg_cook_m2a_merge_projection_metafix.md | worktree/lane/lease ref | 8 lines, e.g. "- Worktree `A:/Games/mc2-cook-m2a-merge`, branch `claude/cook-m2a-merge`." |
| HANDOFF_2026_06_09_dynamic_pipeline_oracle.md | worktree/lane/lease ref | 2 lines, e.g. "- Subagent edited the **stale ROOT** `mclib/mech3d.cpp` instead of the worktree path despite being given" |
| HANDOFF_2026_06_10_asset_viewer_backend_a_v2_shipped.md | missing path | 2/4 missing, e.g. specs/2026-06-10-asset-viewer-backend-a-v2-design.md, plans/2026-06-10-asset-viewer-backend-a-v2.md |
| HANDOFF_2026_06_10_asset_viewer_backend_a_v2_shipped.md | worktree/lane/lease ref | 1 lines, e.g. "**On `claude/nifty-mendeleev` HEAD `f7ef9c56`** (10 commits `4b3eb05b`..`f7ef9c56`). Built+tested on the nifty" |
| HANDOFF_2026_06_10_asset_viewer_backend_a_v2_shipped.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "## OPEN / NEXT (v3 + user checks)" |
| HANDOFF_2026_06_10_asset_viewer_s5_polish_shipped.md | missing path | 2/4 missing, e.g. specs/2026-06-10-asset-viewer-s5-polish-design.md, plans/2026-06-10-asset-viewer-s5-polish.md |
| HANDOFF_2026_06_10_asset_viewer_s5_polish_shipped.md | worktree/lane/lease ref | 1 lines, e.g. "**On `claude/nifty-mendeleev` HEAD `7e4abed9`** (11 commits `323c4bd8`..`7e4abed9`). Built+tested on the nifty" |
| HANDOFF_2026_06_10_asset_viewer_s5_polish_shipped.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "## OPEN / NEXT" |
| HANDOFF_2026_06_10_baseline_A_and_oracle_harness.md | worktree/lane/lease ref | 8 lines, e.g. "in the clean worktree `A:/Games/mc2-nifty-merge` (the `nifty-mendeleev` worktree is contended by" |
| HANDOFF_2026_06_10_baseline_A_and_oracle_harness.md | OPEN/TODO/unfixed claim | 5 lines, e.g. "description: Baseline A captured + oracle/perf-budget harness shipped; modernization backlog now OPEN" |
| HANDOFF_2026_06_10_pick_path_hotspots_recon.md | missing MC2_ name | MC2_MOVE_PATH_CACHE |
| HANDOFF_2026_06_10_pick_path_hotspots_recon.md | OPEN/TODO/unfixed claim | 7 lines, e.g. "description: "NEXT-SESSION handoff — two open perf hotspots (FindTerrainObj pick + Warrior.Path A*) recon'd bu" |
| HANDOFF_2026_06_10_terrain_8z_production_closeout.md | worktree/lane/lease ref | 4 lines, e.g. "snapshot, oracle counters — captured post-8z, **pre-GlStateGuard / pre-visual-lanes**. Baseline A gates" |
| HANDOFF_2026_06_11_modder_loop_v1_complete.md | missing path | 1/6 missing, e.g. docs/superpowers/plans/modder-workflow-first-loop.md |
| HANDOFF_2026_06_11_modder_loop_v1_complete.md | worktree/lane/lease ref | 4 lines, e.g. "- **Worktree**: `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev` — SHARED with a tacmap session; br" |
| HANDOFF_2026_06_12_ktx_residency_slim.md | missing path | 1/2 missing, e.g. docs/texture-residency-registry-recon.md |
| HANDOFF_2026_06_12_ktx_residency_slim.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "## Visual gate OPEN (user must confirm interactively)" |
| HANDOFF_2026_06_14_proof_machine_and_s_program.md | missing path | 1/6 missing, e.g. scripts/residency_slim_report.py |
| HANDOFF_2026_06_14_proof_machine_tooling_slices.md | missing path | 3/7 missing, e.g. scripts/visual_gallery.py, scripts/build_asset_thumbnails.py, data/art/Buildings.c |
| HANDOFF_2026_06_15_editor_projection_split_brain.md | missing path | 1/1 missing, e.g. docs/editor-game-projection-divergence-recon.md |
| HANDOFF_2026_06_15_editor_projection_split_brain.md | worktree/lane/lease ref | 3 lines, e.g. "Worktree A:/Games/mc2-main. **WARNING: the" |
| HANDOFF_2026_06_17_asset_mod_pipeline_v0.md | worktree/lane/lease ref | 2 lines, e.g. "### Python tools (`tools/` in the worktree)" |
| HANDOFF_2026_06_19_harness_stability_arc.md | missing path | 3/9 missing, e.g. .claude/campaign-visual-findings.md, .claude/load-unload-perf-recon.md, .claude/rc1-slimmer-tgl-ini-gap.md |
| HANDOFF_2026_06_19_harness_stability_arc.md | missing MC2_ name | MC2_LOAD_TIMING |
| HANDOFF_2026_06_19_harness_stability_arc.md | worktree/lane/lease ref | 4 lines, e.g. "(nifty HEAD advanced past d6a7e810 via other lanes after the tag was cut.)" |
| HANDOFF_2026_06_19_harness_stability_arc.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "STILL-OPEN ENGINE CRASHES (sweep-found, NOT yet fixed, symbolicated):" |
| HANDOFF_2026_06_20_cheatmode_harness.md | missing path | 1/5 missing, e.g. rc1/crash_catalog.json |
| HANDOFF_2026_06_20_cheatmode_harness.md | missing MC2_ name | MC2_SOAK_ |
| HANDOFF_2026_06_20_cheatmode_harness.md | worktree/lane/lease ref | 4 lines, e.g. "nifty worktree `…/worktrees/nifty-mendeleev`, branch `claude/nifty-mendeleev`. Built on the campaign-soak harn" |
| HANDOFF_2026_06_20_cheatmode_harness.md | OPEN/TODO/unfixed claim | 2 lines, e.g. "- **OPEN: MechCommanderOmnitech** execExpression ablxexpr.cpp:501 m0 crash (config-verified mco-compat/fit `ca" |
| HANDOFF_2026_06_20_terrain_overlay_mask_1.md | missing MC2_ name | MC2_TERRAIN_CEMENT_BAKE, MC2_TERRAIN_OVERLAY_MASK |
| HANDOFF_2026_06_20_terrain_overlay_mask_1.md | missing symbol | isHardSurfaceOverlay, gos_terrain_indirect_CementBakeEnabled |
| HANDOFF_2026_06_20_terrain_overlay_mask_1.md | worktree/lane/lease ref | 3 lines, e.g. "description: "TERRAIN-STATIC-OVERLAY-MASK-1 — CPU hard-surface mask from overlay codes feeds vegetation reject" |
| HANDOFF_2026_06_20_terrain_overlay_mask_1.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "## OPEN / NEXT" |
| HANDOFF_2026_06_21_battletech_first_mech_import.md | missing path | 6/19 missing, e.g. tools/mech_import/mech_prep_marauder.py, tools/mech_import/mech_prep_glb_static.py, .claude/MECH-IMPORT-HARNESS-1-SCOPE.md |
| HANDOFF_2026_06_21_battletech_first_mech_import.md | missing symbol | transform_apply |
| HANDOFF_2026_06_21_battletech_first_mech_import.md | worktree/lane/lease ref | 3 lines, e.g. "- Location: `<worktree>/tools/mech_import_harness/` (CMakeLists.txt + mech_import_harness.cpp + README.md). **" |
| HANDOFF_2026_06_21_battletech_first_mech_import.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "OPEN: (a) `material 1 -> NULLTXM` stray empty slot from Blender join — harmless if no faces use it, but clean " |
| HANDOFF_2026_06_22_bt2018_lowerbody_anim_reversal.md | missing path | 4/8 missing, e.g. docs/bt2018-gltf-fk-oracle-1.md, scripts/gltf_fk_oracle.py, docs/bt2018-palette-perpart-offset-audit-1.md |
| HANDOFF_2026_06_22_bt2018_lowerbody_anim_reversal.md | missing MC2_ name | MC2_VISUAL_DIFF |
| HANDOFF_2026_06_22_bt2018_lowerbody_anim_reversal.md | worktree/lane/lease ref | 2 lines, e.g. "- NOTE branch is multi-lane LIVE — HEAD advanced ~6× during this session (50dcf8cb→05740404). Rebuild from cur" |
| HANDOFF_2026_06_22_bt2018_lowerbody_anim_reversal.md | OPEN/TODO/unfixed claim | 3 lines, e.g. "description: "HANDOFF — clean baseline restored; OPEN BUG: imported BT mech lower body 180° reversed under ANI" |
| HANDOFF_2026_06_22_bt2018_roster_import.md | missing path | 1/9 missing, e.g. tests/smoke/artifacts/mech-shots/view_oneMech.txt |
| HANDOFF_2026_06_22_bt2018_roster_import.md | worktree/lane/lease ref | 1 lines, e.g. "- DEPLOY target = SHARED `releases/mc2-win64-v0.5.0` (other lanes clobber the exe → redeploy MY nifty build64 " |
| HANDOFF_2026_06_22_frame_currentness_guards.md | missing path | 2/8 missing, e.g. docs/frame-contracts/currentness-claim-audit-1.md, .claude/FRAME-LIVENESS-CONTRACT-RECON-1-PLAN.md |
| HANDOFF_2026_06_22_frame_currentness_guards.md | missing MC2_ name | MC2_FRAME_LIVENESS_ASSERT, MC2_SKIP_STATIC_NATURAL |
| HANDOFF_2026_06_22_frame_currentness_guards.md | worktree/lane/lease ref | 2 lines, e.g. "**Worktree:** `A:/Games/mc2-frame-currentness-guards` @ branch `claude/frame-currentness-guards-1`, off nifty " |
| HANDOFF_2026_06_23_bt2018_mech_resume.md | missing path | 1/9 missing, e.g. docs/bt2018-mech-import-arc-summary.md |
| HANDOFF_2026_06_23_bt2018_mech_resume.md | worktree/lane/lease ref | 11 lines, e.g. "multi-lane LIVE so re-check HEAD each session). User-confirmed in-game." |
| HANDOFF_2026_06_25_brain_abl_combat_arc.md | missing path | 1/12 missing, e.g. mission.cpp/warrior.cpp |
| HANDOFF_2026_06_25_brain_abl_combat_arc.md | worktree/lane/lease ref | 5 lines, e.g. "Worktree dir named `nifty-mendeleev` IS on `claude/nifty-mendeleev` now (lightgrid long merged in). nifty is M" |
| HANDOFF_2026_06_25_brain_abl_combat_arc.md | OPEN/TODO/unfixed claim | 2 lines, e.g. "- BRAIN-MISSIONFIT-OPORD-CONSUMER-1 (a+b): mission.fit declarative-patrol consumer. **⚠️ gate-ON populate UNVA" |
| HANDOFF_2026_06_25_brain_arc_resume.md | worktree/lane/lease ref | 11 lines, e.g. "**The worktree dir is named `nifty-mendeleev` but is checked out on `claude/lightgrid-build-native-1`.** Whole" |
| HANDOFF_2026_06_29_unit_profile_seam.md | missing path | 4/4 missing, e.g. docs/superpowers/plans/2026-07-01-unit-profile-arc-roadmap.md, .claude/UNIT-DATA-MODEL-RECON-1.md, .claude/MOD-SYSTEM-RECON-1.md |
| HANDOFF_2026_06_29_unit_profile_seam.md | missing MC2_ name | MC2_UNIT_PROFILE_VEHICLE_JUMP_PROOF |
| HANDOFF_2026_06_29_unit_profile_seam.md | worktree/lane/lease ref | 2 lines, e.g. "**Branches:** DOCS originally on `claude/animated-prop-cook-recon-1`; all CODE on worktree/branch **`claude/un" |
| HANDOFF_2026_07_01_vulkan_lib_track.md | missing path | 1/4 missing, e.g. docs/render-backend-seams/vulkan-island-template.md |
| HANDOFF_2026_07_01_vulkan_lib_track.md | missing MC2_ name | MC2_VULKAN_POSTPROCESS_SUBGRAPH |
| HANDOFF_2026_07_01_vulkan_lib_track.md | worktree/lane/lease ref | 1 lines, e.g. "run_vulkan_probe.py, run_golden_parity.py (+capture-liveness, framecount-normalize), check-gated-layout-macros" |
| HANDOFF_2026_07_01_vulkan_lib_track.md | OPEN/TODO/unfixed claim | 2 lines, e.g. "## OPEN follow-ups" |
| HANDOFF_tube_oracle_gpu_instancing.md | missing MC2_ name | MC2_TUBE_DEFER_PROBE |
| HANDOFF_tube_oracle_gpu_instancing.md | worktree/lane/lease ref | 3 lines, e.g. "`claude/vfx-tube-oracle-stopgap` (worktree `A:/Games/mc2-glstateguard`). Deployed to `mc2-win64-water`." |
| HANDOFF_tube_oracle_gpu_instancing.md | OPEN/TODO/unfixed claim | 2 lines, e.g. "**FOUR layered bugs, 3 FIXED + visible, 4th OPEN:**" |
| INDEX-PROCESS-agents.md | worktree/lane/lease ref | 4 lines, e.g. "- [agents-dir-canonical-root-sync](agents-dir-canonical-root-sync.md) - .claude/agents drifts between root and" |
| INDEX-PROCESS-handoffs.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "- [HANDOFF_2026_06_22_bt2018_lowerbody_anim_reversal](HANDOFF_2026_06_22_bt2018_lowerbody_anim_reversal.md) - " |
| INDEX-PROCESS-workflow.md | missing path | 2/2 missing, e.g. archive/INDEX-PROCESS-archive.md, archive/rule-own-worktree-not-root.md |
| INDEX-PROCESS-workflow.md | worktree/lane/lease ref | 12 lines, e.g. "- [2026-10 reset direction](project-2026-10-reset-direction.md) — one lane, Vulkan target, green main, gate am" |
| INDEX-PROCESS-workflow.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "- [verify-shipped-status-not-stale-docs](verify-shipped-status-not-stale-docs.md) - Recon must check shipped-s" |
| TODO_mechs_invisible_on_quickload.md | missing path | 1/1 missing, e.g. memory/HANDOFF_2026_05_20_stage_0_5_and_step_8b.md |
| TODO_mechs_invisible_on_quickload.md | OPEN/TODO/unfixed claim | 3 lines, e.g. "name: TODO — mechs invisible after savegame-restore (pause-menu Load Mission)" |
| abl-core-library-mount.md | missing path | 1/3 missing, e.g. docs/render-backend-seams/torrin-enhanced-run-recon-1.md |
| abl-core-library-mount.md | missing MC2_ name | MC2_SOAK_ |
| abl_skip_errored_modules_gate.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "- (3) STILL OPEN: a copier downscales the campaign's content atlas 1024->256 on import (the" |
| agents-dir-canonical-root-sync.md | worktree/lane/lease ref | 4 lines, e.g. "description: ".claude/agents drifts between root and worktrees (git-ignored) — root = canonical, hook auto-see" |
| always-run-session-start-preflight.md | worktree/lane/lease ref | 7 lines, e.g. "description: ALWAYS run the mandatory branch/root preflight at session start before any edit — the worktree ma" |
| anim-pipeline-map-and-greybeard-1.md | missing MC2_ name | MC2_GPU_MECH |
| anim-pipeline-map-and-greybeard-1.md | worktree/lane/lease ref | 5 lines, e.g. "Full end-to-end map doc: `A:/Games/mc2-footprint-stamp-1/docs/anim-pipeline-map-1.md` (worktree —" |
| anim-pipeline-map-and-greybeard-1.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "Render(true) STOP. OPEN recon queue (nothing built): IMPORTED-LOD-TYPEKEY-FREEZE-RECON-1 (best real" |
| anims_json_override_registry.md | missing path | 1/6 missing, e.g. data/anim_overrides/anims.json |
| archive/HANDOFF_2026_05_24_editor_bringup_nifty.md | worktree/lane/lease ref | 1 lines, e.g. "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\run-editor.bat" |
| archive/INCIDENT-modmission-fastfail-2026-06-23.md | missing path | 1/1 missing, e.g. docs/render-backend-seams/modmission-fastfail-regression-recon-1.md |
| archive/INCIDENT-modmission-fastfail-2026-06-23.md | worktree/lane/lease ref | 1 lines, e.g. "**RESOLVED 2026-06-23: FALSE POSITIVE. No code regression. Feature-lane freeze LIFTED.**" |
| archive/INDEX-MECH-archive.md | missing path | 12/12 missing, e.g. ../HANDOFF_2026_06_22_bt2018_lowerbody_anim_reversal.md, ../HANDOFF_2026_06_21_battletech_first_mech_import.md, ../mech_normals_fix_shipped.md |
| archive/INDEX-MECH-archive.md | missing MC2_ name | MC2_GPU_MECHS_LIGHTING |
| archive/INDEX-MECH-archive.md | worktree/lane/lease ref | 2 lines, e.g. "-   [Mech R→V lane Batch 1 shipped (recon + JSON + debug views)](mech_rv_batch_1_shipped.md) -- 3 commits `00e" |
| archive/INDEX-MECH-archive.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "- ⭐ [HANDOFF 2026-06-22: BT2018 imported mech LOWER-BODY ANIM REVERSAL (OPEN→now SOLVED above)](../HANDOFF_202" |
| archive/INDEX-PROCESS-archive.md | missing path | 113/114 missing, e.g. ../feedback-adversarial-meta-review-before-implementing.md, ../HANDOFF-2026-07-06-fable-close.md, ../subagent-no-delegation-rule-and-hud-arc.md |
| archive/INDEX-PROCESS-archive.md | worktree/lane/lease ref | 19 lines, e.g. "# PROCESS memory index — ARCHIVE (pre-2026-10-05 reset; many rules assume 4 parallel lanes — treat as history," |
| archive/INDEX-PROCESS-archive.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "- ⛔ [HARD RULE: verify shipped-status, not stale docs](../verify-shipped-status-not-stale-docs.md) — before an" |
| archive/build-gates-need-isolated-worktree.md | worktree/lane/lease ref | 7 lines, e.g. "name: build-gates-need-isolated-worktree" |
| archive/enemy_mech_invisible_debug.md | missing MC2_ name | MC2_MECH_VIS_TRACE |
| archive/rule-own-worktree-not-root.md | worktree/lane/lease ref | 12 lines, e.g. "name: rule-own-worktree-not-root" |
| asset_scale_subsystem.md | missing path | 4/6 missing, e.g. data/art/asset_sizes.c, data/art/asset_sizes.c, docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md |
| asset_scale_subsystem.md | worktree/lane/lease ref | 1 lines, e.g. "Subsystem added on <worktree> (2026-04-23). Handles two bug classes from loose-file upscaled UI assets:" |
| asset_viewer_mech_preview_no_simplecamera.md | missing path | 1/7 missing, e.g. gameobj.h/camera.h/terrain.h/gos_mech_batcher.h |
| asset_viewer_stage1_shipped.md | missing path | 1/3 missing, e.g. docs/superpowers/plans/2026-06-01-mc2-asset-viewer-stage1.md |
| asset_viewer_stage1_shipped.md | worktree/lane/lease ref | 1 lines, e.g. "manual GUI window run still pending user). Built on branch **claude/merge-methuselas-1**, worktree" |
| baseline_A_blessed.md | missing path | 1/2 missing, e.g. .claude/baseline-A/BASELINE-A-BLESS-PACKET.md |
| baseline_a_candidate_s9_s12.md | missing path | 2/12 missing, e.g. exe.sha256/git.c, scripts/residency_slim_report.py |
| baseline_a_candidate_s9_s12.md | worktree/lane/lease ref | 1 lines, e.g. "**S12 unified manifest schema** (`scripts/manifest_schema.py`, schema `mc2-manifest/1`): one identity block (e" |
| bldg_animation_lod_swap_unsafe.md | worktree/lane/lease ref | 2 lines, e.g. "> symptom, and do not send a lane chasing it. A building rendering wrong today is not" |
| blender-asset-pipeline-cli-arc.md | worktree/lane/lease ref | 2 lines, e.g. "**ROUND 2 SHIPPED 2026-07-14:** SUBMATERIAL/MTLID remap DONE (`ec0972ba` — 421 stock files now convert with co" |
| brain-ai-2.14-techscript-arc.md | missing path | 9/21 missing, e.g. docs/render-backend-seams/open-source-vulkan-pattern-recon-1.md, docs/render-backend-seams/modder-carver-conversion-diff-recon-1.md, docs/render-backend-seams/brain-tactic-weights-recon-1.md |
| brain-ai-2.14-techscript-arc.md | worktree/lane/lease ref | 18 lines, e.g. "**⚠ BRANCH CORRECTION (2026-06-24): the entire Brain arc lives on `claude/lightgrid-build-native-1` (the ACTIV" |
| brain-ai-2.14-techscript-arc.md | OPEN/TODO/unfixed claim | 5 lines, e.g. "**★ MILESTONE CHECKPOINT (2026-06-24): BRAIN DECOUPLING SUBSTRATE COMPLETE — nifty HEAD `5ab5b52f`.** 7-rung l" |
| brain-arc-gap-ab-1.md | missing path | 1/1 missing, e.g. mission.cpp/dispatch.cpp |
| brain-arc-gap-ab-1.md | worktree/lane/lease ref | 3 lines, e.g. "GAP A + GAP B of the Brain/AI 2.14 arc (see [[brain-ai-2.14-techscript-arc]]). Built in NEW worktree `A:/Games" |
| brain-empty-brain-skip-and-oporbd-proven.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "Closes brain-arc OPEN #1 ([[brain-missionfit-oporbd-consumer-reframe]]): in-engine OPORD" |
| brain-missionfit-oporbd-consumer-reframe.md | missing path | 1/2 missing, e.g. docs/render-backend-seams/brain-unit-varstore-recon-1.md |
| brain-symbol-resolver-and-settargetpriority.md | missing path | 1/1 missing, e.g. docs/render-backend-seams/symbolic-opord-args-recon-1.md |
| brain-symbol-resolver-and-settargetpriority.md | worktree/lane/lease ref | 2 lines, e.g. "Continuation of the Brain/AI 2.14 arc (see [[brain-arc-gap-ab-1]], [[brain-ai-2.14-techscript-arc]]). **★ MERG" |
| brainstorm_code_grounding_lesson.md | worktree/lane/lease ref | 2 lines, e.g. "**Measured citation-decay rate (2026-05-07):** PR2 Stage 0 recon dispatched against the 2026-05-01 detail-over" |
| bt2018-clip-retarget-fix.md | missing path | 1/5 missing, e.g. docs/bt2018-clip-retarget-fix-1.md |
| bt2018-clip-retarget-fix.md | worktree/lane/lease ref | 1 lines, e.g. "GLBs are NOT git-tracked / not in worktree data/tgl — they live only in the release dir," |
| bt2018-clip-retarget-fix.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "SOLVED 2026-06-22 (supersedes the OPEN bug in [[handoff-2026-06-22-bt2018-lowerbody-anim-reversal]])." |
| bt2018-material-package-contract.md | missing path | 1/1 missing, e.g. 40/mech.vert |
| bt2018-material-package-contract.md | worktree/lane/lease ref | 1 lines, e.g. "**AO-1 ENGINE SEAM — DESIGNED, ready to execute (exact sites in worktree" |
| bt2018-mech-asset-package-arc.md | missing path | 2/4 missing, e.g. docs/bt2018-mech-import-arc-summary.md, docs/bt2018-clip-retarget-fix-1.md |
| bt2018-mech-asset-package-arc.md | worktree/lane/lease ref | 11 lines, e.g. "nifty worktree (backed up to .merge_untracked_backup_*). Worktree `A:/Games/mc2-mech-node-manifest-1a`" |
| bt2018-mech-normals-1.md | worktree/lane/lease ref | 3 lines, e.g. "Worktree `A:/Games/mc2-mech-normals-1` @ branch `claude/mech-normals-1`, forked off" |
| bt2018-mech-pack-mod-recon.md | worktree/lane/lease ref | 1 lines, e.g. "Recon for packaging BattleTech-2018 mech import as a MOD + part-swap idea. All file:line in worktree `A:/Games" |
| build_dir_trap_root_vs_worktree.md | worktree/lane/lease ref | 9 lines, e.g. "name: build-dir-trap-root-vs-worktree" |
| build_fresh_worktree_bootstrap.md | worktree/lane/lease ref | 12 lines, e.g. "name: build_fresh_worktree_bootstrap" |
| c2_residual_was_conflated_zone_misattribution_real_producer_is_c6.md | missing path | 1/1 missing, e.g. docs/superpowers/plans/2026-05-17-c2-residual-fix-DECISION.md |
| campaign-generator-co-heuristics-arc.md | missing path | 3/8 missing, e.g. tools/campaign_gen/campaign_gen.py, docs/superpowers/specs/2026-06-28-campaign-generator-poc-design.md, docs/superpowers/plans/2026-06-28-campaign-generator-poc.md |
| cross_branch_vendor_borrow_needs_on_disk_parity_check.md | worktree/lane/lease ref | 4 lines, e.g. "When borrowing a vendored third-party tree from another branch/worktree" |
| debt_generated_map_pak_convergence.md | missing path | 1/4 missing, e.g. docs/editor-debug-overlay-phase2.md |
| debt_generated_map_pak_convergence.md | worktree/lane/lease ref | 2 lines, e.g. "tail in `EditorData::initTerrainFromTGA` (worktree editor/EditorData.cpp ~1561-1731:" |
| debug_instrumentation_rule.md | missing MC2_ name | MC2_WALL_SHADOW_TRACE |
| deferred_r2b_pure_static_cleanup.md | worktree/lane/lease ref | 5 lines, e.g. "description: Deferred R2b diagnostic cleanup in worktree objmgr.cpp + ROOT-not-worktree mutation-agent trap" |
| deploy-fingerprint-stale-exe-gotcha.md | worktree/lane/lease ref | 2 lines, e.g. "`scripts/run_smoke.py` runs the exe in the leased DEPLOY folder (default `A:\Games\mc2-opengl\mc2-win64-v0.4`)" |
| deploy_fingerprint_chip_2026_06_11.md | worktree/lane/lease ref | 3 lines, e.g. "**What it does:** CMake regenerates `build_fingerprint.h` per build (copy_if_different, no forced rebuild) wit" |
| deploy_must_include_compute_shaders.md | missing path | 1/2 missing, e.g. shaders/gpu_driven_terrain_solid.c |
| deploy_must_include_compute_shaders.md | worktree/lane/lease ref | 2 lines, e.g. "**Shader stages that MUST be deployed (worktree `shaders/`):**" |
| deploy_payload_completeness_gap.md | missing path | 1/1 missing, e.g. scripts/check-deploy-payload.py |
| deploy_payload_completeness_gap.md | worktree/lane/lease ref | 2 lines, e.g. "**A deploy is only valid as a matched tuple:** exe hash + shader/resource payload + data/mod payload + active " |
| deploy_payload_shader_allowlist_black_terrain.md | missing path | 1/2 missing, e.g. shaders/gos_terrain_lighting.c |
| dev-shell-1-shipped.md | missing MC2_ name | MC2_START_SCREEN |
| dev-shell-1-shipped.md | worktree/lane/lease ref | 1 lines, e.g. "- Modder GUI/imgui files live in a DIFFERENT lane (user note) — dev shell is lane-agnostic." |
| doc_accuracy_audit_2026_06_19.md | missing path | 1/6 missing, e.g. memory/csm_dynamic_shadow_architecture.md |
| doc_accuracy_audit_2026_06_19.md | worktree/lane/lease ref | 7 lines, e.g. "Conducted line-by-line verification of 7 memory files and active_campaigns.md against worktree code (nifty-men" |
| editor-crash-hardening-and-convolution.md | missing path | 1/2 missing, e.g. mainmenu.cpp/Editores.cpp |
| editor_feature_audit_ue_parity.md | worktree/lane/lease ref | 1 lines, e.g. "Worktree editor in `editor/`. Status of the wishlist:" |
| editor_feature_audit_ue_parity.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "1. **Frame-selected + focus hotkey** — TODO filed SceneOutliner.cpp:352; `Camera::setPosition` exists; centroi" |
| editor_verification_model_no_smoke.md | worktree/lane/lease ref | 1 lines, e.g. "1.  **Build path.** `cmake --build A:/Games/mc2-main/build64 --config RelWithDebInfo --target EditRel` (or equ" |
| engine-modernization-portfolio-recon.md | missing path | 5/5 missing, e.g. .claude/CONTRACT-FIRST-ENGINE-ROADMAP-1.md, .claude/PORTFOLIO-RECON-LANES.md, .claude/STATIC-PROP-MODERNIZATION-AUDIT-1.md |
| engine-modernization-portfolio-recon.md | worktree/lane/lease ref | 14 lines, e.g. "lens. Method: 11 haiku lanes → 22 sonnet verifiers (adversarial + greybeard) → coverage-gap" |
| engine_convergence_roadmap.md | missing path | 1/1 missing, e.g. docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md |
| engine_convergence_roadmap.md | missing MC2_ name | MC2_VISUALIZE |
| engine_convergence_roadmap.md | worktree/lane/lease ref | 4 lines, e.g. "description: "Master engine convergence reference (2026-05-22). 20-item tally, A/B/C workstreams, Phase 2 meta" |
| example-mods-collection-shipped.md | worktree/lane/lease ref | 2 lines, e.g. "description: "2026-07-12 example-mod tutorial collection shipped on claude/modding-examples-1 (worktree A:/Gam" |
| feedback-ask-inline-not-popup.md | worktree/lane/lease ref | 1 lines, e.g. "See [[session-bookend-reviews]] for the related rule that lanes must report honest blockers rather" |
| feedback-fallback-to-legacy-not-degraded.md | missing MC2_ name | MC2_ROAD_PBR |
| feedback-subagent-model-tiers.md | worktree/lane/lease ref | 1 lines, e.g. "description: "Subagents Sonnet-or-lower only (never Opus), split work into Haiku-sized tasks; be token aware (" |
| feedback_active_worktree_is_gpu_driven_rendering.md | worktree/lane/lease ref | 1 lines, e.g. "`...\worktrees\gpu-driven-rendering\...`, the v0.4 deploy dir, what the user" |
| feedback_always_dispatch_adversarial_review.md | worktree/lane/lease ref | 4 lines, e.g. "description: For any high-stakes plan in mc2-opengl-src, dispatch adversarial-plan-review immediately after wr" |
| feedback_build_verify_link_not_grep.md | worktree/lane/lease ref | 1 lines, e.g. "**ROOT cause of the ≈10 stale-build incidents: a CONCURRENT Claude session editing `tools/asset_viewer/` in th" |
| feedback_caveman_haiku.md | worktree/lane/lease ref | 4 lines, e.g. "description: User prefers caveman mode (full) + aggressive Haiku use for subagents in nifty worktree" |
| feedback_check_firewall_at_plan_time.md | missing path | 1/7 missing, e.g. docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md |
| feedback_data_flow_audit_asymmetry.md | missing path | 3/3 missing, e.g. docs/superpowers/explorations/2026-04-30-water-projection-skip-recon.md, brainstorms/2026-05-01-water-projection-skip-scope.md, explorations/2026-04-30-water-projection-skip-recon.md |
| feedback_dont_overgate_during_iteration.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "- If a polish issue is real but lower priority than completing the slice, document it as a follow-up (TODO/kno" |
| feedback_dont_paper_over_bugs.md | missing path | 1/1 missing, e.g. memory/msvc_incremental_link_silent_staleness.md |
| feedback_editor_data_env_stock_only_never_mods.md | missing path | 1/1 missing, e.g. data/art/Buildings.c |
| feedback_inherited_citations_must_regrep.md | worktree/lane/lease ref | 1 lines, e.g. "anywhere in worktree." Planner copied that into the verification" |
| feedback_no_menu_canary_in_smoke.md | worktree/lane/lease ref | 1 lines, e.g. "**Why:** User stated 2026-05-04 "don't run the menu canaries." The menu canary is documented in worktree CLAUD" |
| feedback_prefer_borrow_over_write_from_scratch.md | missing path | 2/2 missing, e.g. demo/nanite.cpp, demo/clusterlod.h |
| feedback_prefer_borrow_over_write_from_scratch.md | worktree/lane/lease ref | 1 lines, e.g. "-- in OTHER branches/worktrees, vendored 3rd-party libs, and upstream" |
| feedback_smoke_gate_no_menu_canary.md | worktree/lane/lease ref | 1 lines, e.g. "**Why:** User was explicit on three points: (1) do not add `--with-menu-canary` by default (was in both worktr" |
| feedback_subagent_cmake_path_explicit.md | worktree/lane/lease ref | 2 lines, e.g. "When dispatching subagents that need to invoke `cmake`, include the **explicit path** from the worktree CLAUDE" |
| feedback_subagent_no_cmake_configure.md | missing path | 1/1 missing, e.g. agile-hopper/build64/CMakeCache.txt |
| feedback_subagent_no_cmake_configure.md | worktree/lane/lease ref | 13 lines, e.g. "The MC2 worktree's `build64/` dir is configured once with explicit" |
| feedback_terrain_surface_campaign_execution_ops.md | missing MC2_ name | MC2_WATER_INVPROJ_ |
| feedback_use_mc2_build_skill.md | worktree/lane/lease ref | 5 lines, e.g. "description: Build/deploy via the mc2-build / mc2-deploy skills in the nifty worktree — never hand-roll cmake " |
| feedback_worktree_guard_boilerplate.md | worktree/lane/lease ref | 5 lines, e.g. "name: feedback_worktree_guard_boilerplate" |
| feedback_worktree_survey_scope.md | worktree/lane/lease ref | 5 lines, e.g. "name: Worktree surveys - only read nifty + gpu-driven-rendering, skip the rest" |
| forcegroupicon_cascades_widget_expansion.md | missing path | 1/1 missing, e.g. data/art/asset_sizes.c |
| fresh_worktree_cmake_configure.md | worktree/lane/lease ref | 5 lines, e.g. "name: fresh-worktree-cmake-configure" |
| fresh_worktree_configure_recipe.md | worktree/lane/lease ref | 13 lines, e.g. "name: fresh-worktree-configure-recipe" |
| glstateguard_slice2_tube_recon.md | worktree/lane/lease ref | 7 lines, e.g. "slice 1's compute-only SSBO guard to a REAL render-state pass. Recon done in worktree" |
| glstateguard_slice2_tube_recon.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "**SEPARATE OPEN GAP — gauss/AC "missing" on oracle path (NOT this fix):** user reports gauss rifle (and autoca" |
| gosfx_save_roundtrip_bugs.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "2. **OPEN (latent, flagged, NOT fixed): `DynamicMemoryStream::WriteBytes` dead override.** Base `MemoryStream:" |
| haiku-scouts-sonnet-verifies.md | worktree/lane/lease ref | 1 lines, e.g. "**Why:** In the 2026-06-29 ENGINE-MODERNIZATION-PORTFOLIO recon, 11 haiku lanes were" |
| handoff-2026-07-05-unit-profile-add-path.md | worktree/lane/lease ref | 1 lines, e.g. "Branch `claude/unit-profile-seam` @ `da9bacda` (local-only). Worktree `.claude/worktrees/unit-profile-seam`. M" |
| hud_clan_weapon_string_signed_char.md | worktree/lane/lease ref | 1 lines, e.g. "builds from `nifty-mendeleev` worktree, NOT `mc2-opengl-src` root.)" |
| hud_clan_weapon_string_signed_char.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "**OPEN follow-up:** mech bay weapon names — uses `cmponent.cpp` (`masterID` as `long`, no sign" |
| imgui_fit_ui_design.md | missing path | 2/2 missing, e.g. docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md, docs/superpowers/specs/2026-04-26-render-contract-registry-design.md |
| imgui_fit_ui_design.md | worktree/lane/lease ref | 1 lines, e.g. "2. **UI Pack system** — `data/defs/ui/<pack>/` (vanilla / omnitech / wolfmanx). Per-mod theming and full UI re" |
| integration-merge-verification-rules.md | worktree/lane/lease ref | 5 lines, e.g. "Two integration rules, both learned from real failures on the Brain-runtime arc (2026-06-23). Apply to every m" |
| launcher-scrollable-addons-and-magic-mods.md | missing path | 2/4 missing, e.g. compbas.csv/effects.c, scripts/cook_magic_icons.py |
| launcher-scrollable-addons-and-magic-mods.md | worktree/lane/lease ref | 1 lines, e.g. "**★Install dir contested:** another concurrent session overwrote `A:/Games/mc2-opengl/mc2-win64-v0.4/mc2-launc" |
| lesson-worktree-add-collision.md | worktree/lane/lease ref | 15 lines, e.g. "name: lesson-worktree-add-collision" |
| main-is-nifty-and-3rdparty-autoseed.md | missing path | 1/1 missing, e.g. docs/scripts/mcp.json |
| main-is-nifty-and-3rdparty-autoseed.md | worktree/lane/lease ref | 5 lines, e.g. "description: main branch repointed to nifty-mendeleev (2026-07-05); post-checkout hook auto-seeds 3rdparty/ in" |
| mandatory_rules_registry_golden_and_shared_nifty.md | worktree/lane/lease ref | 9 lines, e.g. "name: mandatory-rules-rendererfeatureregistry-count-golden-no-edits-in-shared-nifty-worktree" |
| material_palette_session_lessons.md | missing path | 1/2 missing, e.g. docs/superpowers/specs/2026-05-02-material-palette-sidecar-design.md |
| material_palette_session_lessons.md | missing symbol | tc_displacementAmplitude |
| material_palette_session_lessons.md | worktree/lane/lease ref | 3 lines, e.g. "- Superseded spec: worktree-relative `docs/superpowers/specs/2026-05-02-material-palette-sidecar-design.md` — " |
| mc2_init_order_widgets_before_subsystems.md | missing path | 1/2 missing, e.g. data/art/asset_sizes.c |
| mc2_mission_editor_bringup_state_and_data_env.md | missing path | 2/11 missing, e.g. SDL2/SDL.h, data/art/Buildings.c |
| mc2_mission_editor_bringup_state_and_data_env.md | worktree/lane/lease ref | 2 lines, e.g. "- `data/defs/catalogs/editor/*.fit` + `data/defs/menus/editor/*.fit` + `data/defs/text/en_us/editor/*.fit` - P" |
| mc2_path_separator_linux_build.md | missing path | 3/5 missing, e.g. nlohmann/json.h, code/profile_manager.cpp, code/profile_manager_bind.cpp |
| mc2res_fit_phase1_integrated.md | missing path | 3/6 missing, e.g. docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md, memory/imgui_fit_ui_design.md, memory/modders_paradise_roadmap.md |
| mc2x_importer_manifest_driven.md | missing path | 8/22 missing, e.g. data/art/buildings.c, data/art/buildings.c, .claude/launcher-import-design.md |
| mc2x_importer_manifest_driven.md | worktree/lane/lease ref | 3 lines, e.g. "from folder). Edit is on branch claude/mc2x-importer-1b AND was also applied to the nifty worktree" |
| mc2x_importer_manifest_driven.md | OPEN/TODO/unfixed claim | 5 lines, e.g. "stdout (console/.bat) stall the frame.** OPEN TASK (next session): gate ALL in-mission prints behind" |
| mc3_modernization_philosophy.md | missing path | 1/1 missing, e.g. docs/superpowers/cpu-to-gpu-offload-orchestrator.md |
| mclib-greybeard-bounds-queue-closed.md | missing path | 1/1 missing, e.g. .claude/TXMMGR-BOUNDS-VERIFY-1.md |
| mclib-greybeard-bounds-queue-closed.md | worktree/lane/lease ref | 1 lines, e.g. "Process note: most WATCHID/MECH items were fixed by a PARALLEL session executing the same recon — see [[prefli" |
| mco_mech_csv_format.md | missing path | 1/1 missing, e.g. archive/carver5_stability_session.md |
| mco_omnitech_integration_attempt.md | worktree/lane/lease ref | 6 lines, e.g. "description: Omnitech campaigns boot + briefing + mission-load runs for ~67s before AI-brain crash; same class" |
| mcp-repo-intel-bound-to-nifty.md | missing path | 1/1 missing, e.g. .claude/WORKTREE-GUARD.md |
| mcp-repo-intel-bound-to-nifty.md | worktree/lane/lease ref | 7 lines, e.g. "description: "The mc2-repo-intel MCP server is pinned to the nifty-mendeleev worktree — its preflight/gate_sta" |
| mech-footfall-decal-recon-1.md | missing path | 1/3 missing, e.g. gos_PushDecal/decalBatch_/decal.frag |
| mech-footfall-decal-recon-1.md | missing MC2_ name | MC2_FOOTFALL_MATERIAL |
| mech-footprint-stamp-recon-1.md | missing path | 1/1 missing, e.g. tools/footprint/gen_footstamp_mask.py |
| mech-footprint-stamp-recon-1.md | missing MC2_ name | MC2_FOOTPRINT_STAMP |
| mech-footprint-stamp-recon-1.md | worktree/lane/lease ref | 2 lines, e.g. "crater.cpp/.h (GLSTATE-TEXUNIT + overlay-routing lanes concurrently touch it)." |
| mech-import-1b-runtime-animation.md | missing symbol | clipGlobal |
| mech-import-1b-runtime-animation.md | worktree/lane/lease ref | 1 lines, e.g. "The lanes exist: `GpuMechVertex.boneIndices[4]`/`boneWeights[4]` (loc3/4), unbounded flat" |
| mech_fast_transform_body_shipped.md | missing path | 1/1 missing, e.g. docs/superpowers/plans/progress/2026-05-09-handoff-mech-shadow-fast-transform.md |
| mech_hud_icon_atlas_cveg.md | missing path | 1/4 missing, e.g. data/art/asset_sizes.c |
| mech_hud_icon_atlas_cveg.md | worktree/lane/lease ref | 1 lines, e.g. "worktree, NOT `A:/Games/mc2-opengl-src` root — edits to root do nothing (root CLAUDE.md" |
| mech_hud_icon_atlas_cveg.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "**OPEN follow-up:** weapon descriptor strings show `<missing-string:legacyId:>` /" |
| mech_paint_and_mipmap_system.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "**Paint-instance cache key collision bug (still present, unfixed):** `mclib/mech3d.cpp:1807-1810` hashes paint" |
| mech_vehicle_gpu_pull_in.md | missing path | 1/3 missing, e.g. mech.vert/mech.frag |
| mech_vehicle_gpu_pull_in.md | missing MC2_ name | MC2_GV_SHADOW_SKIP |
| mechlab-mods-ferro-jumpjets-weapons-engines.md | missing path | 2/3 missing, e.g. mods/magic-ballistic-weapons/data/objects/compbas.c, scripts/cook_magic_icons.py |
| mechlab-mods-ferro-jumpjets-weapons-engines.md | worktree/lane/lease ref | 2 lines, e.g. "- PLAN (gated `MC2_ENGINE_XL`, default off): data flip id2/id3 Type→Engine; in resetComponents handle id2/3 gr" |
| merge_chain_water_to_gpu_to_nifty_and_depthbias_header_landmine.md | worktree/lane/lease ref | 4 lines, e.g. "**Worktree/deploy refinement (user, 2026-05-18, post-merge):** "nifty is" |
| merge_methuselas_clean_and_buildable.md | worktree/lane/lease ref | 6 lines, e.g. "**`claude/merge-methuselas-1`**, worktree `A:/Games/mc2-merge-methuselas`." |
| merge_worktree_collision_rule.md | worktree/lane/lease ref | 5 lines, e.g. "name: merge-worktree-collision-rule" |
| methuselas_techscript_proposal.md | missing path | 1/1 missing, e.g. docs/superpowers/specs/2026-04-30-techscript-primary-lua-sidecar-design.md |
| minimal_touch_modern_when_touched.md | missing path | 2/2 missing, e.g. PROJECT.md/CLAUDE.md, PROJECT.md/CLAUDE.md |
| minimal_touch_modern_when_touched.md | worktree/lane/lease ref | 2 lines, e.g. "- "Let's propagate this doc change to all worktrees for consistency" when the substantive content is already e" |
| mission-generator-shape-arc.md | missing path | 1/2 missing, e.g. docs/superpowers/specs/2026-06-28-mission-generator-shape-design.md |
| mission-generator-shape-arc.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "- **Two archetypes** (mc2_03): OPEN outpost (batteries, no walls/gates) vs CLOSED compound" |
| mission_load_inits_mirror_init_per_subsystem.md | missing symbol | primeMissionSubsystems |
| modder-workflow-first-loop.md | missing path | 2/3 missing, e.g. docs/superpowers/plans/modder-workflow-first-loop.md, .modproject/editor.json |
| modder-workflow-first-loop.md | worktree/lane/lease ref | 2 lines, e.g. "**Slice 3 queued** (Open/New Mod Project, mod-aware save paths) â€” prompt in plan doc. Worktree dirty WIP (go" |
| modder-workflow-first-loop.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "**Playtest Results panel v0 SHIPPED** (`2742a22b` panel + `2fb1bb89` harness, branch claude/tacmap-formation-l" |
| modders_paradise_roadmap.md | missing path | 1/1 missing, e.g. docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md |
| modders_paradise_roadmap.md | missing symbol | reloadFromDisk |
| model_override_mod_merge_rootcause.md | worktree/lane/lease ref | 1 lines, e.g. "- Shared nifty worktree is DIRTY with other-session WIP (editor/*, KtxLoader BC7) — stash across merges, do NO" |
| modernization-roadmap-northstar.md | worktree/lane/lease ref | 3 lines, e.g. "in the nifty-mendeleev worktree. Treat as the authoritative sequencing doc for engine modernization." |
| named_contracts_at_intersections.md | missing path | 1/4 missing, e.g. docs/render-pipeline-matrix.md |
| no-chips-use-subagents.md | worktree/lane/lease ref | 4 lines, e.g. "description: "User preference: never park work as spawn_task chips — dispatch subagent lanes directly"" |
| no_stashing_foreign_wip_in_shared_worktree.md | worktree/lane/lease ref | 8 lines, e.g. "name: no_stashing_foreign_wip_in_shared_worktree" |
| ns3_engine_standalone_boundary_surface_and_metafix.md | worktree/lane/lease ref | 1 lines, e.g. "**MILESTONE ACHIEVED 2026-05-18.** `mclib_standalone` (`MC2_BUILD_ENGINE_STANDALONE`, `/WHOLEARCHIVE`) in `cla" |
| one-committer-per-worktree-rule.md | worktree/lane/lease ref | 8 lines, e.g. "name: one-committer-per-worktree-rule" |
| parity-gate-deploy-rule.md | missing symbol | mul_rowmajor |
| parity-gate-deploy-rule.md | worktree/lane/lease ref | 2 lines, e.g. "**No Vulkan/GL shader/uniform fold may deploy to a user lane unless the REAL GL byte-identical parity gate act" |
| parked-2026-07-09-limit-rescue.md | missing path | 1/2 missing, e.g. .claude/GATE-RETIREMENT-PLAN-1.md |
| parked-2026-07-09-limit-rescue.md | worktree/lane/lease ref | 1 lines, e.g. "- **Workflow: macOS port roadmap** — 6/8 recon cached. Resume: `Workflow({scriptPath: 'C:\\Users\\Joe\\.claude" |
| policy_split_wrapper_grep_trap.md | missing path | 1/4 missing, e.g. docs/superpowers/specs/projectz-callsite-inventory.md |
| ppc_decurve_speed_knobs.md | missing path | 1/3 missing, e.g. docs/ppc-flight-and-fx-anim-moddability-recon.md |
| preflight-shared-recon-queue-collisions.md | worktree/lane/lease ref | 3 lines, e.g. "**Merge mechanics into a contended shared worktree (nifty):**" |
| premerge-untracked-cleanup-hazard.md | missing path | 1/1 missing, e.g. docs/bt2018-part-swap-manifest-recon-1.md |
| premerge-untracked-cleanup-hazard.md | worktree/lane/lease ref | 8 lines, e.g. "description: NEVER blanket-delete untracked files by name pattern before a merge — it silently destroys concur" |
| project-2026-10-reset-direction.md | worktree/lane/lease ref | 5 lines, e.g. "description: 2026-10-05 reset after scope creep — ONE lane, Vulkan is the target backend, green main, gate amn" |
| project-mod-compat-targets.md | worktree/lane/lease ref | 1 lines, e.g. "**How to apply:** treat mod compatibility as a release gate; prefer an offline/automated mod-load check over m" |
| project-next-tooling-modernization-plan.md | worktree/lane/lease ref | 2 lines, e.g. "1. **clangd**: `CMAKE_EXPORT_COMPILE_COMMANDS=ON` is set but build64 uses the Visual Studio generator, which i" |
| project_nvidia_hardening_s2.md | worktree/lane/lease ref | 1 lines, e.g. "Full spec + status table: `docs/nvidia-hardening-s2.md` in the nifty worktree." |
| public_fork_and_release.md | worktree/lane/lease ref | 3 lines, e.g. "- **Default branch on fork:** `main` (pushed from local `<worktree>` branch)" |
| rb-viewing-stack-provenance-2026-07-02.md | worktree/lane/lease ref | 5 lines, e.g. "- **Exe identity untrustworthy:** the `0.5 testing/mc2-win64-v0.5.0` slot is coordinator-owned + churned by ma" |
| recon_classify_by_class_id_not_just_name.md | missing path | 2/2 missing, e.g. docs/superpowers/plans/2026-05-20-B1-Stage-0-content-recon.md, docs/superpowers/plans/2026-05-20-integrated-gosfx-retirement-gpu-particles-plan.md |
| refresh-tooling-at-slice-boundaries.md | missing MC2_ name | MC2_CLIFF_FRAME_DUMP |
| refresh-tooling-at-slice-boundaries.md | worktree/lane/lease ref | 2 lines, e.g. "**Why:** long multi-slice arcs accrete tooling debt — capture recipes go stale, agents relaunch the app the wr" |
| render_architecture_recon_2026_06_11.md | missing path | 1/2 missing, e.g. docs/render-architecture-recon-INDEX.md |
| render_architecture_recon_2026_06_11.md | worktree/lane/lease ref | 3 lines, e.g. "2026-06-11: Ten parallel recon docs written to nifty worktree `docs/`, indexed in `docs/render-architecture-re" |
| renderdoc_pixel_history_diagnostic.md | missing path | 1/1 missing, e.g. archive/gosfx_white_saturation_bug.md |
| repo_intel_preflight_baseline.md | worktree/lane/lease ref | 9 lines, e.g. "py -3 tools\repo_intel\repo_query.py preflight --expect-branch <branch> --expect-root <absolute-worktree-root>" |
| rule0_applies_to_reviewer_proposed_fixes_too.md | worktree/lane/lease ref | 1 lines, e.g. "During Water v2 S3 (2026-05-17, isolated worktree `claude/water-material-v1`)," |
| run_smoke_env_allowlist_drops_probe_vars.md | missing MC2_ name | MC2_GPU_, MC2_PROJECTZ_, MC2_TERRAIN_ |
| run_smoke_kill_existing_causes_flaky_crash.md | worktree/lane/lease ref | 1 lines, e.g. "**Fix: omit `--kill-existing`.** Observed 2026-05-28 across the water-rv-lane" |
| s2-dynamic-actor-already-migrated.md | worktree/lane/lease ref | 2 lines, e.g. "S2a attribution (2026-06-10, worktree `A:/Games/mc2-s2-dynamic-attribution`, branch `claude/s2-dynamic-actor-a" |
| shader_deploy_gap_and_hook.md | missing path | 1/4 missing, e.g. docs/mech-viewuniforms-source-dump.md |
| shader_deploy_gap_and_hook.md | worktree/lane/lease ref | 4 lines, e.g. "worktree. A deploy that copies only `mc2.exe` runs **stale shaders** — and a" |
| shadow_quality_upgrade.md | worktree/lane/lease ref | 1 lines, e.g. "Shadow system upgraded (Apr 2026) on `claude/<worktree>`:" |
| shared_worktree_index_race.md | worktree/lane/lease ref | 11 lines, e.g. "name: shared-worktree-index-race" |
| shell_tooling_environment_no_profile_and_explicit_paths.md | worktree/lane/lease ref | 2 lines, e.g. "- `cmake` may be available, but if it is not, use the explicit Visual Studio CMake path from the active worktr" |
| smoke_pass_does_not_verify_gpu_driven_shader_fix.md | worktree/lane/lease ref | 2 lines, e.g. "`A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe` (worktree-specific target — see" |
| startup-async-unguarded-operationfile-crash.md | worktree/lane/lease ref | 1 lines, e.g. "**FIXED 2026-06-21** (uncommitted on nifty worktree, in 0.5.0 release build): primary fix in `code/logistics.c" |
| staticprop_per_frame_cpu_retire_campaign.md | missing MC2_ name | MC2_SNAPSHOT_STATIC_PROP_CACHE, MC2_SNAPSHOT_STATIC_PROP_CACHE_COMPARE, MC2_STATIC_VISIBLE_COMPACT |
| staticprop_per_frame_cpu_retire_campaign.md | worktree/lane/lease ref | 2 lines, e.g. "**Branch:** `claude/nifty-mendeleev` (the canonical worktree IS main now). Spec: `docs/superpowers/specs/2026-" |
| subagent-no-delegation-rule-and-hud-arc.md | missing path | 3/5 missing, e.g. .claude/IN-MISSION-HUD-IMGUI-SCOPE-1.md, .claude/IN-MISSION-HUD-IMGUI-SCOPE-1.md, docs/G2-GL-FRAME-PROFILE-RUNBOOK.md |
| superpowers_train_s1_s8.md | worktree/lane/lease ref | 1 lines, e.g. "**Merge discipline (proven this train):** disposable worktree per merge ([[merge-worktree-collision-rule]]); e" |
| tacmap_formation_line_v1.md | missing path | 4/5 missing, e.g. docs/superpowers/specs/2026-06-11-tacmap-formation-line-v1-design.md, docs/superpowers/plans/2026-06-11-tacmap-formation-line-v1.md, stuff/stuff.h |
| tacmap_formation_line_v1.md | worktree/lane/lease ref | 2 lines, e.g. "Branch `claude/tacmap-formation-line-v1` (off `claude/terrain-gen-pcg` in nifty worktree), HEAD `73e07c41`. Bu" |
| tactical_overview_camera.md | missing path | 4/11 missing, e.g. v0.3/launch_overview.bat, docs/superpowers/specs/2026-06-10-tactical-overview-camera-design.md, docs/superpowers/plans/2026-06-10-tactical-overview-camera.md |
| tactical_overview_camera.md | worktree/lane/lease ref | 3 lines, e.g. "Branch **`tactical-map`** (off `claude/nifty-mendeleev`), worktree" |
| techbrain-conversion-drive-combat.md | missing path | 1/4 missing, e.g. scratchpad/brain_parity.sh |
| techbrain-conversion-drive-combat.md | missing MC2_ name | MC2_BRAIN_ENGAGE_RADIUS |
| track_a1_object_admission_predicate.md | missing path | 2/11 missing, e.g. docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md, docs/superpowers/mc3-rendering-modernization-roadmap.md |
| track_c_command_readability.md | worktree/lane/lease ref | 3 lines, e.g. "description: "Track C Command Readability — zoom presentation ladder (4 bands), unit icons, LOS/sensor overlay" |
| track_d_assimp_axis_convention.md | missing MC2_ name | MC2_ASSIMP_FLIP_Z, MC2_ASSIMP_NO_WRAP_QUAT, MC2_ASSIMP_SCALE, MC2_ASSIMP_ZUP |
| track_d_assimp_axis_convention.md | missing symbol | FindNodeByName |
| track_d_assimp_mvp_done.md | missing path | 1/10 missing, e.g. docs/superpowers/explorations/2026-05-02-track-d-mvp-adversarial-findings.md |
| track_d_assimp_mvp_done.md | worktree/lane/lease ref | 2 lines, e.g. "**Branch:** `claude/assimp-testing` (worktree: `.claude/worktrees/assimp-testing/`)." |
| track_d_e1_skin_import_shipped.md | missing MC2_ name | MC2_ASSIMP_SCALE |
| track_d_e2_psa_gltf_anim_tool.md | missing path | 2/2 missing, e.g. tools/psa_to_gltf_anim.py, tools/psa_to_gltf_anim.py |
| track_d_glb_pipeline_shipped_batch_findings.md | missing path | 1/1 missing, e.g. tools/fbx2gltf_batch.py |
| track_d_glb_pipeline_shipped_batch_findings.md | missing MC2_ name | MC2_ASSIMP_NO_AUTOFIT, MC2_ASSIMP_SCALE |
| track_d_glb_pipeline_shipped_batch_findings.md | missing symbol | LoadAnimationFromFile |
| track_d_glb_pipeline_shipped_batch_findings.md | worktree/lane/lease ref | 2 lines, e.g. "**Branch:** `claude/assimp-testing` (worktree `.claude/worktrees/assimp-testing/`)" |
| track_d_slice_a_shipped.md | missing path | 1/3 missing, e.g. include/render_contract.h |
| track_d_slice_a_shipped.md | worktree/lane/lease ref | 2 lines, e.g. "GPU Mech Batcher Slice A landed on `claude/gpu-mech-batcher` worktree at `.claude/worktrees/gpu-mech-batcher`." |
| track_d_slice_b1_shipped.md | worktree/lane/lease ref | 1 lines, e.g. "VS-side calc_light landed on `claude/gpu-mech-batcher` worktree." |
| tracy_profiler.md | missing path | 1/1 missing, e.g. tracy/Tracy.h |
| tracy_profiler.md | worktree/lane/lease ref | 1 lines, e.g. "Tracy profiler integrated into MC2 build (branch claude/<worktree>, Apr 2026)." |
| ui-defs-lockstep-like-shaders.md | missing path | 1/1 missing, e.g. .claude/BLACK-MENU-BISECT-1.md |
| upcoming_features.md | missing path | 1/4 missing, e.g. shaders/lut.frag |
| upcoming_features.md | missing symbol | glTexParameterf |
| user-product-install-vs-lane-installs.md | worktree/lane/lease ref | 5 lines, e.g. "name: user-product-install-vs-lane-installs" |
| user-product-install-vs-lane-installs.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "**Why:** a full day was spent fixing complaints the user re-reported as unfixed. The" |
| user_project_focus_and_collaborator.md | worktree/lane/lease ref | 2 lines, e.g. "**User's lane (renderer/perf/pretty):**" |
| validation_scaffold_preflight_guardrails.md | missing path | 2/10 missing, e.g. docs/toolchain-bom-generated-draft.md, tools/validate_asset_manifest.py |
| validation_scaffold_preflight_guardrails.md | worktree/lane/lease ref | 6 lines, e.g. "description: Preflight checks/tests added before the asset-pipeline + HZB lanes — what to run and extend" |
| verify-landmines-smoke-and-watchid.md | worktree/lane/lease ref | 1 lines, e.g. "Verification landmines hit while shipping WATCHID-BOUND-1 (2026-06-22, worktree `A:/Games/mc2-watchid-bound-1`" |
| verify-shipped-status-not-stale-docs.md | worktree/lane/lease ref | 4 lines, e.g. "**★ROOT-vs-NIFTY read asymmetry (the deeper root cause, 2026-06-30 16-lane re-audit):** the MCP" |
| verify-shipped-status-not-stale-docs.md | OPEN/TODO/unfixed claim | 3 lines, e.g. "description: Recon must check shipped-status (registry/HANDOFF/code) before claiming a TODO/win; perf numbers " |
| vfx_mc2_10_intro_particle_window.md | worktree/lane/lease ref | 1 lines, e.g. "`run_smoke.py` (it taskkills other lanes'/the user's running mc2.exe)." |
| vfx_mesh_lane_ownership.md | missing path | 1/2 missing, e.g. include/terrain_mat_layers.h |
| vfx_mesh_lane_ownership.md | missing MC2_ name | MC2_VFX_ORACLE_DEBRIS |
| vfx_mesh_lane_ownership.md | worktree/lane/lease ref | 12 lines, e.g. "name: vfx-mesh-lane-ownership" |
| visual_fidelity_roadmap.md | missing path | 15/17 missing, e.g. docs/superpowers/specs/2026-06-04-trackv-visual-fidelity-roadmap.md, include/ambient.h, docs/superpowers/plans/2026-06-04-vl1-filmic-color-baseline-plan.md |
| visual_fidelity_roadmap.md | missing MC2_ name | MC2_FILMIC, MC2_GODRAYS, MC2_GODRAY_INTENSITY, MC2_MECH_IBL |
| visual_fidelity_roadmap.md | missing symbol | calcHemisphereAmbient, applyShadowNormalOffset, runPostFog |
| visual_fidelity_roadmap.md | worktree/lane/lease ref | 14 lines, e.g. "description: "Track V visual fidelity roadmap targeting MW5/UE4-era quality. 2026-06-04 RECON REFRAME (TRACKV-" |
| visual_fidelity_roadmap.md | OPEN/TODO/unfixed claim | 1 lines, e.g. "User soaked MC2_FILMIC → **way overexposed / all white / flat**. Root cause = **double gamma-encode**: Narkowi" |
| wolfman_is_max_zoom.md | worktree/lane/lease ref | 1 lines, e.g. "- For features that ARE actually wolfman-mode-specific (mc2x integration, extended LOD), see `mc2x_wolfman_ref" |
| zoomed_out_big_map_is_an_unexercised_stress_path_that_hides_full_map_regressions.md | missing path | 2/2 missing, e.g. memory/feedback_soak_waiver_with_probes_and_reviews_validated.md, memory/smoke_autonomous_run_pattern.md |
