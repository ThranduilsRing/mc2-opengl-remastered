<!-- doc-status: current | reviewed:2026-10-11 -->
# RENDER-PASS-ATTACHMENT-CONTRACT-RECON-1

Read-only recon. Defines the **per-pass attachment I/O contract** for the 14 major
named passes: what each pass binds, writes, samples, clears, and how it can form a
feedback loop. This is the Vulkan-prep seam after PipelineDesc state ownership +
colorMask ownership + the frame-resource-ledger: the
"pass A writes X, pass B reads X, here is the copy/target contract" middle.

Source-verified against nifty `0aab5e87` (worktree `render-pass-attachment-recon-1`).
File:line are `GameOS/gameos/…`; `.frag` are `shaders/…`. Frag output sets confirmed
by `layout(location=)` grep (the field agents got wrong last arc — re-checked here).

## Contract fields (per pass)

`Phase · TargetFBO · DrawBufferSet · ColorOutputs(loc 0/1/2) · Depth(test/write) ·
SampledFrameResources · Clear/Load · Viewport · PipelineDesc row · ColorMask owned ·
Hazards`.

## The 14-pass contract table

| Pass | Phase | FBO | DrawBufs | Outputs 0/1/2 | Depth | Samples (frame resources) | Viewport | Pipeline row | colorMask owned |
|---|---|---|---|---|---|---|---|---|---|
| **TerrainSolidLODChunk** | Scene | sceneFBO_ | `setSceneDrawBuffers(MainSceneMRT)` → {0,1}\|{0,1,2} | color0✓ / GBuffer1✓ / — | GEQUAL, write ON | colormap atlas, static+dyn shadow maps, normal array, height SSBO, cement atlas, transition mask | full | `TerrainSolid` | no (routed slice, own state block) |
| **MechOpaque** | Scene | sceneFBO_ (inherits) | inherited MRT | color0✓ / GBuffer1✓ / objectId✓ (`#ifdef MC2_OBJECT_ID_BUFFER`) | GEQUAL, write ON | mech albedo + PBR detail/paint normal+ORM, AO/normal (imported), material SSBO | full | `MechOpaque` | no |
| **StaticPropOpaque** | Scene | sceneFBO_ (inherits) | inherited MRT | color0✓ / GBuffer1✓ / objectId✓ (cond) | GEQUAL, write ON | prop tex array, ORM array, per-draw/material/type-color SSBOs | full | `StaticPropOpaque` | no |
| **WaterFastPath** | Scene (late) | sceneFBO_ (inherits) | inherited MRT (alpha-blend over) | color0✓ / GBuffer1✓ (flat-up sentinel) / — | LEQUAL, write ON (OFF if `MC2_WATER_NO_DEPTH_WRITE`) | water base/detail tex, **waterReflRT (unit 2)**, HDRI equirect, height SSBO | full | `WaterArmed` | no |
| **VFX billboard** | Scene VFX | sceneFBO_ (inherits) | inherited MRT | color0✓ / — / — | GEQUAL, write **OFF** | **sceneDepthCopyTex_** (soft, TU1), **sceneColorCopyTex_** (distort, TU2) | full | `VfxBillboard{Alpha,Additive}` | no |
| **VFX tube ribbon** | Scene VFX | sceneFBO_ (inherits) | **forces SingleColor {0}** (`:753`), restores MRT (`:877`) | color0✓ / — / — | GEQUAL, write OFF | particle atlas only | full | `VfxTube{Alpha,Additive}` | no |
| **VFX mesh** | Scene VFX | sceneFBO_ (inherits) | inherited MRT | color0✓ / — / — | GEQUAL, write OFF | mesh tex; (blackbody/distortion variants sample scene-color copy) | full | `VfxMesh{Alpha,Additive}` | no |
| **ProjectedDecals (box)** | Post-opaque | sceneFBO_ (explicit `:1309`) | `SingleColor {0}` (`:1310`) | color0✓ / — / — | ALWAYS, write OFF | **sceneDepthCopyTex_** (TU0), sceneNormalTex_ (TU1, gated reject) | full | `boxDecalProg_` (fixed, not PipelineId) | no |
| **SSAOApply** | PostProcess | sceneFBO_ | `SingleColor {0}` | color0✓ / — / — | (no depth) | ssaoColorTex_ (R16F half-res) | full | `PostProcessSsaoApply` | **YES** (e28a982d) |
| **ScreenShadow** | PostProcess | sceneFBO_ | `SingleColor {0}` | color0✓ / — / — | (no depth) | shadow map, depth | full | `PostProcessScreenShadow` | **YES** |
| **CloudShadow** | PostProcess | sceneFBO_ | `SingleColor {0}` | color0✓ / — / — | (no depth) | cloud tex, depth | full | `PostProcessCloudShadow` | **YES** |
| **Shoreline** | PostProcess | sceneFBO_ | `SingleColor {0}` | color0✓ / — / — | (no depth) | depth | full | `PostProcessShoreline` | **YES** |
| **EdgeFog** | PostProcess | sceneFBO_ | `SingleColor {0}` | color0✓ / — / — | (no depth) | depth | full | `PostProcessEdgeFog` | **YES** |
| **FogOob** | PostProcess | sceneFBO_ | `SingleColor {0}` | color0✓ / — / — | (no depth) | depth | full | `PostProcessFogOob` | **YES** |
| **Composite** | PostProcess (end) | sceneFBO_ → then FBO **0** | `SingleColor {0}` | color0✓ / — / — | OFF | sceneColorTex_ (final resolve to backbuffer; bloom/ACES/FXAA were deleted — DEAD-POST-FX-CLEANUP) | full | `PostProcessComposite` | **YES** |
| **HUD / UI 2D** | Frame end | **FBO 0** (`endScene :1798`) | implicit single (backbuffer) | color0✓ / — / — | OFF, write OFF | font/atlas only — **no scene resource** | fixed 800×600 logical, physical full | `basic/text material` (not PipelineId) | n/a (FBO 0) |

## Cross-cutting findings

1. **Draw-buffer ownership is split.** Scene-geometry + VFX **inherit** the MRT draw
   set from the terrain bind (via the `setSceneDrawBuffers` chokepoint, see
   frame-resource-ledger); only terrain explicitly
   sets it. Post-fx + decals + tube-ribbon explicitly switch to `SingleColor {0}`.
   No pass restores MRT except tube-ribbon (`:877`) and the per-frame `beginScene`
   re-bind. → A pass that writes GBuffer1/objectId must run while the MRT set is
   live; one that doesn't should be on SingleColor. This is the seam DRAWBUFFER-
   OWNERSHIP-1 would formalize.

2. **The AMD R32UI-blend suppression** is real and load-bearing: with `GL_BLEND` on
   AND an integer (R32UI objectId) attachment in the active draw list, AMD drops the
   COLOR0 write. Tube-ribbon defends by forcing SingleColor {0} (`:742`–`:754`).
   Any future blended pass that runs under the 3-attachment MRT inherits this trap.

3. **Feedback safety holds today.** Every pass that samples a scene resource it could
   be writing uses a **copy**: VFX/decals sample `sceneDepthCopyTex_` /
   `sceneColorCopyTex_`, never the live `sceneDepthTex_` / `sceneColorTex_`. Decals
   sample live `sceneNormalTex_` (COLOR1) but never write it. Composite samples
   `sceneColorTex_` only after the scene is fully resolved (FBO switches to 0). **No
   read-from-bound-attachment loop found.**

4. **colorMask vs draw-buffer redundancy.** Post-fx passes both bind SingleColor {0}
   *and* (post e28a982d) own colorMask {t,f,f}. Two mechanisms, same GBuffer
   protection. The colorMask is the safety net for a pass that forgets the draw set.

## Hazard ledger — the bug classes this contract catches

| Hazard class (TD list) | Current verdict | Evidence |
|---|---|---|
| Sample `sceneColorTex_` while rendering into it | SAFE | copies used (`sceneColorCopyTex_`); composite reads after FBO→0 |
| Forget to copy scene color before distortion | GUARDED | distortion path gated on `MC2_VFX_SCENECOLOR_GRAB` copy; no consumer yet |
| Draw to SingleColor but assume MRT | LATENT RISK | inheritance is implicit; only terrain/tube re-assert — a reordered pass could mismatch |
| GBuffer1/objectId accidentally disabled | SAFE-by-AMD-fix | tube-ribbon SingleColor forced; R32UI-blend trap documented |
| Post-FX reads stale depth/normal | SAFE | post-fx depth reads are the live depth attachment as a *texture sample* after scene resolve; not re-written |
| Decals depend on normals before valid | SAFE | decals run post-opaque; normals fully written by then; reject gated default-off |
| Viewport mismatch half-res vs full-res | LATENT | ssao/waterRefl/hzb are sub-res FBOs with their own viewport; no shared-viewport assert exists |
| Clear/load depends on prev-frame garbage | SAFE | `beginScene` clears + colorMask keystone (`:1389`); no `glInvalidateFramebuffer` anywhere |

## Recommended next slices (per TD sequence)

1. **RENDER-PASS-ATTACHMENT-SCAFFOLD-1** (GO, no behavior change) — add
   `RenderPassAttachmentDesc` / `FrameResourceId` / `DrawBufferSetId` and extend the
   `[FRAME_PLAN]` trace with `target=… drawBuffers=… reads=…` fields; a checker that
   verifies the known contracts in this table.
2. **FRAME-RESOURCE-FEEDBACK-CHECKER-1** (GO) — `check-frame-resource-feedback.py`:
   *no pass may sample a resource it is currently rendering into unless via an
   explicit feedback-safe copy*. Encodes finding #3 as a hard gate; protects the
   scene-color-grab / distortion / decal work.
3. **DRAWBUFFER-OWNERSHIP-1** (GO) — make the MRT-vs-SingleColor draw set a pass-owned
   field (finding #1) so inheritance is explicit, not implicit; removes the
   "draw to SingleColor but assume MRT" latent risk.
4. Then route remaining terrain/water/post/VFX passes *with* attachment contracts.

**Do NOT next:** full render graph · pass reordering · one-giant-FBO-bind rewrite ·
viewport ownership rollout · texture-unit ownership rollout · Vulkan backend.
Viewport + tex-unit seams are real but second-tier; attachment I/O is the
Vulkan-critical one.

## Open / UNKNOWN (not blocking)

- WaterFastPath `waterReflRT` (unit 2): RAW-safe only if `RenderWaterReflectionPass`
  populated it this frame *before* water draws; a stale-RT frame-lag artifact is not
  prevented by any contract (perceptual, not a feedback loop).
- VFX-mesh blackbody/distortion exact sampled-resource set: confirm against the
  blackbody draw path when SCAFFOLD-1 wires the trace.
