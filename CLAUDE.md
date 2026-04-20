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

## 2026-04-14 Shadow Collection Debug Note
- What was proven:
- Under terrain tessellation, the legacy `RenderShadows()` actor path is intentionally bypassed for buildings/trees/mechs. Dynamic object shadows therefore depend on visible shapes reaching `TG_Shape::Render(...)` and being collected into `g_shadowShapes` for `Shadow.DynPass`.
- `BldgAppearance::render()` uses `bldgShape->Render()`, so the hangar/building case should flow through the generic dynamic collector in `mclib/tgl.cpp`, not through `bldgShadowShape->RenderShadows()`.
- What was falsified:
- The missing hangar shadow is not explained by the old dedicated building shadow path being absent during the tessellated renderer; that path is disabled by design when tessellation is active.
- Files changed:
- `mclib/tgl.cpp`
- `mclib/txmmgr.cpp`
- Exact remaining blocker:
- Need an in-game run to determine whether the widened collector actually adds the missing large assets, or whether those assets are still skipped in `TG_Shape::Render(...)` because of per-shape state such as `alphaTestOn`, `alphaValue`, HUD/clamped flags, or some other exclusion.
- Temporary diagnostics added:
- `mclib/tgl.cpp`: env-gated `MC2_DEBUG_SHADOW_COLLECT` logging at the dynamic shadow collector site. It prints `[SHADOWCOLLECT][add|skip]` with node name, HUD/clamped state, alpha-test state, alpha value, first-texture alpha flag, texture count, and world translation.
- `mclib/txmmgr.cpp`: env-gated `MC2_DEBUG_SHADOW_COLLECT` logging in `Shadow.DynPass` printing the collected batch count.
- These diagnostics are temporary and should be removed once the hangar/building shadow case is verified and fixed.
- Build/deploy performed:
- Rebuilt `build64/RelWithDebInfo/mc2.exe`.
- Fast direct deploy used: copied `mc2.exe`, `shaders/gos_terrain.frag`, and `shaders/gos_tex_vertex.vert` to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`.
- Next target:
- Run with `MC2_DEBUG_SHADOW_COLLECT=1` and inspect whether the hangar/building nodes log as `[skip]` or `[add]`.
- If they log as `[skip]`, narrow the exclusion and relax only the necessary gate in `mclib/tgl.cpp`.
- If they log as `[add]` and `dynpass` counts are nonzero, instrument or inspect `gos_DrawShadowObjectBatch(...)` / dynamic shadow material state next.
- If object shadows are confirmed fixed, move back to tuning fully concrete runway/concrete appearance in `shaders/gos_terrain.frag` and then remove the temporary diagnostics.

## 2026-04-14 Cement Polish Follow-up
- What was proven:
- The widened dynamic shadow collector fixed the missing large-asset shadows in-game; hangar/building shadows now appear correctly on the rerouted terrain.
- The remaining visible issues after that fix were shader-side polish problems rather than missing geometry submission.
- What was changed:
- `shaders/gos_terrain.frag`: fully concrete/runway tiles now stay hard-forced to the concrete material class, but mixed edge tiles no longer get strong CPU terrain-type forcing. This is intended to preserve smoother terrain-to-cement transitions instead of showing triangulated/jagged polygon edges.
- `shaders/gos_terrain.frag`: pure concrete tiles now suppress the concrete detail-normal contribution and keep a much flatter, more colormap-authored base appearance instead of picking up the modern mottled concrete texture/tint treatment.
- `shaders/gos_tex_vertex_lighted.frag`: projected lighted textured draws only sample terrain/object shadow maps when the sampled texel is effectively opaque (`alpha >= 0.95`). This is intended to stop translucent projected markers from being darkened by world shadows.
- `shaders/gos_tex_vertex.frag`: removed the leftover overlay debug-era solid-red early return for world-projected overlays.
- Files changed:
- `shaders/gos_terrain.frag`
- `shaders/gos_tex_vertex.frag`
- `shaders/gos_tex_vertex_lighted.frag`
- Exact remaining blocker:
- Need in-game verification of whether the mission marker no longer darkens under shadows, whether the pure cement/runway area now reads closer to the intended flat concrete look, and whether the outer mixed transition tiles are acceptably smooth.
- Build/deploy performed:
- Rebuilt `build64/RelWithDebInfo/mc2.exe`.
- Fast direct deploy used: copied `mc2.exe`, `shaders/gos_terrain.frag`, `shaders/gos_tex_vertex.frag`, and `shaders/gos_tex_vertex_lighted.frag` to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`.
- Next target:
- Verify the three remaining visual issues in-game.
- If cement is now too flat or too bright, tune only the pure-concrete path in `shaders/gos_terrain.frag`.
- If mission markers still receive shadows, identify whether they use the basic projected shader or a different projected path and gate shadow sampling there specifically.
- After visual verification, remove the temporary shadow collection diagnostics from `mclib/tgl.cpp` and `mclib/txmmgr.cpp`.

## 2026-04-14 Cement Transition + Marker Shadow Fix
- What was proven:
  - Concrete texture visually cleaned up (confirmed by user).
  - Mission marker still received shadows despite `alpha >= 0.95` gate — the marker's visible pixels are fully opaque so the gate didn't help.
  - Jagged transition edges outside the fence were caused by `pureConcrete = (TerrainType > 2.5) ? 1.0 : 0.0` — the hard step fires at one tessellation-polygon-aligned threshold, producing a visible isoline that follows polygon edges.
- What was changed:
  - `shaders/gos_tex_vertex_lighted.frag`: removed all shadow sampling from the `gpuProjection` path entirely. Ground overlays (mission markers, nav beacons) are 2D UI elements and must not receive world shadows.
  - `shaders/gos_terrain.frag`: replaced hard-step `pureConcrete` with `smoothstep(1.5, 2.5, TerrainType)` so the terrain→concrete material blend happens gradually across the transition tile rather than at a sharp threshold. `matWeights`, `detailN`, `normalLight`, and `baseColor` all now use the smooth 0.0–1.0 float directly.
- Files changed:
  - `shaders/gos_terrain.frag`
  - `shaders/gos_tex_vertex_lighted.frag`
- Build/deploy:
  - No exe rebuild needed (shader-only changes).
  - Fast direct deploy: copied both shaders to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/`. Verified with `diff -q`.
- Next target:
  - Verify in-game: mission marker no longer shadowed, transition edges smooth.
  - If edges still jagged: widen the smoothstep range (e.g. `smoothstep(0.5, 2.5, TerrainType)`) to spread the blend over more of the tile interior.
  - If concrete tiles look too blended at center: `smoothstep` reaches 1.0 at TerrainType=2.5, which is already within pure-cement range — should be fine.
  - Once visuals confirmed: remove `MC2_DEBUG_SHADOW_COLLECT` diagnostics from `mclib/tgl.cpp` and `mclib/txmmgr.cpp`.

## 2026-04-14 Overlay Projection Deep Dive + Fragment Fog Bug Fix
- What was proven (deep analysis):
  - `terrainMVP` in GLSL = `AW^T` (not AW), because C++ mat4 is row-major, uploaded with `GL_FALSE` → GLSL interprets each C++ row as a GLSL column.
  - `AW^T * (vx, vy, elevation, 1)` = `projectZ(vx, vy, elevation)` exactly — the two computations are identical. GL_FALSE with row-major AW is correct.
  - Comment in `code/gamecam.cpp` line 165 says "uploaded with GL_TRUE" — this comment is WRONG. The actual upload uses GL_FALSE. The transpose property means the result is still mathematically correct.
  - The approved design doc "Approach A" (`gl_Position = terrainMVP * MC2WorldPos`) is WRONG — it puts D3D clip coords directly in `gl_Position`. After OpenGL perspective divide, x/w,y/w ∈ [0,1] maps to the upper-right screen quadrant, not the terrain surface.
  - The "precedent" in the design doc (lighted shader GPU projection) is moot — AMD driver makes `gpuProjection` uniform always read 0 in the shader, so that path is dead.
  - Approach B (full TES viewport chain in the overlay vertex shader) = mathematically identical to TES. The code is correct.
- Root cause of invisible cement tiles found:
  - `shaders/gos_tex_vertex.frag` IS_OVERLAY world-pos path used `mix(c, fog_color, FogValue)` — **parameters reversed**.
  - `FogValue = fog.w = fogResult/255`. For nearby terrain (no distance fog, hazeFactor≈0): `fogResult=255 → FogValue≈1.0`.
  - `FogValue=1.0` with the old call: `mix(c, fog_color, 1.0) = fog_color`. When fog is disabled, `fog_color=(0,0,0,0)` → `FragColor=(0,0,0,0)` → **transparent → invisible**.
  - The non-overlay path uses `mix(fog_color.rgb, c.rgb, FogValue)` (correct: FogValue=1 → clear, FogValue=0 → fully fogged). The IS_OVERLAY path had the parameters swapped.
- What was changed:
  - `shaders/gos_tex_vertex.frag`: fixed fog blend in IS_OVERLAY world-pos path to match non-overlay convention:
    ```glsl
    // Before (wrong — FogValue=1 → fog_color → invisible with fog disabled):
    FragColor = mix(c, fog_color, FogValue);
    
    // After (correct — FogValue=1 → c = clear, FogValue=0 → fog_color = fully fogged):
    if(fog_color.x>0.0 || ...) c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    FragColor = c;
    ```
- Files changed:
  - `shaders/gos_tex_vertex.frag`
- Build/deploy:
  - No exe rebuild needed (shader-only change).
  - Fast direct deploy: copied `gos_tex_vertex.frag` to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/`. Verified with `diff -q`.
- Vertex shader status: `gos_tex_vertex.vert` IS_OVERLAY world-pos path uses full TES viewport chain (Approach B) — this is correct and should NOT be changed to Approach A.
- Next target:
  - Verify in-game: partial cement boundary tiles and road overlays now visible on terrain surface.
  - If tiles appear but with wrong color (fog-tinted): check fog_color value at render time.
  - Once overlay visuals confirmed: remove `MC2_DEBUG_SHADOW_COLLECT` diagnostics from `mclib/tgl.cpp` and `mclib/txmmgr.cpp`.

## 2026-04-15 Cement Perimeter Hue Fix + Diagnostic Cleanup
- Root cause identified (code analysis, no guessing):
  - Alpha cement perimeter tiles (cement-to-grass/dirt transitions) inherit `oVertex.argb = vertices[i]->lightRGB` from ALL four quad corners — including the grass/dirt corners.
  - The old tone correction operated on `c = Color.bgra * tex_color`, so the non-cement vertex lighting was multiplied into the base colour BEFORE the desaturate/neutral-push/darken chain.
  - Interior cement tiles use only cement-lit vertices → consistent correction.
  - Perimeter tiles mix in grass/dirt vertex lighting → slightly different hue/brightness reaching the correction → faint tone mismatch at the outer wedges.
- What was changed:
  - `shaders/gos_tex_vertex.frag` (IS_OVERLAY path): tone correction now operates on `tex_color.rgb` directly (raw texture, not pre-multiplied by vertex colour). Vertex luminance is extracted separately and re-applied as a final scalar after the correction. This keeps terrain AO/shading variation while eliminating cross-type hue contamination.
  - `mclib/tgl.cpp`: removed `shadowCollectDebugEnabled()` function and `[SHADOWCOLLECT]` printf diagnostic block (confirmed fixed in previous session).
  - `mclib/txmmgr.cpp`: removed `shadowCollectDebugEnabled()` function and `[SHADOWCOLLECT][dynpass]` printf diagnostic block.
- Build/deploy performed:
  - Shader deployed immediately (no exe needed): `gos_tex_vertex.frag` copied to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/`.
  - Exe rebuild attempted for diagnostic cleanup but failed LNK1201 (PDB locked — game running). Close the game, then rebuild and deploy exe.
- Next target:
  - Test in-game: outer perimeter cement wedges against grass/dirt should now match the interior concrete tone.
  - If they look brighter than expected: `vertexLum` for terrain lighting might average lower than 1.0 — try adding a small bias `vertexLum = mix(vertexLum, 1.0, 0.15)` to compensate.
  - If they look correct: close game, rebuild (diagnostic cleanup), deploy updated exe.
  - Mission marker shadow: already fixed in `gos_tex_vertex_lighted.frag` — verify no shadow darkening in-game.
  - After both confirmed: no further blocking issues. Next features from `docs/plans/` or memory `upcoming_features.md`.
