# Scout: tracked docs/ and .claude/ markdown classification

Scope: `git ls-files 'docs/*.md' 'docs/**/*.md' '.claude/*.md' '.claude/**/*.md'` on branch `claude/nifty-mendeleev` (549 files, all present on disk). Read-only scout; nothing moved, edited or deleted.

Method: classes assigned from explicit per-file overrides plus path rules, using each file's first heading, dates and status markers. Only a sample was opened, so treat LIVE-REFERENCE as "keep candidate", not verified accurate. `referenced-by-code?` is a basename grep over `*.py *.sh *.cpp *.h *.hpp` (excluding the file itself). A hit is not proof of a live link, and a hit on the generic `README.md` basename is noise.

## Totals

| class | files | lines |
|---|---:|---:|
| LIVE-REFERENCE | 86 | 16400 |
| STATUS/LEDGER | 429 | 90423 |
| STALE-RULES | 34 | 5916 |
| **all** | **549** | **112739** |

Code/script-referenced files: 141 (of which 88 are not LIVE-REFERENCE and must have the reference fixed before archiving).

## 15 largest STATUS/LEDGER or STALE-RULES files

| path | lines | class | referenced-by-code? |
|---|---:|---|---|
| .claude/VK-ARC-STATE.md | 3809 | STATUS/LEDGER | yes (3): GameOS/gameos/gameos_graphics.cpp ... |
| docs/plans/2026-04-25-mod-profile-launcher-plan-1-foundation.md | 1957 | STATUS/LEDGER | no |
| docs/plans/2026-04-15-world-space-overlay-impl.md | 1233 | STATUS/LEDGER | no |
| .claude/VK-RUNG8-LADDER-1.md | 1168 | STATUS/LEDGER | yes (4): GameOS/gameos/gos_postprocess.h ... |
| docs/plans/2026-04-12-visual-upgrades-plan.md | 1140 | STATUS/LEDGER | no |
| docs/model-override-mvp-plan.md | 962 | STATUS/LEDGER | yes (1): mclib/model_override_registry.h |
| docs/terrain-lod-chunk-plan.md | 913 | STATUS/LEDGER | no |
| docs/terrain-lod-chunk-spec.md | 827 | STATUS/LEDGER | no |
| .claude/LIGHTING-HEADROOM-AUDIT.md | 745 | STATUS/LEDGER | yes (2): GameOS/gameos/gos_static_prop_batcher.cpp ... |
| docs/plans/2026-04-11-gbuffer-postprocess-shadow.md | 671 | STATUS/LEDGER | no |
| docs/plans/2026-04-19-mech-normal-maps.md | 584 | STATUS/LEDGER | no |
| docs/plans/2026-04-11-tracy-profiler-impl.md | 578 | STATUS/LEDGER | no |
| docs/plans/shadow-tess-plan.md | 567 | STATUS/LEDGER | no |
| docs/plans/2026-04-12-mc2-toolkit-impl.md | 563 | STATUS/LEDGER | no |
| docs/modernization-roadmap-2026-06-09.md | 562 | STATUS/LEDGER | no |

## Full table

| path | lines | class | referenced-by-code? |
|---|---:|---|---|
| .claude/ADD-PATH-RECON-1.md | 101 | STATUS/LEDGER | yes (2): tests/unit/test_variant_csv_modlint.cpp ... |
| .claude/CEMENT-EDGE-CURVE-1-STATUS.md | 134 | STATUS/LEDGER | yes (1): GameOS/gameos/gos_terrain_indirect.cpp |
| .claude/CEMENT-PATCHY-CAUSE-1-STATUS.md | 192 | STATUS/LEDGER | yes (1): RenderCore/RendererFeatureRegistry.h |
| .claude/CONE-SPILL-GRADE-1-STATUS.md | 201 | STATUS/LEDGER | yes (1): mclib/cone_glow.cpp |
| .claude/DARK-PROP-LIGHTING-6-STATUS.md | 109 | STATUS/LEDGER | yes (1): tools/albedo_masked_audit.py |
| .claude/DEFERRED-BATCH-8C-STATUS.md | 122 | STATUS/LEDGER | yes (1): mclib/bdactor.cpp |
| .claude/EQUIPMENT-MASTERID-USAGE-RECON-1.md | 38 | STATUS/LEDGER | yes (1): code/equipment_registry.h |
| .claude/FIXTURE-GLOW-1-STATUS.md | 255 | STATUS/LEDGER | yes (1): GameOS/gameos/gos_static_prop_batcher.cpp |
| .claude/GEOMORPH-NEXT-SLICES.md | 134 | STATUS/LEDGER | yes (2): mclib/terrain.cpp ... |
| .claude/GHOST-CONE-VERIFY-4-STATUS.md | 325 | STATUS/LEDGER | yes (1): mclib/cone_glow.cpp |
| .claude/GOLDEN-v051-v052-RETIREMENT-CASE.md | 91 | STALE-RULES | yes (1): scripts/check-contracts.sh |
| .claude/GPU-PICKING-RECON-1.md | 144 | STATUS/LEDGER | yes (1): code/missiongui.cpp |
| .claude/IBL-NIGHT-SCALE-1-STATUS.md | 278 | STATUS/LEDGER | yes (1): GameOS/gameos/gos_static_prop_batcher.cpp |
| .claude/ISSUE-FIXES-STATUS.md | 532 | STATUS/LEDGER | yes (2): mclib/mc2_audit.h ... |
| .claude/LIGHTING-HEADROOM-AUDIT.md | 745 | STATUS/LEDGER | yes (2): GameOS/gameos/gos_static_prop_batcher.cpp ... |
| .claude/LINEAR-COLOR-AUDIT-1.md | 53 | STATUS/LEDGER | yes (1): RenderCore/SceneLighting.h |
| .claude/MEASUREMENT-LEDGER.md | 508 | STATUS/LEDGER | yes (1): scripts/check-contract-coverage.py |
| .claude/META-FIX-PLAN-2026-07-16.md | 120 | STATUS/LEDGER | yes (1): scripts/smoke_lib/logparse.py |
| .claude/METAL-GATE-AUDIT-2.md | 53 | STATUS/LEDGER | yes (2): GameOS/gameos/vulkan_terrain_island.cpp ... |
| .claude/PAVED-OVERLAY-CLASS-1-STATUS.md | 264 | STATUS/LEDGER | yes (2): GameOS/gameos/gos_terrain_indirect.cpp ... |
| .claude/PRESENTATION-LOADOUT-STRANGLER-RECON-1.md | 56 | STATUS/LEDGER | yes (4): code/hardpoint.h ... |
| .claude/PROP-LIGHT-CLASS-4-STATUS.md | 199 | STATUS/LEDGER | yes (1): tools/albedo_masked_audit.py |
| .claude/SHADOW-DECISIONS-1-STATUS.md | 261 | STATUS/LEDGER | yes (1): RenderCore/RendererFeatureRegistry.h |
| .claude/SHORELINE-BATCH-COOK-1.md | 134 | STATUS/LEDGER | yes (1): tools/mc2data/stock_missions.py |
| .claude/SHORELINE-V0-FALSIFY-1-STATUS.md | 330 | STATUS/LEDGER | yes (2): scripts/check-stock-mission-data.py ... |
| .claude/TECHSCRIPT-GAP-CLOSURE-1.md | 97 | STATUS/LEDGER | yes (2): code/brain_special_dispatch.cpp ... |
| .claude/TERRAIN-CINEMATIC-GROUNDING-1-RECON.md | 89 | STATUS/LEDGER | yes (1): code/ground_contact_blob.h |
| .claude/TERRAIN-CONTROLMAP-GENERATE-1-RECON.md | 176 | STATUS/LEDGER | yes (1): tools/terrain_beautify/control_map_tool.py |
| .claude/TERRAIN-SCATTER-MASK-1-RECON.md | 187 | STATUS/LEDGER | yes (1): tools/terrain_beautify/scatter_tool.py |
| .claude/VISIBILITY-SEMANTICS-1-STATUS.md | 156 | STATUS/LEDGER | yes (1): mclib/vis_semantics.h |
| .claude/VK-ARC-STATE.md | 3809 | STATUS/LEDGER | yes (3): GameOS/gameos/gameos_graphics.cpp ... |
| .claude/VK-P8a-STEP0-CASTER-FEED-NOTE.md | 237 | STATUS/LEDGER | yes (2): GameOS/gameos/vulkan_shadow_island_live.cpp ... |
| .claude/VK-RUNG8-LADDER-1.md | 1168 | STATUS/LEDGER | yes (4): GameOS/gameos/gos_postprocess.h ... |
| .claude/VK-TIER1-SHARED-SCENE-DEPTH-RECON.md | 62 | STATUS/LEDGER | yes (1): tools/vk_gl_interop_spike/main_d32s8_depthtest.cpp |
| .claude/VULKAN-NEXT-SLICES.md | 524 | STATUS/LEDGER | yes (2): GameOS/gameos/gos_postprocess.cpp ... |
| .claude/WEAPON-DEFINITION-ARC-SCOPE-1.md | 156 | STATUS/LEDGER | yes (1): code/weapon_definition.h |
| .claude/agents/DOMAINS.md | 198 | STALE-RULES | no |
| .claude/agents/mc2-build-system-expert.md | 172 | STALE-RULES | no |
| .claude/agents/mc2-cpu-gpu-offload-expert.md | 152 | STALE-RULES | no |
| .claude/agents/mc2-gameos-expert.md | 173 | STALE-RULES | no |
| .claude/agents/mc2-mech-import-expert.md | 126 | STALE-RULES | no |
| .claude/agents/mc2-mech-skeletal-anim-expert.md | 127 | STALE-RULES | no |
| .claude/agents/mc2-mech-update-geometry-expert.md | 176 | STALE-RULES | no |
| .claude/agents/mc2-mission-data-expert.md | 179 | STALE-RULES | no |
| .claude/agents/mc2-render-contract-synthesizer.md | 229 | STALE-RULES | no |
| .claude/agents/mc2-render-expert.md | 206 | STALE-RULES | no |
| .claude/agents/mc2-render-perf-expert.md | 197 | STALE-RULES | no |
| .claude/agents/mc2-shader-expert.md | 184 | STALE-RULES | no |
| .claude/agents/mc2-terrain-indirect-expert.md | 175 | STALE-RULES | no |
| .claude/agents/mc2-water-depth-expert.md | 206 | STALE-RULES | no |
| .claude/archive-2026-07-05/HANDOFF-mech-preview-fbo.md | 145 | STATUS/LEDGER | no |
| .claude/archive/2026-07/README.md | 37 | STATUS/LEDGER | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| .claude/issue-reply-drafts.md | 317 | STATUS/LEDGER | no |
| .claude/prompts/distill-session-into-advisor-agent.md | 127 | STALE-RULES | no |
| .claude/prompts/dump-render-observations.md | 111 | STALE-RULES | no |
| .claude/skills/adversarial-plan-review.md | 200 | STALE-RULES | no |
| .claude/skills/cost-split-recon-bucket-design.md | 300 | STALE-RULES | no |
| .claude/skills/greybeard.md | 117 | STALE-RULES | no |
| .claude/skills/mc2-amd-shader-review.md | 75 | LIVE-REFERENCE | no |
| .claude/skills/mc2-build-deploy.md | 22 | LIVE-REFERENCE | no |
| .claude/skills/mc2-build.md | 40 | LIVE-REFERENCE | no |
| .claude/skills/mc2-check.md | 36 | LIVE-REFERENCE | no |
| .claude/skills/mc2-deploy.md | 141 | LIVE-REFERENCE | yes (1): scripts/write-deploy-manifest.py |
| .claude/skills/mc2-gsd-planner-executor.md | 232 | STALE-RULES | no |
| .claude/skills/mc2-render-spine-advisor.md | 509 | STALE-RULES | no |
| .claude/skills/mc2-shader-diff.md | 54 | LIVE-REFERENCE | no |
| .claude/skills/mc2-validate.md | 50 | LIVE-REFERENCE | no |
| .claude/skills/orchestrator-review-protocol.md | 127 | STALE-RULES | no |
| .claude/user-feedback/QUEUE-2026-08-01.md | 190 | STATUS/LEDGER | no |
| docs/10-10-bespoke-tactics-engine-vision.md | 253 | STATUS/LEDGER | no |
| docs/2026-05-26-next-gen-rts-engine-vision.md | 276 | STATUS/LEDGER | no |
| docs/DECAL-INTEGRATE-1.md | 133 | STATUS/LEDGER | no |
| docs/GLOSSARY.md | 21 | LIVE-REFERENCE | no |
| docs/HANDOFF-model-override-lod.md | 41 | STATUS/LEDGER | no |
| docs/LANE_COMPLETION_REPORT.template.md | 93 | STALE-RULES | yes (1): scripts/parse-done-reports.py |
| docs/MODDER-REPORT-techscript-brainspecial.md | 47 | STATUS/LEDGER | no |
| docs/POSTPROCESS-COMPUTE-BLUR-1.md | 92 | STATUS/LEDGER | no |
| docs/README.md | 79 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/RELEASE-PROCESS.md | 119 | LIVE-REFERENCE | yes (1): scripts/build_release.sh |
| docs/STARTER-GUIDE.md | 117 | LIVE-REFERENCE | no |
| docs/VFX-BLACKBODY-1.md | 139 | STATUS/LEDGER | no |
| docs/VFX-DISTORTION-1.md | 87 | STATUS/LEDGER | no |
| docs/VFX-SCENECOLOR-GRAB-1.md | 79 | STATUS/LEDGER | no |
| docs/VISUAL-CAPTURE-MATRIX-1-DESIGN.md | 127 | STATUS/LEDGER | no |
| docs/active_campaigns.md | 285 | STATUS/LEDGER | no |
| docs/advisor-landscape-audit-2026-05-14.md | 227 | STATUS/LEDGER | no |
| docs/ai-order-triage.md | 130 | STATUS/LEDGER | yes (1): scripts/check-order-gate-coverage.py |
| docs/amd-driver-rules.md | 55 | LIVE-REFERENCE | yes (2): GameOS/gameos/gameos_graphics.cpp ... |
| docs/animated-prop-cook-recon.md | 247 | STATUS/LEDGER | yes (1): tools/asset_cook/gen_probe_glb.py |
| docs/architecture.md | 51 | LIVE-REFERENCE | yes (10): GameOS/gameos/gos_visual_capture.h ... |
| docs/asan-autonomous-run-log.md | 104 | STATUS/LEDGER | no |
| docs/asan-follow-ups.md | 490 | STATUS/LEDGER | yes (4): code/turret.cpp ... |
| docs/asan-triage.md | 209 | STATUS/LEDGER | no |
| docs/asset-manifest-schema.md | 109 | LIVE-REFERENCE | no |
| docs/asset-mod-glb-scope.md | 68 | STATUS/LEDGER | no |
| docs/asset-mod-test-asset.md | 48 | STATUS/LEDGER | no |
| docs/asset-modernization-recon.md | 67 | STATUS/LEDGER | no |
| docs/asset-pipeline.md | 215 | LIVE-REFERENCE | yes (3): scripts/check-asset-pipeline-doc.sh ... |
| docs/asset-viewer-backend-a-shader-contract.md | 93 | STATUS/LEDGER | yes (1): tools/asset_viewer/ModelPreviewEngineShader.cpp |
| docs/asset-viewer-backend-a-uvv-check.md | 145 | STATUS/LEDGER | no |
| docs/asset-viewer-mech-assembly-inspector.md | 423 | STATUS/LEDGER | no |
| docs/background-launch-inventory.md | 56 | LIVE-REFERENCE | yes (1): scripts/check-background-launch.py |
| docs/baseline-A-post-8z.md | 199 | STATUS/LEDGER | no |
| docs/bt2018-reverse-leg-detection-recon-1.md | 126 | STATUS/LEDGER | no |
| docs/bt2018-skel-1b-gpu-recon.md | 65 | STATUS/LEDGER | no |
| docs/bt2018-skel-1b-gpu-skinning-recon-2.md | 87 | STATUS/LEDGER | no |
| docs/bt2018-skel-1b-runtime-scope.md | 47 | STATUS/LEDGER | no |
| docs/bt2018-skel-gpu-draw-engagement-recon-1.md | 77 | STATUS/LEDGER | no |
| docs/build-parallel-and-tooling.md | 193 | STALE-RULES | no |
| docs/capture-evidence-retention.md | 165 | STATUS/LEDGER | no |
| docs/cement-overlay-codemap.md | 84 | STATUS/LEDGER | no |
| docs/critical_inline_rules.md | 56 | LIVE-REFERENCE | yes (2): RenderCore/ResolutionContract.h ... |
| docs/cxx17-coding-rules.md | 435 | LIVE-REFERENCE | no |
| docs/data-provenance.md | 324 | LIVE-REFERENCE | yes (1): tools/mc2data/stock_missions.py |
| docs/debug_state_schema.md | 211 | LIVE-REFERENCE | no |
| docs/deploy-isolation.md | 196 | LIVE-REFERENCE | no |
| docs/dev-shell.md | 70 | LIVE-REFERENCE | no |
| docs/dev-velocity.md | 85 | STALE-RULES | yes (1): tools/spawn_lane.py |
| docs/disciplines.md | 254 | STALE-RULES | no |
| docs/editor-authoring-tools-vision.md | 133 | STATUS/LEDGER | no |
| docs/editor-build-architecture-integrity-audit.md | 119 | STATUS/LEDGER | yes (1): scripts/check-editor-build.sh |
| docs/editor-chunk-path-parity.md | 211 | STATUS/LEDGER | no |
| docs/editor-debug-overlay-phase2.md | 58 | STATUS/LEDGER | no |
| docs/editor-game-projection-divergence-recon.md | 237 | STATUS/LEDGER | no |
| docs/engine-closure-audit.md | 333 | STATUS/LEDGER | yes (2): RenderCore/RenderPassContract.h ... |
| docs/engine-standalone-seams.md | 462 | STATUS/LEDGER | yes (2): HostServices/HostServices.h ... |
| docs/frame-contracts/currentness-claim-audit-1.md | 66 | STATUS/LEDGER | no |
| docs/frame-contracts/same-class-update-sites-1.md | 130 | STATUS/LEDGER | no |
| docs/frame-contracts/skip-safety-audit-1.md | 142 | STATUS/LEDGER | no |
| docs/game-auto-tools.md | 52 | LIVE-REFERENCE | no |
| docs/gameplay-driver.md | 617 | LIVE-REFERENCE | yes (1): GameOS/gameos/gameos_input.cpp |
| docs/gosfx-animation-lifecycle-recon.md | 88 | STATUS/LEDGER | no |
| docs/gosfx-modder-dropin-path.md | 122 | STATUS/LEDGER | no |
| docs/gpu-scene-record-recon.md | 275 | STATUS/LEDGER | no |
| docs/gpu-static-prop-cull-lessons.md | 248 | STATUS/LEDGER | no |
| docs/hud-tab-registry.md | 89 | LIVE-REFERENCE | no |
| docs/hzb-depth-convention.md | 59 | LIVE-REFERENCE | yes (2): GameOS/gameos/gos_postprocess.cpp ... |
| docs/hzb-draw-consumer-recon.md | 235 | STATUS/LEDGER | no |
| docs/hzb-staticprop-cull-readiness.md | 145 | STATUS/LEDGER | no |
| docs/hzb-staticprop-cull-recon.md | 94 | STATUS/LEDGER | no |
| docs/hzb-visibility-mvp.md | 120 | STATUS/LEDGER | yes (1): RenderCore/RendererFeatureRegistry.h |
| docs/ibl-plan.md | 419 | STATUS/LEDGER | no |
| docs/known_issues.md | 157 | LIVE-REFERENCE | yes (4): GameOS/gameos/gameos_graphics.cpp ... |
| docs/lighting-divergence-map.md | 338 | STATUS/LEDGER | yes (2): scripts/check-contracts.sh ... |
| docs/load_bearing_pointers.md | 76 | STALE-RULES | no |
| docs/material-abi-unification-recon.md | 302 | STATUS/LEDGER | no |
| docs/material-m0-contract.md | 68 | LIVE-REFERENCE | yes (4): GameOS/gameos/gos_static_prop_batcher.cpp ... |
| docs/mcp-render-state.md | 108 | LIVE-REFERENCE | no |
| docs/mech-lighting-plan.md | 151 | STATUS/LEDGER | no |
| docs/mech-normals-audit.md | 126 | STATUS/LEDGER | yes (1): GameOS/gameos/gos_mech_batcher.cpp |
| docs/mech-rv-arc-recon.md | 295 | STATUS/LEDGER | no |
| docs/methuselas-handoff.md | 283 | STATUS/LEDGER | no |
| docs/modding-animations.md | 144 | LIVE-REFERENCE | yes (1): mclib/anim_override_registry.h |
| docs/modding-effects.md | 239 | LIVE-REFERENCE | yes (1): RenderCore/RendererFeatureRegistry.h |
| docs/modding-guide.md | 423 | LIVE-REFERENCE | yes (1): tools/terrain_beautify/cook_markings.py |
| docs/modding-weapons.md | 147 | LIVE-REFERENCE | no |
| docs/modding/debugging-render-issues.md | 153 | LIVE-REFERENCE | no |
| docs/modding/examples/README.md | 218 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/building-import/README.md | 229 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/getting-started.md | 166 | LIVE-REFERENCE | no |
| docs/modding/examples/imgui-editing/README.md | 231 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/mech-import/README.md | 162 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/mechanism-map.md | 91 | LIVE-REFERENCE | yes (1): docs/modding/examples/tree-import/mod/source/make_placeholder_glb.py |
| docs/modding/examples/mission-edit/README.md | 250 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/texture-override/README.md | 242 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/tree-import/README.md | 219 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/variant-add/README.md | 218 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/weapon-add/README.md | 282 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/examples/weapon-edit/README.md | 143 | LIVE-REFERENCE | noise: generic README.md basename (10 hits, mostly 3rdparty) |
| docs/modding/renderer-feature-flags.md | 162 | LIVE-REFERENCE | no |
| docs/modding/rendering-overview.md | 87 | LIVE-REFERENCE | no |
| docs/model-override-dual-shape-plan.md | 140 | STATUS/LEDGER | no |
| docs/model-override-gpu-instancing-proposal.md | 318 | STATUS/LEDGER | no |
| docs/model-override-lighting-lod-recon.md | 145 | STATUS/LEDGER | yes (1): mclib/bdactor.cpp |
| docs/model-override-lighting-ownership-recon.md | 130 | STATUS/LEDGER | no |
| docs/model-override-mvp-notes.md | 482 | STATUS/LEDGER | no |
| docs/model-override-mvp-plan.md | 962 | STATUS/LEDGER | yes (1): mclib/model_override_registry.h |
| docs/model-override-system-recon.md | 252 | STATUS/LEDGER | no |
| docs/modernization-roadmap-2026-06-09.md | 562 | STATUS/LEDGER | no |
| docs/ns3-geometry-loader-standalone.md | 113 | STATUS/LEDGER | no |
| docs/nvidia-hardening-s2.md | 178 | STATUS/LEDGER | no |
| docs/observations/2026-04-24-tier2-visual-observations.md | 97 | STATUS/LEDGER | no |
| docs/observations/2026-04-25-abl-library-shadow-rule.md | 139 | STATUS/LEDGER | yes (1): code/ablmc2.cpp |
| docs/observations/2026-05-14-render-pipeline-mapping.md | 105 | STATUS/LEDGER | no |
| docs/observations/2026-05-14-render-pipeline-matrix.md | 237 | STATUS/LEDGER | no |
| docs/observations/2026-05-15-render-vpl-deferred-retirement-topology.md | 64 | STATUS/LEDGER | no |
| docs/observations/2026-05-22-quadSetupTextures-slimReduce-workload-recon.md | 262 | STATUS/LEDGER | no |
| docs/observations/2026-05-22-static-prop-pop-in-investigation.md | 281 | STATUS/LEDGER | no |
| docs/observations/2026-05-22-terrain-admission-hotpath-recon.md | 401 | STATUS/LEDGER | yes (1): mclib/terrain.cpp |
| docs/observations/2026-05-22-unified-proj-apre-gate.md | 50 | STATUS/LEDGER | no |
| docs/observations/2026-05-22-unified-proj-stage-a-shipped.md | 96 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-crater-pipeline-map.md | 478 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-fx-pipeline-map.md | 275 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-hud-pipeline-map.md | 464 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-mech-pipeline-map.md | 509 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-pipeline-master-index.md | 70 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-shadow-postprocess-pipeline-map.md | 483 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-staticprops-pipeline-map.md | 422 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-terrain-pipeline-map.md | 502 | STATUS/LEDGER | no |
| docs/observations/2026-05-25-water-pipeline-map.md | 481 | STATUS/LEDGER | no |
| docs/oracle-dynamic-pipeline-gate.md | 140 | STATUS/LEDGER | yes (1): scripts/check-oracle-registry.py |
| docs/perf-attribution-plan.md | 242 | STATUS/LEDGER | no |
| docs/perf-budget.md | 102 | LIVE-REFERENCE | yes (1): scripts/oracle_report.py |
| docs/plans/2026-04-10-cpu-terrain-displacement-design.md | 133 | STATUS/LEDGER | yes (3): RenderCore/RendererFeatureRegistry.h ... |
| docs/plans/2026-04-10-cpu-terrain-displacement-plan.md | 492 | STATUS/LEDGER | no |
| docs/plans/2026-04-10-shadow-quality-design.md | 134 | STATUS/LEDGER | no |
| docs/plans/2026-04-10-shadow-quality-plan.md | 454 | STATUS/LEDGER | no |
| docs/plans/2026-04-11-gbuffer-postprocess-shadow-design.md | 138 | STATUS/LEDGER | no |
| docs/plans/2026-04-11-gbuffer-postprocess-shadow.md | 671 | STATUS/LEDGER | no |
| docs/plans/2026-04-11-shadow-debug-visualizer-design.md | 77 | STATUS/LEDGER | no |
| docs/plans/2026-04-11-shadow-debug-visualizer-plan.md | 370 | STATUS/LEDGER | no |
| docs/plans/2026-04-11-tracy-profiler-design.md | 112 | STATUS/LEDGER | no |
| docs/plans/2026-04-11-tracy-profiler-impl.md | 578 | STATUS/LEDGER | no |
| docs/plans/2026-04-12-mc2-toolkit-design.md | 113 | STATUS/LEDGER | no |
| docs/plans/2026-04-12-mc2-toolkit-impl.md | 563 | STATUS/LEDGER | no |
| docs/plans/2026-04-12-visual-upgrades-design.md | 186 | STATUS/LEDGER | no |
| docs/plans/2026-04-12-visual-upgrades-plan.md | 1140 | STATUS/LEDGER | no |
| docs/plans/2026-04-13-gpu-projection-migration-design.md | 272 | STATUS/LEDGER | no |
| docs/plans/2026-04-14-overlay-projection-fix-design.md | 72 | STATUS/LEDGER | no |
| docs/plans/2026-04-15-world-space-overlay-design.md | 160 | STATUS/LEDGER | no |
| docs/plans/2026-04-15-world-space-overlay-impl.md | 1233 | STATUS/LEDGER | no |
| docs/plans/2026-04-16-terrain-worldspace-vertex-design.md | 204 | STATUS/LEDGER | no |
| docs/plans/2026-04-16-terrain-worldspace-vertex-impl.md | 561 | STATUS/LEDGER | no |
| docs/plans/2026-04-19-mech-normal-maps.md | 584 | STATUS/LEDGER | no |
| docs/plans/2026-04-20-walls-to-static-shadow-plan.md | 388 | STATUS/LEDGER | no |
| docs/plans/2026-04-23-mod-profile-launcher-design.md | 204 | STATUS/LEDGER | no |
| docs/plans/2026-04-23-mod-profile-launcher-prep-notes.md | 146 | STATUS/LEDGER | no |
| docs/plans/2026-04-25-ai-passive-engagement-prompt.md | 150 | STATUS/LEDGER | no |
| docs/plans/2026-04-25-mod-profile-launcher-plan-1-foundation.md | 1957 | STATUS/LEDGER | no |
| docs/plans/2026-04-25-mod-profile-launcher-scope-additions.md | 366 | STATUS/LEDGER | no |
| docs/plans/2026-04-25-paint-scheme-prompt.md | 106 | STATUS/LEDGER | no |
| docs/plans/2026-04-25-rebind-crash-prompt.md | 86 | STATUS/LEDGER | no |
| docs/plans/2026-04-26-render-contract-f3-mrt-completeness-prompt.md | 107 | STATUS/LEDGER | no |
| docs/plans/2026-04-27-modern-terrain-surface-prompt.md | 168 | STATUS/LEDGER | no |
| docs/plans/cement-overlay-rewrite.md | 193 | STATUS/LEDGER | no |
| docs/plans/overlay-gpu-projection-handoff.md | 131 | STATUS/LEDGER | no |
| docs/plans/overlay-shadow-postprocess-plan.md | 86 | STATUS/LEDGER | no |
| docs/plans/render-contract-and-shadows-roadmap.md | 485 | STATUS/LEDGER | no |
| docs/plans/shadow-tess-design.md | 94 | STATUS/LEDGER | no |
| docs/plans/shadow-tess-plan.md | 567 | STATUS/LEDGER | no |
| docs/plans/static-terrain-shadow-architecture.md | 387 | STATUS/LEDGER | yes (1): mclib/mapdata.cpp |
| docs/qwen-local-workflow.md | 75 | LIVE-REFERENCE | no |
| docs/qwen-system-prompts.md | 37 | LIVE-REFERENCE | no |
| docs/recon/gosfx-render-path.md | 544 | STATUS/LEDGER | no |
| docs/recon/mech-render-path.md | 487 | STATUS/LEDGER | no |
| docs/recon/meta-perf-review.md | 509 | STATUS/LEDGER | no |
| docs/recon/shadows-render-path.md | 432 | STATUS/LEDGER | no |
| docs/recon/static-props-render-path.md | 266 | STATUS/LEDGER | no |
| docs/recon/terrain-aa-recon-1-fbo-edges.md | 109 | STATUS/LEDGER | no |
| docs/recon/terrain-aa-recon-1.md | 92 | STATUS/LEDGER | no |
| docs/recon/terrain-edge-feather-recon-1-shader.md | 103 | STATUS/LEDGER | no |
| docs/recon/terrain-edge-feather-recon-1.md | 137 | STATUS/LEDGER | no |
| docs/recon/terrain-edge-geometry-recon-1-water-backdrop.md | 92 | STATUS/LEDGER | no |
| docs/recon/terrain-edge-geometry-recon-1.md | 97 | STATUS/LEDGER | no |
| docs/recon/terrain-macro-variation-1-recon.md | 86 | STATUS/LEDGER | no |
| docs/recon/terrain-render-path.md | 299 | STATUS/LEDGER | no |
| docs/recon/terrain-round-method-recon-1-shader.md | 90 | STATUS/LEDGER | no |
| docs/recon/terrain-round-method-recon-1.md | 91 | STATUS/LEDGER | no |
| docs/recon/terrain-runtime-api-recon-1-datamodel.md | 52 | STATUS/LEDGER | no |
| docs/recon/terrain-runtime-api-recon-1-landmines.md | 147 | STATUS/LEDGER | no |
| docs/recon/terrain-runtime-api-recon-1-pak-format.md | 119 | STATUS/LEDGER | no |
| docs/recon/terrain-runtime-api-recon-1-render-consumers.md | 80 | STATUS/LEDGER | no |
| docs/recon/terrain-runtime-api-recon-1.md | 234 | STATUS/LEDGER | yes (2): mclib/terrain_runtime.cpp ... |
| docs/recon/terrain-visual-height-sample-1-PLAN.md | 72 | STATUS/LEDGER | no |
| docs/recon/terrain-visual-height-sample-1-core.md | 116 | STATUS/LEDGER | no |
| docs/recon/terrain-visual-height-sample-1-desync.md | 48 | STATUS/LEDGER | no |
| docs/recon/terrain-visual-height-sample-1-shader.md | 123 | STATUS/LEDGER | no |
| docs/recon/vehicles-render-path.md | 374 | STATUS/LEDGER | no |
| docs/recon/water-render-path.md | 300 | STATUS/LEDGER | no |
| docs/release-checklist.md | 45 | LIVE-REFERENCE | yes (1): scripts/build_release.sh |
| docs/render-api-boundary-recon.md | 237 | STATUS/LEDGER | no |
| docs/render-backend-seams/GLSTATE-GUARD-ADOPTION-1-orchestrator-prompt.md | 57 | STALE-RULES | no |
| docs/render-backend-seams/HANDOFF-FRAME-PLAN-COLORMASK-TERRAIN-ARC.md | 87 | STATUS/LEDGER | no |
| docs/render-backend-seams/HANDOFF-WATER-VISUAL-GATE-2.md | 122 | STATUS/LEDGER | no |
| docs/render-backend-seams/SCOPE-ABL-OPCODE-BOUNDS-HARDEN.md | 196 | STATUS/LEDGER | no |
| docs/render-backend-seams/SCOPE-BRAIN-FIT-SCHEMA-1.md | 509 | STATUS/LEDGER | yes (1): scripts/check-brain-fit-schema.py |
| docs/render-backend-seams/abl-arg-guard-2a-1.md | 121 | STATUS/LEDGER | no |
| docs/render-backend-seams/abl-arg-guard-2bc-1.md | 74 | STATUS/LEDGER | no |
| docs/render-backend-seams/abl-arg-guard-3-1.md | 56 | STATUS/LEDGER | no |
| docs/render-backend-seams/abl-arg-guard-complete-1.md | 144 | STATUS/LEDGER | no |
| docs/render-backend-seams/abl-bad-native-arg-repro-1.md | 252 | STATUS/LEDGER | no |
| docs/render-backend-seams/abl-mod-mission-guard-validation-1.md | 124 | STATUS/LEDGER | no |
| docs/render-backend-seams/abl-native-arg-guard-coverage-recon-1.md | 174 | STATUS/LEDGER | no |
| docs/render-backend-seams/binding-slot-occupancy.md | 129 | LIVE-REFERENCE | yes (1): scripts/check-vulkan-bindings.py |
| docs/render-backend-seams/blendmode-additive-vocabulary-1.md | 76 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-ai-2.14-techscript-recon-1.md | 198 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-arc-implementation-roadmap.md | 196 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-commit-phase-1.md | 75 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-fixed-tick-1.md | 143 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-fsm-1k-a.md | 123 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-fsm-1k-b.md | 96 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-fsm-dsl-recon-1.md | 449 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-intent-queue-1.md | 51 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-oport-corepatrol-1.md | 87 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-runtime-1a.md | 48 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-runtime-1b-recon-1.md | 407 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-runtime-1b.md | 42 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-runtime-recon-1.md | 421 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-tactic-weights-a.md | 117 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-tick-architecture-recon-1.md | 304 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-unit-varstore-recon-1.md | 101 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-var-mission-1dm.md | 70 | STATUS/LEDGER | no |
| docs/render-backend-seams/brain-world-snapshot-1.md | 114 | STATUS/LEDGER | no |
| docs/render-backend-seams/buffer-lifetime-ledger.md | 73 | LIVE-REFERENCE | yes (1): scripts/check-buffer-lifetime-ownership.py |
| docs/render-backend-seams/buffer-lifetime-ownership-recon-1.md | 76 | STATUS/LEDGER | no |
| docs/render-backend-seams/class-d-sync-recon-1.md | 61 | STATUS/LEDGER | no |
| docs/render-backend-seams/colormask-rollout-recon-1.md | 73 | STATUS/LEDGER | no |
| docs/render-backend-seams/dispatch-effect-coreattack-1.md | 90 | STATUS/LEDGER | no |
| docs/render-backend-seams/dispatch-effect-coreguard-1.md | 79 | STATUS/LEDGER | no |
| docs/render-backend-seams/dispatch-effect-coremoveto-1.md | 90 | STATUS/LEDGER | no |
| docs/render-backend-seams/dispatch-effect-uniteject-1.md | 56 | STATUS/LEDGER | no |
| docs/render-backend-seams/dispatch-effect-unitretreat-1.md | 110 | STATUS/LEDGER | no |
| docs/render-backend-seams/dispatch-loader-raw-1.md | 99 | STATUS/LEDGER | no |
| docs/render-backend-seams/dryrun-drawsite-order-recon-1.md | 350 | STATUS/LEDGER | no |
| docs/render-backend-seams/dryrun-observe-coverage-recon-1.md | 290 | STATUS/LEDGER | no |
| docs/render-backend-seams/editor-game-parity-arc.md | 557 | STATUS/LEDGER | no |
| docs/render-backend-seams/editor-pass-contract-parity.md | 107 | STATUS/LEDGER | no |
| docs/render-backend-seams/executor-island-recon-1.md | 284 | STATUS/LEDGER | no |
| docs/render-backend-seams/frame-graph-executor-dryrun-recon-1.md | 309 | STATUS/LEDGER | yes (1): RenderCore/frame_pass_trace.h |
| docs/render-backend-seams/frame-graph-executor-staging-recon-1.md | 281 | STATUS/LEDGER | no |
| docs/render-backend-seams/frame-graph-recon-1.md | 188 | STATUS/LEDGER | yes (2): RenderCore/ambient_contract.h ... |
| docs/render-backend-seams/frame-graph-tex-latch-recon-1.md | 283 | STATUS/LEDGER | no |
| docs/render-backend-seams/frame-resource-ledger.md | 94 | STATUS/LEDGER | no |
| docs/render-backend-seams/framegraph-apply-state-island-recon-1.md | 422 | STATUS/LEDGER | no |
| docs/render-backend-seams/framegraph-statepack-recon-1.md | 334 | STATUS/LEDGER | no |
| docs/render-backend-seams/gated-layout-macro-guard.md | 93 | STATUS/LEDGER | no |
| docs/render-backend-seams/gated-layout-macros.md | 873 | LIVE-REFERENCE | yes (1): scripts/check-gated-layout-macros.py |
| docs/render-backend-seams/gpu-buffer-owner-recon-1.md | 204 | STATUS/LEDGER | no |
| docs/render-backend-seams/gpu-buffer-wrapper-design-1.md | 337 | STATUS/LEDGER | yes (3): GameOS/gameos/utils/gpu_ring_buffer.h ... |
| docs/render-backend-seams/gpu-buffer-wrapper-tier0-light-1-recon.md | 251 | STATUS/LEDGER | no |
| docs/render-backend-seams/gpu-buffer-wrapper-tier0-recon-1.md | 40 | STATUS/LEDGER | no |
| docs/render-backend-seams/gpu-buffer-wrapper-tier0-recon-2.md | 144 | STATUS/LEDGER | yes (1): GameOS/gameos/utils/gpu_ring_buffer.h |
| docs/render-backend-seams/gpu-material-contract-recon-1.md | 178 | STATUS/LEDGER | yes (1): scripts/check-material-gpu-mirror.sh |
| docs/render-backend-seams/gpu-resource-manager-seam-recon-1.md | 38 | STATUS/LEDGER | no |
| docs/render-backend-seams/gpu-texture-resource-manager-seam-recon-1.md | 205 | STATUS/LEDGER | no |
| docs/render-backend-seams/mdi-submission-ledger-recon-1.md | 77 | STATUS/LEDGER | yes (2): GameOS/gameos/mdi_submit.h ... |
| docs/render-backend-seams/mechopaque-passidentity-recon-1.md | 254 | STATUS/LEDGER | no |
| docs/render-backend-seams/modder-carver-conversion-diff-recon-1.md | 290 | STATUS/LEDGER | no |
| docs/render-backend-seams/modmission-fastfail-regression-recon-1.md | 134 | STATUS/LEDGER | no |
| docs/render-backend-seams/nvidia-readiness-runbook.md | 108 | LIVE-REFERENCE | yes (1): tools/run_nvidia_readiness_smoke.py |
| docs/render-backend-seams/objbatcher-zero-gpu-drawn-recon-1.md | 79 | STATUS/LEDGER | yes (1): scripts/check-buffer-lifetime-ownership.py |
| docs/render-backend-seams/offline-shader-variant-build-1.md | 78 | STATUS/LEDGER | no |
| docs/render-backend-seams/opengl-correctness-ledger-1.md | 167 | STATUS/LEDGER | no |
| docs/render-backend-seams/particle-flush-stall-measure-1.md | 76 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipeline-key-schema.md | 80 | LIVE-REFERENCE | yes (1): scripts/check-pipeline-key.py |
| docs/render-backend-seams/pipeline-pass-coverage-ledger.md | 86 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipeline-state-contract-recon-1.md | 161 | STATUS/LEDGER | yes (1): scripts/check-pipeline-desc.py |
| docs/render-backend-seams/pipeline-state-occupancy.md | 67 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipeline-state-owner-audit-1.md | 100 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipeline-visual-gate-harness-isolation-plan-1.md | 83 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipeline-visual-gate-harness-recon-1.md | 76 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipelinekey-rasterstate-authority-recon-1.md | 135 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipelinekey-rasterstate-frontface-authority-1.md | 65 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipelinekey-remaining-state-gaps-recon-1.md | 127 | STATUS/LEDGER | no |
| docs/render-backend-seams/pipelinekey-vertexlayout-authority-1.md | 79 | STATUS/LEDGER | no |
| docs/render-backend-seams/postprocess-pipeline-registration-recon-1.md | 85 | STATUS/LEDGER | no |
| docs/render-backend-seams/postprocess-subgraph-recon-1.md | 400 | STATUS/LEDGER | no |
| docs/render-backend-seams/render-contract-index-1.md | 311 | STATUS/LEDGER | no |
| docs/render-backend-seams/render-frame-plan-recon-1.md | 115 | STATUS/LEDGER | no |
| docs/render-backend-seams/render-pass-attachment-contract-recon-1.md | 106 | STATUS/LEDGER | yes (1): GameOS/gameos/render_pass_attachment.h |
| docs/render-backend-seams/same-order-executor-recon-1.md | 286 | STATUS/LEDGER | no |
| docs/render-backend-seams/sampler-unit-occupancy.md | 139 | STATUS/LEDGER | no |
| docs/render-backend-seams/shader-artifact-package-metadata-1.md | 53 | STATUS/LEDGER | no |
| docs/render-backend-seams/shader-permutation-inventory.md | 119 | LIVE-REFERENCE | yes (1): scripts/check-shader-injectors.py |
| docs/render-backend-seams/shader-variant-lockstep-1.md | 47 | STATUS/LEDGER | no |
| docs/render-backend-seams/shader-variant-ownership-recon-1.md | 56 | STATUS/LEDGER | no |
| docs/render-backend-seams/shadow-caster-applypipeline-routing-1.md | 75 | STATUS/LEDGER | no |
| docs/render-backend-seams/shadow-caster-pipeline-registration-1.md | 68 | STATUS/LEDGER | no |
| docs/render-backend-seams/shadow-caster-pipeline-registration-recon-1.md | 129 | STATUS/LEDGER | no |
| docs/render-backend-seams/shadow-observe-2-recon-1.md | 249 | STATUS/LEDGER | no |
| docs/render-backend-seams/shadow-observe-3-recon-1.md | 242 | STATUS/LEDGER | no |
| docs/render-backend-seams/shadow-static-build-model-recon-1.md | 307 | STATUS/LEDGER | no |
| docs/render-backend-seams/slot-14-20-split-plan.md | 215 | STATUS/LEDGER | no |
| docs/render-backend-seams/smoke-modmission-env-guards-1.md | 181 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-consumer-pilot-build-1.md | 77 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-consumer-pilot-recon-1.md | 70 | STATUS/LEDGER | yes (1): tools/shader_offline_build/build_variants.py |
| docs/render-backend-seams/spirv-fbm-postprocess-pilot-1.md | 60 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-keyed-variant-consumer-1.md | 69 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-mechopaque-pilot-build-1.md | 54 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-mechopaque-pilot-recon-1.md | 111 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-mechopaque-pipelinekey-integration-1.md | 52 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-postprocess-family-1.md | 68 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-reflection-contract-check-1.md | 56 | STATUS/LEDGER | no |
| docs/render-backend-seams/spirv-staticprop-depth-recon-1.md | 89 | STATUS/LEDGER | no |
| docs/render-backend-seams/split-path-pairs.md | 101 | LIVE-REFERENCE | yes (1): scripts/check-split-path-contracts.py |
| docs/render-backend-seams/static-prop-family-ledger.md | 46 | STATUS/LEDGER | yes (1): scripts/check-static-prop-family.py |
| docs/render-backend-seams/static-prop-legacy-path-classify-1.md | 67 | STATUS/LEDGER | no |
| docs/render-backend-seams/static-prop-path-ownership-recon-1.md | 87 | STATUS/LEDGER | no |
| docs/render-backend-seams/subgraph-contiguity-rule.md | 62 | LIVE-REFERENCE | no |
| docs/render-backend-seams/symbolic-opord-args-recon-1.md | 78 | STATUS/LEDGER | no |
| docs/render-backend-seams/task-scheduler-core-1.md | 64 | STATUS/LEDGER | no |
| docs/render-backend-seams/task-scheduler-core-recon-1.md | 434 | STATUS/LEDGER | no |
| docs/render-backend-seams/techscript-call-chain-1a.md | 110 | STATUS/LEDGER | no |
| docs/render-backend-seams/techscript-dispatch-1d-vars-1.md | 139 | STATUS/LEDGER | no |
| docs/render-backend-seams/techscript-gap-closure-1.md | 18 | STATUS/LEDGER | no |
| docs/render-backend-seams/techscript-special-dispatch-1-recon-1.md | 352 | STATUS/LEDGER | no |
| docs/render-backend-seams/techscript-special-dispatch-1a.md | 103 | STATUS/LEDGER | no |
| docs/render-backend-seams/techscript-special-dispatch-1b.md | 146 | STATUS/LEDGER | no |
| docs/render-backend-seams/techscript-special-dispatch-1c.md | 156 | STATUS/LEDGER | no |
| docs/render-backend-seams/terrain-indirect-patch-retire-recon-1.md | 312 | STATUS/LEDGER | no |
| docs/render-backend-seams/terrain-legacy-retire-recon-1.md | 449 | STATUS/LEDGER | no |
| docs/render-backend-seams/terrain-lodchunk-pipeline-routing-recon-1.md | 86 | STATUS/LEDGER | no |
| docs/render-backend-seams/terrain-main-pipeline-registration-recon-1.md | 102 | STATUS/LEDGER | no |
| docs/render-backend-seams/terrain-main-pipeline-registration-recon-2.md | 60 | STATUS/LEDGER | no |
| docs/render-backend-seams/terrain-overlay-decal-applypipeline-routing-1.md | 60 | STATUS/LEDGER | no |
| docs/render-backend-seams/terrain-overlay-decal-pipeline-registration-1.md | 59 | STATUS/LEDGER | no |
| docs/render-backend-seams/terrain-subpass-recon-1.md | 285 | STATUS/LEDGER | yes (1): RenderCore/terrain_subpass_contract.h |
| docs/render-backend-seams/tex-latch-executor-recon-1.md | 316 | STATUS/LEDGER | no |
| docs/render-backend-seams/texture-unit-ownership-recon-1.md | 44 | STATUS/LEDGER | no |
| docs/render-backend-seams/torrin-enhanced-run-recon-1.md | 61 | STATUS/LEDGER | no |
| docs/render-backend-seams/torrin-multiwarrior-patrol-soak-1-findings.md | 84 | STATUS/LEDGER | no |
| docs/render-backend-seams/vfx-applypipeline-routing-1.md | 75 | STATUS/LEDGER | no |
| docs/render-backend-seams/vfx-applypipeline-routing-recon-1.md | 85 | STATUS/LEDGER | no |
| docs/render-backend-seams/vfx-pipeline-registration-1.md | 67 | STATUS/LEDGER | no |
| docs/render-backend-seams/vfx-pipeline-registration-recon-1.md | 93 | STATUS/LEDGER | no |
| docs/render-backend-seams/vfx-visual-gate-1-plan.md | 54 | STATUS/LEDGER | no |
| docs/render-backend-seams/viewport-class-ownership-recon-1.md | 45 | STATUS/LEDGER | no |
| docs/render-backend-seams/visual-diff-perceptual-harness-1.md | 58 | STATUS/LEDGER | no |
| docs/render-backend-seams/vulkan-island-template.md | 253 | STATUS/LEDGER | no |
| docs/render-backend-seams/vulkan-readiness-audit-1.md | 71 | STATUS/LEDGER | no |
| docs/render-backend-seams/water-armed-applypipeline-routing-1.md | 65 | STATUS/LEDGER | no |
| docs/render-backend-seams/water-armed-pipeline-registration-1.md | 59 | STATUS/LEDGER | no |
| docs/render-backend-seams/xform-convention-harness-recon-1.md | 24 | STATUS/LEDGER | no |
| docs/render-backend-seams/xform-convention-ledger-1.md | 127 | STATUS/LEDGER | no |
| docs/render-binding-registry.md | 258 | LIVE-REFERENCE | yes (2): scripts/mcp/repo_intel_server.py ... |
| docs/render-contract-audit-2026-05-14.md | 67 | STATUS/LEDGER | no |
| docs/render-contract.md | 566 | LIVE-REFERENCE | no |
| docs/render-pass-gpu-timing-recon.md | 145 | STATUS/LEDGER | no |
| docs/render-perf-snapshot.md | 140 | LIVE-REFERENCE | no |
| docs/render-pipeline-map.md | 367 | LIVE-REFERENCE | no |
| docs/renderdoc-automation.md | 128 | LIVE-REFERENCE | no |
| docs/renderpass-contract-spec.md | 89 | LIVE-REFERENCE | no |
| docs/renderworld-frame-inspector-recon.md | 181 | STATUS/LEDGER | no |
| docs/renderworld-immediate-draw-regression-recon.md | 202 | STATUS/LEDGER | no |
| docs/renderworld-pass-graph-audit.md | 136 | STATUS/LEDGER | no |
| docs/renderworld_arc_status.md | 171 | STATUS/LEDGER | yes (1): RenderCore/RendererFeatureRegistry.h |
| docs/renderworld_migration_guide.md | 729 | LIVE-REFERENCE | yes (1): scripts/check-vfx-no-objectid.sh |
| docs/review-feedback-schema.md | 180 | STALE-RULES | yes (1): scripts/read_review_feedback.py |
| docs/sensor-contact-presentation-recon.md | 97 | STATUS/LEDGER | yes (1): RenderCore/MechVisualState.h |
| docs/shadow-frustum-audit.md | 88 | STATUS/LEDGER | no |
| docs/shadow-pipeline-recon.md | 146 | STATUS/LEDGER | no |
| docs/shadow-rv-arc-recon.md | 493 | STATUS/LEDGER | no |
| docs/shadow-soak-1.md | 213 | STATUS/LEDGER | no |
| docs/shadow-static-buildings-recon.md | 110 | STATUS/LEDGER | no |
| docs/shadow-static-buildings-registry-recon.md | 68 | STATUS/LEDGER | no |
| docs/splits/gameos-graphics-split-1-plan.md | 33 | STATUS/LEDGER | yes (1): GameOS/gameos/gameos_graphics_internal.h |
| docs/static-building-skip-broadening-recon.md | 551 | STATUS/LEDGER | no |
| docs/static-prop-lighting-audit.md | 331 | STATUS/LEDGER | yes (1): GuiRuntime/EditorInspector.cpp |
| docs/static-prop-rv-arc-audit.md | 213 | STATUS/LEDGER | no |
| docs/static-prop-v3-soak-state.md | 209 | STATUS/LEDGER | no |
| docs/staticprop-material-orm-normal-recon.md | 288 | STATUS/LEDGER | no |
| docs/superpowers/strategy/ai-agent-operating-manual.md | 203 | STALE-RULES | no |
| docs/superpowers/strategy/asset-cook-pipeline-architecture.md | 188 | LIVE-REFERENCE | yes (3): scripts/check-asset-manifest-fixtures.py ... |
| docs/superpowers/strategy/baseline-a-capture-runbook.md | 124 | LIVE-REFERENCE | no |
| docs/superpowers/strategy/data-ownership-registry-strategy.md | 164 | LIVE-REFERENCE | yes (2): tools/mod_install/resolver.py ... |
| docs/superpowers/strategy/editor-superpowers-roadmap.md | 148 | STATUS/LEDGER | no |
| docs/superpowers/strategy/mc2-modding-toolchain-architecture.md | 244 | LIVE-REFERENCE | no |
| docs/superpowers/strategy/mod-packaging-deploy-architecture.md | 220 | LIVE-REFERENCE | yes (2): tools/mc2mod/mc2mod.py ... |
| docs/superpowers/strategy/mod-virtual-filesystem-design.md | 245 | LIVE-REFERENCE | yes (1): tools/mod_install/resolver.py |
| docs/superpowers/strategy/props-modernization-architecture.md | 227 | STATUS/LEDGER | no |
| docs/superpowers/strategy/release-train-governance-manual.md | 195 | STALE-RULES | no |
| docs/superpowers/strategy/runtime-bridge-architecture.md | 195 | STATUS/LEDGER | yes (1): code/mission.cpp |
| docs/superpowers/strategy/superpowers-execution-roadmap.md | 249 | STALE-RULES | yes (6): mclib/file.cpp ... |
| docs/superpowers/strategy/telemetry-oracle-cockpit-architecture.md | 224 | STATUS/LEDGER | no |
| docs/superpowers/strategy/tool-preview-rendering-architecture-v2.md | 187 | STATUS/LEDGER | no |
| docs/superpowers/strategy/tool-preview-rendering-architecture.md | 144 | STATUS/LEDGER | no |
| docs/superpowers/strategy/vfx-modernization-roadmap.md | 203 | STATUS/LEDGER | no |
| docs/superpowers/strategy/visual-regression-lab-architecture.md | 249 | STATUS/LEDGER | yes (2): GameOS/gameos/gos_visual_capture.h ... |
| docs/tactical-presentation-visual-state-capture.md | 99 | STATUS/LEDGER | no |
| docs/terrain-8z-A5-setuptextures-deletion-safety.md | 292 | STATUS/LEDGER | no |
| docs/terrain-8z-deletion-plan.md | 384 | STATUS/LEDGER | no |
| docs/terrain-8z-fastpath-drop-recon.md | 197 | STATUS/LEDGER | no |
| docs/terrain-clipmap-bridge.md | 217 | LIVE-REFERENCE | yes (1): tools/terrain_beautify/test_visual_heightfield_clipmap_bridge.py |
| docs/terrain-colormap-modernization-debt.md | 81 | STATUS/LEDGER | no |
| docs/terrain-decal-lighting-recon.md | 225 | STATUS/LEDGER | no |
| docs/terrain-height-audit.md | 210 | STATUS/LEDGER | yes (1): scripts/terrain_height_audit.py |
| docs/terrain-lighting-soak.md | 123 | STATUS/LEDGER | no |
| docs/terrain-lod-chunk-design.md | 400 | STATUS/LEDGER | no |
| docs/terrain-lod-chunk-plan.md | 913 | STATUS/LEDGER | no |
| docs/terrain-lod-chunk-spec.md | 827 | STATUS/LEDGER | no |
| docs/terrain-resample-plan.md | 273 | STATUS/LEDGER | no |
| docs/terrain-rv-arc-audit.md | 276 | STATUS/LEDGER | no |
| docs/terrain-rv-arc-recon.md | 233 | STATUS/LEDGER | no |
| docs/terrain-t3-map-audit.md | 379 | STATUS/LEDGER | no |
| docs/testing-strategy-engine-hooks.md | 183 | STATUS/LEDGER | no |
| docs/testing-strategy.md | 432 | LIVE-REFERENCE | yes (4): GameOS/gameos/gameosmain.cpp ... |
| docs/testing/camera-harness-recon-1.md | 48 | STATUS/LEDGER | no |
| docs/testing/cross-subsystem-audit-recon-1.md | 166 | STATUS/LEDGER | no |
| docs/testing/deploy-asset-contract-harness-1-recon.md | 94 | STATUS/LEDGER | no |
| docs/testing/dev-efficiency-bootstrap-sweep-1.md | 104 | STATUS/LEDGER | no |
| docs/testing/fit-parse-harness-1-recon.md | 97 | STATUS/LEDGER | no |
| docs/testing/fitini-inmem-harness-bootstrap-recon-1.md | 63 | STATUS/LEDGER | yes (2): tests/unit/heap_shim.cpp ... |
| docs/testing/gameos-casefold-harness-1-recon.md | 46 | STATUS/LEDGER | no |
| docs/testing/gameos-pathsep-harness-1.md | 55 | STATUS/LEDGER | no |
| docs/testing/gameos-platform-boundary-recon-1.md | 98 | STATUS/LEDGER | no |
| docs/testing/gameos-timing-math-harness-1.md | 85 | STATUS/LEDGER | no |
| docs/testing/glstate-guard-epilogue-recon-1.md | 106 | STATUS/LEDGER | yes (3): GameOS/gameos/gl_state_guard.h ... |
| docs/testing/golden-scene-manifest.md | 89 | LIVE-REFERENCE | yes (1): scripts/golden_scene.py |
| docs/testing/golden-scene-parity.md | 193 | LIVE-REFERENCE | yes (1): scripts/golden_compare.py |
| docs/testing/harness-target-sweep-2.md | 112 | STATUS/LEDGER | no |
| docs/testing/harness-target-sweep-3.md | 111 | STATUS/LEDGER | no |
| docs/testing/ibl-hdri-asset-pack-manifest-1.md | 63 | STATUS/LEDGER | no |
| docs/testing/lighting-staticprop-harness-recon-1.md | 95 | STATUS/LEDGER | yes (1): tools/ibl_registry_contract_harness/ibl_registry_contract_harness.cpp |
| docs/testing/mechbay-harness-recon-1.md | 48 | STATUS/LEDGER | no |
| docs/testing/minimal-viable-harness.md | 163 | LIVE-REFERENCE | yes (5): tests/unit/heap_shim.cpp ... |
| docs/testing/objmgr-contract-harness-1-recon.md | 109 | STATUS/LEDGER | no |
| docs/testing/shader-variant-matrix-harness-1.md | 67 | STATUS/LEDGER | no |
| docs/testing/smoke-evidence-classifier-1.md | 56 | STATUS/LEDGER | no |
| docs/testing/smoke-fingerprint-full-coverage-1.md | 54 | STATUS/LEDGER | no |
| docs/testing/smoke-first-launch-crash-silent-recon-1.md | 87 | STATUS/LEDGER | yes (1): scripts/smoke_lib/crash_evidence.py |
| docs/testing/subsystem-harness-arc-1.md | 263 | STATUS/LEDGER | yes (2): tools/contract_harness_common/contract_harness.h ... |
| docs/thermal-ir-design.md | 65 | STATUS/LEDGER | no |
| docs/thermal-view-mech-heat-mvp-defer.md | 67 | STATUS/LEDGER | no |
| docs/tier1_env_vars.md | 1436 | LIVE-REFERENCE | yes (22): GameOS/gameos/gameos_graphics.cpp ... |
| docs/toolchain-bom.md | 35 | LIVE-REFERENCE | yes (1): scripts/check-toolchain-bom.py |
| docs/track-r-closeout-opus.md | 152 | STATUS/LEDGER | no |
| docs/trackv-lighting-consistency-opus-1.md | 364 | STATUS/LEDGER | no |
| docs/trackv-post-grounding-soak-1.md | 230 | STATUS/LEDGER | no |
| docs/trackv-status.md | 80 | STATUS/LEDGER | no |
| docs/trackv-vfx-payoff-opus-1.md | 122 | STATUS/LEDGER | no |
| docs/tracy-harness.md | 152 | LIVE-REFERENCE | yes (1): tools/tracy_capture_compare.py |
| docs/transparent-sorting-policy-recon.md | 213 | STATUS/LEDGER | no |
| docs/tree-override-lod-mvp-1-plan.md | 253 | STATUS/LEDGER | no |
| docs/tree-override-lod-spec.md | 126 | STATUS/LEDGER | no |
| docs/ui-art-reconstruction.md | 113 | STATUS/LEDGER | no |
| docs/ui-imgui-port-guide.md | 133 | LIVE-REFERENCE | no |
| docs/v-ibl-static-1-plan.md | 493 | STATUS/LEDGER | no |
| docs/v-material-pbr-2-plan.md | 298 | STATUS/LEDGER | no |
| docs/v-staticprop-visual-review-audit.md | 341 | STATUS/LEDGER | no |
| docs/verify-primitive.md | 130 | LIVE-REFERENCE | yes (2): GameOS/gameos/mc2_verify.cpp ... |
| docs/vfx-3d-mesh-substrate-recon.md | 347 | STATUS/LEDGER | no |
| docs/vfx-bloom-participation.md | 79 | STATUS/LEDGER | no |
| docs/vfx-gpu-sim-spec.md | 258 | STATUS/LEDGER | yes (2): RenderCore/RendererFeatureRegistry.h ... |
| docs/vfx-oracle-coverage.md | 183 | STATUS/LEDGER | no |
| docs/vfx-originals-restoration-design.md | 190 | STATUS/LEDGER | yes (2): RenderCore/RendererFeatureRegistry.h ... |
| docs/vfx-overdraw-audit.md | 150 | STATUS/LEDGER | yes (1): RenderCore/RendererFeatureRegistry.h |
| docs/vfx-payoff-capture-matrix.md | 89 | STATUS/LEDGER | no |
| docs/vfx-rv-arc-recon.md | 415 | STATUS/LEDGER | no |
| docs/viewmode-capture-matrix.md | 119 | STATUS/LEDGER | no |
| docs/visual-baseline-howto.md | 187 | LIVE-REFERENCE | no |
| docs/visual-tuning-profiles.md | 184 | LIVE-REFERENCE | yes (1): GameOS/gameos/visual_tuning_profile.cpp |
| docs/visual-verify.md | 295 | LIVE-REFERENCE | yes (1): tools/visual_verify/bless.py |
| docs/water-lighting-plan.md | 139 | STATUS/LEDGER | no |
| docs/water-reflection-pass-plan.md | 263 | STATUS/LEDGER | no |
| docs/water-reflection-plan.md | 188 | STATUS/LEDGER | no |
| docs/water-rv-arc-audit.md | 139 | STATUS/LEDGER | no |
| docs/water-rv-arc-recon.md | 263 | STATUS/LEDGER | no |
| docs/weapon-tool-design.md | 131 | STATUS/LEDGER | yes (1): tools/mc2weapon/mc2weapon.py |
| docs/worktree-discipline.md | 17 | STALE-RULES | no |

## Notes on specific rows

- `.claude/GOLDEN-v051-v052-RETIREMENT-CASE.md` (STALE-RULES): golden lane retirement; lanes archived; code-coupled
- `.claude/agents/DOMAINS.md` (STALE-RULES): advisor fleet routing
- `.claude/agents/mc2-build-system-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-cpu-gpu-offload-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-gameos-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-mech-import-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-mech-skeletal-anim-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-mech-update-geometry-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-mission-data-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-render-contract-synthesizer.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-render-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-render-perf-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-shader-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-terrain-indirect-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/agents/mc2-water-depth-expert.md` (STALE-RULES): advisor/subagent definition; reset = no parallel agents
- `.claude/prompts/distill-session-into-advisor-agent.md` (STALE-RULES): advisor fleet creation prompt
- `.claude/prompts/dump-render-observations.md` (STALE-RULES): multi-agent observation dump
- `.claude/skills/adversarial-plan-review.md` (STALE-RULES): subagent review protocol
- `.claude/skills/cost-split-recon-bucket-design.md` (STALE-RULES): recon-bucket process design
- `.claude/skills/greybeard.md` (STALE-RULES): review discipline skill
- `.claude/skills/mc2-deploy.md` (LIVE-REFERENCE): deploy_payload; lane-era wording
- `.claude/skills/mc2-gsd-planner-executor.md` (STALE-RULES): planner/executor multi-agent
- `.claude/skills/mc2-render-spine-advisor.md` (STALE-RULES): advisor fleet persona
- `.claude/skills/orchestrator-review-protocol.md` (STALE-RULES): subagent bookend review protocol
- `docs/LANE_COMPLETION_REPORT.template.md` (STALE-RULES): lane report template; code-coupled
- `docs/README.md` (LIVE-REFERENCE): docs index; needs refresh after archive
- `docs/RELEASE-PROCESS.md` (LIVE-REFERENCE): keep; lane-name examples need update
- `docs/asset-pipeline.md` (LIVE-REFERENCE): checked by check-asset-pipeline-doc.sh
- `docs/background-launch-inventory.md` (LIVE-REFERENCE): generated; checked by script
- `docs/build-parallel-and-tooling.md` (STALE-RULES): worktree/deploy-dir map
- `docs/deploy-isolation.md` (LIVE-REFERENCE): lane-lock vocabulary; refresh
- `docs/dev-shell.md` (LIVE-REFERENCE): uses deploy-lane paths; refresh
- `docs/dev-velocity.md` (STALE-RULES): standing up agent lanes
- `docs/disciplines.md` (STALE-RULES): subagent bookend + multi-lane rules
- `docs/hzb-depth-convention.md` (LIVE-REFERENCE): depth convention
- `docs/known_issues.md` (LIVE-REFERENCE): current list; prune on read
- `docs/load_bearing_pointers.md` (STALE-RULES): memory-pointer index extracted from CLAUDE.md 2026-05-24
- `docs/material-m0-contract.md` (LIVE-REFERENCE): GPU material contract
- `docs/modding-animations.md` (LIVE-REFERENCE): modding guide
- `docs/modding-effects.md` (LIVE-REFERENCE): modding guide
- `docs/modding-guide.md` (LIVE-REFERENCE): modding guide
- `docs/modding-weapons.md` (LIVE-REFERENCE): modding guide
- `docs/modding/debugging-render-issues.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/building-import/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/getting-started.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/imgui-editing/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/mech-import/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/mechanism-map.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/mission-edit/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/texture-override/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/tree-import/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/variant-add/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/weapon-add/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/examples/weapon-edit/README.md` (LIVE-REFERENCE): modding guide
- `docs/modding/renderer-feature-flags.md` (LIVE-REFERENCE): verify gate list vs registry
- `docs/modding/rendering-overview.md` (LIVE-REFERENCE): modding guide
- `docs/qwen-local-workflow.md` (LIVE-REFERENCE): local LLM workflow
- `docs/qwen-system-prompts.md` (LIVE-REFERENCE): local LLM prompts
- `docs/release-checklist.md` (LIVE-REFERENCE): keep; verify vs release flow
- `docs/render-backend-seams/GLSTATE-GUARD-ADOPTION-1-orchestrator-prompt.md` (STALE-RULES): paste-into-opus orchestrator prompt
- `docs/render-backend-seams/binding-slot-occupancy.md` (LIVE-REFERENCE): occupancy; script-checked
- `docs/render-backend-seams/buffer-lifetime-ledger.md` (LIVE-REFERENCE): ledger; script-checked
- `docs/render-backend-seams/gated-layout-macros.md` (LIVE-REFERENCE): script-checked
- `docs/render-backend-seams/nvidia-readiness-runbook.md` (LIVE-REFERENCE): runbook; NVIDIA not yet run
- `docs/render-backend-seams/pipeline-key-schema.md` (LIVE-REFERENCE): schema; script-checked
- `docs/render-backend-seams/shader-permutation-inventory.md` (LIVE-REFERENCE): script-checked
- `docs/render-backend-seams/split-path-pairs.md` (LIVE-REFERENCE): script-checked
- `docs/render-contract.md` (LIVE-REFERENCE): large; synthesizer-maintained
- `docs/render-perf-snapshot.md` (LIVE-REFERENCE): perf agent reads; refreshed deliberately
- `docs/renderworld_migration_guide.md` (LIVE-REFERENCE): contributor migration guide; script-referenced
- `docs/review-feedback-schema.md` (STALE-RULES): review-pack lane consumption; code-coupled
- `docs/superpowers/strategy/ai-agent-operating-manual.md` (STALE-RULES): agent operating manual
- `docs/superpowers/strategy/asset-cook-pipeline-architecture.md` (LIVE-REFERENCE): CLAUDE.md names as live
- `docs/superpowers/strategy/baseline-a-capture-runbook.md` (LIVE-REFERENCE): runbook
- `docs/superpowers/strategy/data-ownership-registry-strategy.md` (LIVE-REFERENCE): CLAUDE.md names as live
- `docs/superpowers/strategy/mod-packaging-deploy-architecture.md` (LIVE-REFERENCE): CLAUDE.md names as live
- `docs/superpowers/strategy/mod-virtual-filesystem-design.md` (LIVE-REFERENCE): CLAUDE.md names as live
- `docs/superpowers/strategy/release-train-governance-manual.md` (STALE-RULES): train/worktree governance
- `docs/superpowers/strategy/superpowers-execution-roadmap.md` (STALE-RULES): multi-lane execution roadmap
- `docs/terrain-clipmap-bridge.md` (LIVE-REFERENCE): interface contract; test-referenced
- `docs/testing/golden-scene-manifest.md` (LIVE-REFERENCE): script-referenced
- `docs/testing/golden-scene-parity.md` (LIVE-REFERENCE): script-referenced
- `docs/testing/minimal-viable-harness.md` (LIVE-REFERENCE): test-referenced
- `docs/tier1_env_vars.md` (LIVE-REFERENCE): MC2_* knobs; many code refs
- `docs/toolchain-bom.md` (LIVE-REFERENCE): checked by check-toolchain-bom.py
- `docs/visual-verify.md` (LIVE-REFERENCE): tools/visual_verify; note VK capture gap
- `docs/worktree-discipline.md` (STALE-RULES): worktree rules; reset = one lane

## Conflicts and caveats

- The task premise says `docs/superpowers/` was archived to E:, but all 19 `docs/superpowers/**` files are still tracked and present on disk. `A:/Games/mc2-main/CLAUDE.md` also says `docs/superpowers/strategy/` is still live. Classified per the CLAUDE.md wording (2 LIVE, 3 STALE, rest STATUS); confirm before archiving.
- `docs/tier1_env_vars.md`, `docs/known_issues.md` and `docs/render-contract.md` are kept as LIVE but are large and likely contain stale lane and gate wording. They need a content pass, not just a keep decision.
- `.claude/VK-ARC-STATE.md` (3809 lines) is the largest file and is a rolling state block. Keep one current copy, not the whole history.
- `docs/observations/2026-05-25-*-pipeline-map.md` and `pipeline-master-index.md` are classed STATUS but are read by `mc2-render-expert`. Promote to LIVE-REFERENCE if that agent is kept.
- `.claude/agents/*.md` is classed STALE-RULES as an advisor fleet. These files are loaded as subagent config, so removing them is a config change, not just a doc move.
- Not all referenced-by-code hits are real links. Examples: `docs/modding/examples/tree-import/...` mentions `mechanism-map.md` in-doc; `3rdparty/` hits come from the generic `README.md` basename.
