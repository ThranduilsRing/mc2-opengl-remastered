# GPU Static Prop Renderer — Handoff to next session

## Worktree

All work lives in:
```
A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/
```
Branch: `claude/nifty-mendeleev`. `cd` there for every git command.

Deploy path: `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`. Use `cp -f` per file + `diff -q` (NEVER `cp -r`).

Build: `cmake` isn't on PATH in MSYS2 bash. Use full path:
```
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```
NEVER Release (GL_INVALID_ENUM crash).

Capture stderr by launching via `A:/Games/mc2-opengl/mc2-win64-v0.1.1/run-with-log.bat` — redirects to `stderr.log` in the same folder.

## State as of this handoff

**End-to-end GPU path works.** Last commit: `89430ab feat(props): projection chain + state leak fix — buildings actually render`.

Toggle with `RAlt+0` (prints `GPU Static Props: ON/OFF` to stderr). When ON:
- `GpuStaticPropBatcher::instance().flush()` runs after `Render.TerrainSolid` in `mclib/txmmgr.cpp` (line ~1342)
- Draws complete without GL errors
- Full state save/restore around flush prevents downstream MLR errors under 4.3 core
- Projection chain matches `shaders/terrain_overlay.vert` pattern exactly

**Known issues still to investigate:**

1. **Buildings render BLACK instead of textured.** Most likely `v_argb` (baked vertex lighting) is zero for the buildings reaching the GPU path, OR the valid-texture packets have `listOfColors` that hasn't been populated by `TransformMultiShape` at submit time. Debug: force frag to just `FragColor = vec4(1.0);` (pure white) — if buildings are white, `v_argb` is the bug. If still black, texture sampling is wrong. Then try `FragColor = vec4(v_argb.rgb, 1.0);` to see the vertex color stream alone. Then `FragColor = texture(u_tex, v_uv);` alone.

2. **Some close-camera buildings disappear when killswitch ON.** This is the original project bug. When GPU path is on, buildings that hit Layer B fallback (non-SHAPE child or null `listOfColors`) CPU-fallback to `bldgShape->Render()`. The CPU path's `recalcBounds` angular cull has the 87% false-negative rate at wolfman zoom. The fix was supposed to be: GPU path renders everything, Layer B only triggers for rare cases. Currently Layer B is firing MORE than expected (see stderr: `[GPUPROPS] multi=X child N: non-SHAPE node` and `null listOfColors`). Need to understand why and reduce the false-positive rate in our pre-submit verification.

3. **The 2 "late registerType" types at map load.** Still unknown source. Search for where static-prop meshes get loaded LATE in `Mission::init` or afterwards — those types aren't hitting our initial registration walk.

## Key files

- `GameOS/gameos/gos_static_prop_batcher.cpp` — main renderer (844 lines). registration / submit / flush / upload ring. State save/restore is in flush() around lines 670-690, 820-840.
- `GameOS/gameos/gos_static_prop_batcher.h` — public API + instance struct layout asserts.
- `GameOS/gameos/gos_static_prop_killswitch.h` — `g_useGpuStaticProps` extern + helper accessors.
- `GameOS/gameos/gos_render.cpp:108-115` — GL 4.3 core context request (was 4.0; required for SSBO).
- `GameOS/gameos/gameos_graphics.cpp:4627-4656` — `gos_GetGLTextureId`, `gos_GetTerrainViewportVec4`, `gos_GetProj2ScreenMat4`, `gos_GetTerrainMVPMat4` helpers.
- `GameOS/gameos/gameos_graphics.cpp:1184-1188` — `getTextureListSize()` added to `gosRenderer` for bounds-checked handle lookup.
- `shaders/static_prop.vert` / `shaders/static_prop.frag` — D3D→GL projection chain + real textured frag (uniform ints, no `uniform uint`).
- `mclib/txmmgr.cpp:~1340` — flush call site (moved from 1150 to after Render.TerrainSolid).
- `mclib/bdactor.cpp:1521` — `BldgAppearance::render` with killswitch branch. `mclib/msl.cpp:1683` — canonical CPU render reference.
- `code/mission.cpp:1623, 3112` — `onMapLoad` / `onMapUnload` hooks.
- `GameOS/gameos/gameosmain.cpp:169` — RAlt+0 hotkey handler.

## Reference docs (read before touching this code)

- `docs/superpowers/specs/2026-04-19-gpu-static-prop-renderer-design.md` — authoritative design
- `docs/superpowers/plans/2026-04-19-gpu-static-prop-renderer-plan.md` — implementation plan (tasks 1-17, we're done through Task 11ish)
- `docs/render-contract.md` — project render-path contract
- `docs/amd-driver-rules.md` — AMD RX 7900 XTX driver quirks

## Memory to consult

In user memory (`~/.claude/projects/A--Games-mc2-opengl-src/memory/`):

- **`static_prop_projection.md`** ⭐ — MUST READ. Projection chain, row-vec matrix convention, SSBO `v*M` vs uniform `M*v` with GL_TRUE, full state save/restore rules. These rules took a whole session to find.
- **`uniform_uint_crash.md`** — `uniform uint` crashes shader compile; use `uniform int` + cast. Affects our three uniforms (u_materialFlags, u_maxLocalVertexID, u_packetID).
- **`terrain_tes_projection.md`** — `abs(clip.w)` is load-bearing. Don't touch it. Terrain uses the same D3D chain.
- **`feedback_subagent_deploy.md`** — subagents building mc2.exe must also deploy via `cp -f`. Don't leave stale binaries.
- `clip_w_sign_trap.md`, `shadow_coordinate_spaces.md` — useful if projection bugs come back.

## How to resume

1. `cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`
2. Read the memory files above (especially `static_prop_projection.md`)
3. Read `docs/superpowers/specs/2026-04-19-gpu-static-prop-renderer-design.md` for the full architecture
4. Read the most recent 4-5 commits to understand recent work:
   ```
   git log --oneline -10 | grep props
   ```
5. For the "buildings render black" bug: start with frag shader bisection (pure white → v_argb only → texture only → combined). Expected root cause: vertex lighting baked before TransformMultiShape refreshed, OR texture not actually loaded at submit time.
6. For the "close-camera buildings vanish" bug: add instrumentation to count submitted vs CPU-fallback buildings per frame, find what fraction are hitting the Layer B pre-checks (non-SHAPE node, null listOfColors). Those 2 conditions may be firing for legitimate buildings due to a data-model misunderstanding.

## Tasks from the original plan still pending

- Task 12: color-address debug mode hotkey binding (exists in code via `setDebugAddrMode`, just no hotkey)
- Tasks 13-14: shadow path (`flushShadow()` + shader program, wire into `Shadow.DynPass`)
- Task 15: M1 step 1 sign-off (needs buildings rendering correctly first)
- Tasks 16-17: TreeAppearance / GenericAppearance / GVAppearance wire-ups

## Don't redo

- **Don't** go back to `#version 420` — SSBOs require 430 core. GL context is 4.3 core.
- **Don't** add `layout(row_major)` to the SSBO block — it double-transposes.
- **Don't** move the flush back earlier in the render order — terrain is after Render.3DObjects.
- **Don't** use explicit "cleanup" in flush (unbinding to 0) — full save/restore is the pattern that works.
- **Don't** use `uniform uint` — crashes the shader compile.
- **Don't** assume any GL state is in a sane default at flush entry — MLR leaves various states in use.
