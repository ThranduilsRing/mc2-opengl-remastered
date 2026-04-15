# World-Space Overlay Rewrite — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the IS_OVERLAY / `rhw=1.0` / terrainMVP workaround stack with typed world-space vertex buffers and dedicated draw calls for alpha cement tiles, craters, and footprints.

**Architecture:** Two new per-frame GPU batches (`TerrainOverlayBatch` for cement, `DecalBatch` for craters/footprints) accumulated during `land->render()` / `craterManager->render()` and flushed at the correct point in `txmmgr.cpp`'s `renderLists()`. Vertex shader reuses the `terrainMVP + terrainViewport + mvp` projection chain unconditionally on typed `WorldOverlayVert` inputs — no `rhw` flag detection, no conditional branch. `gos_tex_vertex` IS_OVERLAY variant and `Overlay.SplitDraw` deleted once all callers are migrated.

**Tech Stack:** OpenGL 4.2, GLSL 420, C++14, MSVC (Windows). Build `--config RelWithDebInfo`. Deploy to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`.

**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`

**Key docs to re-read if confused:**
- `docs/plans/2026-04-15-world-space-overlay-design.md` — this design
- `docs/architecture.md` — render pipeline, coordinate spaces
- `docs/cement-overlay-codemap.md` — current overlay data flow
- `CLAUDE.md` — build rules, deploy rules, uniform API order

---

## Task 1: Audit mission markers before touching Overlay.SplitDraw

**Why first:** If mission markers go through `gos_State_Overlay`, deleting `Overlay.SplitDraw` will make them invisible. This was flagged as a potential root cause of the "shadow under marker" bug. Determine this before writing any code.

**Files:** Read-only

**Step 1: Find all gos_State_Overlay callers**

```bash
grep -rn "gos_State_Overlay\|MC2_ISCRATERS\|rhw.*1\.0\|setOverlayWorldCoords" mclib/ code/ --include="*.cpp" --include="*.h"
```

**Step 2: Trace every MC2_ISCRATERS callsite**

For each file that sets `gos_State_Overlay` or `MC2_ISCRATERS`, read the surrounding context. Expected findings:
- `mclib/quad.cpp` — cement overlay (in scope, will migrate)
- `mclib/crater.cpp` — craters/footprints (in scope, will migrate)
- `mclib/txmmgr.cpp` — the dispatch loops (will delete)
- Anything else → **STOP and document before proceeding**

**Step 3: Search for mission marker / objective indicator rendering**

```bash
grep -rn "MissionMarker\|ObjectiveHighlight\|NavMarker\|missionIcon\|objectiveIcon\|addTriangle.*MC2_DRAWALPHA\|addVertices.*MC2_DRAWALPHA" mclib/ code/ --include="*.cpp" | grep -v "ISCRATERS\|ISTERRAIN\|ISWATER\|crater\|quad\."
```

Also search for anything using `gos_tex_vertex_lighted` with projected markers:
```bash
grep -rn "gpuProjection\|gos_State_Overlay\|MC2_ISOVERLAY" code/ mclib/ --include="*.cpp"
```

**Step 4: Document findings and confirm go/no-go**

If mission markers do NOT use `gos_State_Overlay`: proceed. Add a one-line note to `docs/cement-overlay-codemap.md`:
```
## Mission marker audit (2026-04-15)
Markers confirmed to use gos_tex_vertex_lighted.frag gpuProjection path.
They do NOT set gos_State_Overlay and are unaffected by IS_OVERLAY deletion.
```

If markers DO use `gos_State_Overlay`: stop and design a migration path before proceeding with this plan.

**Step 5: Commit**
```bash
git add docs/cement-overlay-codemap.md
git commit -m "docs: mission marker audit — confirm overlay.SplitDraw safe to delete"
```

---

## Task 2: Add WorldOverlayVert struct and new GameOS API declarations

**Files:**
- Modify: `GameOS/gameos/gameos.hpp` (the main GameOS API header — look for `gos_SetTerrainMVP` to find the right section)

**Step 1: Add the struct and function declarations**

Find the section in `gameos.hpp` near `gos_SetTerrainMVP` or `gos_SetRenderState`. Add:

```cpp
// ─── World-space overlay batch API ──────────────────────────────────────────
// Called from mclib during land->render() / craterManager->render().
// Batches are flushed by gos_DrawTerrainOverlays() / gos_DrawDecals()
// which are called from txmmgr::renderLists() at the correct render phase.

struct WorldOverlayVert {
    float wx, wy, wz;   // MC2 world space: x=east, y=north, z=elev
    float u, v;          // texture coordinates
    float fog;           // fog factor [0,1]: 1=clear, 0=fully fogged
    uint32_t argb;       // BGRA packed: high byte=A, byte2=R, byte1=G, byte0=B
};                       // sizeof = 28 bytes

// Push one triangle (3 verts) into the terrain overlay batch.
// All cement perimeter tiles go here.
extern void __stdcall gos_PushTerrainOverlay(const WorldOverlayVert* verts, uint32_t texHandle);

// Push one triangle (3 verts) into the decal batch.
// Craters (texHandle = crater atlas), footprints (texHandle = footprint atlas).
extern void __stdcall gos_PushDecal(const WorldOverlayVert* verts, uint32_t texHandle);

// Flush batches — called from txmmgr::renderLists() after DRAWALPHA terrain detail.
// Internally resets batch vectors after drawing, ready for next frame.
extern void __stdcall gos_DrawTerrainOverlays();
extern void __stdcall gos_DrawDecals();
// ─────────────────────────────────────────────────────────────────────────────
```

**Step 2: Build to verify it compiles (header only change)**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Expected: Compiles (or only fails on unresolved externals for the new functions — that is expected at this stage).

**Step 3: Commit**
```bash
git add GameOS/gameos/gameos.hpp
git commit -m "feat: add WorldOverlayVert struct and overlay batch API declarations"
```

---

## Task 3: Implement batch structures and push functions in gameos_graphics.cpp

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

**Step 1: Add batch member structs near the top of gosRenderer class**

Find the `gosRenderer` class definition (search for `class gosRenderer`). Add private members alongside existing batch state (near `terrain_mvp_`, around line ~1550):

```cpp
// ─── World-space overlay batches ────────────────────────────────────────────
struct OverlayTriVert {
    float wx, wy, wz, u, v, fog;
    uint32_t argb;
};
struct TerrainOverlayBatch_ {
    GLuint vbo = 0, vao = 0;
    std::vector<OverlayTriVert> verts;
    uint32_t texHandle = 0;
} terrainOverlayBatch_;

struct DecalTriEntry_ { uint32_t texHandle; uint32_t firstVert; uint32_t vertCount; };
struct DecalBatch_ {
    GLuint vbo = 0, vao = 0;
    std::vector<OverlayTriVert> verts;
    std::vector<DecalTriEntry_> draws;
} decalBatch_;

glsl_program* overlayProg_ = nullptr;  // shared vert + terrain frag
glsl_program* decalProg_   = nullptr;  // shared vert + decal frag

struct OverlayUniformLocs_ {
    GLint terrainMVP    = -1;
    GLint terrainVP     = -1;
    GLint mvp           = -1;
    GLint tex1          = -1;
    GLint fog_color     = -1;
    GLint time          = -1;
    GLint terrainLightDir = -1;
    // shadow uniforms injected via gos_SetupObjectShadows — no explicit locs needed
} overlayLocs_, decalLocs_;

uint64_t overlayTimeStart_ = 0;
// ─────────────────────────────────────────────────────────────────────────────
```

**Note:** `OverlayTriVert` is the internal copy of `WorldOverlayVert` — same layout. Using a local name avoids including a public header inside the implementation.

**Step 2: Add the push function implementations**

Find the block of `gos_Set*` function implementations (around `gos_SetTerrainMVP` at ~line 4185). Add immediately after:

```cpp
// ─── World-space overlay push functions ──────────────────────────────────────

static void resetOverlayBatchesIfNewFrame() {
    // Reset on first push per frame (called from push functions below).
    // Using vector::clear() keeps capacity, avoiding realloc each frame.
    static int lastResetFrame = -1;
    if (!g_gos_renderer) return;
    int curFrame = g_gos_renderer->frameCount_;  // or whatever the frame counter field is
    if (curFrame != lastResetFrame) {
        g_gos_renderer->terrainOverlayBatch_.verts.clear();
        g_gos_renderer->terrainOverlayBatch_.texHandle = 0;
        g_gos_renderer->decalBatch_.verts.clear();
        g_gos_renderer->decalBatch_.draws.clear();
        lastResetFrame = curFrame;
    }
}

void __stdcall gos_PushTerrainOverlay(const WorldOverlayVert* verts, uint32_t texHandle) {
    if (!g_gos_renderer || !verts) return;
    resetOverlayBatchesIfNewFrame();
    auto& b = g_gos_renderer->terrainOverlayBatch_;
    b.texHandle = texHandle;  // cement uses one texture; overwritten each push is fine
    for (int i = 0; i < 3; ++i) {
        gosRenderer::OverlayTriVert v;
        v.wx = verts[i].wx; v.wy = verts[i].wy; v.wz = verts[i].wz;
        v.u  = verts[i].u;  v.v  = verts[i].v;
        v.fog  = verts[i].fog;
        v.argb = verts[i].argb;
        b.verts.push_back(v);
    }
}

void __stdcall gos_PushDecal(const WorldOverlayVert* verts, uint32_t texHandle) {
    if (!g_gos_renderer || !verts) return;
    resetOverlayBatchesIfNewFrame();
    auto& b = g_gos_renderer->decalBatch_;
    // Merge with previous draw entry if same texture
    if (!b.draws.empty() && b.draws.back().texHandle == texHandle) {
        b.draws.back().vertCount += 3;
    } else {
        gosRenderer::DecalTriEntry_ entry;
        entry.texHandle  = texHandle;
        entry.firstVert  = (uint32_t)b.verts.size();
        entry.vertCount  = 3;
        b.draws.push_back(entry);
    }
    for (int i = 0; i < 3; ++i) {
        gosRenderer::OverlayTriVert v;
        v.wx = verts[i].wx; v.wy = verts[i].wy; v.wz = verts[i].wz;
        v.u  = verts[i].u;  v.v  = verts[i].v;
        v.fog  = verts[i].fog;
        v.argb = verts[i].argb;
        b.verts.push_back(v);
    }
}
// ─────────────────────────────────────────────────────────────────────────────
```

**Note on frameCount_:** Check what the gosRenderer frame counter field is actually named. Search `gameos_graphics.cpp` for `frame` — it might be `frame_`, `frameCount_`, `currentFrame_`, or similar. If there is no frame counter, use `gos_GetElapsedTime() > 0` as a proxy, or just always clear on draw (the draw functions clear after use anyway). The reset-on-first-push pattern prevents double-clear per frame.

**Step 3: Build to verify**
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -30
```
Expected: Compiles. `gos_DrawTerrainOverlays` and `gos_DrawDecals` will be unresolved until Task 8.

**Step 4: Commit**
```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat: add WorldOverlayVert batch structs and push function stubs"
```

---

## Task 4: Write terrain_overlay.vert shader

**Files:**
- Create: `shaders/terrain_overlay.vert`

**IMPORTANT:** Do NOT add `#version` — it is injected by `makeProgram` as `"#version 420\n"`.

**Step 1: Create the file**

```glsl
// terrain_overlay.vert
// World-space overlay vertex shader — shared by terrain_overlay.frag and decal.frag.
// Projection chain: MC2 world coords -> screen pixels (terrainMVP) -> NDC (terrainViewport+mvp).
// Identical math to the TES world-space path, applied unconditionally on typed inputs.
// No rhw flag detection, no conditional branch.

layout(location=0) in vec3  worldPos;
layout(location=1) in vec2  texcoord;
layout(location=2) in float fogVal;
layout(location=3) in vec4  color;     // GL_UNSIGNED_BYTE normalized: memory=BGRA, arrives as (B,G,R,A)/255

uniform mat4 terrainMVP;       // MC2 world -> screen pixel homogeneous (row-major, uploaded GL_FALSE)
uniform vec4 terrainViewport;  // (vmx, vmy, vax, vay) — same as TES
uniform mat4 mvp;              // screen pixels -> NDC

out vec3  WorldPos;
out vec2  Texcoord;
out float FogValue;
out vec4  Color;    // passes raw BGRA-byte-derived vec4 to frag; frag swizzles with .bgra

void main() {
    WorldPos = worldPos;
    Texcoord = texcoord;
    FogValue = fogVal;
    Color    = color;

    // Replicate TES projection chain exactly (non-linear — cannot fold into single matrix).
    vec4  clip4 = terrainMVP * vec4(worldPos, 1.0);
    float rhw_c = 1.0 / clip4.w;
    vec3  px;
    px.x = clip4.x * rhw_c * terrainViewport.x + terrainViewport.z;
    px.y = clip4.y * rhw_c * terrainViewport.y + terrainViewport.w;
    px.z = clip4.z * rhw_c;
    vec4  ndc  = mvp * vec4(px, 1.0);
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);
}
```

**Step 2: Commit**
```bash
git add shaders/terrain_overlay.vert
git commit -m "feat: add terrain_overlay.vert — typed world-space vertex shader"
```

---

## Task 5: Write terrain_overlay.frag shader (cement tiles)

**Files:**
- Create: `shaders/terrain_overlay.frag`

**Step 1: Create the file**

```glsl
// terrain_overlay.frag
// Fragment shader for alpha cement perimeter/transition tiles.
// Tone correction, full-range cloud shadows, map shadow sampling, fog, GBuffer1 terrain flag.
// Full-range cloud FBM (mix(0.70,1.0,...)) matches interior terrain — no luminance compensation
// needed because GBuffer1.alpha=1 tells shadow_screen to skip these pixels (same as solid terrain).

#include <include/noise.hglsl>
#include <include/shadow.hglsl>

in highp vec3  WorldPos;
in highp vec2  Texcoord;
in highp float FogValue;
in highp vec4  Color;   // BGRA-derived: use Color.bgra to get (R,G,B,A)

layout(location=0) out highp vec4 FragColor;
layout(location=1) out highp vec4 GBuffer1;   // terrain flag — shadow_screen skips alpha>=0.5

uniform sampler2D    tex1;
uniform highp vec4   fog_color;
uniform highp float  time;
uniform highp vec4   terrainLightDir;   // world-space sun dir, injected by gos_SetupObjectShadows

void main() {
    highp vec4 tex_color = texture(tex1, Texcoord);
    highp vec4 vertColor = Color.bgra;   // reswizzle BGRA bytes to RGBA [0,1]

    // Tone correction on raw texture (not pre-multiplied by vertex color).
    // Vertex luminance is extracted separately and re-applied so per-vertex AO/lighting
    // is preserved without hue contamination from non-cement neighbour vertices.
    highp float vertexLum = dot(vertColor.rgb, vec3(0.299, 0.587, 0.114));
    highp float texLuma   = dot(tex_color.rgb, vec3(0.299, 0.587, 0.114));
    highp vec3  texGray   = vec3(texLuma);
    highp vec3  neutral   = vec3(0.69, 0.69, 0.68);
    highp vec3  corrected = mix(tex_color.rgb, texGray, 0.22);
    corrected = mix(corrected, neutral, 0.15);
    corrected *= 0.88;
    corrected *= vertexLum;
    highp vec4 c = vec4(corrected, vertColor.a * tex_color.a);

    // Cloud shadows — full range matches interior terrain (no 0.925 luminance hack needed).
    {
        highp vec2  cloudUV    = WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
        highp float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
        c.rgb *= mix(0.70, 1.0, smoothstep(0.3, 0.7, cloudNoise));
    }

    // Map shadows (static terrain + dynamic mechs/buildings).
    // terrainLightDir and shadow samplers injected by gos_SetupObjectShadows().
    {
        highp vec3  lightDir = normalize(terrainLightDir.xyz);
        highp float shadow   = calcShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir, 8);
        shadow = min(shadow, calcDynamicShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir, 4));
        c.rgb *= shadow;
    }

    // Fog — FogValue=1 clear, FogValue=0 full fog.
    if (fog_color.x > 0.0 || fog_color.y > 0.0 || fog_color.z > 0.0 || fog_color.w > 0.0)
        c.rgb = mix(fog_color.rgb, c.rgb, FogValue);

    FragColor = c;
    GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);   // alpha=1 → shadow_screen skips (same as solid terrain)
}
```

**Step 2: Commit**
```bash
git add shaders/terrain_overlay.frag
git commit -m "feat: add terrain_overlay.frag — cement tone correction + full-range cloud shadows"
```

---

## Task 6: Write decal.frag shader (craters and footprints)

**Files:**
- Create: `shaders/decal.frag`

**Step 1: Create the file**

```glsl
// decal.frag
// Fragment shader for bomb craters and mech footprints.
// Alpha-blended, no tone correction (decal textures are authored correct).
// Shadow + narrow cloud FBM + fog + GBuffer1 terrain flag.

#include <include/noise.hglsl>
#include <include/shadow.hglsl>

in highp vec3  WorldPos;
in highp vec2  Texcoord;
in highp float FogValue;
in highp vec4  Color;

layout(location=0) out highp vec4 FragColor;
layout(location=1) out highp vec4 GBuffer1;

uniform sampler2D    tex1;
uniform highp vec4   fog_color;
uniform highp float  time;
uniform highp vec4   terrainLightDir;

void main() {
    highp vec4 tex_color = texture(tex1, Texcoord);
    highp vec4 c = Color.bgra * tex_color;   // no tone correction — decal textures are authored

    // Cloud shadows — narrow range (craters are already dark)
    {
        highp vec2  cloudUV    = WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
        highp float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
        c.rgb *= mix(0.88, 1.0, smoothstep(0.3, 0.7, cloudNoise));
    }

    // Map shadows
    {
        highp vec3  lightDir = normalize(terrainLightDir.xyz);
        highp float shadow   = calcShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir, 8);
        shadow = min(shadow, calcDynamicShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir, 4));
        c.rgb *= shadow;
    }

    // Fog
    if (fog_color.x > 0.0 || fog_color.y > 0.0 || fog_color.z > 0.0 || fog_color.w > 0.0)
        c.rgb = mix(fog_color.rgb, c.rgb, FogValue);

    FragColor = c;
    GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
}
```

**Step 2: Commit**
```bash
git add shaders/decal.frag
git commit -m "feat: add decal.frag — alpha-blend crater/footprint shader"
```

---

## Task 7: Load shaders, create VAOs, cache uniform locations

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

Find where other standalone shaders are loaded (search for `"shadow_object"` or `"gos_grass"` or `"ssao"` — they are loaded outside the material system). Follow that exact pattern.

**Step 1: Load the two shader programs**

In the gosRenderer initialization function (probably `init()` or wherever shadow/post-process programs are created), add:

```cpp
// terrain_overlay.vert is shared — note makeProgram takes (name, vs_path, ps_path, defines)
// or makeProgram2(name, defines) if it auto-derives paths from name.
// Check how shadow_object or gos_grass is loaded and match that exact call.
//
// The #version 420 prefix is injected by makeProgram automatically (per CLAUDE.md rule).
overlayProg_ = glsl_program::makeProgram("terrain_overlay",
    "shaders/terrain_overlay.vert", "shaders/terrain_overlay.frag", "");
decalProg_ = glsl_program::makeProgram("decal",
    "shaders/terrain_overlay.vert", "shaders/decal.frag", "");

if (!overlayProg_ || !decalProg_) {
    fprintf(stderr, "[OverlayBatch] Failed to compile overlay/decal shaders\n");
}

// Cache uniform locations
auto cacheOverlayLocs = [](glsl_program* prog, OverlayUniformLocs_& locs) {
    GLuint shp = prog->shp_;
    locs.terrainMVP     = glGetUniformLocation(shp, "terrainMVP");
    locs.terrainVP      = glGetUniformLocation(shp, "terrainViewport");
    locs.mvp            = glGetUniformLocation(shp, "mvp");
    locs.tex1           = glGetUniformLocation(shp, "tex1");
    locs.fog_color      = glGetUniformLocation(shp, "fog_color");
    locs.time           = glGetUniformLocation(shp, "time");
    locs.terrainLightDir= glGetUniformLocation(shp, "terrainLightDir");
};
if (overlayProg_) cacheOverlayLocs(overlayProg_, overlayLocs_);
if (decalProg_)   cacheOverlayLocs(decalProg_,   decalLocs_);

overlayTimeStart_ = timing::get_wall_time_ms();
```

**Step 2: Create VAOs**

Add VAO setup in the same init block, after VBOs are created:

```cpp
// WorldOverlayVert layout (28 bytes):
//   offset  0: float[3] worldPos     → location 0
//   offset 12: float[2] texcoord     → location 1
//   offset 20: float    fog          → location 2
//   offset 24: uint8[4] argb (BGRA)  → location 3, GL_UNSIGNED_BYTE, normalized

constexpr int kOverlayStride = 28;  // sizeof(WorldOverlayVert)

// Terrain overlay VAO
glGenVertexArrays(1, &terrainOverlayBatch_.vao);
glGenBuffers(1, &terrainOverlayBatch_.vbo);
glBindVertexArray(terrainOverlayBatch_.vao);
glBindBuffer(GL_ARRAY_BUFFER, terrainOverlayBatch_.vbo);
glVertexAttribPointer(0, 3, GL_FLOAT,         GL_FALSE, kOverlayStride, (void*)0);
glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE, kOverlayStride, (void*)12);
glVertexAttribPointer(2, 1, GL_FLOAT,         GL_FALSE, kOverlayStride, (void*)20);
glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE,  kOverlayStride, (void*)24);
glEnableVertexAttribArray(0);
glEnableVertexAttribArray(1);
glEnableVertexAttribArray(2);
glEnableVertexAttribArray(3);
glBindVertexArray(0);

// Decal VAO (same layout)
glGenVertexArrays(1, &decalBatch_.vao);
glGenBuffers(1, &decalBatch_.vbo);
glBindVertexArray(decalBatch_.vao);
glBindBuffer(GL_ARRAY_BUFFER, decalBatch_.vbo);
glVertexAttribPointer(0, 3, GL_FLOAT,         GL_FALSE, kOverlayStride, (void*)0);
glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE, kOverlayStride, (void*)12);
glVertexAttribPointer(2, 1, GL_FLOAT,         GL_FALSE, kOverlayStride, (void*)20);
glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE,  kOverlayStride, (void*)24);
glEnableVertexAttribArray(0);
glEnableVertexAttribArray(1);
glEnableVertexAttribArray(2);
glEnableVertexAttribArray(3);
glBindVertexArray(0);
```

**Step 3: Build to verify**
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -30
```
Expected: Compiles. The `gos_DrawTerrainOverlays` / `gos_DrawDecals` still unresolved — fine.

**Step 4: Commit**
```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat: load terrain_overlay + decal shaders, create VAOs, cache uniform locs"
```

---

## Task 8: Implement gos_DrawTerrainOverlays() and gos_DrawDecals()

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

Add both draw functions near the push functions from Task 3. The draw functions:
1. Upload accumulated vertex data to VBO
2. Bind shader, set render state, upload uniforms
3. Draw
4. Clear the batch vectors (reset for next frame)

**Step 1: Add gos_DrawTerrainOverlays()**

```cpp
void __stdcall gos_DrawTerrainOverlays() {
    if (!g_gos_renderer) return;
    auto& b = g_gos_renderer->terrainOverlayBatch_;
    if (b.verts.empty() || !g_gos_renderer->overlayProg_) {
        b.verts.clear();
        return;
    }

    // Upload vertex data
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        b.verts.size() * sizeof(gosRenderer::OverlayTriVert),
        b.verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Render state: opaque, depth-write ON, depth-test LEQUAL (same as terrain)
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    // Bind shader
    glsl_program* prog = g_gos_renderer->overlayProg_;
    glUseProgram(prog->shp_);
    const auto& L = g_gos_renderer->overlayLocs_;

    // Projection uniforms (same values as injected into Overlay.SplitDraw)
    if (L.terrainMVP >= 0)
        glUniformMatrix4fv(L.terrainMVP, 1, GL_FALSE,
            (const float*)&g_gos_renderer->terrain_mvp_);
    if (L.terrainVP >= 0)
        glUniform4fv(L.terrainVP, 1, (const float*)&g_gos_renderer->terrain_viewport_);

    // Screen-to-NDC mvp: get from the current gosRenderer mvp state.
    // Look at how Overlay.SplitDraw retrieves it — it calls mat->apply() then reads
    // the shader's mvp uniform. We need the same matrix directly.
    // Search gameos_graphics.cpp for where 'mvp' is uploaded to gos_tex_vertex and replicate.
    // (Typically it is stored as mvp_ in gosRenderer or readable from the viewport state.)
    if (L.mvp >= 0) {
        // TODO: replace mvp_matrix with the actual gosRenderer field name for the screen→NDC matrix
        glUniformMatrix4fv(L.mvp, 1, GL_FALSE, (const float*)&g_gos_renderer->mvp_);
    }

    // Time for cloud FBM
    if (L.time >= 0) {
        float elapsed = (float)(timing::get_wall_time_ms() - g_gos_renderer->overlayTimeStart_) / 1000.0f;
        glUniform1f(L.time, elapsed);
    }

    // Shadow + fog uniforms via existing helper (same as SplitDraw used)
    // gos_SetupObjectShadows binds shadow textures and uploads shadow uniforms to currently bound program.
    // NOTE: call BEFORE glUseProgram for the deferred path? Check CLAUDE.md:
    // "setFloat/setInt BEFORE apply(), not after" — but gos_SetupObjectShadows uses direct glUniform*.
    // Direct glUniform* calls work while the program is bound. Call after glUseProgram is fine.
    // Also need fog_color — look at how it's set in the existing basic shader path and replicate.
    gos_SetupObjectShadows(prog);

    // Texture
    if (L.tex1 >= 0) glUniform1i(L.tex1, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, b.texHandle);  // cement overlay atlas

    // Draw
    glBindVertexArray(b.vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b.verts.size());
    glBindVertexArray(0);

    // Restore
    glDepthFunc(GL_LESS);  // or whatever the default is — check if LEQUAL is already default
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glUseProgram(0);

    b.verts.clear();
    b.texHandle = 0;
}
```

**Step 2: Add gos_DrawDecals()**

```cpp
void __stdcall gos_DrawDecals() {
    if (!g_gos_renderer) return;
    auto& b = g_gos_renderer->decalBatch_;
    if (b.draws.empty() || !g_gos_renderer->decalProg_) {
        b.verts.clear();
        b.draws.clear();
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        b.verts.size() * sizeof(gosRenderer::OverlayTriVert),
        b.verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Render state: alpha blend, depth-write OFF, depth-test on
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glEnable(GL_TEXTURE_2D);

    glsl_program* prog = g_gos_renderer->decalProg_;
    glUseProgram(prog->shp_);
    const auto& L = g_gos_renderer->decalLocs_;

    if (L.terrainMVP >= 0)
        glUniformMatrix4fv(L.terrainMVP, 1, GL_FALSE,
            (const float*)&g_gos_renderer->terrain_mvp_);
    if (L.terrainVP >= 0)
        glUniform4fv(L.terrainVP, 1, (const float*)&g_gos_renderer->terrain_viewport_);
    if (L.mvp >= 0)
        glUniformMatrix4fv(L.mvp, 1, GL_FALSE, (const float*)&g_gos_renderer->mvp_);
    if (L.time >= 0) {
        float elapsed = (float)(timing::get_wall_time_ms() - g_gos_renderer->overlayTimeStart_) / 1000.0f;
        glUniform1f(L.time, elapsed);
    }
    gos_SetupObjectShadows(prog);
    if (L.tex1 >= 0) glUniform1i(L.tex1, 0);

    glBindVertexArray(b.vao);
    for (const auto& entry : b.draws) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, entry.texHandle);
        glDrawArrays(GL_TRIANGLES, entry.firstVert, entry.vertCount);
    }
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glUseProgram(0);

    b.verts.clear();
    b.draws.clear();
}
```

**Step 3: Find the mvp_ field name**

The `L.mvp` upload references `g_gos_renderer->mvp_`. Find the actual field name: search `gameos_graphics.cpp` for where `"mvp"` is uploaded to a shader program using `glUniformMatrix4fv`. The matrix stored there is the one we want. If it's computed from the viewport each frame, expose it the same way.

**Step 4: Build**
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -30
```
Expected: Compiles fully. The functions exist but nothing calls them yet.

**Step 5: Commit**
```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat: implement gos_DrawTerrainOverlays + gos_DrawDecals"
```

---

## Task 9: Wire draw calls into txmmgr.cpp renderLists()

**Files:**
- Modify: `mclib/txmmgr.cpp`

**Step 1: Add includes if needed**

At top of txmmgr.cpp, check if `gameos.hpp` is already included (it is). `gos_DrawTerrainOverlays` and `gos_DrawDecals` are declared there — no new includes needed.

**Step 2: Find the right insertion point**

In `renderLists()`, the current structure after DRAWALPHA terrain detail is (around lines 1300-1470):

```
// DRAWSOLID done
{  // Render.Overlays zone
    // ... DRAWALPHA detail loop (lines ~1316-1373) ...
    // ... Render.NoUnderlayer block (lines ~1376-1443, MC2_GPUOVERLAY) ...
    // ... Render.CraterOverlays loop (lines ~1465-1520, MC2_ISTERRAIN+MC2_ISCRATERS) ...
}
```

**Step 3: Insert new draw calls after DRAWALPHA detail, before CraterOverlays**

Find the end of the DRAWALPHA detail loop (around line 1373, look for `// reset alpha test at the end`). Insert BEFORE the `Render.CraterOverlays` block:

```cpp
    // NEW: world-space overlay batches — accumulated during land->render() and craterManager->render()
    gos_DrawTerrainOverlays();   // alpha cement perimeter tiles
    gos_DrawDecals();            // bomb craters + mech footprints
```

Leave the `Render.CraterOverlays` and non-terrain craters blocks in place for now — they will still draw (double rendering during this transition task). That is intentional: it proves the new path renders before we delete the old path.

**Step 4: Build + deploy**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```

Use `/mc2-deploy` skill or manually:
```bash
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
for f in shaders/terrain_overlay.vert shaders/terrain_overlay.frag shaders/decal.frag; do
    cp -f "$f" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/$f"
    diff -q "$f" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/$f"
done
```

**Step 5: Verify game starts, no GL errors in console**

Launch the game. Check stdout for `[OverlayBatch] Failed` or GL errors. If the new shaders compile, nothing visible will change yet (push functions are not called).

**Step 6: Commit**
```bash
git add mclib/txmmgr.cpp
git commit -m "feat: wire gos_DrawTerrainOverlays/gos_DrawDecals into renderLists()"
```

---

## Task 10: Migrate quad.cpp — alpha cement overlay to gos_PushTerrainOverlay

**Files:**
- Modify: `mclib/quad.cpp`

**Step 1: Find all overlay submission sites**

```bash
grep -n "MC2_ISCRATERS\|setOverlayWorldCoords\|overlayHandle" mclib/quad.cpp
```

There are two phases to change:
1. Triangle count reservation (`addTriangle` calls ~line 330)
2. Actual vertex submission (`addVertices` calls inside `TerrainQuad::draw()` ~line 1577)

**Step 2: Remove addTriangle(MC2_ISCRATERS) reservation calls**

Find the isCement section (~lines 329-332):
```cpp
if(overlayHandle!=0) {
    mcTextureManager->addTriangle(overlayHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
    mcTextureManager->addTriangle(overlayHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
}
```
Delete these two lines. The overlayHandle reservation is no longer needed.

**Step 3: Replace addVertices(MC2_ISCRATERS) with gos_PushTerrainOverlay in TerrainQuad::draw()**

Find the overlay submission block inside `draw()` (search for `setOverlayWorldCoords`). It appears twice (BOTTOMRIGHT and BOTTOMLEFT quad orientations). For each occurrence, replace:

```cpp
// OLD — replace this entire if block:
if (useOverlayTexture && (overlayHandle != 0xffffffff))
{
    gos_VERTEX oVertex[3];
    memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
    oVertex[0].u    = oldminU; oVertex[0].v = oldminV;
    oVertex[1].u    = oldmaxU; oVertex[1].v = oldminV;
    oVertex[2].u    = oldmaxU; oVertex[2].v = oldmaxV;
    oVertex[0].argb = vertices[0]->lightRGB;
    oVertex[1].argb = vertices[1]->lightRGB;
    oVertex[2].argb = vertices[2]->lightRGB;
    setOverlayWorldCoords(oVertex[0], vertices[0]);
    setOverlayWorldCoords(oVertex[1], vertices[1]);
    setOverlayWorldCoords(oVertex[2], vertices[2]);
    mcTextureManager->addVertices(overlayHandle, oVertex, MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
}
```

Replace with:
```cpp
// NEW — world-space overlay batch
if (useOverlayTexture && (overlayHandle != 0xffffffff))
{
    // fogRGB.a = fog factor [0,255], already computed above as part of gVertex[i].frgb
    auto extractFog = [](const gos_VERTEX& v) -> float {
        return ((v.frgb >> 24) & 0xFF) / 255.0f;
    };
    WorldOverlayVert ov[3];
    // Vertex 0
    ov[0].wx = vertices[0]->vx;  ov[0].wy = vertices[0]->vy;
    ov[0].wz = vertices[0]->pVertex->elevation + OVERLAY_ELEV_OFFSET;
    ov[0].u  = oldminU;  ov[0].v  = oldminV;
    ov[0].fog = extractFog(gVertex[0]);
    ov[0].argb = vertices[0]->lightRGB;
    // Vertex 1
    ov[1].wx = vertices[1]->vx;  ov[1].wy = vertices[1]->vy;
    ov[1].wz = vertices[1]->pVertex->elevation + OVERLAY_ELEV_OFFSET;
    ov[1].u  = oldmaxU;  ov[1].v  = oldminV;
    ov[1].fog = extractFog(gVertex[1]);
    ov[1].argb = vertices[1]->lightRGB;
    // Vertex 2
    ov[2].wx = vertices[2]->vx;  ov[2].wy = vertices[2]->vy;
    ov[2].wz = vertices[2]->pVertex->elevation + OVERLAY_ELEV_OFFSET;
    ov[2].u  = oldmaxU;  ov[2].v  = oldmaxV;
    ov[2].fog = extractFog(gVertex[2]);
    ov[2].argb = vertices[2]->lightRGB;

    gos_PushTerrainOverlay(ov, overlayHandle);
}
```

**Note:** `OVERLAY_ELEV_OFFSET` is defined as `0.15f` in quad.cpp. Keep it. `vertices[i]->vx/vy` are raw MC2 world coords (same as `setOverlayWorldCoords` set). Repeat this replacement for the BOTTOMLEFT orientation section too. In the BOTTOMLEFT case the vertex indices and UV corners differ — adjust accordingly (v[0]=0, v[1]=3, v[2]=1 or whichever the second triangle uses).

**Step 4: Verify includes**

Ensure `gameos.hpp` is included in quad.cpp (it should be). `WorldOverlayVert` is declared there.

**Step 5: Build + deploy + quick verify**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Deploy and launch game. Cement perimeter tiles should now render via the new path (you may briefly see double-rendering — that is expected and will be fixed in Task 14).

**Step 6: Commit**
```bash
git add mclib/quad.cpp
git commit -m "feat: migrate alpha cement overlay to gos_PushTerrainOverlay"
```

---

## Task 11: Migrate crater.cpp — craters and footprints to gos_PushDecal

**Files:**
- Modify: `mclib/crater.cpp`

**Step 1: Remove addTriangle(MC2_ISCRATERS) reservation calls in update()**

Find update() (~lines 282-295). Remove:
```cpp
// Remove these lines:
mcTextureManager->addTriangle(craterTextureIndices[0], MC2_ISCRATERS | MC2_DRAWALPHA | MC2_ISTERRAIN);
mcTextureManager->addTriangle(craterTextureIndices[0], MC2_ISCRATERS | MC2_DRAWALPHA | MC2_ISTERRAIN);
// and:
mcTextureManager->addTriangle(craterTextureIndices[1], MC2_ISCRATERS | MC2_DRAWALPHA);
mcTextureManager->addTriangle(craterTextureIndices[1], MC2_ISCRATERS | MC2_DRAWALPHA);
```

**Step 2: Replace addVertices in render() with gos_PushDecal**

In `render()` (~lines 540-552), find the block that submits `gVertex` + `sVertex`:

```cpp
// OLD — remove:
if (currCrater->craterShapeId > TURKINA_FOOTPRINT) {
    mcTextureManager->addVertices(craterTextureIndices[handleOffset], gVertex,
        MC2_ISCRATERS | MC2_DRAWALPHA | MC2_ISTERRAIN);
    mcTextureManager->addVertices(craterTextureIndices[handleOffset], sVertex,
        MC2_ISCRATERS | MC2_DRAWALPHA | MC2_ISTERRAIN);
} else {
    mcTextureManager->addVertices(craterTextureIndices[handleOffset], gVertex,
        MC2_ISCRATERS | MC2_DRAWALPHA);
    mcTextureManager->addVertices(craterTextureIndices[handleOffset], sVertex,
        MC2_ISCRATERS | MC2_DRAWALPHA);
}
```

Replace with:
```cpp
// NEW — world-space decal batch
// currCrater->position[0..3] are MC2 world-space positions (already available, no CPU projection needed)
auto makeDV = [&](int posIdx, float u, float v) -> WorldOverlayVert {
    WorldOverlayVert dv;
    dv.wx = -currCrater->position[posIdx].x;   // MC2: x=east = -MLR.x
    dv.wy =  currCrater->position[posIdx].z;   // MC2: y=north = MLR.z
    dv.wz =  currCrater->position[posIdx].y;   // MC2: z=elev  = MLR.y
    dv.u  = u;  dv.v = v;
    dv.fog = ((fogRGB >> 24) & 0xFF) / 255.0f;
    dv.argb = lightRGB;
    return dv;
};
// Map UV corners from existing craterUVTable entries
float u0 = craterUVTable[(currCrater->craterShapeId*2)];
float v0 = craterUVTable[(currCrater->craterShapeId*2)+1];
// Triangle 1: positions 0,1,2; Triangle 2: positions 0,2,3 (reconstruct from gVertex/sVertex layout)
WorldOverlayVert tri1[3] = { makeDV(0,u0,v0), makeDV(1,u0+uvAdd,v0), makeDV(2,u0+uvAdd,v0+uvAdd) };
WorldOverlayVert tri2[3] = { makeDV(0,u0,v0), makeDV(2,u0+uvAdd,v0+uvAdd), makeDV(3,u0,v0+uvAdd) };
uint32_t texH = craterTextureHandles[handleOffset];
gos_PushDecal(tri1, texH);
gos_PushDecal(tri2, texH);
```

**Coordinate space note:** `currCrater->position[i]` is in MLR/Stuff space (x=left, y=elev, z=forward). Convert to MC2 world space: `wx = -pos.x`, `wy = pos.z`, `wz = pos.y`. This matches the axis swap in `gamecam.cpp`.

**Step 3: Build + deploy**
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Deploy and verify craters visible in game via new path.

**Step 4: Commit**
```bash
git add mclib/crater.cpp
git commit -m "feat: migrate craters + footprints to gos_PushDecal"
```

---

## Task 12: Remove old Render.CraterOverlays and non-terrain crater loops from txmmgr.cpp

Both paths are now dead (nothing pushes to MC2_ISCRATERS batches any more).

**Files:**
- Modify: `mclib/txmmgr.cpp`

**Step 1: Delete Render.CraterOverlays block (~lines 1465-1520)**

Remove the entire `for(int states...)` loop that checks `MC2_ISTERRAIN && MC2_DRAWALPHA && MC2_ISCRATERS`, plus:
- `gos_SetRenderState(gos_State_Overlay, 1)` before it
- `gos_SetRenderState(gos_State_Overlay, 0)` after it
- `ZoneScopedN("Render.CraterOverlays")` and `TracyGpuZone`

**Step 2: Delete non-terrain crater loop (~lines 1534-1580)**

Remove the `for` loop checking `!(MC2_ISTERRAIN) && MC2_DRAWALPHA && MC2_ISCRATERS`, plus surrounding render state setup.

**Step 3: Also check the Render.NoUnderlayer block (~lines 1376-1443)**

This block dispatches `MC2_GPUOVERLAY` draws with `gos_State_Overlay`. In nifty-mendeleev, nothing sets `MC2_GPUOVERLAY` (that was a modest-euler workaround for solid cement). Verify by searching:
```bash
grep -n "MC2_GPUOVERLAY" mclib/*.cpp mclib/*.h
```
If only txmmgr.cpp and txmmgr.h define/reference it, the block never fires. Leave it in place for now (it is harmless when nothing sets the flag). Do NOT delete it in this task.

**Step 4: Build + deploy + verify**
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Visual check: cement tiles and craters should look identical to before (rendered only via new path now). No double-rendering.

**Step 5: Commit**
```bash
git add mclib/txmmgr.cpp
git commit -m "feat: remove Render.CraterOverlays + non-terrain crater loops from txmmgr"
```

---

## Task 13: Delete Overlay.SplitDraw from gameos_graphics.cpp

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

**Step 1: Confirm nothing routes gos_State_Overlay through the draw path anymore**

```bash
grep -n "gos_State_Overlay\|gos_SetRenderState.*Overlay" mclib/*.cpp code/*.cpp
```
Expected: only txmmgr.cpp's `Render.NoUnderlayer` block (which was confirmed harmless in Task 12). If anything else shows up, investigate before deleting SplitDraw.

**Step 2: Delete the SplitDraw block**

In `gameos_graphics.cpp`, find the block starting at ~line 2935:
```cpp
if (curStates_[gos_State_Overlay] && terrain_mvp_valid_) {
    ZoneScopedN("Overlay.SplitDraw");
    // ...~80 lines...
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
} else {
    // fallback trace...
}
```
Delete the entire `if/else` block. Also delete the helper functions used only by SplitDraw:
- `dumpOverlayBatchOnce()`
- `dumpOverlayProjectedBatchOnce()`
- `fillForcedOverlayDebugQuad()`
(Search for these function names to find their definitions.)

**Step 3: Build**
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Expected: Compiles.

**Step 4: Commit**
```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat: delete Overlay.SplitDraw — all cement/crater draws via world-space batches"
```

---

## Task 14: Delete clearOverlayAlpha from gos_postprocess.cpp

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.cpp`
- Modify: `GameOS/gameos/gos_postprocess.h`

**Step 1: Delete clearOverlayAlpha() implementation**

In `gos_postprocess.cpp`, find `clearOverlayAlpha()` (~line 611). Delete the entire function body.

**Step 2: Delete the callsite**

In `gos_postprocess.cpp`'s `endScene()` function (~line 959), find and delete:
```cpp
clearOverlayAlpha();
```

**Step 3: Delete declaration from header**

In `gos_postprocess.h`, delete the `clearOverlayAlpha()` declaration.

**Step 4: Also delete `shaders/overlay_alpha_clear.frag`** if it exists and is only used by this function:
```bash
grep -rn "overlay_alpha_clear" GameOS/ shaders/
```
If only referenced by `clearOverlayAlpha()`, delete the file.

**Step 5: Build**
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```

**Step 6: Commit**
```bash
git add GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gos_postprocess.h
git commit -m "feat: delete clearOverlayAlpha — stencil/GBuffer overlay shadow route removed"
```

---

## Task 15: Delete IS_OVERLAY variant from gos_tex_vertex shaders

**Files:**
- Modify: `shaders/gos_tex_vertex.vert`
- Modify: `shaders/gos_tex_vertex.frag`

**Step 1: Strip gos_tex_vertex.vert**

Remove everything guarded by `#ifdef IS_OVERLAY`:
- `uniform mat4 terrainMVP;`
- `uniform vec4 terrainViewport;`
- `out vec3 MC2WorldPos;`
- `out float OverlayUsesWorldPos;`
- The entire IS_OVERLAY branch in `main()` (the `if (OverlayUsesWorldPos > 0.5)` block)

The `main()` function should simplify to just the non-overlay path:
```glsl
void main(void) {
    vec4 p = mvp * vec4(pos.xyz, 1.0);
    gl_Position = p / pos.w;
    Color    = color;
    FogValue = fog.w;
    Texcoord = texcoord;
}
```

**Step 2: Strip gos_tex_vertex.frag**

Remove the `#ifdef IS_OVERLAY` block (lines ~15-89 in the current file):
- `in PREC vec3 MC2WorldPos;`
- `in PREC float OverlayUsesWorldPos;`
- The entire IS_OVERLAY conditional in `main()` (tone correction, cloud shadows, calcShadow, fog, `return`)

Also remove the include-time comment at the top referencing IS_OVERLAY stencil/GBuffer route.
Remove `uniform PREC float time;` if it is only used by the IS_OVERLAY block (check: water path also uses `time` for wave animation — if so, keep the uniform).
Remove `uniform vec4 terrainLightDir;` if only used by IS_OVERLAY.

**Step 3: Build**
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Shader compile errors are likely — the IS_OVERLAY define may have been used elsewhere. Search:
```bash
grep -rn "IS_OVERLAY" shaders/ GameOS/
```
If found in shader compiler invocations, ensure that define is no longer passed.

**Step 4: Deploy shaders**
```bash
cp -f shaders/gos_tex_vertex.vert "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_tex_vertex.vert"
cp -f shaders/gos_tex_vertex.frag "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_tex_vertex.frag"
```

**Step 5: Commit**
```bash
git add shaders/gos_tex_vertex.vert shaders/gos_tex_vertex.frag
git commit -m "feat: remove IS_OVERLAY variant from gos_tex_vertex — overlay draw path deleted"
```

---

## Task 16: Final build, deploy, and visual verification

**Step 1: Full build + full deploy**

Use `/mc2-build` skill and `/mc2-deploy` skill (see `.claude/skills/`). Or manually:
```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Deploy exe + all touched shaders:
```bash
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
for f in shaders/terrain_overlay.vert shaders/terrain_overlay.frag shaders/decal.frag \
          shaders/gos_tex_vertex.vert shaders/gos_tex_vertex.frag; do
    cp -f "$f" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/$f"
    diff -q "$f" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/$f" && echo "OK: $f" || echo "DIFF: $f"
done
```

**Step 2: In-game visual checklist**

Launch a mission with visible cement/runway area and mechs. Verify in order:

1. **Cement perimeter tiles** — visible, correct tone, match interior runway colour
2. **No double-rendering** — cement should appear clean (not over-bright from double draw)
3. **Cloud shadows on cement** — should match intensity of cloud shadows on interior terrain (full range, no narrowing hack)
4. **Mech/building shadows on cement** — shadow should fall correctly on perimeter tiles
5. **Crater visible** — bomb craters appear on terrain
6. **Crater shadows** — craters in shadow of buildings are darker
7. **Footprints visible** — mech footprint decals appear
8. **No stencil artifacts** — shadow debug overlay (RAlt+F2) should show overlays classified correctly
9. **Mission markers unaffected** — no shadow darkening, no visual change from prior behaviour
10. **Water unaffected** — water tiles render as before (non-overlay gos_tex_vertex path unchanged)

**Step 3: If anything is wrong**

- Cement invisible → check terrainMVP/viewport/mvp uniform uploads in `gos_DrawTerrainOverlays()`
- Cement wrong colour → tone correction in `terrain_overlay.frag` — compare with IS_OVERLAY path that was deleted
- Shadows missing → check `gos_SetupObjectShadows(prog)` call, check terrainLightDir upload
- GL errors in console → check VAO attribute setup (stride, offsets, types)
- Shader won't compile → CLAUDE.md rule: bad compile = old shader stays active. Check console for errors.

**Step 4: Final commit**
```bash
git add -u
git commit -m "feat: world-space overlay rewrite complete — cement, craters, footprints on GPU batches

Replaces IS_OVERLAY / rhw=1.0 / terrainMVP workaround stack with typed
WorldOverlayVert buffers and dedicated draw calls for alpha cement tiles,
bomb craters, and mech footprints. Eliminates Overlay.SplitDraw,
clearOverlayAlpha, and the broken stencil/GBuffer shadow route.
Full-range cloud shadows on cement tiles now match interior terrain.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Quick Reference: Files Changed

| File | Change |
|------|--------|
| `GameOS/gameos/gameos.hpp` | Add `WorldOverlayVert`, `gos_PushTerrainOverlay`, `gos_PushDecal`, `gos_DrawTerrainOverlays`, `gos_DrawDecals` |
| `GameOS/gameos/gameos_graphics.cpp` | Add batch structs, push/draw functions, shader loading, VAOs; delete `Overlay.SplitDraw` + helpers |
| `GameOS/gameos/gos_postprocess.cpp` + `.h` | Delete `clearOverlayAlpha` |
| `mclib/quad.cpp` | Replace `setOverlayWorldCoords` + `addVertices(MC2_ISCRATERS)` with `gos_PushTerrainOverlay`; remove `addTriangle(MC2_ISCRATERS)` |
| `mclib/crater.cpp` | Replace `addVertices(MC2_ISCRATERS)` with `gos_PushDecal`; remove `addTriangle(MC2_ISCRATERS)` |
| `mclib/txmmgr.cpp` | Remove `Render.CraterOverlays` loop + non-terrain crater loop + `gos_State_Overlay` calls |
| `shaders/terrain_overlay.vert` | New |
| `shaders/terrain_overlay.frag` | New |
| `shaders/decal.frag` | New |
| `shaders/gos_tex_vertex.vert` | Remove IS_OVERLAY variant |
| `shaders/gos_tex_vertex.frag` | Remove IS_OVERLAY variant |
| `shaders/overlay_alpha_clear.frag` | Delete |
