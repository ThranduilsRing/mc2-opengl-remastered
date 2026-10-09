# Dead-weight inventory (2026-10-09, read-only at HEAD dcc40a886) — DELETE-now ≈224 files / 38k lines / 167 MB; engine #if 0 5.5k lines; ASK list of 11

Owner rule (2026-10-09): superseded test infrastructure and dead code get deleted after a provenance audit; tag locally first (`archive/dead-infra-2026-10-09`).

Method: inbound-reference index over 4,082 tracked text files (3rdparty excluded) + the memory dir. Generated tables (`gate_table.seed.json`, `docs/*.json|yaml`, baselines, allowlists, `.claude/cleanup/*`) do not count as consumers. Nothing built or run.

Already done, do not redo: `d6e722e59` (2026-10-07) deleted 82 unbuilt sources (13k lines); `scout-unbuilt.md` is ~90% stale; 17 of 22 `scout-scripts.md` orphans already gone; `e864ee59b` fixed the `asset_viewer` link.

## 1. tools/ spikes, probes, one-offs

| Path | Files/lines | Last | Inbound | Superseded by | Verdict |
|---|---|---|---|---|---|
| `tools/vk_gl_interop_spike` | 18 / 5942 | 07-04 f781564b8 | exemption lines `check-raw-gl-depthfunc.py:80`, `-depthmask.py:83`; comment `check-lighting-divergence.py:110`; 3 seed rows | VK islands, strict no-interop runner | DELETE |
| `tools/vk_hwnd_present_probe` | 2 / 757 | 07-03 | seed row | `engine_present_vk.cpp` | DELETE |
| `tools/vk_present_smoke` | 3 / 957 | 07-03 | comment `engine_present_vk.cpp:9`; seed | `engine_present_vk.cpp` | DELETE |
| `tools/vk_interop_transport_test` | 5 / 1096 | 07-03 | none | — | DELETE |
| `tools/vk_gl_buffer_interop_probe` | 5 / 489 | 07-04 | none | water island | DELETE |
| `tools/vk_shadow_sampler_probe` | 2 / 705 | 07-04 | prose `tier1_env_vars.md:141` | `vulkan_shadow_island_live.cpp` | DELETE |
| `tools/tgl_loader_standalone_spike` | 3 / 544 | 06-02 | header comments in asset_viewer/tg_import_dump stubs | `tg_import_dump` | DELETE |
| `tools/rendercore_standalone_spike` | 2 / 283 | 06-02 | `docs/engine-standalone-seams.md:353` | `tests/unit` | DELETE |
| `tools/engine_standalone_stub.cpp` + `mclib_standalone` (`MC2_BUILD_ENGINE_STANDALONE`) | 1 / 36 + CMake block | 06-10 | root CMake | mission-load design: "cannot be a base" | DELETE (root CMake = lane 2 window) |
| mc2_01 dressing one-offs (`place_marblecliff_mc2_01.py`, `place_cliffwall_mc2_01.py`, `condition_coastal_cliff_glb.py`, `gen_cliff_wall_glb.py`, `install_cliff_dressing.py`, 2 `marblecliff*_fit_template.txt`) | 7 / 1068 | 07-01..03 | each other + .claude | none | DELETE |
| `install_tree_scatter.py` + 3 `pinescatter*_fit_template.txt` | 4 / ~210 | 07-01 | `terrain_beautify/scatter_tool.py` | — | ASK (with scatter_tool) |
| `tools/asset_viewer` | 82 / 8154 | 10-09 | default build (EXISTS guard); 18 docs | — | KEEP |
| `tools/tg_import_dump` | 5 / 631 | 06-22 | `mech_bone_parity.py`, `mech_shot.py`, GLB pack harness | — | KEEP |
| 16 registered harnesses (`REGISTERED_HARNESSES` in `run_contract_tests.py`) | — | — | runner | — | KEEP |
| `tools/brain_dispatch_harness` | 23 / 1827 | 07-01 | `unregistered-harness-debt.txt`; `run_brain_apply_ab.ps1`; 37 live fixtures | — | ASK (not run by any runner) |
| `brain_missionfit_oporbd_test`, `brain_symbol_resolve_test` | 264, 120 | 07-16, 06-25 | header comments | — | ASK (fold into tests/unit or delete) |
| `tools/mech_import_harness` | 3 / 553 | 06-22 | debt row; `mech_bone_parity.py` | — | ASK |
| `tools/cpp_probes` (assimp_probe, meshopt_lod_probe, meshopt_cook) + `3rdparty/meshoptimizer` (1.6 MB) | 4 / 790 | 07-02 | `ENABLE_ASSET_CPP_PROBES` default OFF | — | ASK |
| `mc2fx`, `mc2fx_preview`, `mc2weapon_gui` | 8+2+6 | 06-10..15 | `ENABLE_MC2FX*` never declared as option; `deploy-mc2fx-tools.sh`, docs | — | ASK |

Notes: deleting `brain_dispatch_harness`/`mech_import_harness` requires removing their `scripts/unregistered-harness-debt.txt` rows in the same commit. Deleting the VK spikes + rendercore spike + D7 probe drops 5 unregistered seed rows (`MC2_COLORCOWRITE_NEGCTL`, `MC2_SAMPLETEX_NEGCTL`, `MC2_SHADOWCMP_NEGCTL`, `MC2_SPIKE_VERBOSE`, `MC2_VK_NEG_TEST`) — regenerate the seed. `tools/vulkan_shader_probe_main.cpp` + `scripts/run_vulkan_probe.py` are D7 (in progress).

## 2. scripts/ and tools/*.py orphans (83 of 494 files, 18.6k lines, zero live refs)

| Path | Lines | Verdict |
|---|---|---|
| `scripts/qwen_local.py`, `docs/qwen-local-workflow.md`, `docs/qwen-system-prompts.md` | 423+76+38 | DELETE (replaced by `local_llm` MCP) |
| `scripts/slice_gate.py` | 299 | DELETE |
| `scripts/bt2018_retarget_clips.py` | 115 | DELETE |
| `scripts/smoke_lib/evidence_classifier.py` + `tests/smoke/test_evidence_classifier.py` | 196+ | DELETE (dead module + dead test) |
| `smoke_lib/oracleparse.py`, `scripts/oracle_report.py`, `scripts/render_pass_report.py` + 2 tests | 242+128+274 | ASK (chain with no gate/runner) |
| `parse-done-reports.py`, `LANE_COMPLETION_REPORT.template.md`, `test_deploy_lease.py`, `test_smoke_lane_lock.py`, `tools/spawn_lane.py`, `tools/test_spawn_lane.py` | ~1.4k | ASK (multi-lane tooling; "delete multi-lane checks" queue item) |
| one-off measurement scripts: `measure_tile_boundary`, `predict_shore_edge`, `water_class_flip_audit`, `terrain_grounding_audit`, `terrain_height_audit`, `object_burial_audit`, `tools/albedo_masked_audit.py`, `vk_shadow_stale_discriminator`, `whatran` | 13 files / 6.6k | ASK (evidence generators for closed arcs) |
| `tests/terrain/object_burial_*.csv` (299 KB), `terrain_*_audit_*.md` | 4 | ASK (regenerable) |
| `verify_campaign_assets`, `verify_executor_slice`, `verify_mod_roundtrip`, `deploy-mc2fx-tools.sh`, `run_smoke_asan.py`, `run_editor_playtest_smoke.py` | 157–569 each | ASK |
| `tools/make_handoff.py` | 112 | KEEP (MEMORY.md lists it) |
| `mech_bone_parity.py`, `run_vk_field_check.py`, `installer/sign_release.ps1`, `configure_analysis.py` | — | KEEP |

Dark pytest: 76 test modules outside `scripts/tests/` (14.7k lines) never run by any gate — `tools/terrain_beautify` (13), `unit_converter` (8), `blender_pipeline` (8), `tests/smoke` (11), `scripts/test_*.py` (5). Mostly test live tools; do not mass-delete (ASK 3).

## 3. tests/ fixtures and baselines

| Path | Size | Readers | Verdict |
|---|---|---|---|
| `tests/visual/baselines/`: 8 `baselineA-*` dirs, 63 root `staticprop_baseline*`/`arc_staticprop_*`, `V-IBL-STATIC-1-SOAK.md` | 117 files / 84.4 MB | `.gitignore:192` ignores the dir yet files are tracked; only `set.json` globs in `check-capture-env-declared.py`/`check-capture-provenance.py` (tolerate empty) | DELETE (golden is `tests/visual/goldens/reset-50331d5`) |
| `tests/visual/baselines/README.md`, `presets.json` | 2 | `capture_baseline.py`, MCP `run_capture_baseline` | KEEP |
| `tests/visual/golden-sets.json` | 1 | `run_visual.py`, `run_visual_capture.py`, `check-manifest-schema.py`; paths point into an old worktree | ASK 6 |
| `tests/smoke/baselines/pre-trackd-mvp.json` | 1.4 KB | none | DELETE |
| `tests/fixtures/abl_compile_fatal/broken.abl` | 89 B | none | DELETE |
| shader_variants splice fixtures, smoke matrices (9), default_build_quiet baseline, brain/asset/asset_viewer fixtures, `tests/terrain/beautify` (60 MB) | — | live checks | KEEP |
| unbuilt test sources: `tests/smoke/brain_task_queue_test.cpp` (155), `GameOS/gameos/tests/gos_terrain_arm_logic_test.cpp` (105), `editor/tests/object_recent_ring_test.cpp` (73), `RenderCore/test_bc7_decode.cpp` (49) | 382 | no CMake target | DELETE (editor one = lane 2 path) |

## 4. Engine dead code

`#if 0` regions > 20 lines: 61 blocks, 5,481 lines, 44 files (all regions: 6,260 lines / 92 files). Verdict DELETE.

| File | Dead lines |
|---|---|
| `mclib/mlr/gosvertex.cpp` | 571 of 647 |
| `mclib/mlr/gospoint.cpp` | 562 of 582 |
| `mclib/stuff/matrix.cpp` | 412 |
| `mclib/stuff/affinematrix.cpp` | 348 |
| `code/team.cpp` | 264 |
| `code/warrior.cpp` | 207 (261 total) |
| `mclib/camera.cpp` | 222 (dirty in tree) |
| `code/gvehicl.cpp` | 217 |
| `mclib/mlr/mlr_i_*tmesh.cpp` (7) | ~114 each, ~880 |
| `mclib/mlr/mlrloadobj.cpp` | 155 |
| `mclib/gosfx/tube.cpp` | 127 |
| `code/mech.cpp` | 170 |
| `mclib/stuff/{namelist,rotation,angle}_test.cpp` | 181+123+67 (stubbed test bodies) |
| `code/ablmc2.cpp` | 77 |
| `code/mover.cpp` | 83 |
| `mclib/vfxtile.cpp` | 89 |

Split-plan extras (DELETE, fold into split commits): ~720 debug lines in `mover.cpp` (+172 `DebugMoveChunk`/`DebugStatusChunk`), ~25 dead `#define`s in `mech.cpp`, ~1.4k comment scaffolding + 6 `MC2_ABL_TRACE` stubs in `ablmc2.cpp`, `OLD_ACKNOWLEDGE_MSG`/`USE_ABL_LOAD` in `warrior.cpp`.

Gate retire verdicts (seed): 78 tierB:MERGE, 2 tierB:DELETE, 1 tierA:NEEDS-READER-REMOVAL — owned by the gate program. HOLD: `MC2_STABLE_LIGHT_SKIP_DIAG`, `MC2_VK_PROP_LIGHT_PROBE`; `MC2_DRAW_PACKET_STATIC_PROP_V6` only in the enum renumber.

Shaders: `shaders/gos_grass.frag` (51) + `.geom` (151): no loader, `render_contract.cpp:553` says "gos_grass is dead", only ref `check-lighting-divergence.py:293` row → DELETE. `shaders/overlay_alpha_clear.frag` (16): no refs → DELETE. `tools/shader_reflect/expected/` JSONs for those three plus `bloom_blur`, `bloom_threshold`, `godray` (shaders gone) → DELETE. `shadow_object`/`shadow_terrain` loaded via `gosRenderMaterial::load("shadow_*")` → KEEP.

Unbuilt leftovers: `code/actor.cpp` (532) DELETE; editor headers `SunDlg.h`, `TGaWnd.h`, `TACMapTGA.h`, `fractaldialog.h` (only `EditorMFC.vcproj`) DELETE (lane 2); `GameOS/include/mfcplatform.hpp` (21) DELETE.

Tracked junk: `editor/.vs/**` (37.5 MB) DELETE; `Tracy/*.tracy` (25.4 MB) DELETE; `Viewer/` (root CMake add_subdirectory commented "TEMP throwaway"; 19 MB `mission.fst`) DELETE — 4 contract scripts scan `Viewer` (`check-gate-default-agreement`, `check-input-capture-visibility`, `check-res-aspect-contract`, `check-tgl-load-callers`): confirm missing-dir tolerance; 7 legacy VS2003 `.vcproj` (13.9k lines) DELETE; `dev/null/{post-checkout,post-commit,post-merge,pre-push}` DELETE; `launch_recon_mission.ps1`, `TODO` (2017), repo `memory/HANDOFF_2026_05_26_*.md` (3) DELETE. Keep `screenshots/hero.png`.

## 5. Docs (304 files, 79k lines)

Zero-inbound: `docs/military.material_suggestions.json`, `docs/rusty_quonset.material_suggestions.json`, `docs/render-backend-seams/pipeline-state-occupancy.json`, `docs/render-backend-seams/subgraph-contiguity-rule.md` (verify) → DELETE; the two qwen docs (+ `docs/README.md` rows) → DELETE.
85 `.md` (17.5k lines) reachable only from other docs, all historical arcs → ASK: `docs/superpowers/` (17), `docs/observations/` (12), `docs/plans/` (7), `docs/recon/` (5), `docs/render-backend-seams/*-recon-1.md` (10), root `terrain-8z-*`, `*-rv-arc-*`, `trackv-*`, `track-r-*`, `model-override-*`.
`docs/assets/trackg-r0/` (14.5 MB PNG evidence; cited by `tools/asset_cook/README.md` + a test manifest) → ASK. Tracked `.claude/` evidence (174 files, 59.5 MB: `vfx-fire/` 24.5 MB, `failsoft-disarm/` 9.4 MB, `captures/` 2 MB, 12 stray PNGs + `tree_export*.py` 16 MB) → ASK (E:).

## Totals

| Class | Files | Lines | MB |
|---|---:|---:|---:|
| DELETE now (DI-1..DI-5) | ~224 | ~38k | ~167 |
| DELETE engine `#if 0` (DI-6) | 44 | 5.5k + ~2.3k debug/scaffold | — |
| ASK | ~330 | ~30k | ~85 |

## Batch proposal (stage by explicit path; lanes have dirty files)

| # | Contents | Verify | Lane collision |
|---|---|---|---|
| DI-1 | tracked junk (editor/.vs, Tracy, Viewer, dev/null, 7 .vcproj, root strays, pre-trackd baseline, abl fixture) | check-contracts + the 4 Viewer-scanning checks | none (non-source) |
| DI-2 | `tests/visual/baselines` purge (keep README + presets.json) | check-contracts | none |
| DI-3 | tools/vk_* ×6, tgl_loader + rendercore spikes, mc2_01 one-offs, orphan scripts; consumer edits (raw-gl exemptions, lighting-divergence comment, seam/tier1 prose, README rows); seed regen | check-contracts + scripts/tests | none |
| DI-4 | shaders gos_grass.*, overlay_alpha_clear.frag, 6 expected JSONs, lighting-divergence row | check-contracts + shader_reflect | none (not lane code) |
| DI-5 | unbuilt leftovers: `code/actor.cpp`, 4 orphan test .cpp, 4 editor headers, `mfcplatform.hpp` | one compile | hold code/ + editor/ until lanes merge |
| DI-6a | mclib/mlr + stuff `#if 0` (gosvertex, gospoint, mlr_i_*tmesh, mlrloadobj, matrix, affinematrix, tube, stuff test stubs) | build + tier1 | lane 2 mlr narrowing merged; re-check |
| DI-6b | code/ `#if 0`, folded into split commits (ablmc2, mover, mech, warrior, team, gvehicl, goal, tacordr, contact, mission) | build + tier1 per file | blocked on lane 1/2 code/ work; needs `check-order-gate-coverage.py` MECH path + yaml/gl_shader_variants regen |
| DI-7 | root CMake: mclib_standalone block, `ENABLE_ASSET_CPP_PROBES`, ui_editor EXISTS guard, `ENABLE_MC2*` | configure | lane 2 window |
| DI-8 | docs + ASK outcomes | check-doc-links | none |

## ASK list (owner)

1. Brain cluster: delete or wire `brain_dispatch_harness`, `brain_missionfit_oporbd_test`, `brain_symbol_resolve_test`, `mech_import_harness`? (unwired; fixtures live; if kept, fold pure logic into tests/unit; if deleted, drop debt rows)
2. `cpp_probes` + `3rdparty/meshoptimizer`: is the offline meshopt LOD cook arc finished?
3. Dark pytest (76 modules, 14.7k lines never run): add a `tools_tests` row for live tools, or delete tests of dead tools (terrain_gen, spawn_lane, pak_*)?
4. Multi-lane tooling (`test_deploy_lease`, `test_smoke_lane_lock`, `parse-done-reports`, lane template, `spawn_lane`): retire now or with the queue item?
5. Measurement one-offs (13 audit/predict scripts + `tests/terrain/*.csv|md` outputs): delete?
6. `tests/visual/golden-sets.json` + `run_visual.py` (legacy registry pointing into an old worktree; `GOLDENS_DIR` still `baselines/`): retire with DI-2?
7. Historical docs (`superpowers`, `observations`, `plans`, `recon`, `*-rv-arc-*`, `terrain-8z-*`, 10 recon-1 seam docs): archive to E: or delete?
8. `docs/assets/trackg-r0` (14.5 MB) + tracked `.claude/` evidence (~52 MB): move to E:?
9. Opt-in modder tools `mc2fx`, `mc2fx_preview`, `mc2weapon_gui`: keep or delete with `deploy-mc2fx-tools.sh`?
10. `ui_editor/` (9 files, 345 KB): not examined; appears only via its CMake EXISTS guard.
11. `install_tree_scatter.py` and the `oracleparse`/`oracle_report`/`render_pass_report` chain: delete with their only consumers?

## Owner rulings (2026-10-09)

1. Brain cluster (`brain_dispatch_harness`, `brain_missionfit_oporbd_test`, `brain_symbol_resolve_test`, `mech_import_harness`): KEEP.
2. `tools/cpp_probes` (+ `3rdparty/meshoptimizer`): DELETE (lane 2 does it).
3. The 76 "dark" pytest modules are LIVE: keep them and ADD a `tools_tests` check-contracts row that runs them.
4. Multi-lane tooling (`parse-done-reports`, lane template, `test_deploy_lease`, `test_smoke_lane_lock`, `spawn_lane`, `test_spawn_lane`): retire now.
5. Measurement one-offs + their `tests/terrain/*.csv|md` outputs: DELETE.
6. `tests/visual/golden-sets.json` + `scripts/run_visual.py`: retire (`GOLDENS_DIR` -> `tests/visual/goldens/`).
7. Historical arc docs: tarball to `E:/mc2-archive-2026-10/docs/`, delete from repo.
8. `docs/assets/trackg-r0` + tracked `.claude/` evidence: move to E:, delete from repo.
9. Modder tools `mc2fx`, `mc2fx_preview`, `mc2weapon_gui` + `deploy-mc2fx-tools.sh`: KEEP.
10. `ui_editor/`: KEEP (lane 2 sets EXCLUDE_FROM_ALL).
11. `install_tree_scatter.py` + `oracleparse`/`oracle_report`/`render_pass_report` chain: DELETE with their consumers.
