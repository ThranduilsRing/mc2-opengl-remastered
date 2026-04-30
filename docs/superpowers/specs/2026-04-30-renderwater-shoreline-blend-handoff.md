# renderWater Stage 2 — Shoreline Blend-Mode Handoff (2026-04-30)

> **Role for fresh session:** Stage 2 of the renderWater architectural slice
> ships VISUALLY working on tier1, with one outstanding bug: shoreline tiles
> render with hard tile boundaries instead of legacy's smooth water-terrain
> alpha fade. **Per-vertex alpha computation is correct (verified via debug
> visualizers).** The user's strong suspicion (informed by past
> decal/overlay debugging on this codebase): the bug is in the **blend
> mode / blend equation**, not per-vertex data.

## TL;DR for the new session

Read 4 things in order, then start:

1. `memory/renderwater_fastpath_stage2.md` — current state, what shipped
2. `memory/gpu_direct_renderer_bringup_checklist.md` — 8 GPU-direct gotchas
3. `docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md` — full spec with Stage 2 ship notes
4. The decal/overlay blend mode fix history via `git log`:
   ```
   git log --oneline --all | grep -iE "decal|overlay|blend|premultiplied" | head -20
   ```

Then run the side-by-side capture to see the bug:
```
py -3 scripts/water_visual_diff.py mc2_01
```

Look at `tests/smoke/artifacts/water-diff-<latest>/legacy.png` vs `fastpath.png`.
Legacy shows smooth water-shore alpha fade; fastpath shows hard tile-aligned blocks.

## What's been ruled OUT

Verified via the `MC2_RENDER_WATER_FASTPATH_DEBUG` shader debug modes:

- **debug=1 (magenta override)** — renders correctly over visible water area → projection chain works
- **debug=4 (alpha-band viz: red=deep, green=mid, blue=edge)** — shows correct per-tile alpha classification (mostly red interior, green shoreline edges = alphaMiddle band, no within-tile gradients visible because alphaDepth=15 makes the middle band tiny)
- **debug=5 (cornerIdx viz: 0=red, 1=green, 2=blue, 3=yellow)** — shows clear smooth R/G/B/Y gradients within each tile → **per-vertex interpolation works correctly, cornerIdx is per-vertex**
- **debug=6 (elev/400 viz)** — water area uniform color because the seabed IS uniformly at elevation 200 in mc2_01 (verified via `event=recipe[N]` dump showing many tiles with `elev=(200,200,200,200)` [UNIFORM] and shoreline tiles with `elev=(200,400,400,200)` [VARIED]).

Recipe dump confirms VARIED elevations at shoreline:
```
event=recipe[5872] elev=(200.0,400.9,400.9,200.0) [VARIED]
```
And alpha uniforms are correct:
```
event=alpha_uniforms waterElevation=350.000 alphaDepth=15.000 alphaEdgeByte=125 alphaMiddleByte=175 alphaDeepByte=255
```

So per-vertex alpha IS varying correctly across shoreline tiles. The issue isn't data — it's how the alpha blends with the underlying terrain.

## Hypothesis (USER-DIRECTED — pursue this first)

> "I think it is the terrain-water blend mode like we had with decals and overlays before"

The fast-path bridge function in `GameOS/gameos/gameos_graphics.cpp` sets:
```cpp
glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
glDepthMask(GL_FALSE);
```

Standard alpha blend. The legacy water draws via `mcTextureManager`'s
`gos_RenderIndexedArray` flush, which goes through `gosRenderer::applyRenderStates`
with `MC2_DRAWALPHA` — which may set a DIFFERENT blend func, possibly
premultiplied alpha or a different equation. The decal/overlay path had a
similar issue where bypass-the-applyRenderStates rendering produced wrong-blend
output that looked subtly different from the queued path.

### Where to investigate

1. **What blend state does `gosRenderer::applyRenderStates` set when
   `MC2_DRAWALPHA` flag is active?** Trace `gameos_graphics.cpp`'s
   render-state cache for `gos_State_AlphaMode`. Compare to the
   `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` my fast path uses.

2. **Look at `gos_State_AlphaMode` mapping**. Possibly different modes:
   - `gos_Alpha_OneZero` — opaque
   - `gos_Alpha_AlphaInvAlpha` — standard alpha blend
   - `gos_Alpha_OneAlpha` — premultiplied alpha (additive-ish)
   - `gos_Alpha_OneOne` — additive
   - `gos_Alpha_AlphaOne` — ???
   - The legacy water flush at `txmmgr.cpp:1471-1493` doesn't explicitly set
     this — it uses the cached state from before. Find where MC2_DRAWALPHA-
     flagged batches inherit their alpha mode.

3. **Compare to the decal/overlay fix.** Search:
   ```
   git log -p --all -- shaders/terrain_overlay.frag shaders/decal.frag | head -200
   git log -p --all -- shaders/gos_terrain_water_fast.vert | head -100
   ```
   Look for `glBlendFunc` or `gos_State_AlphaMode` changes near 2026-04-26
   (the projectz overlay fix era).

4. **Check `gosRenderer::applyRenderStates` for `MC2_DRAWALPHA` handling.** It
   may set blend mode based on the texture's flags, not just the renderer's
   global state.

### What the right fix probably looks like

Either:
- Change my `glBlendFunc` to whatever the MC2_DRAWALPHA path sets (likely
  premultiplied or a different equation), OR
- Pre-multiply alpha into RGB in the VS so the blend equation produces correct
  fade. The legacy path may be doing premultiplied math somewhere implicit, OR
- Set the gos render-state cache (`g_gos_renderer->setRenderState(gos_State_AlphaMode, ...)`
  + `applyRenderStates()`) instead of using raw `glBlendFunc`. This DOES override
  the `glUseProgram`, so save/restore + re-bind your program after.

## What's working

- tier1 5/5 PASS both `MC2_RENDER_WATER_FASTPATH=1` and unset
- mc2_17 visual diff at smoke camera matches legacy near-identically (open ocean — no shoreline visible at this camera, so the bug doesn't show)
- mc2_01 deep-water tiles render correctly
- Marginal FPS improvement on water-heavy missions

## What's broken

Only the SHORELINE rendering. Specifically:
- Tiles where some corners are above water and some below
- Should produce a smooth alpha gradient (legacy does this)
- Currently produces uniform-per-tile alpha with hard tile boundaries
- The per-vertex alpha IS varying correctly per the debug viz; the issue is in how the per-pixel alpha-blended output composites with the underlying terrain

## Files involved (Stage 2 — all work-in-progress, NOT committed)

- `GameOS/gameos/gos_terrain_water_stream.{h,cpp}` — recipe + thin SSBO management
- `shaders/gos_terrain_water_fast.vert` — paired with `gos_tex_vertex.frag`
- `mclib/terrain.{h,cpp}` — `renderWater()` early-return + new `renderWaterFastPath()`
- `code/gamecam.cpp` — calls `land->renderWaterFastPath()` after `renderLists()`
- `mclib/mapdata.h` — public `getBlocks()` accessor
- `GameOS/gameos/gameos_graphics.cpp` — bridge function (THIS IS WHERE THE BLEND STATE LIVES)
- `scripts/water_visual_diff.py` — side-by-side capture utility

## Memory files to read

⭐ Load-bearing:
- `gpu_direct_renderer_bringup_checklist.md` — the 8 traps
- `render_order_post_renderlists_hook.md` — why fast-path runs after renderLists
- `quadlist_is_camera_windowed.md` — why we hash by vertexNum not slot
- `sampler_state_inheritance_in_fast_paths.md` — why we install our own sampler

Also relevant:
- `clip_w_sign_trap.md` — why CPU pre-cull is required
- `terrain_tes_projection.md` — the abs(clip.w) chain
- `terrain_mvp_gl_false.md` — matrix upload conventions
- `mc2_argb_packing.md` — BGRA-in-memory vs SSBO bit decode
- `water_rendering_architecture.md` — water uses gos_tex_vertex, separate from terrain splatting

## Reproduce the bug visually

```
set MC2_RENDER_WATER_FASTPATH=1& set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1& set MC2_MODERN_TERRAIN_PATCHES=1& "A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"
```

Start mc2_01 mission. Camera at start shows the island shoreline — fastpath shows hard tile boundaries between water and beach instead of legacy's smooth fade.

For automated capture (legacy + fastpath + 2 debug modes):
```
py -3 scripts/water_visual_diff.py mc2_01
```
Output in `tests/smoke/artifacts/water-diff-<timestamp>/`.

## Build / deploy

```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64 --config RelWithDebInfo --target mc2
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain_water_fast.vert" "A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_terrain_water_fast.vert"
```

(Or use `/mc2-build-deploy` skill once the worktree is loaded.)

## Definition of done for the new session

- Shoreline alpha-fade visually matches legacy on mc2_01 interactive (user confirms)
- tier1 5/5 PASS both `MC2_RENDER_WATER_FASTPATH=1` and unset, +0 destroys
- Update `memory/renderwater_fastpath_stage2.md` to remove the "outstanding" note about shoreline
- Update orchestrator `cpu-to-gpu-offload-orchestrator.md` to promote Stage 2 from "In progress" to "Shipped"
- Either:
  - Codify the blend-mode lesson as a NEW memory file (likely
    `memory/gpu_direct_blend_state_inheritance.md` or extend
    `sampler_state_inheritance_in_fast_paths.md`), OR
  - Update existing `gpu_direct_renderer_bringup_checklist.md` item 5 to
    cover blend state alongside sampler state.

## What NOT to do

- Don't try to fix per-vertex alpha — verified working via debug=5 (cornerIdx).
- Don't try to expand the alpha-band (alphaDepth) — that's a tuning knob, not the bug.
- Don't add a clip.w sign check or screen.z range check in the VS (memory:clip_w_sign_trap.md).
- Don't move the fast-path call back into renderWater() (memory:render_order_post_renderlists_hook.md).
- Don't index recipes by quadList slot (memory:quadlist_is_camera_windowed.md).
- Don't try Stage 3 (parity check infra) before fixing this — it's blocked
  on visual parity which we don't yet have at the shore.
