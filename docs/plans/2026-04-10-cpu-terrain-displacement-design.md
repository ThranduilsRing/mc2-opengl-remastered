# CPU Terrain Displacement — Design Doc
**Date:** 2026-04-10

## Problem
Units (mechs, vehicles, buildings, turrets) float above tessellation-displaced terrain. The GPU TES applies Phong smoothing and dirt texture displacement to terrain vertices, but `MapData::terrainElevation()` on the CPU returns only the base mesh height. Every system using `terrainElevation()` (movement, pathfinding, collision, appearance) gets a height that's lower than the visible surface.

## Solution
Enhance `MapData::terrainElevation()` to apply the same two displacement stages the GPU TES does:

### 1. Phong Smoothing
The TES projects the barycentric-interpolated position onto tangent planes defined by each vertex normal, then blends toward the projected position by `phongAlpha` (default 0.5).

**CPU equivalent:** `terrainElevation()` already identifies which triangle contains the query point and computes delta offsets from the triangle origin. We compute proper barycentric coordinates from those deltas, then apply the same Phong projection formula using `PostcompVertex::vertexNormal`.

**Formula (matching TES lines 77-84):**
```
proj_i = pos - dot(pos - v_i, n_i) * n_i   // for each triangle vertex i
phongPos = bary.x * proj0 + bary.y * proj1 + bary.z * proj2
displaced.z = mix(flatZ, phongPos.z, phongAlpha)
```

### 2. Dirt Texture Displacement
The TES samples the colormap at the fragment's UV, classifies it via HSV into material weights, then displaces along the surface normal by `(matNormal2.alpha - 0.5) * displaceScale * dirtWeight`.

**CPU equivalent:** Convert world position to colormap UV, sample the retained CPU colormap data, apply HSV classification to get dirt weight, sample retained matNormal2 alpha, compute vertical displacement offset.

**Formula (matching TES lines 87-99):**
```
colormapUV = worldPosToUV(position)
dirtWeight = hsvClassify(sampleColormap(colormapUV)).z
dispUV = colormapUV * detailNormalTiling * TC_MAT_TILING_DIRT  // 1.0 * 1.0
disp = 1.0 - sampleMatNormal2Alpha(dispUV)
elevation += normal.z * (disp - 0.5) * displaceScale * dirtWeight
```

## Data Retention

Currently, terrain texture pixel data is freed immediately after GPU upload. We must retain:

| Data | Current state | Action | Size |
|------|--------------|--------|------|
| matNormal2 alpha | Freed in terrtxm2.cpp:2148 | Keep alpha channel copy | ~256 KB (512x512 * 1 byte) |
| Colormap pixels | Freed in terrtxm2.cpp:1399 | Keep RGBA per-tile or build unified RGB buffer | ~1-4 MB |

## UV Mapping (world position -> texture coordinates)

From quad.cpp line 2140:
```
u = (wx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX
v = (Terrain::mapTopLeft3d.y - wy) * oneOverTF + cloudOffsetY
```
Where `oneOverTF = 1.0 / 64.0` for base terrain. Cloud offset is visual-only (animated); CPU ignores it.

For displacement texture sampling, the UV is further multiplied by `detailNormalTiling.x * TC_MAT_TILING.z` (both default 1.0).

## Files to Modify

### 1. mclib/terrtxm2.cpp
- **Colormap retention:** After loading colormap tiles (~line 1316), keep a CPU copy of the full colormap RGBA data before freeing the heap. Store pointer in TerrainColorMap or a new global.
- **matNormal2 retention:** After loading normal maps (~line 2100), keep a copy of matNormal2's alpha channel before freeing.

### 2. GameOS/gameos/gameos_graphics.cpp
- Store CPU copy of matNormal2 alpha data in gosRenderer (new field).
- Expose via `gos_GetTerrainDisplacementData()` accessor returning alpha data pointer and dimensions.
- Expose `gos_GetTerrainDisplacementScale()` returning `terrain_displace_scale_` and `terrain_phong_alpha_`.

### 3. GameOS/include/gameos.hpp
- Declare new accessor functions.

### 4. mclib/mapdata.cpp — terrainElevation()
- After computing base elevation (line 1802), add displacement:
  1. Compute barycentric coords from existing deltaX/deltaY
  2. Get vertex normals from pVertex1-4 for the selected triangle
  3. Apply Phong smoothing to Z using barycentric coords + normals
  4. Convert world position to colormap UV
  5. Sample colormap, HSV classify, get dirt weight
  6. Sample matNormal2 alpha at tiled UV
  7. Add `normal.z * (disp - 0.5) * displaceScale * dirtWeight`

### 5. mclib/terrain.h / mapdata.h
- Add fields for CPU colormap data pointer, dimensions
- Add field for matNormal2 alpha pointer, dimensions

## Edge Cases

- **Water areas:** HSV classification returns water weight, which maps to rock (w.x += isWater). Dirt weight will be ~0 over water, so no displacement — correct behavior.
- **Map edges:** `terrainElevation()` already returns 0.0 for out-of-bounds positions.
- **Zero displaceScale:** When displacement is disabled (scale = 0), no offset added — matches GPU behavior.
- **No Phong (alpha = 0):** Phong step is skipped — matches GPU behavior.

## What This Fixes
- All `land->getTerrainElevation()` callers automatically get corrected heights
- Mechs, vehicles, buildings, turrets — visual position matches terrain surface
- Pathfinding queries — units navigate on the displaced surface
- Weapon fire height checks — line-of-sight uses correct terrain height

## What This Doesn't Fix
- View-dependent tessellation LOD (minor sub-vertex differences, acceptable)
- GPU bilinear sampling vs CPU nearest-neighbor (negligible visual difference)
- Shadow banding (separate issue, view-dependent terrain geometry)
