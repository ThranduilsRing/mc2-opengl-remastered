# G-Buffer MRT + Post-Process Shadow Pass — Design Doc

**Date:** 2026-04-11
**Branch:** claude/nifty-mendeleev
**Goal:** Add a normal buffer via MRT, convert depth to a sampleable texture, and implement a fullscreen post-process shadow pass that shadows overlays, objects, and other geometry that can't do forward shadow lookups.

## Background

Overlays (roads, cement, craters) and objects use the `gos_tex_vertex` shader with screen-space pre-transformed vertices — no world position available for shadow map lookup. Terrain already has forward shadows via direct shadow map sampling in `gos_terrain.frag`. This creates a visible quality gap: terrain is shadowed, everything on top of it is not.

## Approach: Normals MRT + Depth Reconstruction + Screen-Space Shadow

### Decision Log

- **Normal buffer only (no position buffer):** World position is reconstructable from the existing depth buffer + inverse VP matrix. An explicit position buffer would cost 12 bytes/pixel extra bandwidth for no benefit at RTS camera distances.
- **Keep terrain forward shadows:** Terrain shadow quality is higher when sampled per-fragment in the forward pass (per-material tuning, oblique angle handling). The post-process shadow pass only targets non-terrain geometry.
- **Skip flag in normal alpha:** `GBuffer1.a = 1.0` means "already shadowed forward" (terrain). `GBuffer1.a = 0.0` means "needs post-process shadow" (overlays, objects). The post-process pass skips pixels with alpha=1.

---

## Part 1: Depth Buffer Conversion (Renderbuffer → Texture)

**Why:** The scene depth is currently a `GL_DEPTH24_STENCIL8` renderbuffer, which cannot be bound as a sampler. Convert to a depth texture so the post-process pass can sample it.

**Changes:**
- `gos_postprocess.h`: Replace `GLuint sceneDepthRBO_` with `GLuint sceneDepthTex_`
- `gos_postprocess.cpp createFBOs()`: Replace renderbuffer creation with texture creation:
  - `glGenTextures` / `glTexImage2D(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8)`
  - `glFramebufferTexture2D(GL_DEPTH_STENCIL_ATTACHMENT)`
  - `GL_NEAREST` filtering, `GL_CLAMP_TO_EDGE`
- `gos_postprocess.cpp destroyFBOs()`: `glDeleteTextures` instead of `glDeleteRenderbuffers`

**Cost:** Zero. Same GPU format, different API object.

---

## Part 2: Normal Buffer MRT

**New texture on scene FBO:**
- `sceneNormalTex_` — `GL_RGBA16F`, same dimensions as scene color
- Attach to `GL_COLOR_ATTACHMENT1`
- `glDrawBuffers({GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1})` in `beginScene()`

**Fragment shader MRT outputs:**

| Shader | `layout(location=1)` output |
|--------|----------------------------|
| `gos_terrain.frag` | `vec4(N * 0.5 + 0.5, 1.0)` — world normal, alpha=1 (skip post-process shadow) |
| `gos_tex_vertex.frag` | `vec4(0.5, 0.5, 1.0, 0.0)` — flat up normal, alpha=0 (needs shadow) |
| `object_tex.frag` | `vec4(0.5, 0.5, 1.0, 0.0)` — flat up normal, alpha=0 (needs shadow) |
| Other frag shaders | `vec4(0.5, 0.5, 1.0, 0.0)` — safe default |

**AMD constraint:** All MRT outputs must be declared in every fragment shader, even if writing constants. AMD drivers may skip draws if outputs are undeclared.

**DrawBuffers management:**
- `beginScene()`: set `glDrawBuffers` to 2 attachments
- Before shadow FBO binds (static + dynamic): reset to single attachment
- Bloom FBOs: single attachment (no MRT)

---

## Part 3: Post-Process Shadow Pass

**New fullscreen pass after scene rendering, before bloom.**

### Pipeline Position

```
1. beginScene()           — bind sceneFBO_, glDrawBuffers({0,1})
2. Clear color + depth + normal
3. Terrain render          — writes color + normal(alpha=1)
4. Object render           — writes color + normal(alpha=0)
5. Overlay/water render    — writes color + normal(alpha=0)
6. POST-PROCESS SHADOW     — reads depth+normal+shadowmap, darkens scene color
7. Bloom (threshold+blur)  — reads scene color
8. endScene()             — FXAA + tonemap + bloom → default FB
```

### Shader: `shadow_screen.frag`

```
Inputs:  sceneDepthTex (unit 2), sceneNormalTex (unit 3), shadowMap (unit 4)
Uniform: inverseViewProj (mat4), lightSpaceMatrix (mat4), screenSize (vec2)

Per pixel:
1. Read normal alpha — if >= 0.5, discard (terrain, already shadowed)
2. Sample depth buffer
3. Reconstruct world position:
   NDC.xy = gl_FragCoord.xy / screenSize * 2.0 - 1.0
   NDC.z  = depth * 2.0 - 1.0
   worldPos = inverseViewProj * vec4(NDC, 1.0)
   worldPos.xyz /= worldPos.w
4. Transform to light space: lightPos = lightSpaceMatrix * vec4(worldPos.xyz, 1.0)
5. Sample shadow map (Poisson PCF, 4-8 taps — cheaper than terrain's 16)
6. Output: darken scene color by shadow factor
```

### Rendering Method

Bind scene FBO, set `glDrawBuffers` to color-only (attachment 0), enable multiplicative blending (`GL_DST_COLOR, GL_ZERO`), draw fullscreen quad with shadow shader. White pixels = lit, dark pixels = shadowed.

### New Uniform: `inverseViewProj`

Computed on CPU: `inverse(terrainMVP)`. Uploaded once per frame to the shadow screen shader. The `terrainMVP` matrix is already computed in `gameos_graphics.cpp`.

---

## Part 4: New Files

| File | Type | Purpose |
|------|------|---------|
| `shaders/shadow_screen.vert` | New | Fullscreen quad vertex shader (passthrough) |
| `shaders/shadow_screen.frag` | New | Post-process shadow sampling |

All other changes are modifications to existing files.

## Cost Estimate

- Normal texture write: ~2-3% bandwidth increase
- Depth texture conversion: zero cost
- Post-process shadow pass: ~0.5ms fullscreen
- GPU has massive headroom (11-15% utilization at Wolfman zoom)
- Use Tracy profiler to verify

## AMD Driver Constraints

- No `sampler2DArray` (driver crash) — individual `sampler2D` only
- MRT outputs must be declared in all fragment shaders
- Dummy color attachment on depth-only FBOs (already handled for shadow FBOs)
- Attribute 0 must be active in new shader passes (fullscreen quad VBO)

## Future Work (Enabled by This Infrastructure)

- SSAO: reads normal buffer + depth texture, half-res pass
- Height-based fog: reads depth for distance computation
- Soft particles: depth comparison
- Screen-space contact shadows
- Mech forward shading (independent — uses object shader, not post-process)
