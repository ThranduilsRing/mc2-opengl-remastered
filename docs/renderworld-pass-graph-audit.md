# RenderWorld Pass Graph Audit

> **Verified against code:** 2026-06-11, branch `claude/nifty-mendeleev` (worktree HEAD, terrain-chunk default-ON era).
> Line numbers grep-confirmed at write time; symbols are stable, lines drift.

Scope: the ACTUAL per-frame render passes — order, render target, viewport, depth state,
compositing path. "RenderWorld" the C++ subsystem (`RenderCore/RenderWorld*`) is an object-ID
*identity* layer, not a pass scheduler — there is **no render graph**; passes are invoked
imperatively from `gameosmain.cpp` → `GameCamera::render` (`code/gamecam.cpp:92`) →
`MC_TextureManager::renderLists` (`mclib/txmmgr.cpp:1953`) → `gosPostProcess` (`GameOS/gameos/gos_postprocess.cpp`).

---

## Frame skeleton (ordering diagram)

```
gameosmain.cpp draw_screen:
  pp->beginScene()                 bind sceneFBO_, MRT draw buffers      (gameosmain.cpp:527, gos_postprocess.cpp:930)
  glClearDepth(0) + glClear(C|D|S) reverse-Z clear                       (gameosmain.cpp:579-580)
  pp->clearGBuffer1()              attachment-1 sentinel (.5,.5,1,0)     (gameosmain.cpp:586)
  └─ GameCamera::render (gamecam.cpp:92)            == ALL scene passes into sceneFBO_
       1. HDRI skybox            GameAdapters::Sky::renderHdri          (gamecam.cpp:328 → gos_postprocess.cpp:2008)
       2. land->render()         terrain ENQUEUE + indirect arming      (gamecam.cpp:339)
       3. craterManager->render()  decal/footprint enqueue              (gamecam.cpp:345)
       4. ObjectManager->render()  object enqueue (Spine A) + batcher submits (gamecam.cpp:354)
       5. land->renderWater()    legacy water enqueue (no-op when fastpath owns) (gamecam.cpp:359)
       6. ObjectManager->renderShadows()  shadow-caster enqueue         (gamecam.cpp:365)
       7. Terrain::flushDrawCommands()  ◄ CHUNK TERRAIN DRAW (default-ON) (gamecam.cpp:389 → terrain.cpp:2435)
       8. mcTextureManager->renderLists()  ◄ THE BIG FLUSH (see table)  (gamecam.cpp:394 → txmmgr.cpp:1953)
       9. RenderWaterReflectionPass()  quarter-res RT (default-OFF)     (gamecam.cpp:404)
      10. land->renderWaterFastPath()  GPU water overlay                (gamecam.cpp:411)
      11. particles Batcher::Flush()   GPU billboards                   (gamecam.cpp:446)
      12. theClipper->RenderNow()      MLR immediate FX (Tube etc.)     (gamecam.cpp:463)
      13. weather->render(), DebugRenderer::flushWorldPrims()           (gamecam.cpp:471, 552)
  pp->endScene()                   post stack + COMPOSITE to FBO 0      (gameosmain.cpp:603, gos_postprocess.cpp:1767)
  VisualDiff / RdocCapture / projectz overlay (FBO 0)                   (gameosmain.cpp:609-622)
  gos_RendererFlushHUDBatch()      HUD replay to FBO 0                  (gameosmain.cpp:624)
  GuiRuntime::Render()             ImGui, LAST                          (gameosmain.cpp:628)
  SDL SwapWindow                   present                              (gameosmain.cpp:1593)
```

### Inside renderLists() (txmmgr.cpp:1953) — sub-pass order (Tracy zone names)

```
RenderLists.Preamble (1958) → LightDataUpload (2009) → Camera.SceneDataUpload (2050)
→ Render.3DObjects (2071)            ShapeRenderer loop, legacy Spine A solids — NOTE: runs BEFORE terrain
→ Shadow.StaticFullMapBuild (2165)   one-shot static 4096² shadow map (primed once per mission)
→ RenderLists.StaticPropRegistryFlush (2237)
→ RenderLists.DynamicShadowPass (2258)   dyn-prop + mech shadow into dynShadowFBO_
→ Render.TerrainSolid (2467)         chunk-suppressed (txmmgr.cpp:2511) / DrawIndirect / legacy fallback
→ Render.GpuStaticProps (2615)       gpu_cull::compute_dispatch (2659) + batcher MDI flush (2709)
→ Render.GpuMechs (2719)             GpuMechBatcher::flush
→ Render.TerrainMask.Solid (2733) → TerrainOverlays (2746) → OverlaysStatic (2759)
→ TerrainMines (2771) → Decals (2778) → Overlays (2785)
→ RenderLists.TerrainAlphaWaterLoops (2800) → Render.NoUnderlayer (2871)
→ RenderLists.ShadowBlobs (2936) → NonTerrainAlphaLoops (3006) → VfxHudSubmit (3069)
```

---

## Pass table

All scene passes target **`sceneFBO_`** (RGBA16F color0 + GBuffer1 normal at color1 + optional
R32UI objectID at color2 + depth; `gos_postprocess.h:195`), viewport `0,0,width_,height_`
(full drawable, `gos_postprocess.cpp:964`), **reverse-Z** (clearDepth 0, GL_GEQUAL), unless noted.
No scissor is used anywhere in the main frame.

| # | Pass | Owner (file:function) | Target | Depth buffer | Viewport | Blend / depth / cull | Inputs | Outputs | Consumed by | Legacy immediate draws allowed? |
|---|------|----------------------|--------|--------------|----------|----------------------|--------|---------|-------------|--------------------------------|
| 0 | Scene begin/clear | `gameosmain.cpp:527,579-586`; `gosPostProcess::beginScene` (`gos_postprocess.cpp:930`) | sceneFBO_ (MRT bound) | sceneFBO_ depth, cleared to 0 (reverse-Z) | full | n/a (clear) | prev-frame terrain flag (clear color choice, `gameosmain.cpp:571`) | cleared MRT + GBuffer1 sentinel + objectID=0 (`gos_postprocess.cpp:958-962`) | all scene passes | n/a |
| 1 | HDRI skybox | `GameAdapters::Sky::renderHdri` → `gosPostProcess::renderHdriSkybox` (`gamecam.cpp:328`, `gos_postprocess.cpp:2008`) | sceneFBO_ | no depth write (fullscreen background quad) | full | opaque, no depth | EXR equirect texture | color0 background | composite | no (engine-side GL) |
| 2 | **Chunk terrain solid (DEFAULT)** | `Terrain::flushDrawCommands` (`mclib/terrain.cpp:2435`) → `gos_TerrainLodChunk_SubmitDrawCommands` (`GameOS/gameos/gos_terrain_lod_chunk.cpp:443`) | sceneFBO_ | sceneFBO_ depth, WRITE | full (inherited) | **explicitly self-owned**: DEPTH_TEST on, depthMask TRUE, blend OFF, GL_GEQUAL, CULL_FACE OFF (double-sided) (`gos_terrain_lod_chunk.cpp:497-516`); restores on exit | height/type/cement SSBOs, colormap atlas (unit 0), baked dispatch MVP frame N-1 (`:458-461`), shadow maps | color0 + GBuffer1 + depth | water, particles, post, screen-shadow | no — runs BEFORE renderLists (gamecam.cpp:389), state saved/restored |
| 3 | Spine-A solids ("3DObjects") | `renderLists` ShapeRenderer loop (`mclib/txmmgr.cpp:2071-2130`) | sceneFBO_ | write | per-shape `rs->viewport_` via `shape_renderer.setup` (`txmmgr.cpp:2119`) | gos state machine (ZCompare/ZWrite via gos_SetRenderState) | masterHardwareVertexNodes (vehicles, CPU mechs, animated bldgs, MLR FX shapes); lights SSBO, scene UBO | color0+GBuffer1+depth | post | YES — this IS the legacy enqueue-flush lane; shader `gos_tex_vertex_lighted` |
| 4 | Static shadow map (one-shot) | `Shadow.StaticFullMapBuild` (`txmmgr.cpp:2165`); state via `beginShadowPass` (`gos_postprocess.cpp:2187`) | **shadowFBO_** (depth-only) | shadow depth 4096² (`gos_postprocess.cpp:2118`), **forward-Z** clearDepth 1, GL_LESS | 0,0,4096,4096 (`:2193`) | colorMask OFF, polygon-offset, cull OFF (`:2205-2214`) | terrain patches + static buildings (`getBuildingShadowInstances`, txmmgr.cpp:2208, gate MC2_STATIC_PROP_BUILDING_SHADOW default-OFF) | static shadow depth tex | `calcShadow()` in all lit frags (`shaders/include/shadow.hglsl`) | no |
| 5 | Dynamic shadow map (per-frame) | `RenderLists.DynamicShadowPass` (`txmmgr.cpp:2258-2458`): `drawDynamicPropShadows` (2453), `flushShadow` props (2456), `GpuMechBatcher::flushShadow` (2458) | **dynShadowFBO_** 4096² (`gos_postprocess.cpp:2448`) | dyn shadow depth, forward-Z | 0,0,4096,4096 | colorMask OFF, polygon-offset, cull OFF | dirty-cached dyn-prop instances + mech bones; frustum-fit light matrix | dyn shadow depth tex | `calcDynamicShadow()` in lit frags | no |
| 6 | Indirect terrain solid (suppressed under chunk) | `Render.TerrainSolid` (`txmmgr.cpp:2467`); `gos_terrain_indirect::DrawIndirect` | sceneFBO_ | write, GEQUAL | full | gos states ("no special depth state for DRAWSOLID", txmmgr.cpp:2464) | recipe SSBO + `gpu_driven_terrain_solid.comp` cull | color0+depth | post | When chunk gate ON (default): `modernHandled=true`, **DrawIndirect suppressed** (`txmmgr.cpp:2511-2516`); legacy masterVertexNodes terrain skipped (`:2543`). Opt-out `MC2_TERRAIN_LOD_CHUNK=0` restores it. |
| 7 | GPU static props | `Render.GpuStaticProps` (`txmmgr.cpp:2615`): `gpu_cull::compute_dispatch` (`:2659`) then `GpuStaticPropBatcher::flush` (`:2709` → `gos_static_prop_batcher.cpp` MDI ×2 alpha-OFF/ON) | sceneFBO_ | write | full | opaque + alpha-test group; depth GEQUAL | instance SSBO, frozen cull records, texture arrays, MaterialGpu SSBO | color0+GBuffer1+objectID (StaticProp kind) | post, RenderWorld pick | no |
| 8 | GPU mechs | `Render.GpuMechs` (`txmmgr.cpp:2719`): `GpuMechBatcher::flush` (`GameOS/gameos/gos_mech_batcher.cpp`) | sceneFBO_ | write | full | MechOpaque PipelineDesc; invalidates state cache after | bone/instance/material SSBOs, ViewUniforms UBO | color0+GBuffer1+objectID (Mech kind) | post, RenderWorld pick | no |
| 9 | Terrain overlays/mines/decals/alpha loops | `txmmgr.cpp:2733-3067` zone series | sceneFBO_ | mostly depth-test, no write (alpha) | full | gos alpha states per node flags | masterVertexNodes overlay/alpha lists | color0 | post | YES — pure legacy enqueue-flush (gos_RenderIndexedArray) |
| 10 | VFX/HUD legacy submit | `RenderLists.VfxHudSubmit` (`txmmgr.cpp:3069`) | sceneFBO_ | ZCompare 1, ZWrite 0 (`:3077,3090`) | full | gos_Alpha_OneOne additive | MC2_ISEFFECTS vertex nodes (MLR-fed) | color0 | post | YES (legacy lane) |
| 11 | Water reflection RT | `gos_terrain_indirect::RenderWaterReflectionPass` (`gamecam.cpp:404`) | **waterReflFBO_** quarter-res RGBA16F + DEPTH24 (`gos_postprocess.cpp:692-711`) | own depth | w/4 × h/4 | opaque mirrored terrain | terrain recipes, mirrored MVP | reflection tex | water frag (gated) | default-OFF (`MC2_WATER_REFLECTION_RT`) |
| 12 | Water fast path | `land->renderWaterFastPath` (`gamecam.cpp:411`) → `gosRenderer::renderWaterFastPath` (`GameOS/gameos/gameos_graphics.cpp`, grep `renderWaterFastPath`) | sceneFBO_ | depth TEST GEQUAL, **depthMask OFF** | full | SRC_ALPHA blend, CULL OFF — self-managed raw GL; calls `gos_InvalidateRenderStateCache()` after (`gamecam.cpp:417`) | WaterThinRecord SSBO from `gpu_driven_water.comp`, scene depth | color0 (transparent overlay) | post | no (raw GL bypass + explicit cache invalidate) |
| 13 | GPU particles | `mc2::particles::Batcher::Flush` (`gamecam.cpp:446` → `gos_particle_bridge.cpp`) | sceneFBO_ | depth test, no write | full | per-group additive GL_ONE / alpha; invalidates state cache | particle SSBO (binding 14), scene depth (soft particles gated) | color0 | post | no |
| 14 | MLR immediate FX | `theClipper->RenderNow()` (`gamecam.cpp:463`) | sceneFBO_ | gos states | full | additive/alpha via gos states | MLR clipper (Tube/Shape/DebrisCloud CPU FX) | color0 | post | **YES — the one true immediate-draw exception** (render-pipeline-map.md "MLR path") |
| 15 | Weather + debug prims | `weather->render()` (`gamecam.cpp:471`); `DebugRenderer::flushWorldPrims` (`gamecam.cpp:552`) | sceneFBO_ | depth-tested | full | line/alpha | — | color0 | post | weather = legacy lane; debug = engine GL, gated `MC2_DEBUG_RENDERER` |
| 16 | Post stack (in order) | `gosPostProcess::endScene` (`gos_postprocess.cpp:1767`): `runHzbReduce/Probe` (1779-1781, gated), `runScreenShadow` (1785), `runShoreline` (1788), `runGodRays` (1791), `runSSAO` (1794, default-OFF), `runBloom` (1796) | ping-pong FBOs (bloomFBO_[2], ssaoFBO_, godrayFBO_; `gos_postprocess.h:203-288`) then back into scene color | none (fullscreen quads) | per-FBO size | no depth, no blend | sceneColorTex_, scene depth, GBuffer1, objectID | modified scene color / bloom tex | composite | no |
| 17 | **Composite** | `endScene` tail (`gos_postprocess.cpp:1799-1900`) | **FBO 0 (backbuffer)** | none — DEPTH_TEST off, depthMask FALSE (`:1804-1806`) | full drawable (`:1801`) | **blend force-OFF** (`:1813` — additive-leak fix), cull off | sceneColorTex_ (unit 0), bloom (unit 1), objectID (unit 2, debug view modes), exposure/FXAA/tonemap uniforms | final LDR backbuffer | HUD/ImGui draw on top; SwapWindow | no; invalidates render-state cache after (`:1916`) |
| 18 | HUD replay | `gos_RendererFlushHUDBatch` (`gameosmain.cpp:624`) | FBO 0 | z=HUD_DEPTH 0.9999 (reverse-Z near) | full | alpha blend | deferred hudBatch_ quads (gos_State_IsHUD) | backbuffer | present | replay of legacy gos_DrawQuads — deferred, not immediate |
| 19 | ImGui | `GuiRuntime::Render` (`gameosmain.cpp:628`) | FBO 0 | none | full | ImGui backend | — | backbuffer | present | no |
| 20 | Present | `graphics::swap_window` (`gameosmain.cpp:1593`) | window | — | — | — | backbuffer | screen | — | — |

**Compositing path summary:** everything 3D renders offscreen into `sceneFBO_` (HDR RGBA16F MRT);
`endScene()` runs the post chain on offscreen ping-pongs, then a single fullscreen composite quad
writes FBO 0; HUD/ImGui draw directly on FBO 0 afterward; there is **no glBlitFramebuffer** in the
main path — composite is always the shader quad. Headless screenshots read `sceneFBO_` not FBO 0
(`gameosmain.cpp:1555-1575`).

---

## Key facts the prior docs don't capture

1. **Terrain default flipped to the chunk path** (`mc2TerrainLodChunkEnabled()` default-ON,
   `mclib/terrain.cpp:135`, cutover `a7b090be` 2026-06-09). The chunk draw happens in
   `GameCamera::render` *before* `renderLists()` (`gamecam.cpp:389`), and `Render.TerrainSolid`
   inside renderLists sets `modernHandled=true` to suppress both `DrawIndirect()` and the legacy
   masterVertexNodes terrain loop (`txmmgr.cpp:2511-2516`, `:2543`).
2. **Draw order quirk:** Spine-A objects (`Render.3DObjects`, txmmgr.cpp:2071) draw **before**
   terrain solid in renderLists — correct only because depth testing resolves order; but chunk
   terrain draws even earlier (pre-renderLists).
3. The chunk submit is the only scene pass that **explicitly owns its full depth/blend/cull state
   and restores it** (`gos_terrain_lod_chunk.cpp:493-516`) — the 10.3 transparency-saga lesson.
   Water fastpath, mech batcher, particle bridge, and post all instead mutate raw GL and call
   `gos_InvalidateRenderStateCache()`.
4. Shadow maps are **forward-Z islands** (clearDepth 1 / GL_LESS, `gos_postprocess.cpp:2196-2203`)
   inside a reverse-Z frame; `beginShadowPass` saves/restores the viewport.
5. The objectID attachment (color2, R32UI) is cleared in `beginScene` (`gos_postprocess.cpp:958-962`)
   and written only by static-prop + mech shaders (RenderWorld M1/M2); terrain/VFX writers are
   reserved/prohibited per the arc DECISIONS (renderworld_arc_status.md).

## Staleness assessment of prior docs

| Doc | Last touched | Verdict |
|---|---|---|
| `render-pipeline-map.md` | 2026-06-10 (`102e0da1`) | **Mostly current** — best single map. BUT §1 Terrain is **STALE**: describes GPU-indirect `DrawIndirect()` as the default; the chunk LOD path is default-ON since 2026-06-09 and suppresses DrawIndirect. Its frame-order list also omits `Terrain::flushDrawCommands` (gamecam.cpp:389) and `RenderWaterReflectionPass` (gamecam.cpp:404). Update §1 + the order list there. |
| `renderpass-contract-spec.md` | 2026-05-27 | Current as a spec for the *descriptive* `RenderCore/RenderPassContract.h` registry (5 lanes). Not a pass graph; predates terrain-chunk cutover (its Terrain owner "TerrainPatchStream" is no longer the default draw owner). |
| `renderworld_arc_status.md` | 2026-05-24 | Current for its scope (object-ID identity arc, steady state). Says nothing about passes/targets — not stale, just orthogonal. |
| `renderworld_migration_guide.md` | 2026-05-24 | Contributor onboarding for the identity arc; same as above. Pre-chunk-cutover but its content doesn't claim terrain draw ownership. |
| `render-contract.md` | 2026-05-15 | **Partially stale** — submission-space contracts predate chunk-terrain default-on and the 06-09/06-10 water/decal MVP-frame meta-fixes. Use for lane vocabulary, not for current default routing. |

## Open questions

1. **Editor lane:** `MC2_IS_EDITOR` builds use the same chunk path? (Memory says editor "runs default-on modern chain"; this audit verified game lane only.) Verify `Terrain::flushDrawCommands` cardinality in the editor frame loop.
2. **Render.TerrainMask.Solid / overlays under chunk mode** (txmmgr.cpp:2733+): these legacy alpha lanes still depend on masterVertexNodes fed by `quad.cpp::setupTextures`, which is editor-gated/`#ifdef`-trimmed per the 8z closeout — confirm which overlay sub-lanes still emit in game builds at runtime (mine/decal counts on a tier1 run).
3. **`gos_terrain_surface_bridge_draw()`** (txmmgr.cpp:2536) — additive validation lane, default-OFF (`MC2_TERRAIN_SURFACE`); candidate for deletion now that chunk path shipped?
4. **Pass-contract registry drift:** `RenderCore/RenderPassContract.h` row for Terrain still names TerrainPatchStream as owner — should be updated to the chunk driver (or grow a second terrain row).
5. **Water reflection pass** runs unconditionally as a call site (gamecam.cpp:404) but is env-gated inside; if `MC2_WATER_REFLECTION_RT` ever defaults on, its quarter-res FBO switch must restore terrain MVP (claims it does — unverified here).
