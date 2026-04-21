# Unified Shadow Map + Stratified Poisson Sampling — Design Doc
**Date:** 2026-04-10

## Problem
Two shadow quality issues:
1. **Terrain shadows** use 3x3 PCF grid — blocky edges, visible stepping pattern
2. **Mech/asset shadows** use legacy DX6/7-era projected blob shadows — hard-edged, visible internal polygon structure, flat dark triangles at 25% opacity

## Solution

### Part 1: Stratified Poisson Disk Sampling

Replace 3x3 PCF in `shadow.hglsl` with a 16-sample Poisson Disk pattern. Each pixel rotates the disk by a hash of its world position, breaking up banding and creating organic-looking soft penumbra.

**Current (shadow.hglsl:33-40):**
```glsl
for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
        shadow += texture(shadowMap, vec3(projCoords.xy + vec2(x, y) * texelSize, currentDepth));
    }
}
shadow /= 9.0;
```

**Proposed:**
```glsl
// 16-sample Poisson disk (pre-computed offsets in unit circle)
const vec2 poissonDisk[16] = vec2[](...);

// Per-pixel rotation to break banding (stratified)
float angle = 6.2831 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
float ca = cos(angle), sa = sin(angle);
mat2 rot = mat2(ca, sa, -sa, ca);

float shadow = 0.0;
float radius = shadowSoftness; // uniform, default ~2.0 texels
for (int i = 0; i < 16; i++) {
    vec2 offset = rot * poissonDisk[i] * radius * texelSize;
    shadow += texture(shadowMap, vec3(projCoords.xy + offset, currentDepth));
}
shadow /= 16.0;
```

**New uniform:** `shadowSoftness` (float, default 2.0) — controls penumbra radius in texels. Adjustable via debug key.

### Part 2: Mech/Asset Geometry in Shadow Map

Render mech/object shapes into the same 2048x2048 shadow FBO during the shadow pre-pass. Currently only terrain is rendered; we add a second loop for objects.

**Architecture:**

```
Shadow Pre-Pass (renderLists):
  1. Bind shadow FBO, set viewport
  2. Render ALL terrain nodes as GL_PATCHES (existing)
  3. NEW: Render mech/object shapes as regular triangles
     - For each object shape node:
       - Bind shadow_object shader
       - Set uniform: lightSpaceMatrix * shape.worldMatrix
       - Draw shape's indexed mesh (existing VBO/IBO)
  4. Unbind shadow FBO
```

**New shader pair: `shadow_object.vert` + `shadow_object.frag`**

```glsl
// shadow_object.vert
layout(location = 0) in vec3 position;
uniform mat4 shadowMVP; // lightSpaceMatrix * worldMatrix
void main() {
    gl_Position = shadowMVP * vec4(position, 1.0);
}

// shadow_object.frag
void main() {
    gl_FragDepth = gl_FragCoord.z; // AMD requires explicit write
}
```

**Vertex data source:**
- `TG_HWTypeVertex::position` (model-space, `Stuff::Point3D`) stored in persistent STATIC_DRAW VBOs
- `TG_RenderShape::mw_` provides model-to-world transform
- `lightSpaceMatrix` already computed for terrain shadows

**How objects enter the shadow pass:**
- In `txmmgr.cpp::renderLists()`, during the shadow pre-pass, iterate over object render nodes
- Object nodes use `gos_RenderIndexedArray(ib_, vb_, vdecl_, mvp_)` — we replace `mvp_` with `lightSpaceMatrix * mw_`
- Need a new API function or mode flag so `gos_RenderIndexedArray` can render depth-only with the shadow shader

**Disabling legacy blob shadows:**
- In `mech3d.cpp`, `gvactor.cpp`, `bdactor.cpp` — skip `renderShadows()` calls
- In `tgl.cpp` — skip `RenderShadows()` / `MultiTransformShadows()`
- Guard with a global flag so legacy can be re-enabled if needed

### Part 3: Object-on-Object Shadows (Stretch Goal)

Currently `gos_tex_vertex.frag` (used for objects) does NOT sample the shadow map. To make objects receive shadows from other objects:
- Add `#include <include/shadow.hglsl>` to the object fragment shader
- Pass `lightSpaceMatrix`, `enableShadows`, bind shadow map on unit 9
- Sample shadow in the object fragment shader

This is optional — terrain receiving mech shadows is the primary goal.

## Files to Modify

### Shader changes
- `shaders/include/shadow.hglsl` — Poisson disk sampling, new `shadowSoftness` uniform
- NEW: `shaders/shadow_object.vert` — depth-only vertex shader for objects
- NEW: `shaders/shadow_object.frag` — explicit gl_FragDepth write (AMD)

### C++ changes
- `GameOS/gameos/gameos_graphics.cpp` — load shadow_object shader, extend shadow pre-pass for indexed geometry, expose `shadowSoftness` uniform
- `GameOS/include/gameos.hpp` — declare new shadow API functions
- `mclib/txmmgr.cpp` — add object rendering loop in shadow pre-pass
- `mclib/mech3d.cpp` — skip legacy renderShadows()
- `mclib/gvactor.cpp` — skip legacy renderShadows()
- `mclib/bdactor.cpp` — skip legacy renderShadows()
- `mclib/tgl.cpp` — skip RenderShadows() / MultiTransformShadows()

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Object VBO vertex layout doesn't match shadow shader | Check TG_HWTypeVertex layout; shadow shader only needs position (location 0) |
| AMD driver issues with new shader | Follow all AMD rules: attrib 0 active, explicit gl_FragDepth, GL_FALSE transpose |
| Shadow map resolution insufficient for mech detail | 2048x2048 should be fine at current shadow radius (8000 units) |
| Legacy blob shadow removal breaks something | Guard behind global flag, keep code but skip execution |
| Object world matrices not available during shadow pass | They're stored in TG_RenderShape::mw_, should persist through renderLists() |

## Debug Keys

- Existing `RAlt+F3` toggles terrain shadows
- Add `RAlt+F4` (or similar) to toggle object shadows independently
- `Ctrl+Shift+7/8` to adjust `shadowSoftness` up/down
