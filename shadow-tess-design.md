# Tessellated Shadow Pass + Light Direction — Design

Date: 2026-04-10
Branch: claude/nifty-mendeleev

## Problem

Shadow depth is written from flat GL_TRIANGLES (extras VBO only, no IBO) while terrain is
shaded via GL_PATCHES through the full tessellation pipeline (main VBO + IBO + extras).
This geometry mismatch causes:
1. Wave-pattern self-shadowing (different triangle topology in shadow vs shading)
2. False shadows on hillsides (when displacement is enabled, shadow depth doesn't match)
3. Camera-dependent shadow shifts (TCS LOD changes shaded geometry, shadow stays flat)
4. Ambient cranked to 0.85 to mask artifacts

Additionally, `terrainLightDir` is (0,0,0) and shadow sun direction is hardcoded (0.3, 0.7, 0.2).

## Approach: Separate Shadow Tessellation Shaders (Approach A)

### New Shaders

**shadow_terrain.vert** (modify existing):
- Accept standard vertex attribs (pos at 0, texcoord at 3) + worldPos at 4, worldNorm at 5
- Pass worldPos, worldNorm, texcoord to TCS
- AMD attrib 0 dummy read: reuse pos

**shadow_terrain.tesc** (new):
- Same distance-based LOD as gos_terrain.tesc (uses player cameraPos)
- Passes through: worldPos, worldNorm, texcoord
- No color/fog/terrainType needed (shadow depth only)

**shadow_terrain.tese** (new):
- Barycentric interpolation of worldPos, worldNorm, texcoord
- Displacement via terrain_common.hglsl (dirt-only, same as main TES)
- No Phong smoothing (cosmetic only, doesn't affect shadow silhouette)
- Projection: `gl_Position = lightSpaceMatrix * vec4(worldPos, 1.0);`
- No terrainMVP, no viewport params, no 3-step MC2 pipeline

**shadow_terrain.frag** (unchanged):
- `gl_FragDepth = gl_FragCoord.z;` (AMD requirement)

### Modified Shadow Draw Path

**txmmgr.cpp** shadow loop (lines 1049-1070):
- Pass vertices + totalVerts + indexArray + extras to new API
- `gos_DrawShadowBatchTessellated(verts, numVerts, indices, numIndices, extras, extraCount)`

**gameos_graphics.cpp** new `drawShadowBatchTessellated()`:
1. Upload vertices+indices to `indexed_tris_` mesh
2. Apply `shadow_terrain_tess_material_` (new 5-stage shader)
3. Upload uniforms: lightSpaceMatrix, tessLevel, tessDistanceRange, tessDisplace,
   cameraPos, detailNormalTiling
4. Bind colormap (tex1 unit 0) + matNormal2 (unit 7) for displacement sampling
5. Bind main VBO + IBO from indexed_tris_
6. Bind extras VBO at locations 4-5 (worldPos, worldNorm)
7. glPatchParameteri(GL_PATCH_VERTICES, 3) + glDrawElements(GL_PATCHES, ...)
8. Cleanup attrib arrays

**Material init** in gosRenderer::init():
- Compile shadow_terrain.vert + shadow_terrain.tesc + shadow_terrain.tese + shadow_terrain.frag
- Store as `shadow_terrain_tess_material_`

### Light Direction Fix

**gamecam.cpp ~line 176**: After setting camera position, set light direction:
```cpp
gos_SetTerrainLightDir(-lightDirection.x, lightDirection.z, lightDirection.y);
```
Same MC2->GL swizzle (-x, z, y) as camera position.

**gameosmain.cpp draw_screen()**: Replace hardcoded (0.3, 0.7, 0.2):
```cpp
float lx, ly, lz;
gos_GetTerrainLightDir(&lx, &ly, &lz);
pp->updateLightMatrix(lx, ly, lz, cx, cy, cz, 1500.0f);
```

### Shadow Parameter Tuning (P3)

- Lower ambient 0.85 -> ~0.5-0.6
- Re-enable NdotL < 0.1 back-face skip in shadow.hglsl
- Re-enable normal-offset shadow lookup
- Tune shadow radius vs visible terrain
- Consider 5x5 PCF

## Key Constraints

- AMD attrib 0 must be active (dummy read in vert shader)
- AMD explicit gl_FragDepth required
- No sampler2DArray (crashes AMD)
- No #version in shader files (prefix via makeProgram)
- Uniform API: set before apply()
- GL_FALSE transpose for direct-uploaded matrices
- Unbind shadow texture from unit 9 before writing to shadow FBO
