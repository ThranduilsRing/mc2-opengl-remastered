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

## ⚠️ Load-Bearing Cull Infrastructure — READ BEFORE TOUCHING

MC2's `inView`/`canBeSeen`/`objBlockInfo.active`/`objVertexActive` chain
is NOT just a visibility filter. It ALSO gates:
- Per-object `update()` calls (objmgr.cpp iterates only active blocks)
- TGL vertex pool allocation budget (shapes silently vanish when pool
  exhausted — `getVerticesFromPool` returns NULL → `TG_Shape::Render`
  silent early-out)
- Object lifecycle (`update()` false return → `setExists(false)` →
  permanent destruction)
- `updateGeometry()` which runs `TransformMultiShape`
  (Mech3DAppearance at mech3d.cpp:4170, GVAppearance at gvactor.cpp:2702)

"Just bypass the broken cull" **cascades** into streak artifacts (stale
matrices), destroyed objects (update returning false on stale state),
or silent shape drop-outs (pool exhaustion — mechs are the canary
because they iterate last).

**See:** `memory/cull_gates_are_load_bearing.md`,
`memory/tgl_pool_exhaustion_is_silent.md`,
`docs/gpu-static-prop-cull-lessons.md`, and the handoffs at
`docs/superpowers/plans/progress/2026-04-20-static-prop-handoff*.md`.

**Current state (2026-04-20):** The RAlt+0 killswitch (`g_useGpuStaticProps`)
enables partial bypasses that effectively make GPU-mode a
"static-props-off toggle" rather than a working alternate path. CPU mode
(killswitch OFF, default) is the supported path. Don't treat the GPU
path as working without re-reading the above references first.

## Model Routing
- haiku: lookups, summaries, simple edits, renaming, formatting
- sonnet: standard implementation, debugging, code review. always diff changes from haiku
- opus: architecture, deep analysis, complex refactors only. always diff changes from sonnet/haiku. give other agents isolated context.

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

## Profiling
- **Tracy Profiler** always compiled in (`TRACY_ENABLE`). Connect Tracy GUI to see real-time flame charts.
- **GPU zones** on shadow passes, terrain draw, 3D objects, post-process. Uses GL timer queries.
- **AMD RGP** works externally via Radeon Developer Panel for shader-level analysis.
- Include `gos_profiler.h` to add new zones. Use `ZoneScopedN("Name")` for CPU, add `TracyGpuZone("Name")` for GPU-heavy code.

## Known Issues
- Post-processing (bloom, FXAA) applies to HUD -- FIXED (gos_State_IsHUD buffering, Apr 2026)
- Shadow re-render stutter when camera moves >500 units. Fix: static world-fixed shadow map (design doc ready)
- Shadow banding shifts with camera rotation (view-dependent terrain geometry)

## Do Not Upscale These Art Assets
`code/mechicon.cpp` hardcodes `unitIconX/Y` (32/38) and computes source-pixel offsets directly against `s_MechTextures->width`. If the source TGA is 4x-upscaled via loose-file override in `data/art/`, `MechIcon::init` reads scrambled sub-rectangles and the mech damage schematic renders as garbage (alpha test then discards or shows noise). **Keep these files at their original FST-archive resolution** (do not deploy the `*_4x_gpu/` upscales for them):
- `data/art/mcui_high7.tga` (in-mission mech schematic, high-res)
- `data/art/mcui_med4.tga` (med-res)
- `data/art/mcui_low4.tga` (low-res)
Mechbay/logistics callsites already scale correctly; only the in-mission HUD path is affected.

## Memory & CLAUDE.md Discipline

**Auto-memory index:** `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

Rules for keeping this file and memory healthy:
1. **No session narratives in CLAUDE.md.** Dated "what was proven / what was changed" logs belong in commit messages or a memory file, NOT here. Every line in CLAUDE.md is loaded on every session.
2. **New durable finding → memory file + MEMORY.md index entry.** Writing a memory without updating the index makes it invisible. Group it under the right topic heading in MEMORY.md.
3. **Superseded facts → update or delete the memory, don't append.** Memory files are point-in-time and decay fast. If the "Post-processing applies to HUD" issue gets fixed, update the Known Issues line here AND the relevant memory — don't leave both stale.
4. **Before writing a new memory, search existing ones.** `grep -i <keyword> memory/*.md`. Duplicates fragment knowledge.
5. **CLAUDE.md stays under ~250 lines.** If it grows past that, the signal-to-noise is probably off — extract sections into memory files and link from here.

## Debug Instrumentation Rule (for reworks)

Any rework touching **object lifecycle, cull/visibility gates, render path, resource lifetime, or cross-system control flow** must land in the same commit as env-gated `[SUBSYSTEM]` lifecycle prints — and, for shader changes, a debug-visualization mode branch. Instrumentation stays in the tree **gated off by default**; do not delete it after the bug is fixed, demote it to silent.

**Canonical CPU macro** (matches existing `MC2_DEBUG_SHADOW_COLLECT` pattern — env-gated because this project never builds `_DEBUG`):

```cpp
static const bool s_wsTrace = (getenv("MC2_WALL_SHADOW_TRACE") != nullptr);
#define WS_TRACE(fmt, ...) \
    do { if (s_wsTrace) { printf("[WALL_SHADOW] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)
```

**Log at lifecycle boundaries only** (init, register, first-use, teardown, fallback) — never per-frame at 50-60 FPS. Line format: `[SUBSYS] event=<name> owner=<id> state=<enum> ...` — grep-friendly one-liner.

**Shader side:** add a debug-mode uniform branch that outputs intermediate values as color (precedent: `RAlt+9` GPU static-prop frag debug-mode 0..7).

**Keep until** the feature survives: (a) full mission load from main menu, (b) camera pan into previously-unseen terrain, (c) mission restart without quitting. Then demote — leave gated, silent.

Full rationale, triggers, naming conventions, and anti-patterns: `memory/debug_instrumentation_rule.md`.
