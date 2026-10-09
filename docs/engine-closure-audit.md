# Engine Closure Audit — pre-Track-V receipt

**Branch tip:** `828432b6` (`feat(rendercore): descriptive RenderPassContract registry`)
**Date:** 2026-05-27
**Closure condition:** *every active render path is view-owned, pass-described, inspectable, validated, and has a rollback/debug path.*

This document is a fact-checked closure receipt — it answers "is the engine
ready for Track V (visuals)?" with cited evidence drawn from the in-tree
descriptive registries (`RenderCore/RendererFeatureRegistry.h`,
`RenderCore/RenderPassContract.h`), the snapshot header
(`GameOS/gameos/render_snapshot.h`), the inspector
(`GuiRuntime/EditorInspector.cpp`), the soak docs, and the smoke runner.

Doc-only. No code edits. No speculation about future plans.

---

## 1. Executive summary

The five-lane pass registry is now the structured source of truth for closure
status (`RenderCore/RenderPassContract.h:70-126`, landed `828432b6`). The
registry encodes shipped state on three axes: `viewUniformsBound`,
`pipelineDescRegistered`, `snapshotRowAuthoritative`. The other two closure
axes — `inspectorVisible` and `rollbackPath` — are recorded below by direct
inspection of `EditorInspector.cpp` and the kill-switch column of the same
registry.

| # | Lane | viewUni | pipelineDesc | snapAuth | inspector | rollback |
|---|---|---|---|---|---|---|
| 1 | StaticPropOpaque | green | green | green | green | green (`MC2_SNAPSHOT_STATIC_PROP_BUILD=0`) |
| 2 | Terrain | red | red | yellow (passive recorder) | green | none |
| 3 | MechOpaque | red | red | green (extraction default-off) | green | green (`MC2_SNAPSHOT_MECH_EXTRACT=0`) |
| 4 | Shadow | red | red | red | green (read-only) | none beyond `MC2_SHADOW_ENABLE` master |
| 5 | VFX | red | red | red (object-ID prohibited) | green (read-only) | `MC2_GPU_PARTICLES` master (default OFF; `=1` enables GPU) |

Source: `RenderCore/RenderPassContract.h:71-125` (axes 1-3 and kill-switch);
`GuiRuntime/EditorInspector.cpp:771,804,835,898` (inspector visibility).

**Track-V verdict:** **YELLOW_BUT_READY.** StaticProp is fully closed-out and
proven; the four remaining lanes are all *inspectable* and *contracted* even
where migration is incomplete, so visual work has a stable read-out of state.
The yellow/red cells are tracked-and-known, not unknowns. See §9.

---

## 2. Validators

| Validator | What it gates | Invocation | Status |
|---|---|---|---|
| `scripts/check-env-registry.sh` | Every `MC2_*` env literal in source either appears in `kFeatureTable`/`kAuxEnvVars` or is allow-listed (`RenderCore/RendererFeatureRegistry.h:10-13`). | `sh scripts/check-env-registry.sh` | Active. |
| `tools/shader_reflect/reflect.py` | 71 SPIR-V reflection JSON goldens diff against on-disk shaders. Goldens last refreshed in `80f7c92d` (SHADER-REFLECT-HYGIENE-1) and earlier in `30886561` (post F1-3B ViewUniformsBlock). | `py -3 tools/shader_reflect/reflect.py` | Active. |
| `scripts/run_smoke.py` tier1 | Primary regression gate. 5 missions (mc2_01/03/10/17/24). CLAUDE.md mandates `--keep-logs`, no `--with-menu-canary`, `--duration ≤ 30s`. | `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs` | Active; last green at HEAD `22321bf4` per `docs/static-prop-v3-soak-state.md:131-141`. |
| `RenderSnapshot::ok` aggregator | Per-frame ok flag combining ~13 mismatch / fail counters; explicitly enumerated at `GameOS/gameos/render_snapshot.h:195-204`. v4 adds 5 mech mismatch counters under `MC2_SNAPSHOT_MECH_EXTRACT`. | Read out per frame in log gate `[RENDER_SNAPSHOT ...]`. | Active. |
| `scripts/check-material-gpu-mirror.sh` | MaterialGpu C++ ↔ GLSL mirror; runs as pre-commit when touching `RenderCore/MaterialGpu.h` / mirrors. | `sh scripts/check-material-gpu-mirror.sh` | Active. |
| `scripts/check-shader-schema.sh` + `tools/shader_schema` | SHADER-SCHEMA-1/2: SSBO/UBO layout (offset/stride) contract. Includes `ViewUniforms` ABI as of SHADER-SCHEMA-2 (`136499ba`). | `cmake --build build64 --target shader_schema` (per `docs/tier1_env_vars.md:56-65`) | Active. |
| `scripts/check-visibility-log-schema.sh` | Visibility log line schema check. | `sh scripts/check-visibility-log-schema.sh` | Active. |
| `scripts/check-vfx-no-objectid.sh` | Locks the VFX-object-ID prohibition (M4) (`docs/tier1_env_vars.md:73`). | `sh scripts/check-vfx-no-objectid.sh` | Active. |
| `scripts/check-no-raw-gl-from-game.sh` | Game-side raw-GL prohibition (M6). | `sh scripts/check-no-raw-gl-from-game.sh` | Active. |
| `scripts/check-include-firewall.sh` | SCOPE_DIRS layering between RenderCore / GameOS / mclib / code. | `sh scripts/check-include-firewall.sh` | Active. |
| `scripts/check-render-contract-gbuffer1.sh` | Render-contract GBuffer attachment-1 invariant. | `sh scripts/check-render-contract-gbuffer1.sh` | Active. |
| `scripts/check-particles-no-cpu-projection.sh` | Particles must not do CPU-side projection (the world-vs-clip-space trap, see memory `gpu-particle-age-zero-curve-trap`). | `sh scripts/check-particles-no-cpu-projection.sh` | Active. |
| `scripts/check-destroy-invariant.sh` | Object lifecycle destroy invariant. | `sh scripts/check-destroy-invariant.sh` | Active. |
| `scripts/check-mlr-leaves-gated.sh` | MLR-leaf gating invariant from the unified-projection arc. | `sh scripts/check-mlr-leaves-gated.sh` | Active (currently dormant scope — no MLR leaves expected). |
| `scripts/check-unified-projection-retirement.sh` | Tracks unified-projection retirement allowlist. | `sh scripts/check-unified-projection-retirement.sh` | Active. |
| `scripts/check-asset-scale-callers.sh` | UI icon atlas / `code/mechicon.cpp` callers (`docs/tier1_env_vars.md:55`). | `sh scripts/check-asset-scale-callers.sh` | Active. |
| `scripts/check-claude-md-pointer.sh` | Root `CLAUDE.md` is a thin pointer (sentinels + line cap). | `sh scripts/check-claude-md-pointer.sh` | Active (last enforced in `d5b5b4eb`). |

All validators are file-present on disk; none have been observed to be dead
code in the recent commit window. The static-prop-v3 soak report cites the
runner verdict directly (`docs/static-prop-v3-soak-state.md:141`).

---

## 3. Debug panels & inspectors

All ImGui content lives in `GuiRuntime/EditorInspector.cpp`. Inspection is
behind `MC2_IMGUI=1 MC2_IMGUI_INSPECTOR=1`
(`RenderCore/RendererFeatureRegistry.h:178-189`).

| Panel | Section | File:line | Notes |
|---|---|---|---|
| Renderer Features | window | `GuiRuntime/EditorInspector.cpp:153` | Lists every `kFeatureTable` entry with live env state. |
| Object Inspector — window root | window | `GuiRuntime/EditorInspector.cpp:236` | Master inspector window. |
| Object-ID | header | `GuiRuntime/EditorInspector.cpp:247` | Selection identity. |
| Render Explain | header | `GuiRuntime/EditorInspector.cpp:361` | Per-selection rendering attribution (Mech / StaticProp). |
| StaticProp (selection) | header | `GuiRuntime/EditorInspector.cpp:472` | StaticProp identity, transform, packets. |
| Render Spine | header | `GuiRuntime/EditorInspector.cpp:483` | DrawPacket / dispatch state for selected static prop. |
| Mech | header | `GuiRuntime/EditorInspector.cpp:628` | Mech identity panel (extended in `a6a76a41` MECH-SPINE-1). |
| Mech Snapshot | header | `GuiRuntime/EditorInspector.cpp:672` | MECH-EXTRACTION-2 mech-extraction panel; gate `MC2_SNAPSHOT_MECH_EXTRACT`. |
| Terrain (selection) | header | `GuiRuntime/EditorInspector.cpp:744` | Terrain-tile selection panel. Crosshair highlight uses `MC2_DEBUG_RENDERER`. |
| **Terrain Pass** | header `##tp` | `GuiRuntime/EditorInspector.cpp:771` | TERRAIN-SPINE-0 (`b1a58628`): view id, ViewUniforms bound flag, program ids, last-flush stats, tessellation status. |
| **Shadow Pass** | header `##sp` | `GuiRuntime/EditorInspector.cpp:804` | SHADOW-SPINE-0 (`fdb274e5`): shadows enabled, static-light matrix, map sizes, ViewUniforms (currently **NOT** consumed — `EditorInspector.cpp:810-816`), three lane programs, caster counts. |
| **VFX Pass** | header `##vfx` | `GuiRuntime/EditorInspector.cpp:835` | VFX-SPINE-0 (`09355aa2`): GPU particle gate, init failure, camera basis, ViewUniforms (legacy gosFX path — `EditorInspector.cpp:853-858`), SSBO capacity, lifetime counters. Per-kind draw counts wired only as `n/a` — see TODO at `EditorInspector.cpp:884-890`. |
| **Render Pass Contracts** | header `##rpc` | `GuiRuntime/EditorInspector.cpp:898-921` | RENDERPASS-CONTRACT-2.5 (`828432b6`): 7-column table driven directly from `kRenderPassContracts`. |
| Material (RW handle lookup) | header | `GuiRuntime/EditorInspector.cpp:927` | MaterialGpu sidecar inspection (StaticProp only). |
| Env Gates | header | `GuiRuntime/EditorInspector.cpp:989` | Aggregate env-var state view. |

IMG-INSPECT terrain pick + highlight (HANDOFF 2026-05-24
`HANDOFF_2026_05_24_imgui_inspect_2_3_terrain_highlight.md`) is the source of
the highlight code path; the toggle is `MC2_DEBUG_RENDERER`
(`EditorInspector.cpp:760-765`).

---

## 4. Logs / runtime instrumentation

Log prefixes (all of form `[KEY vN]`) located by grep over `GameOS/gameos/*`:

| Prefix | Source file:line | Gate | Default |
|---|---|---|---|
| `[RENDERER_FEATURES v1]` | `RenderCore/RendererFeatureRegistry.h:309-342` | startup banner | always-on |
| `[RENDER_SNAPSHOT v3]` | `GameOS/gameos/gos_static_prop_batcher.cpp:4216`, `:4337`, `:4441` | `MC2_RENDER_SNAPSHOT_LOG` + v3 path | event-driven |
| `[DRAW_PACKET_V6]` | `GameOS/gameos/gos_static_prop_batcher.cpp:3998`, `:4003`, `:4107`, `:4129`, `:4148`, `:4399`, `:4416`, `:4431` | v6 path active (default per `RendererFeatureRegistry.h:230-237`) | on (default-on path) |
| `[PATCH_STREAM v1]` | `GameOS/gameos/gos_terrain_patch_stream.cpp:239,258,329,350,355,366,370,381,...` | always-on for init/overflow events | always-on |
| `[BUCKET_CENSUS v1]` | `GameOS/gameos/gos_terrain_patch_stream.cpp:150,317`; declared header `:248` | `MC2_BUCKET_CENSUS` | opt-in |
| `[THIN_DEBUG v1]` | `GameOS/gameos/gameos_graphics.cpp:5212` | per-event, no separate gate | event-driven |
| `[VIEW_UNIFORMS v1]` | upload event in `view_uniforms_gl.cpp` (default ON per `docs/tier1_env_vars.md:12`) | always-on under `MC2_VIEW_UNIFORMS!=0` | on by default |
| `[RENDER_WORLD v1]` | banner always-on; `MC2_RENDER_WORLD_TRACE=1` enables per-event lines (`RendererFeatureRegistry.h:290-298`) | trace gate | trace off by default |
| `[mech-extract]` (per-mission canary) | per `docs/tier1_env_vars.md:127-129` | `MC2_SNAPSHOT_MECH_EXTRACT=1` | off |
| `[REVERSE_Z v1]` | `MC2_REVERSE_Z_TRACE` (`docs/tier1_env_vars.md:27`) | trace gate | off |
| `[INSTR v1]` | startup enabled-probes enumeration (`docs/tier1_env_vars.md:8`) | always-on | on |
| `[HEARTBEAT]` | `MC2_HEARTBEAT=1` (`docs/tier1_env_vars.md:26`) | gate | off |
| `[RENDER_CONTRACT v2]` | `MC2_RENDER_CONTRACT_ASSERT=1` (`docs/tier1_env_vars.md:49`) | gate | off |
| `[SHADER_SCHEMA v1]` | `cmake ... --target shader_schema` runtime print (`docs/tier1_env_vars.md:60`) | build-time | on at gate run |

These prefixes are the read-out surface that the smoke runner and the
RENDER_SNAPSHOT counters tap.

---

## 5. Feature flags (env vars)

Direct enumeration of `kFeatureTable` (`RenderCore/RendererFeatureRegistry.h:125-278`).
"Last validation event" cites the most recent SHA in the commit log
(`git log --oneline -50`) where the flag's default or behavior shifted, or
where soak proof was logged.

| Flag | Env var | Default | Status | Kill-switch role | Last validation |
|---|---|---|---|---|---|
| GpuMechs | `MC2_GPU_MECHS` | on | [Retired] (`RendererFeatureRegistry.h:131`) | no | superseded by `RenderWorld::registerMech` |
| StaticPropIndirect | `MC2_GPU_OBJECTS` | off | [Retired] (`:139`) | no | superseded by `RenderWorld::upsertStaticProp` |
| ObjectIdBuffer | `MC2_OBJECT_ID_BUFFER` | off | shipped opt-in (`:146-148`) | yes (required for pick consumers) | M1.5 substrate |
| TerrainTessellation | (none) | always-on (`:152-156`) | shipped default-on, no env | no | always-on |
| ReverseZ | (none) | always-on (`:160-164`) | shipped always-on, no env | no | always-on (trace = `MC2_REVERSE_Z_TRACE`, default OFF) |
| ShadowMaps | `MC2_SHADOW_ENABLE` | on (`:170-172`) | shipped default-on (`=0` kills) | yes (master) | last referenced in shadow inspector `EditorInspector.cpp:804-830` |
| ImGui | `MC2_IMGUI` | on (`GuiRuntime/GuiRuntime.cpp:88`) | default ON | n/a | editor uses |
| ImGuiInspector | `MC2_IMGUI_INSPECTOR` | off (`:185-188`) | shipped opt-in | n/a | editor / debug |
| DebugRenderer | `MC2_DEBUG_RENDERER` | off (`:193-196`) | shipped opt-in | n/a | terrain-pick highlight |
| MaterialGpu | `MC2_MATERIAL_GPU` | **on** (`:202-204`) | shipped default-on (v5 flip 2026-05-26) | yes (`=0`) | MaterialGpu Arc Checkpoint HEAD `94b3ffa5` (memory pointer) |
| MaterialGpuSample | `MC2_MATERIAL_GPU_SAMPLE` | **on** (`:209-212`) | shipped default-on (v7 flip 2026-05-26) | yes (`=0`) | `HANDOFF_2026_05_26_material_gpu_static_prop_complete.md` |
| StaticPropRegistry | `MC2_STATIC_PROP_REGISTRY` | on (`:216-220`) | shipped default-on; editor sets `=0` | yes (=0 bypass for edit-time) | EditorMFC.cpp consumer |
| MaterialKtx | `MC2_MATERIAL_KTX` | off (`:225-228`) | shipped opt-in (Phase 0, RGBA8 only) | n/a | `6a9ecdcc` lands sources |
| StaticPropPacketDispatch | `MC2_STATIC_PROP_LEGACY_DISPATCH` | **on** (i.e. packet path default-on; env=1 forces legacy) (`:232-236`) | shipped default-on (v7) | **yes** (`=1` reverts to legacy MDI) | v7.1 — `MC2_DRAW_PACKET_STATIC_PROP_V6` retirement |
| RenderSnapshotBuild | `MC2_SNAPSHOT_STATIC_PROP_BUILD` | **on** (`:247-252`) | shipped default-on (STATIC-PROP-V3-FLIP `2a88a5a8`, 2026-05-27) | **yes** (`=0` reverts to live builder) | `2a88a5a8` post-flip + `10149e11` validate doc |
| SnapCull | `MC2_SNAP_CULL` | off (`:256-260`) | shipped opt-in | no (collision-incompatible with snapshot build — `RenderSnapshot v3` log) | `066b5b9d` collision-soak |
| ViewUniforms | `MC2_VIEW_UNIFORMS` | **on** (`:264-268`) | shipped default-on (F1-3D `cf5f67bc`, 2026-05-27) | yes (`=0` → legacy `u_worldToClipGL`) | `cf5f67bc` flip + `3a993be6` byte-identical compare |
| SnapshotMechExtract | `MC2_SNAPSHOT_MECH_EXTRACT` | off (`:272-276`) | shipped opt-in (observe-only) | no | MECH-EXTRACTION-4 `a5f04d86` |

Aux env vars (`kAuxEnvVars`, `:290-298`):

| Flag | Env var | Default | Notes |
|---|---|---|---|
| TraceRenderWorld | `MC2_RENDER_WORLD_TRACE` | off | per-frame banner + per-event log (`RenderWorld.cpp`) |

---

## 6. Deferred decisions (not done — by design)

| Item | Why deferred | Block source |
|---|---|---|
| Terrain object-ID | No real consumer yet; per `RenderPassContract.h:91` terrain has no kill-switch and `snapshotRowAuthoritative=false`. | brief §6; awaits a consumer slice |
| Terrain per-tile identity | TerrainPassFacts is pass-level only — explicitly excludes per-tile identity (`render_snapshot.h:122-141`). | scope choice in TERRAIN-PASS-PACKET-0 (`1d7b9ea6`) |
| VFX object-ID | **PROHIBITED**, not just deferred. Locked by `scripts/check-vfx-no-objectid.sh` (M4 invariant). `RenderPassContract.h:124` notes "Object-ID PROHIBITED." | M4 invariant |
| Mech MaterialGpu shader sampling | Mech texture model not decided. Mech rows have materialIdx (`render_snapshot.h:31`) but `MechOpaque` lane shows `pipelineDescRegistered=false` (`RenderPassContract.h:96-102`). | brief §6; awaits texture-model decision |
| Shadow ViewUniforms migration | Inspector explicitly reads "ViewUniforms NOT consumed (legacy shadow matrices)" (`EditorInspector.cpp:813-815`). `RenderPassContract.h:108` `viewUniformsBound=false`. | brief §6; shader-convergence campaign |
| Shadow on PipelineDesc | `RenderPassContract.h:109` `pipelineDescRegistered=false`. Inspector notes `PipelineDesc: legacy (shadow pass not on registry)` at `EditorInspector.cpp:829`. | brief §6 |
| Full render-graph execution | `RenderPassContract.h:13-15` explicitly: "NOT a scheduler. NOT a render graph. NO execute() callbacks." | design choice — descriptive only |
| Full `RenderWorld::render()` centralization | RenderWorld arc status `M2` shipped (`HANDOFF_2026_05_23_evening_renderworld_arc_M2_ready.md`); centralization beyond mech adapter not in scope yet. | arc status doc |
| `shaders/gos_terrain.frag` matNormalBoost/tintStrengthScale uniforms | Uncommitted in worktree (`git status --short` shows ` M shaders/gos_terrain.frag`). | awaits C++ ImGui tuning consumer (brief §6) |
| `shaders/hdri_skybox.frag` v-flip experiment | Uncommitted in worktree (` M shaders/hdri_skybox.frag`). | paused experiment (brief §6) |
| `test_include.cpp` | Pre-existing untracked file (`?? test_include.cpp`). | pre-existing dirty — deferred per user |

---

## 7. Rollback / kill-switch matrix

Only features with explicit `=0`-style reverts (or equivalent) listed. Status
crosses `RenderPassContract.h` `killSwitchEnv` and `RendererFeatureRegistry.h`
descriptor docs.

| Feature | Kill-switch | Expected on-kill behavior | Last verified |
|---|---|---|---|
| Static-prop snapshot build | `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` | Live builder path dispatches; `spBuild*` counters zero (`render_snapshot.h:233-238`). | `2a88a5a8` post-flip on mc2_10 (HANDOFF_2026_05_27_static_prop_v3_flip_shipped pointer in MEMORY.md). |
| Static-prop packet dispatch | `MC2_STATIC_PROP_LEGACY_DISPATCH=1` | Reverts to `glMultiDrawElementsIndirect` (`RendererFeatureRegistry.h:236`). No `[DRAW_PACKET_V6]` lines. | v7.1 retire-doc (`docs/tier1_env_vars.md:121`) |
| Mech snapshot extract | `MC2_SNAPSHOT_MECH_EXTRACT=0` (default) | No extraction; mech mismatch counters trivially zero (`render_snapshot.h:195-198`). | MECH-EXTRACTION-4 `a5f04d86` — default-off behavior preserved by test design |
| ViewUniforms | `MC2_VIEW_UNIFORMS=0` | Reverts to legacy `uniform mat4 u_worldToClipGL` (`docs/tier1_env_vars.md:12`). Requires process restart (shaders compile at startup). | `cf5f67bc` F1-3D flip + `3a993be6` byte-identical compare |
| MaterialGpu | `MC2_MATERIAL_GPU=0` | Skips upload/bind/compare; sampleOn becomes ineffective (`RendererFeatureRegistry.h:204`). | MaterialGpu Arc Checkpoint memory entry |
| MaterialGpu shader sample | `MC2_MATERIAL_GPU_SAMPLE=0` | Frag falls back to `texArrayLayer` (`RendererFeatureRegistry.h:209-212`). | v7 flip |
| StaticProp registry | `MC2_STATIC_PROP_REGISTRY=0` | Bypasses registry (editor edit-time use, EditorMFC.cpp; `RendererFeatureRegistry.h:217-220`). | editor consumer present |
| Shadow master | `MC2_SHADOW_ENABLE=0` (kill-switch; default ON) | No shadow pre-pass or PCF sampling (`RendererFeatureRegistry.h:170`). | on by default; inspector reports state |
| GPU particles master | `MC2_GPU_PARTICLES` unset/`0` (default OFF) | Legacy CPU FX only (`EditorInspector.cpp:842-843`). | inspector live read-out |

The four lanes other than StaticProp lack a *lane-scoped* kill-switch in the
RenderPassContract (`killSwitchEnv = nullptr`), reflecting that those lanes
have not yet been re-architected away from their legacy paths and therefore
have nothing to fall back *from*.

---

## 8. Soak / validation evidence

Major flips and their pre/post evidence:

### F1-3D ViewUniforms default-on (`cf5f67bc`)
- Pre-flip gated soak: `19a58425` (F1-3B static_prop.vert consumer) — draws_issued>0 all 5 missions per `HANDOFF_2026_05_27_f1_3b_static_prop_consumer_shipped.md` (MEMORY pointer).
- Diff probe: `3a993be6` (F1-3C) — `max_diff=0.000000 ok=1` tier1 5/5 (`HANDOFF_2026_05_27_f1_3c_matrix_probe_shipped.md` pointer).
- Default-on flip: `cf5f67bc` — `binding=3`, kill-switch `MC2_VIEW_UNIFORMS=0` (`HANDOFF_2026_05_27_f1_phase3_complete.md` pointer).
- Tier1 missions: mc2_01, mc2_03, mc2_10, mc2_17, mc2_24.

### STATIC-PROP-V3-FLIP default-on (`2a88a5a8`)
- Pre-flip gated soak: `bb5356db` (gate-ON, ok=1, fallback=0 5/5; `docs/static-prop-v3-soak-state.md:17`).
- Default proof: `8ef2e8d2` (gate-OFF, spBuild*=0 5/5; `docs/static-prop-v3-soak-state.md:16`).
- Collision soak: `066b5b9d` (`MC2_SNAP_CULL` + build both =1; collision guard fires; `docs/static-prop-v3-soak-state.md:18`).
- Post-flip validate: `10149e11` doc (HEAD `22321bf4` flip-validate, 14,963 frames total; `docs/static-prop-v3-soak-state.md:124-141`).
- Default-on: `2a88a5a8` (HEAD `828432b6` git-log; `docs/static-prop-v3-soak-state.md:166-172`).
- Kill-switch: `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` (`RendererFeatureRegistry.h:249-252`).

### SHADER-REFLECT-HYGIENE-1 (`80f7c92d`)
- 71 SPIR-V reflection JSON goldens refreshed; commit-log title "refresh shader_reflect goldens".
- No flip — hygiene/maintenance.

### MECH-EXTRACTION-0..-4
- MECH-EXTRACTION-0 substrate: `537d768a` (compare-only) + `eea04637` (persist-buffer fix giving snapshot=6 on mc2_24).
- MECH-EXTRACTION-1: `c0aa13fe` (materialIdx wiring + sentinel counters).
- MECH-EXTRACTION-2: `f1ee54a4` (inspector panel).
- MECH-EXTRACTION-3: `a0f26a1f` (tier1 forced-ON canary). Per-mission results (`docs/tier1_env_vars.md:128`):
  - mc2_01 snapshot=3 mat_valid=3
  - mc2_03 snapshot=1 mat_valid=1
  - mc2_10 snapshot=1 mat_valid=1
  - mc2_17 snapshot=12 mat_valid=12
  - mc2_24 snapshot=6 mat_valid=6
  - All mismatches=0, mat_sentinel=0.
- MECH-EXTRACTION-4: `a5f04d86` (5 mech counters promoted to ok gate, v4).
- Default remains **off** (`MC2_SNAPSHOT_MECH_EXTRACT=0`); no default-on flip yet.

---

## 9. Track V readiness scorecard

Closure axes for each lane. "Yellow" = contracted, partial, but inspectable
and known. "Red" = not yet on the migration path.

| Lane | view-owned | pass-described | inspectable | validated (snapshot row authority) | rollback path |
|---|---|---|---|---|---|
| StaticPropOpaque | green (`RenderPassContract.h:75`) | green (`:71-81`) | green (`EditorInspector.cpp:472,483`) | green (`render_snapshot.h:166-204`, ok gate) | green (`MC2_SNAPSHOT_STATIC_PROP_BUILD=0`, `MC2_STATIC_PROP_LEGACY_DISPATCH=1`) |
| Terrain | red (`RenderPassContract.h:86`) | green-desc, yellow-impl (`:83-91`) | green (`EditorInspector.cpp:771`) | yellow — passive recorder; explicitly *not in ok gate* (`render_snapshot.h:127`, `:240-243`) | none (no kill-switch — but tessellation always-on, no flag-gated revert) |
| MechOpaque | red (`RenderPassContract.h:96`) | green-desc, yellow-impl (`:93-103`) | green (`EditorInspector.cpp:628,672`) | green when gated (`render_snapshot.h:151-164`, mech mismatches in ok-v4) | green (`MC2_SNAPSHOT_MECH_EXTRACT=0` is default-off) |
| Shadow | red (`RenderPassContract.h:108`) | yellow-desc (`:104-113`); inspector states `PipelineDesc: legacy` (`EditorInspector.cpp:829`) | green (`EditorInspector.cpp:804`) | red (no snapshot rows for shadow casters) | partial — `MC2_SHADOW_ENABLE=0` master only |
| VFX | red (`RenderPassContract.h:120`) | yellow-desc (`:115-124`); object-ID prohibited | green (`EditorInspector.cpp:835`) | red (no VFX rows; per-kind counts `n/a`, `EditorInspector.cpp:884-890`) | partial — `MC2_GPU_PARTICLES` master only (default OFF) |

**Closure condition recap:** *every active render path is view-owned,
pass-described, inspectable, validated, and has a rollback/debug path.*

- StaticPropOpaque: **fully closed**, all five axes green.
- Other four lanes: **inspectable + pass-described** (the closure axes that
  the engine convergence campaign explicitly targeted as prerequisites for
  Track V). View-owned / snapshot-authoritative / per-lane rollback remain
  the legitimate work that *follows* Track V.

---

## 10. Known issues / risks moving into Track V

- **Deferred items from §6** that Track V will likely touch first:
  - Terrain ViewUniforms migration (inspector shows "NOT consumed (legacy uniforms)"
    when terrain pass viewUniformsBound is false — `EditorInspector.cpp:779-781`).
  - Shadow ViewUniforms migration (same pattern, `:813-816`).
  - VFX per-kind draw counters (`EditorInspector.cpp:884-890` is `n/a` placeholders).
- **Self-review-only protocol shortcut** on inspector slices `a6a76a41`
  (MECH-SPINE-1), `b1a58628` (TERRAIN-SPINE-0), `fdb274e5` (SHADOW-SPINE-0),
  `09355aa2` (VFX-SPINE-0) — landed without external review. Mitigated by
  read-only-diff scope (no GL state changes, no buffer rebinds) and by tier1
  green; called out as process risk per brief §10.
- **Memory leak that caused a reboot during SHADOW-SPINE-0** — host-side
  issue, not engine; flagged for awareness per brief.
- **3 pre-existing dirty files in worktree**:
  - ` M shaders/gos_terrain.frag` (uncommitted matNormalBoost / tintStrengthScale)
  - ` M shaders/hdri_skybox.frag` (paused v-flip experiment)
  - `?? test_include.cpp` (pre-existing untracked)
  These are deferred per user; not blockers for Track V.
- **Existing player-visible issues** in `docs/known_issues.md` that will
  resurface under visual work: shadow stutter, shadow banding, water
  shoreline z-fight, first-launch black terrain intermittency, options.cfg
  resolution drift on 4K, bloom/FXAA/tonemap applied to HUD.
- **Visual identity probe** for STATIC-PROP-V3-FLIP was downgraded to
  *advisory* (`docs/static-prop-v3-soak-state.md:164-172`); no smoke-time
  identity probe exists. Manual editor verify is the current backstop.

---

## 11. Recommended next steps

Track V can safely start. Concretely:

- **Safe to begin now:** any visual change isolated to the StaticPropOpaque
  lane (material lookups, PBR sampling, normal-map handling, etc.) —
  rollback is robust (`MC2_SNAPSHOT_STATIC_PROP_BUILD=0` or
  `MC2_STATIC_PROP_LEGACY_DISPATCH=1`) and snapshot-row authority gives
  per-frame mismatch counters in the ok gate.
- **Land the uncommitted shader work** (`shaders/gos_terrain.frag`,
  `shaders/hdri_skybox.frag`) under a normal slice once a C++ ImGui tuning
  consumer is wired — they should not stay loose in the worktree
  indefinitely.
- **Wait before touching:** terrain/shadow/VFX shader-side migration to
  ViewUniforms. The current closure receipt explicitly marks these as
  legacy-uniform-bound (inspector text); they form a natural "shader
  convergence" campaign that should land after Track V's visual baseline so
  that visual regressions are easier to bisect.
- **Maintenance items to schedule:**
  - Per-kind VFX draw counters (`EditorInspector.cpp:884-890` TODO).
  - Terrain `visibleBlockCount` counter (intentionally omitted in
    `render_snapshot.h:138-139`; promote when a consumer is wired).
  - Shadow ViewUniforms migration (single-campaign — terrain + mech + static
    prop shadow programs together).
- **Closure audit upkeep:** when any pass-lane flips a closure axis, update
  the matching `kRenderPassContracts` entry *in the same slice* — the
  static_assert at `RenderPassContract.h:131-133` and the imgui table at
  `EditorInspector.cpp:898-921` make drift visible immediately.

---

*End of audit. Branch tip `828432b6`. No commit made by this slice.*
