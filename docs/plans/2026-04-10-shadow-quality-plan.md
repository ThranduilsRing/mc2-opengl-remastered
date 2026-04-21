# Unified Shadow Map + Stratified Poisson Sampling — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace blocky 3x3 PCF with smooth Poisson Disk sampling for terrain shadows, and render mech/object geometry into the shadow map to replace ugly legacy blob shadows.

**Architecture:** Two independent improvements sharing the same shadow map infrastructure. Part 1 (Poisson) is shader-only. Part 2 (object shadows) extends the shadow pre-pass to render non-tessellated object geometry with a simple depth-only shader, then disables legacy blob shadow rendering.

**Tech Stack:** GLSL 420, OpenGL 4.x (sampler2DShadow, Poisson disk), C++ (GameOS renderer, MC2 texture manager)

---

### Task 1: Stratified Poisson Disk Sampling in shadow.hglsl

**Files:**
- Modify: `shaders/include/shadow.hglsl` (replace 3x3 PCF with 16-sample Poisson disk)

**Step 1: Replace the PCF loop with Poisson disk sampling**

Replace the contents of `shaders/include/shadow.hglsl` with:

```glsl
#ifndef __SHADOW_HGLSL__
#define __SHADOW_HGLSL__

uniform sampler2DShadow shadowMap;
uniform mat4 lightSpaceMatrix;
uniform int enableShadows;
uniform float shadowSoftness;  // penumbra radius in texels, default 2.5

// 16-sample Poisson disk (well-distributed points in unit circle)
const vec2 poissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507),
    vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790)
);

float calcShadow(vec3 worldPos, vec3 normal, vec3 lightDir)
{
    if (enableShadows == 0) return 1.0;

    vec4 lsPos = lightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords = projCoords * 0.5 + 0.5;

    // Outside shadow map — fully lit
    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;

    // Back-face guard: surface facing away from light
    float NdotL = max(dot(normal, lightDir), 0.0);
    if (NdotL < 0.15) return 1.0;

    // Slope-dependent depth bias
    float bias = max(0.008 * (1.0 - NdotL), 0.003);
    float currentDepth = projCoords.z - bias;

    // Per-pixel rotation to break banding (stratified sampling)
    float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    // Poisson disk sampling with rotation
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float radius = max(shadowSoftness, 0.5);
    float shadow = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 offset = rot * poissonDisk[i] * radius * texelSize;
        shadow += texture(shadowMap, vec3(projCoords.xy + offset, currentDepth));
    }
    shadow /= 16.0;

    // Shadow ambient — 0.4 = darkest shadow, 1.0 = fully lit
    return mix(0.4, 1.0, shadow);
}

#endif
```

**Step 2: Add shadowSoftness uniform upload in gameos_graphics.cpp**

In `terrainDrawIndexedPatches()` (~line 2200), after the existing uniform uploads, add:

```cpp
loc = glGetUniformLocation(shp, "shadowSoftness");
if (loc >= 0) glUniform1f(loc, terrain_shadow_softness_);
```

Add field to gosRenderer class (~line 1430):
```cpp
float terrain_shadow_softness_ = 2.5f;
```

Add setter/getter:
```cpp
void setTerrainShadowSoftness(float s) { terrain_shadow_softness_ = s; }
float getTerrainShadowSoftness() const { return terrain_shadow_softness_; }
```

**Step 3: Add debug key to adjust softness**

In `code/mission.cpp` tessellation debug keys section (~line 393), add Ctrl+Shift+7/8 to adjust `shadowSoftness` up/down by 0.5, calling a new `gos_SetTerrainShadowSoftness()` API.

Declare in gameos.hpp:
```cpp
void gos_SetTerrainShadowSoftness(float s);
float gos_GetTerrainShadowSoftness();
```

**Step 4: Build, deploy, verify soft terrain shadows**

Run: `/mc2-build-deploy`
Expected: Terrain shadows have smooth, organic penumbra instead of blocky 3x3 grid. Adjustable with Ctrl+Shift+7/8.

**Step 5: Commit**

```
feat: replace 3x3 PCF with 16-sample Stratified Poisson Disk shadows

Terrain shadows now use a rotated Poisson disk pattern instead of a
regular 3x3 grid. Per-pixel rotation breaks banding for organic-looking
soft penumbra. Adjustable via Ctrl+Shift+7/8 (shadowSoftness parameter).
```

---

### Task 2: Create shadow_object shader pair

**Files:**
- Create: `shaders/shadow_object.vert`
- Create: `shaders/shadow_object.frag`

**Step 1: Create shadow_object.vert**

```glsl
//#version 420 (provided by material prefix)

// TG_HWTypeVertex layout (36 bytes):
// position (vec3), normal (vec3), aRGBLight (uint), u (float), v (float)
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;      // unused but must declare (AMD attrib 0/1)
layout(location = 2) in vec4 aRGBLight;   // unused
layout(location = 3) in vec2 texcoord;    // unused

uniform mat4 shadowMVP;  // lightSpaceMatrix * worldMatrix

void main() {
    gl_Position = shadowMVP * vec4(position, 1.0);
}
```

**Step 2: Create shadow_object.frag**

```glsl
//#version 420 (provided by material prefix)

void main() {
    gl_FragDepth = gl_FragCoord.z;  // AMD requires explicit write
}
```

**Step 3: Build and verify shaders compile**

These will be loaded in Task 3 — just create files for now.

**Step 4: Commit**

```
feat: add shadow_object shader pair for depth-only object shadow pass
```

---

### Task 3: Load shadow_object material and extend shadow pre-pass API

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (~lines 1555, 2021, 1270, 1440)
- Modify: `GameOS/include/gameos.hpp`

**Step 1: Add shadow_object material to gosRenderer**

In the gosRenderer class, add field (~line 1423):
```cpp
gosRenderMaterial* shadow_object_material_ = nullptr;
```

In `gosRenderer::init()`, after the shadow_terrain material load (~line 1564), add:
```cpp
{
    gosMaterialVariationHelper helper;
    gosMaterialVariation mvar;
    helper.getMaterialVariation(mvar);
    shadow_object_material_ = gosRenderMaterial::load("shadow_object", mvar);
    if (shadow_object_material_) {
        materialList_.push_back(shadow_object_material_);
    }
}
```

**Step 2: Ensure shadow_object uses VS+FS only (no tessellation)**

In `gosRenderMaterial::load()` (~line 233), the tessellation check currently includes `shadow_terrain`. Make sure `shadow_object` does NOT match that condition — it should fall through to the regular `makeProgram(vs, ps)` path. Verify the condition does NOT include `shadow_object`:

```cpp
if (strcmp(shader, "gos_terrain") == 0 || strcmp(shader, "shadow_terrain") == 0) {
    // tessellation path — shadow_object should NOT enter here
```

**Step 3: Add drawShadowObjectBatch() to gosRenderer**

Add a new method for rendering non-tessellated geometry into the shadow FBO:

```cpp
void gosRenderer::drawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4)
{
    if (!shadow_prepass_active_ || !shadow_object_material_) return;

    shadow_object_material_->apply();
    GLuint shp = shadow_object_material_->getShader()->shp_;

    // Compute shadowMVP = lightSpaceMatrix * worldMatrix
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    const float* lsm = pp->getLightSpaceMatrix();
    // Manual 4x4 multiply: shadowMVP = lsm * worldMatrix
    float shadowMVP[16];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            shadowMVP[row * 4 + col] =
                lsm[row * 4 + 0] * worldMatrix4x4[0 * 4 + col] +
                lsm[row * 4 + 1] * worldMatrix4x4[1 * 4 + col] +
                lsm[row * 4 + 2] * worldMatrix4x4[2 * 4 + col] +
                lsm[row * 4 + 3] * worldMatrix4x4[3 * 4 + col];
        }
    }

    GLint loc = glGetUniformLocation(shp, "shadowMVP");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, shadowMVP);

    // Bind VB/IB and draw
    gos_RenderIndexedArray(ib, vb, vdecl);

    shadow_object_material_->end();
}
```

**Step 4: Expose via GameOS API**

In gameos.hpp:
```cpp
void gos_DrawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4);
```

In gameos_graphics.cpp:
```cpp
void gos_DrawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4)
{
    if (g_gos_renderer) g_gos_renderer->drawShadowObjectBatch(vb, ib, vdecl, worldMatrix4x4);
}
```

**Step 5: Build and verify**

Run: `/mc2-build`
Expected: Clean compile.

**Step 6: Commit**

```
feat: add shadow_object material and drawShadowObjectBatch API
```

---

### Task 4: Render objects into shadow map during pre-pass

**Files:**
- Modify: `mclib/txmmgr.cpp` (~lines 1046-1078, the shadow pre-pass section)

**Step 1: Add object shadow loop after terrain shadow loop**

In `renderLists()`, AFTER the existing terrain shadow loop (which ends before `gos_EndShadowPrePass()` at ~line 1077), add a loop over hardware vertex nodes:

```cpp
    // --- Object shadow pass: render mech/object shapes into shadow map ---
    for (size_t si = 0; si < nextAvailableHardwareVertexNode; si++)
    {
        if (!(masterHardwareVertexNodes[si].flags & MC2_DRAWSOLID)) continue;
        if (masterHardwareVertexNodes[si].flags & MC2_ISTERRAIN) continue;  // terrain already done
        if (!masterHardwareVertexNodes[si].shapes) continue;

        uint32_t totalShapes = masterHardwareVertexNodes[si].numShapes;
        for (uint32_t sh = 0; sh < totalShapes; ++sh)
        {
            TG_RenderShape* rs = masterHardwareVertexNodes[si].shapes + sh;
            if (!rs->vb_ || !rs->ib_) continue;

            // Convert Stuff::Matrix4D to float[16] for GameOS API
            mat4 world_mat = gos2my(rs->mw_);
            gos_DrawShadowObjectBatch(rs->vb_, rs->ib_, rs->vdecl_, (const float*)&world_mat);
        }
    }
```

**Important:** This must go AFTER the terrain loop and BEFORE `gos_EndShadowPrePass()`.

**Step 2: Verify the gos2my conversion is available**

`gos2my()` is a utility in txmmgr.cpp that converts `Stuff::Matrix4D` to the `mat4` type used by GameOS. It's already used for the object rendering path. Verify it's in scope.

**Step 3: Build, deploy, verify object shadows appear on terrain**

Run: `/mc2-build-deploy`
Expected: Mechs and buildings should now cast proper directional shadows onto the terrain via the shadow map. The shadows will be the same quality as terrain shadows (Poisson disk soft edges).

**Step 4: Commit**

```
feat: render mech/object geometry into terrain shadow map

Objects are now rendered into the same 2048x2048 shadow FBO as terrain
during the shadow pre-pass. Uses shadow_object shader with
lightSpaceMatrix * worldMatrix for depth-only rendering.
```

---

### Task 5: Disable legacy blob shadow rendering

**Files:**
- Modify: `mclib/mech3d.cpp:2844-2862` (Mech3DAppearance::renderShadows)
- Modify: `mclib/gvactor.cpp` (GVAppearance::renderShadows — find similar function)
- Modify: `mclib/bdactor.cpp` (BldgAppearance::renderShadows — find similar function)

**Step 1: Guard legacy shadow rendering**

In each file's `renderShadows()` function, add an early return if shadow map shadows are active:

```cpp
long Mech3DAppearance::renderShadows(void)
{
    // Skip legacy blob shadows when shadow map is active
    if (gos_IsTerrainTessellationActive())
        return NO_ERR;

    // ... existing legacy code unchanged ...
}
```

Apply the same pattern to `GVAppearance::renderShadows()` and any building shadow function.

**Note:** Using `gos_IsTerrainTessellationActive()` as the guard because the shadow map system is tied to tessellation being active. When tessellation is off (legacy branch), blob shadows still work.

**Step 2: Build, deploy, verify no double-shadows**

Run: `/mc2-build-deploy`
Expected: Mechs have clean shadow-map shadows only. No more ugly blob shadows underneath.

**Step 3: Commit**

```
feat: disable legacy blob shadows when shadow map is active

Legacy projected blob shadow rendering is skipped when tessellation
shadow maps are active. Shadow map provides higher quality directional
shadows for all objects.
```

---

### Task 6: Verify, tune, and fix issues

**Step 1: Visual verification**

Load a mission with mechs on varied terrain. Check:
- Terrain shadows are soft (Poisson disk, no blocky grid)
- Mech shadows appear on terrain under each mech
- Shadow direction matches the sun direction
- No shadow artifacts (peter-panning, acne, light leaking)
- Adjust shadowSoftness with debug keys if needed

**Step 2: Check for vertex format issues**

If mech shadows don't appear or look wrong, the issue is likely:
- TG_HWTypeVertex layout mismatch with shadow_object.vert attributes
- World matrix (mw_) row/column order mismatch (GL_FALSE vs GL_TRUE transpose)
- Vertex declaration (vdecl_) not matching shadow shader expectations

Debug by adding a printf in drawShadowObjectBatch to confirm shapes are being drawn.

**Step 3: Tune shadow bias if needed**

Object shadows may need different bias than terrain. If shadow acne appears on objects, consider adding a bias uniform to shadow_object.vert:
```glsl
gl_Position.z += 0.001; // small depth offset
```

**Step 4: Commit final tuning**

```
fix: tune shadow parameters for unified shadow map
```

---

## Key Reference: Matrix Conventions

- `Stuff::Matrix4D` — MC2's native 4x4 matrix (row-major)
- `gos2my()` — converts Stuff::Matrix4D to `mat4` (column-major, transposed)
- `lightSpaceMatrix` — uploaded with `GL_FALSE` (already in correct order from postprocess)
- Shadow terrain material uses `GL_FALSE` for lightSpaceMatrix
- For `shadowMVP = lsm * worldMatrix`: both should be in the same convention. `lsm` comes from `pp->getLightSpaceMatrix()` (same float[16] used for terrain). `worldMatrix` comes from `gos2my(rs->mw_)`.

## Key Reference: AMD Driver Rules

- `shadow_object.vert` MUST have `layout(location = 0) in vec3 position` active (AMD skips draws without attrib 0)
- `shadow_object.frag` MUST write `gl_FragDepth` explicitly (AMD optimizes away empty frag shaders)
- Shadow texture on unit 9 MUST be unbound before rendering to shadow FBO (feedback loop crashes)
- These are already handled by `beginShadowPrePass()` — no extra work needed

## Key Reference: Render Order

```
renderLists() phases:
  1. Shadow pre-pass to shadow FBO:
     a. Terrain (tessellated GL_PATCHES) — existing
     b. Objects (triangles) — NEW
  2. DRAWSOLID terrain (tessellated, writes depth+stencil)
  3. DRAWSOLID objects (ShapeRenderer)
  4. DRAWALPHA detail textures
  5. DRAWALPHA+ISCRATERS overlays
  6. Non-terrain craters
  7. Non-terrain alpha draws (was: legacy blob shadows here)
  8. Water layers
  9. Terrain shadows (decal-style)
```
