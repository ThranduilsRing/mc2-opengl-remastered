# MC2 OpenGL — Nifty-Mendeleev Worktree

## What This Is
MechCommander 2 OpenGL port with tessellated terrain (Phong smoothing, displacement), PBR splatting (HSV color classification, 4 per-material normal maps), and post-processing (HDR FBO, bloom, FXAA, tonemapping).

## Key Paths
- **Source (this worktree):** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Main repo:** `A:/Games/mc2-opengl-src/`
- **Runtime deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`
- **CMake:** `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Skills (use these!)
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
- **Uniform API:** Call `setFloat`/`setInt` BEFORE `apply()`, not after. `apply()` flushes dirty uniforms.
- **GL_FALSE for terrainMVP:** Direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE` (engine convention).

## Key File Locations (line numbers for nifty-mendeleev)

### GameOS/gameos/gameos_graphics.cpp (large — read targeted ranges)
- `gosRenderer::init` — line 1418 (terrain VBO setup, material loading)
- `applyRenderStates` — line 1625 (cull face, blend, texture binding)
- `gosRenderer::beginFrame` — line 1751 (binds gVAO)
- `terrainDrawIndexedPatches` — line 1976 (GL_PATCHES draw, uniforms, extra VBO)
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
- `shaders/gos_terrain.tese` — TES (terrainMVP projection, displacement)
- `shaders/gos_terrain.frag` — HSV splatting, POM, anti-tiling, normal mapping

## Known Issues
- Post-processing effects (bloom, FXAA) apply to HUD — needs scene/HUD render split
- Units float above tessellation-displaced terrain — CPU heightmap doesn't match GPU displacement
- Skybox disabled (terrain fog provides atmosphere; 2D skybox looked jarring)
- Tonemapping (ACES) designed for linear HDR, pipeline is sRGB — off by default

## Architecture Notes
- **terrainMVP:** MC2's DX6/7-era projection is non-linear (explicit perspective divide, negative clip.w for visible vertices). TES does a 3-step pipeline: clip→perspDiv→screen→NDC. Cannot be folded into single matrix.
- **Per-node VBO alignment:** Terrain extras stored in texture manager nodes, filled per-node by quad.cpp, drawn per-batch by txmmgr.cpp renderLists().
- **Post-process pipeline:** Scene renders to RGBA16F FBO, composited via fullscreen quad. Bloom at half-res with ping-pong FBOs. All fullscreen draws disable GL_CULL_FACE.
