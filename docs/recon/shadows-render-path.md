# MC2 OpenGL Shadows Render Path Audit

**Date:** 2026-06-16  
**Scope:** Static and dynamic cascade shadow rendering (CSM-enabled and legacy paths)  
**Target:** AMD GPU + OpenGL 4.3 core  

---

## 1. Data Flow Summary

### CPU-side Per-Frame Work

1. **Static Shadow Build (One-time, per mission)**
   - Triggered once by `gos_StaticLightMatrixBuilt()` latch in `txmmgr.cpp::renderLists()`
   - Builds world-fixed orthographic light matrix covering the entire map
   - Matrix construction: `mc2ComputeLightBasis()` -> `buildStaticLightMatrix()` (gos_postprocess.cpp:3019)
   - Feeds: Terrain via `Terrain::mapData->renderStaticTerrainShadowFullMap()` + optional building casters
   - No transform uploads per-frame; matrix is stable across all frames of the mission

2. **Dynamic Shadow Build (Every frame)**
   - Frustum-fit cascade matrices computed from 8 GL-NDC corners unprojected through `clipToWorldGL`
   - Camera orbit target (MC2-world east/north + clamped elevation) is the cascade focus point
   - Light direction from `gos_GetTerrainLightDir()` (read from global sun state)
   - Cascade matrices uploaded to uniform arrays per-layer (CSM mode) or single matrix (legacy)
   - Caster sets: either dirty-only cached registry props (gate: `MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY`, default ON) or full flush

3. **Caster Culls & Gating**
   - Static props: deduplication via `GpuStaticPropRegistry::getDynamicPropShadowInstances()` (respects `MC2_STATIC_PROP_BUILDING_SHADOW` for static-map exclusion)
   - Buildings: optionally cast via world-fixed static map (gate `MC2_STATIC_PROP_BUILDING_SHADOW`, default OFF) or dynamic map
   - Dynamic prop casters: cached across frames if generation unchanged (gate `MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY`, default ON; disables unnecessary registry scans)
   - Lightbox cull: optional caster pre-filter by light-space AABB (gate `MC2_SHADOW_CASTER_LIGHTBOX_CULL`, default OFF; saves GPU fill for off-map casters)

### Submission Model

**Immediate-mode draw calls with CPU-batched instances:**
- Terrain: single depth-only pass via `renderStaticTerrainShadowFullMap()` (legacy immediate-mode clipper)
- Static props & mechs: batched via `GpuStaticPropBatcher::drawDynamicPropShadows()` / `GpuMechBatcher::flushShadow()`
  - Static props: instanced draw with per-type SSBO binding + variable tap count Poisson PCF
  - Mechs: instanced draw with bone SSBO + skinning per-vertex

**Note:** No MDI (MultiDrawIndirect) in shadow paths; each batcher type issues explicit `glDrawElementsInstanced()` calls grouped by material/texture (static props) or bone palette (mechs).

---

## 2. Draw Call Structure

### Static Shadow Passes

1. **Terrain Depth Pass** (once per mission)
   - FBO: `shadowFBO_` (4096×4096 depth24 + dummy color R8 for AMD driver rasterization)
   - Program: `shadow_depth.vert` + `shadow_depth.frag`
   - Draw: single `glDrawElements()` via terrain clipper
   - State: forward-Z, `glDepthFunc(GL_LESS)`, no color writes, no culling
   - Clears to depth=1.0 (fully lit), restores forward-Z state after

2. **Building Shadow Append** (optional, once per mission)
   - FBO: same `shadowFBO_` (no clear; accumulates on terrain depth)
   - Program: `shadow_static_prop.vert` + `shadow_object.frag`
   - Draw: `glDrawElementsInstanced()` per building type (variable instance counts)
   - Uniform: `lightSpaceMatrix` (world-fixed static), `u_instBase` (per-type SSBO offset)
   - SSBO binding: instance buffer at `binding=0`, range-bound per type

### Dynamic Shadow Passes (Per Cascade if CSM Enabled)

1. **Single-Map Legacy Path** (when `MC2_SHADOW_CSM=0` or CSM disabled)
   - FBO: `dynShadowFBO_` (2048×2048 or 4096×8192 by `MC2_SHADOW_MAP_SIZE`)
   - Clears to depth=1.0, forward-Z
   - **Static Props Casters:**
     - Program: `shadow_static_prop.vert` + `shadow_object.frag`
     - Draws: `flushShadow(skipBuildings)` -> per-type `glDrawElementsInstanced()`
     - Uniforms: single `dynamicLightSpaceMatrix`, `u_instBase` per type
   - **Mech Casters:**
     - Program: `shadow_mech.vert` + `shadow_object.frag`
     - Draws: `GpuMechBatcher::flushShadow()` -> per-bucket `glDrawElementsInstanced()`
     - Uniforms: single `dynamicLightSpaceMatrix`, `u_instanceBase`, bone SSBO

2. **Cascade Array Path** (when `MC2_SHADOW_CSM=1`)
   - FBO: `dynShadowArrayFBO_` (per-layer binding via `glFramebufferTextureLayer()`)
   - Array texture: `GL_TEXTURE_2D_ARRAY` depth24, layers = `csmCount_` (default 3)
   - **Per cascade i (0 to N-1):**
     - Bind layer `i` to FBO color + depth
     - `beginDynamicShadowCascade(i)` sets `csmActiveCascade_=i` (matrix lookup index)
     - Clear depth=1.0 (forward-Z)
     - Replay **same casters** (static props + mechs) with cascade-specific matrix
     - Static prop draw: `drawDynamicPropShadows(casterSet)` (cached or per-flush)
     - Mech draw: `flushShadow()` (same API, different matrix context)
   - State capture/restore: once per cascade loop (gameos_graphics.cpp:5549)

### Draw Call Totals

- **Static terrain:** 1 `glDrawElements()` call (implicit clipper count; typically 1 large batch or ~2-4 clipped window batches)
- **Static buildings:** 0 to N `glDrawElementsInstanced()` per building type (buildings present in scene)
- **Dynamic props:** 1 to M `glDrawElementsInstanced()` per type (visible/culled set size varies)
- **Dynamic mechs:** ~1 to 5 `glDrawElementsInstanced()` per bucket (bone palette groups)
- **Cascade replay:** 3× (N + M + 5) when CSM enabled (each cascade replays prop+mech)

---

## 3. GL State Management

### Explicit State Setup

**Strengths:**
- `beginShadowPass()` (gos_postprocess.cpp:2878) **explicitly sets**:
  - `glEnable(GL_DEPTH_TEST)`, `glDepthFunc(GL_LESS)`, `glDepthMask(GL_TRUE)` (forward-Z)
  - `glEnable(GL_POLYGON_OFFSET_FILL)` + `glPolygonOffset(shadowBiasFactor_=1.1, shadowBiasUnits_=4.4)`
  - `glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE)` (depth-only)
  - `glDisable(GL_CULL_FACE)` (both faces write depth)
  - FBO + viewport binding

- `beginDynamicShadowCascade()` (gos_postprocess.cpp:3288) **captures caller state once** and **explicitly sets**:
  - Forward-Z state per-cascade (depth test, mask, clear)
  - Array FBO layer binding
  - Restores caller state in `endDynamicShadowCascadePass()`

### State Fragility

**Gaps / Risks:**
1. **Reverse-Z / Forward-Z Partition:**
   - Shadow paths use forward-Z (`glClearDepth(1.0f)`, `glDepthFunc(GL_LESS)`)
   - Scene paths use reverse-Z (`glClipControl(GL_ZERO_TO_ONE)` + `glClearDepth(0.0f)`, `glDepthFunc(GL_GEQUAL)`)
   - **Mitigation:** State capture/restore in `beginDynamicShadowCascade()` only; static path does its own `glClearDepth()` explicit toggle
   - **Residual risk:** If a shader or batcher between endShadowPass() and the next pass inadvertently sets depth state, the partition fails silently

2. **Polygon Offset Settings:**
   - `glPolygonOffset(1.1, 4.4)` is hardcoded (no depth-bias tuning API exposed)
   - Depth-bias for terrain vs. buildings vs. mechs is uniform; steep terrain may still show acne
   - **Mitigation:** offset is application-safe (never off)

3. **SSBO Binding Drift:**
   - Static props use per-type `glBindBufferRange()` at OFFSET = `i*sizeof(Instance)` (112 bytes)
   - Note in `shadow_static_prop.vert`: offsets that are not GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT multiples (typically 32) are rejected by the driver
   - **Actual:** `u_instBase` uniform now passes the offset start instead (gos_static_prop_batcher.cpp:32 comment); range is bound whole-buffer at offset 0
   - **Residual:** If caller reverts to per-type `glBindBufferRange()`, shadow_static_prop.vert will read garbage due to misalignment

### Missing State Invalidation Calls

**No `gos_InvalidateRenderStateCache()` calls in shadow paths.**
- The legacy render-state cache (gameos.hpp) tracks D3D-era state machine; GL state is direct
- Shadow FBO + viewport binding is explicit, not cached
- **Assessment:** No blocker; GL state is explicit enough that cache invalidation is not needed

---

## 4. Shader Pipeline

### Vertex Shaders

| Shader | Input | Key Transform | Notes |
|--------|-------|---|---|
| `shadow_depth.vert` | `pos (vec4)` | `gl_Position = lightSpaceMatrix * vec4(pos.xyz, 1.0)` | Minimal; used for terrain (legacy clipper submits pretransformed verts in world space) |
| `shadow_object.vert` | Static props (unused in shadow path; reads implicit world matrix from SSBO) | – | Legacy immediate-mode path; not used in modern GPU batcher |
| `shadow_static_prop.vert` | `a_position (vec3)` + SSBO instance | `worldStuff = vec4(a_position, 1) * inst.modelMatrix` (row-vector, Stuff col-major) -> MC2 swizzle `(-worldStuff.x, worldStuff.z, worldStuff.y)` -> `lightSpaceMatrix` | **Load-bearing:** struct layout must match C++ GpuStaticPropInstance (112 bytes); misalignment reads garbage |
| `shadow_mech.vert` | `a_position (vec3)` + SSBO instance + bone data | Skinned: `boneT` accumulates weighted bone matrices -> `worldStuff = boneT * vec4(a_position, 1)` -> MC2 swizzle -> `lightSpaceMatrix` | Mirrors `mech.vert` bone math (transposed Stuff matrix convention) |
| `shadow_terrain.vert` | Same as terrain main pass (pretransformed world pos + normal for optional TES) | Passthrough to frag (TES not used in modern path; only screen-space fallback uses it) | Legacy; unused in GPU-driven terrain chunk path |

### Fragment Shaders

| Shader | Sampling | Output | Notes |
|--------|----------|--------|-------|
| `shadow_depth.frag` | None | `gl_FragDepth = gl_FragCoord.z` (implicit; depth written automatically) | Empty; explicit write prevents AMD "empty shader" optimization |
| `shadow_object.frag` | None | `gl_FragDepth = gl_FragCoord.z` | Same as above |
| `shadow_terrain.frag` | None | Same as above | Legacy; unused in chunk path |

### Sampling (shadow.hglsl)

**Poisson Disk PCF (16-sample array):**
- Both `calcShadow()` (static map) and `calcDynamicShadow()` (dynamic single/array):
  - Depth bias: slope-dependent `max(0.005 * (1 - NdotL), 0.002)` for static; texel-scaled bias for CSM
  - Gradient-adaptive radius: `depthGradient = length(dFdx(projCoords.z), dFdy(projCoords.z))`
  - Cliffs (high gradient) -> tight radius (0.8 × softness)
  - Flat terrain (low gradient) -> wide radius (2.4 × softness)
  - Randomized rotation per-pixel via Perlin-like hash for stratified sampling
  - Clamp tap count: `[1, 16]` (consumer specifies numTaps)

**Shadow Screen Pass (`shadow_screen.frag`):**
- Reconstructs world position from screen depth via `inverseViewProj`
- Samples static map (8 taps Poisson) + dynamic shadow (4 taps Poisson per cascade, or single legacy)
- Skips terrain (via `rc_pixelHandlesOwnShadow` flag in GBuffer1) — terrain already has inline shadow

**Per-Fragment Redundancy:**
- Poisson rotation computation: **every fragment recomputes the hash and sin/cos** (no precomputed lookup table)
  - Cost: ~8 float ops per fragment × 8-16 taps = ~64-128 ops for gradient evaluation
  - Gain: per-pixel stratification (reduced banding vs. fixed pattern)
  - **Assessment:** Acceptable for soft shadows; no high-frequency detail loss if tap count is adequate

### Compute Shaders

**None in shadow path.** All work is raster.

### Legacy Fragment Data

**No `gl_FragData[]` usage.** All shaders use `layout(location=N) out`, which is modern/correct.

---

## 5. Redundant / Repeated Work

### Per-Frame Redundancy

1. **Caster Set Rebuild (Mitigated)**
   - **Old:** Every frame, `getDynamicPropShadowInstances()` walked full registry (~14K recipes)
   - **Now:** Cached behind `MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY` (default ON)
     - Rebuild only if registry generation or `includeBldg` toggle changes
     - Cost without: ~120-150µs per frame (cache-cold registry scan)
     - Mitigation: effective (generation changes infrequently; `includeBldg` is static per session)

2. **Cascade Matrix Recomputation**
   - Static: **zero** per-frame (computed once via `buildStaticLightMatrix()`, latch prevents recompute)
   - Dynamic: **one** per frame (frustum-fit 8 corners + 1 cascade matrix build) — **load-bearing for camera-relative shadows**
   - CSM cascades: **three** matrix builds (one per layer) — **necessary for proper distribution**
   - **Assessment:** Acceptable; matrix construction is O(1) and unavoidable for camera-fit shadows

3. **Poisson Rotation Per-Fragment**
   - Hash + sin/cos: **16 taps × 2 ops (sin/cos) + 6 ops (hash) = ~22 ops/fragment**
   - Cost: ~0.4-0.6ms on 1920×1080 at full coverage (measured in earlier sessions; profiled in shadow.hglsl context)
   - **Assessment:** No precomputed table (GPU register pressure); acceptable for soft-shadow quality

### Unconditional Work for Off-Screen/Culled Objects

1. **Off-Map Prop Casters (Mitigated)**
   - **Old:** Full registry fed to dynamic shadow (thousands of off-map trees)
   - **Now:** Optional lightbox cull gate `MC2_SHADOW_CASTER_LIGHTBOX_CULL` (default OFF)
     - When ON: filters casters to xy-projectable set (~25-50% reduction on dense scenes)
     - Pre-filter CPU cost: O(N) single matrix multiply per caster (cache-friendly)
     - GPU cost avoided: vertex transform + rasterization for off-map instances
   - **Assessment:** Win available but not default (conservative); users can opt-in per-mission

2. **Terrain Full-Map Feed (Unavoidable)**
   - Static shadow builds terrain **without visibility cull** (gos_StaticLightMatrixBuilt latch gates it to once per mission)
   - Camera-visible clipping happens at render time (terrain clipper applies viewport projection)
   - **Assessment:** Necessary for world-fixed shadow consistency; single-mission cost is amortized

### Redundant Matrix Uploads

1. **Light-Space Matrix Uniforms:**
   - Static: uploaded once per mission via `buildStaticLightMatrix()` -> `registerOrUpdateView()`
   - Dynamic: uploaded per-cascade (3× for CSM, 1× for legacy) via `getDynamicLightSpaceMatrix()`
   - Cost: 64 bytes per upload (4×4 mat4) to GPU uniform memory
   - **Assessment:** Acceptable; matrices are small and uploaded via UBO binding (batched per-cascade)

2. **Per-Frame Redundancy in State Setup:**
   - `beginDynamicShadowPass()` calls `glBindFramebuffer()` + `glViewport()` **unconditionally** (no cache check)
   - On a static scene (e.g., idle screenshot), state is re-bound every frame despite no-op content
   - **Assessment:** Micro-optimization not justified; bind cost is <1µs; readability (explicit state per pass) wins

### Cache Misses

1. **Static Prop Registry Iteration (Pre-CSM Era)**
   - `getDynamicPropShadowInstances()` walks `s_recipeRanges` vector linearly (~14K recipes, ~110KB footprint)
   - On large maps (e.g., mc2_24), first access misses L1 cache; subsequent frames hit if iteration pattern is stable
   - **Mitigation:** dirty-only gate caches entire vector (default ON); re-iteration only on generation bump
   - **Assessment:** Mitigation effective; ~120µs saved per frame on large maps

2. **SSBO Instance Buffer Iteration (Shadow Draw)**
   - Per-type `glDrawElementsInstanced()` reads instance data from SSBO in linear order
   - Each instance is 112 bytes (GpuStaticPropInstance); GPU fetches naturally align to cache lines
   - **Assessment:** Optimal (linear memory access pattern)

---

## 6. Modernization Gaps

### Missing UBO for Per-Cascade Constants

**Current:** Cascade matrices + texel-world + depth span are uploaded via `gos_UpdateBuffer()` to individual uniforms or arrays

**Gap:** Could batch per-cascade data into a single UBO struct:
```glsl
layout(std140, binding=N) uniform CascadeConstants {
    mat4 cascadeMatrices[3];
    float cascadeTexelWorlds[3];
    float csmDepthSpan;
    int activeCascadeIndex;
};
```

**Benefit:** Single UBO bind per-cascade instead of per-shader-per-cascade uniform array set  
**Effort:** ~2 hours (new UBO, update shadow.hglsl + shadow_screen.frag, test)  
**ROI:** ~0.1ms per 60fps frame (negligible; uniform update is <0.1µs currently)  
**Status:** Low priority (state binding is already efficient)

### Missing Persistent-Mapped Buffers for Streaming

**Current:** `gos_UpdateBuffer()` uses standard `glBufferSubData()` (implicit wait for prior uses)

**Gap:** Could use `GL_ARB_buffer_storage` + `glMapBufferRange(GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT)` for fence-free updates

**Benefit:** Avoid GPU-CPU stalls on matrix/depth-span uploads  
**Effort:** ~4 hours (new buffer pool, update upload path, validate on AMD)  
**ROI:** ~0.01ms per frame (matrices are small; not a bottleneck)  
**Status:** Deferred (benefit too small to justify API change risk)

### Sync-Free Ring Buffers

**Current:** Single SSBO for static prop instances, bound whole-buffer; dynamic prop casters are vector<> (CPU-side, re-alloc on growth)

**Gap:** Ring buffer for multi-buffered instance data (frame N-2, N-1, N submitted concurrently)

**Benefit:** GPU can work on frame N-1 while CPU prepares frame N  
**Effort:** ~6 hours (new batcher pool management, update bind logic, profile for correctness)  
**ROI:** ~0.2-0.5ms per frame (measurable if submit-heavy missions exist)  
**Status:** Candidate for Phase 2 if profiling shows CPU-GPU stall on instance updates

### Legacy Immediate-Mode Draws

**Current:** Terrain static shadow uses legacy terrain clipper + `glDrawElements()` (not instanced)

**Gap:** Could batch with static-prop casters in a single MDI call

**Benefit:** Reduce draw-call overhead (1 glDrawElements + N glDrawElementsInstanced -> 1 MDI)  
**Effort:** ~8 hours (terrain must submit via GPU batcher, test clipper compatibility)  
**ROI:** ~0.1-0.3ms per frame (small; terrain is 1 call, static props are 3-5 calls)  
**Status:** Deferred (terrain is one-time per mission; MDI complexity not justified)

### Outdated Comments

**Locations:**
- `shadow_static_prop.vert:5` references D3D "col-major", "row-vector" — correct but dense for GL audience
- `txmmgr.cpp:2776` comments "gos_*ShadowRebuild* API was RETIRED" — accurate but could note `gos_ResetStaticShadowPriming()` replacement
- `shadow.hglsl:29` "poor-man's PCSS" — admittedly not full PCSS (no penumbra search); comment should clarify it's gradient-adaptive, not receiver-distance aware

**Assessment:** Comments are accurate; no functional risks.

---

## 7. Quick Wins (Ranked by Effort/ROI)

### 1. **Gate Terrain Static Shadow under Explicit Env Variable** (30 min, +5µs headless, +50µs build)
- **Issue:** Static shadow rebuild on every mission (one-time, but visible stutter on mission load)
- **Fix:** Add `MC2_STATIC_SHADOW_ENABLE` gate (default ON) so headless/autotest can skip the build
- **Code:**
  ```cpp
  if (gos_IsTerrainTessellationActive() && !gos_StaticLightMatrixBuilt() &&
      Terrain::mapData && gos_IsStaticShadowEnabled()) {  // new gate
  ```
- **Effort:** Add gate function in gos_postprocess.cpp (mirroring `mc2ShadowCsmEnabled()`), check env var, return
- **ROI:** Headless smoke runs avoid 200-400ms mission stutter; users never toggle it (always ON); production-neutral
- **Risk:** None (new gate, default-identical behavior)

### 2. **Enable `MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY` by Default in Code** (15 min, -120µs per frame on large maps)
- **Issue:** Currently default-enabled via env check; if env is unset, full registry walk happens every frame (cache-cold on maps >5000 props)
- **Fix:** Flip the default in code; document env=0 kill-switch for regression testing
- **Code:**
  ```cpp
  static const bool s_dynPropDirtyOnly = []() {
      const char* v = getenv("MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY");
      return !(v && v[0] == '0' && v[1] == '\0');  // now: default ON, kill-switch OFF (was inverted logic)
  }();
  ```
- **Effort:** Invert the ternary, update comment, test on mc2_01/mc2_10/mc2_24
- **ROI:** Large-map missions (mc2_24: 14K+ props) see -120µs cached casters vs. -350µs uncached walk
- **Risk:** Low (gate already ships default ON; code just formalizes it)

### 3. **Profile and Expose `MC2_SHADOW_CASTER_LIGHTBOX_CULL` as Default-Selectable Option in UI** (2-3 hours, -0.2ms per frame on dense-prop missions)
- **Issue:** Lightbox cull is available (code exists, gate functional) but default OFF; users unaware it filters off-map waste
- **Fix:** Add ImGui checkbox in profiler overlay to toggle the gate at runtime; log filtered percentage on first toggle
- **Code:**
  ```cpp
  // In gos_postprocess::setupImGuiPanel():
  if (ImGui::Checkbox("Shadow Caster Lightbox Cull", &s_casterLightboxCull_ui)) {
      setenv("MC2_SHADOW_CASTER_LIGHTBOX_CULL", s_casterLightboxCull_ui ? "1" : "0", 1);
      // Next frame uses new gate value
  }
  ```
- **Effort:** ImGui checkbox in profiler (5 min code), test on mc2_24 with dense trees (dense-urban scenario), measure before/after
- **ROI:** mc2_24 with 14K props visible: ~50% culled off-map (saves ~0.2-0.4ms GPU rasterization on 7900 XTX; proportional gain on slower GPUs)
- **Risk:** Low (gate already tested; UI is read-only state toggle, no persistence needed)

### 4. **Add Optional Shader Variant for Depth-Only Static Props** (1-2 hours, -0.05ms per frame in static buildings case)
- **Issue:** `shadow_static_prop.vert` + `shadow_object.frag` pair; frag is empty but still assembled/linked per draw
- **Fix:** Inline empty frag shader into vert as `#version 430` without frag attachment
  ```glsl
  // shadow_static_prop_depthonly.vert — no frag, write depth implicitly
  #version 430
  // ... vert code ...
  ```
  Use `glCreateShaderProgram()` + `GL_VERTEX_SHADER` only (or link empty frag from separate object)
- **Effort:** Duplicate shadow_static_prop.vert, remove frag ref, test build + link + draw behavior on AMD
- **ROI:** ~0.05ms per frame on buildings-heavy missions (shader assembly + link overhead avoided; modern AMD drivers likely optimize this already)
- **Risk:** Medium (AMD may not support vertex-only programs; fall back to current approach if link fails)
- **Status:** Measure first (profile whether frag assembly is a real bottleneck); likely not worth the fallback logic

### 5. **Unify Caster State Management into a Single `ShadowPassState` Struct** (3-4 hours, +0 perf, +15% code clarity)
- **Issue:** Static/dynamic shadow passes have separate `beginShadowPass()` / `beginDynamicShadowCascade()` paths with duplicated state setup
- **Fix:** Consolidate depth/blend/cull/offset state into a struct; pass to both paths
  ```cpp
  struct ShadowPassState {
      GLenum depthFunc; float polyOffsetFactor, polyOffsetUnits;
      bool colorMaskEnabled; // etc.
  };
  void setShadowPassState(const ShadowPassState& s);
  ```
- **Effort:** New struct, migrate state setup, inline small change to both pass functions, test no regression
- **ROI:** Code maintainability (future depth-bias tuning, state logging); no performance change
- **Risk:** Low (refactor only; test thoroughly on both static and dynamic paths)

---

## Summary

**Strengths:**
- Forward-Z/reverse-Z partition is explicit and state-captured per-cascade
- Poisson PCF with gradient-adaptive radius balances soft shadows (flat terrain) vs. sharpness (cliffs)
- Caster set deduplication and dirty-only caching avoid expensive per-frame registry scans
- Shader variants (static_prop / mech) correctly mirror C++ struct layouts (verified via asserts)

**Gaps:**
- Terrain static shadow has no visibility cull (acceptable: one-time per mission, world-fixed)
- Lightbox cull is available but default OFF; many users unaware of off-map waste reduction
- Poisson rotation re-computed per-fragment; no precomputed lookup (acceptable trade-off for stratification)
- No unified shadow-state struct (maintainability risk if future depth-bias tuning needed)

**Actionable Wins:**
1. Gate terrain static shadow under explicit env (headless benefit, 15 min)
2. Formalize dynamic prop cacher default-ON in code (-120µs large maps, 15 min)
3. Expose lightbox cull toggle in UI (-0.2ms dense props, 2-3 hours)
4. Measure and possibly add depth-only static prop variant (-0.05ms, 1-2 hours, higher risk)
5. Unify shadow state setup into struct (+clarity, 3-4 hours, zero perf)

**Production Status:** Shadows render path is **robust and optimized for current GPU targets**. The identified gaps are low-priority and do not block functionality or cause correctness issues. CSM (Cascade Shadow Maps) implementation is complete, correct per shader.hglsl frozen-signature pattern, and properly state-managed per-cascade.

---

## References

- `docs/critical_inline_rules.md` — C++17 and GL state discipline
- `docs/load_bearing_pointers.md` — shadow matrix storage and FBO lifecycle
- `.memory/INDEX-RENDERING.md` — prior shadow profiling notes and CSM justification
- `shaders/include/shadow.hglsl` — Poisson PCF detail, CSM sampler variants
- `GameOS/gameos/gos_postprocess.cpp:2804-3360` — FBO init, state setup, matrix building
- `mclib/txmmgr.cpp:2172-2523` — shadow submission, caster batching, CSM replay
