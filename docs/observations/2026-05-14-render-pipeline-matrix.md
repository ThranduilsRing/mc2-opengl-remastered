# Render Pipeline Matrix

> **Staleness (2026-06-04):** Status column predates the June perf closeout. Known drift: water fast-path is now DEFAULT-ON (WATER-GPU-FULL-RECIPE-AUTHORITATIVE), quadSetupTextures RETIRED, static-prop substrate snapshot-owned (live-builder retired). Re-validate the Status column against the June ship-log in the convergence roadmap before quoting. Cell symbols (file:line) remain grep-to-confirm as always.

For each thing-type the engine renders, this document maps where it lives at each of the 4 stages of the pipeline. Use this to answer "what does X touch?" in 30 seconds instead of 30 minutes of grep.

Operating principle behind this artifact: every cell is a NAMED contract surface that was previously implicit. See `memory/named_contracts_at_intersections.md`.

All file:line citations were grep-verified on 2026-05-14 against this worktree (`nifty-mendeleev`). Symbols are stable; line numbers drift. Always grep the symbol when precision matters.

## How to read this doc

- **Columns are stages** — what work runs on the CPU at vertex-projection time, how the work gets queued, when it hits the GPU, and which shader runs. A single thing-type may have multiple rows when variants coexist (e.g. terrain has solid / overlay / mine / cement-bake).
- **Cells cite a starting-point symbol**, not a line range. The Rule 0 grep-before-citing discipline (`memory/brainstorm_code_grounding_lesson.md`) applies — grep the symbol to find current code.
- **Status column is load-bearing.** LEGACY vs DEFAULT vs SHIPPED-OPTIONAL vs IN-FLIGHT vs DESIGN vs GAP. Future decisions key on this.
- **Cross-cutting concerns** (lighting, cull, shadow, state cache, texture cache) are bands across rows — captured in their own section, not duplicated per row.
- **Pattern families** at the bottom capture named-contract shapes new work matches against (producer/consumer frame-stamp, pause-gated state, dual-output wrapper, render-state cache invalidation).

## The 4 stages

**Stage 1 — Vertex projection (CPU).** Per-thing CPU work to compute or update vertex positions, world transforms, light indices, fog values, per-corner texture coordinates. For mechs/vehicles/buildings: `*Appearance::update()` -> `TransformMultiShape`. For terrain: `Terrain::geometry()` -> per-vertex `projectZ()` -> per-quad `setupTextures()`. For fast-path overlay-style draws (water fast, indirect-terrain): a static recipe built at mission load + a per-frame thin record packer.

**Stage 2 — Enqueue (CPU).** Getting the projected data into one of two master arrays inside `MC_TextureManager` (`mclib/txmmgr.cpp`): `masterVertexNodes` (legacy flat `gos_VERTEX` stream) or `masterHardwareVertexNodes` (modern `TG_RenderShape` with UBO bindings, walked by `ShapeRenderer`). New fast paths bypass both and queue work into their own per-pass thin-record/indirect-cmd SSBO buffers. Per `memory/render_functions_are_enqueuers_not_submitters.md`: functions named `render`/`draw` here are ENQUEUERS, not submitters. The MLR path (`mclib/mlr/`) is the immediate-draw exception.

**Stage 3 — Upload (GPU buffer upload).** Mostly coalesced inside `MC_TextureManager::renderLists()` (mclib/txmmgr.cpp:1331). Uploads light UBO + scene UBO + per-shape VB/IB/UBO bindings. Fast paths upload their own thin-record SSBO at hook time. Indirect/compute paths run `glDispatchCompute` here.

**Stage 4 — GPU execution.** The final `glDraw*` / `glMultiDrawElementsIndirect`. Triggered by `gos_RendererEndFrame` (GameOS/gameos/gameos_graphics.cpp:4813) -> `mcTextureManager->renderLists()`. Fast paths hook AFTER `renderLists()` from `code/gamecam.cpp:245` per `memory/render_order_post_renderlists_hook.md`. Shaders live in `shaders/`.

## Status legend

- **LEGACY** — on `masterVertexNodes` flat-vertex queue path; targeted for retirement per `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`
- **DEFAULT** — currently-default modern path on this worktree
- **SHIPPED-OPTIONAL** — code shipped + working, behind env var or killswitch; not yet default-on
- **IN-FLIGHT** — active work; not stable
- **DESIGN** — design phase only; no code yet (or scaffolding only)
- **GAP** — no advisor / doc / memory owns this cell; research opportunity

## The matrix

| Thing | Vertex projection | Enqueue | Upload | GPU execution | Status | Notes |
|---|---|---|---|---|---|---|
| Terrain solid (tessellated) | `Terrain::geometry` -> `projectZ` -> `setupTextures` (mclib/terrain.cpp:982 enqueue site; geometry/setupTextures earlier in file) | `TerrainQuad::draw` (mclib/quad.cpp:1932) -> `masterVertexNodes` flagged `MC2_DRAWSOLID|MC2_ISTERRAIN` | Light/scene UBO in `renderLists()`; per-batch `gos_RenderIndexedArray` upload | `shaders/gos_terrain.vert/.tesc/.tese/.frag` (tessellated splat/POM) inside `renderLists` terrain main pass (`mclib/txmmgr.cpp:1438` `MC2_ISTERRAIN` branch) | DEFAULT (LEGACY queue) | Tessellation control units own per-vertex GPU projection via `terrainMVP`; CPU still produces `pz` used as visibility/cull side-data (debt per `render-contract.md` D1). |
| Terrain overlay (cement / transitions) | Per-quad CPU classifier in `TerrainQuad::draw` decides overlay emit; data pushed via `pushOverlayTri` / `pushDecalTri` API at `gameos_graphics.cpp:6259` | World-overlay batch ring inside `gosRenderer::terrainOverlayBatch_` (`gameos_graphics.cpp:1766`) | `drawTerrainOverlays` builds VBO at draw time (gameos_graphics.cpp:6134) | `shaders/terrain_overlay.vert` + `terrain_overlay.frag`; called from `gameos_graphics.cpp:6263` after `renderLists` flush | SHIPPED-OPTIONAL | `gos_InvalidateRenderStateCache` called at end (gameos_graphics.cpp:6185). Future state: world-space typed overlay batches per `render-contract.md` A3. |
| Terrain decals (craters footprint / cement perimeter dynamic) | Same as overlay path; classified per-tri | `decalBatch_` ring (gameos_graphics.cpp:1766) | `drawDecals` (gameos_graphics.cpp:6192) | `shaders/terrain_overlay.vert` + `decal.frag` (compiled at `gameos_graphics.cpp:2863`); called from `:6267` | SHIPPED-OPTIONAL | `gos_InvalidateRenderStateCache` at `:6245`. Decals share VS with overlays; only the FS differs. |
| Terrain mine (static-bake variant) | Per-mission bake at mission load; `MarkMineDirty` (`gos_terrain_indirect.cpp:1995`) flags rebuild | Static SSBO + indirect-cmd stream in `gos_terrain_indirect.cpp` | Built once per dirty-cycle; resident across frames | `shaders/gos_terrain_mine_static.vert/.frag` | SHIPPED-OPTIONAL | PR2c MINE static-bake default-on per `docs/render-perf-snapshot.md`. Counters: `Counters_AddIndirectMineDrawnCells` (gos_terrain_indirect.cpp:117). |
| Terrain mine (legacy per-frame) | Per-quad in `TerrainQuad::drawMine` (mclib/quad.cpp:4400) | `masterVertexNodes` | `renderLists` flush | `gos_terrain.frag` | LEGACY | Counters `Counters_AddLegacyMineEnqueueQuad` / `Counters_AddLegacyMineDrawQuad` (gos_terrain_indirect.cpp:115-116) — kept for parity / fallback. |
| Terrain indirect SOLID (recipe-driven) | Static `TerrainQuadRecipe` SSBO built at mission load (`gos_terrain_indirect.cpp`); per-frame thin-record packer indexed by `vertexNum` | Per-frame thin-record SSBO + `glMultiDrawElementsIndirect` cmd list | Built inline; `Counters_AddIndirectSolidPackedQuad` (gos_terrain_indirect.cpp:113) | Tessellated solid shader path (consumes recipe from SSBO) | SHIPPED-OPTIONAL | Env: `MC2_TERRAIN_INDIRECT` (gos_terrain_indirect.cpp:50). Defer questions on cement atlas / thin records / PR1/PR2 history to `mc2-terrain-indirect-expert`. Note: planned compute shader `gpu_driven_terrain_solid.comp` does not yet exist as a file in shaders/ — GAP marker. |
| Water (legacy) | `TerrainQuad::drawWater` (mclib/quad.cpp:2988) — projected vertices per quad | `masterVertexNodes` flagged `MC2_DRAWALPHA` | `renderLists` alpha pass | `gos_tex_vertex.vert` + `gos_tex_vertex.frag` (projected-space — Bucket B1 per `render-contract.md`) | LEGACY | `Terrain::renderWater` (mclib/terrain.cpp:1075) early-returns when fast path active (line 1109). |
| Water (fast path) | Static recipe SSBO + per-frame thin record (`gos_terrain_water_stream.cpp`) | Thin-record SSBO ring | One `glBufferSubData` per frame | `shaders/gos_terrain_water_fast.vert` paired with `gos_tex_vertex.frag` — one `glDrawArrays` from `Terrain::renderWaterFastPath` (mclib/terrain.cpp:1200) called at gamecam.cpp:256 AFTER `renderLists` | DEFAULT (post-renderLists hook) | Pattern template for indirect-terrain endpoint. Env: `MC2_RENDER_WATER_PARITY_CHECK=1` for parity audit. Per `memory/water_ssbo_pattern.md`. |
| Mechs (CPU baseline) | `Mech3DAppearance::update` (mclib/mech3d.cpp:4096) -> `TransformMultiShape`; gated by `inView` per `cull_gates_are_load_bearing.md` | `Mech3DAppearance::render` (mech3d.cpp:2440) -> `masterHardwareVertexNodes` (modern queue) | UBO + light UBO + scene UBO via `renderLists` (mclib/txmmgr.cpp:1331) shape pass | `ShapeRenderer::render` in shape pass loop (mclib/txmmgr.cpp:1459); `shaders/mech.vert/.frag` | DEFAULT | Per-instance `cachedFrame_` / `cachedGpuLightIndex_` stamp at `mclib/msl.h:276,286` — the black-tree-bug fix per `memory/black_tree_bug_investigation_state.md`. |
| Mechs (Track D — GPU batcher) | `GpuMechBatcher::submitActor` (GameOS/gameos/gos_mech_batcher.cpp:567); per-LOD `registerTypeLod` at `:334` | `GpuMechBatcher::flush` (gos_mech_batcher.cpp:636) | Batched VB + per-actor instance data SSBO | TBD shader path (Slice A) | SHIPPED-OPTIONAL | Slice A parity sign-off 2026-05-08 per `docs/render-perf-snapshot.md`. `flushShadow` is no-op in Slice A (gos_mech_batcher.cpp:329). Defer to `mc2-mech-update-geometry-expert`. |
| Vehicles | `GVAppearance::update` -> `TransformMultiShape`; `GroundVehicle::update` (code/gvehicl.cpp:3156) is game-side AI/movement, not render | `GVAppearance::render` -> `masterHardwareVertexNodes` | Shape pass in `renderLists` | `shaders/mech.vert/.frag` (shared with mechs) | DEFAULT | Per `memory/cull_gates_are_load_bearing.md` gvactor.cpp:2773 `inView` gate around `updateGeometry()`. `Vehicles AppearanceUpdate` ~0.3-0.4 ms per `render-perf-snapshot.md`. |
| Buildings (animated) | `BldgAppearance::update` (mclib/bdactor.cpp:2131) | `BldgAppearance::render` (bdactor.cpp:1606) -> `masterHardwareVertexNodes` | Shape pass | `mech.vert/.frag` shape path | DEFAULT | LOD swap unsafe for animated types per `memory/bldg_animation_lod_swap_unsafe.md`. `TG_AnimateShape` caches LOD-0 node->index. |
| Static props (LEGACY) | `TG_MultiShape` per-frame transform via owner appearance `update()` | `masterHardwareVertexNodes` per type | Shape pass in `renderLists` | `mech.vert/.frag` | LEGACY | Path under `g_useGpuStaticProps=false`. On nifty-mendeleev: substrate=OFF default renders zero static props (`memory/substrate_off_renders_no_static_props.md`). |
| Static props (registry / substrate) | `BldgAppearance::registerStatic` (bdactor.cpp:2805); `BldgAppearance::touch` (`:2912`) under `MC2_STATIC_UPDATE_SKIP=1`; per-frame `frameBegin` reset clears live list | `GpuStaticPropRegistry::markVisible(regIdx, lightDataIndex)` (gos_static_prop_registry.cpp:278); registration via `registerStaticProp` (`:160`) | `GpuStaticPropRegistry::flush` (gos_static_prop_registry.cpp:310) called from inside `renderLists` (mclib/txmmgr.cpp:1784 comment) | `static_prop.vert/.frag`; per-bucket draws | SHIPPED-OPTIONAL (default-on for substrate-coalesce) | Env: `MC2_GPU_CULL_SUBSTRATE` default ON (gameosmain.cpp:734). Per `memory/substrate_coalesce_armed_multi_packet_limitation.md`. `GpuStaticPropRegistry::frameBegin` called from gamecam.cpp:201 (Stage 3.C). |
| Static props (substrate-coalesce / compute-cull) | Same as registry path; substrate ring slot append in `substrate_appendStaticPropRecord` (gpu_cull_substrate.cpp:298) | `gpu_cull::substrate_frameBegin` (gpu_cull_substrate.cpp:182) — CURRENTLY CALLED FROM `objmgr.cpp:1933` inside `GameObjectManager::update` (NOT yet hoisted on this worktree; see Pause-gated pattern below) | `GpuStaticPropBatcher::flush` (gos_static_prop_batcher.cpp:2714); compute dispatch in `gpu_cull::compute_dispatch` (gpu_cull_compute.cpp:786) | `static_prop.vert/.frag` via `glMultiDrawElementsIndirect` per packet | DEFAULT (substrate-coalesce armed) | Coalesce-armed default-on as of 7b9ad5f per memory. Sync-stall lesson: `glGetBufferSubData` after `glCopyBufferSubData` causes implicit GPU sync — `memory/substrate_coalesce_sync_point_lesson.md`. |
| Craters | `craterManager->update()` (mclib/crater.cpp) | `CraterManager::render` (mclib/crater.cpp:299) -> `masterVertexNodes` flagged `MC2_ISCRATERS` | `renderLists` crater pass | `gos_vertex.vert/.frag` (legacy flat stream) | LEGACY | Called from gamecam.cpp:208 (and also mclib/camera.cpp:1794 — verify which is active on cinematics). Crater pass walks `MC2_ISCRATERS` nodes last. Target migration: typed world-space overlay batch per `render-contract.md` A3. |
| Decals (legacy crater/footprint stream) | Same as craters | Through crater path or pushDecalTri overlay path | See decals row above | `decal.frag` | LEGACY (migrating to overlay batch) | The legacy crater stream is the old API; new code uses `gos_pushDecalTri` (gameos_graphics.cpp:6259). Coexists. |
| Shadow caster (terrain) | Re-run terrain VS in shadow space | Shadow depth pass inside `renderLists` walks `MC2_DRAWSOLID|MC2_ISTERRAIN` nodes again | Shadow FBO bound at `gos_postprocess.cpp:607` (early-return if `!shadowsEnabled_`) | `shaders/shadow_terrain.vert/.tesc/.tese/.frag` | DEFAULT | `gosPostProcess::shadowsEnabled_` (gos_postprocess.cpp:59). `shadowDepthProg_` at `:1081`. |
| Shadow caster (TG_Shape mechs/buildings) | `Mech3DAppearance::renderShadows` (mech3d.cpp:3051), `BldgAppearance::renderShadows` (bdactor.cpp:2112), `TreeAppearance::renderShadows` (bdactor.cpp:4703) | Shadow depth pass walks shape nodes flagged for shadow casting | Shadow FBO | `shaders/shadow_object.vert/.frag` (and shadow_depth.vert/.frag) | DEFAULT | Eligibility gate excludes `firstTextureAlpha` (suppresses fences/gates without trees) — `memory/shadow_caster_eligibility_gate.md`. Commit 743efd6 misdiagnosed; alpha-exclusion is correct fix. |
| Shadow consumer | n/a (samples shadow map in main pass) | n/a | Shadow depth tex bound at `gos_postprocess.cpp:658` before scene | `shaders/include/shadow.hglsl` `calcShadow()` (variable-tap Poisson PCF) sampled inside `gos_terrain.frag`, `mech.frag`, `static_prop.frag` | DEFAULT | Terrain shader must get FLAT geometric normal into `calcShadow` per `memory/terrain_lighting_range_ceiling.md`. |
| Grass | Derived from terrain vertices | Geometry shader expansion | Inherits terrain world-space submission | `shaders/gos_grass.geom` + `gos_grass.frag` | DEFAULT | Aligned with terrain world-space contract — `render-contract.md` A2. |
| Skybox | Procedural | n/a — no master-array enqueue | n/a | `shaders/skybox.vert/.frag` called from `pp->renderSkybox` in `draw_screen` (gameosmain.cpp:141) BEFORE main pass | DEFAULT | A skybox actor (`Cylinder01`) still exists in late-register log spam but draws nothing — vestigial per `memory/skybox_actor_vestigial_post_terrain_gpu.md`. |
| HUD / UI / text | Screen-space at widget-tree time | UI submission code (gui/) | Per-call upload | `shaders/gos_text.vert/.frag` + UI shaders | DEFAULT | Bucket C1 in render-contract.md. Screen-space authoritative; do not migrate. |
| Particle FX (gosfx) | `gosfx::Effect::Animate` etc. | `masterVertexNodes` (legacy stream) | renderLists alpha pass | `gos_vertex_lighted.vert/.frag` | LEGACY | Container at `mclib/gosfx/`. Particles never modernized; allowed legacy containment per `render-contract.md`. |
| MLR-appearance objects (immediate draw) | `ObjectAppearance` derived paths under `mclib/mlr/` | NONE — MLR path bypasses both master queues and draws immediately | n/a | Per-MLR-class FFP-style shader | LEGACY EXCEPTION | `ObjectManager->render()` called at gamecam.cpp:213 (verify; line not directly grepped here — see render_functions_are_enqueuers_not_submitters memory). The single naming exception to "render() enqueues". |
| Post-process scene | n/a | n/a | HDR scene FBO bound by `pp->beginScene` | `shaders/postprocess.vert/.frag`, `bloom_threshold.frag`, `bloom_blur.frag`, `ssao.frag`, `ssao_apply.frag`, `godray.frag`, FXAA, tonemap | DEFAULT | Composite path in `draw_screen` (gameosmain.cpp:141). |

## Per-thing rundowns (complex cases)

### Terrain

The most-variant thing in the matrix. Four to five coexisting variants.

- **Solid (tessellated)** — `mclib/terrain.cpp:982` `Terrain::render`, enqueues into `masterVertexNodes` via `TerrainQuad::draw` (`mclib/quad.cpp:1932`). Shader: `shaders/gos_terrain.{vert,tesc,tese,frag}`. World-space submission; CPU still produces `pz` (debt — Bucket D1 in `docs/render-contract.md`).
- **Overlay (cement / transitions / runway)** — gosRenderer overlay batch (`gameos_graphics.cpp:6134` `drawTerrainOverlays`). Pushed via `gos_pushOverlayTri`. Shader: `terrain_overlay.vert` + `terrain_overlay.frag`. Called after `renderLists`. `gos_InvalidateRenderStateCache` at end.
- **Decals (craters/footprints/dynamic)** — gosRenderer decal batch (`gameos_graphics.cpp:6192` `drawDecals`). Shares VS with overlays. Shader: `terrain_overlay.vert` + `decal.frag`. Compiled at gameos_graphics.cpp:2863.
- **Mine (static-bake)** — Per-mission rebuild driven by `gos_terrain_indirect::MarkMineDirty` (`gos_terrain_indirect.cpp:1995`). Shader: `gos_terrain_mine_static.{vert,frag}`. Default-on (PR2c).
- **Mine (legacy)** — `TerrainQuad::drawMine` (`mclib/quad.cpp:4400`). Kept for parity / fallback path.
- **Indirect SOLID (recipe)** — `gos_terrain_indirect.cpp` — static recipe SSBO + per-frame thin record + `glMultiDrawElementsIndirect`. Env `MC2_TERRAIN_INDIRECT` (`:50`). Counters: `Counters_AddIndirectSolidPackedQuad` (`:113`). Defer detail questions to `mc2-terrain-indirect-expert`.

Invariants:
- `quadList` is camera-windowed — rebuilt every frame. Static SSBOs must index by `vertexNum = mapY * realVerticesMapSide + mapX`, not by quadList slot. Per `memory/quadlist_is_camera_windowed.md`.
- `terrainMVP` uploaded with `GL_FALSE` + row-major; the gamecam.cpp comment saying `GL_TRUE` is wrong (`memory/terrain_mvp_gl_false.md`).
- `Terrain::vertexProjectLoop` slice is asymptotic (D1 hoist shipped 2026-04-30); per-vertex projectZ remains in CPU loop at `mclib/terrain.cpp:1351`, `:1425`, `:1566`.

### Water

- **Legacy** — `Terrain::renderWater` (`mclib/terrain.cpp:1075`) enqueues into `masterVertexNodes` via `TerrainQuad::drawWater` (`mclib/quad.cpp:2988`). Legacy is now an early-return shell when fast path is active (`mclib/terrain.cpp:1109`). Shader: `gos_tex_vertex.{vert,frag}`. Projected-space (Bucket B1).
- **Fast path** — `Terrain::renderWaterFastPath` (`mclib/terrain.cpp:1200`) called from `code/gamecam.cpp:256` AFTER `mcTextureManager->renderLists()`. Single `glDrawArrays`. Shader: `gos_terrain_water_fast.vert` + `gos_tex_vertex.frag`. Recipe + thin-record machinery in `GameOS/gameos/gos_terrain_water_stream.cpp`.

Pattern template for new GPU-direct overlay/terrain offloads: per `memory/water_ssbo_pattern.md`.

### Mechs

- **CPU baseline (DEFAULT)** — `Mech3DAppearance::update` (`mclib/mech3d.cpp:4096`) feeds `TransformMultiShape`; render at `:2440` enqueues into `masterHardwareVertexNodes`. Shadows at `:3051`.
- **Track D (Slice A)** — `GpuMechBatcher` in `GameOS/gameos/gos_mech_batcher.cpp` — `registerTypeLod` (`:334`), `submitActor` (`:567`), `flush` (`:636`). `flushShadow` no-op in Slice A (`:329`). Parity sign-off 2026-05-08.

Invariants:
- Per-mech `cachedGpuLightIndex_` / `cachedFrame_` stamp on `TG_MultiShape` (`mclib/msl.h:276,286`) is the black-tree-bug fix — see `memory/black_tree_bug_investigation_state.md`.
- `inView` cull gates `updateGeometry()`. Bypass cascades into stale matrices / lifecycle destruction — `memory/cull_gates_are_load_bearing.md`.
- `TG_Shape::init()` must reset `s_listOfLights`/`s_numLights` together — `memory/tg_shape_static_state_lifecycle_trap.md`.

Routing: `mc2-mech-update-geometry-expert`, `mc2-mech-skeletal-anim-expert`, `mc2-mech-import-expert`.

### Static props (highest variant complexity)

Three coexisting paths.

- **LEGACY** — TG_Shape path through `masterHardwareVertexNodes`. Path under `g_useGpuStaticProps=false`. On nifty-mendeleev, substrate=OFF renders zero static props — `memory/substrate_off_renders_no_static_props.md`. Tier1 PASS under substrate=OFF is a false-positive on visual correctness.
- **Registry (Track B)** — `GpuStaticPropRegistry` in `GameOS/gameos/gos_static_prop_registry.cpp`. Per-frame: `frameBegin()` (`:225`) clears live list; `markVisible(regIdx, lightDataIndex)` (`:278`) populates it during shape render; `flush()` (`:310`) emits draws inside `renderLists` (per comment in `mclib/txmmgr.cpp:1784`). Per `memory/track_b_widen_static_prop_registry.md`.
- **Substrate-coalesce (Track C)** — `gpu_cull::substrate_*` in `GameOS/gameos/gpu_cull_substrate.cpp`. Ring slot append in `substrate_appendStaticPropRecord` (`:298`), per-frame reset in `substrate_frameBegin` (`:182`). `GpuStaticPropBatcher::flush` (`gos_static_prop_batcher.cpp:2714`); compute dispatch `gpu_cull::compute_dispatch` (`gpu_cull_compute.cpp:786`). Default ON via `MC2_GPU_CULL_SUBSTRATE` (gameosmain.cpp:734). Per `memory/substrate_coalesce_armed_multi_packet_limitation.md`, `memory/track_c_compute_cull.md`.

Critical invariant the matrix exposes: on this worktree, `gpu_cull::substrate_frameBegin()` is still called from `code/objmgr.cpp:1933` inside `GameObjectManager::update`. That site is pause-gated. The hoist-to-Mission-update fix from the `gpu-driven-rendering` branch (commit f8d6b17 per `memory/pause_unpause_diagnostic_for_static_render_bugs.md` 2026-05-13 entry) is NOT yet applied here. See Pause-gated pattern below.

## Cross-cutting concerns

### Lighting

- **Producer:** `MC_TextureManager::renderLists` (`mclib/txmmgr.cpp:1331`) uploads `lightDataBuffer_` UBO and `sceneDataBuffer_` (TG_HWSceneData) once per frame.
- **Per-shape consumer:** `ShapeRenderer::render(..., light_data_buffer_index)` — index assigned per-shape during `update()`.
- **Stamp contract:** `cachedFrame_` (mclib/msl.h:286) on `TG_MultiShape` + skip-stale in `GpuStaticPropRegistry::flush` is the named contract that fixed the black-tree-bug. Per `memory/black_tree_bug_investigation_state.md`. Producer = `update()`; consumer = `flush()` / `render()`.
- **Shaders consuming light UBO:** `gos_terrain.frag`, `mech.frag`, `static_prop.frag` (and the bridge / overlay shaders that read `fogRGB` from per-vertex stream).
- **Lighting cache refresh** (`CacheGpuLightData` / `ResubmitCachedGpuLightData`) is the second pause-gated mechanism (memory caveat 2026-05-06).

### Cull (5-way cascade)

Per `memory/cull_gates_are_load_bearing.md`. Each is load-bearing for MORE than visibility:

1. **`inView` / `canBeSeen` / `objBlockInfo.active`** gate `GameObjectManager::update` (`code/objmgr.cpp:1731`) — out-of-block objects never have `update()` called.
2. **Lifecycle** — `update()` returning false at objmgr.cpp:1748 -> `setExists(false)` -> permanent destruction. Bypass kills objects.
3. **TGL pool budget** — `getVerticesFromPool` returning NULL silently drops shapes (`memory/tgl_pool_exhaustion_is_silent.md`). Pools at 500K.
4. **Per-instance refresh** — `MechAppearance::update` at mech3d.cpp:4256 gates `updateGeometry()` on `inView`. Same shape at gvactor.cpp:2773.
5. **Projection rhw** — GPU static-prop / terrain overlay shaders do manual perspective divide; vertices with `clip.w <= 0` produce garbage. CPU pre-cull is THE load-bearing frustum gate. Per `memory/clip_w_sign_trap.md`.

GPU compute cull path: `gpu_cull::compute_dispatch` (gpu_cull_compute.cpp:786). Cull-dilation + conservative-OR shipped 89e35ac per `render-perf-snapshot.md`.

### Shadow

- **Two-pass model.** Pass 1: render shadow depth into shadow FBO (`gos_postprocess.cpp:1099` framebuffer attachment). Pass 2: main pass samples shadow tex via `calcShadow` in `shaders/include/shadow.hglsl`.
- **Eligibility gate** — `!firstTextureAlpha` exclusion in shape shadow path; suppresses fences/gates without affecting trees. Per `memory/shadow_caster_eligibility_gate.md`. tgl.cpp:3051 NOTE comment.
- **FBO management** — `shadowFBO_`, `shadowDepthTex_`, `shadowDepthProg_` all in `gos_postprocess.cpp` (constructor :55, init :1081, bind :607, cleanup :1186). `shadowsEnabled_` (`:59`) is the master switch.
- **Cascade structure** — dynamic CSM (`MC2_SHADOW_CSM`, default-ON since 2026-06-18) is now the multi-cascade dynamic system; the *static* world-fixed terrain shadow map was SHIPPED `e9b9aea7` (2026-05-16, predates this doc's original "design ready" note — never reconciled until 2026-07-17 DEFERRED-SWEEP-1 de-stale). Built once per mission from the full map, camera-move-independent — see `docs/architecture.md` Shadow Pipeline section.

### State cache (applyRenderStates equality-cache)

- **Contract:** `gosRenderer::applyRenderStates` (`gameos_graphics.cpp:1283`) uses a state-equality cache (`stateCacheValid_` at :1467) that early-outs when no state changes. Per `memory/render_state_change_cost_hierarchy.md`, bindings dominate state-change cost; the cache amortizes that.
- **Invalidation API:** `gos_InvalidateRenderStateCache` (gameos_graphics.cpp:5801). Any code path that mutates GL state outside `applyRenderStates` MUST call this hook at the END of the path.
- **Known invalidation sites:** `gos_terrain_bridge_drawSingleBucket`, `GpuStaticPropBatcher::flush`, `drawTerrainOverlays` (gameos_graphics.cpp:6185), `drawDecals` (`:6245`). The 6-initial + 4-from-adversarial-review story per `render-perf-snapshot.md`.

### Texture cache

- **Eviction site:** `mcTextureManager->update()` called from `code/mission.cpp:527` (was :509 in older memories; line has drifted) — gated on `!isPaused() || MPlayer`. Per `memory/pause_unpause_diagnostic_for_static_render_bugs.md`.
- **Re-cache producer:** `objectManager->update` -> per-actor `update()` -> `TransformMultiShape` -> `SetTextureHandle`.
- **PAUSE/UNPAUSE diagnostic:** if render bug clears on pause and re-appears on unpause, suspect the eviction/re-cache seam. Now a multi-mechanism diagnostic (texture-cache OR lighting-cache OR substrate-ring-slot).
- **Texture handle is live** — never cache handles, only slot indexes; resolve at draw time. Per `memory/mc2_texture_handle_is_live.md`.

## Pattern families (named contracts new work matches against)

### Producer/consumer with frame-stamp

**Exemplar:** black-tree-bug fix (`memory/black_tree_bug_investigation_state.md`).

**Shape:** A producer (`*Appearance::update`) writes per-actor cached state (`cachedGpuLightIndex_`); a consumer (`render` / `flush`) reads it. Cull can skip the producer for offscreen actors but not the consumer. Fix: stamp the per-actor cache with `cachedFrame_` (mclib/msl.h:286); skip-stale in the consumer.

**Apply when:** any new per-actor cached state is added that is set by `update` and read by `render`/`flush`. Stamp it. Make the consumer skip stale entries.

### Pause-gated state inside update() consumed by render()

**Three known instances:**
1. `mcTextureManager->update()` — texture cache eviction inside `Mission::update` (mission.cpp:527). Skipped under pause.
2. Lighting-cache refill (`CacheGpuLightData` / `ResubmitCachedGpuLightData`) — same shape.
3. `gpu_cull::substrate_frameBegin()` — currently at `code/objmgr.cpp:1933` inside `GameObjectManager::update`, pause-gated. **Open on nifty-mendeleev** — the hoist-fix from `gpu-driven-rendering` (commit f8d6b17) is not present here. Visible bug: paused frames append to substrate ring without resetting it; render-time submitMultiShape keeps appending; result is layered prop geometry.

**Pattern:** anything that mutates per-frame inside `ObjectManager::update()` and is consumed by `render()` is structurally pause-broken. The named contract for new work: "do not put per-frame ring/cache resets inside update; put them at the frame boundary in `Mission::update` BEFORE the pause/non-pause branch."

**Audit grep:** `grep -nE "(substrate_frameBegin|frameBegin|reset|clear)" code/objmgr.cpp` — every hit there is a candidate.

### Dual-output wrapper for CPU-to-GPU port

**Exemplar:** Track A1 object admission predicate (`memory/track_a1_object_admission_predicate.md`).

**Shape:** Wrap the legacy CPU site (e.g. `Camera::projectForObjectAdmission`). Capture legacy output in a `LegacyProjectionResult`. New predicate decides admission from clip-space; screen output stays byte-identical via legacy capture. Env opt-out (`MC2_OBJECT_ADMISSION_PREDICATE=legacy`). Self-test at startup. Default-on after silent-on-pass tier1 soak.

**Apply when:** any port of a CPU correctness predicate to GPU. Always run both during soak; switch default-on only after parity is silent for tier1 5/5 across the env-var triple (unset / new=on / new=on + PARITY=on).

### Render-state cache invalidation contract

**Shape:** any function that mutates persistent GL state (depth func, blend, bindings) outside `gosRenderer::applyRenderStates` MUST call `gos_InvalidateRenderStateCache()` at end. The cache is keyed on `stateCacheValid_` (gameos_graphics.cpp:1467).

**Known caller-side gaps and fixes** (6 original + 4 from adversarial review per `render-perf-snapshot.md`):
- `gos_terrain_bridge_drawSingleBucket`
- `GpuStaticPropBatcher::flush` wrap-state mutation
- `drawTerrainOverlays` (now-present at gameos_graphics.cpp:6185)
- `drawDecals` (now-present at gameos_graphics.cpp:6245)
- Doc-of-contract gap (this matrix entry exists partly to close it)

**Apply when:** a new fast path or bridge bypasses `applyRenderStates`. Audit at end-of-function for `gos_InvalidateRenderStateCache()`.

## Known gaps in this matrix

| Cell | Gap |
|---|---|
| Indirect-terrain compute shader | `gpu_driven_terrain_solid.comp` referenced in advisor docs but does not exist as a file in `shaders/`. Either named differently (current candidate: dispatch path inside `gos_terrain_indirect.cpp`) or the compute portion is not yet implemented. Research before citing as code. |
| MLR immediate-draw catalog | The full set of MLR-appearance classes that draw immediately (vs the modern hardware path) is not enumerated. `mclib/mlr/` is large; only the principle (immediate draw) is captured. |
| Crater-pass call-site disambiguation | `craterManager->render()` is called both at `gamecam.cpp:208` and `mclib/camera.cpp:1794`. Which fires under which camera mode (game vs cinematic) needs grep. |
| GVAppearance render() line | Confirmed `GVAppearance::update` exists in gvactor.cpp but `GVAppearance::render` line not explicitly grep-verified (the function exists; cite via grep at use time). |
| Tree shadow vs static-prop shadow path | TreeAppearance has its own renderShadows (bdactor.cpp:4703). Whether it goes through the same `MC2_DRAWSOLID|MC2_ISTERRAIN` shadow walk in renderLists or a separate path needs grep. |
| Effects / weapon bolts / explosions | Not covered as a row (gosfx particle FX has one). Per-effect-type matrix entries are a follow-up. |
| Track D shadow handling | `GpuMechBatcher::flushShadow` is a no-op in Slice A. The Slice B path is roadmap, not code. |

## Maintenance

This doc is refreshed when:

- A new fast path ships (add row or update existing).
- A legacy path retires (mark RETIRED or remove row).
- A cross-cutting concern changes shape (e.g. a new state-cache invalidation site lands; the pause-gated pattern gains a fourth instance).
- The `[SUBSYS vN]` log-banner version of any contract surface bumps.
- A grep against the cited symbols reveals significant drift; bump line numbers in the most-cited symbols and date-stamp the refresh.

The maintenance hook (`.claude/maintenance-rules.json`) reminds about this doc when rendering-source files are touched.

The synthesizer pattern from `mc2-render-contract-synthesizer.md` could be adapted for this doc; not yet built.

Cross-references:

- `docs/render-contract.md` — submission-space contracts
- `docs/render-perf-snapshot.md` — bucket map and in-flight slice state
- `.planning/codebase/ARCHITECTURE.md` — higher-level frame flow
- `.claude/agents/DOMAINS.md` — who-owns-what for advisor routing
- `memory/named_contracts_at_intersections.md` — the operating principle
- `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md` — legacy/modern coexistence
- `memory/render_functions_are_enqueuers_not_submitters.md` — the foundational engine convention
- `memory/render_order_post_renderlists_hook.md` — operational rule for fast paths
- `memory/gpu_direct_renderer_bringup_checklist.md` — the 9 traps every new fast path hits
- `memory/water_ssbo_pattern.md` — reusable CPU-to-GPU offload template
