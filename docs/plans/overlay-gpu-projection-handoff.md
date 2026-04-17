# Overlay GPU Projection — Session Handoff

## Goal
Add shadow sampling to terrain overlays (cement pads, roads, burn marks). These are drawn via `gos_tex_vertex` shader over the base terrain. Currently they have no shadows — buildings don't cast shadows on cement.

## What Works
- **Mechs/buildings:** GPU projection via `gos_tex_vertex_lighted` + `terrainMVP`. Shadow sampling works.
- **Terrain:** `gos_terrain` shader has full shadow sampling via `calcShadow()`.
- **Post-process shadow pass:** Darkens non-terrain 3D objects via screen-space depth reconstruction.

## What's Stuck — Overlay Shadow Sampling

### The Core Problem
Overlays are drawn through `gos_tex_vertex.frag`. We need shadow sampling in that shader, but ONLY for overlay draws (not HUD, water, other gos_tex_vertex users).

### Approach 1: Runtime Uniform `gpuProjection` (FAILED — AMD driver bug?)
**Fully diagnosed this session.** The C++ pipeline is 100% correct:
- `setInt("gpuProjection", 1)` returns `true` (found in deferred cache, type matches)
- `glGetUniformLocation(shp, "gpuProjection")` returns valid location (1 or 5 depending on variant)
- `glGetUniformiv()` readback RIGHT BEFORE `glDrawElements()` confirms value = **1** on GPU
- Correct program is active (`GL_CURRENT_PROGRAM` matches `shp_`)
- No sampler location collision (`tex1` at different location)
- Shader compiles and links (confirmed by unconditional magenta test — ALL gos_tex_vertex pixels turned magenta)

**BUT:** The shader ALWAYS reads `gpuProjection` as 0. Tested with:
- `if (gpuProjection != 0) { RED } else { BLUE }` → everything blue
- `c.rgb = vec3(float(gpuProjection), 0.5, 0.0)` → everything green (red channel = 0)

**This is unexplainable.** GPU readback says 1, shader sees 0. Possible AMD RX 7900 XTX driver bug with `uniform int` across stages, or something deeply wrong with how the deferred uniform cache interacts with the driver. **Do not pursue this approach further.**

### Approach 2: Compile-Time Variant `IS_OVERLAY` (IN PROGRESS — not yet working)
Added `IS_OVERLAY` as a second shader flag alongside `ALPHA_TEST`. The variant system compiles 4 combinations: `{0, ALPHA_TEST, IS_OVERLAY, ALPHA_TEST|IS_OVERLAY}`.

**Changes made:**
- `gosGLOBAL_SHADER_FLAGS::IS_OVERLAY = 1` added
- `g_shader_flags[]` updated with `"IS_OVERLAY"`
- `combinations[]` expanded to 4 entries
- `selectBasicRenderMaterial()` adds `IS_OVERLAY` flag when `rs[gos_State_Overlay]` is set
- Shaders have `#ifdef IS_OVERLAY` guards

**Current status:** A simple red tint test (`c.rgb = mix(c.rgb, vec3(1.0,0,0), 0.5)`) inside `#ifdef IS_OVERLAY` shows NO red tint on overlays. The variant either isn't being selected, or the shader compilation is silently failing.

**Known issue:** `getMaterialVariation()` at line 177 generates `#define IS_OVERLAY = 1` (with `= 1`) instead of `#define IS_OVERLAY 1`. This is wrong C preprocessor syntax — it defines `IS_OVERLAY` to expand to `= 1`. However, `#ifdef IS_OVERLAY` should still be true (the macro IS defined). Same bug exists for `ALPHA_TEST` and alpha test works. **But worth fixing as a potential AMD-specific issue.**

### Untested Next Steps

1. **Fix the `= 1` define syntax** — Change line 177 from `" = 1\n"` to `" 1\n"`. Test if that fixes `#ifdef IS_OVERLAY`.

2. **Verify variant compilation** — Add a printf in the shader compilation loop (line 1598-1606) to log each variant name + shp_ to confirm IS_OVERLAY variants are actually created and linked.

3. **Verify variant selection at draw time** — Add a one-time log in the overlay draw path showing `mat->getName()` to confirm the IS_OVERLAY variant is selected (name should contain `#IS_OVERLAY#`).

4. **Alternative: Use the post-process shadow pass** — Instead of per-pixel shadow sampling in the overlay shader, extend the existing post-process shadow pass to also darken overlay pixels. The post-process pass already reconstructs world position from depth and samples shadow maps. Currently it skips terrain (via normal alpha flag). Overlays might also be skipped. This approach avoids per-shader changes entirely.

5. **Alternative: Vertex color flag** — Encode overlay status in vertex data (e.g., abuse frgb alpha or color alpha). The fragment shader checks the vertex attribute instead of a uniform. Bypasses the uniform bug entirely.

## Tracy Profiler Zones Added This Session

### gameos_graphics.cpp (CPU + GPU):
- `DrawIndexedTris.Basic` — the main gos_VERTEX indexed draw (overlays, terrain, details)
- `DrawIndexedTris.Lighted` — 3D objects (mechs, buildings) with lighting UBOs
- `DrawIndexedTris.Unlighted` — 3D objects without lighting
- `Terrain.TessDraw` + `Terrain.DrawPatches` — tessellated terrain rendering (CPU+GPU)
- `Overlay.SplitDraw` — the overlay split apply/draw path with shadow setup (CPU+GPU)
- `BasicDraw.Indexed` — generic indexed draw fallback
- `Grass.Draw` — geometry shader grass pass (CPU+GPU)
- `SetupObjectShadows` — shadow map uniform upload for objects/overlays
- `ApplyRenderStates` — GL state machine updates
- `SelectBasicMaterial` — material variant selection (texture/alpha/overlay flags)
- `DrawQuads`, `DrawTris`, `DrawLines`, `DrawPoints`, `DrawText` — other draw primitives

### shader_builder.cpp (CPU):
- `Shader.Apply` — glUseProgram + dirty uniform flush
- `Shader.Reload` — hot-reload recompile + relink
- `Shader.MakeProgram` — initial shader compilation + linking

### txmmgr.cpp (CPU + GPU):
- `Render.Overlays` + GPU zone — the full overlay render section
- `Render.CraterOverlays` + GPU zone — specifically the gos_State_Overlay=1 crater/overlay draws
- `Render.NoUnderlayer` + GPU zone — objects without terrain underlayer

## Key Files (Current State)

### Modified this session:
- `GameOS/gameos/gameos_graphics.cpp` — IS_OVERLAY variant system, cleaned overlay draw path, `selectBasicRenderMaterial` with overlay flag
- `shaders/gos_tex_vertex.vert` — `#ifdef IS_OVERLAY` for terrainMVP (but currently uses legacy projection for both paths since overlay vertices are screen-space)
- `shaders/gos_tex_vertex.frag` — `#ifdef IS_OVERLAY` with red tint test, water code in `#else` block
- `mclib/txmmgr.cpp` — `gos_SetRenderState(gos_State_Overlay, 1)` around overlay draw loop (unchanged from previous session)

### Key reference:
- `GameOS/gameos/utils/shader_builder.cpp` — `parse_uniforms()`, `apply()`, `makeProgram2()`, `reload()`
- `docs/uniform-contracts.md` — uniform pipeline documentation

## Diagnostic Findings

### Confirmed Facts:
1. **CWD = `A:\Games\mc2-opengl\mc2-win64-v0.1.1`** — shader files load from correct deploy directory
2. **Shader compiles** — unconditional magenta test turned ALL gos_tex_vertex pixels magenta (HUD, mechs, overlays, buildings)
3. **Overlay state propagates** — `renderStates_[gos_State_Overlay]=1` confirmed via file logging, `curStates_` copies it
4. **Overlay split draw path executes** — the code inside `if (curStates_[gos_State_Overlay] && terrain_mvp_valid_)` runs
5. **Program handle consistent** — `shp_=6`, `active_prog=6`, same throughout
6. **Uniform location valid** — `gpuProjection` at location 1 (or 5 depending on shader version)
7. **GPU readback = 1** — `glGetUniformiv` confirms value is 1 right before `glDrawElements`
8. **Shader sees 0** — fragment shader's `float(gpuProjection)` outputs 0.0 in red channel
9. **printf/stdout doesn't work** — Windows GUI app. Must use `fopen()` with ABSOLUTE paths for logging
10. **Relative fopen fails** — despite CWD being correct, relative `fopen("overlay_diag.log")` didn't create files (reason unknown)

### Debug Hotkeys:
- RAlt+F5 = terrain draw killswitch (toggle)
- Other hotkeys per MEMORY.md

## Architecture Notes

### Overlay Rendering Pipeline:
```
txmmgr.cpp renderLists():
  gos_SetRenderState(gos_State_Overlay, 1)
  for each overlay vertex node:
    gos_RenderIndexedArray(vertices, count, indices, count)
      → gosRenderer::drawIndexedTris(vertices, num_v, indices, num_i)
        → applyRenderStates() copies Overlay to curStates_
        → selectBasicRenderMaterial(curStates_) picks variant
        → overlay split path: apply(), direct GL uniforms, glDrawElements
  gos_SetRenderState(gos_State_Overlay, 0)
```

### Overlay Vertices:
Overlays use `gos_VERTEX` struct: `{x,y,z,rhw, argb, frgb, u,v}`. The x,y,z are **screen-space** (CPU-projected by MC2's terrain renderer in quad.cpp). They are NOT MC2 world coordinates. Previous session added `setOverlayWorldCoords()` but overlay vertices going through `drawIndexedTris` still appear to have screen-space coords (the legacy projection path works, GPU projection via terrainMVP produces garbage positions).

### Variant System:
`getMaterialVariation()` builds a prefix string: `"#version 420\n#define FLAG = 1\n..."`. Note the `= 1` syntax — technically wrong but works for `#ifdef`. Each combination (flag bitmask) maps to a separate `gosRenderMaterial` in `materialDB_[shader_name][flags]`. Selection via `selectBasicRenderMaterial()` computes flags from render state and looks up the material.
