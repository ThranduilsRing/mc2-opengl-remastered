# MC2 OpenGL Architecture Reference

Read this file when working on rendering pipeline, coordinate spaces, or render order.

## Coordinate Spaces
- **Raw MC2 (WorldPos):** x=east, y=north, z=elevation. Extras VBO, terrainLightDir, lightSpaceMatrix all use this.
- **Stuff/MLR (getCameraOrigin):** .x=left, .y=elevation, .z=forward. Transform to raw MC2: `terrain.x = -camera.x`, `terrain.y = camera.z`, `terrain.z = camera.y`. In shader: `vec2 camGround = vec2(-cameraPos.x, cameraPos.z)`.
- **terrainMVP:** DX6/7-era non-linear projection (explicit perspective divide, negative clip.w for visible). TES does clip->perspDiv->screen->NDC. Cannot be folded into single matrix.

## Map Dimensions
- **worldUnitsPerVertex:** 128.0 (terrain.cpp:76)
- **Map sizes:** 60, 80, 100, or 120 vertices/side (terrain.cpp:296-303, from pak)
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

**Caching:** shouldRenderShadows() skips pre-pass when camera moves <500 units. cachedLightSpaceMatrix_ frozen alongside content. Dirty flag on toggle (RAlt+F3).

**Planned:** Static world-fixed shadow map (render once at load). See docs/plans/2026-04-11-static-terrain-shadows-design.md.

## Water Pipeline
NOT part of terrain splatting. Separate alpha-blended overlay via gos_tex_vertex shader. Two layers: base water + detail/spray. gos_State_Water routes isWater/time uniforms. UVs world-derived, per-quad wrapped -- use sin(UV * 2pi * N) for seamless tiling.

## Overlay/Decal Depth
TES computes UndisplacedDepth matching basic shader projection. Frag writes gl_FragDepth = clamp(UndisplacedDepth + 0.0005, 0, 1). Overlays pass GL_LEQUAL naturally.

## Performance Notes
- **CPU-bound, not GPU-bound.** GPU util 11-15% at Wolfman zoom. Fragment shader LOD (branching) has zero effect on AMD -- wavefronts don't skip texture reads.
- Shadow caching: 19->50 FPS. Uniform location caching: +3-5 FPS.
- TerrainUniformLocs/ShadowUniformLocs structs cache glGetUniformLocation. Auto-invalidate on shader program change (hot-reload safe).
