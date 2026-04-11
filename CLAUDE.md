# MC2 OpenGL — Nifty-Mendeleev Worktree

## What This Is
MechCommander 2 OpenGL port with tessellated terrain (Phong smoothing, displacement), PBR splatting (HSV color classification, 4 per-material normal maps), and post-processing (HDR FBO, bloom, FXAA, tonemapping).

## Key Paths
- **Source (this worktree):** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Main repo:** `A:/Games/mc2-opengl-src/`
- **Runtime deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`
- **CMake:** `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Skills (use these!)
base directory: `A:\Games\mc2-opengl-src\.claude\skills\`
- `/mc2-build` — build mc2.exe in current worktree
- `/mc2-deploy` — deploy exe + all shaders with diff verification
- `/mc2-build-deploy` — full cycle: build then deploy
- `/mc2-check` — verify deployed files match source (dry run)

## Critical Rules
- **Build config:** ALWAYS `--config RelWithDebInfo`. Release crashes with GL_INVALID_ENUM.
- **Shader deploy:** NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q` to verify. `cp -r` silently fails to overwrite on Windows/MSYS2.
- **Git:** NEVER push to alariq/mc2 origin. All work is local.
- **sampler2DArray:** NEVER use — crashes AMD drivers. Use individual sampler2D on units 5-8.
- **Shader #version:** Never put `#version` in shader files. Pass `"#version 420\n"` as prefix to `makeProgram()`.
- **Shadow light direction:** `lightDirection` points scene→sun. `updateLightMatrix` expects light→scene. MUST negate: `pp->updateLightMatrix(-lx, -ly, -lz, ...)`. Getting this wrong puts light underground.
- **Shadow coordinate space:** lightSpaceMatrix must be in raw MC2 space (Z=up). Use `gos_GetShadowCenter` (raw MC2) for camera pos, NOT `gos_GetTerrainCameraPos` (swizzled GL). `terrainLightDir` must also be raw MC2 (matching tangent-space normals where Z=up).
- **Config overrides:** Camera limits (zoom, altitude, clip distance, visible vertices) are set in 3-4 places. Hardcoded defaults get overwritten by config file reads and prefs.cpp. Override AFTER config reads, not before.
- **Buffer scaling:** Increasing `GameVisibleVertices` requires scaling: vertex pool (`startVertices` in mission2.cpp), `MC_MAXFACES` (txmmgr.h), `indexed_tris_` capacity, and `terrain_extra_capacity_` (gameos_graphics.cpp). Missing any one causes crash.
- **Shader hot-reload fails silently:** If a shader compile error occurs during hot-reload (e.g. bad `#include`), the old shader stays active with no visual indication. The terrain goes on rendering normally. Always check console output for compile errors when shader changes appear to have no effect. Hot-reload checks every 500ms in `endFrame()` and also monitors include file timestamps.
- **`gos_tex_vertex` is the shared basic material:** Water, HUD elements, detail textures, and all non-terrain/non-lighted batched draws use `gos_tex_vertex.frag`. Adding uniforms or branching here (like `isWater`) must reset state for non-water draws to avoid bleed — the material instance is shared.
- **Uniform API:** Call `setFloat`/`setInt` BEFORE `apply()`, not after. `apply()` flushes dirty uniforms.
- **GL_FALSE for terrainMVP:** Direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE` (engine convention).

## Key File Locations (line numbers for nifty-mendeleev)

### GameOS/gameos/gameos_graphics.cpp (large — read targeted ranges)
- `gosRenderer::init` — line 1418 (terrain VBO setup, material loading)
- `applyRenderStates` — line 1625 (cull face, blend, texture binding)
- `gosRenderer::beginFrame` — line 1751 (binds gVAO)
- `beginShadowPrePass` — line 2014 (shadow two-pass: begin/draw/end)
- `terrainDrawIndexedPatches` — line 2094 (GL_PATCHES draw, uniforms, extra VBO)
- `gos_CreateRenderer` — line 2513 (renderer + postprocess init)
- `gos_DestroyRenderer` — line 2522

### GameOS/gameos/gameosmain.cpp
- `handle_key_down` — line 37 (debug keys: RAlt+F1 bloom, F2 tonemap, F3 shadows, F5 FXAA)
- `draw_screen` — line 141 (postprocess beginScene/endScene wrapping)
- `main()` — line 283

### GameOS/gameos/gos_postprocess.cpp
- `init` — line 66 (FBO creation, shader compilation)
- `beginScene` — line 270 (binds scene FBO)
- `endScene` — line 333 (bloom pass, composite to screen)
- `renderSkybox` — line 385 (currently disabled)

### Terrain pipeline (mclib)
- `code/gamecam.cpp` ~line 147 — terrainMVP + viewport composition
- `code/mission.cpp` ~line 393 — tessellation debug keys (Ctrl+Shift+1-8)
- `mclib/txmmgr.h` — MC_VertexArrayNode extras, addTerrainExtra()
- `mclib/txmmgr.cpp` — renderLists() per-node batch extras
- `mclib/quad.cpp` — fillTerrainExtra() routes to addTerrainExtra()

### Shaders
- `shaders/gos_terrain.vert` — vertex shader with worldPos/worldNorm inputs
- `shaders/gos_terrain.tesc` — TCS (Phong smoothing, distance LOD)
- `shaders/gos_terrain.tese` — TES (terrainMVP projection, displacement, outputs WorldPos)
- `shaders/gos_terrain.frag` — HSV splatting, POM, anti-tiling, normal mapping, shadow sampling
- `shaders/shadow_terrain.vert` — shadow depth VS (pos, texcoord, worldPos, worldNorm → TCS)
- `shaders/shadow_terrain.tesc` — shadow TCS (same distance LOD as main terrain)
- `shaders/shadow_terrain.tese` — shadow TES (displacement + lightSpaceMatrix projection)
- `shaders/shadow_terrain.frag` — explicit gl_FragDepth write (AMD requirement)
- `shaders/include/shadow.hglsl` — calcShadow() with PCF, bias, NdotL back-face guard
- `shaders/include/noise.hglsl` — simplex 2D noise (Ashima Arts) + FBM, reusable utility
- `shaders/gos_tex_vertex.frag` — generic textured vertex shader, also handles water (isWater uniform)

## Known Issues
- Post-processing effects (bloom, FXAA) apply to HUD — needs scene/HUD render split
- Skybox disabled (terrain fog provides atmosphere; 2D skybox looked jarring)
- Tonemapping (ACES) designed for linear HDR, pipeline is sRGB — off by default
- Shadow banding shifts with camera rotation — MC2 generates view-dependent terrain geometry (LOD changes per camera angle). Shadow pass draws the same view-dependent triangles from the light's perspective, so shadow depth shifts when camera rotates. Fix requires generating camera-independent terrain for shadow pass (major architectural change).
- Shadow re-render stutter — cached shadow map re-renders all ~200 terrain nodes in one frame when camera moves >500 units. Causes single-frame dip to ~21 FPS. Fix: two-layer shadow maps (static terrain + dynamic objects).

## Map Dimensions
- **worldUnitsPerVertex:** 128.0 (terrain.cpp:76)
- **Map sizes vary per mission:** 60, 80, 100, or 120 vertices per side (terrain.cpp:296-303, read from pak)
- **Coordinate range:** `±(halfVerticesMapSide * 128)`, centered at origin
- **World extents by map size:**

| Vertices/side | Half | World range | Total extent |
|--------------|------|-------------|--------------|
| 60 | 30 | ±3840 | 7680 |
| 80 | 40 | ±5120 | 10240 |
| 100 | 50 | ±6400 | 12800 |
| 120 | 60 | ±7680 | 15360 |

- **Formula (mapdata.cpp:803-804):** `vx = (col - half) * 128`, `vy = (half - row) * 128` (Y inverted from row index)
- **Runtime access:** `Terrain::realVerticesMapSide`, `Terrain::halfVerticesMapSide` (set during `Terrain::init()`)

## Architecture Notes
- **terrainMVP:** MC2's DX6/7-era projection is non-linear (explicit perspective divide, negative clip.w for visible vertices). TES does a 3-step pipeline: clip→perspDiv→screen→NDC. Cannot be folded into single matrix.
- **Per-node VBO alignment:** Terrain extras stored in texture manager nodes, filled per-node by quad.cpp, drawn per-batch by txmmgr.cpp renderLists().
- **Post-process pipeline:** Scene renders to RGBA16F FBO, composited via fullscreen quad. Bloom at half-res with ping-pong FBOs. All fullscreen draws disable GL_CULL_FACE.
- **Shadow pipeline (tessellated two-pass, cached):** Shadow pre-pass in `renderLists()` (txmmgr.cpp) draws ALL terrain nodes to 4096x4096 shadow FBO as GL_PATCHES via `drawShadowBatchTessellated` (same TCS/TES pipeline as main terrain). Light matrix built in `draw_screen()` from `gos_GetShadowCenter` (raw MC2 camera pos) and negated `gos_GetTerrainLightDir` (scene→sun direction, negated to light→scene for matrix). Shadow radius 8000 units. Terrain frag samples via sampler2DShadow + 16-tap Stratified Poisson Disk PCF with NdotL back-face guard. **Shadow map is cached:** `shouldRenderShadows()` skips the entire pre-pass when camera moves <500 units. `cachedLightSpaceMatrix_` frozen alongside shadow content; `getLightSpaceMatrix()` returns cached version. Dirty flag set on shadow toggle (RAlt+F3). Object shadows (`g_shadowShapes`) also skipped but array always reset.
- **Water pipeline:** Water is NOT part of the terrain splatting shader (`gos_terrain.frag`). It's a separate alpha-blended overlay drawn AFTER terrain via `TerrainQuad::drawWater()` (quad.cpp), using the generic `gos_tex_vertex` shader. Two layers: base water (`waterHandle`, `MC2_ISWATER`) and detail/spray (`waterDetailHandle`, `MC2_ISWATERDETAIL`). Both batched through texture manager `renderLists()` DRAWALPHA pass. `gos_State_Water` render state routes `isWater`/`time` uniforms via the deferred uniform system (`setInt`/`setFloat` before `apply()`). UVs are world-derived but per-quad wrapped by integer amounts in `drawWater()` — use `sin(UV * 2π * N)` with integer N for seamless tiling across wrapping boundaries (simplex noise breaks at these seams).
- **Coordinate spaces:** worldPos in extras VBO is raw MC2 (x, y, elevation=Z). `terrainLightDir` stored in raw MC2 space (Z=up, matching fragment tangent-space normals). Shadow `lightSpaceMatrix` built in raw MC2 space via `gos_GetShadowCenter`. `cameraPos` for TCS distance LOD MUST be raw MC2 space (matching vs_WorldPos). Two call sites set it: `gamecam.cpp:176` (raw MC2, correct) and `terrain.cpp:788` (was GL-swizzled, fixed to raw MC2). Mismatched spaces → garbage distances → tess level always 1.
- **Stuff/MLR → raw MC2 coordinate transform:** `getCameraOrigin()` returns Stuff space (.x=left, .y=elevation, .z=forward). To match WorldPos in terrain shaders: `terrain.x = -camera.x`, `terrain.y = camera.z`, `terrain.z = camera.y`. In fragment shader: `vec2 camGround = vec2(-cameraPos.x, cameraPos.z)`. Verified via heatmap debug visualization.
- **Performance bottleneck is CPU, not GPU:** At Wolfman zoom, GPU utilization is only 11-15% (was 45-50% before shadow caching). Fragment shader LOD (branch-based) has zero performance effect on AMD — wavefront execution schedules all texture reads regardless of branching. Future perf work should target CPU-side draw call reduction. Shadow map caching (19→50 FPS) and uniform location caching (+3-5 FPS) are the current mitigations. Two-layer shadow maps (static terrain + dynamic objects) planned next.
- **Uniform location caching:** `terrainDrawIndexedPatches` and `drawShadowBatchTessellated` cache all `glGetUniformLocation` results in `TerrainUniformLocs`/`ShadowUniformLocs` structs. Cache auto-invalidates when shader program ID changes (hot-reload safe). Eliminates ~6000 string lookups/frame.
- **Overlay/decal depth via gl_FragDepth:** Terrain overlays (cement, craters) use the basic shader without tessellation displacement, so they're at original depth — behind displaced terrain. The TES computes `UndisplacedDepth` using `mvp * interpScreenPos` (matching the basic shader's projection exactly), and the frag shader writes `gl_FragDepth = clamp(UndisplacedDepth + 0.0005, 0, 1)`. This makes the depth buffer match what overlays/objects expect, so GL_LEQUAL works naturally. The +0.0005 bias overcomes floating-point precision differences. **Failed approaches:** GL_ALWAYS (covers objects), polygon offset (angle-dependent), stencil (AMD driver doesn't write stencil on renderbuffer-backed FBOs), render reorder (objects share texture manager flush with terrain).
- **Render order in gamecam.cpp (CANNOT be changed):** `land->render()` (queues terrain) → `craterManager->render()` (queues craters) → `ObjectManager->render()` (DRAWS objects immediately via MLR) → `land->renderWater()` (queues water) → `renderLists()` (FLUSHES all queued terrain/overlays/craters/water). Objects and terrain share the texture manager; moving renderLists() before objects breaks everything because object draws get queued but never flushed.
- **renderLists() internal phase order:** (1) Shadow pre-pass to shadow FBO, (2) DRAWSOLID terrain (tessellated, writes depth+stencil), (3) DRAWALPHA detail textures, (4) DRAWALPHA+ISCRATERS terrain overlays (cement/road, ZWrite=0), (5) non-terrain craters, (6) non-terrain alpha draws, (7) water layers, (8) terrain shadows. Overlays render AFTER objects (which drew during ObjectManager::render before renderLists).

## AMD Driver Rules (RX 7900 XTX, driver 26.3.1)
These were discovered through extensive debugging and MUST be followed:
- **Attribute 0 must be active** — AMD skips draws if vertex attrib 0 isn't enabled. Add `layout(location = 0) in vec4 dummyPos;` with a dummy read.
- **Explicit gl_FragDepth** — AMD optimizes away "empty" fragment shaders (just `void main() {}`). Write `gl_FragDepth = gl_FragCoord.z;` for depth-only passes.
- **Dummy color attachment on depth-only FBOs** — AMD may not rasterize into FBOs with only a depth attachment. Add a small R8 color texture.
- **No texture feedback loops** — unbind shadow texture from sampler unit 9 before rendering to shadow FBO (same texture as depth attachment). Re-bind after.
- **Matrix transpose: GL_FALSE for direct upload** — the deferred uniform system (`setMat4`/`apply()`) uses `GL_TRUE` transpose. Shadow shader uses direct `glUniformMatrix4fv(..., GL_FALSE, ...)` to avoid double-transpose. Never mix deferred and direct for the same uniform.
- **Deferred vs direct uniforms** — Use `setFloat`/`setInt` BEFORE `apply()` for uniforms that should be flushed during `apply()`. Direct `glUniform*` calls must happen AFTER `apply()` (which calls `glUseProgram`). `drawIndexed()` calls `apply()` internally — so deferred uniforms set before `drawIndexed()` get flushed correctly, but direct GL calls before it get overwritten.
- **material->end() deactivates shader** — In multi-batch draw loops (like shadow pre-pass), `material->end()` calls `glUseProgram(0)`. Must call `material->apply()` again at the start of each batch, then re-upload all direct uniforms (lightSpaceMatrix, tess params, etc). Forgetting this causes `GL_INVALID_OPERATION` on every `glUniform*` call.
- **draw_screen() timing** — `draw_screen()` runs BEFORE `gamecam.cpp` sets camera/light values each frame. Shadow matrix and uniforms use previous frame's values. First ~240 frames (during loading) have zero camera pos and fallback light dir. Must handle gracefully (fallback values).
