# G-Buffer MRT + Post-Process Shadow — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a normal buffer via MRT and a post-process shadow pass so overlays, objects, and all non-terrain geometry receive shadows.

**Architecture:** Convert the scene depth renderbuffer to a sampleable texture. Add an RGBA16F normal buffer as a second MRT attachment. After scene rendering, run a fullscreen shadow pass that reconstructs world position from depth, samples the shadow map, and darkens non-terrain pixels via multiplicative blending. Terrain keeps its existing forward shadows.

**Tech Stack:** GLSL 4.20, OpenGL 4.x FBO/MRT, existing `glsl_program` material system, `gosPostProcess` pipeline, existing Poisson PCF shadow sampling.

---

## Task 1: Convert Depth Renderbuffer to Depth Texture

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.h:79` (member declaration)
- Modify: `GameOS/gameos/gos_postprocess.cpp:31-40` (constructor init)
- Modify: `GameOS/gameos/gos_postprocess.cpp:182-207` (createFBOs)
- Modify: `GameOS/gameos/gos_postprocess.cpp:237-261` (destroyFBOs)

**Step 1: Change member from renderbuffer to texture**

In `gos_postprocess.h`, replace line 79:
```cpp
    GLuint sceneDepthRBO_;
```
with:
```cpp
    GLuint sceneDepthTex_;
```

**Step 2: Update constructor initializer**

In `gos_postprocess.cpp` constructor, change `sceneDepthRBO_(0)` to `sceneDepthTex_(0)`.

**Step 3: Replace renderbuffer creation with texture creation in createFBOs()**

Replace lines 198-202 (the depth renderbuffer block):
```cpp
    // Depth/stencil renderbuffer
    glGenRenderbuffers(1, &sceneDepthRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, sceneDepthRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sceneDepthRBO_);
```
with:
```cpp
    // Depth/stencil texture (sampleable for post-process depth reconstruction)
    glGenTextures(1, &sceneDepthTex_);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, sceneDepthTex_, 0);
```

**Step 4: Update destroyFBOs()**

Replace lines 247-249:
```cpp
    if (sceneDepthRBO_) {
        glDeleteRenderbuffers(1, &sceneDepthRBO_);
        sceneDepthRBO_ = 0;
    }
```
with:
```cpp
    if (sceneDepthTex_) {
        glDeleteTextures(1, &sceneDepthTex_);
        sceneDepthTex_ = 0;
    }
```

**Step 5: Build, deploy, test**

Run: `/mc2-build-deploy`
Expected: Compiles clean. No visual change — depth testing works identically with depth textures. Verify scene renders correctly, shadows still work.

**Step 6: Commit**

```
feat(postprocess): convert depth renderbuffer to sampleable depth texture
```

---

## Task 2: Add Normal Buffer MRT Attachment

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.h` (new member + getter)
- Modify: `GameOS/gameos/gos_postprocess.cpp` (createFBOs, destroyFBOs, beginScene)

**Step 1: Add normal texture member and getter to header**

In `gos_postprocess.h`, after `sceneDepthTex_` (line 79), add:
```cpp
    GLuint sceneNormalTex_;   // RGBA16F: rgb=normal, a=shadow flag (1=already shadowed)
```

Add public getter near the shadow getters (after line 30):
```cpp
    GLuint getSceneNormalTexture() const { return sceneNormalTex_; }
    GLuint getSceneDepthTexture() const { return sceneDepthTex_; }
    GLuint getSceneColorTexture() const { return sceneColorTex_; }
```

**Step 2: Initialize in constructor**

Add `sceneNormalTex_(0)` to the constructor initializer list.

**Step 3: Create normal texture in createFBOs()**

After the depth texture creation and before the framebuffer completeness check (before line 204), add:
```cpp
    // Normal buffer: MRT attachment 1 (rgb=world normal encoded, a=shadow skip flag)
    glGenTextures(1, &sceneNormalTex_);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sceneNormalTex_, 0);

    // MRT: draw to both color attachments
    GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBuffers);
```

**Step 4: Destroy normal texture in destroyFBOs()**

After the `sceneDepthTex_` cleanup, add:
```cpp
    if (sceneNormalTex_) {
        glDeleteTextures(1, &sceneNormalTex_);
        sceneNormalTex_ = 0;
    }
```

**Step 5: Set glDrawBuffers in beginScene()**

In `beginScene()`, after `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` (line 301), add:
```cpp
    // Ensure MRT is active (shadow passes may have changed glDrawBuffers)
    GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBuffers);
```

**Step 6: Build, deploy, test**

Run: `/mc2-build-deploy`
Expected: Compiles clean. Normal buffer is attached but nothing writes to location=1 yet. AMD drivers require MRT outputs to be declared in shaders (Task 3), so **expect possible rendering issues until Task 3 is complete**. If scene goes black, that's the AMD MRT declaration requirement — proceed to Task 3.

**Step 7: Commit**

```
feat(postprocess): add RGBA16F normal buffer as MRT attachment 1
```

---

## Task 3: Add MRT Output to All Scene Fragment Shaders

**Files:**
- Modify: `shaders/gos_terrain.frag` (write actual normal data)
- Modify: `shaders/gos_tex_vertex.frag` (write default normal)
- Modify: `shaders/gos_vertex.frag` (write default normal)
- Modify: `shaders/gos_vertex_lighted.frag` (write default normal)
- Modify: `shaders/gos_tex_vertex_lighted.frag` (write default normal)
- Modify: `shaders/gos_text.frag` (write default normal)

**Important:** All shaders that render while sceneFBO_ is bound MUST declare `layout(location=1)` output. AMD drivers may skip draws for shaders that don't declare all MRT outputs.

**Step 1: Add MRT normal output to gos_terrain.frag**

After line 15 (`layout (location=0) out PREC vec4 FragColor;`), add:
```glsl
layout (location=1) out PREC vec4 GBuffer1;  // rgb=normal*0.5+0.5, a=1.0 (terrain: already shadowed)
```

At the end of main(), just before the `gl_FragDepth` line (before line 335), add:
```glsl
    // G-buffer: world-space normal + shadow skip flag
    GBuffer1 = vec4(N * 0.5 + 0.5, 1.0);
```

Also handle the early-out paths: after the debug visualization `FragColor = vec4(1,0,0,1)` (line 160) and the `FragColor = vec4(matWeights...)` (line 218), add:
```glsl
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
```

**Step 2: Add MRT output to gos_tex_vertex.frag**

After line 11 (`layout (location=0) out PREC vec4 FragColor;`), add:
```glsl
layout (location=1) out PREC vec4 GBuffer1;  // rgb=normal, a=0.0 (needs post-process shadow)
```

At the end of main(), after the `FragColor = c;` line (line 66), add:
```glsl
    GBuffer1 = vec4(0.5, 0.5, 1.0, 0.0);
```

**Step 3: Add MRT output to gos_vertex.frag**

After the existing `layout (location=0) out PREC vec4 FragColor;` line, add:
```glsl
layout (location=1) out PREC vec4 GBuffer1;
```

Before the closing brace of main(), add:
```glsl
    GBuffer1 = vec4(0.5, 0.5, 1.0, 0.0);
```

**Step 4: Repeat for gos_vertex_lighted.frag, gos_tex_vertex_lighted.frag, gos_text.frag**

Same pattern as Step 3: add `layout(location=1) out PREC vec4 GBuffer1;` declaration and `GBuffer1 = vec4(0.5, 0.5, 1.0, 0.0);` write in main().

**Step 5: Build, deploy, test**

Run: `/mc2-build-deploy`
Expected: All shaders compile. Scene renders correctly with no visual change (normal buffer is written but not read yet).

**Step 6: Verify normal buffer with shadow debug overlay (optional)**

Temporarily modify `drawShadowDebugOverlay()` to render `sceneNormalTex_` in the debug quad (swap `tex` variable). Terrain should appear as encoded normals (blueish for up-facing, shifting toward red/green on slopes). Non-terrain should be flat blue (0.5, 0.5, 1.0). Revert after testing.

**Step 7: Commit**

```
feat(shaders): write G-buffer normals from all scene fragment shaders
```

---

## Task 4: Add mat4 Inverse Utility Function

**Files:**
- Modify: `GameOS/gameos/utils/vec.h` (function declaration)
- Modify: `GameOS/gameos/utils/vec.cpp` (function implementation)

**Step 1: Declare inverse function**

In `vec.h`, after the `frustumProjMatrix` declaration (line 509), add:
```cpp
mat4 inverseMat4(const mat4& m);
```

**Step 2: Implement inverse function**

In `vec.cpp`, after the `frustumProjMatrix` implementation, add:
```cpp
mat4 inverseMat4(const mat4& m)
{
    // Cofactor expansion for general 4x4 matrix inverse
    const float* s = &m.elem[0][0];
    float inv[16];

    inv[0]  =  s[5]*s[10]*s[15] - s[5]*s[11]*s[14] - s[9]*s[6]*s[15] + s[9]*s[7]*s[14] + s[13]*s[6]*s[11] - s[13]*s[7]*s[10];
    inv[4]  = -s[4]*s[10]*s[15] + s[4]*s[11]*s[14] + s[8]*s[6]*s[15] - s[8]*s[7]*s[14] - s[12]*s[6]*s[11] + s[12]*s[7]*s[10];
    inv[8]  =  s[4]*s[9]*s[15]  - s[4]*s[11]*s[13] - s[8]*s[5]*s[15] + s[8]*s[7]*s[13] + s[12]*s[5]*s[11] - s[12]*s[7]*s[9];
    inv[12] = -s[4]*s[9]*s[14]  + s[4]*s[10]*s[13] + s[8]*s[5]*s[14] - s[8]*s[6]*s[13] - s[12]*s[5]*s[10] + s[12]*s[6]*s[9];
    inv[1]  = -s[1]*s[10]*s[15] + s[1]*s[11]*s[14] + s[9]*s[2]*s[15] - s[9]*s[3]*s[14] - s[13]*s[2]*s[11] + s[13]*s[3]*s[10];
    inv[5]  =  s[0]*s[10]*s[15] - s[0]*s[11]*s[14] - s[8]*s[2]*s[15] + s[8]*s[3]*s[14] + s[12]*s[2]*s[11] - s[12]*s[3]*s[10];
    inv[9]  = -s[0]*s[9]*s[15]  + s[0]*s[11]*s[13] + s[8]*s[1]*s[15] - s[8]*s[3]*s[13] - s[12]*s[1]*s[11] + s[12]*s[3]*s[9];
    inv[13] =  s[0]*s[9]*s[14]  - s[0]*s[10]*s[13] - s[8]*s[1]*s[14] + s[8]*s[2]*s[13] + s[12]*s[1]*s[10] - s[12]*s[2]*s[9];
    inv[2]  =  s[1]*s[6]*s[15]  - s[1]*s[7]*s[14]  - s[5]*s[2]*s[15] + s[5]*s[3]*s[14] + s[13]*s[2]*s[7]  - s[13]*s[3]*s[6];
    inv[6]  = -s[0]*s[6]*s[15]  + s[0]*s[7]*s[14]  + s[4]*s[2]*s[15] - s[4]*s[3]*s[14] - s[12]*s[2]*s[7]  + s[12]*s[3]*s[6];
    inv[10] =  s[0]*s[5]*s[15]  - s[0]*s[7]*s[13]  - s[4]*s[1]*s[15] + s[4]*s[3]*s[13] + s[12]*s[1]*s[7]  - s[12]*s[3]*s[5];
    inv[14] = -s[0]*s[5]*s[14]  + s[0]*s[6]*s[13]  + s[4]*s[1]*s[14] - s[4]*s[2]*s[13] - s[12]*s[1]*s[6]  + s[12]*s[2]*s[5];
    inv[3]  = -s[1]*s[6]*s[11]  + s[1]*s[7]*s[10]  + s[5]*s[2]*s[11] - s[5]*s[3]*s[10] - s[9]*s[2]*s[7]   + s[9]*s[3]*s[6];
    inv[7]  =  s[0]*s[6]*s[11]  - s[0]*s[7]*s[10]  - s[4]*s[2]*s[11] + s[4]*s[3]*s[10] + s[8]*s[2]*s[7]   - s[8]*s[3]*s[6];
    inv[11] = -s[0]*s[5]*s[11]  + s[0]*s[7]*s[9]   + s[4]*s[1]*s[11] - s[4]*s[3]*s[9]  - s[8]*s[1]*s[7]   + s[8]*s[3]*s[5];
    inv[15] =  s[0]*s[5]*s[10]  - s[0]*s[6]*s[9]   - s[4]*s[1]*s[10] + s[4]*s[2]*s[9]  + s[8]*s[1]*s[6]   - s[8]*s[2]*s[5];

    float det = s[0]*inv[0] + s[1]*inv[4] + s[2]*inv[8] + s[3]*inv[12];
    if (fabsf(det) < 1e-12f) {
        // Singular matrix — return identity
        return mat4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    }

    float invDet = 1.0f / det;
    mat4 result;
    for (int i = 0; i < 16; i++)
        ((float*)&result.elem[0][0])[i] = inv[i] * invDet;
    return result;
}
```

**Step 3: Build**

Run: `/mc2-build`
Expected: Compiles clean. No runtime change (function is unused until Task 5).

**Step 4: Commit**

```
feat(math): add mat4 inverse utility function
```

---

## Task 5: Create Post-Process Shadow Shader

**Files:**
- Create: `shaders/shadow_screen.frag`

The vertex shader will reuse `shaders/postprocess.vert` (fullscreen quad passthrough).

**Step 1: Create shadow_screen.frag**

```glsl
//#version 420 (version provided by prefix)

#define PREC highp

in vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

uniform sampler2D sceneDepthTex;       // unit 0: scene depth (GL_DEPTH24_STENCIL8)
uniform sampler2D sceneNormalTex;      // unit 1: normal buffer (rgb=normal, a=shadow flag)
uniform sampler2DShadow shadowMap;     // unit 2: static shadow map (comparison mode)
uniform sampler2DShadow dynamicShadowMap; // unit 3: dynamic shadow map
uniform mat4 inverseViewProj;          // inverse of terrainMVP (NDC → world)
uniform mat4 lightSpaceMatrix;         // world → static shadow light space
uniform mat4 dynamicLightSpaceMatrix;  // world → dynamic shadow light space
uniform vec2 screenSize;               // viewport width, height
uniform int enableShadows;
uniform int enableDynamicShadows;
uniform float shadowSoftness;

// 8-sample Poisson disk (subset of terrain shader's 16-sample disk)
const vec2 poissonDisk[8] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379)
);

float sampleShadowMap(sampler2DShadow smap, vec3 worldPos, mat4 lsMatrix, int numTaps)
{
    vec4 lsPos = lsMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords = projCoords * 0.5 + 0.5;

    // Outside shadow map — fully lit
    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;

    float bias = 0.003;
    float currentDepth = projCoords.z - bias;

    // Per-pixel rotation
    float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    vec2 texelSize = 1.0 / vec2(textureSize(smap, 0));
    float radius = max(shadowSoftness, 0.5);
    float shadow = 0.0;
    int taps = clamp(numTaps, 1, 8);
    for (int i = 0; i < taps; i++) {
        vec2 offset = rot * poissonDisk[i] * radius * texelSize;
        shadow += texture(smap, vec3(projCoords.xy + offset, currentDepth));
    }
    shadow /= float(taps);

    return mix(0.4, 1.0, shadow);
}

void main()
{
    // Read normal buffer alpha: 1.0 = terrain (already shadowed forward), skip
    PREC vec4 normalData = texture(sceneNormalTex, TexCoord);
    if (normalData.a > 0.5) {
        // Terrain pixel — already shadowed in forward pass, output white (no darkening)
        FragColor = vec4(1.0);
        return;
    }

    // Read depth and reconstruct world position
    float depth = texture(sceneDepthTex, TexCoord).r;
    if (depth >= 1.0) {
        // Sky / far plane — no shadow
        FragColor = vec4(1.0);
        return;
    }

    // NDC reconstruction
    vec2 ndc_xy = TexCoord * 2.0 - 1.0;
    float ndc_z = depth * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc_xy, ndc_z, 1.0);
    vec4 worldPos4 = inverseViewProj * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    // Sample static shadow map
    float shadow = 1.0;
    if (enableShadows == 1) {
        shadow = min(shadow, sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8));
    }

    // Sample dynamic shadow map
    if (enableDynamicShadows == 1) {
        float dynShadow = sampleShadowMap(dynamicShadowMap, worldPos, dynamicLightSpaceMatrix, 4);
        shadow = min(shadow, dynShadow);
    }

    // Output shadow factor as grayscale for multiplicative blending
    FragColor = vec4(shadow, shadow, shadow, 1.0);
}
```

**Step 2: Build, deploy**

Run: `/mc2-build-deploy`
Expected: Shader file is deployed. Not compiled yet (loaded in Task 6).

**Step 3: Commit**

```
feat(shaders): add post-process screen-space shadow shader
```

---

## Task 6: Wire Post-Process Shadow Pass into Pipeline

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.h` (new members + method)
- Modify: `GameOS/gameos/gos_postprocess.cpp` (init, destroy, new runScreenShadow method, endScene)
- Modify: `GameOS/gameos/gameos_graphics.cpp` (compute + store inverse MVP)

**Step 1: Add members and method to header**

In `gos_postprocess.h`, add to public section (after `runBloom()` line 25):
```cpp
    void runScreenShadow();
    bool screenShadowEnabled_;
```

Add to private section (after `bloomBlurProg_` line 97):
```cpp
    // Post-process screen shadow
    glsl_program* screenShadowProg_;
```

Also add storage for the inverse VP matrix, public:
```cpp
    void setInverseViewProj(const float* m) { memcpy(inverseViewProj_, m, 16 * sizeof(float)); }
    const float* getInverseViewProj() const { return inverseViewProj_; }
```

And private:
```cpp
    float inverseViewProj_[16];
```

**Step 2: Initialize in constructor**

Add to constructor initializer list:
```cpp
    , screenShadowProg_(nullptr)
    , screenShadowEnabled_(true)
```

Add to constructor body:
```cpp
    memset(inverseViewProj_, 0, sizeof(inverseViewProj_));
```

**Step 3: Load shader in init()**

After the `shadowDebugProg_` loading block (after line 121), add:
```cpp
    screenShadowProg_ = glsl_program::makeProgram("shadow_screen",
        "shaders/postprocess.vert", "shaders/shadow_screen.frag", kShaderPrefix);
    if (!screenShadowProg_ || !screenShadowProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shadow_screen shader\n");
```

**Step 4: Clean up in destroy()**

After the `shadowDebugProg_` cleanup, add:
```cpp
    if (screenShadowProg_) {
        glsl_program::deleteProgram("shadow_screen");
        screenShadowProg_ = nullptr;
    }
```

**Step 5: Implement runScreenShadow()**

Add this method to `gos_postprocess.cpp`, after `runBloom()`:

```cpp
void gosPostProcess::runScreenShadow()
{
    ZoneScopedN("Render.ScreenShadow");
    TracyGpuZone("Render.ScreenShadow");

    if (!screenShadowEnabled_) return;
    if (!screenShadowProg_ || !screenShadowProg_->is_valid()) return;
    if (!shadowsEnabled_) return;

    // Render to sceneFBO_ color-only (no normal write) with multiplicative blending
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
    glViewport(0, 0, width_, height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Multiplicative blending: dst * src (shadow darkening)
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);

    // Set uniforms BEFORE apply()
    screenShadowProg_->setInt("sceneDepthTex", 0);
    screenShadowProg_->setInt("sceneNormalTex", 1);
    screenShadowProg_->setInt("shadowMap", 2);
    screenShadowProg_->setInt("dynamicShadowMap", 3);
    screenShadowProg_->setInt("enableShadows", shadowsEnabled_ ? 1 : 0);
    screenShadowProg_->setInt("enableDynamicShadows", (dynShadowDepthTex_ != 0) ? 1 : 0);
    screenShadowProg_->setFloat("shadowSoftness", 2.5f);
    float screenSz[2] = { (float)width_, (float)height_ };
    screenShadowProg_->setFloat2("screenSize", screenSz);
    screenShadowProg_->apply();

    // Upload matrices via direct GL (after apply binds the program)
    GLint loc;
    loc = glGetUniformLocation(screenShadowProg_->shp_, "inverseViewProj");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, inverseViewProj_);
    loc = glGetUniformLocation(screenShadowProg_->shp_, "lightSpaceMatrix");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, staticLightSpaceMatrix_);
    loc = glGetUniformLocation(screenShadowProg_->shp_, "dynamicLightSpaceMatrix");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, dynamicLightSpaceMatrix_);

    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);

    // Shadow maps: need to temporarily disable comparison mode for the
    // screen shadow shader which uses sampler2DShadow (comparison stays on)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, dynShadowDepthTex_);

    // Draw fullscreen quad
    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore state
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}
```

**Step 6: Call runScreenShadow() in endScene()**

In `endScene()`, insert the shadow pass call BEFORE `runBloom()` (before line 367):
```cpp
    // Post-process shadow pass (darkens non-terrain geometry)
    runScreenShadow();
```

**Step 7: Compute and upload inverse VP in gameos_graphics.cpp**

Find where `terrainMVP` is uploaded (around line 2402-2405). After it, add the inverse computation:

```cpp
    // Compute inverse VP for post-process depth reconstruction
    if (terrain_mvp_valid_) {
        mat4 invVP = inverseMat4(terrain_mvp_);
        gosPostProcess* pp = getGosPostProcess();
        if (pp) pp->setInverseViewProj((const float*)&invVP);
    }
```

Add `#include "utils/vec.h"` if not already included (it likely is).

**Step 8: Build, deploy, test**

Run: `/mc2-build-deploy`
Expected: Compiles. Overlays and non-terrain geometry should now receive shadows. Terrain shadows should be unchanged (skip flag in normal alpha). Check:
- Roads/cement have shadows
- Terrain shadows look the same as before
- No double-darkening on terrain
- Tracy profiler shows ScreenShadow pass timing

**Step 9: Commit**

```
feat(postprocess): wire post-process shadow pass with depth reconstruction
```

---

## Task 7: Add Debug Toggle for Screen Shadows

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (keyboard handler)

**Step 1: Add hotkey for screen shadow toggle**

Find the existing debug hotkey block (RAlt+F3 for shadows). Add a new hotkey, e.g., RAlt+F4 for screen shadows:

```cpp
// RAlt+F4: toggle post-process screen shadows
if (key == KEY_F4 && (modifiers & RALT)) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) {
        pp->screenShadowEnabled_ = !pp->screenShadowEnabled_;
        printf("[DEBUG] Screen shadows: %s\n", pp->screenShadowEnabled_ ? "ON" : "OFF");
    }
}
```

**Step 2: Build, deploy, test**

Run: `/mc2-build-deploy`
Expected: RAlt+F4 toggles screen shadows on/off. Console prints state.

**Step 3: Commit**

```
feat(debug): add RAlt+F4 hotkey for post-process screen shadow toggle
```

---

## Execution Notes

### Build/deploy workflow
All tasks use `/mc2-build-deploy` (build in worktree, deploy exe + shaders to game directory).

### Key constraints (from CLAUDE.md)
- Build: ALWAYS `--config RelWithDebInfo` (Release crashes)
- Deploy: ALWAYS `cp -f` per file + `diff -q`, NEVER `cp -r`
- Shader `#version`: NEVER in shader files — prefix `"#version 420\n"` via `makeProgram()`
- Uniform API: `setFloat`/`setInt` BEFORE `apply()`, `glUniform*` AFTER `apply()`
- AMD: MRT outputs MUST be declared in all fragment shaders

### Testing with Tracy
After Task 6, connect Tracy Profiler to verify:
- `Render.ScreenShadow` zone appears and is < 1ms
- Overall frame time doesn't regress significantly
- No GPU stalls from depth texture read-back

### Coordinate space reminder
- `terrainMVP` maps MC2 world space (x=east, y=north, z=elev) → clip space
- `inverseViewProj` maps NDC → MC2 world space
- `lightSpaceMatrix` maps MC2 world space → shadow map UV space
- All consistent — no coordinate swizzling needed in the shadow screen shader
