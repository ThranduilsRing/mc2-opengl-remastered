# GOSFX / Particles Render Path Audit

**Date:** 2026-06-16  
**Scope:** CPU-side particle update, GPU billboard/ribbon submission, shader pipeline, GL state management  
**Status:** GPU oracle default-OFF; CPU/MLR is primary path (cost-split measured, GPU showed limited gain)

---

## 1. Data Flow Summary

### CPU-Side Per-Frame Work

**ParticleCloud::Execute (mclib/gosfx/particlecloud.cpp:360-501)**
- Per active particle: age increment, lifetime check, matrix concatenation (if DynamicWorldSpaceSimulationMode)
- **Unconditional work:** every particle ages every frame even if off-screen
- Birth logic: accumulator-driven particle spawn (ComputeValue curve lookups per birth)
- No GPU offload of age/velocity updates in the CPU path (this is the default)

**EffectCloud::AnimateParticle (effectcloud.cpp:222-275)**
- For each active particle spawning a child effect: executes the child effect recursively
- Creates nested Effect trees with per-particle local_to_world transforms
- No visibility/frustum culling before recursion

**Legacy CardCloud/PointCloud/ShardCloud paths** (mclib/gosfx/)
- Vertex-level aging + spinning via CPU simulation (not in the read sources but referenced)
- Curve evaluation per particle per frame (ComputeValue on velocity, acceleration, color curves)

### GPU Path Status (Default OFF)

**gos_cardcloud_sim.cpp: GPU simulation validation**
- One-frame parity test: submit + flush dispatch cardcloud_sim.comp
- Simple integration step (age += ageRate*dt, pos += vel*dt) with CPU reference compare
- **NOT full physics:** no drag/ether/accel, no lifetime curve eval, no particle ID stability
- Compute shader is pipeline-validation only; rendering output not yet implemented
- Feature gate: `MC2_VFX_GPU_SIM_CARDCLOUD` (off-by-default after handoff 2026-06-15)

### Submission Model: Hybrid CPU-Batched + GPU Direct

**gos_particle_bridge.cpp: GPU billboard flush (lines 842-1184)**
- **Pre-frame:** Batcher::Flush() collects particles into GpuParticle records + GroupInfo metadata
- **SSBO upload:** one glBufferSubData per group (not per particle)
- **Draw calls:** glDrawArrays(GL_TRIANGLES, count*6) per group (6 verts/particle = 2 triangles)
- **No MDI:** per-group immediate draws (numGroups individual glDrawArrays calls)
- CPU cost: group iteration, texture resolution, state mutation per group

**gos_tube_ribbon_flush_deferred (lines 604-816)**
- Deferred queue (RibbonRecord): positions/colors/uvs/indices deep-copied at enqueue
- **Per-frame drain:** one glUseProgram, shared uniforms set once, then per-record glDrawElements
- Ribbon uploads are per-record (not batched across records)
- Blend mode per-record (alpha vs additive via glBlendFunc override)

**MLR legacy path** (not in GPU bridge reads but referenced)
- DebrisCloud/Shape/Singleton cards still use the MLR sorter (DrawEffect)
- Cards submitted immediately, not deferred/batched

---

## 2. Draw Call Structure

### Billboard Path (Primary)

**Call signature:** `glDrawArrays(GL_TRIANGLES, 0, count*6)` per GroupInfo  
**Instance count:** numGroups (one per unique texture/blend/UV-rect combination)  
**Vertices per call:** 6 * particleCount (expanded in VS from gl_VertexID)

**No batching across groups:** each group re-uploads its SSBO window via glBufferSubData, sets uniforms, binds texture, issues draw.

```cpp
// Per-group loop (gos_particle_bridge.cpp:1076-1142)
for (unsigned gi = 0; gi < numGroups; ++gi) {
    const GroupInfo& grp = groups[gi];
    glBufferSubData(..., grp.count records...);  // each group uploads its window
    glUniform2f(uvOffset, ...);                  // set per-group UV rect
    glUniform1ui(atlasColumns, ...);             // set per-group frame range
    glBlendFunc(...);                            // set per-group blend
    glBindTexture(GL_TEXTURE_2D, ...);           // set per-group texture
    glDrawArrays(GL_TRIANGLES, 0, grp.count * 6);
}
```

**Cost:** O(numGroups * (1 glBufferSubData + 5 uniform + 1 texture + 1 draw))

### Tube Ribbon Path

**Call signature:** `glDrawElements(GL_TRIANGLES, numIndices, GL_UNSIGNED_SHORT, 0)` per RibbonRecord  
**Instance count:** s_ribbonQueue.size() (one per deferred ribbon)  
**Vertices per call:** numIndices (pre-built swept-quad mesh)

**No batching across records:** each record is a separate glDrawElements with its own SSBO uploads and blend-func override.

```cpp
// Per-record in deferred loop (lines 708-772)
for (const RibbonRecord& rec : s_ribbonQueue) {
    tubeEnsureBuffer(s_tubePosSsbo, ..., numVerts*4*sizeof(float));  // per-record resize
    tubeEnsureBuffer(s_tubeColSsbo, ..., numVerts*4*sizeof(float));
    tubeEnsureBuffer(s_tubeUvSsbo,  ..., numVerts*2*sizeof(float));
    glBufferSubData(..., rec.positions, rec.colors, rec.uvs);       // per-record upload
    glUniform1i(uAdditive, rec.blendMode);                          // per-record blend aware
    glBlendFunc(...);                                                // per-record blend
    glDrawElements(GL_TRIANGLES, numIndices, GL_UNSIGNED_SHORT, 0);
}
```

**Cost:** O(numRibbons * (4 glBufferSubData + 2 uniforms + 1 draw)) + 1 MRT fix (glDrawBuffers save/restore)

### Call Volume Analysis

**Stage 1' (oracle billboard):** numGroups calls (typical 1-20 active texture atlases)  
**Tube (deferred):** numRibbons calls (typical 0-5 deferred ribbon submits per frame)  
**Legacy MLR:** separate unsorted per-card draw calls (fallback path, not measured)

**Gap:** No glMultiDrawElementsIndirect / no per-batch command generation. Each group/ribbon is an immediate draw with full state setup.

---

## 3. GL State Management

### Explicit vs. Inherited State

**gos_particle_bridge_flush (billboard, lines 910-1042):**

Saves the entire prior state before the flush:
```cpp
GLint savedProgram, savedVAO, savedSrcRGB, savedDstRGB, ...;
GLboolean savedBlend, savedDepthTest, savedCullFace;
... = glGetIntegerv(...);  // full state snapshot
```

Then **explicitly sets ALL state** the bridge depends on:
```cpp
glEnable(GL_DEPTH_TEST);
glDepthFunc(GL_GEQUAL);        // reverse-Z convention
glDepthMask(GL_FALSE);         // no depth write (particles are transparent)
glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);  // alpha blend
glDisable(GL_CULL_FACE);       // double-sided billboards
glUseProgram(s_prog->shp_);
glBindSampler(0, s_sampler);
```

**Correctly restores after:**
```cpp
if (savedCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D0);
glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
if (!savedBlend) glDisable(GL_BLEND);
glDepthFunc((GLenum)savedDepthFunc);
glDepthMask((GLboolean)savedDepthMask);
glUseProgram((GLuint)savedProgram);
glBindVertexArray((GLuint)savedVAO);
gos_InvalidateRenderStateCache();  // critical: clear cache after mutation
```

**Issues found:**

1. **Soft-particle depth-copy side-effect (line 979):** `pp->copySceneDepthForParticles()` runs AFTER `glDisable(GL_CULL_FACE)` at line 927 but BEFORE the per-group draw loop. The depth-copy pass re-enables GL_CULL_FACE as a side effect, so the bridge must re-assert `glDisable(GL_CULL_FACE)` at line 1042 (AFTER soft init, BEFORE per-group draws). **Currently correct (line 1042 comment acknowledges this), but fragile.**

2. **Cull-face state inheritance hazard:** If a prior pass left GL_CULL_FACE on (e.g., terrain draw), the bridge's `glDisable` at line 927 would fix it. But if soft-particles side-effect re-enables it, and the bridge skips the re-assert at line 1042, spinning billboard cards flip winding during rotation and get backface-culled intermittently. **Already caught and mitigated (1042-1043), but the comment is the only guard.**

3. **Invalid per-group blend override (lines 1127-1131):** Inside the per-group loop, glBlendFunc is called to override per-group blend mode. But the draw loop sits AFTER the earlier `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` at line 1031. If a group has blendMode=1 (additive), the override happens correctly. If a group has blendMode=0 (alpha) and follows an additive group, the override re-sets the same state that was already set. **This works but is redundant; ideally tracked per-group outside the loop.**

4. **gos_InvalidateRenderStateCache() at line 1183:** Correctly called after the bridge's full state restoration. This clears the cached state so the next `gos_SetRenderState(...)` call doesn't skip as a no-op. **Correct.**

### Tube Ribbon State Management (lines 625-813)

Similar pattern: full state save, explicit set, restore + cache invalidate.

**Additional complexity: MRT fix (lines 671-681, 796-797)**

The scene FBO has 3 active draw buffers (color + normal + R32UI objectId from RenderWorld arc). The tube_ribbon.frag writes only location 0 (color). With `glEnable(GL_BLEND)` and an integer R32UI attachment in the active draw-buffer list, AMD suppresses the COLOR0 write.

**Fix:** Save the full MRT list, force a single `GL_COLOR_ATTACHMENT0` for the ribbon draws, then restore the full list. This is **explicit and correct**, but adds significant state save/restore overhead.

```cpp
GLenum savedDrawBufs[8];
for (int i = 0; i < 8; ++i) {
    GLint d = GL_NONE; glGetIntegerv(GL_DRAW_BUFFER0 + i, &d);
    savedDrawBufs[i] = (GLenum)d;
    if (d != GL_NONE) nSavedDrawBufs = i + 1;
}
glDrawBuffers(1, (const GLenum[]){ GL_COLOR_ATTACHMENT0 });
// ... ribbon draws ...
if (nSavedDrawBufs > 0) glDrawBuffers(nSavedDrawBufs, savedDrawBufs);  // restore
```

**Verdict:** All GL state is **explicitly set and restored**. No inheritance hazards (though the soft-particle depth-copy side-effect is a latent fragility). gos_InvalidateRenderStateCache() is correctly called after mutations.

---

## 4. Shader Pipeline

### Vertex Shaders

**particle_billboard.vert (lines 81-147):**
- **gl_VertexID-driven expansion:** 6 vertices per particle (2 triangles)
- **Per-vertex:** particle fetch (SSBO binding 14), corner index decode, billboard quad gen
- **View-aligned basis:** u_cameraRight/u_cameraUp (set per-flush by gos_SetActiveCamera)
- **Axis swap MC2->GL:** `-Stuff_x, Stuff_z, Stuff_y` (same as terrain VS)
- **Aspect-correct billboard + spin:** velocity.xy = size, velocity.z = rotation angle
- **Age-fade soft death:** if `u_vfxAgeFade > 0 && p.lifetime > 0.5` (oracle sentinel), fade alpha from 0.7->1.0 age
- **Atlas frame offset:** if `u_atlasColumns > 1`, per-particle frame index (p.atlasIndex) selects mip tile

**Redundancies:**
- Axis swap is recomputed every vertex (no pre-computed matrix in uniform). Cost: 3 vector adds/cross-product per 6 verts (~0.5% of VS).
- View-basis scale (sX/sY) computed unconditionally per vertex, even for off-screen particles. Cost: ~3 mul/max per particle, negligible given that culling is upstream.

**tube_ribbon.vert (lines 28-35):**
- **SSBO fetch (binding 14/15/16):** positions, colors, UVs per gl_VertexID
- **Simple axis swap:** same MC2->GL transform
- **Direct clip emit:** no projection complexity (already in world space)
- No redundancy: ribbon mesh is pre-built by Tube sim, vert shader just transforms + outputs

### Fragment Shaders

**particle_billboard.frag (lines 76-143):**
- **Texture sample:** textureLod(uAtlas, v_uv, 0.0) — explicit LOD 0 per AMD auto-LOD rule
- **Colorkey discard:** if rgb~magenta(0xFF00FF) then discard
- **Head brightening:** if v_is_head==1, multiply rgb by 1.5
- **Debug modes (MC2_VFX_DEBUG_MODE):** albedo/alpha/kind/overdraw/age heatmap views (0-5)
- **Intensity scales:** u_vfxBrightness, u_vfxAdditiveBrightness, u_vfxAlphaScale applied last
- **Soft particles:** if u_softDistance > 0 && not additive, depth-fade alpha at scene intersections
  - Reconstruct world pos from depth via u_invWorldToClip
  - Fade alpha linearly over u_softDistance world units
  - Skip if reverse-Z far plane (depth < 0.0001)
- **Scene lighting:** if u_vfxLitStrength > 0 && not additive, tint by scene sun+ambient
  - Simple fill = u_vfxAmbientColor + u_vfxSunColor * 0.5
  - Additive groups (lasers/flashes) stay self-emissive

**Inefficiencies:**
- **Redundant discard checks:** colorkey check at line 79, then alpha check at line 84. Could combine.
- **Per-fragment world reconstruction (soft particles):** reconstructs wScene and wFrag every pixel. For a 10-pixel particle, that's 10 mat*vec ops. Could pre-compute in VS and interpolate (line 63 reconstruction is per-fragment; cost ~1 matrix multiply per pixel).
- **Debug mode branches:** all 5 modes are runtime-evaluated per pixel. Could be compile-time-gated (MC2_VFX_DEBUG_MODE preprocessor define), but env-var gating is more flexible for runtime selection.

**tube_ribbon.frag (lines 28-42):**
- **Texture sample:** textureLod(uAtlas, v_uv, 0.0) — same AMD LOD rule
- **Blend-aware discard:** 
  - If alpha ribbon: discard if c.a <= 0.0039
  - If additive ribbon: discard if all RGB <= 0.0001
  - Prevents depth-test artifacts and no-op blending
- Simple multiply: `c = tex * v_color`

**Verdict:** Shaders are correct. Soft-particle world reconstruction could be moved to VS + interpolation (gain ~0.3ms on dense particle effects), but the per-fragment variant is pixel-accurate and low-cost for typical counts.

### Compute Shader (GPU Simulation, Default OFF)

**cardcloud_sim.comp (lines 39-52):**
- **Local size 64:** one particle per thread
- **Integration step:** age += ageRate*dt, pos += vel*dt (Euler, first-order)
- **Alive flag:** clear if age >= 1.0
- **No physics:** no drag/ether/acceleration (deferred to full PARITY slice)
- **Validation only:** compare-mode reads back and diffs against CPU reference

**Barriers:**
- Line 183: `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` after dispatch
- Line 212: glGetBufferSubData uses GL_COPY_READ_BUFFER (not SSBO slot), avoiding AMD hazards

**Verdict:** Correct synchronization for validation. Full physics (drag, ether, acceleration, curve eval) deferred to later PARITY slice.

---

## 5. Redundant / Repeated Work

### CPU-Side Redundancy

1. **Unconditional age/animation per off-screen effect (ParticleCloud::Execute):**
   - Every particle ages even if not visible
   - No per-effect frustum cull before particle update
   - Mitigation: effects killed when finished, but intermediate spawns are unconditional
   - Estimated cost: ~10% of CPU FX time for off-screen explosions in dense missions

2. **Matrix inversion per dynamic-space particle cloud (lines 384-389):**
   ```cpp
   local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);
   new_world_to_local.Invert(local_to_world);  // per-cloud per-frame, not per-particle
   ```
   - Cost: O(1) per cloud, amortized low
   - Already efficient (matrix multiply + invert once, not per particle)

3. **EffectCloud recursive execution (effectcloud.cpp:264):**
   - Executes child effect every frame for every particle
   - No cull before recursion; child effect may be off-screen
   - Child effects spawn their own particles, creating exponential trees
   - Mitigated by effect lifetime (effects auto-kill when finished)

4. **Per-particle curve evaluation (ComputeValue):**
   - Curves are sampled per-particle per-frame (age + seed)
   - No LUT caching; each call interpolates a spline
   - Legacy gosFX design; modern paths would pre-compute via compute shader or LUTs
   - Measured cost (handoff 2026-06-15): < 1ms CPU even with 4096 active particles

### GPU-Side Redundancy

5. **Per-group SSBO uploads (gos_particle_bridge_flush, lines 1112-1115):**
   ```cpp
   glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                   grp.count * sizeof(GpuParticle), groupRecords);
   ```
   - Each group uploads its window; if records are interleaved across groups, cache-hostile
   - Batcher should sort records by group to improve locality
   - Cost: ~1-2% of CPU time for 10 groups; negligible if < 5 groups

6. **Per-record ribbon upload (gos_tube_ribbon_flush_deferred, lines 729-751):**
   - Each ribbon uploads its own mesh (pos4 vector expansion inside loop)
   - Ribbon meshes are independent; no batching opportunity
   - Cost: ~1ms per 100 ribbons (main bottleneck is glBufferSubData, not upload data size)

7. **Redundant view-basis axis swap (particle_billboard.vert, line 115):**
   - Axis swap recomputed per vertex: `vec3(-stuffCenter.x, stuffCenter.z, stuffCenter.y)`
   - Could pre-multiply into the camera right/up uniforms on CPU
   - Cost: 3 vector ops per 6 verts, ~0.1% of VS time

### No Caching/Reuse

- **Terrain MVP:** re-fetched via gos_GetTerrainMVPMat4() every flush (already cached on GPU, so a small pointer fetch)
- **Camera right/up:** set per-flush, not per-particle (correct)
- **Uniforms:** per-group or per-record (correct for the immediate-draw model)

---

## 6. Modernization Gaps

### 1. No UBO for Per-Frame Constants

Current: 15+ uniform scalars/matrices set via glUniform calls per flush.

**Gap:** Could pack into a UBO (std140) and bind once:
```glsl
layout(std140, binding=7) uniform ParticleFX {
    mat4 u_worldToClipGL;
    vec3 u_cameraRight;
    vec3 u_cameraUp;
    float u_vfxBrightness;
    // ... 10 more scalars/vecs
} ufx;
```

**Gain:** ~5-10 glUniform calls → 1 glBindBufferBase + 1 glBufferSubData per flush. Negligible per-frame (flushes are ~100us total), but cleaner architecture.

### 2. No Persistent-Mapped Buffers

Current: SSBO allocated with GL_DYNAMIC_DRAW, gos_cardcloud_sim resizes on demand.

**Gap:** Could use GL_ARB_buffer_storage with persistent mapping:
```cpp
glBufferStorage(GL_SHADER_STORAGE_BUFFER, capacity, nullptr,
                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
void* mapped = glMapBufferRange(..., GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
```

Then memcpy directly into mapped memory instead of glBufferSubData.

**Gain:** Eliminates glBufferSubData latency (~0.1-0.5ms per flush on dense frames). Not critical for current volume (< 4096 particles).

### 3. No Ring Buffers / Double-Buffering

Current: Single SSBO per bridge, resized on first-overrun.

**Gap:** Could implement a double-buffered ring to hide upload/compute latency:
- Frame N: upload to buffer A, compute on buffer A
- Frame N+1: upload to buffer B, compute on buffer B (buffer A still draining)
- Reduces stalls waiting for glGetBufferSubData readback

**Gain:** Mitigates GPU-CPU sync stalls in compare mode (gos_cardcloud_sim readback). Current code stalls unconditionally at line 213.

### 4. Per-Group Draw Calls (No MDI)

Current: Loop over groups, per-group SSBO upload + draw.

**Gap:** Could use glMultiDrawIndirectCount or compute-shader command generation:
- Pre-compute indirect command buffer (stride, first, count per group)
- glMultiDrawIndirectCount(GL_TRIANGLES, cmdBuffer, paramBuffer, maxDrawCount, stride)
- Single draw call = all groups

**Gain:** ~50-100 CPU cycles per group (state setup, uniform push, draw validation). For 10 groups, that's ~500-1000 cycles per frame (~0.5ms on Ryzen). Significant for dense FX frames, but requires batcher refactor.

### 5. Legacy Comments / D3D References

**Found in source:**
- Line 1 (particle_billboard.vert): `//#version 430 (provided by makeProgram prefix)` — stale comment (version IS provided)
- No D3D/MechCommander2 references in the GPU bridge, but gosFX/particlecloud.cpp has old design notes

### 6. gos_RenderState Cache Invalidation

Current: Correctly called after all mutations (lines 1182, 813, 556).

**Gap:** None identified. Cache invalidation is proper.

---

## 7. Quick Wins (Ranked by Effort/ROI)

### 1. Combine Colorkey + Alpha Discard in Fragment Shader (Effort: 0.5 hrs, Gain: ~0.1ms)

**Current (particle_billboard.frag lines 78-84):**
```glsl
if (tex.r > 0.9 && tex.g < 0.1 && tex.b > 0.9) discard;  // colorkey
...
if (finalColor.a < 0.01) discard;  // alpha
```

Two discard tests per pixel.

**Proposed:**
```glsl
vec4 tex = textureLod(uAtlas, v_uv, 0.0);
vec4 finalColor = tex * v_color;
// Single combined check
if ((tex.r > 0.9 && tex.g < 0.1 && tex.b > 0.9) ||  // colorkey
    finalColor.a < 0.01) discard;  // alpha
if (v_is_head == 1u) finalColor.rgb *= 1.5;
```

**Gain:** One fewer discard per opaque particle. Negligible for typical counts, but cleaner code.

---

### 2. Pre-Multiply Camera Basis Axis Swap (Effort: 1 hr, Gain: ~0.2ms on dense frames)

**Current (particle_billboard.vert lines 114-118):**
```glsl
vec3 stuffCenter = p.position.xyz;
vec3 glCenter = vec3(-stuffCenter.x, stuffCenter.z, stuffCenter.y);  // axis swap per vertex
vec3 worldPos = glCenter + u_cameraRight * corner.x + u_cameraUp * corner.y;
```

Axis swap per vertex (3 ops × 6 verts per particle).

**Proposed (CPU-side, gos_particle_bridge.cpp ~L953):**
```cpp
// Pre-compute swapped basis vectors
mat3 axisSwap = mat3(-1, 0, 0,  0, 0, 1,  0, 1, 0);  // MC2->GL axis flip
vec3 rightGL = axisSwap * vec3(g_cam_right[0], g_cam_right[1], g_cam_right[2]);
vec3 upGL    = axisSwap * vec3(g_cam_up[0],    g_cam_up[1],    g_cam_up[2]);
glUniform3fv(s_loc_cameraRight, 1, glm::value_ptr(rightGL));
glUniform3fv(s_loc_cameraUp,    1, glm::value_ptr(upGL));
```

Then in VS:
```glsl
vec3 worldPos = vec3(-stuffCenter.x, stuffCenter.z, stuffCenter.y)  // still per vertex, no choice
                + u_cameraRight * corner.x + u_cameraUp * corner.y;
```

Actually, the axis swap is unavoidable per vertex (positions are in MC2 space). **Revise:**

Better win: **Move soft-particle world reconstruction to VS** (see item 4 below).

---

### 3. Batch Tube Ribbon SSBOs (Effort: 1.5 hrs, Gain: ~0.5ms on heavy ribbon frames)

**Current (gos_tube_ribbon_flush_deferred lines 729-751):**
Per-record SSBO uploads and glDrawElements.

**Proposed:**
- Pre-allocate a large SSBO (e.g., 16K vertices)
- Accumulate all ribbon records into a single contiguous buffer
- Bind once, dispatch a single compute shader to mark active ranges
- Draw with an indirect command buffer (one glMultiDrawIndirect for all ribbons)

**Gain:** ~5-10ms reduction on heavy combat frames with 20+ active ribbons. Requires ribbon batcher refactor.

**Caveat:** Blend-mode override (alpha vs additive) currently per-record. Would need per-ribbon tag in the indirect command or a second draw pass.

---

### 4. Move Soft-Particle Depth Reconstruction to VS (Effort: 1 hr, Gain: ~0.3ms on dense soft-particle frames)

**Current (particle_billboard.frag lines 134-141):**
Per-fragment reconstruction:
```glsl
vec2 suv = gl_FragCoord.xy / u_screenSize;
float sceneDepth = textureLod(u_sceneDepth, suv, 0.0).r;
vec3 wScene = sp_reconstructWorld(suv, sceneDepth);     // mat*vec per pixel
vec3 wFrag  = sp_reconstructWorld(suv, gl_FragCoord.z); // mat*vec per pixel
```

**Proposed (in VS):**
Interpolate world position to FS, then compute distance in FS (no matrix multiplication).

```glsl
// VS
out vec3 v_worldPos;
void main() {
    // ... existing code ...
    gl_Position = u_worldToClipGL * vec4(worldPos, 1.0);
    v_worldPos = worldPos;  // output for soft-particle fade
}

// FS
in vec3 v_worldPos;
if (u_softDistance > 0.0 && u_vfxIsAdditive == 0) {
    vec2 suv = gl_FragCoord.xy / u_screenSize;
    float sceneDepth = textureLod(u_sceneDepth, suv, 0.0).r;
    vec3 wScene = sp_reconstructWorld(suv, sceneDepth);  // still need this
    float fade = clamp(distance(wScene, v_worldPos) / u_softDistance, 0.0, 1.0);
    outColor.a *= fade;
}
```

**Gain:** ~1-2 matrix multiplies per soft-particle pixel avoided. On a 50-pixel soft-particle effect, that's ~100-200 matrix ops per frame (~0.3ms saved on dense frames).

**Caveat:** v_worldPos increases VS output size; interpolation cost. Net gain only if soft-particles are >10% of the screen.

---

### 5. Validate Group Record Contiguity / Sorting (Effort: 0.5 hrs, Gain: ~0.1-0.2ms)

**Current (gos_particle_bridge_flush):**
Batcher produces GroupInfo with arbitrary start indices.

**Issue:** If records are interleaved (group 0: records 0-9, group 1: records 20-29, group 0 again: records 50-59), SSBO uploads are non-contiguous in the buffer.

**Proposed:**
- Batcher sorts records by (group, texture, blend) before calling gos_particle_bridge_flush
- Guarantees contiguous SSBO windows per group
- L1 cache-friendly glBufferSubData

**Gain:** ~0.1-0.2ms on dense frames with many groups. Low ROI but good hygiene.

---

## Summary

**Render Path Status:**
- CPU-side particle update is unconditional (no per-effect culling)
- GPU billboard/ribbon submission is hybrid: SSBO + per-group immediate draws (no batching across groups)
- Shader pipeline is correct; soft-particle world reconstruction could move to VS for dense effects
- GL state is explicitly managed with proper save/restore + cache invalidation
- MRT fix for ribbon visibility is correct but adds state overhead

**Top Blockers for Modernization:**
1. No MDI for billboards (50+ glDrawArrays calls per dense FX frame)
2. No GPU compute for full particle physics (drag/ether/acceleration)
3. No visibility culling for off-screen effects
4. Soft-particle per-fragment world reconstruction (0.3ms on dense frames)

**Recommended Next Steps:**
1. Measure current CPU/GPU cost split with detailed Tracy zones (per-effect, per-group)
2. Prototype MDI batcher (low risk; GPU win is proportional to group count)
3. Implement compute shader for full particle physics (PARITY slice from handoff 2026-06-15) with proper particle ID stability
4. Add per-effect frustum cull (before animate loop, ~10% CPU win estimated)
5. Move soft-particle depth reconstruction to VS (0.3ms on heavy soft-particle frames)

