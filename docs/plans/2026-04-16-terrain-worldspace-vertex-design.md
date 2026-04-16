# Terrain World-Space Vertex Migration — Design

**Date:** 2026-04-16
**Goal:** Eliminate `eye->projectZ()` for terrain vertices by removing the single remaining consumer of CPU-projected screen-space positions in the tessellation pipeline.

## Context

The terrain tessellation pipeline already operates in world space. Vertices carry two position streams:
- `pos` (location 0, `gos_VERTEX.x/y/z/rhw`) — CPU-projected screen-space coords, built each frame via `eye->projectZ()`
- `worldPos` (location 4, extras VBO) — MC2 world coords (x=east, y=north, z=elev), built from `v->vx, v->vy, v->elevation`

The TES uses `tcs_WorldPos` (from the extras) for all real work: displacement, Phong smoothing, shadow sampling, fog, and final `terrainMVP` projection. The screen-space `pos` survives in `gl_in[].gl_Position` for exactly one computation: `UndisplacedDepth` (TES lines 67–75), which gives overlays a reference depth matching the non-tessellated terrain surface so they can depth-test correctly.

Once `UndisplacedDepth` is derived from world space instead, `projectZ()` has no remaining consumers in the terrain path.

**`shadow_terrain.tese` is already clean** — uses `tcs_WorldPos` exclusively, no `gl_in[].gl_Position`, no changes needed.

## Staged Implementation

The migration is broken into four discrete steps. Each step compiles and runs correctly in isolation. `projectZ()` remains active until the final step, so any individual step can be validated before proceeding.

---

### Step 1 — TES: derive `UndisplacedDepth` from world space

**File:** `shaders/gos_terrain.tese`

Replace the `UndisplacedDepth` block (lines 67–75):

```glsl
// BEFORE — uses gl_in[].gl_Position (CPU screen-space)
{
    vec4 interpScreenPos = bary.x * gl_in[0].gl_Position
                         + bary.y * gl_in[1].gl_Position
                         + bary.z * gl_in[2].gl_Position;
    vec4 ndcBasic = mvp * interpScreenPos;
    UndisplacedDepth = (ndcBasic.z / ndcBasic.w) * 0.5 + 0.5;
}
```

With:

```glsl
// AFTER — uses tcs_WorldPos through terrainMVP (same chain as displaced path)
{
    vec3 undisplacedWorldPos = bary.x * tcs_WorldPos[0]
                             + bary.y * tcs_WorldPos[1]
                             + bary.z * tcs_WorldPos[2];
    vec4 uclip = terrainMVP * vec4(undisplacedWorldPos, 1.0);
    float urhw = 1.0 / uclip.w;
    float uscreenZ = uclip.z * urhw;
    // mvp maps screen coords → NDC; z is independent of x/y in this ortho-like pass
    vec4 undc = mvp * vec4(0.0, 0.0, uscreenZ, 1.0);
    UndisplacedDepth = undc.z * 0.5 + 0.5;
}
```

`terrainMVP`, `terrainViewport`, and `mvp` are already in scope. No new uniforms.

**Why this works:** `terrainMVP` is built from the same camera matrices `projectZ()` uses, so the depth values are mathematically equivalent. The `pos.w` divide in the VS has always been `1/rhw` which cancels — the two paths produce identical depths in the absence of floating-point divergence.

**Debug hook (Step 1):** Add a uniform `int undisplacedDepthMode` (default 0 = new world-space path, 1 = old screen-space path). Gate the two blocks on this uniform for the first build. Toggle via **RAlt+9** hotkey (currently assigned to SSAO — repurpose it temporarily since SSAO is disabled by default and this debug is short-lived; restore SSAO binding after validation). If any overlay z-fighting appears that differs between modes, it will be immediately visible on road/cement edges.

```glsl
// Temporary — remove after validation
if (undisplacedDepthMode == 1) {
    // OLD path (screen-space)
    vec4 interpScreenPos = bary.x * gl_in[0].gl_Position + ...;
    vec4 ndcBasic = mvp * interpScreenPos;
    UndisplacedDepth = (ndcBasic.z / ndcBasic.w) * 0.5 + 0.5;
} else {
    // NEW path (world-space)
    ...
}
```

Remove the `undisplacedDepthMode` uniform and the old block once validated.

---

### Step 2 — Vertex Shader: stop using `pos` for `gl_Position`

**File:** `shaders/gos_terrain.vert`

`gl_Position` in the VS is used only to supply `gl_in[].gl_Position` in the TCS/TES. After Step 1, `gl_in[].gl_Position` is no longer read. Change the VS to pass world coords through `pos.xyz` as-is:

```glsl
// BEFORE
vec4 p = mvp * vec4(pos.xyz, 1);
gl_Position = p / pos.w;

// AFTER — pos.xyz now expected to carry world coords, rhw=1.0
// gl_in[].gl_Position no longer consumed by TES
gl_Position = vec4(pos.xyz, 1.0);
```

At this step the CPU still fills `gVertex.x/y/z/rhw` with screen-space values, so `gl_Position` will be wrong (screen coords treated as world coords). That's fine — it's unused after Step 1. This step just removes the dead `mvp * pos` computation.

**Debug hook (Step 2):** No additional hook needed — this is a dead-code removal. Validate simply by confirming terrain still renders identically (TES overrides `gl_Position` unconditionally).

---

### Step 3 — CPU: submit world coords in `gos_VERTEX.x/y/z/rhw`

**Files:** `mclib/quad.cpp` (terrain triangle submission, ~8–10 sites)

Change all terrain `gos_VERTEX` construction from screen-space to world-space:

```cpp
// BEFORE (screen-space from projectZ)
gVertex[0].x   = v0->px;
gVertex[0].y   = v0->py;
gVertex[0].z   = v0->pz;
gVertex[0].rhw = v0->pw;

// AFTER (MC2 world coords)
gVertex[0].x   = v0->vx;
gVertex[0].y   = v0->vy;
gVertex[0].z   = v0->pVertex->elevation;
gVertex[0].rhw = 1.0f;
```

Applies to: solid terrain and alpha terrain (cement overlays, craters) paths — specifically `gVertex` built from `v->px/py/pz/pw`. **Water is excluded:** water vertices use a separate field set (`wx/wy/wz/ww`) written by their own `projectZ()` calls inside `quad.cpp` (with wave-height offsets), and water `gos_VERTEX` is assembled from those fields, not from `px/py/pz/pw`. The extras VBO (`fillTerrainExtra`) is already using `vx/vy/elevation` and is unchanged.

`projectZ()` is still called at this step (results written to `px/py/pz/pw` but no longer consumed). This confirms the vertex builder and shader pipeline are correct before removing the call.

**Debug hook (Step 3):** Enable `MC2_DEBUG_TERRAIN_VTXSRC=1` env var to print the first 3 terrain vertices each frame (world coords submitted vs. screen coords from `projectZ`, for sanity comparison). Remove after validation.

```cpp
// In quad.cpp, first triangle submission per frame only:
static bool vtxDebug = getenv("MC2_DEBUG_TERRAIN_VTXSRC") != nullptr;
if (vtxDebug) {
    static int frame = 0; if (++frame <= 3)
        printf("[TERRAIN_VTX] world=(%.1f,%.1f,%.1f) screen=(%.1f,%.1f,%.4f)\n",
               v0->vx, v0->vy, v0->pVertex->elevation, v0->px, v0->py, v0->pz);
}
```

---

### Step 4 — CPU: remove `projectZ()` from terrain vertex loop

**File:** `mclib/terrain.cpp` (vertex transform loop, ~line 1060)

Remove the `projectZ()` call and the `px/py/pz/pw` writes:

```cpp
// REMOVE this block:
Stuff::Vector3D vertex3D(currentVertex->vx, currentVertex->vy, currentVertex->pVertex->elevation);
inView = eye->projectZ(vertex3D, screenPos);
currentVertex->px = screenPos.x;
currentVertex->py = screenPos.y;
currentVertex->pz = screenPos.z;
currentVertex->pw = screenPos.w;

// AND the off-screen sentinel:
// currentVertex->px = currentVertex->py = 10000.0f;
// currentVertex->pz = -0.5f; currentVertex->pw = 0.5f;
```

Keep: the frustum visibility test (angular dot-product check), the `hazeFactor` fog computation, and the `clipInfo` flag — all still needed to skip building triangles for invisible tiles.

**`inView` / `leastZ/mostZ` dependency — must resolve before Step 4:**

`inView` is the return value of `projectZ()`. After the call is removed it no longer exists. Its outputs are consumed in two ways:

1. **`leastZ/mostZ/leastW/mostW/leastWY/mostWY` → `setInverseProject()`:** These track the linear range of screen-space z/w across all visible vertices and are passed to `eye->setInverseProject()` which stores `startZInverse, zPerPixel, startWInverse, wPerPixel`. Those parameters feed `Camera::inverseProjectZ()`, a CPU depth-estimation helper.

2. **`v->pz` in `Camera::inverseProject()` (camera.cpp:~790):** The mouse picking function reads `closestTiles[i]->vertices[j]->pz` directly to find the closest tile under the cursor. If `projectZ()` is removed, `pz` goes stale and picking breaks.

Both `inverseProject()` and `inverseProjectZ()` are legacy CPU mouse-picking helpers that estimate world position from a screen coordinate. With the tessellated renderer these should be replaced by a proper raycast against the terrain elevation grid (already available via `Terrain::getTerrainElevation()`), but that is a separate task.

**For Step 4 specifically:** either (a) replace `inverseProject`/`inverseProjectZ` with an elevation-grid raycast before removing `projectZ()`, or (b) keep a minimal `projectZ()` call only for the picking path (separate from the rendering path). Option (a) is cleaner and is already the right direction for GPU projection; option (b) is safe and reversible. Document the chosen approach in the implementation plan.

**Validation:** Tracy profiler should show `Camera.UpdateRenderers` dropping by the per-vertex `projectZ()` cost across all visible terrain vertices (typically 500K+ per frame in Wolfman mode). Expect 1–3ms reduction in the 6ms budget.

---

## Acceptance Criteria

1. Road/cement overlays have no new z-fighting vs. terrain after each step
2. Mission markers still sit correctly on terrain surface
3. Water surface renders correctly (has its own `projectZ()` path in `quad.cpp` using `wx/wy/wz/ww`, unaffected by this change)
4. Mouse terrain picking still works (blocked on `inView` resolution in Step 4)
5. Tracy shows `Camera.UpdateRenderers` reduction after Step 4
6. RAlt+9 toggle (Step 1 debug) shows no visible difference between old and new `UndisplacedDepth` modes

## Files Changed

| File | Change |
|------|--------|
| `shaders/gos_terrain.tese` | Step 1: new `UndisplacedDepth` + debug toggle uniform |
| `shaders/gos_terrain.vert` | Step 2: `gl_Position = vec4(pos.xyz, 1.0)` |
| `mclib/quad.cpp` | Step 3: world coords in `gVertex.x/y/z/rhw` for all terrain paths |
| `mclib/terrain.cpp` | Step 4: remove `projectZ()` + `px/py/pz/pw` writes from vertex loop |
| `GameOS/gameos/gameos_graphics.cpp` | Step 1: add `RAlt+9` hotkey for `undisplacedDepthMode` toggle (temporarily repurposes SSAO key; restore after validation) |

## Out of Scope

- Water `projectZ()` calls in `quad.cpp` — water vertices use separate `wx/wy/wz/ww` fields from wave-animated `projectZ()` calls; the terrain.cpp removal does not affect them
- Mouse picking (`Camera::inverseProject`) — depends on `v->pz`; must be replaced with elevation-grid raycast before Step 4 fully lands, tracked as a separate task
- Old-style `gos_DrawTriangles` screen-space paths (particles, HUD)
- Frustum culling — stays on CPU (Option B/C are separate)
- GPU terrain chunk streaming (Option B is separate)
