# RenderWorld Immediate-Draw Regression Recon — Tube Oracle Invisibility

**Date:** 2026-06-11 · **Recon-only, no code changes.**
**Traced against:** branch `claude/nifty-mendeleev` tip `185ae3b1` (all file:line below
are nifty-tip unless noted). NOTE: this worktree's checked-out HEAD is
`claude/tacmap-formation-line-v1` (`f66c2e8e`), which does NOT contain the Tube oracle
at all — do not grep the working tree for it; use `git show claude/nifty-mendeleev:<file>`.

**Headline:** the regression is ROOT-CAUSED and the fix is ALREADY ON THE NIFTY TIP
(cherry-picked from `claude/vfx-tube-oracle-stopgap`): an MRT draw-buffer interaction
introduced by the RenderWorld arc's R32UI objectId attachment, plus a draw-phase
(pre-renderLists) ordering bug. The Tube oracle gate remains **stopgap default-OFF**
(`MC2_VFX_ORACLE_TUBE=1` to enable) pending visual A/B.

---

## 0. Commit archaeology (the whole story in four commits)

| Commit | Branch | What |
|---|---|---|
| `e0d064e0` / `acad80e7` / `cb3598d1` | in nifty | Tube ribbon oracle slices 1+2, then default-ON |
| `ac9f3315` | in nifty (the "known-good" ref) | blend-aware discard in `tube_ribbon.frag` — Tube oracle visible here, **pre-RenderWorld-objectId on the user's render path** |
| `7bdedbf1` | stopgap branch (equiv. on nifty tip, `batcher.cpp:175-192`) | STOPGAP: gate default-OFF; submits confirmed (`addRibbon=83538`) but nothing rendered |
| `766648da` | stopgap branch (equiv. on nifty tip) | Phase fix: Tube::Draw ENQUEUES; deferred flush post-renderLists (`gamecam.cpp:453`) |
| `31ac0dcc` | stopgap branch (equiv. on nifty tip) | **THE root cause** (probe-confirmed): scene FBO has 3 active draw buffers (color + GBuffer1 normal + **R32UI objectId, landed with RenderWorld arc**); `tube_ribbon.frag` writes only location 0; with `GL_BLEND` enabled AND an integer attachment in the active draw-buffer list, AMD suppresses the COLOR0 write → ribbons invisible. Fix: force single `GL_COLOR_ATTACHMENT0` for the ribbon batch, restore MRT list after. |

Probe (`MC2_TUBE_DEFER_PROBE`, still in tree, env-gated) ruled out stale-MVP,
FBO mismatch, colorMask, scissor: `drawBufs=3, mvp valid, colorMask=1111, scissor=0`.

---

## 1. Pass graph — before vs after the RenderWorld arc

### Presented-frame plumbing (unchanged by the arc)

```
gameosmain.cpp:527  pp->beginScene()          → glBindFramebuffer(sceneFBO_)   gos_postprocess.cpp:938
                                                 MRT draw buffers bound for the ENTIRE scene draw
  GameCamera::render (code/gamecam.cpp):
    ~:327  Sky::renderHdri              → sceneFBO_
    ~:338  land->render()  (terrain)    → sceneFBO_
    ~:358  land->renderWater()
    ~:364  ObjectManager->renderShadows
    ~:393  mcTextureManager->renderLists()      ← Spine A/B flush (txmmgr.cpp:1922 region)
    ~:410  land->renderWaterFastPath()
    ~:444  particles Batcher::Flush()           ← GPU billboards (gos_particle_bridge.cpp:190)
    ~:453  gos_tube_ribbon_flush_deferred()     ← Tube oracle (NEW deferred site)
    ~:472  theClipper->RenderNow()              ← legacy MLR CPU gosFX leaves
gameosmain.cpp:603  pp->endScene()             → glBindFramebuffer(0), composite sceneColorTex_
                                                 (+bloom/FXAA/tonemap) to BACKBUFFER   gos_postprocess.cpp:1767/1798
gameosmain.cpp:624  gos_RendererFlushHUDBatch() → backbuffer (HUD)
gameosmain.cpp:628  GuiRuntime::Render()        → backbuffer (ImGui)
SwapBuffers
```

### What the RenderWorld arc changed

- **Before (≈`ac9f3315` runtime state):** sceneFBO_ draw-buffer list = 2 entries
  (COLOR0 RGBA16F + COLOR1 GBuffer1 normal). A blended frag shader writing only
  location 0 composites fine.
- **After (M1.5 objectId substrate, default ON `MC2_OBJECT_ID_BUFFER`,
  `RenderWorld/RenderWorld.cpp:74-79`):** `setSceneDrawBuffers(MainSceneMRT, true)`
  binds a **3-entry list incl. GL_COLOR_ATTACHMENT2 = GL_R32UI**
  (`gos_postprocess.cpp:37-60`, attachment created `:575-594`), and **MRT stays bound
  for the entire scene draw** (`beginScene`, `gos_postprocess.cpp:930-961`).
  Any blended immediate draw that doesn't manage its own draw-buffer list now blends
  against an integer attachment → undefined per GL spec; AMD drops the COLOR0 write.

### Tube oracle pass position, before vs after fix

- **Broken (as default-ON'd in `cb3598d1`):** immediate `gos_tube_ribbon_flush` ran
  inside `gosFX::Tube::Draw` — the effect-render phase BEFORE `renderLists()`, with
  `glDepthMask(FALSE)` → no depth written → opaque geometry from renderLists
  overwrote the ribbon color. Two stacked failure modes (phase + MRT).
- **Fixed (nifty tip):** `Tube::Draw` enqueues world-space ribbon records
  (`gos_tube_ribbon_enqueue`, `mclib/gosfx/tube.cpp:~1554` region;
  `gos_particle_bridge.cpp:556`); drained at `gamecam.cpp:453` right after
  `Batcher::Flush()`.

---

## 2. Current FBO / viewport / depth / blend at Tube-oracle flush time

`gos_tube_ribbon_flush_deferred` (`gos_particle_bridge.cpp:582-731`):

- **FBO:** sceneFBO_ (inherited — beginScene bound it; correct target).
- **Viewport:** scene viewport (inherited, full scene res).
- **Depth:** `GL_DEPTH_TEST` ON, `glDepthFunc(GL_GEQUAL)` (reverse-Z),
  `glDepthMask(FALSE)` — set explicitly (`:621-624`). Depth buffer is populated
  because flush is post-renderLists.
- **Blend:** ON; per-record alpha (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`) or additive
  (`SRC_ALPHA, ONE`), cull OFF, VAO bound (AMD trap #4).
- **Draw buffers:** saved 8-slot list, then `glDrawBuffers(1, {COLOR_ATTACHMENT0})`
  for the batch (`:634-648`), restored after (`:731`). This is the MRT fix.
- **MVP:** `gos_GetTerrainMVPMat4()` uploaded at flush time (valid in this phase;
  neutralizes the stale-MVP failure mode).
- Full GL state save/restore around the batch (`:592-605`, restore tail).

## 3. Where legacy MLR renderLists draws

- Lasers/beams/CPU-baked gosFX geometry enqueue via `addTriangle(...,
  MC2_ISEFFECTS/MC2_DRAWALPHA)` into the masterHardwareVertexNodes arrays
  (`mclib/txmmgr.cpp:764` family) and are drawn inside
  `MC_TextureManager::renderLists()` → `ShapeRenderer::render` →
  `gos_RenderIndexedArray` (`txmmgr.cpp:1922 / 2088 / 1750`), shader
  `gos_tex_vertex_lighted`. This path goes through `applyRenderStates` / the gos
  pipeline, which is MRT-aware — that is why legacy MLR weapon FX still appear.
- Remaining CPU gosFX leaves draw at `theClipper->RenderNow()`
  (`code/gamecam.cpp:~472`, "Draw the FX"), also via the gos submission pipeline,
  after renderLists — correctly phased by the MLR sorter.

## 4. Where the gos_particle_bridge immediate draw happens

`GameOS/gameos/gos_particle_bridge.cpp` — `gos_particle_bridge_flush(:190)`:
SSBO binding 14 upload, per-group blend, `glDrawArrays(GL_TRIANGLES, n*6)`,
shaders `particle_billboard.{vert,frag}`. Called from `Batcher::Flush()` at
`gamecam.cpp:~444` — already DEFERRED post-renderLists (the precedent the tube fix
copied). It saves/restores GL state and calls `gos_InvalidateRenderStateCache()`.
(Note: the billboard bridge writes only location 0 too — it survives because it
likewise constrains state; if any blended bridge ever skips the draw-buffer
narrowing it will hit the same AMD MRT suppression.)

## 5. Which target is presented

`sceneFBO_` color attachment 0 (`sceneColorTex_`, RGBA16F,
`gos_postprocess.cpp:541-553`) is the ONLY thing that reaches the presented frame —
sampled by the composite fullscreen quad in `endScene()` after
`glBindFramebuffer(GL_FRAMEBUFFER, 0)` (`gos_postprocess.cpp:1767/1798-1830`),
with bloom/FXAA/tonemap, then HUD batch + ImGui draw directly on the backbuffer
(`gameosmain.cpp:624/628`). GBuffer1 (COLOR1) feeds screen-shadow/SSAO; objectId
(COLOR2) feeds pick readback only. **Anything that fails to land in COLOR0 of
sceneFBO_ during the scene phase simply does not exist in the presented frame** —
which is exactly what happened to the tube ribbons.

## 6. Legal fix options

1. **(Shipped on tip)** Deferred flush post-renderLists + single-COLOR0 draw-buffer
   override inside the bridge, restore after. Self-contained, zero contract change.
2. Per-pass draw-buffer policy: have `beginScene` expose a
   `setSceneDrawBuffers(SingleColor/MainSceneMRT, ...)` bracket that all transparent
   passes (particles, tubes, water fast-path) call — centralizes the rule
   "transparent VFX never write COLOR1/COLOR2".
3. Make VFX shaders MRT-complete (write a sentinel to COLOR1) — does NOT fix COLOR2:
   integer attachments cannot be blended at all, and M4 PROHIBITS VFX objectId writes
   (`scripts/check-vfx-no-objectid.sh`). Not legal for attachment 2; rejected.
4. A real transparent-pass stage in the frame graph after the opaque MRT stage, with
   draw-buffers narrowed once for the whole stage (see §9). The structural version
   of (2).

## 7. Smallest stopgap

Already in tree: `MC2_VFX_ORACLE_TUBE` stopgap default-OFF
(`mclib/particles/batcher.cpp:175-192`) — legacy MLR renders all weapon FX. Zero
risk; weapon FX visible today.

## 8. Recommended real fix

The mechanical fix is done (deferred flush + draw-buffer narrowing, §2). What
remains:

1. **Visual A/B on AMD** with `MC2_VFX_ORACLE_TUBE=1` (PPC/gauss/AC fire in
   mc2_10/mc2_24 interactive) — if ribbons render, flip the gate default back ON
   (revert the `7bdedbf1`-equivalent default in `batcher.cpp`).
2. **Generalize before the next bolt-on:** adopt option (2)/(4) — a
   `TransparentScenePass` bracket (narrow to COLOR0 + depth-test GEQUAL +
   depthMask FALSE + restore) owned by gos_postprocess or RenderPassContract, used
   by particle bridge, tube bridge, water fast-path, future vfx_mesh bridge. This
   kills the recurring class: "bolt-on immediate draw inherits MRT/blend/depth
   state it never set" (same lesson as the terrain-chunk 10.3 transparency saga and
   `memory/blend_state_inheritance_in_post_process.md`).
3. Register the tube pass in `RenderCore/RenderPassContract.h` (VFX row exists,
   id=5) and keep the probe env documented in `docs/tier1_env_vars.md`.

## 9. Legal home for each draw family

All world-space content must land in **sceneFBO_ COLOR0 during the scene phase**
(between beginScene and endScene); screen-space UI lands on the **backbuffer after
endScene**. Per family:

| Family | Legal home | Draw-buffer rule |
|---|---|---|
| **GPU Tube ribbons** | deferred post-renderLists scene-phase slot (`gamecam.cpp:453`), sceneFBO_ | COLOR0 only (transparent; no normal/objectId) — shipped |
| **VFX mesh (Shape/ShapeCloud/DebrisCloud oracle)** | same deferred transparent slot as tubes/particles, after renderLists (depth populated) | COLOR0 only; must reuse the same draw-buffer bracket from day 1 — do NOT repeat the tube mistake |
| **Tactical overlays (move orders, formation lines, LOS)** | world-space: scene-phase after opaque flush (can depth-test) → sceneFBO_ COLOR0; screen-space variants: HUD batch (`gos_RendererFlushHUDBatch`, backbuffer, HUD_DEPTH=0.9999) | COLOR0 only if scene-phase |
| **Debug overlays (DebrisCloud wires, probe viz, DebugRenderer prims)** | existing DebugRenderer slot (`gamecam.cpp` post-RenderNow, pre-endScene) → sceneFBO_ | COLOR0 only; never bloom-critical, so post-process placement is also acceptable |
| **Selection rings / range rings** | scene-phase transparent slot (need depth-test vs terrain, want bloom OFF on them ideally pre-tonemap is fine) → sceneFBO_ COLOR0, depthMask FALSE | COLOR0 only |
| **Thermal view (fullscreen recolor)** | post-process: a pass in `gos_postprocess.cpp` between scene and composite (reads sceneColorTex_/GBuffer1), or a composite-shader mode | owns its own FBO/draw-buffer state like bloom does |

**Rule of thumb going forward:** anything blended NEVER inherits the MRT list;
anything opaque-with-identity goes through the contracted batchers (which manage
COLOR2 writes); anything screen-space-2D goes after endScene.

---

## Appendix — why legacy MLR FX survived while the oracle vanished

Legacy MLR FX flow through the gos submission pipeline (`renderLists` /
`RenderNow` → `gos_RenderIndexedArray` → `applyRenderStates`), whose state and
output wiring were already reconciled with the MRT scene FBO during the RenderWorld
arc (M1.5/M6 gauntlet: `mlr_mech_draws=0`, contract scripts green). The tube oracle
was the one VFX path issuing raw immediate GL in the wrong phase with the inherited
3-buffer MRT list — both properties unique to it, both now corrected.
