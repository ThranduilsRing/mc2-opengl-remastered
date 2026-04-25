# RTS Visual Effects Design — MC2 OpenGL
**Date:** 2026-04-24  
**Scope:** TAA, 3D LUT, Aerial Perspective, Vignette, Unit Outline, Biome Fog  
**Reference:** CoH3 / AoE4 / SC2 Remastered post-FX philosophy — targeted per-camera effects only  

---

## 0. Design Philosophy

**Design target:** 4K (3840×2160) on a 42" OLED at RX 7900 XTX. 1080p on mid-range hardware is expected to work, not expected to be the reference point. When a tradeoff comes up (default sizes, quality vs cost, "affordable" threshold), bias toward what reads well at 4K on that display. Lower resolutions get a scaled-down version that is good enough, not the other way around.

This renderer serves a fixed-overhead RTS camera. The standard AAA post-FX catalog is the wrong reference. The correct reference is what CoH3, AoE4, and SC2 Remastered actually ship: TAA + LUT + outline + subtle aerial perspective — nothing else from the catalog.

**What this spec adds:**
1. TAA (replaces FXAA — solves subpixel aliasing on tessellated terrain + POM)
2. 3D LUT (replaces ACES + hardcoded warm grade — per-mission art direction)
3. Aerial perspective lite (terrain frag shader, ~20 lines — sells world scale)
4. Vignette (clean, uniform-driven — replaces the tangled version in composite)
5. Unit outline + selection glow (replaces "circle under unit" — biggest visual upgrade)
6. Biome-tinted depth fog (post-process fullscreen — complements aerial perspective)

**What this spec explicitly removes:**
- SSAO (wrong for RTS camera; disabled but still consuming init cost)
- God rays (removed — wrong camera, invisible at RTS zoom)
- ACES filmic tonemapper (designed for HDR scene-referred input; our pipeline is sRGB-authored)
- Hardcoded sunset filter in `postprocess.frag` (lines 99–121 — replaced by LUT)
- FXAA (replaced by TAA)

**What this spec does NOT add:**
- Screen-space reflections, SSGI, DOF, chromatic aberration, lens flares — wrong camera
- Bloom tuning — wrong tool; bloom infrastructure stays but is effectively off

---

## 1. Current Pipeline State (Baseline)

### Relevant infrastructure confirmed by code audit:

**FBOs:**
- `sceneFBO_`: RGBA16F color (attachment 0), RGBA16F normals (attachment 1, terrain-only — AMD constraint), D24S8 depth+stencil
- `bloomFBO_[2]`: RGBA16F half-res ping-pong
- `ssaoFBO_` / `ssaoBlurFBO_`: R16F half-res (DISABLED — reusable)
- `godrayFBO_`: RGBA16F half-res (DISABLED — reusable)
- `shadowFBO_` / `dynShadowFBO_`: D24 4096² (active)

**Post-process pass order (current):**
```
shadow_screen → shoreline → SSAO(disabled) → godray(disabled) → bloom → composite(ACES+FXAA)
```

**Composite shader state (`postprocess.frag`):**
- ACES filmic tonemapper on `enableTonemap=0` (disabled, passthrough)
- Hardcoded sunset filter: warm grade + 18%-corner vignette + top-of-screen glow (lines 99–121)
- FXAA: `enableFXAA` flag, compiled in but disabled
- Bloom: `enableBloom` flag, disabled

**Jitter / TAA / LUT / Outline:** None. Clean slate.

**Stencil buffer:** Allocated (D24S8) — currently unused. Available for new passes.

**AMD RX 7900 XTX constraints that shape this design:**
- `usampler2D` crashes shader compile → cannot read stencil index as integer sampler
- `glTextureView` for stencil extraction is unreliable → avoid GL_STENCIL_INDEX views
- Writing non-terrain fragments to attachment 1 corrupts color output → new passes get their own FBOs with single attachment
- No feedback loops (read+write same texture in one draw) → TAA ping-pong must swap indices correctly

**Key code path for jitter injection:**
- `gameos_graphics.cpp:setTerrainMVP()` is the single point where the VP matrix is set on the render thread and forwarded to `pp->setViewProj(m)`. TAA jitter is applied here.

---

## 2. Effect 1 — TAA (Temporal Anti-Aliasing)

### Why TAA wins here

Tessellated terrain generates subpixel geometric detail that moves each frame as the camera rotates. POM offsets that detail further. High-frequency splat normals produce aliased specular flicker. FXAA only blurs existing frame edges and does nothing for temporal subpixel shimmer. MSAA is expensive on the deferred path. TAA with a narrow history blend is the correct and targeted solution.

### Architecture

**New textures:**
- `taaHistoryTex_[2]`: RGBA16F, full-res, ping-pong. Each holds one accumulated frame.
- No velocity buffer required for Phase 1 — camera-only reprojection derives motion vectors from `prevViewProj` and `currViewProj` via depth reconstruction in the resolve shader.

**New FBOs:**
- `taaFBO_[2]`: each with single color attachment `taaHistoryTex_[i]`

**New shader:** `shaders/taa_resolve.frag`

**New uniform on postprocess object:**
- `prevViewProj_[16]`: previous frame's un-jittered VP matrix, swapped each frame
- `taaEnabled_`: bool toggle (RAlt key TBD)
- `taaAlpha_`: float, default 0.15 (history weight = 0.85, current = 0.15; tune down to 0.10 if ghosting is acceptable)
- `taaJitterIdx_`: int, cycles 1–8 (Halton sequence; **starts at 1, not 0** — Halton(2,3) at index 0 returns (0,0), which produces a no-jitter frame that contaminates the history with an unjittered sample)

### Jitter Injection

Halton(2,3) sub-pixel offsets, 8-frame sequence (indices 1–8). Applied in `setTerrainMVP()`:

```
// Conceptual — not implementation code
// halton[] is a precomputed static table of 8 vec2 values (Halton base-2/base-3),
// indexed 1–8 (index 0 is (0,0) — unused). Values are in [0,1].
// Subtract 0.5 to center on zero, then scale to ±1 NDC pixel.
// Without the -0.5, offsets are always positive and produce a persistent
// sub-pixel image shift rather than symmetric dithering.
static const vec2 halton[9] = { {0,0}, {0.5,0.333}, {0.25,0.667}, {0.75,0.111},
    {0.125,0.444}, {0.625,0.778}, {0.375,0.222}, {0.875,0.556}, {0.0625,0.889} };
vec2 jitter = (halton[taaJitterIdx_] - 0.5) * 2.0 / vec2(screenW, screenH);
mat4 jitteredProj = applyJitterToProjection(cleanVP, jitter);
// upload jitteredProj to GPU (this is what terrain sees)
// store cleanVP as currViewProj_ for TAA reprojection math
```

**Critical:** The shadow cameras are never jittered. `shadowFBO_` and `dynShadowFBO_` always use the clean un-jittered matrices. Only the main scene view gets jitter.

**Critical:** `pp->setViewProj(m)` receives the CLEAN (un-jittered) VP matrix for use in post-process depth reconstruction. TAA resolve receives BOTH jittered (via sceneDepthTex_) and clean VP matrices.

### TAA Resolve Shader Logic (`taa_resolve.frag`)

```
Inputs:
  sampler2D sceneTex       // current jittered frame (unit 0)
  sampler2D historyTex     // previous accumulated frame (unit 1)
  sampler2D depthTex       // current depth, D24S8 (unit 2)
  mat4 inverseViewProj     // clean current VP inverse (same name as in shadow_screen and fog passes)
  mat4 prevViewProj        // clean previous frame VP
  float taaAlpha           // 0.15

Per pixel:
  1. Sample current color C at UV
  2. Compute motion vector:
       ndcPos  = vec4(uv*2-1, sampleDepth(uv)*2-1, 1)
       worldP  = (inverseViewProj * ndcPos).xyz/w
       prevNDC = prevViewProj * vec4(worldP, 1)
       prevUV  = prevNDC.xy / prevNDC.w * 0.5 + 0.5
  3. Sample history H at prevUV (bilinear; if out-of-bounds: use C, skip blend)
  4. Neighborhood AABB clamp (3×3 around current UV):
       Compute min/max of 9 neighbors in YCoCg space
       Clamp H into that AABB → H_clamped
  5. Blend: result = mix(H_clamped, C, taaAlpha)
  6. Write result to taaHistoryTex_[currIdx]
```

YCoCg neighborhood clamp is more robust than RGB AABB for temporal stability. The clamp is the ghost-rejection mechanism — fast-moving edges in the current frame update the history AABB, forcing history samples outside the local color envelope to be pulled in before blending.

**Jitter/depth approximation note:** The depth buffer is rasterized by the *jittered* projection, but `currViewProjInv` is the inverse of the *clean* (unjittered) projection. The reconstructed `worldP` is therefore slightly off by the current frame's subpixel jitter. Similarly, `prevViewProj` is the clean previous-frame VP, but history was accumulated under the previous frame's jitter. The net error is sub-pixel and is absorbed by the neighborhood AABB clamp. This is the same approximation most shipping TAA implementations use. If future debugging reveals residual shimmer on static geometry, the fix is to store and subtract the per-frame jitter offset in the history sample UV: `prevUV -= prevJitter / screenSize`.

**Disocclusion handling:** If `prevUV` lands outside [0,1], the history sample is invalid (camera moved far). Use `taaAlpha = 1.0` for that pixel (accept current frame directly with no history blend). This avoids ghosting at edges of large camera pans.

**Known failure mode — fast objects:** A fast-moving mech that crosses more than ~1 pixel per frame reveals terrain behind it within the same frame. The newly-revealed terrain pixel's `prevUV` lands inside [0,1] but points at mech-body color in the history. The neighborhood AABB clamp catches this when the mech's color doesn't match the local terrain color envelope — which it usually won't. This fails (produces a trailing smear) when the mech color is close to the background, which is rare at RTS zoom. Per-object velocity buffers (Phase 2) solve this correctly by providing an exact motion vector for each pixel rather than relying on camera reprojection.

### Pipeline Position

```
shadow_screen → shoreline → [TAA resolve] → fog → outline blend → bloom → composite
```

TAA runs after shadow_screen and shoreline (so shadows and shoreline foam are temporally stable) and before bloom (so bloom applies to the stable image, not to jitter noise).

After TAA, the **working scene texture** (`workingSceneTex`) is `taaHistoryTex_[currIdx]`. Each subsequent pass that produces a new color buffer advances `workingSceneTex` to its output:

| After pass | `workingSceneTex` |
|------------|------------------|
| Scene render | `sceneColorTex_` |
| TAA resolve (enabled) | `taaHistoryTex_[currIdx]` |
| Fog pass (enabled) | `fogOutputTex_` |
| (fog disabled, TAA enabled) | `taaHistoryTex_[currIdx]` |
| (both disabled) | `sceneColorTex_` |

The final composite always reads `workingSceneTex` — whatever texture last held the fully-processed scene color. Bloom threshold reads the same `workingSceneTex` so bloom is always extracted from the post-fog image, not the pre-fog image.

**`endScene()` skeleton — required ownership pattern:**

```cpp
// The current post-process color source. Each pass may replace this
// with a newly written texture; no pass may read and write the same texture.
GLuint workingSceneTex = sceneColorTex_;

if (taaEnabled_) {
    runTAA(workingSceneTex, taaHistoryTex_[prev], depthTex_, taaFBO_[curr]);
    workingSceneTex = taaHistoryTex_[curr];
}

if (fogEnabled_) {
    runFog(workingSceneTex, depthTex_, fogFBO_);
    workingSceneTex = fogOutputTex_;
}

runBloom(workingSceneTex);        // reads workingSceneTex, writes bloomFBO_
runOutline(classTex_, outlineFBO_);
runComposite(workingSceneTex, outlineTex_, bloomColorTex_[0], lutTex_);
```

The ownership comment is load-bearing. Any future pass that attempts to read and write the same texture is a feedback loop — the pattern catches it at code-review time rather than as a visual artifact.

### Alpha Tuning and Fast-Moving Units

Default `taaAlpha_ = 0.15` (85% history). At 60fps, ghosting tails decay to ~10% after ~14 frames (~230ms). Fast-turning mechs will smear if alpha is too low (history too dominant). Tune upward from 0.15, not downward from 0.10.

**Velocity-weighted alpha (Phase 2 addition):** When the neighborhood AABB clamp has to pull the history sample a large distance (clamped a lot), that's a cheap proxy for disocclusion or fast motion. In those pixels, raise `taaAlpha` toward 1.0 to weight the current frame more heavily. This reduces ghosting on moving units without changing the static-geometry stability. Requires only one extra `length(H_clamped - H)` comparison in the resolve shader.

### SMAA T2x Fallback Note

If TAA ghosting proves unacceptable with camera-only reprojection (e.g., mechs fast-moving at close zoom), SMAA T2x is the fallback. It uses the same 2-frame jitter infrastructure and prevViewProj, but blends only 2 frames (0.5/0.5) with no neighborhood clamping. Less stable but zero ghosting since history is discarded after 1 frame. The jitter + matrix management infrastructure is identical — only the resolve shader logic changes.

### Toggle

`RAlt+T` (suggested). Adds to existing debug hotkey map. When disabled, TAA resolve pass is skipped and subsequent passes read `sceneColorTex_` directly (same as current behavior).

---

## 3. Effect 2 — 3D LUT Color Grade

### Why LUT replaces ACES

ACES is designed for scene-referred HDR input with physical exposure values (photography). MC2's textures are SDR-authored: artist-painted palette at ~0.5 average luminance, with a few emissive bright spots. ACES on SDR input mostly just adds contrast and a blue-shadow push that reads as "filmic" but fights the game's intended palette. A 3D LUT encodes both tonemapping curve and color grade in a single artistdirected 33³ lookup — same cost, full control.

The existing "sunset filter" in `postprocess.frag` (lines 99–121) is a manually-coded approximation of what a LUT does, but it's hardcoded and mission-agnostic. The LUT replaces it entirely.

### Architecture

**New texture:**
- `lutTex_`: `GL_TEXTURE_3D`, 33×33×33, `GL_RGB8`, `GL_LINEAR` filtering on all axes, `GL_CLAMP_TO_EDGE`
- Memory: 33³ × 3 bytes = ~108 KB per LUT (negligible)

**Sampling convention:**
- R is the fastest-varying axis (inner loop), B is slowest (outer loop) — standard .cube convention
- Uploaded with: `glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB8, 33, 33, 33, 0, GL_RGB, GL_FLOAT, data)`
- B=0 slice at depth=0, B=32 at depth=1.0 (GL_LINEAR handles this correctly)

**Identity LUT:** Generated procedurally at init as fallback (no file I/O required). Each entry `(r,g,b) = (R/32, G/32, B/32)` — passthrough.

### .cube File Parser

Minimal parser in `gos_postprocess.cpp`, function `bool loadLUT(const char* path)`:

```
Parse rules:
  - Skip lines starting with '#' (comment)
  - Skip blank lines
  - Parse 'LUT_3D_SIZE N' header; validate N==33
  - Read N³ lines of "R G B" floats (range 0.0–1.0)
  - Values ordered: R inner, G middle, B outer (r-minor, b-major)
  - Upload to GL_TEXTURE_3D
  - On failure: keep current LUT (no visual glitch)
```

### Per-Mission LUT Selection

**Directory:** `data/luts/` (loose files, same override priority as textures)

**Shipped LUTs:**
- `identity.cube` — passthrough (used as fallback)
- `desert_warm.cube` — warm highlights, slightly de-saturated shadows, faint sand tint
- `arctic_cool.cube` — blue-grey cast, boosted contrast, cold shadows
- `urban_grey.cube` — desaturated mid-tones, slight green-grey industrial cast
- `default.cube` — warm but neutral, replaces the existing hardcoded sunset filter tone

**Selection API (new on gosPostProcess):**
- `void setLUT(const char* lutName)` — loads `data/luts/<lutName>.cube`, falls back to identity
- `void setLUTBlend(float t)` — blend factor for crossfade between two LUTs (mission transitions)
- Called from mission startup code, same site where other per-mission uniforms are set

**LUT crossfade:** Optional for v1. If implemented: hold two `lutTex_[2]`, blend in shader `mix(texture(lut0, uvw), texture(lut1, uvw), blendT)`. Add as Phase 2.

### Composite Shader Changes

The existing `postprocess.frag` changes:

**Remove:** Lines 99–121 (entire sunset filter block: warm grade, vignette, top-of-screen glow)  
**Remove:** `ACESFilm()` function and its invocation  
**Remove:** `enableTonemap` uniform (no longer needed — LUT handles tonemapping)  

**Add:**
```glsl
uniform sampler3D lutTex;       // unit 2: 3D LUT
uniform float vignetteStrength; // separate, clean vignette
uniform float vignetteInner;    // default 0.25
uniform float vignetteOuter;    // default 0.65

// LUT lookup (replaces ACES + sunset filter)
// Texel-center correction: color=0 maps to edge (0.0), not center (0.5/N).
// Without this, the LUT squashes ~3% of dynamic range at each extreme,
// making in-engine output look slightly off from Resolve/Photoshop preview.
const float N = 33.0;
vec3 lutUVW = clamp(color * exposure, 0.0, 1.0) * ((N - 1.0) / N) + 0.5 / N;
vec3 gradedColor = texture(lutTex, lutUVW).rgb;

// Clean vignette (separate from grade)
vec2 vc = TexCoord - 0.5;
float vd = length(vc);
float vignette = 1.0 - smoothstep(vignetteInner, vignetteOuter, vd);
gradedColor *= mix(1.0 - vignetteStrength, 1.0, vignette);
```

The LUT samples the scene color (post-exposure, pre-bloom-add) for the grade, then bloom is added after. This keeps bloom from being double-graded through the LUT.

**LUT is SDR-only:** Input is clamped to [0,1] before the lookup. Any HDR content above 1.0 (emissive muzzle flashes, weapon bolts, bright particle effects) is hard-clipped before the grade. This is correct for our SDR-authored pipeline. Any future HDR emissive contributors must add their contribution *after* the LUT lookup in the composite shader — same as bloom does today — not before it.

### Bloom Demotion

With LUT, the bloom threshold becomes more predictable (LUT controls output range). Bloom intensity drops to near-zero for the RTS camera (emissives only: mech exhaust, weapon fire). The bloom infrastructure stays in place but `bloomIntensity_` defaults to 0.05 or 0.0.

---

## 4. Effect 3 — Aerial Perspective Lite

### Why this effect

At RTS zoom the terrain visible extent is 1000+ world units. Without atmospheric scattering, distant terrain reads at the same color intensity and saturation as near terrain, flattening the sense of depth. A simple desaturate + warm/cool push based on horizontal distance reads as "the world is bigger" at zero post-process cost.

### Implementation Location

In `gos_terrain.frag`, after the PBR splat computation and before the final `fragColor` write.

### Shader Addition

```glsl
// New uniforms (add to gos_terrain.frag uniform block or include/scene.hglsl)
uniform float aerialNear;       // start distance (default 200.0 world units)
uniform float aerialRange;      // falloff range (default 600.0)
uniform float aerialDesatStr;   // max desaturation (default 0.15)
uniform float aerialTintStr;    // max tint blend (default 0.10)
uniform vec3  aerialColor;      // haze color (per-mission; default vec3(0.72, 0.68, 0.60))

// Aerial perspective term (inserted after PBR, before final output)
float horizDist = length(worldPos.xz - camPos.xz);
float aerialFactor = smoothstep(aerialNear, aerialNear + aerialRange, horizDist);
// Desaturation
float lum = dot(albedo, vec3(0.299, 0.587, 0.114));
albedo = mix(albedo, vec3(lum), aerialFactor * aerialDesatStr);
// Haze color push
albedo = mix(albedo, aerialColor, aerialFactor * aerialTintStr);
```

`worldPos` is already computed in `gos_terrain.frag` for POM and shadow sampling. `camPos` is already a uniform (used for LOD distance). No new vertex attributes, no new passes.

### Also in `gos_tex_vertex.frag`

The same term should be added to the world-overlay branch (cement, roads) so overlay tiles don't visually pop out against the aerial-haze terrain. Same uniforms, same formula, triggered only in the overlay branch where `worldPos` is available.

### Aerial Perspective on Units and Buildings — Intentional Omission

The aerial term is added to terrain and overlays only, not to the mech/building object shaders (`gos_tex_vertex_lighted.frag`, `static_prop.frag`). This is an intentional gameplay decision: distant enemies and buildings should remain visually distinct against the haze rather than fading into it. Unit readability at range is more important than physical accuracy. The post-process fog pass (§7) provides a consistent depth veil over *everything* including objects, which covers the coarser version of the atmospheric effect. If future playtesting shows buildings pop unacceptably against highly hazy terrain, add the aerial term to `static_prop.frag` first (buildings are large and benefit most), then evaluate mechs.

### Per-Mission Values

`aerialColor` and `aerialDesatStr` vary per mission biome:
- Desert: `aerialColor = (0.78, 0.70, 0.58)`, `aerialDesatStr = 0.18` — warm sandy haze
- Arctic: `aerialColor = (0.70, 0.76, 0.85)`, `aerialDesatStr = 0.20` — cool blue-grey
- Urban: `aerialColor = (0.68, 0.68, 0.65)`, `aerialDesatStr = 0.12` — neutral grey

Set at mission startup via `gosRenderer::setAerialParams(...)`.

---

## 5. Effect 4 — Vignette (Clean)

### Current State

A vignette already exists in `postprocess.frag` lines 112–116 (`smoothstep(0.85, 0.25, vdist)`, mixed with aspect-ratio ellipse `vec2(1.0, 0.6)`, result blended `mix(0.82, 1.0, vignette)` — 18% max corner darkening). It is tangled with the sunset filter warm grade and the top-of-screen glow, making it impossible to tune independently.

### Design

Remove the entire sunset filter block. Add a clean, independently-tunable vignette uniform in its place:

```glsl
uniform float vignetteStrength; // 0.0–0.3, default 0.08 (8%)
uniform float vignetteInner;    // start radius, default 0.25
uniform float vignetteOuter;    // full-dark radius, default 0.65

vec2  vc  = TexCoord - 0.5;            // center at 0,0
float vd  = length(vc);               // circular vignette
float vf  = smoothstep(vignetteInner, vignetteOuter, vd);
color    *= (1.0 - vignetteStrength * vf);
```

**Why circular (not aspect-corrected):** At RTS zoom the action center is approximately centered. Circular vignette pulls all four corners equally, which is the correct RTS behavior. Cinematic vignettes are aspect-corrected for narrative framing — irrelevant here.

**8% strength:** Enough to gently pull the eye toward center, not enough to be visible as a style choice. Increase to 12% if the corner areas are bright (desert sand).

---

## 6. Effect 5 — Unit Outline + Selection Glow

### Why this is the highest-priority visual upgrade

The outline pass is the RTS-native equivalent of bloom. SC2, CoH3, and CoH2 all ship outlines on units. It solves three problems simultaneously: unit readability at any zoom level, faction identification without UI cluttering, and selection feedback without the 2001-era "circle underfoot." The existing G-buffer infrastructure makes this achievable without changes to the object management system.

### Architecture Overview

Two passes:
1. **Classification pre-pass:** Render mech/object geometry to a classification texture with faction ID
2. **Outline resolve pass:** Edge-detect classification discontinuities, output per-faction colored outlines

Both passes run before the main lit scene render (classification) and after TAA (outline blend).

### Classification Pass

**New texture:** `classTex_` — `GL_R8`, full-res, `GL_NEAREST` filtering (no interpolation of class IDs)  
**New FBO:** `classFBO_` — color attachment `classTex_` **plus a D24 depth renderbuffer**  
**Clear value:** color=0, depth=1.0

**Why a depth attachment is required:** Without a depth attachment, `glEnable(GL_DEPTH_TEST)` is a no-op (depth testing requires a depth buffer in the bound FBO). Without depth testing, every fragment that touches a pixel writes its class ID regardless of occlusion order, so a mech's back-face and a building's interior fragments all write over each other. The depth attachment ensures only the frontmost surface per pixel survives — which is what makes the "only frontmost surface gets classified" claim in this spec true.

**Depth attachment options (choose one):**
- *(Preferred)* Allocate a dedicated D24 renderbuffer for `classFBO_`. ~Same size as a full-res depth texture. No sharing concerns.
- *(Alternative)* Render the classify pass *after* main scene depth is established, then blit `sceneDepthTex_` as a read-only depth source. More complex (requires `GL_DEPTH_ATTACHMENT` from an external texture on an FBO), skip for Phase 1.

**Faction encoding (0–5 in GL_R8 normalized storage):**

| Value | Normalized | Meaning |
|-------|-----------|---------|
| 0 | 0.000 | background (terrain, sky) |
| 1 | 1/255 | friendly unit |
| 2 | 2/255 | hostile unit |
| 3 | 3/255 | neutral unit |
| 4 | 4/255 | selected friendly |
| 5 | 5/255 | selected hostile / targeted |

**Why GL_R8 (not stencil):** The AMD RX 7900 XTX driver does not support reading stencil index via `usampler2D` (driver bug confirmed in `docs/amd-driver-rules.md` and `memory/overlay_shadow_session.md`). Reading the D24S8 stencil plane requires `glTextureView` with `GL_STENCIL_INDEX` format, also unreliable on this driver. GL_R8 with a regular `sampler2D` reads cleanly with `.r * 255.0` rounding.

**New shader pair:** `shaders/classify_object.vert` / `shaders/classify_object.frag`

```
classify_object.vert:
  in vec3 inPosition;
  uniform mat4 mvp;        // same MVP uploaded for the normal draw
  void main() { gl_Position = mvp * vec4(inPosition, 1.0); }

classify_object.frag:
  layout(location=0) out float classOut;
  uniform float classValue;   // faction byte / 255.0
  void main() { classOut = classValue; }
```

**CPU injection point:**

In `gameos_graphics.cpp`, alongside the existing `drawShadowObjectBatch()` pattern, add `drawClassifyBatch()`:

```
// In gos_postprocess.cpp — called ONCE at the start of pipeline step 3:
void gosPostProcess::beginClassifyPass() {
    glBindFramebuffer(GL_FRAMEBUFFER, classFBO_);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    classifyProg_->apply();    // bind program once
}

// In gameos_graphics.cpp — called per-batch, classFBO_ already bound:
void gosRenderer::drawClassifyBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix,
    int factionId)
{
    // classFBO_ already bound — just update factionId uniform and draw
    glUniform1f(classValueLoc_, factionId / 255.0f);
    glUniformMatrix4fv(mvpLoc_, 1, GL_FALSE, worldMatrix);
    // draw position-only geometry
}

// In gos_postprocess.cpp — called ONCE after all batches:
void gosPostProcess::endClassifyPass() {
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);  // restore
}
```

**Critical:** `classFBO_` is bound once for the entire classification block, not per-batch. With potentially hundreds of object batches per frame, per-batch FBO bind/unbind would add hundreds of driver state-change round-trips. The bind happens at pipeline step 3, all object batches render in sequence, then `sceneFBO_` is restored for the main scene pass.

**Position attribute only:** The classify shader reads only position. If the existing vertex declaration includes position at location 0 (confirmed by `docs/vertex-formats.md`), no new VBO or vertex format is needed.

**Depth testing during classification:** Enabled (same near/far as main scene). Only the frontmost surface per pixel gets classified — correct behavior for outline edge detection.

**Known Phase 1 limitation — same-class adjacent units merge visually:**

The edge test (`c != cN || ...`) fires only on class *value* changes. Two adjacent friendly mechs both write `class=1`; their shared border has `c == cN` on all sides — no edge detected, no outline between them. Units touching will read as a single silhouetted blob.

**Phase 2 fix (choose one):**
- *(Preferred — RG8 approach)* Expand `classTex_` to `GL_RG8`: R channel = faction class (0–5), G channel = per-unit object ID (1–255, 0=background). Edge test becomes `rClass != rNeighborClass || gID != gNeighborID`. 256 unique object IDs is sufficient for MC2 unit counts. No new FBO needed — update format and both shader sides.
- *(Free — depth-discontinuity approach)* Add a second depth texture read from `classFBO_`'s own depth buffer in `outline.frag`. Where adjacent pixels have the same class but a significant depth gap, emit an outline. Requires binding classFBO_'s depth as a second sampler in the outline pass. Only catches 3D separation (units at different elevations), not coplanar units.

Document Phase 1 limitation in release notes; ship Phase 2 before RTS multiplayer where unit pileups are common.

**Occlusion intent — outlines through terrain/buildings:** The classify pass renders *only* mech/unit geometry. Terrain and buildings are not drawn into `classFBO_`. This means an enemy mech behind a building is classified at its visible silhouette but the building surface is written as depth=0 (sky). Specifically: the building would not occlude the mech's classification because the building was never drawn into classFBO_'s depth buffer. The outline resolve will therefore draw outlines on occluded mechs — an X-ray effect.

**This is an intentional Phase 1 decision:** tactical readability at RTS zoom outweighs physical occlusion for unit outlines. SC2 does not outline through terrain; CoH3 does. Either can be correct. To get SC2-style occlusion-aware outlines in Phase 2: draw terrain/buildings with `classValue=0` into `classFBO_` (just their depth, no color write needed) so their depth blocks mech classification behind them. Alternatively, depth-test the outline resolve against `sceneDepthTex_` and discard outline pixels where scene depth is closer than the classify sample.

### Outline Resolve Pass

**New texture:** `outlineTex_` — `GL_RGBA8`, full-res (RGBA for colored outlines with alpha for blending)  
**New FBO:** `outlineFBO_` — single color attachment `outlineTex_`  
**Clear:** (0, 0, 0, 0) each frame

**New shader:** `shaders/outline.frag` (paired with existing `postprocess.vert`)

```glsl
uniform sampler2D classTex;        // unit 0: classification texture
uniform vec2 invScreenSize;        // 1/w, 1/h
uniform float outlineWidth;        // default 1.0 pixel

const vec3 colorFriendly  = vec3(0.20, 0.50, 1.00);
const vec3 colorHostile   = vec3(1.00, 0.25, 0.10);
const vec3 colorNeutral   = vec3(1.00, 0.85, 0.10);
const vec3 colorSelected  = vec3(1.00, 1.00, 1.00);  // white, faction-tinted below
const vec3 colorTargeted  = vec3(1.00, 0.40, 0.00);

vec3 classToColor(float c) {
    if (c < 1.5) return colorFriendly;
    if (c < 2.5) return colorHostile;
    if (c < 3.5) return colorNeutral;
    if (c < 4.5) return colorSelected;
    return colorTargeted;
}

void main() {
    float w = outlineWidth;
    float c  = round(texture(classTex, TexCoord).r * 255.0);
    float cN = round(texture(classTex, TexCoord + vec2(0,  w) * invScreenSize).r * 255.0);
    float cS = round(texture(classTex, TexCoord + vec2(0, -w) * invScreenSize).r * 255.0);
    float cE = round(texture(classTex, TexCoord + vec2( w, 0) * invScreenSize).r * 255.0);
    float cW = round(texture(classTex, TexCoord + vec2(-w, 0) * invScreenSize).r * 255.0);

    bool onEdge = (c != cN || c != cS || c != cE || c != cW);
    float maxClass = max(max(c, cN), max(max(cS, cE), cW));

    if (!onEdge || maxClass < 0.5) {
        // Interior pixel or background — no outline
        // For selection glow: also check 2px radius
        fragColor = vec4(0.0);
        return;
    }

    // Classify by the non-background neighbor
    float cls = (c > 0.5) ? c : maxClass;
    vec3 outlineColor = classToColor(cls);

    // Selected units get a wider soft glow (2px outer ring at 40% alpha)
    float alpha = (cls > 3.5) ? 1.0 : 0.90;
    fragColor = vec4(outlineColor, alpha);
}
```

**Selection glow extension:** The same shader (single draw, no second sub-pass) adds 8 more taps at 2-pixel radius for selected units. A pixel that has no 1px neighbor edge but has a 2px neighbor with `cls >= 3.5` (selected class) outputs at reduced alpha — the outer glow ring. Total 12 taps per pixel for selected-unit pixels, 4 taps otherwise.

```glsl
// This code REPLACES the early-return body:
//   if (!onEdge || maxClass < 0.5) { fragColor = vec4(0.0); return; }
// becomes:
if (!onEdge || maxClass < 0.5) {
    // Check 2px ring for selection glow before giving up
float cls2 = 0.0;
cls2 = max(cls2, round(texture(classTex, TexCoord + vec2(0,    2.0*w) * invScreenSize).r * 255.0));
cls2 = max(cls2, round(texture(classTex, TexCoord + vec2(0,   -2.0*w) * invScreenSize).r * 255.0));
cls2 = max(cls2, round(texture(classTex, TexCoord + vec2( 2.0*w, 0)   * invScreenSize).r * 255.0));
cls2 = max(cls2, round(texture(classTex, TexCoord + vec2(-2.0*w, 0)   * invScreenSize).r * 255.0));
cls2 = max(cls2, round(texture(classTex, TexCoord + vec2( w,  w) * invScreenSize).r * 255.0));
cls2 = max(cls2, round(texture(classTex, TexCoord + vec2(-w,  w) * invScreenSize).r * 255.0));
cls2 = max(cls2, round(texture(classTex, TexCoord + vec2( w, -w) * invScreenSize).r * 255.0));
cls2 = max(cls2, round(texture(classTex, TexCoord + vec2(-w, -w) * invScreenSize).r * 255.0));

    if (cls2 > 3.5 && c < 0.5) {
        // Outer glow ring: 2px neighbor is selected, current pixel is background
        fragColor = vec4(classToColor(cls2), 0.40);
        return;
    }
    fragColor = vec4(0.0);  // no outline, no glow
    return;
}
```

The 8 diagonal taps use `vec2(w, w)` etc. (not `vec2(2w, 0)`), producing a smooth octagonal glow ring rather than a cross-shaped artifact. 12 taps total per pixel is cheap on any modern GPU.

### Composite Blend

In `postprocess.frag`, after LUT grade and before vignette:

```glsl
uniform sampler2D outlineTex;   // unit 3

vec4 outline = texture(outlineTex, TexCoord);
color = mix(color, outline.rgb, outline.a);  // alpha-blend outline over scene
```

Outline is blended AFTER the LUT so it isn't color-graded (faction colors are absolute gameplay signals, not art-directed).

### Pipeline Position

```
[classification pre-pass → classFBO_]
↓ main scene render
shadow_screen → shoreline → TAA → fog → [outline.frag → outlineTex_] → bloom → composite
```

Composite blends `outlineTex_` over the final color after LUT grade.

### Debug Toggle

`RAlt+8` (suggested, currently unbound). Skips both classification and outline passes when off.

---

## 7. Effect 6 — Biome-Tinted Depth Fog

### Design

A gentle linear depth-based fog that applies to everything (terrain, objects, overlays) uniformly via a single post-process pass. Complements the per-terrain aerial perspective term: aerial perspective desaturates/tints the terrain surface itself; fog adds a thin atmospheric veil over all geometry at distance.

**Key distinction from aerial perspective:**
- Aerial perspective: modifies albedo in `gos_terrain.frag` — affects the surface color before lighting
- Fog: post-process fullscreen pass — affects the final composed pixel including all objects and overlays

### New Shader: `fog.frag`

```
Inputs:
  sampler2D sceneTex    // current working color (TAA output)
  sampler2D depthTex    // D24S8 scene depth
  mat4  inverseViewProj // for world-position reconstruction (already in pp)
  vec3  camPos          // camera world position
  vec3  fogColor        // per-mission
  float fogNear         // distance at fog=0 (default 400 units)
  float fogFar          // distance at fog=fogDensity (default 1200 units)
  float fogDensity      // max blend (default 0.12 — used only when no biome is active; biomes always override)

Per pixel:
  1. Sample depth d
  2. Reconstruct worldPos:
       ndcPos = vec4(uv*2-1, d*2-1, 1.0)
       worldP = (inverseViewProj * ndcPos).xyzw
       worldPos = worldP.xyz / worldP.w
  3. linearDist = length(worldPos - camPos)
  4. fogFactor = smoothstep(fogNear, fogFar, linearDist) * fogDensity
  5. result = mix(scene, fogColor, fogFactor)
```

This reuses `inverseViewProj_` already computed and stored on `gosPostProcess` — the same matrix used by `shadow_screen` and SSAO. It is set via `pp->setInverseViewProj()` inside `setTerrainMVP()` in `gameos_graphics.cpp`, which runs before the scene render each frame. The fog pass runs during `endScene()`, well after that update — the matrix is always current. No new matrix uploads needed.

**Per-mission fog colors:**
- Desert: `(0.82, 0.76, 0.64)` — warm sandy haze
- Arctic: `(0.75, 0.82, 0.92)` — cool pale blue
- Urban: `(0.70, 0.72, 0.70)` — neutral grey-green industrial

**Sky exclusion:** Sky pixels have depth=1.0 (at far plane). They get maximum fog factor, which is correct — distant sky and fog blend naturally. No special sky exclusion needed.

**HUD exclusion:** By the time fog runs in the post-process chain, the HUD is not yet composited (it renders to the default framebuffer in a separate pass). No issue.

**Feedback loop prevention:** The fog pass cannot read from and write to `taaHistoryTex_[curr]` simultaneously — that is a GL feedback loop. It writes to `fogOutputTex_` (full-res RGBA16F), which becomes the new working buffer. The SSAO and god ray FBO textures freed in Phase 0 are half-resolution; `fogOutputTex_` is full-resolution, so this is not a memory-neutral trade — it is a net increase. VRAM headroom at the target spec (RX 7900 XTX, 1.4 GB observed in-game at 4K) makes this a non-issue.

### Pipeline Position

After TAA resolve, before outline blend:

```
shadow_screen → shoreline → TAA → [fog → fogOutputTex_] → outline → bloom → composite
```

Composite reads `fogOutputTex_` instead of `taaHistoryTex_[curr]` when fog is enabled. With fog disabled, composite reads `taaHistoryTex_[curr]` directly (or `sceneColorTex_` when TAA is also disabled). This pointer-swap is a single conditional in `endScene()`.

Fog applies before outlines so unit outlines are visible through the fog at distance (gameplay legibility). The fog subtly darkens/tints distant terrain and units, then the outline is drawn crisp on top.

---

## 8. Updated Render Pipeline

```
SHADOW PASSES
─────────────────────────────────────────
1. Static shadow pre-pass      → shadowFBO_ (4096², D24)
2. Dynamic shadow pre-pass     → dynShadowFBO_ (4096², D24)

CLASSIFICATION PASS (new)
─────────────────────────────────────────
3. Classification pre-pass     → classFBO_ (full-res, GL_R8)
   Render mech/object geometry with classify_object shader
   Outputs: faction ID per pixel

MAIN SCENE RENDER
─────────────────────────────────────────
4. Clear + bind sceneFBO_
5. Terrain (MRT on: attachment0=color, attachment1=normals)
   → aerial perspective term in gos_terrain.frag
6. 3D objects / mechs  (MRT off: attachment0 only)
7. Overlays

POST-PROCESS CHAIN  (all fullscreen quads on quadVAO_)
─────────────────────────────────────────
8.  shadow_screen      reads: depthTex, normalTex, shadowMaps   → sceneColorTex_ (in-place)
9.  shoreline          reads: depthTex, normalTex               → sceneColorTex_ (in-place)
10. TAA resolve        reads: sceneColorTex_, historyTex[prev], depthTex
                       writes: taaHistoryTex_[curr]  ← working buffer from here forward
11. Fog pass           reads: taaHistoryTex_[curr], depthTex
                       writes: fogOutputTex_  ← working buffer from here forward
12. Outline resolve    reads: classTex_
                       writes: outlineTex_ (RGBA8, full-res)
13. Bloom threshold    reads: fogOutputTex_ (workingSceneTex)   → bloomFBO_[0]
14. Bloom blur         ping-pong bloomFBO_[0↔1]
15. Composite (→ screen):
    reads: fogOutputTex_ (scene), outlineTex_, bloomColorTex_[0], lutTex_
    ─ bloom add (very low intensity)
    ─ LUT lookup (replaces ACES + sunset filter)
    ─ outline alpha-blend over scene
    ─ clean vignette (8%, uniform-driven)
    ─ → default framebuffer
```

---

## 9. FBO and Texture Budget

### New Resources

| Resource | Format | Purpose |
|----------|--------|---------|
| `taaHistoryTex_[2]` | RGBA16F full-res | TAA history ping-pong |
| `taaFBO_[2]` | — | TAA resolve targets |
| `fogOutputTex_` | RGBA16F full-res | Fog pass output (avoids feedback loop with TAA history) |
| `fogFBO_` | — | Fog render target |
| `classTex_` | R8 full-res | Classification (faction IDs per pixel) |
| `classFBO_ depth RBO` | D24 renderbuffer full-res | Depth for classFBO_ (required for depth testing; see §6) |
| `classFBO_` | — | Classification render target |
| `outlineTex_` | RGBA8 full-res | Sparse outline output |
| `outlineFBO_` | — | Outline resolve target |
| `lutTex_` | RGB8 (3D, 33³) | 3D LUT (~108 KB, negligible) |

### Reclaimable Resources (removed passes)

| Resource | Freed by |
|----------|----------|
| `ssaoColorTex_`, `ssaoBlurTex_`, `ssaoNoiseTex_` | Phase 0: SSAO removal |
| `ssaoFBO_`, `ssaoBlurFBO_` | Phase 0: SSAO removal |
| `godrayColorTex_`, `godrayFBO_` | Phase 0: god ray removal |

### Texture Unit Usage in New Passes

| Pass | Unit 0 | Unit 1 | Unit 2 | Unit 3 |
|------|--------|--------|--------|--------|
| TAA resolve | sceneTex | historyTex | depthTex | — |
| Fog | taaHistoryTex_[curr] | depthTex | — | — |
| Outline | classTex | — | — | — |
| Composite | fogOutputTex_ | bloomTex | lutTex (3D) | outlineTex |

No conflicts with existing shadow_screen (units 0–3) since these passes run sequentially.

---

## 10. AMD RX 7900 XTX Compliance

All new passes follow the rules from `docs/amd-driver-rules.md`:

| Rule | Applied Where |
|------|--------------|
| No `usampler2D` | Classification uses GL_R8 + `sampler2D`, read as float × 255 |
| No `glTextureView` | No stencil extraction; separate `classTex_` instead |
| No attachment 1 from non-terrain | All new FBOs have single attachment at location 0 |
| No feedback loops | TAA ping-pong always writes to opposite index from what it reads |
| `uniform int` not `uint` | Faction values passed as `uniform float classValue` |
| Attribute 0 occupied | classify_object.vert: `in vec3 inPosition` is location 0 |
| `gl_FragDepth` avoid | No new fragment depth writes |

---

## 11. New Uniforms Summary

### On `gosPostProcess`:

**TAA:**
- `float prevViewProj_[16]` — previous frame VP (clean, no jitter)
- `int taaJitterIdx_` — Halton sequence index **1–8** (never 0; see §2)
- `bool taaEnabled_` — toggle
- `float taaAlpha_` — history blend (default **0.15**; see §2)
- *(Phase 2, deferred)* `vec2 currJitter_`, `prevJitter_` — per-frame jitter offsets for exact history UV correction (see §2 "store and subtract" note)

**LUT:**
- `GLuint lutTex_` — 3D texture handle
- `float lutBlend_` — for future crossfade (Phase 2)

**Fog:**
- `float fogColor_[3]`, `fogNear_`, `fogFar_`, `fogDensity_` — per-mission

**Outline:**
- `float outlineWidth_` — default **2.0** (calibrated for 4K on a 42" OLED; see §0 design target). At 1080p this reads as slightly chunky, which is a forgiving failure mode — looks like a stylistic choice rather than a readability bug. If users report outlines too heavy at 1080p, expose as a settings option.

**Composite:**
- `float vignetteStrength_`, `vignetteInner_`, `vignetteOuter_` — replaces hardcoded values

### On `gosRenderer` (for aerial perspective):

- `float aerialNear_`, `aerialRange_`, `aerialDesatStr_`, `aerialTintStr_`
- `float aerialColor_[3]`

---

## 12. Implementation Priority

**Recommended implementation order.** SSAO + god ray removal comes first: it clarifies resource ownership before new FBO allocations begin, and it means new allocations are clearly using fresh budget rather than implicitly relying on "recovered" memory that hasn't been freed yet.

### Phase 0: SSAO + God Ray Removal

Do this before adding any new FBOs. The resource story is cleaner: new passes allocate into freed budget rather than into memory that will theoretically be freed later.

1. Delete `runSSAO()`, `runGodRays()` call sites from `endScene()`
2. Delete function bodies from `gos_postprocess.cpp`
3. Delete texture allocations: `ssaoColorTex_`, `ssaoBlurTex_`, `ssaoNoiseTex_`, `godrayColorTex_`
4. Delete FBO allocations: `ssaoFBO_`, `ssaoBlurFBO_`, `godrayFBO_`
5. Delete shader programs: `ssaoProg_`, `ssaoBlurProg_`, `ssaoApplyProg_`, `godrayProg_`
6. Delete shader files: `ssao.frag`, `ssao_blur.frag`, `ssao_apply.frag`, `godray.frag`
7. Remove all `ssaoEnabled_`, `godrayEnabled_` flags and hotkey bindings
8. Build and smoke-test — verify no regressions, confirm freed memory

### Phase 1: Outline Pass (~1 day)

1. Add `classFBO_` + `classTex_` allocation to `gos_postprocess.cpp`
2. Write `classify_object.vert` + `classify_object.frag`
3. Add `drawClassifyBatch()` to `gameos_graphics.cpp`
4. Wire call site in `txmmgr.cpp:renderLists()` with team/faction ID
5. Write `outline.frag`
6. Add `outlineFBO_` + `outlineTex_` to post-process
7. Add `runOutline()` call in `endScene()` (after TAA slot, before bloom)
8. Blend `outlineTex_` in composite shader
9. Add `RAlt+8` toggle

### Phase 2: LUT + Vignette Cleanup (~half day)

1. Add `lutTex_` (3D) allocation + identity LUT generation to `gos_postprocess.cpp`
2. Write `loadLUT(path)` parser
3. Modify `postprocess.frag`: remove sunset filter block + ACES, add LUT lookup + clean vignette
4. Add `setLUT()` API
5. Ship `data/luts/identity.cube`, `desert_warm.cube`, `arctic_cool.cube`, `urban_grey.cube`

### Phase 3: Aerial Perspective + Fog (~half day)

1. Add aerial uniforms to `gos_terrain.frag` and `gos_tex_vertex.frag`
2. Add aerial perspective term in both shaders after PBR/surface computation
3. Write `fog.frag`; allocate `fogOutputTex_` + `fogFBO_`
4. Add `runFog()` in `endScene()` after TAA, writing to `fogOutputTex_`
5. Update composite to read `fogOutputTex_` as primary scene input
6. Add per-mission `setFogParams()` and `setAerialParams()` APIs

### Phase 4: Biome Config System (~half day)

1. Write `BiomePreset` struct and `BiomeConfig` parser in `gos_postprocess.cpp`
2. Parse `data/biomes.cfg` at startup; load preset files from `data/biomes/`
3. Implement `loadBiome(missionName)` and `applyBiome(preset)`
4. Connect to mission startup call site
5. Ship base biome presets: `default`, `desert`, `arctic`, `urban`, `night`, `forest`
6. Ship base LUT files in `data/luts/`
7. Add `RAlt+B` biome cycle for runtime debugging
8. Replace individual per-mission manual setter calls with `loadBiome()`

### Phase 5: TAA (~1 day)

1. Add Halton jitter computation to `gos_postprocess.cpp`
2. Modify `setTerrainMVP()` in `gameos_graphics.cpp` to inject jitter before upload
3. Add `taaFBO_[2]` + `taaHistoryTex_[2]` allocation
4. Write `taa_resolve.frag` with neighborhood AABB clamp
5. Add `prevViewProj_` matrix management (swap at frame boundary)
6. Add `runTAA()` call in `endScene()` after shoreline
7. Update fog + outline + composite to read `taaHistoryTex_[curr]` as their source
8. Add `RAlt+T` toggle
9. Remove `enableFXAA` path from composite (FXAA replaced)

### Phase 6: Composite Cleanup

1. Remove `enableBloom`, `enableTonemap`, `enableFXAA` uniforms and code paths from composite shader
2. Update debug hotkey documentation
3. Update `docs/architecture.md` with new pipeline order

---

## 13. SSAO and God Ray Removal

**Confirmed decision:** SSAO and god rays are not disabled — they are removed. Both effects are wrong for this camera at RTS zoom and their infrastructure is consuming FBO/texture init cost on every launch for zero visual benefit.

**SSAO:** Screen-space AO is fundamentally an effect for close-range or third-person cameras. At RTS zoom, the hemisphere sample radius needed to capture meaningful contact shadows exceeds half the screen. The result is noise and banding at the tile-to-tile scale that reads as dirt, not shadow. The correct AO approach for this game is baked vertex AO on static props (single precompute at load time, trivial to add to the prop shader as a per-vertex term). That is out of scope for this spec but is the right future target.

**God rays:** The god ray implementation uses radial scattering from a screen-space sun position. At RTS zoom the sky subtends less than 30° of screen height on average, and the sun is frequently off-screen entirely. Radial scattering from an off-screen point produces no visible effect. God rays are a cinematic effect designed for first- or third-person cameras looking at the sky — wrong tool, wrong camera.

**What gets removed:**
- `ssaoFBO_`, `ssaoBlurFBO_`: allocation, clear, bind, and destroy calls in `gos_postprocess.cpp`
- `ssaoColorTex_`, `ssaoBlurTex_`, `ssaoNoiseTex_`: texture allocation and GL handles
- `ssaoProg_`, `ssaoBlurProg_`, `ssaoApplyProg_`: shader compile, bind, destroy
- `runSSAO()` function body and call in `endScene()`
- `godrayFBO_`, `godrayColorTex_`, `godrayProg_`: same
- `runGodRays()` function body and call in `endScene()`
- `ssao.frag`, `ssao_blur.frag`, `ssao_apply.frag`, `godray.frag`: deleted from `shaders/`
- All `ssaoEnabled_`, `godrayEnabled_` flags and their hotkey bindings

**GPU memory freed:** ~8 MB (ssao pair + godray at half-res). New full-res allocations (`taaHistoryTex_[2]`, `fogOutputTex_`, `outlineTex_`, `classTex_`) are a net increase of ~50+ MB at 4K. At the target spec (RX 7900 XTX, 24 GB VRAM, 1.4 GB observed usage) this is well within budget.

---

## 14. Per-Map Biome Config System

### Problem

Every visual effect in this spec has per-mission parameters (LUT name, fog color, aerial tint, vignette strength). Without a data-driven system, those parameters must be hardcoded per mission in C++, which makes modding impossible and makes adding new missions require a rebuild.

### Design

**Single config file:** `data/biomes.cfg` — plain-text, loaded once at startup, parsed into a lookup table keyed by mission name prefix.

**Format:**
```
# mission_prefix = biome_name
mc2_01 = desert
mc2_02 = desert
mc2_03 = urban
mc2_10 = night
mc2_17 = arctic
mc2_24 = urban
default = default
```

**Biome preset files:** `data/biomes/<biome_name>.cfg` — one per biome, defines all visual parameters.

**Format for each biome file:**
```ini
[lut]
file = desert_warm.cube

[fog]
color = 0.82 0.76 0.64
near = 400.0
far = 1200.0
density = 0.10

[aerial]
color = 0.78 0.70 0.58
near = 200.0
range = 600.0
desatStr = 0.18
tintStr = 0.10

[vignette]
strength = 0.08
inner = 0.25
outer = 0.65

[sky]
zenithColor = 0.28 0.42 0.68
horizonColor = 0.62 0.55 0.44
sunColor = 1.00 0.95 0.78
```

The `[sky]` block maps to the existing skybox uniforms (already exposed on the skybox shader) so biome config shifts the sky color in concert with the ground-level effects.

### Shipped Biome Presets

| Biome | LUT | Character |
|-------|-----|-----------|
| `default` | `default.cube` | Warm neutral, replaces hardcoded sunset filter |
| `desert` | `desert_warm.cube` | Sandy warm haze, tan aerial, bright sky |
| `arctic` | `arctic_cool.cube` | Blue-grey cast, cool aerial, pale overcast sky |
| `urban` | `urban_grey.cube` | Neutral grey-green, muted saturation, hazy |
| `night` | `night.cube` | Dramatically dark LUT, heavy fog density, navy sky |
| `forest` | `forest_green.cube` | Desaturated warm-green, low fog, deep blue sky |

**Night biome specifics:** The `night.cube` LUT compresses the entire brightness range significantly (similar to a film-pushed underexposed look). Combined with heavy fog (`density=0.30`, `near=200`), distant terrain fades to near-black. Vignette increases to 0.15. This produces a genuine "night operation" feel without adding fake "night vision" post-effects that look wrong.

### API

New class `BiomeConfig` (or member of `gosPostProcess`):

```cpp
// Called at mission start, before first frame renders
void gosPostProcess::loadBiome(const char* missionName);

// Called per-frame only if biome changes (mission restart, debug cycle)
// Sets: LUT, fog params, aerial params, vignette, skybox colors
void gosPostProcess::applyBiome(const BiomePreset& preset);

// Debug: cycle through biomes at runtime
void gosPostProcess::cycleBiome();  // bound to RAlt+B (suggested)
```

### Parsing

The parser for `biomes.cfg` and individual `*.cfg` files is minimal — a line-by-line `sscanf` for known keys, skip unknown lines. No external parser library. Failure to find `biomes.cfg` falls back to `default` biome silently. Failure to find a biome file falls back to `default.cfg`.

### Mod Workflow

A modder shipping a new mission `mc2_42` with a snowy highland biome:

1. Add `mc2_42 = highland_snow` to `data/biomes.cfg`
2. Write `data/biomes/highland_snow.cfg` with custom parameters
3. Optionally ship `data/luts/highland_snow.cube`
4. Ship their mission `.fit` file as usual

No C++ changes, no rebuild. The loose-file override system already handles this correctly.

### Relation to `gosRenderer::setAerialParams()`

The aerial perspective uniforms (Section 4) are uploaded once per frame from the current biome preset. The biome system replaces the per-mission manual API calls — instead of calling `setAerialParams()`, `setFogParams()`, etc. individually, mission start calls `loadBiome(missionName)` and the system handles all uploads.

Direct uniform setters (`setAerialParams`, `setFogParams`, `setLUT`) remain as lower-level API for runtime debug overrides.

---

## 15. Per-Mission Art Direction Summary

With the biome config system in place, the per-mission visual kit is entirely data-driven:

1. Tag the mission in `biomes.cfg`
2. Tune `data/biomes/<biome>.cfg` if needed
3. Optionally bake a custom LUT in Resolve/Photoshop and export `.cube`

The three numbers that matter most per biome:
- `lut.file` — overall look and tonemapping curve
- `fog.color` + `fog.density` — what the far field reads as
- `aerial.color` + `aerial.desatStr` — how the terrain surface grades with distance

Everything else (vignette, sky) is secondary and defaults well from the base presets.

---

## 16. What This Does NOT Include

These are explicitly out of scope per the design brief and this spec:

- **Screen-space reflections / SSGI / DOF** — wrong camera angle
- **Chromatic aberration / lens flares** — cinematic only, read as broken at RTS zoom
- **SSAO in any form** — wrong for top-down; baked vertex AO in static props is the correct approach if AO is needed later
- **Bloom tuning** — the bloom infrastructure stays but is nearly-off; it is not the RTS visual upgrade path
- **Per-object velocity buffer** — Phase 1 TAA uses camera-only reprojection; object velocity is Phase 2 if TAA ghosting on fast mechs proves problematic at real zoom levels
- **God rays** — removed; wrong camera and invisible at RTS zoom (see Section 13)

---

## 17. Implementation Notes

### Performance Budget

No formal frame-time target is set in this spec — the target hardware (RX 7900 XTX) has enough headroom that micro-optimization is premature. Rough back-of-envelope for Phase 1–3 new passes at 4K 60fps: TAA resolve (~0.3ms), fog (~0.2ms), classification prepass (~0.1ms mech-only geometry), outline resolve (~0.2ms). Total new post-process cost estimated <1ms on the 7900 XTX. Verify with Tracy GPU zones after each phase ships; add `TracyGpuZone` annotations to each new `run*()` function. On lower-spec AMD hardware (RX 580, Vega), the full-res RGBA16F passes (TAA, fog) are the most likely pressure points — TAA can be disabled via `RAlt+T` and fog via its toggle independently.

### Resolution Change and Window Resize

All new full-res textures (`taaHistoryTex_[2]`, `fogOutputTex_`, `classTex_`, `outlineTex_`) are allocated at construction time in `gosPostProcess::init()` using `width_` and `height_`. If the game supports runtime resolution changes (windowed resize, dynamic resolution), these textures need a reallocation path:

1. On resize: call a new `gosPostProcess::resize(int w, int h)` method
2. `resize()` deletes and reallocates all full-res textures at the new dimensions
3. The TAA history is invalidated on resize — force `taaAlpha_ = 1.0` for one frame (or `taaJitterIdx_ = 0` reset) to avoid blending stale history at the wrong resolution
4. `classFBO_` depth renderbuffer also needs resize

MC2 does not currently support runtime resolution changes (windowed mode is fixed-size). This is a gap to acknowledge but not implement in Phase 1.

### Phase Ordering Rationale — TAA Last

TAA is intentionally last (Phase 5). Phases 1–4 ship and are tunable without temporal stability. This is the correct order for two reasons: (a) TAA is the highest-risk effect — a jitter implementation bug causes obvious full-frame shimmering that makes every other effect look broken, making debugging harder; (b) outline, LUT, and biome tuning are best evaluated without TAA so artists can see the unfiltered image. Once those effects are locked, TAA is added as the final stability layer. Expect Phase 1–4 screenshots and playtests to show more aliasing than the shipped result — this is expected, not a regression.

---

*Spec written 2026-04-24. Reference: CoH3 / AoE4 / SC2 Remastered post-FX analysis.*
