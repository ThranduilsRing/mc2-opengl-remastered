<!-- doc-status: current | reviewed:2026-10-11 -->
# Docs Index

## Start here

- [docs/STARTER-GUIDE.md](STARTER-GUIDE.md) — orientation for new contributors/agents
- [../README.md](../README.md) — project overview
- [../BUILD-WIN.md](../BUILD-WIN.md) — build, deploy, first smoke test

## For modders

- [docs/modding-guide.md](modding-guide.md)
- [docs/modding-effects.md](modding-effects.md)
- [docs/modding-animations.md](modding-animations.md)
- [docs/asset-pipeline.md](asset-pipeline.md)
- [docs/content-pipeline.md](content-pipeline.md) - importing/replacing models: IR, cook contract, material schema, packages
- [docs/GLOSSARY.md](GLOSSARY.md)
- [MODDER-REPORT-techscript-brainspecial.md](MODDER-REPORT-techscript-brainspecial.md) - MC2R engine support for your TechScript / mission_specials system
- [gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md) - gosFX Modder Drop-In: New Effect - Tie to Weapon
- [modding-weapons.md](modding-weapons.md) - Modding weapons - annotated equipmentdef.fit reference
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
- [material-m0-contract.md](material-m0-contract.md) - Material M0 - GPU material contract pin
- [lighting-divergence-map.md](lighting-divergence-map.md) - Lighting divergence map (ARCH-CONVERGE-S2)
- [amd-driver-rules.md](amd-driver-rules.md) - AMD Driver Rules (RX 7900 XTX, driver 26.3.1)
- [asset-manifest-schema.md](asset-manifest-schema.md) - Asset manifest schema (scaffold)
- [hzb-depth-convention.md](hzb-depth-convention.md) - HZB / Reverse-Z Depth Convention
- [hud-tab-registry.md](hud-tab-registry.md) - HUD tab registry (MC2_HUD_IMGUI_TABS, slice C)
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
- [POSTPROCESS-COMPUTE-BLUR-1.md](POSTPROCESS-COMPUTE-BLUR-1.md) - POSTPROCESS-COMPUTE-BLUR-1
- [VFX-DISTORTION-1.md](VFX-DISTORTION-1.md) - VFX-DISTORTION-1 - heat-haze refraction (procedural, clean-room)
- [VFX-SCENECOLOR-GRAB-1.md](VFX-SCENECOLOR-GRAB-1.md) - VFX-SCENECOLOR-GRAB-1
- [VISUAL-CAPTURE-MATRIX-1-DESIGN.md](VISUAL-CAPTURE-MATRIX-1-DESIGN.md) - VISUAL-CAPTURE-MATRIX-1 Design
- [advisor-landscape-audit-2026-05-14.md](advisor-landscape-audit-2026-05-14.md) - Advisor Landscape Audit - 2026-05-14
- [asset-modernization-recon.md](asset-modernization-recon.md) - Asset modernization recon - geometry import + texture compression
- [baseline-A-post-8z.md](baseline-A-post-8z.md) - Baseline A - post-8z frozen render/perf reference
- [bt2018-skel-1b-runtime-scope.md](bt2018-skel-1b-runtime-scope.md) - BT2018-SKEL-ENGINE-1B-RUNTIME - imported mech per-frame animation
- [bt2018-skel-gpu-draw-engagement-recon-1.md](bt2018-skel-gpu-draw-engagement-recon-1.md) - BT2018-SKEL-GPU-DRAW-ENGAGEMENT-RECON-1 - findings
- [editor-build-architecture-integrity-audit.md](editor-build-architecture-integrity-audit.md) - Editor Build - Architecture Integrity Audit
- [editor-chunk-path-parity.md](editor-chunk-path-parity.md) - Editor Chunk-Path Parity - 8z-B Gate Document
- [gpu-static-prop-cull-lessons.md](gpu-static-prop-cull-lessons.md) - GPU static-prop path - cull infrastructure lessons
- [hzb-staticprop-cull-recon.md](hzb-staticprop-cull-recon.md) - HZB Static-Prop Cull - Recon
- [hzb-visibility-mvp.md](hzb-visibility-mvp.md) - HZB Visibility Substrate (MVP)
- [modernization-roadmap-2026-06-09.md](modernization-roadmap-2026-06-09.md) - MC2-OpenGL Modernization Roadmap
- [nvidia-hardening-s2.md](nvidia-hardening-s2.md) - NVIDIA Hardening S2 - plan
- [render-contract-audit-2026-05-14.md](render-contract-audit-2026-05-14.md) - Render Contract Audit - 2026-05-14
- [shadow-soak-1.md](shadow-soak-1.md) - SHADOW-SOAK-1: First Debug/Tuning Soak
- [static-prop-v3-soak-state.md](static-prop-v3-soak-state.md) - Static-Prop Extraction v3 - Soak State (pre-authority-flip)
- [thermal-ir-design.md](thermal-ir-design.md) - Thermal / IR view - design (THERMAL-IR-DESIGN-1)
- [thermal-view-mech-heat-mvp-defer.md](thermal-view-mech-heat-mvp-defer.md) - Thermal ViewMode - engine-bearing units read hot (THERMAL-VIEW-MECH-HOT-1)
- [trackv-post-grounding-soak-1.md](trackv-post-grounding-soak-1.md) - VISUAL-CAPTURE-MATRIX-SOAK-1: Track V Post + Grounding MVP
- [tree-override-lod-spec.md](tree-override-lod-spec.md) - TREE-OVERRIDE-LOD-SPEC-1 - Design spec
- [v-material-pbr-2-plan.md](v-material-pbr-2-plan.md) - V-MATERIAL-PBR-2 - Cheap Specular for StaticPropOpaque (PLAN)
- [vfx-3d-mesh-substrate-recon.md](vfx-3d-mesh-substrate-recon.md) - VFX 3D Mesh GPU Substrate Recon
- [vfx-originals-restoration-design.md](vfx-originals-restoration-design.md) - VFX Originals Restoration - Design (CPU-oracle - GPU-sim arc)

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
- [asan-follow-ups.md](asan-follow-ups.md) - ASan MVP - Follow-up Findings
- [asan-triage.md](asan-triage.md) - ASan Findings Triage - 2026-04-24
- [asset-viewer-backend-a-shader-contract.md](asset-viewer-backend-a-shader-contract.md) - Backend-A Shader Contract - static_prop.{vert,frag}
- [asset-viewer-backend-a-uvv-check.md](asset-viewer-backend-a-uvv-check.md) - Backend-A UV-V Verification Check (Task 8)
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
