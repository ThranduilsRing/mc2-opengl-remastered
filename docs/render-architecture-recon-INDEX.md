# Render Architecture Recon — INDEX (2026-06-11)

Ten parallel recons answering "what owns the frame?" Goal: turn many successful modern
lanes (chunk terrain, GPU props, GPU mechs, VFX, HZB, Backend-A) into one coherent renderer.
All docs in this directory, all file:line-cited against nifty worktree at recon time.

## The docs

| # | Doc | One-line verdict |
|---|-----|------------------|
| 1 | [renderworld-pass-graph-audit.md](renderworld-pass-graph-audit.md) | No render graph; imperative chain gamecam → renderLists → endScene. All 3D into `sceneFBO_` (RGBA16F + GBuffer1 + optional R32UI objectId + reverse-Z depth), fullscreen-shader composite to FBO 0 (no blit). Chunk terrain draws BEFORE renderLists and is the ONLY scene pass that explicitly owns/restores full GL state. render-pipeline-map.md §1 Terrain STALE (chunk path default-on since a7b090be). |
| 2 | [renderworld-immediate-draw-regression-recon.md](renderworld-immediate-draw-regression-recon.md) | Tube-oracle vanish SOLVED, fix already on nifty tip `185ae3b1`: R32UI objectId in MRT draw-buffer list + GL_BLEND → AMD suppresses COLOR0; plus pre-renderLists flush with depthMask FALSE. Fix = deferred flush post-renderLists with single-COLOR0 bracket (`gos_particle_bridge.cpp:582-731`). Gate `MC2_VFX_ORACLE_TUBE=1` still default-OFF pending AMD visual A/B. Generalize bracket into a TransparentScenePass for particle/tube/water/vfx-mesh. |
| 3 | [transparent-sorting-policy-recon.md](transparent-sorting-policy-recon.md) | Three ordering worlds (MLR true sorter ~dead, legacy flag-bucket walk, GPU call-position). TWO LIVE BUGS: `NonTerrainAlphaLoops` (txmmgr.cpp:3011) and spotlights (:3135) blend with depth-write ON. Proposes 8-layer policy + 64-bit sort key (layer/view/translucency/depth24/material/instance). |
| 4 | [material-abi-unification-recon.md](material-abi-unification-recon.md) | Unified record ALREADY EXISTS: `RenderCore/MaterialGpu.h:88` (32B std430). Props live (binding 5), mechs uploaded-but-compare-only (binding 2). #1 blocker = texture-identity split-brain (array layer vs texmgr handle vs GL id). Terrain splat stays separate palette ABI. Bug found: roughness default disagrees 1.0/0.6/0.5 across table/frag/viewer. V2 = 48B. |
| 5 | [render-pass-gpu-timing-recon.md](render-pass-gpu-timing-recon.md) | Timer infra exists 3 places; `editor/EditorGpuTimer` = the never-stall template; Tracy GpuZones already mark every boundary. 15-row insertion table specced. Format `[RENDER_PASS_TIME v1]`, gate `MC2_RENDER_PASS_TIME=1`, 4-frame query ring, MCP `get_render_pass_times` phase 3. |
| 6 | [shadow-pipeline-recon.md](shadow-pipeline-recon.md) | Two 4096² DEPTH24 sun maps (static world-fixed terrain-only + dynamic per-frame mechs/props). Terrain receives inline PCF; everything else deferred `shadow_screen.frag` via GBuffer1 sentinel. Scene reverse-Z but shadow passes FORWARD-Z (manual glClearDepth swap — fragile, don't touch). Cheapest visual win: flip already-shipped default-OFF B′ bounded-near fit + registry building casters. Then 2-cascade CSM. |
| 7 | [hzb-draw-consumer-recon.md](hzb-draw-consumer-recon.md) | HZB built in endScene, probe-only, zero cull coupling. Smallest safe consumer = HZB term inside C1b static-prop bucket admit (`shaders/gpu_cull.comp:264-282`). Hazard: pyramid is frame N−1 (cull dispatch precedes scene draw) → margin + fail-open. Sticky-block re-admit path must NEVER get HZB. Rollout: advisory counters → large buildings → all props → default-on. Never terrain, never movers, never gameplay flags. |
| 8 | [renderworld-frame-inspector-recon.md](renderworld-frame-inspector-recon.md) | ~70% exists (RenderSnapshot + EditorInspector ImGui spines + MC2_DEBUG_STATE_DUMP/MCP). Gaps: runtime current-pass ID, FBO tracker, chunk-terrain accessor, per-frame VFX counts. Rec: one `FramePassStats` collector feeding JSON dump + ImGui tab; hook = RenderCore beginPass/endPass on existing RenderPassId. |
| 9 | [gpu-scene-record-recon.md](gpu-scene-record-recon.md) | YES shared record — for cull/identity tier only, and it half-exists: `GpuActorRecord` 64B binding 8, already shared by props+mechs+vehicles+turrets+gates. Draw payloads can NOT merge (prop 112B / mech 64B boneless / terrain per-quad recipes / chunk = none). Proposal: keep 64B hot record byte-identical, add cold 16B Ext SSBO (binding 26: meshId, materialIdx, sortKey, payloadIndex). Fix binding-14 particle/readback clash first. |
| 10 | [render-api-boundary-recon.md](render-api-boundary-recon.md) | Boundary mostly real: code/=0 raw GL (lint live), RenderCore GL-free, mclib diag-only. GameOS/gameos = 3990 calls = legit owned zone. Leaks: RenderWorld.cpp:829 pick glReadPixels; editor/ 68 + GuiRuntime/ 7 unpoliced. REAL problem = inherited cross-pass GL state (~25 TUs, 13 manual cache invalidations) = the bug factory behind 10.3 transparency / explosion cull / water vanish. GlStateGuard is still docs-only. |

## Cross-cutting findings (things multiple recons hit independently)

1. **State inheritance is the renderer's #1 bug factory.** Pass graph (#1), compositing (#2), transparency (#3), and RHI (#10) all converge: passes inherit GL state instead of owning it. Chunk terrain submit is the only correct citizen. The Tube vanish, the 10.3 terrain transparency, depth-write-ON alpha bugs — all the same class. → GlStateGuard (post-Baseline-A) + per-pass explicit-state brackets.
2. **MRT draw-buffer list is now a contract.** RenderWorld M1.5's R32UI attachment means ANY blended draw into sceneFBO_ must bracket down to COLOR0. The deferred-tube fix is the prototype; needs generalizing (TransparentScenePass) before VFX mesh lands.
3. **The substrate convergence already started.** Cull/identity (GpuActorRecord b8), material (MaterialGpu b5/b2), and frozen-static-prefix are each half-unified. The recons give per-lane completion orders that don't collide.
4. **Introspection is cheap.** Timers (#5) and inspector (#8) are mostly assembly of existing parts (EditorGpuTimer, Tracy zones, RenderSnapshot, debug-state JSON).

## Ranked next actions — USER-RATIFIED ORDER (2026-06-11 review)

Verdict: renderer no longer blocked by ignorance — blocked by STATE OWNERSHIP.
Strategy = "render stability train", not render rewrite. Do NOT open ten branches.

1. **Deploy coherence chip** (verify deployed exe mtimes vs fix commits — known v0.4 vs 0.4c trap).
2. **Fix two depth-write-ON transparency bugs** (txmmgr.cpp:3011 NonTerrainAlphaLoops, :3135 spotlights).
3. **`[RENDER_PASS_TIME v1]`** timers (`MC2_RENDER_PASS_TIME=1`, 4-frame query ring) — makes everything below budget-driven.
4. **Shadow Stability v1** — NOT just Shadow Pretty: (a) explicit static/dynamic shadow-pass state entry/exit, (b) log clearDepth/depthFunc/colorMask/cull/viewport, (c) B′ fit behind flag, (d) building casters behind flag, (e) repeated-run shadow-presence test. State/clear restoration is the likely stability fix, B′ is quality.
5. **Tube AMD A/B** → flip `MC2_VFX_ORACLE_TUBE` default-ON if clean. Do not revisit profile/growth/spawn/MVP/FBO/draw-phase unless A/B fails in a NEW way.
6. **FramePassStats collector** — one collector feeding JSON + ImGui; pairs with 3. No giant inspector UI first.
7. **Material M0** — pin texture-semantic contract, fix roughness default disagreement, document terrain splat as separate palette ABI. No "PBR everywhere" yet.
8. **GlStateGuard slice 1** (after Baseline A) — kills the bug-factory class.
9. **TransparentScenePass bracket** (generalize the tube single-COLOR0 bracket).
10. **HZB advisory counters ONLY** (wouldCull / guardedWouldCull / skippedCameraDiscont / falseNegative budget) — gated on Baseline A + GlStateGuard slice 1 + frozen-records M1.
Also now: lint widening (editor/GuiRuntime allowlist, RenderWorld strict); pick-readback rehome soon.

### Explicitly DEFERRED (prereqs now clear, not bad ideas)

full render graph · RHI rewrite · 2-cascade CSM · HZB real culling · scene-record Ext SSBO ·
PBR material expansion · VFX mesh substrate · full 64-bit transparent sort key · another pass-graph doc.

## Stale-doc notes

- `render-pipeline-map.md` §1 Terrain claims DrawIndirect default — STALE, chunk path default-on.
- `renderpass-contract-spec.md` Terrain owner row stale.
- `render-contract.md` partially pre-cutover.
- `gos_terrain.frag:831` comment claims `*` combine — code is `min()`.
- This worktree HEAD during recon = tacmap branch; Tube fixes live on nifty tip `185ae3b1`.
