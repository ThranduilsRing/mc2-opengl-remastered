# Docs Index

## Start here

- [docs/STARTER-GUIDE.md](STARTER-GUIDE.md) — orientation for new contributors/agents
- [../README.md](../README.md) — project overview
- [../BUILD-WIN.md](../BUILD-WIN.md) — build, deploy, first smoke test

## For modders

- [docs/modding-guide.md](modding-guide.md)
- [docs/beta/README_modder_beta.md](beta/README_modder_beta.md)
- [docs/modding-effects.md](modding-effects.md)
- [docs/modding-animations.md](modding-animations.md)
- [docs/asset-pipeline.md](asset-pipeline.md)
- [docs/GLOSSARY.md](GLOSSARY.md)
- [MODDER-REPORT-techscript-brainspecial.md](MODDER-REPORT-techscript-brainspecial.md) - MC2R engine support for your TechScript / mission_specials system
- [gosfx-animation-lifecycle-recon.md](gosfx-animation-lifecycle-recon.md) - gosFX Effect Animation - Full Lifecycle Recon (load - spawn - animate - hit - unload)
- [gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md) - gosFX Modder Drop-In: New Effect - Tie to Weapon
- [modding-weapons.md](modding-weapons.md) - Modding weapons - annotated equipmentdef.fit reference
- [ui-imgui-port-guide.md](ui-imgui-port-guide.md) - ImGui UI port - integration guide (for the UI modder)
- [weapon-tool-design.md](weapon-tool-design.md) - Weapon Viewer / Modder Tool - Design

## Engine reference

- [docs/critical_inline_rules.md](critical_inline_rules.md)
- [docs/tier1_env_vars.md](tier1_env_vars.md)
- [.planning/codebase/ARCHITECTURE.md](../.planning/codebase/ARCHITECTURE.md) — codebase map
- [docs/render-contract.md](render-contract.md)
- [docs/cxx17-coding-rules.md](cxx17-coding-rules.md)
- [docs/disciplines.md](disciplines.md)
- [docs/deploy-isolation.md](deploy-isolation.md) — hardlinked deploy dirs: why a "free" lane dir can be someone else's install
- [docs/known_issues.md](known_issues.md)
- [ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md) - MC2 Architecture Map (orientation, not reference)
- [architecture.md](architecture.md) - MC2 OpenGL Architecture Reference
- [render-pipeline-map.md](render-pipeline-map.md) - Render Pipeline Map
- [renderpass-contract-spec.md](renderpass-contract-spec.md) - Render Pass Contract Registry - Spec
- [render-binding-registry.md](render-binding-registry.md) - Render Binding Registry
- [renderworld_migration_guide.md](renderworld_migration_guide.md) - RenderWorld Contributor Migration Guide
- [debug_state_schema.md](debug_state_schema.md) - MC2 Debug State JSON Schema
- [data-provenance.md](data-provenance.md) - Game Data Provenance
- [toolchain-bom.md](toolchain-bom.md) - Toolchain bill-of-materials
- [engine-standalone-seams.md](engine-standalone-seams.md) - Engine Standalone Seams - ENGINE-STANDALONE-SEAMS-RECON-1
- [micro_scene_spec.md](micro_scene_spec.md) - MC2 Micro-Scene Regression Spec
- [material-m0-contract.md](material-m0-contract.md) - Material M0 - GPU material contract pin
- [lighting-divergence-map.md](lighting-divergence-map.md) - Lighting divergence map (ARCH-CONVERGE-S2)
- [amd-driver-rules.md](amd-driver-rules.md) - AMD Driver Rules (RX 7900 XTX, driver 26.3.1)
- [cement-overlay-codemap.md](cement-overlay-codemap.md) - Cement/Overlay/Shadow Codemap
- [asset-manifest-schema.md](asset-manifest-schema.md) - Asset manifest schema (scaffold)
- [hzb-depth-convention.md](hzb-depth-convention.md) - HZB / Reverse-Z Depth Convention
- [hud-tab-registry.md](hud-tab-registry.md) - HUD tab registry (MC2_HUD_IMGUI_TABS, slice C)
- [load_bearing_pointers.md](load_bearing_pointers.md) - Load-bearing memory pointers (read before touching the area)
- [mcp-render-state.md](mcp-render-state.md) - MC2 Render-State MCP Server
- [testing-strategy.md](testing-strategy.md) - MC2 Automated Testing Strategy
- [testing-strategy-engine-hooks.md](testing-strategy-engine-hooks.md) - Engine-Side Hooks Required by the Testing Strategy
- [vfx-oracle-coverage.md](vfx-oracle-coverage.md) - VFX Oracle Coverage Matrix
- [vfx-bloom-participation.md](vfx-bloom-participation.md) - VFX bloom participation (VFX-BLOOM-PARTICIPATION-1)
- [terrain-clipmap-bridge.md](terrain-clipmap-bridge.md) - Terrain clipmap bridge - bake-mip / morph interface contract
- [vfx-gpu-sim-spec.md](vfx-gpu-sim-spec.md) - VFX GPU Simulation Spec - first class: CardCloud (VFX-GPU-SIM-SPEC-0)

## Ledgers

- [docs/active_campaigns.md](active_campaigns.md)
- [docs/renderworld_arc_status.md](renderworld_arc_status.md)
- Historical arc docs (model-override, R-V arc recons, terrain-8z, trackv, superpowers, observations, plans, recon, seam recon-1 reports) were archived 2026-10-09 to `E:/mc2-archive-2026-10/docs/arc-docs-2026-10-09.tar.gz` (sha256 beside it); recover single files from git history.
- [docs/render-perf-snapshot.md](render-perf-snapshot.md)
- [2026-05-26-next-gen-rts-engine-vision.md](2026-05-26-next-gen-rts-engine-vision.md) - Next-Gen RTS Engine Vision
- [DECAL-INTEGRATE-1.md](DECAL-INTEGRATE-1.md) - DECAL-INTEGRATE-1 - joining the two parked projected-decal branches
- [HANDOFF-model-override-lod.md](HANDOFF-model-override-lod.md) - HANDOFF - Model-Override + Tree LOD (resume cold)
- [POSTPROCESS-COMPUTE-BLUR-1.md](POSTPROCESS-COMPUTE-BLUR-1.md) - POSTPROCESS-COMPUTE-BLUR-1
- [VFX-DISTORTION-1.md](VFX-DISTORTION-1.md) - VFX-DISTORTION-1 - heat-haze refraction (procedural, clean-room)
- [VFX-SCENECOLOR-GRAB-1.md](VFX-SCENECOLOR-GRAB-1.md) - VFX-SCENECOLOR-GRAB-1
- [VISUAL-CAPTURE-MATRIX-1-DESIGN.md](VISUAL-CAPTURE-MATRIX-1-DESIGN.md) - VISUAL-CAPTURE-MATRIX-1 Design
- [advisor-landscape-audit-2026-05-14.md](advisor-landscape-audit-2026-05-14.md) - Advisor Landscape Audit - 2026-05-14
- [animated-prop-cook-recon.md](animated-prop-cook-recon.md) - Animated Prop Cook Recon
- [asset-modernization-recon.md](asset-modernization-recon.md) - Asset modernization recon - geometry import + texture compression
- [baseline-A-post-8z.md](baseline-A-post-8z.md) - Baseline A - post-8z frozen render/perf reference
- [bt2018-skel-1b-gpu-recon.md](bt2018-skel-1b-gpu-recon.md) - BT2018-SKEL-1B-GPU - recon + plan (NOT yet implemented)
- [bt2018-skel-1b-gpu-skinning-recon-2.md](bt2018-skel-1b-gpu-skinning-recon-2.md) - BT2018-SKEL-ENGINE-1B-GPU-SKINNING-RECON-2 - compatibility PROVEN
- [bt2018-skel-1b-runtime-scope.md](bt2018-skel-1b-runtime-scope.md) - BT2018-SKEL-ENGINE-1B-RUNTIME - imported mech per-frame animation
- [bt2018-skel-gpu-draw-engagement-recon-1.md](bt2018-skel-gpu-draw-engagement-recon-1.md) - BT2018-SKEL-GPU-DRAW-ENGAGEMENT-RECON-1 - findings
- [editor-build-architecture-integrity-audit.md](editor-build-architecture-integrity-audit.md) - Editor Build - Architecture Integrity Audit
- [editor-chunk-path-parity.md](editor-chunk-path-parity.md) - Editor Chunk-Path Parity - 8z-B Gate Document
- [engine-closure-audit.md](engine-closure-audit.md) - Engine Closure Audit - pre-Track-V receipt
- [gpu-static-prop-cull-lessons.md](gpu-static-prop-cull-lessons.md) - GPU static-prop path - cull infrastructure lessons
- [hzb-staticprop-cull-readiness.md](hzb-staticprop-cull-readiness.md) - HZB Static-Prop Cull - Readiness
- [hzb-staticprop-cull-recon.md](hzb-staticprop-cull-recon.md) - HZB Static-Prop Cull - Recon
- [hzb-visibility-mvp.md](hzb-visibility-mvp.md) - HZB Visibility Substrate (MVP)
- [ibl-plan.md](ibl-plan.md) - IBL Plan (V-IBL-STATIC-0)
- [material-abi-unification-recon.md](material-abi-unification-recon.md) - Material ABI Unification Recon - MATERIAL-ABI-UNIFICATION-RECON-1
- [mech-lighting-plan.md](mech-lighting-plan.md) - Mech Lighting Plan - First Safe Visual Improvement (MECH-LIGHTING-PLAN-0)
- [mech-normals-audit.md](mech-normals-audit.md) - MECH-NORMALS-AUDIT-0
- [mech-rv-arc-recon.md](mech-rv-arc-recon.md) - Mech R-V Lane - Arc Recon (MECH-RV-ARC-RECON-0)
- [model-override-lighting-lod-recon.md](model-override-lighting-lod-recon.md) - MODEL-OVERRIDE - Lighting + LOD perf-architecture recon
- [model-override-lighting-ownership-recon.md](model-override-lighting-ownership-recon.md) - MODEL-OVERRIDE - Light-slot ownership recon (Task 0 M2 gate result: **STOP**)
- [model-override-mvp-notes.md](model-override-mvp-notes.md) - MODEL-OVERRIDE-MVP-1 - Implementation Notes
- [model-override-mvp-plan.md](model-override-mvp-plan.md) - Model Override MVP Implementation Plan (MODEL-OVERRIDE-MVP-1)
- [model-override-system-recon.md](model-override-system-recon.md) - Model Override System - Recon & Design (MODEL-OVERRIDE-SYSTEM-RECON-1)
- [modernization-roadmap-2026-06-09.md](modernization-roadmap-2026-06-09.md) - MC2-OpenGL Modernization Roadmap
- [nvidia-hardening-s2.md](nvidia-hardening-s2.md) - NVIDIA Hardening S2 - plan
- [perf-attribution-plan.md](perf-attribution-plan.md) - Perf Attribution Plan (R0-R3) - supersedes the S2-only framing
- [render-contract-audit-2026-05-14.md](render-contract-audit-2026-05-14.md) - Render Contract Audit - 2026-05-14
- [sensor-contact-presentation-recon.md](sensor-contact-presentation-recon.md) - Sensor / contact presentation - recon (SENSOR-CONTACT-PRESENTATION-RECON-1)
- [shadow-soak-1.md](shadow-soak-1.md) - SHADOW-SOAK-1: First Debug/Tuning Soak
- [static-building-skip-broadening-recon.md](static-building-skip-broadening-recon.md) - Static-Building Skip Broadening Recon
- [static-prop-lighting-audit.md](static-prop-lighting-audit.md) - Static-Prop Lighting Audit (V-LIGHTING-STATIC-0)
- [static-prop-v3-soak-state.md](static-prop-v3-soak-state.md) - Static-Prop Extraction v3 - Soak State (pre-authority-flip)
- [staticprop-material-orm-normal-recon.md](staticprop-material-orm-normal-recon.md) - Static-Prop Material ORM + Normal-Map Recon - STATICPROP-MATERIAL-ORM-NORMAL-RECON-1
- [terrain-height-audit.md](terrain-height-audit.md) - Terrain Height Audit (TERRAIN-HEIGHT-AUDIT-0)
- [terrain-resample-plan.md](terrain-resample-plan.md) - Terrain Visual Resample Plan (TERRAIN-RESAMPLE-PLAN-0)
- [terrain-t3-map-audit.md](terrain-t3-map-audit.md) - Terrain T3 Map-Compatibility Audit (Phase 8z Gate)
- [thermal-ir-design.md](thermal-ir-design.md) - Thermal / IR view - design (THERMAL-IR-DESIGN-1)
- [thermal-view-mech-heat-mvp-defer.md](thermal-view-mech-heat-mvp-defer.md) - Thermal ViewMode - engine-bearing units read hot (THERMAL-VIEW-MECH-HOT-1)
- [trackv-lighting-consistency-opus-1.md](trackv-lighting-consistency-opus-1.md) - TRACKV-LIGHTING-CONSISTENCY-OPUS-1
- [trackv-post-grounding-soak-1.md](trackv-post-grounding-soak-1.md) - VISUAL-CAPTURE-MATRIX-SOAK-1: Track V Post + Grounding MVP
- [trackv-vfx-payoff-opus-1.md](trackv-vfx-payoff-opus-1.md) - TRACKV-VFX-PAYOFF-OPUS-1 - recon synthesis + plan
- [tree-override-lod-mvp-1-plan.md](tree-override-lod-mvp-1-plan.md) - TREE-OVERRIDE-LOD-MVP-1 Implementation Plan
- [tree-override-lod-spec.md](tree-override-lod-spec.md) - TREE-OVERRIDE-LOD-SPEC-1 - Design spec
- [v-ibl-static-1-plan.md](v-ibl-static-1-plan.md) - V-IBL-STATIC-1 Plan (recon + implementation slice)
- [v-material-pbr-2-plan.md](v-material-pbr-2-plan.md) - V-MATERIAL-PBR-2 - Cheap Specular for StaticPropOpaque (PLAN)
- [vfx-3d-mesh-substrate-recon.md](vfx-3d-mesh-substrate-recon.md) - VFX 3D Mesh GPU Substrate Recon
- [vfx-originals-restoration-design.md](vfx-originals-restoration-design.md) - VFX Originals Restoration - Design (CPU-oracle - GPU-sim arc)
- [vfx-overdraw-audit.md](vfx-overdraw-audit.md) - VFX Overdraw & Draw-Coverage Audit (VFX-OVERDRAW-AUDIT-0)
- [vfx-rv-arc-recon.md](vfx-rv-arc-recon.md) - VFX R-V Lane - Arc Recon (VFX-ARC-RECON-0)

## Tooling / Dev Harness Index

Core references:

- [docs/dev-shell.md](dev-shell.md) — live-edit loop (shader/UI hot-reload, one-shot commands, screenshots) without relaunching
- [docs/tier1_env_vars.md](tier1_env_vars.md) — every `MC2_*` gate: defaults, polarity, the add-a-gate checklist
- [docs/modding-guide.md §8](modding-guide.md#8-autonomous-development) — `--validate` headless validation mode + Tracy profiling
- [docs/release-checklist.md](release-checklist.md) — the zips-only release gate
- [docs/RELEASE-PROCESS.md](RELEASE-PROCESS.md) — end-to-end release runbook (version bump → package → gate → publish)
- [docs/tracy-harness.md](tracy-harness.md) — canonical Tracy capture/compare/baseline/regress harness (perf measurement + per-release baselines)
- [docs/visual-verify.md](visual-verify.md) — `tools/visual_verify/` region-level visual verification: capture every saved camera pose from a build, crop the known artifact regions, diff vs a blessed golden, emit a verdict report + contact sheets (automates the manual verification-fleet loop)
- [docs/renderdoc-automation.md](renderdoc-automation.md) — `tools/renderdoc/` draw-call attribution: capture a posed frame under RenderDoc, then walk pixel-history to report which draws wrote a screen region (shaders/blend/depth/marker) and dump full pipeline state for one draw — the automation the manual black-band tint-hunt needed (complements the Tier-5 parity hook)
- [../CONTRIBUTING.md](../CONTRIBUTING.md) — branch/PR flow, mandatory build config, the tier-1 smoke gate
- [ai-order-triage.md](ai-order-triage.md) - AI order triage playbook (META-FIX design F)
- [asan-autonomous-run-log.md](asan-autonomous-run-log.md) - ASan Autonomous Run Log - 2026-04-24
- [asan-follow-ups.md](asan-follow-ups.md) - ASan MVP - Follow-up Findings
- [asan-triage.md](asan-triage.md) - ASan Findings Triage - 2026-04-24
- [asset-viewer-backend-a-shader-contract.md](asset-viewer-backend-a-shader-contract.md) - Backend-A Shader Contract - static_prop.{vert,frag}
- [asset-viewer-backend-a-uvv-check.md](asset-viewer-backend-a-uvv-check.md) - Backend-A UV-V Verification Check (Task 8)
- [asset-viewer-mech-assembly-inspector.md](asset-viewer-mech-assembly-inspector.md) - Asset Viewer - Mech Assembly Inspector (Recon + Design)
- [background-launch-inventory.md](background-launch-inventory.md) - Background-launch inventory (generated)
- [build-parallel-and-tooling.md](build-parallel-and-tooling.md) - Parallel build, smoke, and deploy-dir/worktree map (STALE: 2026-06 multi-worktree era)
- [game-auto-tools.md](game-auto-tools.md) - Game Auto Tools
- [gameplay-driver.md](gameplay-driver.md) - Gameplay driver - driving a running mission from an agent
- [oracle-dynamic-pipeline-gate.md](oracle-dynamic-pipeline-gate.md) - Oracle Gate - Legacy Dynamic Pipeline (mech/vehicle/turret/effects)
- [perf-budget.md](perf-budget.md) - Perf budget + oracle harness
- [review-feedback-schema.md](review-feedback-schema.md) - Review-pack feedback - annotation schema and how a lane consumes it
- [tactical-presentation-visual-state-capture.md](tactical-presentation-visual-state-capture.md) - Tactical-presentation visual-state capture & validation
- [verify-primitive.md](verify-primitive.md) - MC2_VERIFY - the live data-contract guard primitive
- [vfx-payoff-capture-matrix.md](vfx-payoff-capture-matrix.md) - VFX payoff capture matrix (VFX-PAYOFF-CAPTURE-MATRIX-1)
- [viewmode-capture-matrix.md](viewmode-capture-matrix.md) - ViewMode capture matrix (TACTICAL-PRESENTATION-CAPTURE-MATRIX-1)
- [visual-baseline-howto.md](visual-baseline-howto.md) - Visual Baseline How-To (V-BASELINE-0)
- [visual-tuning-profiles.md](visual-tuning-profiles.md) - Visual Tuning Profiles - MISSION-VISUAL-TUNING-1

Fixtures shipped in the v0.5 cycle (all in-tree; one-liners — see
[docs/tier1_env_vars.md](tier1_env_vars.md) for full semantics):

- **Camera bookmarks** — the ImGui inspector's "Save Camera Bookmark" button
  (`code/gamecam.cpp`) writes a schema-valid bookmark JSON
  (`MC2_BOOKMARK_SAVE_DIR=<dir>`, default `saved_bookmarks/`);
  `MC2_VISUAL_BOOKMARK_CAPTURE=<bookmark.json>` replays the poses and captures
  frames at each one — the deterministic "see what I see" loop.
- **`MC2_CAMERA_PARK=x,y,rot,proj,alt[@frame]`** — one-shot camera pose pin at frame
  N (default 300), sim keeps running; stable captures without input scripting
  (`GameOS/gameos/gos_visual_capture.cpp`).
- **`MC2_DEBUG_VIEW=N`** — boot straight into a global full-scene debug render
  view (1=shadow 2=depth 3=albedo 4=overdraw 5=normals 6=chunk); the command
  channel's `debugview N` verb switches it at runtime.
- **`MC2_UI_COMMAND_FILE=<path>`** — headless UI driver: the engine consumes
  commands appended to the file (`toggle_pause`, `press pause.<BTN>`,
  `press dialog.YES|NO`, `debugview N`, `screenshot <name>`) and acks each
  line to `<path>.ack` (`code/missiongui.cpp`).
- **`MC2_BOOT_QUICKSAVE_AT=N` / `MC2_BOOT_QUICKLOAD_AT=M`** — one-shot
  quicksave/quickload at frame N/M via the same flags the pause-menu buttons
  set; M > N gives a deterministic save-then-load cycle in one headless run.
- **`tools/tracy_capture_compare.py`** — the canonical Tracy harness: capture,
  compare (zone-level before/after deltas), `baseline` (both arms at a fixed
  pose → dated baseline dir), and `regress` (re-capture + pass/warn/fail vs a
  baseline). Full usage, arms, hygiene rules, and the release baseline flow:
  [docs/tracy-harness.md](tracy-harness.md).

## Everything else

Point-in-time recon/slice reports and superseded docs live under [docs/archive/](archive/).
