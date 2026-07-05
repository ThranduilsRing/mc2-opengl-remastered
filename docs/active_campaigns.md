# Active campaigns

> Pointer doc. Detail in memory/ handoffs and docs/. Add new campaigns at top.

## 2026-06-27 — DEPTH/Z-FIGHT/FOG FOLLOWUPS — SHIPPED + MERGED nifty (`e0cd4ff5`)

Three render bugs from `.claude/HANDOFF-depth-precision-fog-followups.md`, all user-confirmed
fixed and merged (built on a fork `claude/depth-fog-followups-1` off `c982817e`, merged clean).
Each took the long way — most effort spent discarding the wrong axis (depth precision, geometry,
fog, skybox); the unlock each time was a precise user symptom detail.

| Fix | Commit | Gate (default ON, =0 reverts) | Root cause |
|---|---|---|---|
| **Roads vanish on slopes** | `ea3cc267` | `MC2_TERRAIN_LOD_CHECKER_DIAG` | LOD-chunk render IBO was the lone fixed-TL-BR triangle diagonal vs the per-cell CHECKERBOARD `worldQuadUVMode` used by grounding/overlay/water/shadow/GPU-compute → on ~50% of cells the two triangulations cross on warped quads, terrain occludes the road center. Render now honors the checkerboard. |
| **Building/object base z-fight on ZOOM** | `7f43ee37` | `MC2_PROP_FIXB_MVP` + `MC2_MVP_PUBLISH_EARLY` | Motion 1-frame whole-scene MVP lag: terrain compute snapshot (`g_dispatchMvp16`) was taken in UPDATE while the camera MVP was published in RENDER → terrain co-trailed by a frame; objects used live MVP → diverged during zoom. Fix-B = objects consume the same snapshot; early-publish = snapshot captures the current frame (kills the lag at source). NOT depth precision (D32F null) / NOT geometry / NOT bias. |
| **OOB blue/black screen strobe** | `6773853b` | `MC2_OOB_LETTERBOX` | When the backbuffer > scene FBO (smoke-minimized start; menus where window > render res), the composite quad covered only the FBO region; the rest of the backbuffer was never cleared → alternating swapchain content (old HDRI-sky-blue vs black) strobed. Fix = clear the backbuffer black before the composite blit. NOT the fog (de-animating u_time was a no-op), NOT the legacy skybox (off), NOT the HDRI toggling (it renders every gameplay frame). |

Prior-session parked render WIP preserved in `aa97244d` (gated default-OFF): TERRAIN-DETAIL-ANTI-TILE-1,
OBJECT-DECAL-MATRIX-SHARE-1 + OBJECT-TERRAIN-COPLANAR-OFFSET-1 (superseded by the above), cloud-shadow UV.
Editor WIP left untouched. Memory: [[building-zfight-is-grounding-not-depth]], [[live-terrain-path-is-lod-chunk]],
[[oob-strobe-is-uncleared-backbuffer]]. OPEN/NEXT: world renders small in a larger window (HUD-RES-CLAMP
sets projection aspect to 800×600); making the world FILL the window is the shelved hor+ widescreen arc.

## 2026-06-23 — DECAL-INTEGRATE-1 (ring-fed projected decals) — BUILT (`claude/decal-integrate-1`)

Joins the two parked projected-decal branches: wires the runtime impact-decal RING
producer (`decal-terrain-impact-mvp` @ `585d29c9` cherry-picked + 2 hunks reapplied) to
the screen-space box-decal PROJECTION consumer (`box-decal-1` @ `86e634c0` cherry-picked
clean). Box-decal now reads `dynamic_decal_ring` live slots (one projected decal per
impact) instead of the v1 hardcoded screen-center test box. Parallel to baked craters,
not canonical. Detail: `docs/DECAL-INTEGRATE-1.md`.

```
DECAL_INTEGRATE:
  status: BUILT
  gate_default: OFF (MC2_PROJECTED_DECALS)
  gate_off: BYTE_IDENTICAL
  ownership: PARALLEL_TO_BAKED (not canonical)
  producer: dynamic_decal_ring
  consumer: box_decal projection (now ring-fed, not hardcoded)
  normal_reject: DISABLED_DEFAULT (up-axis frame TODO)
  surface_class_masking: DEFERRED
  visual_status: see docs/DECAL-INTEGRATE-1.md (non-deterministic / fixture-driven)
```

## 2026-06-22 — MECH-UBLB-ATTACHMENT-FIX-1 (BT2018 clip pose) — SHIPPED nifty (`570cfab4`)

| Slice | Commit | Status | Notes |
|---|---|---|---|
| **PER-ACTOR-1** | `627a61d9` | SHIPPED | Imported GPU mechs animate INDEPENDENTLY (no lockstep). Per-frame anim state (1C clip select + time + model-delta palette + foot lift) moved from the shared per-type entry into a per-actor `ActorAnimState` keyed by the actor instance (`mechShape*`); type entry keeps static data only. `TickImportedMechs`/`ImportedGpuModelDelta`/`ImportedGpuLift` now actor-keyed; `submitActor` passes `desc.mechShape`; updateGeometry passes `mechShape`+`mechType->mechShape[currentLOD]`. Stale-actor prune. CPU fallback stays lockstep (shared verts). Single-mech idle/walk unchanged; 2-mech independence structural. |
| **ANIM-COVERAGE-1** | `21738968` | SHIPPED | combat-gesture → BT clip map (hit/fall/knockdown/limp) — see memory. |
| **1B-GPU-PALETTE-PLACEMENT-1** | `3a6990d6` | SHIPPED | Imported mech now ANIMATES on the default GPU mech path (no MC2_GPU_MECHS=0) — upright, textured, feet grounded, walk animates (170k px). Per-vertex BT bone index + `rec.numBones=28` override + submitActor composes `boneT_i = shapeToWorld_root · model_delta_i` (placement from `_HierarchyOnly`). model_delta `D_i=(A1·S·C_i)·(A2·S·R_i)^-1` (A2=VBO Y-up, A1=`MC2_MECH_IMPORT_GPU_AXIS` default 2). Auto foot-lift = -min(VBO vtx Y), world-up=Stuff.y. NO shader/ABI/VBO-rebake. Gate `MC2_MECH_IMPORT_GPU=1` default OFF; tier1 PASS gate-off, stock byte-unchanged. Recon trail: `docs/bt2018-skel-gpu-draw-engagement-recon-1.md`. |
| **1B-GPU skinning RECON-2** | `b8be31e5` | PROVEN → implemented in PLACEMENT-1 | Render+shader experts agree: imported merged mesh CAN ride the existing GPU skinned path as ONE draw, NO vertex-struct ABI change, NO shader edit (boneIndices/boneWeights lanes + unbounded flat bone SSBO `bones[idx+baseBoneOffset]` + weighted `u_skinningMode` default-ON, built for Track D; shadow_mech mirrors it). Sole obstacle: `rec.numBones=GetNumShapes()`=1 for merged mesh (recipe, not structural). Plan = Route B (per-vertex BT-bone side table + numBones=28 override + submit pushes 28 joint-globals). Doc `docs/bt2018-skel-1b-gpu-skinning-recon-2.md`. NEXT = implement `MC2_MECH_IMPORT_GPU`. |
| **1B-GPU (default GPU path) recon-1** | `c7c34f04` | SUPERSEDED by RECON-2 | GPU mech path draws from an immutable rest-pose VBO + per-node `shapeToWorld` SSBO → CPU re-bake shows a FROZEN pose there; merged single-node mech can't articulate on the rigid-per-node path. Recommended real fix = true GPU skinning via existing C2 boneIndices/boneWeights lanes + inject `EvaluateClipGpuBones` joint-globals into the bone SSBO (default-OFF `MC2_MECH_IMPORT_GPU`, dedicated slice w/ render expert). Shipped: recon/plan `docs/bt2018-skel-1b-gpu-recon.md` + one-shot frozen-pose warning. Until then use `MC2_GPU_MECHS=0`. |
| **1C gesture map** | `8ea12056` | SHIPPED | Imported mech selects its clip per frame from movement: gesture (debounced by the stock state machine) → idle/walk/run/reverse/jump, + leg-heading-rate → turn-in-place. Debounced switches, continuous clip time. `MC2_MECH_IMPORT_FORCE_CLIP` now PINS one clip (1B); without it, dynamic (1C). Verified in-engine (trace: gesture=Stand→idle plays+advances). CPU path only. Detail: memory `mech-import-1b-runtime-animation.md`. |
| **1B-runtime per-frame animation** | `8516666b` | SHIPPED | Imported mech now MOVES (was one static FORCE_CLIP pose). `mc2mechanim::TickImportedMechs` (assimp_importer.cpp) re-bakes the merged TG type geometry from a looping clip each frame (same `EvaluateClipGpuBones` math, clip global per frame), hooked in `mech3d.cpp` updateGeometry before TransformMultiShape. Gates `MC2_MECH_IMPORT_ANIMATE=1` (default OFF) + `MC2_GPU_MECHS=0` (REQUIRED — GPU batcher uploads type verts once) + `MC2_MECH_IMPORT_FORCE_CLIP`. Verified in-engine mc2_24 madcat walks (legs stride, torso seated). Deferred: actors lockstep (shared type), GPU path, per-actor, 1C gesture map. Detail: `docs/bt2018-skel-1b-runtime-scope.md` + memory `mech-import-1b-runtime-animation.md`. |
| **Rotation-only retarget** | `570cfab4` | SHIPPED | BT2018 mech is RIGID (every part `bones=1`, no blend). Clips carry TRANSLATION channels on spine chain (`j_Pitch`/`j_Spine`/`j_Spine1`) + pelvis; a ball-joint waist can't absorb translation → rigid UB lifts off LB socket (constant ~0.0154 every clip/frame). Fix = rotation-only retarget in shared `mclib/mech_skel_import.cpp` (`sampleChannel`/`computeGlobals`): channel ROTATION only, keep bind translation+scale. Gate `MC2_MECH_ANIM_ROTATION_ONLY` default ON (`=0` = raw, byte-identical pre-fix). Spine2→Pelvis gap 0.0275→0.0091; **verified in-engine** mc2_24 `madcat` torso seated. **Overturns the prior "EvaluateClipGpuBones mis-composes / idle0 must==rest" diagnosis — FK was always correct; idle frame0 ≠ bind is expected.** Detail: memory `mech-ublb-rotation-only-retarget.md`. NEXT: 1B-runtime (per-frame re-bake → moving mech) → 1C gesture map. |

## 2026-06-22 — FRAME-CURRENTNESS-GUARDS-1 — MERGED nifty (`0dd12b39`)

Targeted currentness sentinels + the mech double-step fix. Rejected a grand "frame-liveness framework" (greybeard: premature abstraction); shipped narrow guards at consumer boundaries instead. Built in worktree `claude/frame-currentness-guards-1` (off nifty `96378cc1`), merged via `0dd12b39`. Detail: memory `HANDOFF_2026_06_22_frame_currentness_guards` + `docs/frame-contracts/`.

| Slice | Commit | Status | Notes |
|-------|--------|--------|-------|
| **ANIM-CADENCE-FIX (mech double-step)** | `07d27b2f` `7603d042` | SHIPPED | root cause: `Mover::getLOSPosition` (mover.cpp:3528) 2nd `appearance->update()` ticked gait twice/frame (gestures 2/4/7). Idempotent gait advance (`lastAnimAdvanceFrame`). **User-confirmed visual fix** (mc2_17 Catapult/Bushwacker). Default-ON, killswitch `MC2_ANIM_CADENCE_FIX=0` |
| STATIC-REGISTRY-CURRENTNESS-GUARD-2 | `83d30112` | SHIPPED | generalizes R2b `stale_after_drawn` → per-typeID + `persistent_vanish` streak. A/B mc2_24 11→0. Gate `MC2_REGFLUSH_GUARD2` default-off |
| TARGETING-CURRENTNESS-GUARD-1 | `1c3e7a43` `6c079c7e` | SHIPPED | chokepoint `Mover::handleTacticalOrder`; logs every player attack order (stale_wid/behind/far). mc2_17 manual: 50 orders all clean (whole-map bug NOT reproduced). Gate `MC2_TARGETING_GUARD` default-off |
| Recon + review-rule | `be99dd51` `5d3606d3` | SHIPPED | `docs/frame-contracts/` (claim-audit, skip-safety, same-class-update — all clean); frame-currentness review-checklist line in `docs/critical_inline_rules.md` |

## 2026-06-22 — OpenGL correctness campaign + RENDER-BACKEND-SEAMS arc — SHIPPED nifty

Run as a ledger, not a cleanup binge: `docs/render-backend-seams/opengl-correctness-ledger-1.md` (status taxonomy, queue CLEAR, closure audit CONFIRMED_CLEAN). Dual-benefit (NVIDIA GL correctness + Vulkan-prep seams).

| Slice | Commit | Status | Notes |
|-------|--------|--------|-------|
| RENDER-PASS-CONTRACT-ENFORCEMENT-1 | `8d250041` | SHIPPED | non-fatal pass-scope tracker; gates `MC2_RENDER_PASS_CONTRACT_TRACE/ASSERT` (default-off) |
| WATER-THINRING-FENCE-1 | `bc424dc2` | SHIPPED | fenced the unfenced water thin-record ring (solid was fenced); gate `MC2_WATER_THINRING_TRACE` |
| UB2-02 mech.frag non-uniform sampling | `55f6cc71` | SHIPPED | hoist texture/derivative out of `flat v_mechSunFound` branch |
| UB2-01 terrain non-uniform sampling | `f4208726` | SHIPPED | textureGrad/textureLod; **byte-exact visual gate 9/9** (golden ub201-pre2) |
| **R2B tree-disappear (cachedFrame_)** | `07a1f8ac` | SHIPPED | reintroduced black-tree bug: R2b update-skip froze cachedFrame_ → registry dropped trees. Restored stamp + `stale_after_drawn` regression guard. A/B mc2_24 15336→0. Gate `MC2_R2B_TOUCH_PRESERVE` default-ON |
| OMT-1 overlay resolve-fail guard | `36d6a254` | SHIPPED | no GL texture-0 bind on resolve-fail → 1×1 magenta fallback + log; gate `MC2_OVERLAY_TEXTURE_TRACE` |
| UB2-05 building_pbr discard-before-sample | `1851b16d` | SHIPPED | sample/derivative hoisted above ALPHA_TEST discard |
| DEAD-POST-FX-CLEANUP-1 (bloom/ACES/FXAA + god rays) | `92d3a821` `9c2187d8` `3e1f9e0a` | SHIPPED | deleted as wrong-for-RTS; removed `MC2_HDR_POST/BLOOM/TONEMAP_ACES`, RAlt+F1/RAlt+6 hotkeys, 3 shader files; LIVE composite (sunset/exposure/view-modes) preserved |
| Docs: buffer-owner recon / GpuBuffer wrapper DESIGN / OMT-2-INDIRECT | `e5201fac` `68f98cb8` `b7406682` | SHIPPED | recon + design only (no wrapper code yet) |

**Remaining (vendor-run only):** VENDOR-CERT-PACK-1 — on-NVIDIA visual confirm of the tree fix; empty shadow frags (UB2-06/07); shadow PCF derivatives (UB2-04). Plus DEFERRED_LOW_RISK: veg-cards blockVis OOB (TREE-N2, veg shipped-off), static-decal stale-handle rebake. Detail: `memory/render-backend-seams-arc.md`.

---

## 2026-06-21 — Startup / load performance arc

| Slice | Commits | Status | Notes |
|-------|---------|--------|-------|
| SAVE-LOAD-FAST-1: skip redundant save-game scan on campaign start | `77f0478c` | **SHIPPED** | `beginLoad(bSkipSaveScan)` + dedup `isCorrectVersionSaveGame`; fixed memory leak on rejected entries |
| STARTUP-INIT-ASYNC-1: bg-thread LogisticsData::init + null guard | `4da59f21` | **SHIPPED** | `std::async` in `Logistics::start(log_SPLASH)`; blocking join before menu; `getCurrentABLScript()` null guard |
| STARTUP-PARALLEL-VARIANTS-1: parallel mech CSV reads | `8ede25b5` | **SHIPPED** | `std::async` per mech job inside `initVariants()`; serial scan + parallel read + serial merge |
| SMART-LOAD-1: defer mech-bay Phase B until first accessor | `bcc6359d` `27768580` | **SHIPPED** | Phase A (campaign.fit only) on bg thread; Phase B (components/pilots/variants + updateAvailability) lazy on first mech-bay call. Gate `MC2_SMART_LOAD=1` default OFF. Smoke mc2_01 PASS gate-OFF + gate-ON. |
| SMART-LOAD-DEFAULT-ON-1: flip `MC2_SMART_LOAD` default OFF->ON | (this branch) | **SHIPPED** | Kill-switch preserved: `MC2_SMART_LOAD=0` restores legacy eager `LogisticsData::init()`. Caveat: only affects the interactive splash->logistics->mech-bay flow (`Logistics::initializeLogData()`); smoke/tier1's `justStartMission` fast-path calls `Mission::init()` directly and never touches `LogisticsData`, so this flip does not move smoke `mission_ready_ms` -- the win is on the real player-facing load path. |

---

## Asset Modernization Pipeline — v0 SHIPPED (merged nifty `fdb7c470`, 2026-06-17)

| Slice | Status | Notes |
|-------|--------|-------|
| P1-A→D: ASE → GLB + sidecar roundtrip tooling | SHIPPED | `tools/ase_to_glb.py`, `tools/validate_glb.py`, `tools/asset_baseline.py`, `.mcasset.json` sidecar spec |
| Phase 2: texture cook (BC7 KTX2, AI-upscaled sources) | SHIPPED | `tools/mc2texcook/cook_pbr_maps.py`; `fireantrgb.ktx2` + `a_hangar.ktx2` deployed rc1 |
| Phase 3: mech PBR (StandardLit GGX) | ALREADY LIVE | Metal061B+PaintedMetal003 active in mech3d pipeline |
| P1-E: HangarGLB building GLB load | SHIPPED | Positive load confirmed mc2_01 via ASSIMP_TRACE; `data/tgl/HangarGLB.glb` + sidecar in rc1 |
| P1-F: material classification + CorrugatedSteel006A cook | SHIPPED | `tools/classify_materials.py`; BC7 KTX2 pack in `gameassets/materials/CorrugatedSteel006A/` |
| Runtime `MC2_ASSIMP_IMPORT=0` kill switch | SHIPPED | `mclib/msl.cpp LoadFromFile`; bdactor/genactor/gvactor all probe `[Import] Source=` |

**In flight / next:**

- **P1-G: building MaterialGpu SSBO wiring + `building.frag` PBR** — branch `claude/building-materialgpu-v1`
  - Status: NOT STARTED — foundation merged, new branch required
  - Kill switch: `MC2_BUILDING_PBR=0` (default OFF)
  - Blocked on: nothing — unblocked after v0 merge
  - First step: wire `CorrugatedSteel006A` BC7 KTX2 into MaterialGpu SSBO per building instance

---

## 2026-06-16 — NVIDIA hardening S2 — SHIPPED + merged to nifty (`97be4e5c`)

7 hardening items targeting correctness gaps AMD silently tolerates but NVIDIA exposes:

| Item | Result | Commit |
|------|--------|--------|
| 1. `gpuBindSsboRange` — 24 SSBO bind sites + alignment assert | DONE | `61b3684b` |
| 2. GPU buffer zero-init audit | CLEAN | `bb550ed4` |
| 3. `assertPassContract` — depth-func + blend aborts under `MC2_RENDER_CONTRACT_ASSERT=1` | DONE | `50ee010a` |
| 4. `[SHADER WARN]` on successful compile/link | DONE | `594eafe3` |
| 5. PBO unbind audit | CLEAN — `GlPixelStoreGuard` already correct | — |
| 6. Pass-entry state audit | CLEAN — mech via `applyPipeline`, decal+post explicit | — |
| 7. `barrierBitsFor` table extended + cardcloud migrated | DONE | `90b6aa5a` |
| 8. NVIDIA validation smoke checklist | PENDING — requires NVIDIA HW | — |

Invariants now in `docs/critical_inline_rules.md` (GPU resource invariants section).
Full plan: `docs/nvidia-hardening-s2.md`.
Tier1 5/5 PASS. Item 8 fires when NVIDIA hardware available.

## 2026-06-14 — Proof-machine S-program + GlStateGuard + RC reconcile + FX/pixel gate (nifty `1f0419ac`)

**SHIPPED (S11-S20 proof machine):**
- S12 unified manifest schema `ee4a583f` (`scripts/manifest_schema.py` + checker + tests + cockpit adoption)
- S9/S13 pixel gate `8461e3e0` (`scripts/run_visual.py` capture/compare/verify, byte-hash, refuses non-blessed) + **Baseline-A v2 `7e997e8f` (all 5 tier1, 15 golden frames, coherent off `1f0419ac`)**
- S11/S15 `c9421306` (release-install report w/ S12 identity; residency slim report sha+keep/drop/fail/unknown)
- S14 render-pass reader `0017bb2b` (`scripts/render_pass_report.py` + --compare)
- S20 GlStateGuard slice 2 `783406a8` (RAII depth/blend/cull on terrain chunk draw, A/B pixel-neutral)
- S19 FX fixture `1f0419ac` (`MC2_FX_FORCE_SPAWN` mech-fire PPC tubes in MechWarrior::updateActions)
- RC→nifty reconcile `d69611f1`/`7d087f16`/`77beb69c`/`4855fcf9`/`745a8a5b` (5 fixes incl. dark-water uninit-VRAM); merged engine set + importer `d821018c`/`eca37ed3`
- MC2_LOG hitch gate `47ef382f` (stdout→NUL default kills the 400ms printf hitch; harness sets MC2_LOG=1)

**LEFT:** S16 modder round-trip (`verify_mod_roundtrip.py`), S17 editor authoring depth, S18 asset-browser thumbnails, S13+ HTML gallery + advisory smoke-gate auto-wire, S15+ cook.json provenance + texture-res validator, #5 Tube/render merge (unblocked). Full: MEMORY.md 2026-06-14 handoff.

## Current state (2026-06-01)

**Nifty HEAD:** see MEMORY.md handoffs for current commit.

**In-flight / recently shipped:**
- Center-screen flicker + cinematic water (9d55bcbc, 47198695) — SHIPPED. Mech/water flicker (SimpleCamera cull MVP + mech readback conservative-OR); cinematic GPU water (SimpleCamera renderWaterFastPath + GL_CULL_FACE management). See 2026-06-01 handoff.
- TERRAIN-CLASSIFY-TUNING-1 + Mech ImGui section (3d5457d0) — see handoff
- PERF-GPU-CULL-READBACK-ID-CACHE-1 (ef03162a) — merge to nifty pending
- VFX-WEAPON-FX-RESTORE-OPUS-1 (9dfff4a9) — shipped
- VFX-DISTORTION-1 — BUILT (gate `MC2_VFX_DISTORTION`, default OFF; requires `MC2_VFX_SCENECOLOR_GRAB`). Heat-haze: distortion alpha group refracts the pre-VFX scene-color grab at a procedural (in-house value-noise) screen-UV offset, soft-clipped by scene depth; reuses `VfxBillboardAlpha` (no new pipeline/blend). Fixture `MC2_VFX_DISTORT_FIXTURE` tags alpha groups for observability. gate-OFF byte-identical. See `docs/VFX-DISTORTION-1.md`.
- Colormap BC7 KTX2 arc (7768d4e5) — shipped; .burnin soak + dead helpers pending
- perf session: setupTextures gate + water intro + recalcBounds (d65552ab)

**Outstanding work items:**
- WATER-REFLECTION-CLIP-1: terrain-RT empty at 20° camera (reflected projection + Lengyel oblique near-plane)
- VFX GPU sim: PARITY-ID-1 (per-particle IDs + forces)
- ~~Shadow cherry-pick to nifty (v0.4 crashes shadow-on at batcher.cpp:4778)~~ [CORRECTED 2026-07-01: obsolete — shadow lane long since landed on nifty; CSM default-ON since 2026-06-18]
- Colormap: delete dead helpers in mapdata.cpp + .burnin files after soak

## Infrastructure / permanent decisions

- **Track V post+grounding** (merged d8ccd032): MC2_HDR_POST/BLOOM/TONEMAP_ACES/SSAO all default-OFF. Soak doc: `docs/trackv-post-grounding-soak-1.md`.
- **Shadow lane** (merged 69522900; default-ON since c0525b27): dynamic shadows + 733 prop casters working. Kill-switch MC2_SHADOW_ENABLE=0. [CORRECTED 2026-07-01: current shadow architecture is cascaded shadow maps — gate `MC2_SHADOW_CSM`, default **ON** since 2026-06-18 (`8ff13a36`), kill-switch `MC2_SHADOW_CSM=0` (code `gos_postprocess.cpp mc2ShadowCsmEnabled()`); `MC2_SHADOW_ENABLE=0` remains a batcher-level kill-switch]
- **ViewUniforms UBO** (default-ON): binding=3, kill-switch MC2_VIEW_UNIFORMS=0.
- **MaterialGpu** (default-ON): MC2_MATERIAL_GPU / MC2_MATERIAL_GPU_SAMPLE.
- **DrawPacket v7** (default-ON): kill-switch MC2_STATIC_PROP_LEGACY_DISPATCH=1.
- **DrawPacket v8 — live-builder retired** (merged `96c27c2a`, default-ON): snapshot is sole static-prop draw-packet owner; flush() skips per-flush live build + compare. Kill-switch MC2_STATIC_PROP_LIVE_BUILDER=1. Commit-1 (retire); follow-ups: live-builder DELETE after soak (STATIC-PROP-LIVE-BUILDER-DELETE) + the Extract.SP.Fill dirty-list — **shipped as STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1, see below.** Spec/plan in docs/superpowers/{specs,plans}/2026-06-02-static-prop-snapshot-finish*.
- **Snapshot static-prop Fill dirty-only — SHIPPED + merged `cf654080` (2026-06-04), default-ON** (`STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1`, branch `8a209710..60cb4cf8`): `ExtractRenderSnapshot` skips `Extract.SP.Fill`+`WriteLoop` on clean (regGen+cullVer unchanged) frames and memcpy's cached rows into the snapshot arena → **median 1.68ms→36.7µs (−97%, user Tracy)**. Gate `MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY` (=0 kill-switch). Review chain complete; a material-cache staleness hole (`staticPropCacheTypePrimaryMaterial` bumped no generation, compare-oracle blind) caught + fixed `60cb4cf8`. **Contract: any registry mutation of a snapshot-captured field MUST bump `s_registryGeneration`.** Companion compare gate `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE`. Greybeard meta-fix debt (registry-owned incremental flat-row store = real end-state) logged in `memory/staticprop_per_frame_cpu_retire_campaign.md`.
- **GOM.readbackSnapshot eliminated** (SHADOW-COPY-1 `99519cbc`/`00bfc28a`): GPU-cull vis bits shadow-copied to system RAM; killed the per-frame BAR-read stall (360ns now). Kill-switch MC2_GPU_CULL_READBACK=0.
- **TRACKV CPU quick wins** (merged `26974734`): Probe-6 thin-canary readback gated MC2_THIN_CANARY default-OFF; mech MaterialGpu table persistent + dirty-gated upload; ShapeRenderer material-handle cache. renderLists self-time carve ABANDONED (audit rank-2 stale; the 985µs is static-prop registry/light/shadow = other-session lighting-ownership domain, per-instance light slots U=K). Audit: `docs/trackv-whole-frame-cpu-optimization-audit.md`.
- **HZB substrate** (merged 5864882c): depth pyramid built, no culling yet. Next: HZB-STATICPROP-CULL-CONSUMER-0 (default-OFF, 1e-4 margin, discontinuity guard).
- **Water reflection** (merged 68343329): SH-L2 sky, default-OFF. MC2_WATER_REFLECTION=1.
- **C++17** (CMAKE_CXX_STANDARD 17 in root CMakeLists).

## Hitch stability track (H-series)

H-series is recon/diagnostic only — no behavior changes. Must complete before Phase 8z deletion.

| Slice | Status | Detail |
|---|---|---|
| H1a — GL/resource attribution | **SHIPPED** `30dfd015` | `[HITCH]` / `[HITCH_GL]` / `[HITCH_TERRAIN_TEX]` / `[HITCH_STATIC_FLUSH]` / `[HITCH_WATER]` |
| H1b — WaterFastPath CPU sub-scopes | **SHIPPED** (2026-06-09) | `[HITCH_WATER_DETAIL]` guards/recipe/buildWindow/upload/dispatch sub-times |
| H1c — Broad frame-phase attribution | **PARTIAL** [corrected 2026-07-01] | `[HITCH_PHASE]` tag exists (`GameOS/gameos/mc2_hitch_trace.cpp:190`); full frame-phase breakdown not yet complete |
| H2 — Fast-path disruption / setupTextures guard | **SHIPPED** [corrected 2026-07-01] | `setupTextures` gated to `#ifdef MC2_IS_EDITOR` (`mclib/quad.cpp:706-1337`); game build compiled out |

**H2 priority:** ~~run before Phase 8z (legacy terrain deletion)~~ [CORRECTED 2026-07-01: H2 SHIPPED — setupTextures is editor-build-only; Phase 8z blocker cleared. Remaining 8z work: formally delete unreachable `makeLists` (skipped under chunk-ON but not deleted).]

## Unstarted campaigns (queued)

- **Terrain continuous surface** (all forks ruled 2026-05-18, design complete): see `terrain_continuous_surface_forks_ruled_option1_killlegacy.md`
- ~~**DrawPass retirement Slice B**: conjunction gate implemented but not shipped (`terrain.cpp`)~~ [CORRECTED 2026-07-01: SHIPPED — drawPass loop is gated off under `MC2_TERRAIN_LOD_CHUNK=1` (default ON); `mclib/terrain.cpp` "Terrain::render drawPass" zone ~empty on armed frames, one-shot retired log present]
- **Unified-projection F1**: plan at `docs/superpowers/plans/2026-05-22-unified-projection-v2-f1-atomic-plan.md`
- **VFX GPU sim Cardcloud parity**: `docs/vfx-gpu-sim-spec.md`
- **HZB static-prop cull consumer**: first draw-affecting slice, spec needed

## Compute substrate — light-grid lane

Default-OFF, zero-visual-change GPU compute infrastructure. Build facts → parity-prove → consume later (the depth-pyramid discipline). Reference corpus (clean-room, read-only): BT2018 `ClusteredLighting` GLSL + MW5 disassembled compute (`G:\bt_assets\computeshaders_dump`, `G:\mw5_shaders\global_out`); recon docs `.claude/BT-COMPUTE-SHADER-TECHNIQUE-RECON-1.md`, `.claude/FROXEL-LIGHTGRID-RECON-1.md`.

| Slice | Status | Detail |
|---|---|---|
| CLUSTER-DEPTH-PYRAMID-NATIVE-1 | **MERGED** `125b6e3e` | RG32F per-tile min/max depth substrate. Gates `MC2_CLUSTER_DEPTH_PYRAMID`/`_VERIFY`/`_PLANT` (OFF). Reversed-Z: near=MAX(G)/far=MIN(R). CPU/GPU parity proven; PLANT inspection-proven (runtime capture deferred — deploy-lock). `INFRA_PROVEN`. |
| MC2-LIGHTGRID-BUILD-NATIVE-1 | **MERGED** `05600221` | Inert per-tile light-bin grid builder. `MC2LightCullSphere` (sphere-only; cone DEFERRED — no source data) from `ObjectLights@20`. BT-shaped append (LDS stage → one global atomic reserve). Gates `MC2_LIGHTGRID_BUILD`/`_VERIFY`/`_PLANT` (OFF). CPU/GPU parity proven (exact set), PLANT **runtime-captured**. 16-cap unchanged. `INFRA_PARITY_PROVEN`. |
| POSTPROCESS-COMPUTE-BLUR-1 | **branch** `claude/postprocess-compute-blur-1` | Greenfield INERT GPU compute downsample (2x2 box) + separable 5-tap Gaussian blur (binomial `[1,4,6,4,1]/16`), ping-pong half-res RGBA16F. Vulkan-prep: typed-sync + ping-pong pattern. Gates `MC2_POSTPROCESS_COMPUTE_BLUR`/`_VERIFY`/`_PLANT` (OFF). Typed `gpuSyncBarrier` edges (`ComputeImageWrite→TextureSample`/`→TextureReadback`), no raw glMemoryBarrier. CPU/GPU parity PASS tol 1e-3 (worst 5.3e-4), PLANT runtime-captured FAIL. No consumer (substrate). mc2_24 GL-fatal PASS, gate-OFF byte-identical. `INFRA_PARITY_PROVEN`. Doc `docs/POSTPROCESS-COMPUTE-BLUR-1.md`. |
| LIGHT-ABI-WIDEN-STAGE0-1 | **MERGED** `2c50c8f1` | Per-object GPU light record widened 16→32 slots (stride 1808→3600, N·112+16). 5-site lockstep + tripwire `scripts/check-light-abi-lockstep.py`. Runtime clamp kept 16. `ABI_WIDENED_PARITY_PROVEN`. |
| LIGHT-CLAMP-RAISE-STAGE1-1 | **MERGED** `893346cf` | Runtime clamp raised 16→32 (`txmmgr.cpp kRuntimeLightClamp`). Default-OFF `MC2_LIGHT_CLAMP_PROBE` (high-water logger) + `MC2_LIGHT_CLAMP_FIXTURE` (synthetic >16 injector). New checker `scripts/check-light-loop-numlights-bound.py`. stock ≤16: `NO_VISUAL_CHANGE`; fixture >16: `VISUAL_PROVEN`. `RUNTIME_CLAMP_32_PROVEN`. |

POSTPROCESS-COMPUTE-BLUR-1 ledger (machine-readable):

```yaml
POSTPROCESS_COMPUTE_BLUR:
  type: COMPUTE_SUBSTRATE
  gate_default: OFF
  gate_off: BYTE_IDENTICAL
  visual_change: NONE (no consumer)
  parity: CPU_GPU_PROVEN (tol 1e-3, worst 5.3e-4)
  sync: TYPED_GPUSYNCBARRIER
  unlocks: future bloom/glow/DOF consumer
```

**Lane PARKED 2026-06-23.** The ABI (32) + runtime clamp (32) substrate is built and proven, but `MC2_LIGHT_CLAMP_PROBE` shows stock content uses **~2 lights/object** (mc2_24 light-dense canary high-water = 2). The raised capacity is latent/future-facing; froxel/clustered binning (pays off at hundreds of lights/tile) has **no stock payoff** and stays **DEFERRED indefinitely** until dense dynamic-light content exists. No shading consumer of the lightgrid built. Full state: memory `light-cap-lane-parked.md` + recon docs `.claude/{LIGHT-CAP-LIFT,LIGHT-CLAMP-RAISE-STAGE1}-RECON-1.md`.

## RenderWorld arc

Steady-state ledger: `docs/renderworld_arc_status.md`. M1-M2 shipped; M3-M5 = DECISIONS (not ongoing work). Migration guide: `docs/renderworld_migration_guide.md`.
