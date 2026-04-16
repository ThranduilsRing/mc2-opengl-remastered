# Terrain World-Space Vertex Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate `eye->projectZ()` from the terrain vertex loop by migrating `gos_VERTEX.x/y/z/rhw` from CPU screen-space to MC2 world coords, staged so each step is independently testable.

**Architecture:** Four steps: (1) port `UndisplacedDepth` in the TES to use world-space + `terrainMVP`; (2) simplify the terrain VS; (3) switch quad.cpp to submit world coords; (4) remove the `projectZ()` call from terrain.cpp. Steps 1–3 keep `projectZ()` running so they are independently revertible. The debug toggle (RAlt+9) lets you visually A/B test Step 1 without rebuilding.

**Tech Stack:** GLSL 4.2 tessellation shaders, C++17, OpenGL 4.x, SDL2 key events, Tracy profiler.

---

## Files Changed

| File | Role |
|------|------|
| `shaders/gos_terrain.tese` | Step 1: new `UndisplacedDepth` + debug toggle uniform; Step 4 cleanup |
| `shaders/gos_terrain.vert` | Step 2: remove dead `mvp * pos` computation |
| `mclib/quad.cpp` | Step 3: world coords in `gVertex.x/y/z/rhw`, remove four z-range guards |
| `GameOS/gameos/gameos_graphics.cpp` | Step 1: `undisplacedDepthMode_` member + RAlt+9 toggle + uniform cache + upload |
| `GameOS/gameos/gameosmain.cpp` | Step 1: RAlt+9 key handler |
| `mclib/terrain.cpp` | Step 4: remove `projectZ()` call from vertex loop, remove `leastZ/mostZ` tracking |
| `mclib/camera.cpp` | Step 4: replace `pz`-based tile selection in `inverseProject()` with world-dist comparison |

---

## Build & Deploy Reference

**Build** (always RelWithDebInfo):
```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo
```

**Deploy shaders only** (when only .tese/.vert changed — no exe rebuild needed):
```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.tese" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.tese"
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.vert" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.vert"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.tese" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.tese"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.vert" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.vert"
```

**Deploy exe** (close mc2.exe first or linker will fail with LNK1201):
```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

**Visual validation checklist** (run in-game after each task):
1. Terrain renders (no black screen, no missing tiles)
2. Road/cement overlays sit flush against terrain — no z-fighting at edges
3. Mission markers sit on terrain surface
4. Pan camera across map — no terrain pop-in or missing patches

---

## Task 1: TES dual-path debug toggle (Step 1)

Adds the new world-space `UndisplacedDepth` computation alongside the old one, gated by `undisplacedDepthMode` uniform. Wires RAlt+9 to flip between them in-game.

**Files:**
- Modify: `shaders/gos_terrain.tese`
- Modify: `GameOS/gameos/gameos_graphics.cpp`
- Modify: `GameOS/gameos/gameosmain.cpp`

- [ ] **Step 1.1: Add `undisplacedDepthMode_` to the renderer**

In `GameOS/gameos/gameos_graphics.cpp`, inside the `gosRenderer` class (around the other `terrain_*` member variables at line ~1585), add:

```cpp
int terrain_undisplaced_depth_mode_ = 0;  // 0=world-space (new), 1=screen-space (old)
```

In `TerrainUniformLocs` struct (line ~1591), add:
```cpp
GLint undisplacedDepthMode = -1;
```

In `cacheTerrainUniformLocations()` (line ~1611), add after the existing `glGetUniformLocation` calls:
```cpp
terrainLocs_.undisplacedDepthMode = glGetUniformLocation(shp, "undisplacedDepthMode");
```

In the terrain uniform upload block (line ~2646, after the `tessDebug` upload), add:
```cpp
if (tl.undisplacedDepthMode >= 0)
    glUniform1i(tl.undisplacedDepthMode, terrain_undisplaced_depth_mode_);
```

- [ ] **Step 1.2: Wire RAlt+9 to toggle the mode**

In `GameOS/gameos/gameosmain.cpp`, find the existing `case SDLK_9:` block (line ~129). It currently toggles SSAO. Replace it with:

```cpp
case SDLK_8:
    if (alt_debug) {
        gosRenderer* r = getGosRenderer();
        if (r) {
            r->terrain_undisplaced_depth_mode_ ^= 1;
            fprintf(stderr, "UndisplacedDepth mode: %s\n",
                r->terrain_undisplaced_depth_mode_ == 0 ? "WORLD-SPACE (new)" : "SCREEN-SPACE (old)");
        }
    }
    break;
```

`terrain_undisplaced_depth_mode_` is a private member — add `friend class gameosmain;` or expose via an accessor. Simplest: make it `public` temporarily since it's a debug variable.

- [ ] **Step 1.3: Add the dual-path block to the TES**

In `shaders/gos_terrain.tese`, replace lines 67–75 (the `UndisplacedDepth` block):

```glsl
// BEFORE:
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
// AFTER: dual-path for validation — remove old branch after Task 4
uniform int undisplacedDepthMode;  // 0=world-space (new), 1=screen-space (old)
{
    if (undisplacedDepthMode == 1) {
        // OLD: screen-space from CPU projectZ (via gl_in[].gl_Position)
        vec4 interpScreenPos = bary.x * gl_in[0].gl_Position
                             + bary.y * gl_in[1].gl_Position
                             + bary.z * gl_in[2].gl_Position;
        vec4 ndcBasic = mvp * interpScreenPos;
        UndisplacedDepth = (ndcBasic.z / ndcBasic.w) * 0.5 + 0.5;
    } else {
        // NEW: world-space through terrainMVP — same chain as displaced position
        vec3 undisplacedWorldPos = bary.x * tcs_WorldPos[0]
                                 + bary.y * tcs_WorldPos[1]
                                 + bary.z * tcs_WorldPos[2];
        vec4 uclip = terrainMVP * vec4(undisplacedWorldPos, 1.0);
        float urhw = 1.0 / uclip.w;
        float uscreenZ = uclip.z * urhw;
        // mvp maps screen coords → NDC; z output is independent of x/y
        vec4 undc = mvp * vec4(0.0, 0.0, uscreenZ, 1.0);
        UndisplacedDepth = undc.z * 0.5 + 0.5;
    }
}
```

Place the `uniform int undisplacedDepthMode;` declaration at the top of the TES uniform block (alongside the other uniforms).

- [ ] **Step 1.4: Build (shader + exe) and deploy**

Close `mc2.exe` if running. Build:
```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo
```
Expected: build succeeds, no errors.

Deploy exe + terrain shaders:
```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.tese" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.tese"
```

- [ ] **Step 1.5: Validate in-game**

Launch `mc2.exe`. Load a mission with roads/cement (e.g., any urban map).

Run visual validation checklist. Then:
- Press RAlt+9 repeatedly — console should print `UndisplacedDepth mode: WORLD-SPACE (new)` / `SCREEN-SPACE (old)`.
- In both modes, roads and cement overlays must sit flush on terrain with no z-fighting.
- No visible difference between modes = Step 1 is correct.

If z-fighting appears in world-space mode only: the `terrainMVP` projection chain in the new block has a precision or sign error. Compare depth output by adding a temporary `FragColor = vec4(UndisplacedDepth, 0, 0, 1)` to `gos_terrain.frag` and checking both modes produce identical red gradients.

- [ ] **Step 1.6: Commit**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add shaders/gos_terrain.tese GameOS/gameos/gameos_graphics.cpp GameOS/gameos/gameosmain.cpp
git commit -m "feat: dual-path UndisplacedDepth with RAlt+9 toggle (Step 1)"
```

---

## Task 2: Terrain VS — remove dead `mvp * pos` computation (Step 2)

After Task 1, `gl_in[].gl_Position` (the output of the VS) is unused by the TES unless `undisplacedDepthMode=1`. This step removes the computation but keeps `pos.xyz` carrying screen-space values (the CPU still fills them). Shader-only change, no exe rebuild.

**Files:**
- Modify: `shaders/gos_terrain.vert`

- [ ] **Step 2.1: Simplify the VS `gl_Position` assignment**

In `shaders/gos_terrain.vert`, replace lines 31–33:

```glsl
// BEFORE:
// Screen-space position (fallback, TES overrides gl_Position)
vec4 p = mvp * vec4(pos.xyz, 1);
gl_Position = p / pos.w;
```

With:

```glsl
// AFTER: gl_Position is overridden by TES — pass pos.xyz as-is.
// CPU still fills pos.xyz with screen-space values at this step;
// Task 3 will change them to world coords.
gl_Position = vec4(pos.xyz, 1.0);
```

The `uniform mat4 mvp;` declaration stays — it is still used in the TES for the NDC pass (the `mvp * vec4(0, 0, uscreenZ, 1)` computation in both `UndisplacedDepth` paths).

- [ ] **Step 2.2: Deploy shader and validate**

Deploy shader only (no exe rebuild needed):
```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.vert" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.vert"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.vert" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.vert"
```

Note: shaders hot-reload silently on failure (old shader stays active). Check `stderr` for compile errors before validating visually.

Launch `mc2.exe`. Run visual validation checklist. Terrain must look identical to Task 1 result.

- [ ] **Step 2.3: Commit**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add shaders/gos_terrain.vert
git commit -m "feat: terrain VS — pass pos.xyz directly, remove dead mvp*pos (Step 2)"
```

---

## Task 3: quad.cpp — submit world coords, remove z-range guards (Step 3)

The CPU switches from writing `px/py/pz/pw` (screen-space) to `vx/vy/elevation` (MC2 world) in `gVertex.x/y/z/rhw`. Four z-range guards that check `gVertex[i].z ∈ [0,1)` must be removed — with world elevation in `pos.z` those checks would always fail and terrain would disappear entirely.

`projectZ()` still runs at this step (its results go to `px/py/pz/pw` which are now unused by rendering). The call is kept deliberately so Task 4 can remove it cleanly.

**Files:**
- Modify: `mclib/quad.cpp`

- [ ] **Step 3.1: Change solid terrain triangle 1 (`gVertex[0/1/2]`, line ~1506)**

In `quad.cpp`, find the first terrain `gos_VERTEX` assembly block (top triangle of the solid terrain quad, lines ~1506–1534). Change the x/y/z/rhw assignments:

```cpp
// BEFORE:
gVertex[0].x   = vertices[0]->px;
gVertex[0].y   = vertices[0]->py;
gVertex[0].z   = vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
gVertex[0].rhw = vertices[0]->pw;
// ... same pattern for [1] and [2]

// AFTER:
gVertex[0].x   = vertices[0]->vx;
gVertex[0].y   = vertices[0]->vy;
gVertex[0].z   = vertices[0]->pVertex->elevation;
gVertex[0].rhw = 1.0f;
// ... same pattern for [1] and [2]
```

`TERRAIN_DEPTH_FUDGE` (0.001f) was a screen-depth bias to prevent z-fighting between terrain layers. With the TES computing depth from world coords via `terrainMVP`, `pos.z` no longer affects rendered depth — drop it.

- [ ] **Step 3.2: Remove z-range guard 1 (line ~1536)**

Immediately after the `gVertex[0/1/2]` assembly, remove the guard:

```cpp
// REMOVE this entire if-block wrapper (keep its body):
if ((gVertex[0].z >= 0.0f) &&
    (gVertex[0].z < 1.0f) &&
    (gVertex[1].z >= 0.0f) &&
    (gVertex[1].z < 1.0f) &&
    (gVertex[2].z >= 0.0f) &&
    (gVertex[2].z < 1.0f))
{
    // keep everything inside
}
```

The guard existed to skip vertices outside the screen-space depth range. With world coords in `pos.z`, the check is meaningless. Frustum culling via `clipInfo` (still running) handles visibility.

- [ ] **Step 3.3: Change solid terrain triangle 2 (`gVertex[2]` from `vertices[3]`, line ~1670)**

Find the second solid terrain triangle (bottom triangle of the quad, shares `gVertex[0/1]` from the first triangle, replaces `gVertex[2]` with `vertices[3]`). Apply the same change:

```cpp
// BEFORE:
gVertex[2].x   = vertices[3]->px;
gVertex[2].y   = vertices[3]->py;
gVertex[2].z   = vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
gVertex[2].rhw = vertices[3]->pw;

// AFTER:
gVertex[2].x   = vertices[3]->vx;
gVertex[2].y   = vertices[3]->vy;
gVertex[2].z   = vertices[3]->pVertex->elevation;
gVertex[2].rhw = 1.0f;
```

Remove the z-range guard at line ~1680 (same pattern as Step 3.2).

- [ ] **Step 3.4: Change remaining terrain triangle sites (lines ~1807, ~1977)**

There are two more `gVertex` assembly sites for alternate quad orientation (`uvMode != BOTTOMRIGHT`). Apply the same `vx/vy/elevation/1.0f` replacement and remove their z-range guards (lines ~1837 and ~1977).

Search for all remaining `->px` and `->pz + TERRAIN_DEPTH_FUDGE` references in the terrain solid/alpha paths to confirm no sites are missed:
```bash
grep -n "->px\|->pz + TERRAIN_DEPTH_FUDGE" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp" | grep -v "water\|Water"
```
Expected: zero results after this step for terrain paths (water uses `->wx/wz` not `->px/pz`).

- [ ] **Step 3.5: Add debug env-var logging**

Near the first `gVertex` assembly (after the new world-coord assignments), add:

```cpp
static const bool vtxDebug = getenv("MC2_DEBUG_TERRAIN_VTXSRC") != nullptr;
if (vtxDebug) {
    static int dbgFrame = 0;
    if (dbgFrame < 3) {
        ++dbgFrame;
        printf("[TERRAIN_VTX] world=(%.1f,%.1f,%.1f) old_screen=(%.1f,%.1f,%.4f)\n",
               vertices[0]->vx, vertices[0]->vy, vertices[0]->pVertex->elevation,
               vertices[0]->px, vertices[0]->py, vertices[0]->pz);
    }
}
```

This lets you verify world coords are being submitted without rebuilding.

- [ ] **Step 3.6: Build exe, deploy, and validate**

Close `mc2.exe`. Build and deploy exe + shaders.

Launch with env var to check first frame:
```bash
cd "A:/Games/mc2-opengl/mc2-win64-v0.1.1"
MC2_DEBUG_TERRAIN_VTXSRC=1 ./mc2.exe 2>&1 | grep TERRAIN_VTX
```
Expected output (approximate — world coords should be large integers/floats, not 0–1 screen values):
```
[TERRAIN_VTX] world=(1280.0,960.0,45.3) old_screen=(640.5,480.2,0.4971)
```

Run full visual validation checklist. The terrain must look **identical** to Tasks 1–2. Press RAlt+8 to confirm both `UndisplacedDepth` modes still work.

If terrain disappears: a z-range guard was missed. Run the grep from Step 3.4.
If terrain is displaced/wrong: check that `vx/vy` are MC2 east/north coords (same as `extras.wx/wy`).

- [ ] **Step 3.7: Commit**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add mclib/quad.cpp
git commit -m "feat: terrain gVertex — world coords in pos.xyz, remove z-range guards (Step 3)"
```

---

## Task 4: Remove debug toggle, restore RAlt+9 to SSAO

Once Task 3 validates cleanly, the old screen-space `UndisplacedDepth` path and the RAlt+9 debug toggle can be removed. This is the cleanup before the final step.

**Files:**
- Modify: `shaders/gos_terrain.tese`
- Modify: `GameOS/gameos/gameos_graphics.cpp`
- Modify: `GameOS/gameos/gameosmain.cpp`

- [ ] **Step 4.1: Remove the dual-path from the TES**

In `shaders/gos_terrain.tese`, replace the dual-path block with the new path only:

```glsl
// Remove: uniform int undisplacedDepthMode;
// Remove: the if/else structure
// Keep only the new path:
{
    vec3 undisplacedWorldPos = bary.x * tcs_WorldPos[0]
                             + bary.y * tcs_WorldPos[1]
                             + bary.z * tcs_WorldPos[2];
    vec4 uclip = terrainMVP * vec4(undisplacedWorldPos, 1.0);
    float urhw = 1.0 / uclip.w;
    float uscreenZ = uclip.z * urhw;
    vec4 undc = mvp * vec4(0.0, 0.0, uscreenZ, 1.0);
    UndisplacedDepth = undc.z * 0.5 + 0.5;
}
```

- [ ] **Step 4.2: Remove C++ debug scaffolding**

In `gameos_graphics.cpp`: remove `terrain_undisplaced_depth_mode_` member and `terrainLocs_.undisplacedDepthMode` cache and its upload line.

In `gameosmain.cpp`, restore `case SDLK_9:` to SSAO:
```cpp
case SDLK_9:
    if (alt_debug) {
        gosPostProcess* pp = getGosPostProcess();
        if (pp) {
            pp->ssaoEnabled_ = !pp->ssaoEnabled_;
            fprintf(stderr, "SSAO: %s\n", pp->ssaoEnabled_ ? "ON" : "OFF");
        }
    }
    break;
```

- [ ] **Step 4.3: Build, deploy, validate**

Build exe and deploy shaders + exe. Run visual validation checklist. Confirm terrain looks identical.

- [ ] **Step 4.4: Commit**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add shaders/gos_terrain.tese GameOS/gameos/gameos_graphics.cpp GameOS/gameos/gameosmain.cpp
git commit -m "cleanup: remove UndisplacedDepth debug toggle, restore RAlt+9 to SSAO (Step 4)"
```

---

## Task 5: terrain.cpp — remove `projectZ()` and fix mouse picking (Step 4 of design)

This is the final step. Removes `projectZ()` from the terrain vertex loop. The `inView`/`leastZ/mostZ` values fed `Camera::inverseProject()` (mouse picking) via `v->pz` and `setInverseProject()`. Both must be replaced before removing the call.

**Files:**
- Modify: `mclib/terrain.cpp`
- Modify: `mclib/camera.cpp`

- [ ] **Step 5.1: Replace `pz`-based tile selection in `inverseProject()`**

In `mclib/camera.cpp`, `Camera::inverseProject()` (line ~785) selects the closest tile by finding the one with the minimum `v->pz` (screen-space z). Replace with world-space distance-to-camera:

```cpp
// BEFORE (line ~787 area): finds tile with minimum pz
float leastZ = 1.0f;
for (long i = 0; i < currentClosest; i++) {
    if ((closestTiles[i]->vertices[0]->pz > 0.0f) && ...) {
        if (closestTiles[i]->vertices[0]->pz < leastZ) {
            leastZ = closestTiles[i]->vertices[0]->pz;
            closestTile = closestTiles[i];
        }
        // ... similar for vertices[1-3]
    }
}

// AFTER: find tile whose first vertex is closest to camera in world space
//
// Coordinate spaces:
//   getCameraOrigin() returns Stuff coords: .x=left(=-east), .y=elev, .z=north
//   MC2 world: vx=east, vy=north
//   So: MC2 east = -getCameraOrigin().x
//       MC2 north =  getCameraOrigin().z
Stuff::Vector3D cam = getCameraOrigin();
float camEast  = -cam.x;
float camNorth =  cam.z;
float leastDist = 1e30f;
for (long i = 0; i < currentClosest; i++) {
    VertexPtr v = closestTiles[i]->vertices[0];
    float dx = v->vx - camEast;
    float dy = v->vy - camNorth;
    float dist2 = dx*dx + dy*dy;
    if (dist2 < leastDist) {
        leastDist = dist2;
        closestTile = closestTiles[i];
    }
}

- [ ] **Step 5.2: Remove `setInverseProject()` call**

In `terrain.cpp` (line ~1141):
```cpp
// REMOVE:
eye->setInverseProject(mostZ, leastW, yzRange, ywRange);
```

Also remove the `ywRange/yzRange` computation (lines ~1134–1138) and the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` declarations at the top of the function (line ~950).

`Camera::inverseProjectZ()` uses `startZInverse/zPerPixel` set by `setInverseProject()`. Check if `inverseProjectZ()` is called anywhere; if so, it will now use stale/default values. Search:
```bash
grep -rn "inverseProjectZ" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/" --include="*.cpp"
```
If used, replace those call sites with a world-space elevation lookup via `Terrain::getTerrainElevation()`.

- [ ] **Step 5.3: Remove `projectZ()` from the vertex loop**

In `terrain.cpp`, the vertex loop (line ~1055–1078). Remove the `projectZ()` call and its outputs:

```cpp
// REMOVE this entire block:
bool inView = false;
Stuff::Vector4D screenPos(-10000.0f, -10000.0f, -10000.0f, -10000.0f);
if (onScreen) {
    Stuff::Vector3D vertex3D(currentVertex->vx, currentVertex->vy, currentVertex->pVertex->elevation);
    inView = eye->projectZ(vertex3D, screenPos);
    currentVertex->px = screenPos.x;
    currentVertex->py = screenPos.y;
    currentVertex->pz = screenPos.z;
    currentVertex->pw = screenPos.w;
}
else {
    currentVertex->px = currentVertex->py = 10000.0f;
    currentVertex->pz = -0.5f;
    currentVertex->pw = 0.5f;
}
```

`inView` was only used for the `leastZ/mostZ` tracking (now removed). The `onScreen` flag (from the frustum test) still correctly gates `clipInfo`, `setObjBlockActive`, `setObjVertexActive`.

- [ ] **Step 5.4: Build, deploy, validate with Tracy**

Close `mc2.exe`. Build and deploy.

Launch game, connect Tracy profiler. Check `Camera.UpdateRenderers` zone — the `projectZ()` call cost across 500K+ vertices should be gone. Expect visible reduction in self-time.

Run full visual validation checklist. Confirm mouse clicking on terrain still selects correct positions (the `inverseProject` replacement in Step 5.1 must pick the right tile under the cursor).

- [ ] **Step 5.5: Remove the `MC2_DEBUG_TERRAIN_VTXSRC` logging**

In `quad.cpp`, remove the debug block added in Task 3 Step 3.5:
```cpp
// REMOVE:
static const bool vtxDebug = getenv("MC2_DEBUG_TERRAIN_VTXSRC") != nullptr;
if (vtxDebug) { ... }
```

- [ ] **Step 5.6: Commit**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add mclib/terrain.cpp mclib/camera.cpp mclib/quad.cpp
git commit -m "feat: remove projectZ() from terrain vertex loop, world-space mouse picking (Step 5)"
```

---

## Final Acceptance Check

After Task 5 passes, confirm all acceptance criteria from the design doc:

- [ ] Roads/cement overlays have no z-fighting vs. terrain
- [ ] Mission markers sit on terrain surface
- [ ] Water surface renders correctly (unaffected — uses separate `wx/wz` path)
- [ ] Mouse terrain clicking selects the correct world position
- [ ] Tracy shows `Camera.UpdateRenderers` total reduced (compare before/after profiler captures)
- [ ] No `px/py/pz/pw` writes remain in terrain vertex loop: `grep -n "->px\|->py\|->pz\|->pw" mclib/terrain.cpp` → zero results in the vertex loop
