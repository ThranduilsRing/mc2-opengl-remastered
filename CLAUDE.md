# MC2 OpenGL -- Nifty-Mendeleev Worktree

MechCommander 2 OpenGL port with tessellated terrain, PBR splatting, shadow maps, and post-processing.

## Key Paths
- **Source:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`
- **CMake:** `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Skills (use these!)
Skills in `.claude/skills/` (copied from main repo):
- `/mc2-build` -- build mc2.exe in current worktree
- `/mc2-deploy` -- deploy exe + all shaders with diff verification
- `/mc2-build-deploy` -- full cycle: build then deploy
- `/mc2-check` -- verify deployed files match source (dry run)

If skills aren't found by the Skill tool, they're also at `A:/Games/mc2-opengl-src/.claude/skills/`. Read the skill file and follow its instructions manually.

## Critical Rules
- **Build:** ALWAYS `--config RelWithDebInfo`. Release crashes with GL_INVALID_ENUM.
- **Deploy:** NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`. `cp -r` silently fails on Windows/MSYS2.
- **Git:** NEVER push to alariq/mc2 origin. All work is local.
- **Shader #version:** Never in shader files. Pass `"#version 420\n"` as prefix to `makeProgram()`.
- **Uniform API:** `setFloat`/`setInt` BEFORE `apply()`, not after. `apply()` flushes dirty uniforms.
- **GL_FALSE for terrainMVP:** Direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE`.
- **Shader hot-reload fails silently:** Bad compile = old shader stays active. Check console for errors.

## Reference Docs (read on demand)
- `docs/architecture.md` -- render pipeline, coordinate spaces, map dimensions, render order, shadow pipeline, performance notes
- `docs/amd-driver-rules.md` -- AMD RX 7900 XTX driver quirks (sampler2DArray crash, attribute 0, gl_FragDepth, feedback loops, etc.)
- `docs/plans/` -- design docs for upcoming features

## Key Files
- `GameOS/gameos/gameos_graphics.cpp` -- renderer core (terrain draw, shadow draw, uniform caching)
- `GameOS/gameos/gos_postprocess.cpp` -- FBOs, bloom, shadows, post-process
- `mclib/txmmgr.cpp` -- renderLists() batch flush, shadow pre-pass
- `shaders/gos_terrain.frag` -- terrain splatting, POM, shadow sampling, distance LOD
- `shaders/include/shadow.hglsl` -- calcShadow() with variable-tap Poisson PCF

## Known Issues
- Post-processing (bloom, FXAA) applies to HUD -- needs scene/HUD split
- Shadow re-render stutter when camera moves >500 units. Fix: static world-fixed shadow map (design doc ready)
- Shadow banding shifts with camera rotation (view-dependent terrain geometry)
