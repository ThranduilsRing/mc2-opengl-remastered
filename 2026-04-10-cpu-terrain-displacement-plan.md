# CPU Terrain Displacement Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix floating units by making `MapData::terrainElevation()` return GPU-displaced terrain heights (Phong smoothing + dirt texture displacement).

**Architecture:** Retain CPU copies of colormap RGBA and matNormal2 alpha at load time. Expose displacement parameters via GameOS API. Enhance `terrainElevation()` to apply identical displacement math to the GPU TES, using retained texture data and vertex normals already available in `PostcompVertex`.

**Tech Stack:** C++, OpenGL (GameOS layer), MC2 terrain pipeline

---

### Task 1: Retain matNormal2 alpha on CPU (terrtxm2.cpp)

**Files:**
- Modify: `mclib/terrtxm2.h:48-85` (TerrainColorMap class — add fields)
- Modify: `mclib/terrtxm2.cpp:2137-2151` (material load — retain alpha before freeing)

**Step 1: Add CPU displacement data fields to TerrainColorMap**

In `mclib/terrtxm2.h`, add after `hasNormalMap` (line 83):

```cpp
        // CPU-side displacement data for terrain elevation correction
        unsigned char*  cpuDispAlpha;       // matNormal2 alpha channel (dirt displacement)
        int             cpuDispAlphaSize;   // width=height of the square texture
```

**Step 2: Initialize new fields in `init()` and clean up in `destroy()`**

In `mclib/terrtxm2.cpp`, find `TerrainColorMap::init(void)` and add:
```cpp
cpuDispAlpha = NULL;
cpuDispAlphaSize = 0;
```

In `TerrainColorMap::destroy()`, add:
```cpp
if (cpuDispAlpha) { free(cpuDispAlpha); cpuDispAlpha = NULL; }
cpuDispAlphaSize = 0;
```

**Step 3: Extract and retain matNormal2 alpha before freeing**

In `mclib/terrtxm2.cpp`, at line ~2137 (inside the `if (allLoaded)` block), BEFORE the free loop at line 2148, add code to extract matNormal2 (index 2) alpha channel:

```cpp
// Retain CPU copy of matNormal2 alpha for terrain elevation displacement
if (normalLayers[2]) {
    long pixels = (long)arrayWidth * (long)arrayWidth;
    cpuDispAlpha = (unsigned char*)malloc(pixels);
    cpuDispAlphaSize = arrayWidth;
    const unsigned char* src = normalLayers[2];
    for (long i = 0; i < pixels; i++) {
        cpuDispAlpha[i] = src[i * 4 + 3]; // alpha channel (RGBA)
    }
    printf("[SPLATTING] retained matNormal2 alpha on CPU (%dx%d)\n", arrayWidth, arrayWidth);
}
```

Insert this after line 2143 (`gos_SetTerrainMaterialNormal(i, nmId)` loop) and before line 2148 (the free loop).

**Step 4: Build and verify no crashes**

Run: `/mc2-build`
Expected: Clean compile, no errors.

**Step 5: Commit**

```
feat: retain matNormal2 alpha channel on CPU for displacement queries
```

---

### Task 2: Retain colormap RGBA on CPU (terrtxm2.cpp)

**Files:**
- Modify: `mclib/terrtxm2.h:48-85` (add colormap fields)
- Modify: `mclib/terrtxm2.cpp:1376-1414` (retain colormap before freeing)

**Step 1: Add CPU colormap fields to TerrainColorMap**

In `mclib/terrtxm2.h`, add after `cpuDispAlphaSize`:

```cpp
        unsigned char*  cpuColorMap;        // full colormap RGBA for HSV classification
        int             cpuColorMapSize;    // width=height of the full colormap
```

**Step 2: Initialize and destroy**

In `init()`:
```cpp
cpuColorMap = NULL;
cpuColorMapSize = 0;
```

In `destroy()`:
```cpp
if (cpuColorMap) { free(cpuColorMap); cpuColorMap = NULL; }
cpuColorMapSize = 0;
```

**Step 3: Retain colormap before freeing**

In `mclib/terrtxm2.cpp`, in `resetBaseTexture()`, BEFORE the free loop at line 1399, add:

```cpp
// Retain CPU copy of colormap for terrain displacement HSV classification
{
    long pixels = (long)colorMapInfo.width * (long)colorMapInfo.width;
    cpuColorMap = (unsigned char*)malloc(pixels * 4);
    cpuColorMapSize = colorMapInfo.width;
    memcpy(cpuColorMap, ColorMap, pixels * 4);
    printf("[COLORMAP] retained colormap on CPU (%dx%d)\n", colorMapInfo.width, colorMapInfo.width);
}
```

Insert this after line 1394 (end of the texture upload loop) and before line 1397 (the "freeable" comment).

**Step 4: Build and verify**

Run: `/mc2-build`
Expected: Clean compile.

**Step 5: Commit**

```
feat: retain colormap RGBA on CPU for displacement HSV classification
```

---

### Task 3: Expose displacement parameters via GameOS API

**Files:**
- Modify: `GameOS/include/gameos.hpp:2291-2297` (add getter declarations)
- Modify: `GameOS/gameos/gameos_graphics.cpp:3476-3481` (add getter implementations)

**Step 1: Declare getters in gameos.hpp**

After line 2297 (`gos_SetTerrainViewDir`), add:

```cpp
// CPU displacement query API
float gos_GetTerrainPhongAlpha();
float gos_GetTerrainDisplaceScale();
float gos_GetTerrainDetailTiling();
```

**Step 2: Implement getters in gameos_graphics.cpp**

After the existing `gos_SetTerrainDisplaceScale` implementation (~line 3481), add:

```cpp
float gos_GetTerrainPhongAlpha() {
    return g_gos_renderer ? g_gos_renderer->getTerrainPhongAlpha() : 0.0f;
}
float gos_GetTerrainDisplaceScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDisplaceScale() : 0.0f;
}
float gos_GetTerrainDetailTiling() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDetailTiling() : 1.0f;
}
```

Note: `getTerrainDetailTiling()` doesn't exist yet. Add to gosRenderer class (~line 1298):

```cpp
float getTerrainDetailTiling() const { return terrain_detail_tiling_; }
```

**Step 3: Build and verify**

Run: `/mc2-build`
Expected: Clean compile.

**Step 4: Commit**

```
feat: expose terrain displacement parameters via GameOS getters
```

---

### Task 4: Implement CPU displacement in terrainElevation()

This is the core task. Modify `MapData::terrainElevation()` to apply Phong smoothing and dirt displacement after computing base elevation.

**Files:**
- Modify: `mclib/mapdata.cpp:1584-1806` (enhance terrainElevation)
- Modify: `mclib/mapdata.h:135` (add include if needed)

**Step 1: Add helper functions above terrainElevation()**

Add these static helpers before `terrainElevation()` (~line 1583):

```cpp
//--- CPU-side terrain displacement helpers (match GPU TES) ---

// HSV conversion matching terrain_common.hglsl tc_rgb2hsv
static void cpu_rgb2hsv(float r, float g, float b, float& h, float& s, float& v) {
    float maxC = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    float minC = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    float d = maxC - minC;
    v = maxC;
    s = (maxC > 1e-10f) ? (d / maxC) : 0.0f;
    if (d < 1e-10f) { h = 0.0f; return; }
    if (maxC == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (maxC == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

// smoothstep matching GLSL
static float cpu_smoothstep(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Material weight classification matching terrain_common.hglsl tc_getColorWeights
// Returns dirt weight (w.z) only, since that's all we need for displacement
static float cpu_getDirtWeight(float r, float g, float b) {
    float h, s, v;
    cpu_rgb2hsv(r, g, b, h, s, v);

    float grassW = cpu_smoothstep(0.14f, 0.17f, h) * cpu_smoothstep(0.15f, 0.28f, s);
    float dirtW = cpu_smoothstep(0.155f, 0.13f, h) * cpu_smoothstep(0.15f, 0.28f, s);
    float concreteW = cpu_smoothstep(0.18f, 0.10f, s) * cpu_smoothstep(0.50f, 0.62f, v);
    float rockW = cpu_smoothstep(0.18f, 0.10f, s) * cpu_smoothstep(0.45f, 0.30f, v);
    rockW += cpu_smoothstep(0.38f, 0.28f, v) * cpu_smoothstep(0.15f, 0.25f, s);

    float isWater = cpu_smoothstep(0.35f, 0.45f, h);
    rockW += isWater;
    grassW *= (1.0f - isWater);
    dirtW *= (1.0f - isWater);

    float total = rockW + grassW + dirtW + concreteW;
    if (total < 0.01f) return 1.0f; // default to dirt if unclassified
    return dirtW / total;
}

// Bilinear sample from unsigned char texture (wrapping)
static float cpu_sampleAlpha(const unsigned char* data, int size, float u, float v) {
    // Wrap UV to [0,1)
    u = u - floorf(u);
    v = v - floorf(v);
    float fx = u * (float)size - 0.5f;
    float fy = v * (float)size - 0.5f;
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float dx = fx - (float)x0;
    float dy = fy - (float)y0;
    // Wrap pixel coords
    x0 = ((x0 % size) + size) % size;
    y0 = ((y0 % size) + size) % size;
    int x1 = (x0 + 1) % size;
    int y1 = (y0 + 1) % size;
    float s00 = data[y0 * size + x0] / 255.0f;
    float s10 = data[y0 * size + x1] / 255.0f;
    float s01 = data[y1 * size + x0] / 255.0f;
    float s11 = data[y1 * size + x1] / 255.0f;
    return (s00 * (1.0f-dx) + s10 * dx) * (1.0f-dy) +
           (s01 * (1.0f-dx) + s11 * dx) * dy;
}

// Sample colormap RGBA at UV (nearest neighbor — colormap is coarse)
static void cpu_sampleColormap(const unsigned char* data, int size, float u, float v,
                               float& r, float& g, float& b) {
    u = u - floorf(u);
    v = v - floorf(v);
    int x = (int)(u * (float)size) % size;
    int y = (int)(v * (float)size) % size;
    int idx = (y * size + x) * 4;
    // TGA format is BGRA
    b = data[idx + 0] / 255.0f;
    g = data[idx + 1] / 255.0f;
    r = data[idx + 2] / 255.0f;
}
```

**Step 2: Add displacement offset at the end of terrainElevation()**

After `result += triVert[0].z;` (line 1802), before `return (result);` (line 1805), add:

```cpp
    // --- CPU-side terrain displacement (matching GPU TES) ---
    // Access retained texture data from terrain colormap
    if (Terrain::terrainTextures2) {
        TerrainColorMap* tcm = Terrain::terrainTextures2;
        float phongAlpha = gos_GetTerrainPhongAlpha();
        float displaceScale = gos_GetTerrainDisplaceScale();
        float detailTiling = gos_GetTerrainDetailTiling();

        // Compute colormap UV from world position (matching quad.cpp UV formula)
        // u = (wx - mapTopLeft3d.x) / worldUnitsMapSide
        // v = (mapTopLeft3d.y - wy) / worldUnitsMapSide
        float cmapU = (position.x - Terrain::mapTopLeft3d.x) * Terrain::oneOverWorldUnitsMapSide;
        float cmapV = (Terrain::mapTopLeft3d.y - position.y) * Terrain::oneOverWorldUnitsMapSide;

        // --- Phong smoothing (matching TES lines 77-84) ---
        if (phongAlpha > 0.0f) {
            // Identify which 3 vertices form our triangle and get their normals
            // triVert[0..2] already set above with x,y,z positions
            // Need vertex normals — determine which pVertices correspond to triVert[]
            // The triangle vertices come from pVertex1-4 depending on uvMode and deltaX>deltaY
            PostcompVertexPtr triPV[3];
            if (uvMode == BOTTOMRIGHT) {
                if (deltaX > deltaY) {
                    triPV[0] = pVertex1; triPV[1] = pVertex2; triPV[2] = pVertex3;
                } else {
                    triPV[0] = pVertex1; triPV[1] = pVertex3; triPV[2] = pVertex4;
                }
            } else { // BOTTOMLEFT
                if ((-deltaX) > deltaY) { // deltaX was negated for BOTTOMLEFT
                    triPV[0] = pVertex2; triPV[1] = pVertex1; triPV[2] = pVertex4;
                } else {
                    triPV[0] = pVertex2; triPV[1] = pVertex4; triPV[2] = pVertex3;
                }
            }

            // Compute barycentric coordinates from the triangle
            // pos = triVert[0] + deltaX * edge0_dir + deltaY * edge1_dir
            // We use the cross-product method for 2D barycentric
            float x0 = triVert[0].x, y0 = triVert[0].y;
            float x1 = triVert[1].x, y1 = triVert[1].y;
            float x2 = triVert[2].x, y2 = triVert[2].y;
            float det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
            float bary0 = 0.333f, bary1 = 0.333f, bary2 = 0.333f;
            if (fabsf(det) > 1e-6f) {
                bary0 = ((y1 - y2) * (position.x - x2) + (x2 - x1) * (position.y - y2)) / det;
                bary1 = ((y2 - y0) * (position.x - x2) + (x0 - x2) * (position.y - y2)) / det;
                bary2 = 1.0f - bary0 - bary1;
            }

            // Interpolated position in 3D (flat, no smoothing yet)
            float flatZ = result;

            // Phong projection: project interpolated pos onto tangent plane of each vertex
            // proj_i = pos - dot(pos - v_i, n_i) * n_i
            // We only need the Z component of each projection
            float px = position.x, py = position.y, pz = result;
            float projZ0, projZ1, projZ2;
            {
                Stuff::Vector3D& n = triPV[0]->vertexNormal;
                float dx = px - triVert[0].x, dy = py - triVert[0].y, dz = pz - triVert[0].z;
                float dot = dx * n.x + dy * n.y + dz * n.z;
                projZ0 = pz - dot * n.z;
            }
            {
                Stuff::Vector3D& n = triPV[1]->vertexNormal;
                float dx = px - triVert[1].x, dy = py - triVert[1].y, dz = pz - triVert[1].z;
                float dot = dx * n.x + dy * n.y + dz * n.z;
                projZ1 = pz - dot * n.z;
            }
            {
                Stuff::Vector3D& n = triPV[2]->vertexNormal;
                float dx = px - triVert[2].x, dy = py - triVert[2].y, dz = pz - triVert[2].z;
                float dot = dx * n.x + dy * n.y + dz * n.z;
                projZ2 = pz - dot * n.z;
            }

            float phongZ = bary0 * projZ0 + bary1 * projZ1 + bary2 * projZ2;
            result = flatZ + phongAlpha * (phongZ - flatZ);
        }

        // --- Dirt texture displacement (matching TES lines 87-99) ---
        if (displaceScale > 0.0f && tcm->cpuColorMap && tcm->cpuDispAlpha) {
            // Sample colormap to get dirt weight via HSV classification
            float cr, cg, cb;
            cpu_sampleColormap(tcm->cpuColorMap, tcm->cpuColorMapSize, cmapU, cmapV, cr, cg, cb);
            float dirtWeight = cpu_getDirtWeight(cr, cg, cb);

            if (dirtWeight > 0.01f) {
                // Sample displacement texture (matNormal2 alpha) at tiled UV
                // TC_MAT_TILING.z = 1.0 for dirt
                float dispU = cmapU * detailTiling * 1.0f;
                float dispV = cmapV * detailTiling * 1.0f;
                float disp = 1.0f - cpu_sampleAlpha(tcm->cpuDispAlpha, tcm->cpuDispAlphaSize, dispU, dispV);

                // Interpolate surface normal Z for this position
                // For mostly flat terrain, normal.z ≈ 1.0
                float nz = 1.0f;
                if (phongAlpha > 0.0f) {
                    // Already computed bary coords and have triPV — but they're out of scope
                    // Use perpendicularVec.z which is the triangle face normal Z
                    float len = sqrtf(perpendicularVec.x*perpendicularVec.x +
                                     perpendicularVec.y*perpendicularVec.y +
                                     perpendicularVec.z*perpendicularVec.z);
                    if (len > 1e-6f) nz = fabsf(perpendicularVec.z) / len;
                }

                result += nz * (disp - 0.5f) * displaceScale * dirtWeight;
            }
        }
    }
```

**Important scoping note:** The `perpendicularVec` is already in scope from the base elevation calculation. The barycentric coords (`bary0/1/2`) and `triPV` need to be declared at a scope visible to both the Phong and displacement blocks. Move the Phong variable declarations (`triPV`, `bary0/1/2`, `phongAlpha`) to before the Phong if-block so they're accessible from the displacement block too.

**Step 3: Add necessary includes to mapdata.cpp**

At top of `mclib/mapdata.cpp`, add if not present:
```cpp
#include "gameos.hpp"  // for gos_GetTerrainPhongAlpha, etc.
#include "terrtxm2.h"  // for TerrainColorMap CPU data access
```

Also need access to `Terrain::terrainTextures2` — verify it's accessible (it's a static member of the Terrain class declared in terrain.h, which mapdata.cpp likely already includes).

**Step 4: Build and verify compilation**

Run: `/mc2-build`
Expected: Clean compile.

**Step 5: Deploy and test in-game**

Run: `/mc2-build-deploy`
Expected: Units should sit closer to the terrain surface, especially on dirt-classified areas.

**Step 6: Commit**

```
feat: apply Phong + dirt displacement in CPU terrainElevation()

Units were floating above tessellation-displaced terrain because the CPU
height lookup only returned base mesh elevation. Now terrainElevation()
applies the same Phong smoothing and dirt displacement that the GPU TES
does, using retained CPU copies of the colormap and matNormal2 alpha.
```

---

### Task 5: Verify and tune

**Step 1: Visual verification**

Launch the game, load a mission with visible mechs on dirt terrain. Compare unit foot placement before and after. Units should no longer float.

**Step 2: Tune if needed**

If units are slightly too high or too low, the issue is likely:
- Colormap UV mapping mismatch (check `oneOverTF` vs `oneOverWorldUnitsMapSide`)
- TGA byte order (BGRA vs RGBA — verify with `printf` of sample values)
- Phong barycentric coords (verify a few known positions)

Add `printf` diagnostics if needed: sample a known dirt area, print dirtWeight, disp, and offset values.

**Step 3: Remove debug prints**

Remove any `printf` diagnostics added during tuning.

**Step 4: Commit final tuning**

```
fix: tune CPU displacement to match GPU terrain surface
```

---

## Key Reference: UV Mapping

The colormap UV in quad.cpp uses `oneOverTF = 1.0f / 64.0f` which is `tilingFactor / worldUnitsMapSide` where `tilingFactor` is the detail tiling (default 1.0). However, for the COLORMAP specifically, the tiling is 1:1 across the map (each tile covers `COLOR_MAP_TEXTURE_SIZE` pixels of the full colormap). The global UV is:

```
u = (wx - mapTopLeft3d.x) / worldUnitsMapSide  // [0..1] across full map
v = (mapTopLeft3d.y - wy) / worldUnitsMapSide  // [0..1] across full map
```

This maps world position to the full colormap texture. The `oneOverWorldUnitsMapSide` static is available in `Terrain::`.

## Key Reference: Coordinate Spaces

- MC2 world: X=east, Y=north, Z=up (elevation)
- Vertex normals (`PostcompVertex::vertexNormal`): in MC2 world space (Z=up)
- Displacement in TES: `worldPos += worldNorm * offset` — applies along surface normal in world space
- For CPU: only the Z component of displacement matters for elevation: `result += normal.z * offset`

## Key Reference: Data Dependencies

```
terrainElevation() needs:
├── phongAlpha ← gos_GetTerrainPhongAlpha() ← gosRenderer::terrain_phong_alpha_
├── displaceScale ← gos_GetTerrainDisplaceScale() ← gosRenderer::terrain_displace_scale_
├── detailTiling ← gos_GetTerrainDetailTiling() ← gosRenderer::terrain_detail_tiling_
├── cpuColorMap ← TerrainColorMap::cpuColorMap (retained at load)
├── cpuDispAlpha ← TerrainColorMap::cpuDispAlpha (retained at load)
└── vertex normals ← PostcompVertex::vertexNormal (always available)
```
