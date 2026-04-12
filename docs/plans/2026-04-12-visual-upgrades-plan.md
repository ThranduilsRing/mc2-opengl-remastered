# Visual Upgrades Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add GPU grass, god rays, depth-aware shorelines, and upscale remaining textures to the MC2 OpenGL renderer.

**Architecture:** Four independent features layered onto the existing tessellation + post-process pipeline. Grass uses a geometry shader in the forward pass. God rays and shorelines are fullscreen post-process passes. Texture upscaling is an offline Python pipeline.

**Tech Stack:** OpenGL 4.2, GLSL 420, C++, Python (Pillow + ESRGAN)

**Design Doc:** `docs/plans/2026-04-12-visual-upgrades-design.md`

---

### Task 1: GBuffer1 Foundation — Proper Normal Buffer Writes

Currently `gos_terrain.frag` does NOT write to `layout(location=1)` despite MRT being enabled. The normal buffer alpha classification (`> 0.5` = terrain) in `shadow_screen.frag` relies on undefined OpenGL behavior. Fix this properly as foundation for shoreline water flags.

**Files:**
- Modify: `shaders/gos_terrain.frag` (add GBuffer1 output)
- Modify: `shaders/gos_tex_vertex.frag` (add GBuffer1 output for water flag)
- Modify: `GameOS/gameos/gos_postprocess.cpp` (clear normal buffer each frame)
- Modify: `GameOS/gameos/gameos_graphics.cpp` (enable MRT for water overlays too)

**Step 1: Add GBuffer1 output to gos_terrain.frag**

After line 15 (`layout (location=0) out PREC vec4 FragColor;`), add:
```glsl
layout (location=1) out PREC vec4 GBuffer1;
```

At line 330, before `FragColor = c;`, add normal buffer write:
```glsl
    // GBuffer1: world normal (RGB) + material flag (A)
    // alpha: 1.0 = terrain, 0.25 = water, 0.0 = non-terrain (default from clear)
    PREC float waterFlag = smoothstep(0.35, 0.45, rgb2hsv(texColor.rgb).x);
    PREC float materialAlpha = mix(1.0, 0.25, waterFlag);
    GBuffer1 = vec4(N * 0.5 + 0.5, materialAlpha);
```

Note: `N` is the final world-space normal computed earlier in the shader. `texColor` is the colormap sample. `waterFlag` reuses the existing HSV water detection logic (same hue range 0.35-0.45 as `getColorWeights()`).

**Step 2: Add GBuffer1 output to gos_tex_vertex.frag**

After line 6 (`layout(location = 0) out PREC vec4 FragColor;`), add:
```glsl
layout (location=1) out PREC vec4 GBuffer1;
```

At end of `main()`, before the closing brace, add:
```glsl
    // Flag water overlays in normal buffer for shoreline detection
    if (isWater > 0) {
        GBuffer1 = vec4(0.5, 0.5, 1.0, 0.25);  // up-facing normal, water flag
    } else {
        GBuffer1 = vec4(0.5, 0.5, 1.0, 0.0);  // non-terrain overlay
    }
```

**Step 3: Enable MRT for water overlay draws**

In `gameos_graphics.cpp`, the water overlay path is at ~line 2629-2640 (inside the `else` branch after terrain draw). Water overlays use `gos_tex_vertex` shader. Currently MRT is disabled for non-terrain draws (AMD workaround). For water overlays specifically, we need MRT:

After line 2632 (`if (curStates_[gos_State_Water]) {`), add MRT enable:
```cpp
                if (curStates_[gos_State_Water]) {
                    // Enable MRT so water overlay can write water flag to GBuffer1
                    gosPostProcess* pp_water = getGosPostProcess();
                    if (pp_water) pp_water->enableMRT();
```
And after the water draw completes (after `apply()`/draw), disable MRT:
```cpp
                    if (pp_water) pp_water->disableMRT();
```

**Step 4: Clear normal buffer at frame start**

In `gos_postprocess.cpp`, modify `beginScene()` (line 443) to clear the normal buffer:
```cpp
void gosPostProcess::beginScene()
{
    if (!initialized_)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // Clear normal buffer to (0,0,0,0): alpha=0 means "no terrain/no data"
    if (sceneNormalTex_) {
        GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBuffers);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // Restore to single draw buffer (MRT enabled selectively during draws)
        GLenum singleBuf = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &singleBuf);
    }
    glViewport(0, 0, width_, height_);
}
```

Note: The main scene clear (color + depth) happens in `gameosmain.cpp:168` AFTER `beginScene()`, which will overwrite ATTACHMENT0 with the correct clear color. ATTACHMENT1 keeps the (0,0,0,0) clear from our new code.

**Step 5: Build, deploy, test**

Run: `/mc2-build-deploy`

Verify:
- Terrain renders normally (no color corruption)
- Shadow screen pass still classifies terrain correctly (terrain alpha = 1.0, objects = 0.0)
- RAlt+F2 shadow debug shows correct terrain classification (dark red = terrain)
- Water areas should now have alpha = 0.25 in normal buffer

**Step 6: Commit**

```
feat: add proper GBuffer1 normal buffer writes to terrain and water shaders

Terrain shader now writes world-space normals + material alpha flag to
GL_COLOR_ATTACHMENT1. Water pixels get alpha=0.25 for shoreline detection.
Fixes previously undefined MRT behavior where terrain classification
relied on driver-specific output for unwritten attachment.
```

---

### Task 2: GPU Grass — Geometry Shader

**Files:**
- Create: `shaders/gos_grass.geom` (geometry shader)
- Create: `shaders/gos_grass.frag` (fragment shader)
- Modify: `GameOS/gameos/gos_postprocess.h` (add grassProg_ member, grassEnabled_ toggle)
- Modify: `GameOS/gameos/gos_postprocess.cpp` (compile grass program)
- Modify: `GameOS/gameos/gameos_graphics.cpp` (add grass draw call after terrain)
- Modify: `GameOS/gameos/gameosmain.cpp` (add RAlt+F4 toggle)

**Step 1: Create gos_grass.geom**

```glsl
//#version 420 (version provided by prefix)

#define PREC highp

layout(triangles) in;
layout(triangle_strip, max_vertices = 12) out;

// From TES (per-vertex)
in vec4 Color[];
in float FogValue[];
in vec2 Texcoord[];
in float TerrainType[];
in vec3 WorldNorm[];
in vec3 WorldPos[];
in float UndisplacedDepth[];

// To fragment shader
out PREC vec2 GrassUV;
out PREC vec3 GrassWorldPos;
out PREC vec3 GrassBaseColor;
out PREC float GrassAlpha;

uniform sampler2D tex1;          // colormap (unit 0)
uniform mat4 terrainMVP;         // axisSwap * worldToClip
uniform vec4 terrainViewport;    // (vmx, vmy, vax, vay)
uniform mat4 mvp;                // projection_ : screen pixels -> NDC
uniform vec4 cameraPos;
uniform float time;

// HSV helper (matches gos_terrain.frag)
PREC vec3 gs_rgb2hsv(PREC vec3 c) {
    PREC vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    PREC vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    PREC vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    PREC float d = q.x - min(q.w, q.y);
    PREC float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// Project world position to clip space (same as TES)
vec4 projectWorldPos(vec3 wp) {
    vec4 clip = terrainMVP * vec4(wp, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    return vec4(ndc.xyz * absW, absW);
}

void emitGrassBlade(vec3 basePos, vec3 terrainNorm, vec3 color, float dist) {
    // MC2 world: x=east, y=north, z=elevation. "Up" is +Z.
    vec3 up = vec3(0.0, 0.0, 1.0);

    // Billboard facing: rotate around Z toward camera
    vec3 toCamera = normalize(cameraPos.xyz - basePos);
    vec3 right = normalize(cross(up, toCamera));

    // Randomize using position hash
    float hash = fract(sin(dot(basePos.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float bladeHeight = mix(15.0, 25.0, hash);
    float bladeWidth = mix(8.0, 14.0, fract(hash * 7.31));

    // Slight random lean
    float lean = (hash - 0.5) * 0.15;
    vec3 leanOffset = right * lean * bladeHeight;

    // Wind: sinusoidal displacement of top vertices
    float wind = sin(time * 1.5 + basePos.x * 0.05 + basePos.y * 0.07) * 4.0;
    vec3 windOffset = right * wind;

    // Distance fade (alpha)
    float fadeStart = 3000.0;
    float fadeEnd = 5000.0;
    float fadeFactor = 1.0 - smoothstep(fadeStart, fadeEnd, dist);

    vec3 halfRight = right * bladeWidth * 0.5;
    vec3 top = up * bladeHeight + leanOffset + windOffset;

    // Emit 4 vertices as triangle strip: BL, BR, TL, TR
    GrassBaseColor = color;
    GrassAlpha = fadeFactor;

    GrassWorldPos = basePos - halfRight;
    GrassUV = vec2(0.0, 0.0);
    gl_Position = projectWorldPos(GrassWorldPos);
    EmitVertex();

    GrassWorldPos = basePos + halfRight;
    GrassUV = vec2(1.0, 0.0);
    gl_Position = projectWorldPos(GrassWorldPos);
    EmitVertex();

    GrassWorldPos = basePos - halfRight + top;
    GrassUV = vec2(0.0, 1.0);
    gl_Position = projectWorldPos(GrassWorldPos);
    EmitVertex();

    GrassWorldPos = basePos + halfRight + top;
    GrassUV = vec2(1.0, 1.0);
    gl_Position = projectWorldPos(GrassWorldPos);
    EmitVertex();

    EndPrimitive();
}

void main() {
    // Process each vertex of the input triangle
    for (int i = 0; i < 3; i++) {
        vec3 wp = WorldPos[i];
        float dist = distance(wp, cameraPos.xyz);

        // Skip beyond fade distance
        if (dist > 5000.0) continue;

        // Thin out grass at distance by skipping based on position hash
        float hash = fract(sin(dot(wp.xy, vec2(127.1, 311.7))) * 43758.5453);
        float densityThreshold = smoothstep(3000.0, 5000.0, dist);
        if (hash < densityThreshold) continue;

        // Sample colormap and classify
        vec3 colSample = texture(tex1, Texcoord[i]).rgb;
        vec3 hsv = gs_rgb2hsv(colSample);

        // Grass detection: hue 0.14-0.17, saturation > 0.15
        float grassWeight = smoothstep(0.14, 0.17, hsv.x) * smoothstep(0.15, 0.28, hsv.y);

        // Skip water
        float isWater = smoothstep(0.35, 0.45, hsv.x);
        grassWeight *= (1.0 - isWater);

        if (grassWeight < 0.3) continue;

        emitGrassBlade(wp, WorldNorm[i], colSample, dist);
    }
}
```

**Step 2: Create gos_grass.frag**

```glsl
//#version 420 (version provided by prefix)

#define PREC highp

#include <include/shadow.hglsl>

in PREC vec2 GrassUV;
in PREC vec3 GrassWorldPos;
in PREC vec3 GrassBaseColor;
in PREC float GrassAlpha;

layout (location=0) out PREC vec4 FragColor;
layout (location=1) out PREC vec4 GBuffer1;

uniform PREC vec4 terrainLightDir;

void main()
{
    // Procedural grass blade shape: narrow triangle tapering to tip
    float bladeShape = 1.0 - abs(GrassUV.x * 2.0 - 1.0);  // 0 at edges, 1 at center
    float taper = 1.0 - GrassUV.y;  // 1 at base, 0 at tip
    float bladeMask = bladeShape * smoothstep(0.0, 0.3, taper + bladeShape * 0.5);

    // Discard transparent pixels
    if (bladeMask < 0.3 || GrassAlpha < 0.01) discard;

    // Color: tint green from terrain base color
    vec3 grassGreen = vec3(0.25, 0.45, 0.15);
    vec3 color = mix(grassGreen, GrassBaseColor * vec3(0.8, 1.1, 0.7), 0.4);

    // Darken base, lighten tips
    float heightGradient = mix(0.6, 1.0, GrassUV.y);
    color *= heightGradient;

    // Simple diffuse lighting
    vec3 grassNormal = vec3(0.0, 0.0, 1.0);  // always face up in MC2 world space
    float diffuse = max(dot(grassNormal, normalize(terrainLightDir.xyz)), 0.0);
    color *= mix(0.5, 1.1, diffuse);

    // Shadow sampling (same as terrain)
    float staticShadow = calcShadow(GrassWorldPos, grassNormal, terrainLightDir.xyz, 4);
    float dynShadow = calcDynamicShadow(GrassWorldPos, grassNormal, terrainLightDir.xyz, 4);
    color *= staticShadow * dynShadow;

    FragColor = vec4(color, bladeMask * GrassAlpha);

    // GBuffer1: up-facing normal, terrain-class alpha
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
}
```

**Step 3: Add grassProg_ to gos_postprocess.h**

In the public section (after `bool ssaoEnabled_;` ~line 80), add:
```cpp
    // Grass rendering
    bool grassEnabled_;
    glsl_program* getGrassProgram() const { return grassProg_; }
```

In the private section (after `GLuint ssaoNoiseTex_;` ~line 158), add:
```cpp
    // Grass shader program
    glsl_program* grassProg_;
```

**Step 4: Compile grass program in gos_postprocess.cpp**

In the constructor, initialize:
```cpp
    , grassEnabled_(true)
    , grassProg_(nullptr)
```

In `init()` (after the ssaoApplyProg_ compilation, ~line 159), add:
```cpp
    // Grass geometry shader program — full tessellation pipeline + geometry expansion
    grassProg_ = glsl_program::makeProgram2("grass",
        "shaders/gos_terrain.vert",
        "shaders/gos_terrain.tesc",
        "shaders/gos_terrain.tese",
        "shaders/gos_grass.geom",
        "shaders/gos_grass.frag",
        0, nullptr, kShaderPrefix);
    if (!grassProg_ || !grassProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile grass shader\n");
```

In `destroy()`, add cleanup:
```cpp
    delete grassProg_; grassProg_ = nullptr;
```

**Step 5: Add grass draw call in gameos_graphics.cpp**

Create a new function `drawGrassPass()` somewhere near `terrainDrawIndexedPatches()` (~line 2370):

```cpp
void gosRenderer::drawGrassPass(gosMesh* mesh)
{
    ZoneScopedN("Render.Grass");
    TracyGpuZone("Render.Grass");

    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->grassEnabled_) return;

    glsl_program* grassProg = pp->getGrassProgram();
    if (!grassProg || !grassProg->is_valid()) return;

    // Enable alpha blending for grass fade
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);  // grass visible from both sides

    // Use grass program directly (bypass material system)
    glUseProgram(grassProg->program_);

    // Upload all terrain uniforms (same as terrainDrawIndexedPatches)
    auto& tl = terrainLocs_;
    // Re-cache uniform locations for grass program (different program!)
    GLuint gp = grassProg->program_;
    GLint gTerrainMVP = glGetUniformLocation(gp, "terrainMVP");
    GLint gMvp = glGetUniformLocation(gp, "mvp");
    GLint gTessLevel = glGetUniformLocation(gp, "tessLevel");
    GLint gTessDistRange = glGetUniformLocation(gp, "tessDistanceRange");
    GLint gTessDisplace = glGetUniformLocation(gp, "tessDisplace");
    GLint gCameraPos = glGetUniformLocation(gp, "cameraPos");
    GLint gTerrainViewport = glGetUniformLocation(gp, "terrainViewport");
    GLint gLightDir = glGetUniformLocation(gp, "terrainLightDir");
    GLint gDetailTiling = glGetUniformLocation(gp, "detailNormalTiling");
    GLint gTime = glGetUniformLocation(gp, "time");

    // Upload matrices (GL_FALSE = column-major, same as terrain)
    if (gTerrainMVP >= 0)
        glUniformMatrix4fv(gTerrainMVP, 1, GL_FALSE, terrain_mvp_);
    if (gMvp >= 0)
        glUniformMatrix4fv(gMvp, 1, GL_TRUE, (const float*)&projection_);

    // Upload tessellation params
    if (gTessLevel >= 0)
        glUniform4fv(gTessLevel, 1, terrain_tess_level_);
    if (gTessDistRange >= 0)
        glUniform4fv(gTessDistRange, 1, terrain_tess_dist_range_);
    if (gTessDisplace >= 0)
        glUniform4fv(gTessDisplace, 1, terrain_tess_displace_);
    if (gCameraPos >= 0)
        glUniform4fv(gCameraPos, 1, terrain_camera_pos_);
    if (gTerrainViewport >= 0)
        glUniform4fv(gTerrainViewport, 1, terrain_viewport_);
    if (gLightDir >= 0)
        glUniform4fv(gLightDir, 1, terrain_light_dir_);
    if (gDetailTiling >= 0)
        glUniform4fv(gDetailTiling, 1, terrain_detail_tiling_);

    // Time for wind animation
    static uint64_t grass_start = timing::get_wall_time_ms();
    float elapsed = (float)(timing::get_wall_time_ms() - grass_start) / 1000.0f;
    if (gTime >= 0)
        glUniform1f(gTime, elapsed);

    // Bind colormap (unit 0) — should still be bound from terrain draw
    GLint gTex1 = glGetUniformLocation(gp, "tex1");
    if (gTex1 >= 0) glUniform1i(gTex1, 0);

    // Bind material normal maps (units 5-8) for TES displacement
    for (int i = 0; i < 4; i++) {
        char uname[32];
        snprintf(uname, sizeof(uname), "matNormal%d", i);
        GLint loc = glGetUniformLocation(gp, uname);
        if (loc >= 0 && terrain_mat_normal_[i] != 0) {
            glUniform1i(loc, 5 + i);
            glActiveTexture(GL_TEXTURE5 + i);
            glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
        }
    }

    // Bind shadow maps
    gosPostProcess* ppShadow = getGosPostProcess();
    if (ppShadow) {
        GLint gShadowMap = glGetUniformLocation(gp, "shadowMap");
        GLint gEnableShadows = glGetUniformLocation(gp, "enableShadows");
        GLint gShadowSoftness = glGetUniformLocation(gp, "shadowSoftness");
        GLint gLightSpace = glGetUniformLocation(gp, "lightSpaceMatrix");
        if (gShadowMap >= 0) {
            glUniform1i(gShadowMap, 9);
            glActiveTexture(GL_TEXTURE9);
            glBindTexture(GL_TEXTURE_2D, ppShadow->getShadowTexture());
        }
        if (gEnableShadows >= 0)
            glUniform1i(gEnableShadows, ppShadow->shadowsEnabled_ ? 1 : 0);
        if (gShadowSoftness >= 0)
            glUniform1f(gShadowSoftness, 2.0f);
        if (gLightSpace >= 0)
            glUniformMatrix4fv(gLightSpace, 1, GL_FALSE, ppShadow->getLightSpaceMatrix());

        // Dynamic shadows
        GLint gDynShadowMap = glGetUniformLocation(gp, "dynamicShadowMap");
        GLint gEnableDynShadows = glGetUniformLocation(gp, "enableDynamicShadows");
        GLint gDynLightSpace = glGetUniformLocation(gp, "dynamicLightSpaceMatrix");
        if (gDynShadowMap >= 0) {
            glUniform1i(gDynShadowMap, 10);
            glActiveTexture(GL_TEXTURE10);
            glBindTexture(GL_TEXTURE_2D, ppShadow->getDynamicShadowTexture());
        }
        if (gEnableDynShadows >= 0)
            glUniform1i(gEnableDynShadows, 1);
        if (gDynLightSpace >= 0)
            glUniformMatrix4fv(gDynLightSpace, 1, GL_FALSE, ppShadow->getDynamicLightSpaceMatrix());
    }

    glActiveTexture(GL_TEXTURE0);

    // Draw with same geometry as terrain (VAO should still be bound)
    int ni = mesh->getNumIndices();
    glPatchParameteri(GL_PATCH_VERTICES, 3);
    glDrawElements(GL_PATCHES, ni,
        mesh->getIndexSizeBytes() == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

    // Restore state
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
}
```

**Step 6: Call drawGrassPass after terrain draw**

In `gameos_graphics.cpp`, after `terrainDrawIndexedPatches(tmat, indexed_tris_);` (line 2613), but BEFORE `disableMRT()` (line 2614), add:

```cpp
        terrainDrawIndexedPatches(tmat, indexed_tris_);
        // GPU grass pass: same VAO, geometry shader emits grass quads
        drawGrassPass(indexed_tris_);
        if (pp_mrt) pp_mrt->disableMRT();
```

Note: Grass draws while MRT is still active, so grass writes GBuffer1 correctly.

**Step 7: Declare drawGrassPass in the header**

In `gameos_graphics.cpp` or the relevant header, add declaration:
```cpp
void drawGrassPass(gosMesh* mesh);
```

This should be a method on `gosRenderer` alongside `terrainDrawIndexedPatches`.

**Step 8: Add RAlt+F4 toggle**

In `gameosmain.cpp`, after the F3 case (~line 84), add:
```cpp
        case SDLK_F4:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->grassEnabled_ = !pp->grassEnabled_;
                    fprintf(stderr, "Grass: %s\n", pp->grassEnabled_ ? "ON" : "OFF");
                }
            }
            break;
```

**Step 9: Build, deploy, test**

Run: `/mc2-build-deploy`

Verify:
- Green grass quads appear on grassy terrain areas
- Grass animates with wind
- Grass fades at distance
- RAlt+F4 toggles grass on/off
- Grass receives terrain shadows
- No rendering artifacts or GL errors

**Step 10: Commit**

```
feat: add GPU grass via geometry shader

Tessellation pipeline + geometry shader emits axis-aligned billboard grass
quads on terrain classified as grass via HSV colormap sampling. Features
wind animation, distance fadeout, shadow sampling, and procedural blade
alpha. Toggle with RAlt+F4.
```

---

### Task 3: God Rays — Post-Process Radial Light Scattering

**Files:**
- Create: `shaders/godray.frag` (occlusion + radial blur)
- Modify: `GameOS/gameos/gos_postprocess.h` (add godrayProg_, FBO, toggle)
- Modify: `GameOS/gameos/gos_postprocess.cpp` (compile shader, create FBO, add pass)

**Step 1: Create godray.frag**

```glsl
//#version 420 (version provided by prefix)

#define PREC highp

#include <include/noise.hglsl>

in vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

uniform sampler2D sceneDepthTex;
uniform sampler2D sceneColorTex;
uniform vec2 sunScreenPos;        // sun position in UV space [0,1]
uniform float time;
uniform int godrayEnabled;

// Tunable parameters
const float density = 0.9;
const float weight = 0.5;
const float decay = 0.97;
const float exposure = 0.35;
const int numSamples = 32;

void main()
{
    if (godrayEnabled == 0) {
        FragColor = vec4(0.0);
        return;
    }

    // Direction from pixel toward sun
    vec2 deltaTexCoord = (TexCoord - sunScreenPos) * density / float(numSamples);

    vec2 sampleCoord = TexCoord;
    float illuminationDecay = 1.0;
    vec3 godrayColor = vec3(0.0);

    for (int i = 0; i < numSamples; i++) {
        sampleCoord -= deltaTexCoord;

        // Clamp to valid UV range
        if (sampleCoord.x < 0.0 || sampleCoord.x > 1.0 ||
            sampleCoord.y < 0.0 || sampleCoord.y > 1.0) break;

        float depth = texture(sceneDepthTex, sampleCoord).r;

        // Sky pixels (depth == 1.0 at far plane) are light sources
        float isSky = step(0.999, depth);

        // Cloud shadow modulation: FBM noise creates gaps in the rays
        // Scale world-space-ish coords from screen pos for noise continuity
        vec2 noiseCoord = sampleCoord * 8.0 + vec2(time * 0.02, time * 0.01);
        float cloudDensity = fbm(noiseCoord, 3) * 0.5 + 0.5;  // [0, 1]
        float cloudGap = smoothstep(0.3, 0.7, cloudDensity);   // thin clouds = bright

        float sampleValue = isSky * cloudGap;

        // Accumulate with exponential decay
        sampleValue *= illuminationDecay * weight;
        godrayColor += vec3(sampleValue);
        illuminationDecay *= decay;
    }

    // Warm sun tint
    vec3 sunTint = vec3(1.0, 0.9, 0.7);
    godrayColor *= sunTint * exposure;

    // Fade rays when sun is near screen edge (avoid hard cutoff)
    float edgeFade = 1.0 - smoothstep(0.4, 0.8, length(sunScreenPos - vec2(0.5)));
    godrayColor *= max(edgeFade, 0.0);

    FragColor = vec4(godrayColor, 1.0);
}
```

**Step 2: Add god ray members to gos_postprocess.h**

In the public section, add:
```cpp
    // God rays
    bool godrayEnabled_;
    void runGodRays();
    void setSunScreenPos(float x, float y) { sunScreenPos_[0] = x; sunScreenPos_[1] = y; }
```

In the private section, add:
```cpp
    // God ray
    glsl_program* godrayProg_;
    GLuint godrayFBO_;
    GLuint godrayColorTex_;  // half-res
    float sunScreenPos_[2];
```

**Step 3: Compile god ray program and create FBO**

In the constructor, initialize:
```cpp
    , godrayEnabled_(true)
    , godrayProg_(nullptr)
    , godrayFBO_(0)
    , godrayColorTex_(0)
```
Initialize `sunScreenPos_` to `{0.5f, 0.5f}`.

In `init()`, after grass program compilation, add:
```cpp
    godrayProg_ = glsl_program::makeProgram("godray",
        "shaders/postprocess.vert", "shaders/godray.frag", kShaderPrefix);
    if (!godrayProg_ || !godrayProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile godray shader\n");
```

In `createFBOs()`, after bloom FBO creation, add half-res god ray FBO:
```cpp
    // God ray FBO (half resolution)
    int ghw = w / 2, ghh = h / 2;
    if (ghw < 1) ghw = 1;
    if (ghh < 1) ghh = 1;

    glGenTextures(1, &godrayColorTex_);
    glBindTexture(GL_TEXTURE_2D, godrayColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, ghw, ghh, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &godrayFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, godrayFBO_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, godrayColorTex_, 0);
```

In `destroyFBOs()`, add cleanup:
```cpp
    if (godrayColorTex_) { glDeleteTextures(1, &godrayColorTex_); godrayColorTex_ = 0; }
    if (godrayFBO_) { glDeleteFramebuffers(1, &godrayFBO_); godrayFBO_ = 0; }
```

In `destroy()`, add:
```cpp
    delete godrayProg_; godrayProg_ = nullptr;
```

**Step 4: Implement runGodRays()**

```cpp
void gosPostProcess::runGodRays()
{
    ZoneScopedN("Render.GodRays");
    TracyGpuZone("Render.GodRays");

    if (!godrayEnabled_ || !godrayProg_ || !godrayProg_->is_valid()) return;

    int hw = width_ / 2, hh = height_ / 2;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;

    // Pass 1: Render god rays into half-res FBO
    glBindFramebuffer(GL_FRAMEBUFFER, godrayFBO_);
    glViewport(0, 0, hw, hh);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    godrayProg_->setInt("sceneDepthTex", 0);
    godrayProg_->setInt("sceneColorTex", 1);
    godrayProg_->setFloat2("sunScreenPos", sunScreenPos_);
    godrayProg_->setInt("godrayEnabled", 1);

    static uint64_t gr_start = timing::get_wall_time_ms();
    float elapsed = (float)(timing::get_wall_time_ms() - gr_start) / 1000.0f;
    godrayProg_->setFloat("time", elapsed);
    godrayProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Pass 2: Additive composite onto scene
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
    glViewport(0, 0, width_, height_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);  // Additive

    // Reuse composite-style fullscreen quad to blit godray texture
    // Simple pass-through: just sample godrayColorTex and output it
    // We can reuse bloomThresholdProg with threshold=0 as a pass-through,
    // but cleaner to just use the godray shader in composite mode.
    // For simplicity, use glBlitFramebuffer or a minimal pass-through shader.
    // Actually, we'll create a simple blit by binding godray tex to unit 0
    // and using bloom_threshold with threshold=-1 (passes everything).
    bloomThresholdProg_->setInt("sceneTex", 0);
    bloomThresholdProg_->setFloat("threshold", -1.0f);  // pass everything
    bloomThresholdProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, godrayColorTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}
```

**Step 5: Compute sun screen position and wire into endScene()**

In `renderSkybox()` (which receives sun direction), compute and store the sun screen position after projecting through VP matrix:

```cpp
// After setting sunDir uniform in renderSkybox(), compute sun screen pos:
// Project a far-away point along sun direction from world origin
float sunWorld[4] = { sunDirX * 100000.0f, sunDirY * 100000.0f, sunDirZ * 100000.0f, 1.0f };
// Multiply by viewProj_ to get clip space
float clip[4] = {0};
for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
        clip[r] += viewProj_[c * 4 + r] * sunWorld[c];
if (clip[3] > 0.0f) {
    sunScreenPos_[0] = (clip[0] / clip[3]) * 0.5f + 0.5f;
    sunScreenPos_[1] = (clip[1] / clip[3]) * 0.5f + 0.5f;
}
```

In `endScene()`, insert god ray pass after screen shadow, before bloom:
```cpp
    runScreenShadow();
    runSSAO();
    runGodRays();      // NEW — additive god rays before bloom
    runBloom();
```

**Step 6: Build, deploy, test**

Run: `/mc2-build-deploy`

Verify:
- Light shafts radiate from sun position
- Rays are occluded by terrain/objects (dark areas block rays)
- Cloud gaps modulate ray brightness
- Rays fade when sun is near screen edge
- No artifacts when sun is behind camera

**Step 7: Commit**

```
feat: add god ray post-process with cloud shadow interaction

Screen-space radial blur creates volumetric light scattering from sun
position. Half-res with 32 samples, exponential decay. Cloud shadow FBM
noise modulates ray brightness to simulate light streaming through cloud
gaps. Additive composite before bloom.
```

---

### Task 4: Depth-Aware Shorelines — Post-Process

**Files:**
- Create: `shaders/shoreline.frag`
- Modify: `GameOS/gameos/gos_postprocess.h` (add shorelineProg_, toggle)
- Modify: `GameOS/gameos/gos_postprocess.cpp` (compile shader, add pass)

**Step 1: Create shoreline.frag**

```glsl
//#version 420 (version provided by prefix)

#define PREC highp

#include <include/noise.hglsl>

in vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

uniform sampler2D sceneDepthTex;
uniform sampler2D sceneNormalTex;
uniform vec2 screenSize;
uniform float time;

void main()
{
    PREC vec4 normalData = texture(sceneNormalTex, TexCoord);
    float myAlpha = normalData.a;

    // Only process water pixels (alpha ~0.25 from terrain or overlay)
    bool isWater = (myAlpha > 0.15 && myAlpha < 0.35);
    if (!isWater) {
        FragColor = vec4(1.0);  // multiplicative identity
        return;
    }

    float myDepth = texture(sceneDepthTex, TexCoord).r;
    vec2 texelSize = 1.0 / screenSize;

    // Sample 8 neighbors to detect land/water boundary
    float shoreScore = 0.0;
    int landNeighbors = 0;
    float minDepthDiff = 1.0;

    const vec2 offsets[8] = vec2[](
        vec2(-1, 0), vec2(1, 0), vec2(0, -1), vec2(0, 1),
        vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1)
    );

    // Sample at multiple radii for wider shore detection
    for (int r = 1; r <= 3; r++) {
        for (int i = 0; i < 8; i++) {
            vec2 sampleUV = TexCoord + offsets[i] * texelSize * float(r) * 2.0;
            float neighborAlpha = texture(sceneNormalTex, sampleUV).a;
            float neighborDepth = texture(sceneDepthTex, sampleUV).r;

            // Neighbor is terrain (alpha > 0.5) or object (alpha < 0.15)
            bool neighborIsLand = (neighborAlpha > 0.5);

            if (neighborIsLand) {
                landNeighbors++;
                float depthDiff = abs(myDepth - neighborDepth);
                minDepthDiff = min(minDepthDiff, depthDiff);
            }
        }
    }

    if (landNeighbors == 0) {
        FragColor = vec4(1.0);  // no shore here
        return;
    }

    // Shore intensity based on how many land neighbors + depth proximity
    float shoreIntensity = float(landNeighbors) / 24.0;  // normalize
    shoreIntensity = smoothstep(0.1, 0.5, shoreIntensity);

    // Animated foam pattern using FBM noise
    vec2 foamCoord = TexCoord * screenSize * 0.02;  // scale for foam detail
    float foam = fbm(foamCoord + vec2(time * 0.1, time * 0.05), 3);
    foam = smoothstep(0.0, 0.4, foam * 0.5 + 0.5);

    // Pulsing wave edge
    float wave = sin(time * 2.0 + length(TexCoord - vec2(0.5)) * 50.0) * 0.5 + 0.5;
    foam = mix(foam, 1.0, wave * 0.2);

    // Final shore color: white foam, multiplicative blend
    float foamBrightness = shoreIntensity * foam * 0.4;  // subtle
    FragColor = vec4(1.0 + foamBrightness, 1.0 + foamBrightness, 1.0 + foamBrightness * 0.9, 1.0);
}
```

**Step 2: Add shoreline members to gos_postprocess.h**

In public section:
```cpp
    // Shoreline
    bool shorelineEnabled_;
    void runShoreline();
```

In private section:
```cpp
    glsl_program* shorelineProg_;
```

**Step 3: Compile shoreline program**

In constructor:
```cpp
    , shorelineEnabled_(true)
    , shorelineProg_(nullptr)
```

In `init()`:
```cpp
    shorelineProg_ = glsl_program::makeProgram("shoreline",
        "shaders/postprocess.vert", "shaders/shoreline.frag", kShaderPrefix);
    if (!shorelineProg_ || !shorelineProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shoreline shader\n");
```

In `destroy()`:
```cpp
    delete shorelineProg_; shorelineProg_ = nullptr;
```

**Step 4: Implement runShoreline()**

```cpp
void gosPostProcess::runShoreline()
{
    ZoneScopedN("Render.Shoreline");
    TracyGpuZone("Render.Shoreline");

    if (!shorelineEnabled_ || !shorelineProg_ || !shorelineProg_->is_valid()) return;

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
    glViewport(0, 0, width_, height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Multiplicative blend: foam brightens water
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);

    shorelineProg_->setInt("sceneDepthTex", 0);
    shorelineProg_->setInt("sceneNormalTex", 1);
    float screenSz[2] = { (float)width_, (float)height_ };
    shorelineProg_->setFloat2("screenSize", screenSz);

    static uint64_t shore_start = timing::get_wall_time_ms();
    float elapsed = (float)(timing::get_wall_time_ms() - shore_start) / 1000.0f;
    shorelineProg_->setFloat("time", elapsed);
    shorelineProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}
```

**Step 5: Wire into endScene()**

```cpp
    runScreenShadow();
    runShoreline();     // NEW — after shadows, before SSAO
    runSSAO();
    runGodRays();
    runBloom();
```

**Step 6: Build, deploy, test**

Run: `/mc2-build-deploy`

Verify:
- White foam lines appear at water/land boundaries
- Foam animates with time (shifting, pulsing)
- No foam on terrain or objects far from water
- Foam width is subtle and appropriate

**Step 7: Commit**

```
feat: add depth-aware shoreline foam via post-process

Detects water/land boundary using GBuffer1 alpha flags (water=0.25,
terrain=1.0). Applies animated FBM foam pattern at shore edges via
multiplicative blend. Runs after screen shadow pass.
```

---

### Task 5: Texture Upscaling — Menu/UI Assets

**Files:**
- Create: `upscale_ui_textures.py` (new script for UI/menu textures)

**Step 1: Identify UI texture locations**

Investigate `mc2srcdata/` and game data directories for menu/UI TGA files. Look for:
- Loading screen backgrounds
- Menu backgrounds and buttons
- HUD elements
- Mission briefing art
- Faction/unit portraits

Use the existing `esrgan_upscale.py` as the base — it already has RRDB network, tiled inference, and TGA deploy.

**Step 2: Create upscale_ui_textures.py**

Adapt `esrgan_upscale.py` to:
- Accept a directory of TGA files instead of burnin PNGs
- Handle RGBA (preserve alpha channel — process RGB with ESRGAN, keep original alpha)
- Output to the deploy directory matching original paths
- Skip normal maps (detect by filename or alpha channel analysis)

Key alpha handling:
```python
# Split RGBA, upscale RGB only, recombine with upscaled alpha
img = Image.open(src_path).convert('RGBA')
rgb = img.convert('RGB')
alpha = img.split()[3]

# Upscale RGB with ESRGAN
rgb_up = upscale_with_model(model, rgb)

# Upscale alpha with bicubic (preserves sharp edges)
alpha_up = alpha.resize(rgb_up.size, Image.LANCZOS)

# Recombine
result = Image.merge('RGBA', (*rgb_up.split(), alpha_up))
result.save(dst_path)
```

**Step 3: Run on UI textures**

```bash
python upscale_ui_textures.py esrgan_models/4x_GameAI_2.0.pth --batch --deploy
```

**Step 4: Test in-game**

Run: `/mc2-build-deploy`

Verify:
- Menus render with higher-res textures
- No alpha artifacts on transparent UI elements
- No visual glitches from upscaling artifacts
- Loading screens look crisp

**Step 5: Commit**

```
feat: add 4x AI-upscaled UI/menu textures

ESRGAN 4x upscale of menu backgrounds, loading screens, HUD elements,
and unit portraits. Alpha channels preserved via separate LANCZOS upscale.
```

---

## Implementation Order Summary

1. **Task 1:** GBuffer1 foundation (prerequisite for Task 4)
2. **Task 2:** GPU grass (biggest visual impact)
3. **Task 3:** God rays
4. **Task 4:** Shorelines (depends on Task 1)
5. **Task 5:** Texture upscaling (independent, can happen anytime)

## Key Technical Notes

- **Uniform API:** `setFloat`/`setInt` BEFORE `apply()`, not after. `apply()` flushes dirty uniforms.
- **GL_FALSE for terrainMVP:** Direct-uploaded row-major matrices use `GL_FALSE`.
- **Shader #version:** Never in shader files. Pass `"#version 420\n"` as prefix to `makeProgram()`.
- **Shader includes:** Use `#include <include/file.hglsl>` — custom preprocessor resolves relative to shader dir.
- **MC2 coordinates:** WorldPos x=east, y=north, z=elevation. "Up" is +Z.
- **Build:** ALWAYS `--config RelWithDebInfo`. Release crashes with GL_INVALID_ENUM.
- **Deploy:** NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`.
