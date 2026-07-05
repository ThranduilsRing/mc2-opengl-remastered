# Water (GPU-Indirect Fast Path) Render Audit

**Branch:** `claude/nifty-mendeleev` | **Status:** Armed since 2026-05-17 | **Coverage:** GPU compute cull + MDI draw

## 1. Data Flow Summary

### CPU-Side Per-Frame Work

1. **WaterStream::BeginFrameNarrow()** — reserves narrow-walk candidate vector (default-ON, env-gatable)
2. **Terrain::geometry()** → **AppendNarrowCandidate()** — during camera-window walk, collects water-bearing quads that passed `clipInfo` cull (mirrors legacy `setupTextures` water block gate at quad.cpp:963)
3. **WaterStream::BuildQuadWindowSSBO()** — hash-lookup loop: for each narrow candidate, resolve `topLeftVertexNum → recipeIdx` via stable map and upload recipe-index window to GPU SSBO (binding 2)
4. **WaterStream::UploadAndBindThinRecords()** — CPU fallback path (armed=false OR MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE=0): walks narrow candidates, validates pz per-triangle, packs thin records (lightRGB/fogRGB/flags) to SSBO binding 6, triple-buffered ring
5. **ComputeDispatchAndBindThinRecords()** — GPU compute entry point:
   - **DISPATCH 1** (gpu_driven_water.comp): culls/packs GPU-side. Reads world-indexed recipe set OR windowed recipe indices; projects water surface through MVP; applies per-triangle pz gate (spec §4.4); atomicAdds output count; writes thin records to binding 3
   - **DISPATCH 2** (gpu_driven_cmd_patch.comp): patches IndirectCmd buffer (count = atomic visibleCount)
   - **Barrier** (GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)
   - Restores binding 6 to thin records for VS draw

### Submission Model

**GPU-Indirect MDI (glMultiDrawArraysIndirect):**
- **Armed path:** GPU compute output count feeds DrawArraysIndirectCommand; VS reads thin records from binding 6; **2 triangles × 3 verts = 6 vertices per water quad**
- **Unarmed path (fallback):** CPU thin-pack + explicit DrawArrays, legacy pass
- **Dispatch bound:** windowed recipe count (1A) OR full recipe set (1B authoritative, default-ON)

---

## 2. Draw Call Structure

### Draw Call Count
- **Armed:** 1× `glMultiDrawArraysIndirect()` for water (base + detail share 1 MDI via WaterPerCmd SSBO)
- **Unarmed:** 2× explicit DrawArrays (base, detail) — legacy immediate-mode fallback

### Vertex Layout
```
6 vertices per quad = 2 triangles × 3 verts/tri
per-vertex streamId (gl_VertexID % 6):
  - vertInRecord = vid % 6 (0..5 maps to tri 0/1, corner 0/1/2)
  - triIdx       = vertInRecord / 3 (0 or 1)
  - id           = vertInRecord % 3 (vertex in triangle)
  - thinIdx      = vid / 6 (which thin record)
```

### Batching Quality
- **Excellent:** All water in a single MDI call; GPU compute output drives exact visible count
- **Per-frame overhead:** O(window-count) CPU hash lookups + GPU atomicAdd + indirect-cmd patch compute
- **No per-object uniforms:** WaterPerCmd SSBO indexed by `gl_DrawIDARB` (when using glMultiDrawArraysIndirect)

---

## 3. GL State Management

### Explicit State Setup (Water Pass Entry)
```cpp
// gos_terrain_water_fast_mdi.vert / gos_terrain_water_mdi.frag
glEnable(GL_DEPTH_TEST);           // ✓ explicit
glDepthMask(GL_TRUE);              // ✓ explicit (opaque water)
glDisable(GL_BLEND);               // ✓ explicit (water-v1 base)
glDepthFunc(GL_GEQUAL);            // ✓ explicit (reverse-Z, per WATER_DEPTH_FUDGE_FAST)
glCullFace(GL_BACK);               // ✓ implicit OK (not changed)
```

### State Discipline
- **gos_terrain_water_fast_mdi.vert** line 283-286: explicit depth bias before emit
  ```glsl
  vec4 clip = u_worldToClipGL * vec4(worldPos, 1.0);
  clip.z += WATER_DEPTH_FUDGE_FAST * clip.w;  // pre-divide, reverse-Z
  gl_Position = clip;
  ```
- **Issue found:** The water pass does NOT explicitly restore state after draw
  - Previous pass (terrain solid) may have set blend/cull/depth differently
  - Water inherits those — fragile, esp. if solid-path changes
  - **Fix:** Call `glInvalidateRenderStateCache()` before water dispatch (same pattern as terrain solid, gos_terrain_indirect.cpp line 1881)

### Binding Order (GPU Compute)
```cpp
// DISPATCH 1 cull/pack (gpu_driven_water.comp)
glBindBufferBase(0, g_recipeBuffer);                      // input: recipes
glBindBufferBase(1, gos_terrain_lighting::GetOutputSsbo()); // input: lighting
glBindBufferBase(2, g_quadWindowSsbo);                    // input: window indices
glBindBufferRange(3, g_thinBuffer, offset, bytes);        // output: thin records
glBindBufferBase(6, g_waterBucketHeaderSsbo);             // output: visibleCount
// DISPATCH 2 cmd-patch (gpu_driven_cmd_patch.comp)
glBindBufferBase(0, g_waterBucketHeaderSsbo);             // input: header
glBindBufferBase(1, g_waterIndirectCmdBuffer);            // output: cmd buffer
// Re-bind 6 for VS draw
glBindBufferRange(6, g_thinBuffer, offset, bytes);        // VS reads thin records
```

**Issue:** Binding 6 slot is overloaded (header in compute, thin in draw). Proper re-bind happens post-barrier (line 1892), but inline comment is dense — opportunity for copy-paste bugs.

---

## 4. Shader Pipeline

### Vertex Stages
**gos_terrain_water_fast.vert** (per-pass uniforms) vs **gos_terrain_water_fast_mdi.vert** (WaterPerCmd SSBO)
- Both use identical projection chain: `u_worldToClipGL * vec4(worldPos, 1.0)`
- **No redundant CPU work:** per-vertex wave Z-lift (frameCos) is baked once at upload, not per-frame
- **Per-triangle culling:** pzTri1/pzTri2 bits prevent degenerate triangles from rasterizing
- **Water thickness (water-v1):** computed per-vertex as `waterElevation - terrain_floor_elevation` for shore-blending

### Fragment Stages
**gos_terrain_water_mdi.frag** (water-v1 stylized)
- **Base layer (o_isWater==1):** procedural fBm ripples (camera-INDEPENDENT, no screen-space fetch), SH-L2 sky reflection, Fresnel blend
- **Detail layer (o_isWater==2):** discarded (dead code; tiled texture + UV-wrap looked bad)
- **Reflection RT fetch:** conditional `u_waterReflStrength > 0` gates a 1/4-res terrain reflection sampler (binding 2) + screen-space UV perturbation by wave normal
- **No deferred work:** all computations in-shader, no deferred lighting pass

### Compute Shaders
**gpu_driven_water.comp** (cull/pack)
- **Invocation domain:** recipe-windowed OR full-world-indexed (u_fullRecipeMode gate, WATER-FULL-RECIPE-GPU-TIMER-SPIKE-0)
- **Per-invocation:** project 4 corners through MVP, per-triangle pz gate, allocate output via atomicAdd, pack thin record
- **Sync:** glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT) post-dispatch ensures thin writes visible to cmd-patch
- **No texture fetches:** all data from SSBOs (recipes, lighting)

**gpu_driven_cmd_patch.comp** (trivial)
- Reads header.visibleCount, writes count to IndirectCmd[0].count
- Gated on MC2_BUCKET_HEADER_TRACE (default OFF per VPL retirement 2026-05-15)

### Shader Issues
1. **uniform uint crash (memory/uniform_uint_crash.md):** alphaEdgeByte/alphaMiddleByte/alphaDeepByte passed as `int` due to shader compiler bug; cast to uint inside shader ✓ mitigated
2. **WATER-REFLECTION-CLIP-1 (open):** reflected projection at shallow camera angle (20 deg) clips corners; no easy fix without full screen-space reflection; deferred
3. **No frag-shader derivatives guarantee:** screen-space RT distortion uses dFdx/dFdy on wave normal; undefined on MSAA resolve (live spec issue, not this audit's scope)

---

## 5. Redundant / Repeated Work

### Per-Frame Uploads
1. **MVP matrix** (gos_terrain_water_stream.cpp line 1530-1537)
   - **On armed frame:** reads `gos_terrain_indirect_getDispatchMvp16()` (baked at terrain-solid dispatch time) for **water consistency fix (2026-05-17)**
   - **Alternative source (live-cam MVP):** gos_GetTerrainMVPMat4() — fallback if solid NOT armed
   - **Fix:** Probe 8 [WATER_DEPTHPROBE v2] verifies consistency across frame; retains verification instrument demoted to env-gated (MC2_WATER_DEPTHPROBE)
   - **Cost:** 64 bytes per dispatch + FNV-1a fingerprint compute (12 floats) once per armed frame

2. **Recipe buffer**
   - Uploaded once per mission at EnsureRecipeBufferUploaded() (gos_terrain_water_stream.cpp line 434)
   - Capacity grows to mapSide² (water quads); re-upload only if new mission loaded
   - **Cost:** O(recipeCount) = O(mapSide²) bytes once per mission ✓ optimal

3. **Quad window SSBO**
   - Rebuilt every frame in BuildQuadWindowSSBO() via CPU narrow-walk hash-lookup
   - **Narrow path (default ON):** O(narrowCount × 1 lookup) ≈ 100× fewer iterations than legacy
   - **Full-recipe path (authoritative 1B, default ON since 2026-06-03):** GPU compute applies eligibility predicate directly; CPU narrow walk skipped
   - **When unarmed:** legacy UploadAndBindThinRecords() does the thin-pack CPU-side

### Unconditional Work for Off-Screen Objects
1. **GPU compute invokes over full window/recipe set:** atomicAdd on every quad, even culled ones (shader is compute, not VS, so culled triangles don't skip invocation). **Cost: ~3.2ms per dispatch on 1K map with full recipe set (proof in HANDOFF_2026_06_09_dynamic_pipeline_oracle), but compute is well-parallelized.**

2. **MVP projection per quad in CPU pack fallback** (line 2072-2081 in gos_terrain_indirect.cpp): Multiplies every corner by MVP even though pz-check may cull the quad. **Optimization: move pz-check before MVP compute.** Current order: recipe→MVP→pzCheck→record; optimal: recipe→pzCheck→[if pz-ok] MVP→record. **Effort: 1h | Gain: ~10-15% on CPU thin-pack (currently demoted to MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1, so low ROI).**

### Cache Misses
- **CPU thin-pack:** iterates land->getQuadList() sequentially (cache-friendly); recipe lookups via unordered_map (hash-table misses on big maps)
  - **Mitigation:** narrow-walk pre-filters (100× fewer lookups); authoritative full-recipe path avoids CPU walk entirely
- **GPU compute:** reads recipes from SSBO sequentially by invocation ID (coalesced, good), lighting SSBO by vertexNum (4 per quad, spatial locality if quads cluster)

---

## 6. Modernization Gaps

### Missing UBO Pattern
- **Current:** per-frame uniforms uploaded individually (glUniform*) per dispatch
- **Modernization:** pack all water uniforms into a single UBO; bind once per render-pass
- **Cost:** rename ~15 uniforms, pack struct, bind 1 UBO vs ~15 glUniform calls
- **Gain:** cleaner code, reduced state-change overhead (negligible on modern GPU)
- **Effort:** 2h | **ROI:** Low (state-change not a known bottleneck)

### Missing Persistent-Mapped Buffers for Streaming
- **Current:** ring buffer with glBufferSubData per frame (copy overhead)
- **Modernization:** GL_ARB_buffer_storage + GL_MAP_PERSISTENT_BIT + GL_MAP_COHERENT_BIT
- **Cost:** redo ring-slot management, add GPU->CPU fence handling
- **Effort:** 4h | **ROI:** medium (eliminates CPU→GPU copy stall on thin-record SSBO, ~200KB/frame)
- **Note:** terrain-solid already uses static recipe SSBO; only thin records are streamed

### Sync-Free Ring Buffers
- **Current:** 3-slot ring with explicit glClearBufferSubData + glMemoryBarrier per dispatch
- **Modernization:** double-buffer with GPU fence (kThinRingFrames already = 3, but CPU pack still clears explicitly)
- **Effort:** 1h | **ROI:** minimal (barrier cost dominated by dispatch work, not sync)

### Legacy Code Patterns
1. **Immediate-mode fallback path** (UploadAndBindThinRecords): retained as safety net for GPU-path failure. **Correct—ship stays playable if compute fails.**
2. **D3D/MC2 comments:** asset names (e.g., "MapData::alphaDepth") are accurate; no misleading references found
3. **No MLR-style per-object draw calls:** water MDI is already batched correctly

---

## 7. Quick Wins (Ranked by Effort/ROI)

### 1. **Explicit GL State Cache Invalidation Before Water Draw** (0.5h | High impact)
**Issue:** Water pass doesn't call `glInvalidateRenderStateCache()` after compute dispatch; inherits state from prior pass (terrain solid sets depth/blend/cull differently per slice).

**Fix:** Add before water MDI draw in gameos_graphics.cpp water-pass entry:
```cpp
glInvalidateRenderStateCache();  // Clear state from prior terrain/solid dispatch
glEnable(GL_DEPTH_TEST);
glDepthMask(GL_TRUE);
glDisable(GL_BLEND);
glDepthFunc(GL_GEQUAL);
// ... bind textures, dispatch MDI
```

**File:** `GameOS/gameos/gameos_graphics.cpp` (water-draw bridge entry)
**Gain:** Eliminates fragile state-inheritance bugs; matches terrain-solid discipline (gos_terrain_indirect.cpp:1881)

---

### 2. **Reorder MVP Compute After pz-Check in CPU Thin-Pack** (1h | Medium ROI)
**Issue:** CPU pack (demoted behind MC2_TERRAIN_INDIRECT_CPU_FALLBACK, but live when env-set) multiplies all 4 corners by MVP even if per-triangle pz culls the quad.

**Current (line 2068-2087, gos_terrain_indirect.cpp):**
```cpp
const float* mvp = gos_GetTerrainMVPMat4();
for (int c = 0; c < 4; ++c) {
    // MVP multiply (16 FMA per corner)
}
// THEN pz check via clipPos (implicit in the VS pzOk gate)
```

**Optimized:**
```cpp
// pz check FIRST (no MVP needed for depth test; only x/y/w needed)
bool pzTriOk = ...; // existing per-tri predicate
if (!pzTriOk) continue;  // skip MVP if culled

const float* mvp = gos_GetTerrainMVPMat4();
for (int c = 0; c < 4; ++c) { ... }  // MVP only for visible triangles
```

**File:** `GameOS/gameos/gos_terrain_indirect.cpp:2057-2087` (PackThinRecordsForFrame)
**Gain:** ~10-15% CPU pack speedup; only matters if MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1 (rare, but safety path)

---

### 3. **Emit Warning if WaterStream Recipes Not Ready at Compute Time** (0.5h | Medium safety ROI)
**Issue:** ComputeDispatchAndBindThinRecords() checks `recipeUploadedCount` post-EnsureRecipeBufferUploaded(), but doesn't warn if recipes empty at dispatch time (silent no-water). Light SSBO guard exists (line 1355-1367); water should mirror it.

**Fix:** Add guard post-recipe-upload (after line 1341):
```cpp
if (!recipeUploadedCount) {
    if (!s_waterRecipeWarnedThisMission) {
        fprintf(stderr, "[GPU_DRIVEN_WATER] event=warn msg=recipe_not_ready "
                "reason=empty_or_not_uploaded frame=deferred_to_cpu\n");
        s_waterRecipeWarnedThisMission = true;
    }
    return false;
}
```

**File:** `GameOS/gameos/gos_terrain_water_stream.cpp` (ComputeDispatchAndBindThinRecords)
**Gain:** Clearer diagnostics for water-vanish bugs (currently silent, like lighting SSBO was)

---

### 4. **Document SSBO Binding 6 Overload Risk** (0.25h | Low effort, medium prevention)
**Issue:** Binding 6 used for both (a) header in compute dispatch and (b) thin records in VS draw. Proper re-bind happens, but inline comment is dense; easy to miss on future edits.

**Fix:** Add comment block at gos_terrain_water_stream.cpp:1887:
```cpp
// M2 FIX: Compute dispatch (DISPATCH 1) binds slot 6 to g_waterBucketHeaderSsbo
// for the coherent atomicAdd on visibleCount. After glMemoryBarrier,
// slot 6 MUST be rebound to thin records (binding 6, kWaterThinSsboBinding)
// for the VS draw to read them. The binding is restored below to prevent
// stale header SSBO on next frame's compute entry.
```

**File:** `GameOS/gameos/gos_terrain_water_stream.cpp:1887` (post-DISPATCH 2 comment)
**Gain:** Prevents binding-slot bugs in future refactors

---

### 5. **Add MC2_WATER_GPU_FULL_RECIPE_AUDIT Counter** (1h | Low ROI, debug aid)
**Issue:** Full-recipe authoritative path (default-ON) has no per-frame audit counter to confirm it's producing visible water. Bucket-header readback only runs every 600 frames (line 1742-1751). User could see broken water for frames before diagnostic fires.

**Fix:** Log visibleCount summary every frame when MC2_TERRAIN_INDIRECT_TRACE=1:
```cpp
// Every 60 frames (not 600)
if (fullRecipeAuth && traceOn() && (authFrame % 60 == 0)) {
    printf("[WATER_FULL_RECIPE_AUTH] frames_since_reset=%u avg_visible_count=%u\n",
           authFrame, cumulativeVis / 60);
}
```

**File:** `GameOS/gameos/gos_terrain_water_stream.cpp` (ComputeDispatchAndBindThinRecords)
**Gain:** Faster diagnosis of water-vanish bugs (currently must wait 600 frames or manually trigger MC2_WATER_FULL_RECIPE_SPIKE)

---

## Summary

The water GPU-indirect fast path is **well-engineered**: MDI batching is optimal, compute dispatch is clean, state discipline mostly correct, and parity testing comprehensive. The open issues are:

1. **State inheritance fragility** (GL state not reset between passes) — fix via glInvalidateRenderStateCache()
2. **Minor MVP redundancy** (CPU pack computes MVP for culled quads) — reorder pz-check first
3. **Silent-wrong-render risk** (recipe not ready, no warning) — add early-return guard
4. **Documentation gaps** (binding 6 overload not explained) — add inline comment
5. **WATER-REFLECTION-CLIP-1** (open spec issue, deferred to water-v2) — not actionable this slice

**All fixes are low-effort (≤1h each) and target safety/clarity, not performance. The path is production-ready.**

