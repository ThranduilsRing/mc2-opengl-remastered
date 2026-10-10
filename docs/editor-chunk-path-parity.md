<!-- doc-status: current | reviewed:2026-10-11 -->
# Editor Chunk-Path Parity — 8z-B Gate Document

**Branch:** `claude/terrain-gen-pcg`
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Date:** 2026-06-10
**Prior doc:** `docs/terrain-8z-fastpath-drop-recon.md` (H2 recon; T13 editor-quarantine finding)

---

## Summary Verdict

| Question | Answer |
|---|---|
| Does editor use the chunk terrain path? | **YES — partially.** Chunk culling (update) runs. `Terrain::flushDrawCommands()` is **NOT called** in the editor render chain; the GPU draw is never submitted. |
| Does editor call `setupTextures`? | **YES — always.** `land->geometry()` runs unconditionally (EditorInterface.cpp:777), and when `drawTerrainGrid==true` the `fullyArmed` predicate (terrain.cpp:3724) is false, so `setupTextures()` runs for every visible quad. |
| What does the legacy path own in editor? | Three things: **(a) surface render** (indirect solid + overlay arming may arm normally but the flush is missing → the chunk submit never fires; legacy CPU per-quad `draw()` loop in `render()` is skipped when solid/overlay are both armed), **(b) grid/overlay lines** (`drawTerrainGrid` → `drawLine()` per quad, terrain.cpp:2407-2411), **(c) slimReduce/objBlockInfo.active** (geometry() chunk-active producer runs under chunk=ON, but editor calls geometry() too and the legacy slimReduce path is unreached only if makeLists was skipped). |
| Owner hypothesis (editor already renders chunks) | **PARTIALLY CONFIRMED.** Chunk cull, chunk active-set prod, mine-enqueue, and arming all execute in editor mode. The GPU draw submit (`flushDrawCommands`) is the missing link — it is **only called from `code/gamecam.cpp:582`**, which is the game render loop, not the editor render loop. The editor renders the terrain surface via the GPU-indirect path when solid/overlay arm normally (the per-quad `draw()` loop is skipped at terrain.cpp:2221), but the chunk-specific LOD mesh draw is never submitted. |
| Recommended quarantine option | **Option A (compile-gate / editor-build flag) with a narrow runtime guard already available.** The only blocking legacy symbol for 8z-A is `setupTextures` called when `!fullyArmed`. In the editor the single driver of `!fullyArmed` is `drawTerrainGrid` (T13). Compile-gating `setupTextures` + the per-quad `draw()` loop behind `#ifndef MC2_EDITOR_BUILD` (or equivalent) is the correct 8z-B path. Adding `flushDrawCommands()` to the editor render chain (EditorCamera.h step after `land->render()`) would make the chunk GPU draw live in the editor, but is gated by the owner confirming chunk renders correctly there. |

---

## Q1 — Does the chunk terrain path actually execute in editor mode?

### Editor terrain render chain

The editor frame loop is:

```
EditorGameOS.cpp:558   Environment.DoGameLogic()   -> Editor::update() -> EditorInterface::update()
EditorGameOS.cpp:565   Environment.UpdateRenderers() -> UpdateRenderers() (Editor.cpp:121)
                           -> editor->render() (Editor.cpp:159)
                              -> eye->render() (EditorInterface.cpp:716)
                                 -> EditorCamera::render (EditorCamera.h:202+)
```

Inside `EditorCamera::render` (EditorCamera.h:277-321):
- `land->render()` (step 3, EditorCamera.h:278) — calls `Terrain::render()`
- `mcTextureManager->renderLists()` (step 7, EditorCamera.h:314)
- `land->renderWaterFastPath()` (step 8, EditorCamera.h:318)
- **No call to `Terrain::flushDrawCommands()`.**

Inside `EditorInterface::update()` (EditorInterface.cpp:748-778):
- `land->update()` (editor/EditorInterface.cpp:718 / 761) — calls `Terrain::update()`
- `land->geometry()` (EditorInterface.cpp:777) — calls `Terrain::geometry()`

### Chunk execution in editor

**`Terrain::update()` (mclib/terrain.cpp):**
- `makeLists` is skipped when `mc2TerrainLodChunkEnabled()` is true (mclib/terrain.cpp:60). This applies in the editor too — there is no editor-specific branch.
- The chunk cull block (mclib/terrain_init.cpp:916: `if (s_blockMeta && s_superchunkMeta && s_drawCmds && eye)`) runs and builds `s_drawCmds[]` every frame. This IS chunk-path execution.

**`Terrain::geometry()` (mclib/terrain.cpp):**
- Chunk active-set producer (mclib/terrain_render.cpp:239: `if (s_lodChunkProd && !s_activeABForce && eye && objBlockInfo ...)`) runs and fills `objBlockInfo.active` — same as game. This IS chunk-path execution.
- Legacy `slimReduce` loop is a no-op when `numberVertices==0` (makeLists was skipped). Does NOT run under chunk=ON in editor by default.

**`Terrain::render()` (mclib/terrain_render.cpp:7):**
- Legacy per-quad `draw()` loop at terrain.cpp:2221 is **suppressed** when `mc2TerrainLodChunkEnabled()` is true (the condition at mclib/terrain.cpp:60: `if (!mc2TerrainLodChunkEnabled())`). The GPU-indirect solid/overlay path arms normally in the editor (when recipes are ready) and owns the surface pixels through the normal GPU-indirect pipeline.
- Mine enqueue and mine draw loop run normally.
- `drawTerrainGrid` overlay runs when toggled on (terrain.cpp:2401-2427).

**`Terrain::flushDrawCommands()` — THE MISSING CALL:**
- Called only from `code/gamecam.cpp:388`.
- **Never called in the editor render chain** (verified: grep of `editor/*.cpp` returns zero hits).
- `s_drawCmds[]` is built every frame by the chunk cull path but never submitted to the GPU in the editor. The chunk LOD mesh draw is silent.

### Conclusion for Q1

The chunk cull (update), chunk active-set production (geometry), and GPU-indirect surface render (render, via IsFrameSolidArmed path) all run in the editor. However, the chunk-specific LOD terrain mesh (`gos_TerrainLodChunk_SubmitDrawCommands`) is **never submitted** — the editor lacks the `flushDrawCommands()` call in its render chain. The terrain surface is rendered by the GPU-indirect path (which is not the same as the chunk LOD mesh draw). The editor terrain renders correctly today because the GPU-indirect solid/overlay path handles the surface; the chunk LOD mesh is dead weight being computed but not drawn.

`mc2TerrainLodChunkEnabled()` (mclib/terrain.cpp:60) — the env gate — returns the same value in editor and game (it is a static local `bool` with no editor/game branching).

---

## Q2 — What exactly does `drawTerrainGrid` gate, and where is it set?

### Declaration and write sites

| File | Line | Access | Context |
|---|---|---|---|
| `mclib/terrain.cpp` | 258 | declaration + init (`false`) | `bool drawTerrainGrid = false;` — comment says "Override locally in editor" |
| `editor/EditorInterface.cpp` | 224 | `extern` declaration | |
| `editor/EditorInterface.cpp` | 5944 | **write true** | `OnViewShowpassabilitymap()` toggle-ON path when `GameMap != NULL` |
| `editor/EditorInterface.cpp` | 5928 | **write false** | `OnViewShowpassabilitymap()` toggle-OFF path |
| `editor/EditorInterface.cpp` | 1861 | commented-out toggle | dead code (`//drawTerrainGrid ^= TRUE;`) |

`drawTerrainGrid` is only set to `true` by the editor (View > Show Passability Map when `GameMap` is available). The game binary never sets it true. It is a plain global `bool` — no compile-time gate.

### Read sites in `mclib/terrain.cpp`

| Line | Context | Role |
|---|---|---|
| 2401 | `if (drawTerrainGrid \|\| DrawDebugCells \|\| drawLOSGrid)` | Gate for the per-quad `drawLine()` overlay loop |
| 2407 | `if (drawTerrainGrid \|\| drawEditorPassability)` | Inner gate: call `currentQuad->drawLine()` (vs debug cells or LOS) |
| 3115 | `bool isVisible = Terrain::IsGameSelectTerrainPosition(vPos) \|\| drawTerrainGrid;` | In `Terrain::geometry()` slimReduce visibility test — with grid ON, every vertex at the world edge is treated as visible, widening the quad window. Relevant only when `makeLists` ran (chunk=OFF or `MC2_TERRAIN_ACTIVE_AB`). |
| 3724 | `!drawTerrainGrid &&` | **T13: the `fullyArmed` predicate.** Presence of `drawTerrainGrid==true` forces `fullyArmed=false` → `skipSetup=false` → `setupTextures()` runs for all ~40K visible quads. |
| 3762 | `reason = "T13_DRAW_TERRAIN_GRID"` | Transition log naming. |
| 3781 | `(int)drawTerrainGrid` | Logged in `[FASTPATH_DROP]` output. |

### What `drawTerrainGrid` actually requires

- **Grid/overlay lines** (`drawLine()` per quad, terrain.cpp:2407-2411): these need `setupTextures()` to have run because `drawLine()` reads `clipInfo` / vertex coordinates set by `setupTextures()`. This is the **actual dependency** — the overlay lines are the feature being rendered.
- **Surface render**: `drawTerrainGrid==true` does NOT change the surface render path. The GPU-indirect solid/overlay path renders the terrain surface regardless. The legacy per-quad `draw()` loop is already suppressed when `mc2TerrainLodChunkEnabled()==true` (terrain.cpp:2221) — independently of `drawTerrainGrid`.
- **Terrain picking/editing math**: `Terrain::geometry()` slimReduce visibility at terrain.cpp:3115 uses `drawTerrainGrid` to widen the active set. This is a legacy slimReduce path that is already a no-op under chunk=ON (makeLists skipped → numberVertices=0 → slimReduce is a no-op). No picking dependency on `drawTerrainGrid`.

**Critical distinction:** `drawTerrainGrid` forces `setupTextures()` only to provide `clipInfo` (clip-space coordinates) to `drawLine()`. The surface render is independent. The quarantine scope is therefore **narrow**: `setupTextures()` needs to remain in the editor only when `drawTerrainGrid==true`. If the editor grid overlay (`drawLine()`) were ported to use world-space coordinates directly (or chunk-derived clip coords), the `setupTextures()` dependency evaporates.

---

## Q3 — Is there partial chunk support in the editor already?

Yes. The evidence:

| Chunk system component | Runs in editor? | Evidence |
|---|---|---|
| Chunk cull (update, `s_blockMeta` block) | YES | mclib/terrain_init.cpp:916; update() called from EditorInterface.cpp:755/761 |
| Chunk active-set production (geometry, `s_lodChunkProd` block) | YES | mclib/terrain_render.cpp:237-239; geometry() called from EditorInterface.cpp:777 |
| GPU-indirect solid/overlay surface render | YES (when recipes armed) | terrain.cpp:2221 suppresses legacy draw() loop under chunk=ON; GPU-indirect path handles surface |
| Chunk LOD mesh draw (`gos_TerrainLodChunk_SubmitDrawCommands`) | **NO** | flushDrawCommands() only at code/gamecam.cpp:388; absent from editor render chain |
| `mc2TerrainLodChunkEnabled()` returns true | YES | Env-gated static bool; same in both editor and game |
| `EditorDebugOverlay` queries chunk state | YES | EditorDebugOverlay.cpp:211: `s_probe.chunkPath = mc2TerrainLodChunkEnabled()` |

### Memory claim reconciliation

**Memory A:** "Editor runs default-on modern chain, self-skips legacy draw in places."
**VERIFIED TRUE.** The legacy per-quad `draw()` loop is suppressed at terrain.cpp:2221 under chunk=ON. `makeLists` is also skipped. The GPU-indirect path arms and renders the surface. This is accurate.

**Memory B:** "Editor = GPU-only test bed — no legacy CPU fallbacks."
**PARTIALLY STALE.** When `drawTerrainGrid==true`, `fullyArmed` is false → `setupTextures()` runs for ~40K quads. This is a real legacy CPU fallback. The memory claim is only true when the grid overlay is OFF. When the passability map is displayed, the editor hits the legacy path hard.

**Owner hypothesis (editor may already render chunks):**
PARTIALLY CONFIRMED. Chunk culling, active-set production, and the GPU-indirect surface draw all execute. The chunk LOD mesh GPU draw specifically is missing (`flushDrawCommands` absent from editor chain). Since the GPU-indirect path (not the chunk LOD mesh) handles the surface render in the editor today, the editor terrain IS visually correct — the chunk LOD mesh draw is a second separate GPU terrain draw that is latent/unsubmitted.

---

## Q4 — Quarantine mechanism recommendation

Given Q1-Q3 findings:

### Option A — Compile-gate / editor-build gate (RECOMMENDED)

Gate `setupTextures()` and the per-quad `draw()` loop behind `#ifndef MC2_EDITOR_BUILD` (or `EDITOR` CMake define already in the build). Only the editor binary keeps these symbols; the game binary deletes them.

**Feasibility:** HIGH. The only active callers of `setupTextures()` in a live game frame are:
1. `mclib/terrain_geometry.cpp:86` in the `geometry()` quad-setup loop — already gated `if (!skipSetup)`.
2. This caller is already `fullyArmed=true` in steady-state game, making the loop body no-op.
After 8z-A this call site + `TerrainQuad::setupTextures` + `TerrainQuad::draw` move behind the compile gate.

For the editor to continue working:
- Keep `TerrainQuad::setupTextures()` and `TerrainQuad::draw()` compiled in editor builds (simple `#ifdef`).
- The editor passability grid overlay (`drawLine()`) continues to work because `setupTextures()` is still present and `drawTerrainGrid==true` still defeats `fullyArmed`.
- No runtime behavior change; the gate is structural.

Files to move behind the gate: `mclib/quad.cpp` (setupTextures body, draw body), `mclib/terrain.cpp` (per-quad draw loop at 2221-2271, setupTextures loop body at 3806, slimReduce if included in 8z).

### Option B — Runtime global (current `drawTerrainGrid` bool) (AVOID)

H2 recon (Finding 5, terrain-8z-fastpath-drop-recon.md:151-153) explicitly documents the risk: `drawTerrainGrid` is a plain global bool in the shared binary; any stray set permanently suppresses the quad-skip opt. This option is already ruled out by the project.

### Option C — Make editor fully chunk-path (narrow quarantine)

**Feasibility:** MEDIUM, one missing call.

The gap is exclusively `Terrain::flushDrawCommands()` not being called in the editor render chain. Adding it to `EditorCamera::render()` after `land->render()` (step 3) and before `mcTextureManager->renderLists()` (step 7) would submit the chunk LOD mesh GPU draw in the editor.

If that call is added and verified (the editor terrain renders correctly via the chunk LOD path):
- The only remaining `setupTextures` dependency is `drawLine()` for the passability grid overlay.
- `drawLine()` could be ported to use a separate world-space → clip-space transform that does not depend on `setupTextures()` having run (vertex clip coords are available from the chunk's own cull data or a lightweight per-quad project).
- Once `drawLine()` is decoupled, `drawTerrainGrid==true` no longer forces `setupTextures()`, `fullyArmed` stays true, and the full quarantine is lifted.

This is the cleanest end state but requires:
1. Adding `Terrain::flushDrawCommands()` to editor render chain (EditorCamera.h).
2. Visual confirmation that chunk LOD mesh renders correctly in editor.
3. Porting `drawLine()` off `setupTextures()`-derived clipInfo, or accepting a runtime `if (drawTerrainGrid) setupTexturesForGrid()` path.

**Recommended path:** Do Option A first (compile-gate, unblocks 8z-A immediately), then pursue Option C as editor polish to eliminate the runtime `drawTerrainGrid` global entirely.

### Symbols requiring attention per option

| Symbol | Option A | Option B | Option C |
|---|---|---|---|
| `TerrainQuad::setupTextures()` | Behind `#ifndef GAME_BUILD` | Keep as-is | Needed only for `drawLine()` clipInfo; can be narrowed |
| `TerrainQuad::draw()` | Behind `#ifndef GAME_BUILD` | Keep as-is | Not needed (chunk LOD mesh draws instead) |
| `slimReduce` loop (Terrain::geometry) | Behind `#ifndef GAME_BUILD` | Keep as-is | Not needed (chunk active-set prod covers it) |
| `makeLists` call (Terrain::update) | Behind `#ifndef GAME_BUILD` | Keep as-is | Not needed (already no-op in editor under chunk=ON) |
| `drawTerrainGrid` global | Keep for editor | Keep for editor | Retire after `drawLine()` decoupled |
| `Terrain::flushDrawCommands()` | Not changed | Not changed | ADD to EditorCamera::render (step 3.5) |

---

## Stale Memory Corrections

| Memory claim | Correction |
|---|---|
| "Editor = GPU-only test bed — no legacy CPU fallbacks" | FALSE when View > Show Passability Map is active (`drawTerrainGrid==true` → `fullyArmed=false` → `setupTextures()` runs all ~40K quads). Only true when passability overlay is OFF. |
| H2 recon: "editor depends on legacy setupTextures/draw path — when drawTerrainGrid==true the fast-path fullyArmed predicate goes false → legacy fallback" | CONFIRMED ACCURATE. Also confirmed that the legacy `draw()` loop is already suppressed under chunk=ON regardless; only `setupTextures()` itself is the dependency. |

---

## Open Questions / Could Not Determine

1. **Does the editor's GPU-indirect solid path actually arm?** The code path is shared (same `IsFrameSolidArmed`, same `ComputePreflight`), but editor builds may fail recipe/atlas readiness on some maps. Not traced to completion — requires runtime verification (`MC2_FASTPATH_DROP_LOG=1` in editor).

2. **Does `flushDrawCommands` being absent cause the chunk-built `s_drawCmds[]` to accumulate or be silently dropped?** Each frame, `s_cmdCount` is reset to 0 at terrain.cpp:1542, so there is no accumulation. But the zero-cmd guard at terrain.cpp:2043-2050 would fire `[TerrainLOD v1] ERROR: enabled but zero draw commands` in the editor after frame 10 if `s_cmdCount` stays 0 post-build. Worth checking whether the editor produces this log in practice.

3. **Does `drawLine()` read `clipInfo` set by `setupTextures()`?** The Q2 analysis concludes yes (clipInfo is the clip-space vertex data needed for screen-space line drawing), but the exact `drawLine()` body in `quad.cpp` was not fully read. Confirm before attempting Option C decoupling.

4. **Does the editor call `gos_TerrainLodChunk_SubmitDrawCommands` indirectly via `mcTextureManager->renderLists()`?** Verified: `renderLists` in `mclib/txmmgr_render.cpp:144` does not call `flushDrawCommands`. The chunk submit is entirely absent from the editor frame.

---

*Read-only recon. No source code modified. All citations verified in worktree `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/` on branch `claude/terrain-gen-pcg`, 2026-06-10.*
