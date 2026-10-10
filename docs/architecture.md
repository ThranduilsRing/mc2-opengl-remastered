<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 OpenGL Architecture Reference

> **PARTIALLY STALE (banner added 2026-07-09; last full refresh 2026-04-11).**
> Coordinate-space and map-dimension sections remain valid. The pipeline/render-order
> sections describe the legacy tessellated path only: bloom was deleted 2026-06-22,
> shadows are CSM 3x8192 (default-ON since 2026-06-18), and the default terrain
> renderer is the LOD-chunk path (`MC2_TERRAIN_LOD_CHUNK` default-ON since 2026-06-09,
> shaders `shaders/terrain_lod_chunk.*` — see `docs/known_issues.md`). For a current
> map read `.planning/codebase/ARCHITECTURE.md`.

Read this file when working on coordinate spaces; verify pipeline claims against code.

## Coordinate Spaces
- **Raw MC2 (WorldPos):** x=east, y=north, z=elevation. Extras VBO, terrainLightDir, lightSpaceMatrix all use this.
- **Stuff/MLR (getCameraOrigin):** .x=left, .y=elevation, .z=forward. Transform to raw MC2: `terrain.x = -camera.x`, `terrain.y = camera.z`, `terrain.z = camera.y`. In shader: `vec2 camGround = vec2(-cameraPos.x, cameraPos.z)`.
- **terrainMVP:** DX6/7-era non-linear projection (explicit perspective divide, negative clip.w for visible). TES does clip->perspDiv->screen->NDC. Cannot be folded into single matrix.

## Map Dimensions
- **worldUnitsPerVertex:** 128.0 (mclib/terrain_internal.h:71)
- **Map sizes:** 60, 80, 100, or 120 vertices/side (mclib/terrain_update.cpp:81-88, from pak)
- **Coordinate range:** +/-(halfVerticesMapSide * 128), centered at origin
- 60v=+/-3840, 80v=+/-5120, 100v=+/-6400, 120v=+/-7680
- **Formula:** `vx = (col - half) * 128`, `vy = (half - row) * 128` (Y inverted)
- **Runtime:** `Terrain::realVerticesMapSide`, `Terrain::halfVerticesMapSide`

## Render Pipeline
- **Per-node VBO:** Terrain extras stored in texture manager nodes, filled per-node by quad.cpp, drawn per-batch by txmmgr.cpp renderLists().
- **Post-process:** Scene -> RGBA16F FBO -> bloom (half-res ping-pong) -> composite fullscreen quad. All fullscreen draws disable GL_CULL_FACE.
- **Render order (gamecam.cpp, CANNOT change):** land->render() (queues terrain) -> craterManager->render() (queues craters) -> ObjectManager->render() (DRAWS objects immediately via MLR) -> land->renderWater() (queues water) -> renderLists() (FLUSHES all queued).
- **renderLists() phases:** (1) Shadow pre-pass, (2) DRAWSOLID terrain (tessellated), (3) DRAWALPHA detail, (4) DRAWALPHA+ISCRATERS overlays, (5) non-terrain craters, (6) non-terrain alpha, (7) water, (8) terrain shadows.

## Shadow Pipeline
Shadow pre-pass in renderLists() draws ALL terrain nodes to 4096x4096 shadow FBO as GL_PATCHES via drawShadowBatchTessellated. Light matrix built from gos_GetShadowCenter (raw MC2) and negated light dir. Shadow radius 8000 units. Fragment shader: sampler2DShadow + 16-tap Stratified Poisson Disk PCF.

**STALE (fixed `e9b9aea7`, 2026-05-16; this paragraph described pre-fix behavior):** ~~Caching: shouldRenderShadows() skips pre-pass when camera moves <500 units. cachedLightSpaceMatrix_ frozen alongside content.~~ `shouldRenderShadows()` / `cachedLightSpaceMatrix_` no longer exist in the code (grep-verified). The camera-move-gated re-bake was retired wholesale by `e9b9aea7` ("static full-map shadow feed + Phase-1 camera-accum retire") — see the "Static world-fixed shadow map" note below, which is SHIPPED, not planned.

**Static world-fixed shadow map: SHIPPED, not planned (`e9b9aea7`, 2026-05-16).** The static terrain shadow FBO is built exactly ONCE per mission (`mclib/txmmgr.cpp` ~2692, `Shadow.StaticFullMapBuild` zone), gated solely by the `gos_StaticLightMatrixBuilt()` latch, from `Terrain::mapData->renderStaticTerrainShadowFullMap()` (full map, not a camera-windowed subset). It is reset only by `Terrain::destroy` (new mission) or the `RAlt+F3` shadow-toggle re-enable path — never by camera movement. Consequence: a >500-unit camera jump mid-mission cannot trigger any static-shadow re-render (the old known_issues.md "shadow stutter on >500-unit jumps" bug class is structurally impossible post-fix). Empirically re-verified 2026-07-17 (`DEFERRED-SWEEP-1`, no code change): a hard ~990-unit camera teleport onto a `shadow_cascade`-covering mc2_24 ridge view (`tests/visual/bookmarks/mc2_24_shadow_jump500.json`) produces a byte-identical captured frame at `MC2_VISUAL_SETTLE=1` vs `=2/30/60` — the shadow is fully converged one frame after the hard cut, zero pop. Dynamic CSM cascades (see `docs/known_issues.md` Shadows section) are unrelated: they are rebuilt fully every frame regardless of camera motion, so a jump changes cascade *content*, never adds re-render *cost*.

## Water Pipeline
NOT part of terrain splatting. Separate alpha-blended overlay via gos_tex_vertex shader. Two layers: base water + detail/spray. gos_State_Water routes isWater/time uniforms. UVs world-derived, per-quad wrapped -- use sin(UV * 2pi * N) for seamless tiling.

## Overlay/Decal Depth
TES computes UndisplacedDepth matching basic shader projection. Frag writes gl_FragDepth = clamp(UndisplacedDepth + 0.0005, 0, 1). Overlays pass GL_LEQUAL naturally.

## Profiling (Tracy)
18 zones instrument the frame: GameLogic, DrawScreen, SwapWindow, Camera.BuildMVP, Camera.UpdateRenderers, Camera.SceneDataUpload, Shadow.StaticBuild, Shadow.StaticPrePass, Shadow.StaticBatch, Shadow.StaticEnd, Shadow.DynMatrixBuild, Shadow.DynPass, Shadow.DynObjectBatch, Render.3DObjects, Render.TerrainSolid, Render.Overlays, Render.PostProcess. GPU zones (TracyGpuZone) on: StaticBatch, StaticBuild, DynPass, DynObjectBatch, 3DObjects, TerrainSolid, PostProcess. TracyGpuContext initialized after GL context creation. TracyGpuCollect called after swap_window each frame.

## Performance Notes
- **CPU-bound, not GPU-bound.** GPU util 11-15% at Wolfman zoom. Fragment shader LOD (branching) has zero effect on AMD -- wavefronts don't skip texture reads.
- Shadow caching: 19->50 FPS. Uniform location caching: +3-5 FPS.
- TerrainUniformLocs/ShadowUniformLocs structs cache glGetUniformLocation. Auto-invalidate on shader program change (hot-reload safe).
