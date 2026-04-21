# Tessellated Shadow Pass + Light Direction — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace flat GL_TRIANGLES shadow depth pass with tessellated GL_PATCHES pass that matches the terrain shading pipeline, and wire up the game's actual light direction for both shadow mapping and terrain N·L lighting.

**Architecture:** New shadow tessellation shaders (vert/tesc/tese/frag) compile into a shadow material loaded at init. The txmmgr shadow loop passes vertices+indices+extras through a new API. A new `drawShadowBatchTessellated` function mirrors `terrainDrawIndexedPatches` but projects through lightSpaceMatrix. Light direction flows from Camera::lightDirection through gamecam.cpp to both the terrain shader and shadow matrix.

**Tech Stack:** OpenGL 4.2, GLSL 420, MC2 engine (DX6-era with GL port), AMD RX 7900 XTX driver workarounds.

**Current state context:** tess=1 (no subdivision), Phong disabled, displacement disabled. Shadow TES must support displacement for when these are re-enabled, but the immediate win is matching geometry topology (indexed VBO+IBO+extras vs extras-only).

---

### Task 1: Create shadow_terrain.tesc (TCS)

**Files:**
- Create: `shaders/shadow_terrain.tesc`

**Step 1: Create shadow TCS shader**

This is a subset of `gos_terrain.tesc` — same distance LOD logic, but only passes through worldPos, worldNorm, and texcoord (no color, fog, terrainType needed for depth-only).

```glsl
//#version 420 (version provided by material prefix)

layout(vertices = 3) out;

in vec3 vs_WorldPos[];
in vec3 vs_WorldNorm[];
in vec2 vs_Texcoord[];

out vec3 tcs_WorldPos[];
out vec3 tcs_WorldNorm[];
out vec2 tcs_Texcoord[];

uniform vec4 tessLevel;         // x=inner, y=outer
uniform vec4 tessDistanceRange; // x=near, y=far
uniform vec4 cameraPos;

void main()
{
    tcs_WorldPos[gl_InvocationID]  = vs_WorldPos[gl_InvocationID];
    tcs_WorldNorm[gl_InvocationID] = vs_WorldNorm[gl_InvocationID];
    tcs_Texcoord[gl_InvocationID]  = vs_Texcoord[gl_InvocationID];

    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

    if (gl_InvocationID == 0) {
        vec3 mid01 = 0.5 * (vs_WorldPos[0] + vs_WorldPos[1]);
        vec3 mid12 = 0.5 * (vs_WorldPos[1] + vs_WorldPos[2]);
        vec3 mid20 = 0.5 * (vs_WorldPos[2] + vs_WorldPos[0]);
        vec3 center = (vs_WorldPos[0] + vs_WorldPos[1] + vs_WorldPos[2]) / 3.0;

        float d01 = distance(cameraPos.xyz, mid01);
        float d12 = distance(cameraPos.xyz, mid12);
        float d20 = distance(cameraPos.xyz, mid20);
        float dc  = distance(cameraPos.xyz, center);

        float near = tessDistanceRange.x;
        float far  = tessDistanceRange.y;
        float maxTess = max(tessLevel.x, 1.0);

        gl_TessLevelOuter[0] = mix(maxTess, 1.0, smoothstep(near, far, d12));
        gl_TessLevelOuter[1] = mix(maxTess, 1.0, smoothstep(near, far, d20));
        gl_TessLevelOuter[2] = mix(maxTess, 1.0, smoothstep(near, far, d01));
        gl_TessLevelInner[0] = mix(maxTess, 1.0, smoothstep(near, far, dc));
    }
}
```

**Step 2: Commit**

```
git add shaders/shadow_terrain.tesc
git commit -m "feat: add shadow terrain TCS (distance LOD matching main terrain)"
```

---

### Task 2: Create shadow_terrain.tese (TES)

**Files:**
- Create: `shaders/shadow_terrain.tese`

**Step 1: Create shadow TES shader**

Same displacement as `gos_terrain.tese` via `terrain_common.hglsl`, but projection is a simple `lightSpaceMatrix * worldPos` instead of the 3-step MC2 pipeline. No Phong smoothing (cosmetic only, won't affect shadow silhouette at shadow-map resolution).

```glsl
//#version 420 (version provided by material prefix)

layout(triangles, equal_spacing, ccw) in;

in vec3 tcs_WorldPos[];
in vec3 tcs_WorldNorm[];
in vec2 tcs_Texcoord[];

uniform vec4 tessDisplace;      // x=phongAlpha (unused here), y=displaceScale
uniform mat4 lightSpaceMatrix;

// Textures for displacement sampling
uniform sampler2D tex1;         // colormap (for material classification)
uniform sampler2D matNormal2;   // dirt normal+disp (only dirt displaces)
uniform vec4 detailNormalTiling; // .x = base tiling multiplier

#include <include/terrain_common.hglsl>

void main()
{
    vec3 bary = gl_TessCoord;

    vec3 worldPos = bary.x * tcs_WorldPos[0]
                  + bary.y * tcs_WorldPos[1]
                  + bary.z * tcs_WorldPos[2];

    vec3 worldNorm = normalize(
        bary.x * tcs_WorldNorm[0]
      + bary.y * tcs_WorldNorm[1]
      + bary.z * tcs_WorldNorm[2]);

    vec2 texcoord = bary.x * tcs_Texcoord[0]
                  + bary.y * tcs_Texcoord[1]
                  + bary.z * tcs_Texcoord[2];

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
}
```

**Step 2: Commit**

```
git add shaders/shadow_terrain.tese
git commit -m "feat: add shadow terrain TES (displacement + lightSpaceMatrix projection)"
```

---

### Task 3: Modify shadow_terrain.vert to pass through tessellation attributes

**Files:**
- Modify: `shaders/shadow_terrain.vert`

**Step 1: Update shadow vertex shader**

The current shadow vert only reads worldPos (location 4). For TCS/TES it also needs: pos (location 0, AMD attrib 0 requirement + used for gl_Position passthrough), texcoord (location 3, for displacement sampling in TES), and worldNorm (location 5, for displacement direction).

Replace the entire file with:

```glsl
//#version 420 (version provided by prefix)

layout(location = 0) in vec4 pos;       // AMD requires attrib 0 active
layout(location = 3) in vec2 texcoord;
layout(location = 4) in vec3 worldPos;
layout(location = 5) in vec3 worldNorm;

uniform mat4 mvp;  // projection_ for screen-space fallback (TES overrides gl_Position)

out vec3 vs_WorldPos;
out vec3 vs_WorldNorm;
out vec2 vs_Texcoord;

void main()
{
    vs_WorldPos = worldPos;
    vs_WorldNorm = worldNorm;
    vs_Texcoord = texcoord;

    // Screen-space position (TES overrides this with lightSpaceMatrix projection)
    vec4 p = mvp * vec4(pos.xyz, 1);
    gl_Position = p / pos.w;
}
```

**Important:** The `mvp * pos / pos.w` pattern matches the main terrain vert shader. TES overrides gl_Position, so this is just a passthrough for the fixed-function clipper between VS and TCS.

**Step 2: Commit**

```
git add shaders/shadow_terrain.vert
git commit -m "feat: update shadow vert to pass worldPos/worldNorm/texcoord to TCS"
```

---

### Task 4: Wire up shadow tessellation material in shader loader + renderer init

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp:233` (shader loader — add shadow_terrain TCS/TES check)
- Modify: `GameOS/gameos/gameos_graphics.cpp:1418-1419` (add `shadow_terrain_tess_material_` member)
- Modify: `GameOS/gameos/gameos_graphics.cpp:1550-1560` (init — load tessellated shadow material)

**Step 1: Add TCS/TES loading for shadow_terrain**

In `gosRenderMaterial::load()` at line 233, the check `strcmp(shader, "gos_terrain") == 0` triggers TCS/TES loading. Add a similar check for `shadow_terrain`:

```cpp
// At line 233, modify the if-block:
            if (strcmp(shader, "gos_terrain") == 0 || strcmp(shader, "shadow_terrain") == 0) {
```

This makes `gosRenderMaterial::load("shadow_terrain", ...)` automatically load `shaders/shadow_terrain.tesc` and `shaders/shadow_terrain.tese` alongside the vert/frag.

**Step 2: Replace `shadow_terrain_material_` with tessellated version**

The existing `shadow_terrain_material_` at line 1419 will now automatically compile with TCS/TES because of the loader change in Step 1. No separate member needed — the same material pointer gets the tessellated program.

**But** `beginShadowPrePass()` at line 2042 calls `shadow_terrain_material_->apply()` and then `drawShadowBatch()` draws GL_TRIANGLES. After our change, the shader has TCS/TES, so drawing GL_TRIANGLES would be incorrect — must draw GL_PATCHES. This is addressed in Task 5.

**Step 3: Commit**

```
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat: load shadow_terrain shader with TCS/TES for tessellation"
```

---

### Task 5: Add drawShadowBatchTessellated function

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp:1287-1290` (add declaration in class)
- Modify: `GameOS/gameos/gameos_graphics.cpp:2051` (add new function after drawShadowBatch)
- Modify: `GameOS/include/gameos.hpp:2274` (add API declaration)
- Modify: `GameOS/gameos/gameos_graphics.cpp` bottom (add API wrapper)

**Step 1: Add class declaration**

After `drawShadowBatch` declaration at line 1289, add:

```cpp
        void drawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
            WORD* indices, int numIndices,
            const gos_TERRAIN_EXTRA* extras, int extraCount);
```

**Step 2: Add the function implementation**

After `drawShadowBatch()` at line 2074, add:

```cpp
void gosRenderer::drawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount)
{
    if (!shadow_prepass_active_ || numVerts <= 0 || extraCount <= 0) return;

    // Upload vertices + indices to indexed_tris_ (same mesh used by normal terrain draw)
    indexed_tris_->rewind();
    indexed_tris_->addVertices(vertices, numVerts);
    indexed_tris_->addIndices(indices, numIndices);
    indexed_tris_->uploadBuffers();

    // Shadow material already applied in beginShadowPrePass (shader + lightSpaceMatrix)
    // Upload tessellation uniforms
    GLuint shp = shadow_terrain_material_->getShader()->shp_;
    GLint loc;

    float tessParams[4] = { terrain_tess_level_, terrain_tess_level_, 0.0f, 0.0f };
    float tessDist[4] = { terrain_tess_dist_near_, terrain_tess_dist_far_, 0.0f, 0.0f };
    float tessDisp[4] = { 0.0f, terrain_displace_scale_, 0.0f, 0.0f };  // no Phong in shadow

    loc = glGetUniformLocation(shp, "tessLevel");
    if (loc >= 0) glUniform4fv(loc, 1, tessParams);
    loc = glGetUniformLocation(shp, "tessDistanceRange");
    if (loc >= 0) glUniform4fv(loc, 1, tessDist);
    loc = glGetUniformLocation(shp, "tessDisplace");
    if (loc >= 0) glUniform4fv(loc, 1, tessDisp);
    loc = glGetUniformLocation(shp, "cameraPos");
    if (loc >= 0) glUniform4fv(loc, 1, (const float*)&terrain_camera_pos_);

    // projection_ needed by shadow vert shader (TES overrides gl_Position, but VS needs it)
    loc = glGetUniformLocation(shp, "mvp");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_TRUE, (const float*)&projection_);

    // Displacement textures: colormap on unit 0, dirt normal on unit 7
    loc = glGetUniformLocation(shp, "tex1");
    if (loc >= 0) glUniform1i(loc, 0);
    // Note: colormap texture is already bound to unit 0 by beginShadowPrePass state
    // But we need to bind it explicitly since shadow pass doesn't set textures
    // The texture handle is set per-batch in txmmgr but we need the terrain colormap
    // Actually: the colormap is the node's texture. We'll bind it from txmmgr. See Task 6.

    loc = glGetUniformLocation(shp, "matNormal2");
    if (loc >= 0) {
        glUniform1i(loc, 7);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[2]);  // dirt normal map
        glActiveTexture(GL_TEXTURE0);
    }

    float tiling[4] = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    loc = glGetUniformLocation(shp, "detailNormalTiling");
    if (loc >= 0) glUniform4fv(loc, 1, tiling);

    // Bind main VBO (pos, color, fog, texcoord at locations 0-3)
    glBindBuffer(GL_ARRAY_BUFFER, indexed_tris_->getVB());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexed_tris_->getIB());
    shadow_terrain_material_->applyVertexDeclaration();

    // Bind extras VBO for worldPos (location 4) and worldNorm (location 5)
    updateBuffer(terrain_extra_vb_, GL_ARRAY_BUFFER,
        extras, extraCount * sizeof(gos_TERRAIN_EXTRA), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, terrain_extra_vb_);

    glEnableVertexAttribArray(4);  // worldPos
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(gos_TERRAIN_EXTRA), (void*)0);
    glEnableVertexAttribArray(5);  // worldNorm
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(gos_TERRAIN_EXTRA), (void*)(3 * sizeof(float)));

    // Draw tessellated patches
    glPatchParameteri(GL_PATCH_VERTICES, 3);
    glDrawElements(GL_PATCHES, numIndices,
        indexed_tris_->getIndexSizeBytes() == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

    // Cleanup
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
    shadow_terrain_material_->endVertexDeclaration();
    shadow_terrain_material_->end();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
```

**Step 3: Add gameos.hpp API declaration**

After `gos_DrawShadowBatch` at line 2274, add:

```cpp
void gos_DrawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount);
```

**Step 4: Add API wrapper function**

Near the other shadow API wrappers (around line 3420), add:

```cpp
void gos_DrawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount) {
    if (g_gos_renderer) g_gos_renderer->drawShadowBatchTessellated(
        vertices, numVerts, indices, numIndices, extras, extraCount);
}
```

**Step 5: Commit**

```
git add GameOS/gameos/gameos_graphics.cpp GameOS/include/gameos.hpp
git commit -m "feat: add drawShadowBatchTessellated for GL_PATCHES shadow depth"
```

---

### Task 6: Update txmmgr.cpp shadow loop to call tessellated draw

**Files:**
- Modify: `mclib/txmmgr.cpp:1048-1071` (shadow pre-pass loop)

**Step 1: Add forward declaration**

At the top of txmmgr.cpp (near other extern declarations), add:

```cpp
extern void gos_DrawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount);
```

**Step 2: Update shadow loop to pass vertices+indices+extras**

Replace line 1067 (`gos_DrawShadowBatch(masterVertexNodes[si].extras, extraCount);`) with:

```cpp
					// Bind this node's colormap texture (unit 0) for displacement sampling
					gos_SetRenderState(gos_State_Texture, masterTextureNodes[masterVertexNodes[si].textureIndex].get_gosTextureHandle());

					gos_DrawShadowBatchTessellated(
						masterVertexNodes[si].vertices, totalVerts,
						indexArray, totalVerts,
						masterVertexNodes[si].extras, extraCount);
```

**Important:** `indexArray` is the identity index array `{0,1,2,3,...}` allocated at txmmgr.cpp:168. It's available in scope. `totalVerts` is already computed at line 1056-1059.

**Step 3: Commit**

```
git add mclib/txmmgr.cpp
git commit -m "feat: shadow loop passes vertices+indices for tessellated shadow draw"
```

---

### Task 7: Fix light direction — gamecam.cpp

**Files:**
- Modify: `code/gamecam.cpp:174-177` (after camera pos set, also set light dir)

**Step 1: Add gos_SetTerrainLightDir call**

After line 176 (`gos_SetTerrainCameraPos(camOrig.x, camOrig.y, camOrig.z);`), add:

```cpp
			// Light direction in MC2 world space -> swizzled GL space (-x, z, y)
			// Same swizzle applied to camera pos in terrain.cpp:787
			gos_SetTerrainLightDir(-lightDirection.x, lightDirection.z, lightDirection.y);
```

**Note:** `lightDirection` is a member of the Camera base class, computed from `lightYaw`/`lightPitch` in `Camera::init()` (camera.cpp:266). GameCamera inherits Camera, so it's available here. The swizzle `(-x, z, y)` matches the terrain camera pos swizzle at terrain.cpp:787.

**Important caveat:** terrain.cpp:782 also calls `gos_SetTerrainLightDir(eye->lightDirection.x, eye->lightDirection.y, eye->lightDirection.z)` WITHOUT the swizzle. That call uses raw MC2 coords. Our gamecam.cpp call uses swizzled GL coords and runs BEFORE `land->render()` (line 184), so terrain.cpp:782 would overwrite it with the UN-swizzled version. We need to either:
- Option A: Remove the terrain.cpp:782 call (let gamecam.cpp be the sole source)
- Option B: Fix terrain.cpp:782 to also apply the swizzle

**Recommendation: Option A** — remove terrain.cpp:782 call. gamecam.cpp is the authoritative camera setup path, and having two places set lightDir is confusing.

**Step 2: Comment out terrain.cpp:782**

```cpp
		// Light direction now set from gamecam.cpp with proper MC2->GL swizzle
		// gos_SetTerrainLightDir(eye->lightDirection.x, eye->lightDirection.y, eye->lightDirection.z);
```

**Step 3: Commit**

```
git add code/gamecam.cpp mclib/terrain.cpp
git commit -m "fix: set terrain light direction from gamecam.cpp with MC2->GL swizzle"
```

---

### Task 8: Fix shadow sun direction — gameosmain.cpp

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp:162-168` (replace hardcoded sun direction)

**Step 1: Use terrain light direction for shadow matrix**

Replace lines 162-168:

```cpp
        {
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            gos_GetTerrainCameraPos(&cx, &cy, &cz);
            // Hardcoded sun direction — known to produce visible shadows in swizzled space
            float lx = 0.3f, ly = 0.7f, lz = 0.2f;
            pp->updateLightMatrix(lx, ly, lz, cx, cy, cz, 1500.0f);
        }
```

With:

```cpp
        {
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            gos_GetTerrainCameraPos(&cx, &cy, &cz);
            // Use game light direction (set from gamecam.cpp with MC2->GL swizzle)
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            gos_GetTerrainLightDir(&lx, &ly, &lz);
            // Fallback to hardcoded direction if light dir not yet set
            float len2 = lx*lx + ly*ly + lz*lz;
            if (len2 < 0.001f) { lx = 0.3f; ly = 0.7f; lz = 0.2f; }
            pp->updateLightMatrix(lx, ly, lz, cx, cy, cz, 1500.0f);
        }
```

**Step 2: Verify gos_GetTerrainLightDir is declared**

Already declared at gameosmain.cpp:18: `extern void gos_GetTerrainLightDir(float* x, float* y, float* z);`. Good.

**Step 3: Commit**

```
git add GameOS/gameos/gameosmain.cpp
git commit -m "fix: use game light direction for shadow matrix instead of hardcoded"
```

---

### Task 9: Build, deploy, and visual test

**Step 1: Build**

Use `/mc2-build` skill to compile.

**Step 2: Deploy**

Use `/mc2-deploy` skill to deploy exe + all shaders (including new .tesc/.tese).

**Step 3: Visual test checklist**

Launch the game and check:
- [ ] No GL errors on startup (check console for `[TESS] FAILED` or `GL ERROR`)
- [ ] Shadows render (RAlt+F3 toggles)
- [ ] No wave-pattern self-shadowing on flat terrain
- [ ] Hills cast shadows in a consistent direction regardless of camera angle
- [ ] Shadow direction matches N·L lighting direction (sun-lit side should be the non-shadowed side)
- [ ] No per-batch seams in shadows
- [ ] Performance acceptable (shadow pass is now tessellated)

**Step 4: If shadow direction looks wrong**

The MC2 coordinate swizzle (-x, z, y) may need adjustment. Debug by printing the light direction:
```cpp
printf("[SHADOW] lightDir: %.3f %.3f %.3f\n", lx, ly, lz);
```
If shadows face wrong direction, try alternate swizzles or negate components.

---

### Task 10: Tune shadow parameters

Only do this after Tasks 1-9 are verified working.

**Files:**
- Modify: `shaders/include/shadow.hglsl` (re-enable features)
- Modify: `GameOS/gameos/gameosmain.cpp` or relevant uniform code (ambient level)

**Step 1: Lower shadow ambient**

Find where shadow ambient is set to 0.85 and lower to ~0.5. Check `shadow.hglsl` and `gos_terrain.frag` for the ambient value.

**Step 2: Re-enable back-face shadow skip**

In `shadow.hglsl`, the `NdotL < 0.1 → return 1.0` guard was disabled. Re-enable it now that shadow geometry matches:

```glsl
float NdotL = dot(normal, lightDir);
if (NdotL < 0.1) return 1.0;  // back-face: skip shadow lookup
```

**Step 3: Re-enable normal-offset shadow lookup**

If there's a normal-offset bias in shadow.hglsl that was disabled, re-enable it.

**Step 4: Consider 5x5 PCF**

Current PCF is 3x3. Wider kernel (5x5) gives softer shadows. Cost: 25 texture lookups vs 9.

**Step 5: Commit**

```
git add shaders/include/shadow.hglsl GameOS/gameos/gameosmain.cpp
git commit -m "tune: lower shadow ambient, re-enable NdotL guard and normal offset"
```
