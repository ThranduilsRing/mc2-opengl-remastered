# Shadow Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate camera-move shadow stutter and reduce view-dependent shadow edge banding on the nifty-mendeleev terrain renderer.

**Architecture:** Two targeted fixes to the existing static shadow accumulation system. Task 1 adds a CPU-side coverage grid so the static shadow pre-pass only fires for world cells not yet rendered — once a cell is covered it is never re-rendered, eliminating stutter for explored areas. Task 2 enables Phong tessellation smoothing in the shadow depth pass so the shadow map geometry matches the Phong-smoothed positions used when sampling, closing the depth-bias gap that causes banding.

**Tech Stack:** OpenGL 4.2, GLSL 4.20, C++14, MSYS2/CMake `--config RelWithDebInfo`

**2026-04-18 review update:** The camera-cell trigger described later in Task 1 is superseded. The correct implementation shape is: gather the terrain batches that would be submitted this frame, compute per-batch world-space bounds from `gos_TERRAIN_EXTRA`, and only run the static pre-pass if at least one submitted batch intersects uncovered coverage cells. Coverage marking should also be done per batch, not from one frame-wide bounding box.

**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`
**Build skill:** `/mc2-build` then `/mc2-deploy`

---

## Background the implementer needs

### Why there is stutter today

The static terrain shadow map uses depth accumulation: the FBO is cleared only on the first frame, then new terrain batches are appended (no clear) as the camera pans. This works because `GL_LESS` keeps the nearest depth across all writes, and the light-space matrix is world-fixed, so any camera angle produces the same depth values for the same terrain.

The problem: in `mclib/txmmgr.cpp` the shadow pre-pass fires every time the camera moves more than 100 world units — even if all visible terrain was already in the shadow map from a previous pass. That constant re-rendering is the stutter source.

The fix: maintain a CPU-side `bool shadowCoverageGrid_[32][32]` in `gosPostProcess`. Before the shadow pass, walk the visible terrain batches, compute per-batch world-space bounds from `gos_TERRAIN_EXTRA`, and check whether any batch intersects uncovered cells. If every visible batch is already covered, skip the GPU pass entirely. After each pass, mark coverage per batch as it is rendered. Once the player has panned across the full map, the static shadow pass stops running forever (until shadow reset).

### Why there is banding on camera rotation

The main terrain TES (`gos_terrain.tese`) applies Phong tessellation smoothing: each tessellated vertex is projected onto the tangent planes of its three patch corners and blended back. The result is a smoother curved surface. The fragment's `WorldPos` output is this Phong-smoothed position.

The shadow terrain TES (`shadow_terrain.tese`) does NOT do Phong smoothing — it uses raw linear barycentric interpolation. The CPU passes `tessDisplace.x = 0.0` to the shadow pass (see `drawShadowBatchTessellated` in `gameos_graphics.cpp`).

Effect: the shadow map stores depth for non-Phong positions, but `calcShadow()` in `shadow.hglsl` looks up shadow at Phong-smoothed worldPos. The offset varies with surface curvature and which barycentric sample is being shaded — and because the tessellation pattern is deterministic-but-camera-angle-dependent in subtle ways, the offset shifts as the camera rotates. The current slope-bias `max(0.005 * (1 - NdotL), 0.002)` can absorb small errors but not the full Phong displacement on steep terrain.

The fix: add the identical Phong block from `gos_terrain.tese` into `shadow_terrain.tese`, and change the `tessDisplace.x` passed to the shadow pass from `0.0f` to `terrain_phong_alpha_`.

### Coordinate space reminder

MC2 world space: `x = east`, `y = north`, `z = elevation`.
Camera (Stuff-space): `cp.x = left (west+)`, `cp.y = elevation`, `cp.z = forward (north+)`.
Convert camera position to MC2 world: `mc2X = -cp.x`, `mc2Y = cp.z`.

The coverage grid is indexed in MC2 world XY. Map is centred at origin; half-extent is `pp->getMapHalfExtent()`.

### Coverage invalidation rules

Coverage is only valid while all of the following remain true:

- Same map and same `mapHalfExtent_`
- Same static light-space matrix inputs
- Shadows remain enabled without resetting the static shadow resources

Coverage must be reset when any of these occur:

- `initShadows()` recreates or clears the static shadow map
- A new map is loaded and `gos_SetMapHalfExtent(...)` changes extent
- `RAlt+F3` turns shadows back on after being off
- Any future runtime feature changes the static light matrix inputs

### Key files

| File | Role |
|------|------|
| `GameOS/gameos/gos_postprocess.h` | gosPostProcess class declaration |
| `GameOS/gameos/gos_postprocess.cpp` | Coverage grid implementation, initShadows() |
| `GameOS/gameos/gameos_graphics.cpp` | `drawShadowBatchTessellated()`, global gos_* wrappers |
| `mclib/txmmgr.cpp` | Shadow pre-pass trigger (~line 1140) |
| `shaders/shadow_terrain.tese` | Shadow depth TES — add Phong block here |
| `shaders/include/shadow.hglsl` | `calcShadow()` — bias tuning fallback only |

### gos_TERRAIN_EXTRA struct layout

Defined by usage in `mclib/quad.cpp:79-84`:
```cpp
struct gos_TERRAIN_EXTRA {
    float wx, wy, wz;   // MC2 world position
    float nx, ny, nz;   // MC2 world normal
};
```
Stride = 24 bytes. Attribute layout in GPU: location 4 = worldPos (offset 0), location 5 = worldNorm (offset 12).

---

## Task 1: Shadow coverage grid — eliminate re-render stutter

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.h`
- Modify: `GameOS/gameos/gos_postprocess.cpp`
- Modify: `GameOS/gameos/gameos_graphics.cpp`
- Modify: `mclib/txmmgr.cpp`

### Step 1.1 — Add coverage grid fields to `gos_postprocess.h`

In the `private:` section, after the `float mapHalfExtent_;` line (~line 154):

```cpp
    // Shadow coverage grid — 32×32 cells over the world map.
    // Each cell is (2*mapHalfExtent/32) world units wide.
    // Once a cell is rendered into the shadow FBO it is never re-rendered.
    static const int kCoverageGridSize = 32;
    bool shadowCoverageGrid_[kCoverageGridSize][kCoverageGridSize];
```

In the `public:` section, after `void setMapHalfExtent(...)`:

```cpp
    bool isShadowRegionCovered(float worldX, float worldY) const;
    bool isShadowBoundsCovered(float minX, float minY, float maxX, float maxY) const;
    void markShadowRegionCovered(float minX, float minY, float maxX, float maxY);
    void resetShadowCoverage();
```

- [ ] **Step 1.1:** Add coverage grid fields and public method declarations to `gos_postprocess.h` as shown above.

### Step 1.2 — Implement coverage methods in `gos_postprocess.cpp`

Add these three functions anywhere after `gosPostProcess::buildStaticLightMatrix` (~line 1240):

```cpp
void gosPostProcess::resetShadowCoverage() {
    memset(shadowCoverageGrid_, 0, sizeof(shadowCoverageGrid_));
}

bool gosPostProcess::isShadowRegionCovered(float worldX, float worldY) const {
    if (mapHalfExtent_ <= 0.0f) return false;
    float nx = (worldX + mapHalfExtent_) / (2.0f * mapHalfExtent_);
    float ny = (worldY + mapHalfExtent_) / (2.0f * mapHalfExtent_);
    int cx = (int)(nx * kCoverageGridSize);
    int cy = (int)(ny * kCoverageGridSize);
    if (cx < 0 || cx >= kCoverageGridSize || cy < 0 || cy >= kCoverageGridSize)
        return false;  // outside map — let the pass run to write border-colour depth
    return shadowCoverageGrid_[cy][cx];
}

bool gosPostProcess::isShadowBoundsCovered(float minX, float minY, float maxX, float maxY) const {
    if (mapHalfExtent_ <= 0.0f) return false;
    float ext2 = 2.0f * mapHalfExtent_;
    int x0 = (int)(((minX + mapHalfExtent_) / ext2) * kCoverageGridSize);
    int y0 = (int)(((minY + mapHalfExtent_) / ext2) * kCoverageGridSize);
    int x1 = (int)(((maxX + mapHalfExtent_) / ext2) * kCoverageGridSize);
    int y1 = (int)(((maxY + mapHalfExtent_) / ext2) * kCoverageGridSize);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= kCoverageGridSize ? kCoverageGridSize - 1 : x1;
    y1 = y1 >= kCoverageGridSize ? kCoverageGridSize - 1 : y1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (!shadowCoverageGrid_[y][x])
                return false;
    return true;
}

void gosPostProcess::markShadowRegionCovered(float minX, float minY, float maxX, float maxY) {
    if (mapHalfExtent_ <= 0.0f) return;
    float ext2 = 2.0f * mapHalfExtent_;
    // Convert bounding box corners to grid indices with a 1-cell margin
    int x0 = (int)(((minX + mapHalfExtent_) / ext2) * kCoverageGridSize) - 1;
    int y0 = (int)(((minY + mapHalfExtent_) / ext2) * kCoverageGridSize) - 1;
    int x1 = (int)(((maxX + mapHalfExtent_) / ext2) * kCoverageGridSize) + 1;
    int y1 = (int)(((maxY + mapHalfExtent_) / ext2) * kCoverageGridSize) + 1;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= kCoverageGridSize ? kCoverageGridSize - 1 : x1;
    y1 = y1 >= kCoverageGridSize ? kCoverageGridSize - 1 : y1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            shadowCoverageGrid_[y][x] = true;
}
```

Also hook `resetShadowCoverage()` into `initShadows()`. Near the end of `initShadows()` (around line 1103 where `staticLightMatrixBuilt_ = false` is set), add:

```cpp
    staticLightMatrixBuilt_ = false;
    resetShadowCoverage();  // <-- add this line
```

- [ ] **Step 1.2:** Implement the four coverage methods in `gos_postprocess.cpp` and call `resetShadowCoverage()` from `initShadows()`.

### Step 1.3 — Add global gos_* wrappers in `gameos_graphics.cpp`

Find the block of `gos_` wrappers around `gos_BeginShadowPrePass` / `gos_EndShadowPrePass` (~line 4080). Add immediately after them:

```cpp
bool gos_IsShadowRegionCovered(float worldX, float worldY) {
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->isShadowRegionCovered(worldX, worldY);
}
bool gos_AreShadowBoundsCovered(float minX, float minY, float maxX, float maxY) {
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->isShadowBoundsCovered(minX, minY, maxX, maxY);
}
void gos_MarkShadowRegionCovered(float minX, float minY, float maxX, float maxY) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->markShadowRegionCovered(minX, minY, maxX, maxY);
}
void gos_ResetShadowCoverage() {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->resetShadowCoverage();
}
```

Find the extern declarations block in `gameos_graphics.cpp` (or the matching header where `gos_BeginShadowPrePass` is declared — search for `gos_BeginShadowPrePass` in `.h` files). Add matching declarations there:

```cpp
bool gos_IsShadowRegionCovered(float worldX, float worldY);
bool gos_AreShadowBoundsCovered(float minX, float minY, float maxX, float maxY);
void gos_MarkShadowRegionCovered(float minX, float minY, float maxX, float maxY);
void gos_ResetShadowCoverage();
```

- [ ] **Step 1.3:** Add `gos_IsShadowRegionCovered`, `gos_AreShadowBoundsCovered`, `gos_MarkShadowRegionCovered`, and `gos_ResetShadowCoverage` wrappers in `gameos_graphics.cpp` and their declarations in the matching header.

### Step 1.4 — Replace camera-distance check in `txmmgr.cpp`

In `mclib/txmmgr.cpp` find the shadow accumulation block (~line 1140). The existing block opens with:

```cpp
{
    static float lastShadowCamX = 1e9f, ...
    float shadowCamDist = ...;
    float shadowCacheThreshold = 100.0f;

    if (gos_IsTerrainTessellationActive() && shadowCamDist > shadowCacheThreshold * shadowCacheThreshold) {
```

Replace the **entire scope** from the opening `{` down through the closing `}` that wraps `gos_EndShadowPrePass()` with this two-phase implementation:

```cpp
{
    // Phase 1: determine if any visible terrain batch has uncovered cells.
    // Walk the vertex node list once, compute per-batch MC2 world XY bounds from
    // gos_TERRAIN_EXTRA (wx = MC2 east, wy = MC2 north), and ask the coverage grid
    // if those bounds are already fully covered.
    bool anyUncovered = false;
    if (gos_IsTerrainTessellationActive()) {
        for (long si = 0; si < nextAvailableVertexNode && !anyUncovered; si++) {
            if (!((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
                  (masterVertexNodes[si].flags & MC2_ISTERRAIN) &&
                  masterVertexNodes[si].extras))
                continue;
            int extraCount = masterVertexNodes[si].currentExtra
                ? (int)(masterVertexNodes[si].currentExtra - masterVertexNodes[si].extras)
                : 0;
            if (extraCount <= 0) continue;

            float bMinX = 1e9f, bMinY = 1e9f, bMaxX = -1e9f, bMaxY = -1e9f;
            const gos_TERRAIN_EXTRA* ex = masterVertexNodes[si].extras;
            for (int e = 0; e < extraCount; e++) {
                if (ex[e].wx < bMinX) bMinX = ex[e].wx;
                if (ex[e].wx > bMaxX) bMaxX = ex[e].wx;
                if (ex[e].wy < bMinY) bMinY = ex[e].wy;
                if (ex[e].wy > bMaxY) bMaxY = ex[e].wy;
            }
            if (!gos_AreShadowBoundsCovered(bMinX, bMinY, bMaxX, bMaxY))
                anyUncovered = true;
        }
    }

    // Phase 2: if any batch is uncovered (or this is the first frame), run the
    // static shadow pre-pass and mark each batch covered after rendering it.
    bool firstFrame = !gos_StaticLightMatrixBuilt();
    if (gos_IsTerrainTessellationActive() && (firstFrame || anyUncovered)) {
        ZoneScopedN("Shadow.StaticAccum");
        TracyGpuZone("Shadow.StaticAccum");

        if (firstFrame) {
            gos_BuildStaticLightMatrix();
            gos_MarkStaticLightMatrixBuilt();
        }

        gos_BeginShadowPrePass(firstFrame);

        for (long si = 0; si < nextAvailableVertexNode; si++) {
            if (!((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
                  (masterVertexNodes[si].flags & MC2_ISTERRAIN) &&
                  masterVertexNodes[si].vertices &&
                  masterVertexNodes[si].extras))
                continue;

            DWORD totalVerts = masterVertexNodes[si].numVertices;
            if (masterVertexNodes[si].currentVertex !=
                (masterVertexNodes[si].vertices + masterVertexNodes[si].numVertices))
                totalVerts = masterVertexNodes[si].currentVertex - masterVertexNodes[si].vertices;

            int extraCount = masterVertexNodes[si].currentExtra
                ? (int)(masterVertexNodes[si].currentExtra - masterVertexNodes[si].extras)
                : 0;

            if (totalVerts <= 0 || extraCount <= 0) continue;

            gos_SetRenderState(gos_State_Texture,
                masterTextureNodes[masterVertexNodes[si].textureIndex].get_gosTextureHandle());

            gos_DrawShadowBatchTessellated(
                masterVertexNodes[si].vertices, totalVerts,
                indexArray, totalVerts,
                masterVertexNodes[si].extras, extraCount);

            // Mark this batch's coverage immediately after rendering it
            float bMinX = 1e9f, bMinY = 1e9f, bMaxX = -1e9f, bMaxY = -1e9f;
            const gos_TERRAIN_EXTRA* ex = masterVertexNodes[si].extras;
            for (int e = 0; e < extraCount; e++) {
                if (ex[e].wx < bMinX) bMinX = ex[e].wx;
                if (ex[e].wx > bMaxX) bMaxX = ex[e].wx;
                if (ex[e].wy < bMinY) bMinY = ex[e].wy;
                if (ex[e].wy > bMaxY) bMaxY = ex[e].wy;
            }
            if (bMaxX > bMinX)
                gos_MarkShadowRegionCovered(bMinX, bMinY, bMaxX, bMaxY);
        }

        gos_EndShadowPrePass();
    }
} // end shadow coverage scope
```

**Notes:**
- `gos_TERRAIN_EXTRA` is already visible in `txmmgr.cpp` via `txmmgr.h`. Fields `wx`/`wy` match `mclib/quad.cpp:79-84`.
- Phase 1 early-exits on `anyUncovered = true` to avoid scanning all nodes when a single uncovered batch is found.
- Phase 2 re-walks the same nodes. This is a deliberate duplicate traversal — the two loops have different concerns (query vs. render+mark) and keeping them separate avoids complex conditional logic inside one loop.

- [ ] **Step 1.4:** Replace the camera-distance shadow check in `txmmgr.cpp` with the two-phase implementation above.

### Step 1.5 — Build and verify

```bash
# Kill any running mc2.exe first (locks the PDB)
# Then build:
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" \
  --config RelWithDebInfo 2>&1 | tail -20
```

Expected output: `mc2.exe` rebuild succeeds, no linker errors.

- [ ] **Step 1.5:** Build with `--config RelWithDebInfo`. Fix any compile errors before proceeding.

### Step 1.6 - Deploy and in-game verify

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

In-game verification:
1. Open Tracy profiler before launching the game.
2. Load a mission, pan the camera steadily in one direction.
3. On the first pan or rotate that reveals a new uncovered area, `Shadow.StaticAccum` should appear in the Tracy timeline.
4. Pan back over the same fully covered area. `Shadow.StaticAccum` should not appear.
5. Rotate in place near a ridge, map edge, or broad vista. If new terrain becomes visible while the camera origin stays in the same cell, the pass should still fire once.
6. Toggle shadows off and back on with `RAlt+F3`. The next qualifying view should rebuild because coverage and static-light state were invalidated on re-enable.

- [ ] **Step 1.6:** Deploy and verify: no re-renders on fully covered revisits, but new view-exposed terrain still triggers once.

### Step 1.7 — Commit

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add GameOS/gameos/gos_postprocess.h \
        GameOS/gameos/gos_postprocess.cpp \
        GameOS/gameos/gameos_graphics.cpp \
        mclib/txmmgr.cpp
git commit -m "$(cat <<'EOF'
perf: shadow coverage grid eliminates re-render stutter

Static shadow pre-pass now skips terrain already in the shadow FBO.
A 32x32 world-space boolean grid tracks per-batch covered cells; the
pre-pass only fires when at least one visible batch intersects an
uncovered cell. Once the full map is explored the static pass stops
running entirely, removing the 100-unit-move hitching.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 1.7:** Commit the coverage grid changes.

---

## Task 2: Phong smoothing in shadow TES — reduce view-dependent banding

**Files:**
- Modify: `shaders/shadow_terrain.tese`
- Modify: `GameOS/gameos/gameos_graphics.cpp` (one float change)

### Background recap

The main terrain TES (`gos_terrain.tese:72-80`) applies PN-triangle Phong smoothing:

```glsl
float alpha = tessDisplace.x;  // phongAlpha, typically 0.75
if (alpha > 0.0) {
    vec3 proj0 = worldPos - dot(worldPos - tcs_WorldPos[0], tcs_WorldNorm[0]) * tcs_WorldNorm[0];
    vec3 proj1 = worldPos - dot(worldPos - tcs_WorldPos[1], tcs_WorldNorm[1]) * tcs_WorldNorm[1];
    vec3 proj2 = worldPos - dot(worldPos - tcs_WorldPos[2], tcs_WorldNorm[2]) * tcs_WorldNorm[2];
    vec3 phongPos = bary.x * proj0 + bary.y * proj1 + bary.z * proj2;
    worldPos = mix(worldPos, phongPos, alpha);
}
```

The shadow TES receives the same vertex normals (`tcs_WorldNorm`) but does not run this block. By adding it here and passing the real `phongAlpha` from C++, the shadow depth map geometry matches the fragment positions and the depth bias requirement drops to near zero.

### Step 2.1 — Add Phong block to `shadow_terrain.tese`

In `shaders/shadow_terrain.tese`, after the world-normal interpolation and before the displacement block, insert the Phong block. The file currently looks like:

```glsl
    vec3 worldNorm = normalize(
        bary.x * tcs_WorldNorm[0]
      + bary.y * tcs_WorldNorm[1]
      + bary.z * tcs_WorldNorm[2]);

    vec2 texcoord = ...;

    // Texture-based displacement along normal (dirt only) — matches main TES
    float displaceScale = tessDisplace.y;
```

Insert between the `worldNorm` block and `texcoord`:

```glsl
    // Phong tessellation smoothing — matches gos_terrain.tese exactly.
    // tessDisplace.x = phongAlpha (0 = off, >0 = smoothing strength).
    float phongAlpha = tessDisplace.x;
    if (phongAlpha > 0.0) {
        vec3 proj0 = worldPos - dot(worldPos - tcs_WorldPos[0], tcs_WorldNorm[0]) * tcs_WorldNorm[0];
        vec3 proj1 = worldPos - dot(worldPos - tcs_WorldPos[1], tcs_WorldNorm[1]) * tcs_WorldNorm[1];
        vec3 proj2 = worldPos - dot(worldPos - tcs_WorldPos[2], tcs_WorldNorm[2]) * tcs_WorldNorm[2];
        vec3 phongPos = bary.x * proj0 + bary.y * proj1 + bary.z * proj2;
        worldPos = mix(worldPos, phongPos, phongAlpha);
    }
```

Full context for the edit — replace the block from `vec2 texcoord` down to `gl_Position`:

```glsl
    vec3 worldNorm = normalize(
        bary.x * tcs_WorldNorm[0]
      + bary.y * tcs_WorldNorm[1]
      + bary.z * tcs_WorldNorm[2]);

    vec2 texcoord = bary.x * tcs_Texcoord[0]
                  + bary.y * tcs_Texcoord[1]
                  + bary.z * tcs_Texcoord[2];

    // Phong tessellation smoothing — matches gos_terrain.tese exactly.
    float phongAlpha = tessDisplace.x;
    if (phongAlpha > 0.0) {
        vec3 proj0 = worldPos - dot(worldPos - tcs_WorldPos[0], tcs_WorldNorm[0]) * tcs_WorldNorm[0];
        vec3 proj1 = worldPos - dot(worldPos - tcs_WorldPos[1], tcs_WorldNorm[1]) * tcs_WorldNorm[1];
        vec3 proj2 = worldPos - dot(worldPos - tcs_WorldPos[2], tcs_WorldNorm[2]) * tcs_WorldNorm[2];
        vec3 phongPos = bary.x * proj0 + bary.y * proj1 + bary.z * proj2;
        worldPos = mix(worldPos, phongPos, phongAlpha);
    }

    // Texture-based displacement along normal (dirt only) — matches main TES
    float displaceScale = tessDisplace.y;
    if (displaceScale > 0.0) {
        vec3 colSample = texture(tex1, texcoord).rgb;
        vec4 matWeights = tc_getColorWeights(colSample);

        float dirtWeight = matWeights.z;
        if (dirtWeight > 0.01) {
            float baseTiling = detailNormalTiling.x;
            vec2 dispUV = texcoord * baseTiling * TC_MAT_TILING.z;
            float disp = 1.0 - texture(matNormal2, dispUV).a;
            worldPos += worldNorm * (disp - 0.5) * displaceScale * dirtWeight;
        }
    }

    // Simple orthographic projection into light space
    gl_Position = lightSpaceMatrix * vec4(worldPos, 1.0);
```

- [ ] **Step 2.1:** Add the Phong smoothing block to `shaders/shadow_terrain.tese` between the worldNorm and texcoord lines.

### Step 2.2 — Pass `terrain_phong_alpha_` to shadow pass in `gameos_graphics.cpp`

Find `drawShadowBatchTessellated` in `gameos_graphics.cpp` (~line 2373). The current tessDisplace upload is:

```cpp
float tessDisp[4] = { 0.0f, terrain_displace_scale_, 0.0f, 0.0f };  // no Phong in shadow
```

Change it to:

```cpp
float tessDisp[4] = { terrain_phong_alpha_, terrain_displace_scale_, 0.0f, 0.0f };
```

Also update the comment on that line (remove "no Phong in shadow").

- [ ] **Step 2.2:** In `gameos_graphics.cpp` `drawShadowBatchTessellated`, change `tessDisp[0]` from `0.0f` to `terrain_phong_alpha_`.

### Step 2.3 — Shader-only deploy and visual check

Since this is a shader-only change (no C++ rebuild needed), deploy just the shader:

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/shadow_terrain.tese" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/shadow_terrain.tese"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/shadow_terrain.tese" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/shadow_terrain.tese"
```

Wait — Step 2.2 changes a `.cpp` file, so a full rebuild is required. Build first:

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" \
  --config RelWithDebInfo 2>&1 | tail -20
```

Then deploy both:
```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/shadow_terrain.tese" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/shadow_terrain.tese"
```

In-game verification:
1. Load a mission with visible terrain slopes (hills, elevations).
2. Rotate the camera in place — shadow edges on slopes should stay stable instead of crawling.
3. Pan across the terrain — no new banding patterns should appear at tessellation patch boundaries.
4. Shadow acne (dark speckles on lit faces) should not increase. If it does, the bias needs tuning — see the fallback in Task 3.

**Known risk:** Adding Phong smoothing to the shadow pass slightly changes all shadow depths. The existing slope bias (`max(0.005 * (1 - NdotL), 0.002)`) was tuned for non-Phong geometry. After this change the geometry matches better, so acne should decrease. If you observe MORE acne (the Phong offset overshoots), reduce `phongAlpha` passed to the shadow pass — try `terrain_phong_alpha_ * 0.5f` as a diagnostic step. Do not change `shadow.hglsl` bias unless the acne is confirmed to be bias-related.

- [ ] **Step 2.3:** Build and deploy. Visually verify: shadow edges stable on rotation, no new acne.

Diagnostic guardrail: if you temporarily try `terrain_phong_alpha_ * 0.5f`, treat it as a debugging aid only. The final state should keep the shadow TES and main terrain TES aligned unless bias tuning proves otherwise.

### Step 2.4 — Commit

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add shaders/shadow_terrain.tese \
        GameOS/gameos/gameos_graphics.cpp
git commit -m "$(cat <<'EOF'
fix: Phong smoothing in shadow TES to reduce view-dependent banding

Shadow depth pass now applies the same PN-triangle Phong smoothing as
the main terrain TES. Fragment worldPos (Phong-smoothed) now matches
the positions written into the shadow FBO, closing the depth-offset gap
that caused shadow edges to crawl on camera rotation.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 2.4:** Commit the Phong shadow fix.

---

## Task 3 (fallback): Bias tuning if banding persists after Task 2

**Only do this task if Task 2 leaves visible banding after in-game review.**

**File:** `shaders/include/shadow.hglsl`

### Step 3.1 — Diagnose whether bias or geometry is the remaining cause

If after Task 2 there is still crawling on rotation:
1. Temporarily set `terrain_phong_alpha_` passed to shadow to `0.0f` (disable Phong in shadow again).
2. If banding is worse → Phong fix is helping, bias just needs upward tuning.
3. If banding is the same → Phong offset is not the root cause; check whether the shadow pre-pass is running at all (verify Task 1 didn't suppress needed re-renders).

### Step 3.2 — Increase slope bias range in `shadow.hglsl`

In `shaders/include/shadow.hglsl` line 47, the current bias is:

```glsl
    float bias = max(0.005 * (1.0 - NdotL), 0.002);
```

If banding persists on steep slopes, increase the slope coefficient. Try `0.008` first:

```glsl
    float bias = max(0.008 * (1.0 - NdotL), 0.002);
```

**Do not increase the constant term (`0.002`)** — that causes light-leak (shadow gaps on flat terrain). Only touch the slope coefficient.

This is a shader-only change; deploy without rebuilding the exe:

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/include/shadow.hglsl" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/include/shadow.hglsl"
```

- [ ] **Step 3.1 (conditional):** Diagnose whether Phong fix closed the gap or bias tuning is still needed.
- [ ] **Step 3.2 (conditional):** Increase slope bias coefficient in `shadow.hglsl` to `0.008` if banding remains.

### Step 3.3 — Commit if done

```bash
git add shaders/include/shadow.hglsl
git commit -m "$(cat <<'EOF'
fix: increase shadow slope bias to reduce banding on steep terrain

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3.3 (conditional):** Commit bias change.

---

## Self-review

**Spec coverage:**
- "re-render stutter when the camera moves" → Task 1 (coverage grid, skip already-covered cells). ✓
- "banding/shifting tied to camera-dependent terrain geometry" → Task 2 (Phong match) + Task 3 fallback. ✓
- "Truly stable world-fixed terrain shadow map behavior" → covered: once all cells are covered the static pass never fires. ✓
- "Less view-dependent shadow edge movement" → covered by Phong match. ✓
- "Fewer expensive re-renders" → covered by Task 1 (zero re-renders on revisit). ✓

**2026-04-18 review addendum:**
- Task 1 is now defined by submitted-terrain coverage, not camera-cell coverage.
- Per-batch coverage marking supersedes the earlier frame-wide bounding-box shortcut.
- Shadow toggle reset is in scope and must be implemented, not left as a known limitation.

**Placeholder check:** No TBDs. All code blocks contain compilable snippets. Types, method names, and field names (`wx`, `wy`, `kCoverageGridSize`, `shadowCoverageGrid_`, `terrain_phong_alpha_`) are consistent across tasks.

**Risk flags:**
- The coverage grid still uses bounds rather than exact triangle coverage. Per-batch bounds are intentionally chosen as the pragmatic middle ground because they are safer than a single frame-wide bbox and avoid the false assumption that the camera cell predicts the visible terrain footprint.
- If `mapHalfExtent_` is 0 when the shadow pass first fires, `isShadowRegionCovered` returns `false` (safe: pass runs). `mapHalfExtent_` is set by `gos_SetMapHalfExtent` at map load — verify it's called before the first shadow render (it currently is via `gos_BuildStaticLightMatrix`).
- The RAlt+F3 shadow toggle currently only flips `shadowsEnabled_` in `gos_postprocess.h`. This plan now treats reset-on-reenable as required behaviour, so the implementation must add an explicit coverage and static-light invalidation path instead of leaving this as a known limitation.

