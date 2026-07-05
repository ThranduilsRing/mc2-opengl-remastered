# Static Props Render Path Audit

**Date:** 2026-06-16  
**Scope:** GPU-batched static props (trees, buildings) pipeline  
**Coverage:** CPU submission, MDI dispatch, GL state, shaders, redundancy analysis  

---

## 1. Data Flow Summary

### Per-Frame CPU Work
1. **GpuStaticPropRegistry::flush()** (gos_static_prop_registry.cpp:871–1000+)
   - Iterates `s_liveRangeIndices` (actors culled to frustum)
   - Skips tombstoned recipes (`count == 0`)
   - Staleness gate: checks `multi->getCachedFrame() == currentFrame` (offscreen actors skipped)
   - **Per-instance light patching:** reads `freshLightIdx` from multi->cachedGpuLightIndex() or per-instance capture (MC2_STATIC_PER_INSTANCE_LIGHT=1)
   - **Two submit paths:**
     - **Legacy (default):** per-leaf rebuild → `batcher.submitCachedInstance(inst)` with patched lightDataIndex
     - **Cached blob (MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1 or persistent-buckets gate):** bulk append to persistent/per-frame SSBO if light index matches stored value
   - **Frozen-records mode (MC2_GPU_CULL_STATIC_FROZEN_RECORDS=1):** skips per-frame instance append; uses pre-built static store
   - **GPU cull submission:** `substrate_appendStaticPropRecord()` per-leaf (C1b authority flip)

2. **GpuStaticPropBatcher::flush()** (gos_static_prop_batcher.cpp)
   - Receives per-frame instance SSBOs (either dynamic bucket or persistent static)
   - **Bucket building:** per-type instance lists organized by typeID
   - **Color SSBO upload:** frame-local colors zeroed per-frame (debug path)
   - **Snapshot fill/dirty-only:** ExtractRenderSnapshot → static-prop extraction (99.7% cut via dirty-only cache when registry generation clean)
   - **Two dispatch paths:**
     - **v6 (default):** glMultiDrawElementsIndirect via sorted packet order (coalesce variant)
     - **Legacy (MC2_STATIC_PROP_LEGACY_DISPATCH=1):** one glDrawElementsInstancedBaseVertex per type
   - **State management:** no explicit GL_DEPTH_TEST/blend/cull guards → inherits from prior pass (fragile, see issues below)

3. **Shadow passes:**
   - **flushShadow():** per-frame visible types depth-only (dynamic shadow FBO)
   - **drawStaticBuildingShadows():** all registered buildings (visibility-independent, static light-space matrix)
   - **drawDynamicPropShadows():** registry-supplied non-buildings depth-only (per-frame, dynamic light matrix)

### Submission Model
- **CPU-batched grouping** per typeID; GPU does visibility/frustum cull (MC2_GPU_CULL_SUBSTRATE=default-ON)
- **Indirect mode:** glMultiDrawElementsIndirect(s_cmdBuffer, typeCount) under v6 path
- **Persistent static buckets (MC2_STATIC_PROP_PERSISTENT_BUCKETS=default-ON):** reuses per-frame static SSBO across frames unless registry generation bumps
- **Snapshot cull (MC2_SNAP_CULL=1):** prior-frame instance count gates per-slot draw (weak optimization)

---

## 2. Draw Call Structure

### Number of Draws
- **v6 path (default):** one `glMultiDrawElementsIndirect` call encompassing ALL packets
  - Packet count = `batcher_getSortedPacketCount()` (varies by alpha grouping and registration)
  - Instance count per packet = `s_typeRanges[typeID].visibleCount` (zero-culled by GPU shader in compute path)
  - Typical tier1 mission: ~20–50 visible types, 40–120 packets total

- **Legacy path:** one `glDrawElementsInstancedBaseVertex` per type (N = type count, typically 20–50 calls)

### Batching Quality
- **Coalesced by sorted packet order:** packets grouped alpha-OFF then alpha-ON
- **Per-packet MDI params:** firstIndex, indexCount, baseVertex, instanceCount all encoded in draw-command SSBO (s_cmdBuffer, binding 7)
- **No per-object draw calls:** eliminated via MDI and instance arrays
- **Instance indexing:** `gl_BaseInstanceARB + gl_InstanceID` (ARB extension required, GL 4.2+)

### Coalesce State (v3.8)
- **PerDrawEntry SSBO (binding 4):** one entry per sorted packet; indexed by `gl_DrawIDARB` in fragment shader
- **Variant modes:**
  - Opaque (MC2_COALESCE, no alpha): single MaterialGpu table (binding 5)
  - Alpha-test: separate table per group OR shared table with per-entry flags
- **Draw-packet reuse:** `s_snapV6Packets[]` / `s_snapV6Meta[]` rebuilt or reused each frame

---

## 3. GL State Management

### Critical Gap: No Explicit State Guards
**Current behavior:** static_prop flush() does NOT call `gos_InvalidateRenderStateCache()` or explicitly set:
- `glEnable(GL_DEPTH_TEST) / glDepthMask(GL_TRUE)`
- `glEnable/glDisable(GL_BLEND)` (opaque = GL_BLEND OFF)
- `glCullFace(GL_BACK)` and `glEnable(GL_CULL_FACE)` (must cull CCW if mesh is CCW)
- `glDepthFunc(GL_GEQUAL)` (reverse-Z contract; static_prop uses reverse-Z per terrain)

**Risk:** If prior render pass (e.g., transparent post-process, UI, prior static-prop color) left depth-write disabled or blend enabled, static props render with:
- **No depth occlusion** (props see through to skybox)
- **Alpha-blended appearance** when should be opaque
- **Example:** terrain LOD chunk fixed this in `f375e0ba` (explicit state guards added mid-flush per comment)

**Fix:** Before first draw in flush(), call:
```cpp
glEnable(GL_DEPTH_TEST);
glDepthMask(GL_TRUE);
glDisable(GL_BLEND);
glDepthFunc(GL_GEQUAL);  // or GL_LEQUAL if forward-Z
gos_InvalidateRenderStateCache();  // invalidate cache so next caller doesn't inherit
```
After flush(), restore prior state via `gos_SaveRestoreRenderState` RAII guard (see terrain path for pattern).

### Pipeline Binding
- `applyPipeline(PipelineDesc)` set via pipeline_binder.h (queries RenderCore::PipelineRegistry)
- Ensures correct vertex layout, attribute bindings, sampler units for the pipeline
- **Gap:** no explicit cull-face direction validation (static props assume CCW winding order from original meshes)

### Render Contract
- **Pass:** StaticProp (opaque) / StaticPropAlphaTest
- **Color0 output:** RGBA (opaque for Color0, alpha-tested for AlphaTest variant)
- **GBuffer1:** `rc_gbuffer1_screenShadowEligible()` (normal-based shadow eligibility)
- **Depth:** write (GL_EQUAL in shadow passes, GL_GEQUAL in color)
- **Blend:** off for opaque, alpha-test for alpha variant

---

## 4. Shader Pipeline

### Vertex Shader (static_prop.vert, 499 lines)

**Key stages:**
1. **Instance indexing:** `instances_.i[gl_BaseInstanceARB + gl_InstanceID]` (coalesce) or `[gl_InstanceID]` (legacy)
2. **Transform stack:**
   - Position: `v * M` row-vector form (Stuff convention) → MC2 world via axis-swap (Stuff x/y/z → MC2 east/north/elev)
   - Normal: `a_normal * mat3(M)` (row-vector to preserve rotation; inverse-transpose avoided as non-uniform scale assumed absent)
3. **GPU lighting (Stage 2.C.2):** reads `inst.lightDataIndex` → `ObjectLights[lightDataIndex]` UBO → `calc_light()` per-vertex
   - Decodes per-vertex `a_aRGBLight` tag into BGRA vec4
   - Per-type hot-color SSBO (binding 2) for magic-color decode
   - Skips lighting for window nodes (hot-color only, no directional/ambient)
   - **Performance note:** reading full ObjectLights entry (up to 6 lights × 16 bytes) per instance; no vL pre-caching
4. **SH-L2 ambient (V-IBL-STATIC-1):** `evalShL2(worldNormal)` if gate enabled (MC2_STATIC_PROP_IBL_SH_STRENGTH env)
5. **Hemisphere ambient (V-AMBIENT-STATIC-1):** subtle fill on non-window nodes
6. **Highlight addition:** additive per-instance tint (clamped to [0,1])
7. **PBR pre-compute (V-MATERIAL-PBR-3):** scans ObjectLights for INFINITE sun light → forwards (sunDir, sunColor, sunFound) flat varyings to frag

**Redundant work:**
- Full `ObjectLights` read per instance (per-vertex lit) even when lighting is identical across all instances of one type. No per-type light cache.
- Normal-to-world transform `a_normal * mat3(M)` done per-vertex; could pre-compute per-instance if geometry is shared.

### Fragment Shader (static_prop.frag, 445 lines)

**Key stages:**
1. **Coalesce mode:** resolve PerDrawEntry (binding 4) via `v_drawID + u_drawIDBase` → material flags, packet ID, texArrayLayer, uvScale
2. **Alpha-test:** if ALPHA_TEST_BIT set and sampled alpha < 0.5, discard
3. **Debug material mode (0–8):** if `u_debugMaterialMode != 0`, emit debug visualization (gradient, palette hash, normal, white, argb-only, tex-only, highlight-only)
4. **Per-fragment PBR (V-MATERIAL-PBR-3, MC2_USE_VIEW_UNIFORMS only):**
   - Schlick-Fresnel + power-lobe specular (requires per-fragment view vector from `u_cameraWorldPos`)
   - Converts N and L to GL world space (Stuff→GL axis swap to preserve light-relative geometry)
   - Reads material roughness/metallic from MaterialGpu or defaults (metallic=0, roughness=1.0)
   - Scales by `u_pbrV1Strength` (env override or ImGui slider, default 0.5)
5. **Final composite:** tex_color × lit + highlight + PBR specular, fog mix

**Redundant work:**
- Texture sampling occurs unconditionally even in pure-debug modes (modes 1–8 sample but may not use result)
- Material scalar reads (roughness, metallic) done per-fragment even when adjacent fragments share material (no per-draw material cache at shader compile time)
- View vector reconstruct per-fragment (V_eye = u_cameraWorldPos - v_worldPos) on every fragment; could pre-compute per-instance if camera static (rare)

### Shadow Vertex Shader (shadow_static_prop.vert, 37 lines)

- Minimal: position transform + lightSpaceMatrix, no lighting
- `u_instBase` uniform to handle per-type SSBO binding alignment (GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT = 32, instance stride = 112 bytes)
- **Performance note:** straightforward depth-only path; no over-fetches

---

## 5. Redundant / Repeated Work

### CPU Side (Registry Flush)
1. **Per-instance light resolution:** `rng.multi->getCachedGpuLightIndex()` called per visible range, but value is constant for all instances of the same multi-shape. **Fix:** cache at registration or per-multi, not per-instance.
2. **CullRecordVersion bump on every flush:** `s_cullRecordVersion++` even when same props submitted (line 201 + fence at 1000+). **Impact:** snapshot bridge-compare re-fires even on identical frames. **Fix:** only bump when `s_recipeHasSubstrateRecord` changes from prior frame.
3. **Persistent-buckets rebuild:** cleared and re-filled even when `s_persistentBuckets && registryGen clean`. **Fix (already in code):** guarded by `s_storeDirty` check at line 900.

### GPU Side (Shaders)
1. **ObjectLights SSBO read per-vertex:** lighting.hglsl `light[inst.lightDataIndex]` read in vert shader; ObjectLights struct is ~32 bytes (type+count+up-to-6 lights), streamed from SSBO. Multi-instance type submits the same light index → cache miss on every instance. **Fix:** pre-compute per-type, store in type-data SSBO (binding 2 already hosts hot-color, could extend).
2. **Normal transform per-vertex:** `a_normal * mat3(M)` done in vert shader even when all instances of one type share the same orientation (common for trees). **Fix:** pre-compute rotation matrix per-type, store as 9 floats, apply in vert.
3. **Texture sampling in pure-debug paths:** fragments in mode 1/2/3/4 still call `texture(u_texArr, ...)` even though result isn't used. **Fix:** move sampling into `if (u_debugMaterialMode == 0)` guard.
4. **Material scalar read per-fragment:** `materialTable_.materials[materialIdx].roughness/metallicFactor` read from binding 5 every pixel, even though PBR is disabled by default (u_pbrV1Strength=0.0 short-circuits, but read still happens). **Fix:** short-circuit the entire PBR block before any GPU reads.
5. **Coalesce PerDrawEntry re-read:** `perDraw_.entries[v_drawID + uint(u_drawIDBase)]` accessed 6+ times in main() to get materialFlags, packetID, maxLocalVertexID, texArrayLayer, uvScaleX, uvScaleY. Compiler may not coalesce these into one load. **Fix:** read once into local struct, reuse fields.

### Buffer Uploads
1. **Color SSBO zero-fill:** `submitCachedInstance` path zeros per-frame color block even though debug path is off by default. **Fix:** gate zero-fill on `MC2_STATIC_PROP_DEBUG_COLOR_BLOCK env` or remove if debug path unused.
2. **Per-type bucket re-upload:** legacy path (pre-persistent-buckets) rebuilds per-type instance SSBO every frame. **Fix (already enabled by default, MC2_STATIC_PROP_PERSISTENT_BUCKETS=1):** reuse across frames when registry clean.

---

## 6. Modernization Gaps

### Missing Patterns
1. **UBO for per-frame constants:** view-uniform, PBR uniforms, ambient strength all uploaded as individual glUniform* calls. **Better:** pack into a single UBO (64B for view, PBR, ambient), upload once per frame.
2. **No persistent-mapped buffers:** instance SSBO rebuilt/re-uploaded every frame (even under persistent-buckets, color SSBO refreshed). **Better:** ring-buffered persistent-mapped SSBO with sync fences (MC2_GPU_CULL_SUBSTRATE pattern).
3. **No sync-free ring-buffer for dynamic data:** per-frame instance lists use `glBindBufferRange(GL_COPY_WRITE_BUFFER, offset)` into a shared pool. **Better:** fence-synchronized ring with N frames of storage, MapBufferRange with GL_MAP_UNSYNCHRONIZED_BIT.
4. **Legacy draw-dispatch abstraction:** v6 path uses `glMultiDrawElementsIndirect` (good), but fallback to v5 (single glDrawElementsInstancedBaseVertex per type) still in codebase. **Better:** require v6, retire v5 path entirely.
5. **Per-fragment material reads:** MaterialGpu table (binding 5) accessed per-pixel for roughness/metallic even when 99% of static props use default (metallic=0, roughness=1.0). **Better:** bake defaults into shader literal, read MaterialGpu only when explicitly flagged (MC2_STATICPROP_PBR_SLOTS).

### Outdated Comments/Patterns
- Line 242 in static_prop.vert: "modelMatrix from SSBO std430 default col-major" — correct, but no mention that Stuff convention is row-vector (v*M), not column-vector (M*v).
- Line 34 in shadow_static_prop.vert: "SSBO-BIND-ALIGN" comment explains alignment workaround; good defensive note, but the proper fix (bind whole buffer at offset 0, use u_instBase) is non-obvious from code alone.

---

## 7. Quick Wins (Ranked by ROI)

### 1. **Add explicit GL state guards to flush()** ⭐⭐⭐ **Critical**
**Effort:** 1–2 hours  
**Gain:** fixes "transparent static props" and "props rendering on top of UI" bugs (likely lurking in dark missions or post-effect sequences)  
**Code pattern (from terrain_lod_chunk):**
```cpp
gos_SaveRestoreRenderState saver;  // RAII guard
glEnable(GL_DEPTH_TEST);
glDepthMask(GL_TRUE);
glDisable(GL_BLEND);
glDepthFunc(GL_GEQUAL);
gos_InvalidateRenderStateCache();
// ... flush MDI calls here ...
// RAII ~dtor restores prior state
```

### 2. **Coalesce PerDrawEntry reads into single load**
**Effort:** 30 minutes  
**Gain:** 1–2 fewer L1 cache misses per fragment, subtle 1–3% throughput on dense props  
**Fragment shader:**
```glsl
#ifdef MC2_COALESCE
PerDrawEntry pde = perDraw_.entries[v_drawID + uint(u_drawIDBase)];
int materialFlags    = pde.materialFlags;
int texArrayLayer    = pde.texArrayLayer;
float uvScaleX       = pde.uvScaleX;
// ... reuse pde.* for all fields ...
#endif
```

### 3. **Cache ObjectLights read per-type**
**Effort:** 2–3 hours (requires per-type SSBO extension)  
**Gain:** ~5–10% reduction in SSBO memory pressure for tree/building-heavy missions (many instances, few unique light sets)  
**Pattern:** store 1 ObjectLights entry per type in a new binding (slot 6), index by `inst.typeID` instead of per-instance `inst.lightDataIndex`.

### 4. **Pre-compute per-type rotation matrix**
**Effort:** 1–2 hours (requires layout change in GpuStaticPropType)  
**Gain:** eliminate `mat3(inst.modelMatrix)` per-vertex normal transform; ~0.5% vert shader cost for build-heavy missions  
**Storage:** add mat3 (36 bytes) to per-type data SSBO, index by typeID, use for normal-to-world instead of full matrix multiply.

### 5. **Gate texture sampling in debug-only modes**
**Effort:** 30 minutes  
**Gain:** eliminate wasted texture fetches in modes 1–4 (save L1/L2 memory traffic)  
```glsl
vec4 tex_color = vec4(1.0);
if (u_debugMaterialMode == 0 || u_debugMaterialMode >= 5) {
    tex_color = texture(u_texArr, ...);
}
```

---

## Summary Table

| Category | Finding | Priority | Est. Fix Time |
|----------|---------|----------|--------------|
| **GL State** | No explicit depth/blend guards in flush() → fragile inheritance | 🔴 Critical | 1–2 hrs |
| **SSBO Reads** | PerDrawEntry accessed 6× per frag instead of 1× | 🟡 Medium | 30 min |
| **Vert Shader** | ObjectLights fetch per-instance (cache miss on multi-instance types) | 🟡 Medium | 2–3 hrs |
| **Vert Shader** | Normal transform mat3(M) per-vertex (could pre-compute per-type) | 🟠 Low | 1–2 hrs |
| **Frag Shader** | Texture sample in pure-debug modes (wasted L1/L2) | 🟠 Low | 30 min |
| **CPU** | Per-instance light resolution (constant for multi-instances) | 🟠 Low | <1 hr cache-only fix |
| **Perf** | CullRecordVersion bump on every flush (even identical frames) | 🟠 Low | 30 min (gating) |

---

## References

- **Design**: docs/load_bearing_pointers.md (GL state contract)
- **Render Contract**: docs/render-contract.md + mclib/render_contract.h
- **Memory notes**: memory/mc2_argb_packing.md, memory/uniform_uint_crash.md, memory/cpp_glsl_ubo_struct_lockstep.md
- **Terrain precedent**: GameOS/gameos/gos_terrain_pbr_mod.cpp (f375e0ba state guards, terrain LOD chunk fixes)
- **Profiling**: MC2_STATIC_PROP_FLUSH_COST_SPLIT env, [SPFLUSH_COST_SPLIT v1] instrumentation in registry
