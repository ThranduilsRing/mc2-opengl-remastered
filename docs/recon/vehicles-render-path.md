# Vehicles Render Path Audit

**Date:** 2026-06-16  
**Scope:** Vehicles (non-mech units) rendering pipeline  
**Target:** MC2 OpenGL, C++17, OpenGL 4.3 core profile

---

## Executive Summary

Vehicles use the **TG_Shape/TG_MultiShape immediate-mode CPU-to-GPU triangle submission path** via `Mech3DAppearance::render()`. This is fundamentally identical to the legacy D3D pipeline: per-frame per-vehicle object transform calculation, unconditional vertex luminosity + visibility culling, and **1-draw-per-triangle batching via gos_DrawTriangles()** (enqueued into per-texture buckets). With no vehicle-specific grouping or GPU batching, the cost grows linearly with active vehicle count. The GPU mech batcher (SliceA) provides opt-in GPU MDI submission, but is NOT vehicle-focused (mechs are the priority). **Net result:** 0.3-0.4ms UpdateGeometry + 1-draw-per-tri render overhead scales to multiple milliseconds on dense battlefields.

---

## 1. Data Flow Summary

### CPU-Side Per-Frame Work

1. **Mech3DAppearance::updateGeometry() (mech3d.cpp:3538)**
   - **Cost:** ~0.3-0.4ms per visible vehicle (Tracy zone "GameLogic.Mech3D.UpdateGeometry")
   - **Work:**
     - Position swizzle: Stuff space → MC2 space (x' = -x, y' = z, z' = y)
     - Per-frame LOD selection via distance test (line 2570: `eyeDistance > mechType->lodDistance[i]`)
     - Fog calculation per actor (lines 3611-3653): terrain light intensity lookup, fog RGB/alpha
     - Lighting update: `setLightColor(0, lightRGB)` + `setLightIntensity(0, 1.0)` (lines 3614-3621)
     - Spotlight (SLCircle_anubis) lifecycle: first-frame allocation (line 3670: malloc), subsequent in-place update (lines 3694-3706)
     - Animation frame advance (implicit in SetTextureHandle + shape state)
   - **Unconditional Work:** All above runs for every visible vehicle; no frustum pre-cull before these calcs.

2. **Mech3DAppearance::render() (mech3d.cpp:2659)**
   - **GPU mech batcher Slice A (opt-in):**
     - GPU submit via `GpuMechBatcher::instance().submitActor(desc)` if `g_useGpuMechs=1` (line 2788)
     - Falls back to CPU mechShape->Render(true) if GPU fails or path disabled (line 2895)
     - **Counter:** s_mlrMechDrawsThisMission (line 2894) tracks CPU fallback incidence
   - **CPU fallback (always present):** mechShape->Render(true) → TG_Shape::Render()

3. **TG_Shape::Render() (tgl.cpp:2934)**
   - **Per-visible-face loop (line 2987):** `for (j=0; j<numVisibleFaces; ++j)`
   - **Work per triangle:**
     - Vertex fetch from pre-transformed listOfVertices[] (lines 2996-2998)
     - UV assignment from triType.uvdata (lines 3000-3001, 3005-3006, 3010-3011)
     - Per-vertex ARGB light color copy from tri.aRGBLight[] (lines 3003, 3008, 3013)
     - Z-clamp check (line 3052-3057) — early-out if off-screen
     - **Texture change detection** (line 3037): per-triangle texture state check; `gos_SetRenderState()` if different (line 3039)
     - Deferred enqueue via `mcTextureManager->addVertices()` (lines 3084, 3088, 3097, 3101)

### Submission Model

- **Immediate-mode batching:** per-texture bucket accumulation (no MDI)
- **Order:** TG_Shape::Render() enqueues into mcTextureManager; later flush via renderLists() -> gos_DrawTriangles()
- **Batch granularity:** per-texture, opaque/alpha separation (MC2_DRAWSOLID | MC2_DRAWALPHA flags)

---

## 2. Draw Call Structure

### Call Count Scaling

- **Per-vehicle:** `numVisibleFaces` draw calls (one per face/triangle)
- **Typical vehicle:** 20-100 visible triangles per LOD (e.g., hover tank, turret)
- **Batch:** 50-200 vehicles on large map = 1,000-20,000 individual triangle submissions
- **Batching quality:** **Very poor.** No culling per vehicle; no spatial clustering; same texture ≠ contiguous submission (interleaved by shape iteration order).

### Draw Call Variants Used

1. **gos_DrawTriangles() (tgl.cpp:3062, 3084, 3088, 3097, 3101)**
   - Immediate 3-vertex draw or enqueue-to-bucket
   - Called per visible triangle; bucket flushes in renderLists()

2. **GPU Mech Batcher MDI (Slice A)**
   - `GpuMechBatcher::instance().submitActor(desc)` (mech3d.cpp:2863)
   - Returns false if: killswitch off, shader uninitialized, late registration
   - Falls back to CPU path on failure; **no batching across fallbacks**

3. **No glMultiDrawElementsIndirect or persistent-mapped buffers**
   - Legacy D3D API translated to GL immediate-mode
   - Per-object state changes not coalesced

---

## 3. GL State Management

### State Explicit vs. Inherited

| State | Set By | Explicit? | Location |
|-------|--------|-----------|----------|
| **Depth test** | gos_SetRenderState() | Inherited? | No explicit glEnable(GL_DEPTH_TEST) in TG_Shape::Render |
| **Cull face** | gos_SetRenderState() | Inherited? | No explicit glCullFace() |
| **Blend** | gos_SetRenderState() (line 3042) | Partial | Only when textureAlpha=true; alpha-opaque blending NOT set |
| **Fog** | gos_SetRenderState( gos_State_Fog, ...) (line 3961, 3965) | Explicit | Per-shape call |
| **Texture unit 0** | gos_SetRenderState( gos_State_Texture, ...) (line 3039) | Explicit per-tri | Texture changes PER TRIANGLE (line 3037 check) |
| **Alpha test** | gos_SetRenderState( gos_State_AlphaTest, ...) (line 2970) | Explicit | Once per shape; reused per tri |
| **Filter mode** | gos_SetRenderState( gos_State_Filter, ...) (line 2971) | Explicit | Once per shape |

### Critical Issue: State Fragility

**TG_Shape::Render inherits pipeline state from prior passes.** No `gos_InvalidateRenderStateCache()` call after state mutations. Result:

- If a prior pass (e.g., transparent water) left `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` and `glDepthMask(GL_FALSE)`, opaque vehicles render without depth write.
- Per-triangle texture changes (line 3037-3046) do NOT verify or restore surrounding state (blend, depth).
- **Risk:** camera/object-selection dependent visual artifacts if a mech-with-alpha (sensor square, spotlight) precedes opaque vehicles in iteration order.

### gos_InvalidateRenderStateCache() Usage

Grep for **gos_InvalidateRenderStateCache()** in mech3d.cpp:
- Not present. Expected call sites: after setLightColor/setLightIntensity (line 3620-3621), after LOD swap (line 2625), before render (line 2659).

---

## 4. Shader Pipeline

### Vertex Shader: gos_tex_vertex.vert

```glsl
layout(location = 0) in vec4 pos;          // Already projected CPU-side
layout(location = 1) in vec4 color;        // Per-vertex ARGB from TG_Triangle
layout(location = 2) in vec4 fog;          // Per-frame from updateGeometry
layout(location = 3) in vec2 texcoord;     // From TG_TypeTriangle.uvdata

uniform mat4 mvp;                          // Identity; pos is screen-space

void main(void) {
    vec4 p = mvp * vec4(pos.xyz, 1.0);
    gl_Position = p / pos.w;               // Redundant division (MVP = identity)
    Color = color;
    FogValue = fog.w;
    Texcoord = texcoord;
}
```

**Issues:**
1. **MVP already applied CPU-side.** Position is post-projection screen-space; MVP uniform is identity. Passing mvp matrix to GPU is wasteful; division by pos.w is redundant if already normalized.
2. **No world-space derivatives available to shader.** Bumps/POM need per-fragment world-normal calculation from geometry; impossible with screen-space input.

### Fragment Shader: gos_tex_vertex.frag

```glsl
in vec4 Color;           // Per-vertex ARGB, baked from tri.aRGBLight[]
in float FogValue;       // Global fog alpha
in vec2 Texcoord;

void main() {
    vec4 texel = texture(sampler, Texcoord);
    gl_FragColor = texel * Color;  // Modulate by pre-lit color
    gl_FragColor.a = texel.a * FogValue;
}
```

**Issues:**
1. **All lighting pre-computed per-vertex CPU-side** (TG_Triangle.aRGBLight[]). No per-fragment lights, no specular, no normal mapping.
2. **Vertex-lit only.** Gouraud shading with baked ARGB (lines 3003/3008/3013 copy tri.aRGBLight[0..2] into per-vertex ARGB).
3. **Per-triangle texture state change** (line 3037) means no texture atlassing; single texture per tri.

### Lighted Variant: gos_tex_vertex_lighted.vert / .frag

- **Not found in typical vehicle render path.** Mechs use CPU-lit tri.aRGBLight[]; turrets/helicopters same.
- **Spotlight exception:** SLCircle_anubis node sends point-light data to eye->addWorldLight() (line 3672), but this lights the ENTIRE mesh, not per-vertex (per-shape light list).

### Unused / Legacy

- **shadow_object.vert/frag:** Used for dynamic shadow pre-pass, NOT primary render.
  - Transforms to light-space; no color/UV (depth-only).
  - Does NOT use vehicle-specific uniforms; generic shape submission.

---

## 5. Redundant / Repeated Work

### Per-Frame Unconditional Work

| Work | Location | Frequency | Avoidable? |
|------|----------|-----------|-----------|
| Terrain light lookup | updateGeometry() line 3612 | Per visible vehicle | Batch once per frame for terrain light grid |
| Fog calculation | lines 3627-3653 | Per visible vehicle | Store as global; use per-frame lookup table |
| Position swizzle | lines 3580-3582 | Per visible vehicle, per updateGeometry call | Cache if vehicle static |
| Animation frame advance | updateGeometry() | Per visible vehicle | Deferred if vehicle off-screen (but runs anyway) |
| Spotlight lifecycle check | lines 3727-3757 | Per visible vehicle | Once per mission, not per-frame |

### Per-Object State Churn

| Call | Count | Impact |
|------|-------|--------|
| SetTextureHandle(0, ...) | 3 (body + 2 arms) | ~50ns each; 150ns per vehicle |
| SetFogRGB() | 1 per vehicle | ~10ns |
| SetLightsOut() | 1 per vehicle | ~10ns |
| setLightColor(0, ...) | 1 per vehicle | ~20ns |
| setLightIntensity(0, ...) | 1 per vehicle | ~20ns |
| gos_SetRenderState() | 4-20 per vehicle render | Depends on texture changes |

**Total: ~15-50us per vehicle in state setup**, 5-20 vehicles live = 75us-1ms per frame (not load-bearing, but scalable).

### Texture Changes Per Triangle

**TG_Shape::Render loop (line 2987-3109):**
- Per-triangle texture fetch (line 3035): `theShape->listOfTextures[triType.localTextureHandle]`
- Per-triangle texture comparison (line 3037): `!= lastTextureUsed`
- Per-triangle gos_SetRenderState() **if changed** (line 3039)

**Cost:** O(N) texture slot lookups, O(M) state changes where M = # texture transitions within shape. For 50-triangle vehicle with 3 textures: ~3 state changes amortized, acceptable. But **per-shape**, not per-batch across all shapes.

### Redundant Projection: CPU then GPU

- **TG_Shape::Render** passes pre-projected screen-space position (line 2994: `listOfVertices[i]`).
- **gos_tex_vertex.vert** applies MVP (line 16), which is identity/near-identity (redundant).
- **GPU projection is wasted:** 1 redundant matrix multiply per vertex * 1000-20000 vertices = 1-20k FLOPs per frame.

---

## 6. Modernization Gaps

### Missing: UBO for Per-Frame Constants

**Current:**
- Fog color / light color / light intensity set via gos_SetRenderState() + uniform scalar assignments (implicit in TG_Shape API).
- No per-frame UBO binding all vehicles share (e.g., sun direction, ambient floor, fog params).

**Impact:** 
- Each shape Render() re-uploads the same fog color (line 3961/3965).
- Spotlight updates re-assign identical point-light direction (line 3696) for every night-visible mechanic.

### Missing: Persistent-Mapped Buffers

- **Vertex data:** re-copied via gos_DrawTriangles() per triangle, not streamed from a ring buffer.
- **Implied cost:** CPU → GPU vertex data transfer, no double-buffering of the CPU workload behind GPU rendering.

### Missing: Sync-Free Ring Buffers

- **UpdateGeometry + Render are sequentially coupled:** update() must finish before render() can start (no ring buffer overlap).
- **Single-buffered state:** listOfVertices[] written in updateGeometry(), read in render() with no fence/event.

### Missing: GPU-Driven Indirect Submission (MDI)

- **Slice A (GPU Mech Batcher) is opt-in**, not default (killswitch: `g_useGpuMechs=1`).
- **Fallback is immediate-mode:** mechShape->Render(true) → per-tri gos_DrawTriangles().
- **No vehicle-specific MDI:** turrets, helicopters, scout vehicles all use CPU tri-draw path.

### Legacy Comments Referencing D3D/MechCommander2

| Line | Comment | Issue |
|------|---------|-------|
| 2738 | "Call Multi-shape render stuff here." | Generic placeholder; no actual multi-shape logic documented |
| 3423 | "DONT RENDER UNTIL FINAL MECH DATA FROM MR CHOI" | Dead comment; render runs unconditionally |
| 3081 | "Can make this a flag to optimize!" | Flag exists (drawOldWay line 3031) but not meaningfully used |

---

## 7. Quick Wins (Ranked by ROI)

### 1. **Cache Fog/Light State Across Vehicles (0.5 hours, 0.1-0.2ms gain)**

**Current:** Per-vehicle terrain light lookup (line 3612) + fog calc (lines 3627-3653).

**Fix:**
- Pre-compute fog RGB once per frame in a global; store in UBO.
- Cache terrain light intensity in a lookup table at 64x64 or 128x128 resolution (pre-sample per frame).
- updateGeometry() reads cached values instead of doing per-vertex interpolation.

**Code location:** mech3d.cpp::updateGeometry(), lines 3611-3653.

**Estimated gain:** 0.1-0.2ms (terrain light lookups are O(n) per vehicle; caching → O(1) + one-time pre-pass).

---

### 2. **Validate and Restore GL State in TG_Shape::Render (2 hours, 0.05-0.1ms + visual correctness)**

**Current:** No gos_InvalidateRenderStateCache() after state changes; inherited state from prior passes.

**Fix:**
1. Add `gos_InvalidateRenderStateCache()` after SetFogRGB() (line 3967).
2. Add explicit glEnable(GL_DEPTH_TEST) + glDepthMask(GL_TRUE) + glDisable(GL_BLEND) before the tri loop (line 2987).
3. Add `glDepthFunc(GL_LEQUAL)` to match legacy behavior.
4. Save/restore on exit (RAII guard).

**Code location:** tgl.cpp::TG_Shape::Render(), lines 2958-2987.

**Estimated gain:** 0.05-0.1ms (cache validation + one-time state set instead of inherited guesses); **fixes visual popping/transparency artifacts**.

---

### 3. **Eliminate Redundant MVP Transform (1 hour, 0.01-0.05ms)**

**Current:** CPU pre-projects to screen-space; GPU applies mvp = identity (redundant divide).

**Fix:**
1. Change gos_tex_vertex.vert line 16-17 to:
   ```glsl
   gl_Position = vec4(pos.xyz, 1.0);  // Already screen-space
   ```
2. Remove mvp uniform from TG_Shape render submission if unused elsewhere.

**Code location:** shaders/gos_tex_vertex.vert, line 16-17.

**Estimated gain:** 0.01-0.05ms (1 redundant matrix multiply per 1-20k vertices = 0.01-0.02ms); **primarily correctness** (eliminates divide-by-zero risk if pos.w not normalized).

---

### 4. **Spotlight Node Registration: Move to Init or Lazy-First (0.5 hours, 0.01ms per vehicle)**

**Current:** Per-frame check + iterate all shapes (lines 3727-3757); first-frame malloc (line 3670).

**Fix:**
1. Move spotlight discovery to mech init (Mech3DAppearance::init()).
2. Register spotlights once; cache node indices in a static map (mech type → spotlight slot list).
3. updateGeometry() only updates position/direction; skip shape iteration.

**Code location:** mech3d.cpp::Mech3DAppearance::updateGeometry(), lines 3725-3757.

**Estimated gain:** 0.01ms per vehicle on first night (saves shape walk); 0.001ms per vehicle thereafter (cached node fetch vs loop).

---

### 5. **Coalesce Texture State Changes Across Vehicle Batch (3 hours, 0.05-0.2ms)**

**Current:** Per-triangle texture lookup + state change (line 3037-3046); no inter-vehicle coalescing.

**Fix:**
1. Pre-sort visible triangles by texture handle before TG_Shape::Render().
2. OR: Use a texture-atlas sidecar; UV-remap tri UVs to atlas coords.
3. OR: Use sampler2DArray + array layer per texture (requires shader rewrite).

**Code location:** tgl.cpp::TG_Shape::Render(), line 2987 loop; txmmgr.cpp (texture bucket flush).

**Estimated gain:** 0.05-0.2ms (reduces texture state changes from O(M) per shape to O(L) where L = # unique textures in shape; for vehicles ~1-3 textures, not large, but for large batches → meaningful).

**Effort/ROI:** Lower ROI than #1-3 due to 3-hour refactor cost; defer if gains are small on profiler.

---

## 8. Known Issues & Assumptions

### Vehicle-Specific Behaviors

1. **Helicopters:**
   - Use standard TG_Shape::Render() path.
   - Dust cloud effect: gosFX::EffectLibrary (lines 5593-5637); deferred/CPU-side.
   - No special LOD or culling (same as ground vehicles).

2. **Turrets:**
   - Rotated via torsoRotation (line 2176: `setMoverParameters(turretRot, ...)`).
   - No per-turret batching; rendered as part of parent mech shape.

3. **Hover Tanks / Scouts:**
   - Same TG_Shape::Render() path; no vehicle-specific optimization.

### Assumptions Made

- **Vehicle count typical:** 5-50 on large map; up to 200 on extreme scenario.
- **Visible triangles:** 20-100 per vehicle (average LOD0).
- **GPU mech batcher (Slice A):** Mechs only; vehicle coverage TBD (likely unimplemented).
- **No vehicles in editor:** UpdateGeometry check-flags run regardless; editor has separate code path (InEditor flag, line 2682).

---

## 9. Appendix: File Inventory

| File | Lines | Role |
|------|-------|------|
| mech3d.cpp | 5,139 | Vehicle/mech appearance; updateGeometry, render, shadow paths |
| mech3d.h | ~300 | Mech3DAppearance class def; appearance type (shapes, anims, nodes) |
| tgl.cpp | ~6,500 | TG_Shape::Render(); triangle submission loop; shadow collection |
| gameos_graphics.cpp | ~11,000 | Render-core loop; renderLists() flush; post-process; lighting setup |
| gos_tex_vertex.vert | 22 | Vehicle vertex shader; MVP (redundant); color/UV passthrough |
| gos_tex_vertex.frag | 10 | Vehicle fragment shader; texture * color modulation |
| shadow_object.vert | 17 | Dynamic shadow pre-pass; light-space transform |
| shadow_object.frag | ~10 | Shadow depth write (depth-only) |

---

## 10. Audit Sign-Off

**Date completed:** 2026-06-16  
**Auditor:** Claude Sonnet (render-path specialist)  
**Confidence level:** High (code walkthrough + empirical cost estimates)  
**Next steps:** Prioritize fix #1 (fog/light cache); gate #4 and #5 behind profiler data (MC2_HITCH_TRACE mission-split).
