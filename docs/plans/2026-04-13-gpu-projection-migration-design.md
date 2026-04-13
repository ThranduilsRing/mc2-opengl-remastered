# GPU Projection Migration — Design & Feasibility

**Date:** 2026-04-13
**Goal:** Migrate MC2's CPU-side vertex projection to GPU-side MVP transforms using a parallel "strangler fig" approach — growing the new pipeline alongside the old, one object type at a time.

## Why This Matters

MC2's renderer is a 1999-era CPU projector using OpenGL as a 2D blitter. Every vertex is transformed to screen-space on the CPU via `Camera::projectZ()` before the GPU ever sees it. This means the GPU has no knowledge of 3D world positions, which is the root cause of multiple bugs and workarounds:

- **Overlay shadows:** Objects/overlays can't sample shadow maps because they don't have world position. Required building an entire post-process shadow pass with depth reconstruction (~200 lines of workaround).
- **MRT normal buffer:** Exists solely to distinguish terrain from non-terrain because both can't just sample shadows directly.
- **Shadow banding on rotation:** View-dependent tessellation means shadow edges shift with camera angle.
- **Post-process applies to HUD:** Can't distinguish 3D scene from 2D UI because both are screen-space.
- **Shoreline detection:** Water alpha flag doesn't reach G-buffer reliably; world-space detection would be direct.

GPU projection eliminates these workarounds by giving every fragment its actual world position.

## Architecture Overview

### Current Pipeline (CPU-Projected)

```
Game Logic (MC2 world space: x=east, y=north, z=elev)
    |
    v
CPU coordinate swizzle (x=-x, y=z, z=y) — 19 sites across 4 actor files
    |
    v
CPU projection (Camera::projectZ / TG_Shape::MultiTransformShape) — 96 call sites
    |
    v
Screen-space vertices submitted to GL (gos_VERTEX with pixel x,y)
    |
    v
GL: trivial 2D ortho mapping to NDC (no 3D transform on GPU)
```

### Target Pipeline (GPU-Projected)

```
Game Logic (MC2 world space)
    |
    v
Model matrix per object (swizzle baked into one mat4)
    |
    v
Upload uniforms: modelMatrix + viewProjection (from Camera)
    |
    v
Vertex Shader: gl_Position = VP * model * vertexPos
    |
    v
Fragment Shader: has worldPos — can sample shadows, do lighting directly
```

### Migration Strategy: Strangler Fig

Both pipelines coexist. Objects migrate one type at a time. At each step:
1. Add GPU path alongside CPU path (flag-controlled)
2. Validate with `--validate` mode (crash detection, shader errors)
3. Visual check by user
4. Disable CPU path for that object type
5. Repeat for next type

The game works at every intermediate state. Each migration is independently revertible.

## What Already Exists

The nifty-mendeleev branch has already proven GPU projection works in this codebase:

- **Terrain:** Full tessellation pipeline with `terrainMVP` matrix. World-space vertices on GPU, shader samples shadow maps directly.
- **Shadow casting:** Direct `glUseProgram` + `glDrawElements` bypassing the material system. Uploads matrices, draws with existing GPU buffers.
- **Post-process infrastructure:** FBOs, depth texture, inverse VP matrix — all available.
- **Validation mode:** `--validate` flag for autonomous crash detection.

## Engine Analysis

### Two Separate Object Pipelines (NOT one)

**Critical finding:** Mechs, buildings, and vehicles do NOT use the MLR clipper. They use the completely separate `TG_Shape` pipeline:

| Objects | Pipeline | Complexity |
|---------|----------|------------|
| Mechs (mech3d.cpp) | TG_Shape → MultiTransformShape → renderLists | Medium — cleanest transform code |
| Buildings/trees (bdactor.cpp) | TG_Shape → MultiTransformShape → renderLists | Medium — 5 swizzle sites |
| Vehicles (gvactor.cpp) | TG_Shape → MultiTransformShape → renderLists | Medium — 6 swizzle sites |
| Generic actors (genactor.cpp) | TG_Shape → MultiTransformShape → renderLists | Easy — 1 swizzle site |
| Particles/FX (gosFX) | MLR clipper → sorter → gos_DrawTriangles | Hard — has alpha sorting |
| Overlays (roads, water) | Direct gos_DrawTriangles | Easy — simple textured quads |
| Terrain | Already GPU-projected (tessellation) | Done |
| HUD/UI | Direct gos_DrawTriangles (screen space) | Keep as-is |

### TG_Shape Pipeline (Objects)

The object render chain in `mclib/tgl.cpp`:

1. `TG_Shape::MultiTransformShape()` — CPU multiplies every vertex by `shapeToClip` matrix, does perspective divide, viewport mapping. This is what we replace.
2. `TG_Shape::Render()` — per visible face, assembles GOSVertex (already screen-space), submits to `mcTextureManager::addVertices()`.
3. `mcTextureManager::renderLists()` — batches by texture, draws solid before alpha.

**Key observation:** The `shapeToClip` matrix already exists as `shapeToWorld * worldToClipMatrix`. We just need to split it: upload `shapeToWorld` (model) and `worldToClip` (VP) separately, let the vertex shader recombine.

### MLR Clipper (Particles Only)

The MLR clipper does three things:
1. **Transform pipeline** — MVP on CPU. GPU replaces this trivially.
2. **6-plane frustum clipping** — Sutherland-Hodgman triangle splitting. GPU hardware does this natively.
3. **Alpha depth sorting** — per-triangle painter's algorithm for transparency. **Only real concern.**

However: MC2 particles (PPC blasts, explosions, jet trails) are mostly additive-blended (`GL_ONE, GL_ONE`), which is commutative — draw order doesn't matter. Non-additive particles (smoke?) may show minor artifacts but are acceptable.

### The Coordinate Swizzle

MC2 world space (x=east, y=north, z=elevation, Z-up) ≠ Stuff/MLR camera space (x=left, y=up, z=forward, Y-up).

The swizzle is: `camera.x = -world.x, camera.y = world.z, camera.z = world.y`

Currently applied as explicit component shuffles at 19 sites across 4 files. Can be consolidated into a single basis-change matrix:

```
swizzleMatrix = | -1  0  0  0 |
                |  0  0  1  0 |
                |  0  1  0  0 |
                |  0  0  0  1 |
```

Apply once at the boundary: `viewMatrix = cameraView * swizzleMatrix`. Then all objects submit raw MC2 world coordinates.

## Phased Implementation Plan

### Phase 0: Foundation (1 session)

**Upload VP matrix as a global uniform.**

- Extract `worldToClip` from `Camera` (it already exists as `worldToClipMatrix`)
- Bake the swizzle into it: `VP = worldToClipMatrix * swizzleMatrix`
- Upload as uniform to all shaders via `gosPostProcess` or similar global
- Does NOT change any rendering behavior — just makes the matrix available
- Verify with `--validate`

**Files:** `mclib/camera.cpp`, `GameOS/gameos/gameos_graphics.cpp`

### Phase 1: Mechs on GPU (2-3 sessions)

**Migrate mech rendering to GPU projection.**

Mechs are the best first candidate: most visually prominent, cleanest transform code, and already have GPU vertex buffers from shadow casting work.

- Add `modelMatrix` uniform to `object_tex` vertex shader
- In `TG_Shape::Render()`, add flag: if GPU path, submit world-space vertices + model matrix instead of screen-space
- Keep `MultiTransformShape()` CPU path as fallback (compile flag or runtime toggle)
- Mech model matrix = existing `shapeToWorld` from `Mech3DAppearance`
- Fragment shader gains `worldPos` varying → can sample shadow map directly
- Remove mech-specific post-process shadow workaround

**Files:** `mclib/mech3d.cpp`, `mclib/tgl.cpp`, `shaders/object_tex.vert`, `shaders/object_tex.frag`

**Validation:** Mechs render at correct positions, cast and receive shadows, no Z-fighting.

### Phase 2: Buildings & Vehicles (1-2 sessions each)

Same pattern as Phase 1, applied to `bdactor.cpp` and `gvactor.cpp`.

- Each has its own swizzle sites (5 in bdactor, 6 in gvactor)
- Replace with model matrix upload
- Validate independently

**Files:** `mclib/bdactor.cpp`, `mclib/gvactor.cpp`, `mclib/genactor.cpp`

### Phase 3: Overlays on GPU (1 session)

**Migrate road/cement overlays to GPU projection.**

Overlays are simple textured quads. Currently rendered through `gos_tex_vertex` shader with no world position.

- Add VP matrix to `gos_tex_vertex.vert`
- Submit overlay quads in world space instead of screen space
- Fragment shader samples shadow map directly → **overlay shadows fixed**
- Remove MRT alpha flag workaround for shadow pass

**Files:** `mclib/quad.cpp` (overlay rendering), `shaders/gos_tex_vertex.vert/frag`

### Phase 4: Cleanup (1-2 sessions)

**Remove workarounds that GPU projection made unnecessary.**

- Remove or simplify post-process shadow pass (objects now shadow themselves)
- Remove MRT normal buffer alpha flag (no longer needed to distinguish terrain/non-terrain)
- Remove inverse VP matrix depth reconstruction (world pos available directly)
- Simplify `gosPostProcess` pipeline
- Consider separating scene FBO from HUD FBO (post-process on HUD bug)

**Estimated removal:** ~300-500 lines of workaround code.

### Phase 5: Particles (optional, lowest priority)

**Migrate gosFX effects to GPU projection.**

- Modify gosFX renderers (CardCloud, DebrisCloud, etc.) to submit world-space quads
- Keep additive blending (order-independent, no sorting needed)
- For the few non-additive effects: either accept minor artifacts or add simple distance sort on CPU
- Can defer MLR clipper replacement indefinitely — particles work fine as-is

## Risk Assessment

| Phase | Risk | Mitigation |
|-------|------|------------|
| Phase 0 (VP uniform) | None — no behavior change | Validation mode |
| Phase 1 (mechs) | Medium — could break mech positioning, Z-order | Runtime toggle, `--validate`, shadow casting already proves the matrix math works |
| Phase 2 (buildings) | Low — same pattern as mechs | Incremental, one type at a time |
| Phase 3 (overlays) | Low — simple geometry | Direct test: do overlays have shadows? |
| Phase 4 (cleanup) | Medium — removing load-bearing workarounds | Only after all objects confirmed working on GPU path |
| Phase 5 (particles) | Low impact — particles look fine as-is | Optional, defer indefinitely |

## Key Technical Details

### Camera Matrix Extraction

`Camera::setCameraOrigin()` builds `worldToCameraMatrix` (view) and `Camera::setOrthogonal()` builds `cameraToClip` (projection). Both are `Stuff::Matrix4D`. The combined `worldToClipMatrix` already exists.

Need to extract as a `float[16]` and upload via `glUniformMatrix4fv()`. The Stuff `Matrix4D` is row-major — use `GL_TRUE` for transpose parameter (same as existing `terrainMVP` pattern).

### Vertex Format Change

Current `gos_VERTEX`: `x, y, z, rhw, argb, frgb, u, v` — screen-space coords with reciprocal W.

GPU-projected vertices need world-space `x, y, z` instead. Two approaches:
1. **New vertex format** with world-space positions (cleaner, more work)
2. **Reuse existing format**, put world coords in x/y/z fields, ignore rhw (quicker, hackier)

Recommend approach 2 for initial migration (less plumbing), refactor to approach 1 during cleanup.

### Object Model Matrix Source

Each object type already computes a world transform:
- Mechs: `Mech3DAppearance` has `rotation` (yaw) + `position` (world). Build model matrix from these.
- Buildings: `BldgAppearance` has `rotation` + `position`. Same pattern.
- The existing `xlatPosition` swizzle code in each actor type IS the model matrix, just expressed as component shuffles instead of a matrix multiply.

### Shadow Map Direct Sampling

Once objects have `worldPos` in the fragment shader, shadow sampling is identical to terrain:
```glsl
vec4 lightSpacePos = lightSpaceMatrix * vec4(worldPos, 1.0);
vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
float shadow = calcShadow(projCoords); // existing Poisson PCF in shadow.hglsl
```

This replaces the entire post-process shadow reconstruction pipeline for any object on the GPU path.

## Token Budget Estimate

| Phase | Sessions | Token-Heavy? |
|-------|----------|-------------|
| Phase 0 | 1 | Light — small uniform plumbing |
| Phase 1 | 2-3 | Medium — shader changes + TG_Shape modifications |
| Phase 2 | 2-3 | Light — repeat Phase 1 pattern |
| Phase 3 | 1 | Light — overlay quads are simple |
| Phase 4 | 1-2 | Light — mostly deleting code |
| Phase 5 | 1-2 | Medium — gosFX is complex but optional |
| **Total** | **8-12 sessions** | |

Compare to token cost of continuing with workarounds: every new rendering feature that needs world position requires another depth-reconstruction workaround (~1-2 sessions each). The migration pays for itself after 3-4 features.

## Success Criteria

- All object types render at correct positions with GPU projection
- Objects receive shadows via direct shadow map sampling (no post-process reconstruction)
- Post-process shadow pass removed or simplified to HUD-only
- Validation mode confirms zero shader errors, zero GL errors
- Frame time equal or better than CPU path (GPU projection is faster than CPU)
- No visual regressions visible at normal gameplay zoom
