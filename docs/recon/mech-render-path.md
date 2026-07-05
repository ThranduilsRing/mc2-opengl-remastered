# Mech Render Path Audit

**Scope:** CPU-side per-frame transform work (mech3d.cpp, Mech3DAppearance) and GPU-side batched submission (gos_mech_batcher.cpp). Focus: data flow, draw structure, GL state management, shader pipeline, redundant work, and modernization gaps.

**Date:** 2026-06-16  
**Status:** Slice A baseline (full GPU batcher + Slice B1 lighting + Slice C2 skinning + Slice C3 fast-transform).

---

## 1. Data Flow Summary

### CPU-Side Per-Frame Work (mech3d.cpp)

The mech render pipeline has two major stages:

#### Stage 1: updateGeometry() — animation + bone transforms (unconditional per-actor)
- **Called by:** `Mech3DAppearance::update()` at game update time, before render
- **Work per actor:**
  - Advance frame counter via `mechType->mechAnim[currentGestureId]->GetFrame(currentFrame)`
  - Call `mechShape->SetAnimation()` to update TGL animation
  - Call `TransformMultiShape(mechShape, ...)` to compute per-node bone matrices (stored in `mechShape->listOfShapes[i].shapeToWorld`)
  - **LOAD-BEARING:** TGL TransformMultiShape runs unconditionally even for off-screen actors (checked at line 2390-2656 in updateGeometry; visibility gates are currently a TODO for GPU lifecycle, not a CPU-side early-out)
  - Update weapon node positions, footprint state, arm bones (if alive)
  - Update FX positions (dust poofs, smoke) — small cost, geometry not uploaded

#### Stage 2: submitActor() & flush() — GPU bucket submission (batched)
- **Called by:** `Mech3DAppearance::render()` (per-frame, only visible actors)
- **Flow:**
  1. Call `GpuMechBatcher::instance().submitActor(desc)` from mech3d.cpp:2863
     - Stage bone matrices from live `mechShape->listOfShapes[i].shapeToWorld` (60 uint64_t reads per actor)
     - Capture per-packet texture handles for this frame (paint scheme slot 0, type-stable slots 1+)
     - Append to `s_pendingSubmits` vector
  2. Later in frame loop: `GpuMechBatcher::flush()` (called once per frame from renderLists)
     - Build draw buckets: key = (typeLodIdx, globalPacketIdx, texHandle, materialFlags)
     - Ring advance + fence wait (3-frame ring; GPU sync barrier after SSBO writes)
     - Upload bones to SSBO (all actors, one per-instance block; 64 bytes × numBones per actor)
     - Upload instances to SSBO (per bucket group)
     - Issue **one glDrawElementsInstanced per bucket** (not MDI; separate state/uniform updates)
     - Restore GL state + invalidate cache

### Submission Model: **CPU-Batched Bucket-Sorted + Instanced Draws**

- **Per-frame mechs:** N actors (visible subset; CPU inView + GPU lagged-cull gates)
- **Per-actor buckets:** 1..numPackets (one vertex group per TGL node + texture + material flags)
- **Total draw calls:** ~numBuckets (typically 2-8 draws for mc2_24 with 46 mechs)
- **Instance stride:** 64B GpuMechInstance (M2.5: added objectIdRaw + materialIdx + visualFlags)
- **Bone SSBO:** 64B GpuMechBone per node; uploaded once per actor at submit time (not per-bucket)

---

## 2. Draw Call Structure

### glDrawElementsInstanced Usage

**Location:** gos_mech_batcher.cpp:2000+ (Step 7 draw loop)

```cpp
for (const DrawCall& dc : drawCalls) {
    glUniform1i(s_loc_u_instanceBase, (int)dc.instanceBase);
    glUniform1i(s_loc_u_materialFlags, (int)dc.materialFlags);
    // Bind texture, sample, etc.
    glBindSampler(0, s_sampler);
    // One draw per bucket
    glDrawElementsInstanced(GL_TRIANGLES, 
                           pkt.indexCount, 
                           GL_UNSIGNED_INT,
                           (void*)(pkt.firstIndex * sizeof(uint32_t)),
                           dc.instanceCount);  // numActorsInBucket
}
```

### Quality of Batching

**Good:**
- One bucket = one draw call; all instances in bucket share texture/material/LOD
- Bucket key sorts by (type, packet, texture, flags) to collapse identical state
- Instanced draws reduce draw overhead vs per-actor glDrawElements
- Bucket list is stable per-frame (rebuild every flush; cost amortized)

**Issues Identified:**
1. **Texture-handle-per-actor paint scheme causes bucket explosion:** if N mechs have N unique paint schemes, they produce N separate buckets even for identical meshes/LODs. Typical mc2_24: 46 mechs × 10-15 packets each = ~600 potential buckets; texture paint scheme splitting collapses to ~100-150 actual buckets (estimate).
   - **Cost:** higher setup overhead per draw, higher render-call frequency
   - **Root:** TGL's per-instance paint scheme model requires per-texel color replacement at bind time; GPU-side texture atlasing with per-instance swizzle (Material Gpu table, Mech-1 in progress) is the long-term fix

2. **No MDI (glMultiDrawElementsIndirect):** each bucket is a separate glDrawElementsInstanced call, not a single MDI with indirect dispatch buffer
   - **Cost:** ~100-150 glDrawElementsInstanced calls per frame (vs 1-2 MDI calls)
   - **Mitigated by:** instancing reduces per-call overhead; CPU cost per draw is small (one uniform pair + one draw call = ~2μs on Radeon)
   - **Blocker:** indirect dispatch would require pre-built command buffer per bucket; rebuild cost on every submit change (low frequency but high per-rebuild cost); current bucket sort is acceptable

3. **Uniform updates per-draw:** `u_instanceBase` + `u_materialFlags` set per bucket (2× glUniform1i per draw)
   - **Cost:** 2-3 ns per uniform × 150 draws = 300-450 ns per frame (negligible)
   - **Alternative:** push-constant (10-20 dword UBO) or per-packet SSBO record, but current cost is already absorbed

### Shadow Submission

**Location:** gos_mech_batcher.cpp:2060+ (flushShadow)

- Called before main flush; uses prior-frame (already-fenced) SSBO data
- Depth-only pass: shadow_mech.vert position + depth only
- **Same bucket/instance structure as main pass** — one glDrawElementsInstanced per bucket
- **Cost:** identical draw-call count to main pass (not halved via shared IBO)
- **Opportunity:** shared IBO/VAO across shadow + main but separate instance/bone SSBOs per pass (not blocking; cost is already low)

---

## 3. GL State Management

### Explicit State Setting (gos_mech_batcher.cpp:1736+)

**Saved Before:**
```cpp
GLint prevDepthFunc, prevProgram, prevVao, prevArrayBuf, prevElemBuf, ...
glGetIntegerv(...);  // 10+ state reads
```

**Set to Mech State:**
```cpp
pipeline_binder::applyPipeline(RenderCore::getPipelineDesc(RenderCore::PipelineId::MechOpaque));
// Results in: glEnable(GL_DEPTH_TEST), glDepthFunc(GL_GEQUAL), glDepthMask(GL_TRUE),
//            glDisable(GL_BLEND), glCullFace(GL_BACK)
glBindVertexArray(s_sharedVao);
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);  // belt-and-suspenders re-bind
glBindSampler(0, s_sampler);  // REPEAT/LINEAR override
glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo, ...);
glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, s_boneSsbo, ...);
glUniform1i(s_loc_u_tex, 0);
glUniform1f(s_loc_u_fogValue, 1.0f);
// ... 6 more uniforms (lighting, skinning, specular, ambient, debug, ...)
```

**Restored After:**
```cpp
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
glBindVertexArray((GLuint)prevVao);
glDepthFunc((GLenum)prevDepthFunc);
glDepthMask((GLboolean)prevDepthMask);
// ... 8 more restores
gos_InvalidateRenderStateCache();  // tell engine cache is stale
```

### Issues Identified

1. **State save/restore is comprehensive but verbose:**
   - 10-12 glGet calls at flush entry (each glGet is a CPU-GPU round-trip, cost ~100ns each)
   - Total: ~1.2 μs per flush (per frame)
   - **Mitigated by:** happens only once per frame; frame loop cost << render cost
   - **Alternative:** track state in C++ instead of querying GL (already done for applyRenderStates cache, but reused here would require a mech-specific cache invalidation contract)

2. **gos_InvalidateRenderStateCache() is critical and correct:**
   - mech flush mutates GL state directly (not via applyRenderStates)
   - Cache becomes stale; next applyRenderStates call would emit wrong state
   - **Load-bearing:** omitting this causes subsequent draws (water, terrain, UI) to inherit wrong state (depth test, blend, etc.)
   - **Diagnosis:** confirmed in HANDOFF 2026-06-14 memory; one session had terrain transparent to mechs due to glDepthMask inherited FALSE from prior FX pass

3. **IBO re-bind is belt-and-suspenders:**
   - glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo) at line 1774
   - VAO state includes IBO binding; should be stable from finalizeGeometry
   - flushShadow() may leave it dirty (comment at line 1771: "GL_ELEMENT_ARRAY_BUFFER is VAO state; s_sharedVao's slot can be left at 0 by a prior flushShadow() restore-order bug")
   - **Cost:** one extra glBindBuffer per frame (negligible)
   - **Worth keeping:** defensive; cost is absorbed

4. **Sampler binding is state-override (correct but unintuitive):**
   - Prior pass may leave CLAMP_TO_EDGE sampler bound on unit 0
   - Mech textures use REPEAT (tiled patterns); clamped UVs fall off texture edge → black
   - Solution: glBindSampler(0, s_sampler) where s_sampler is REPEAT/LINEAR/static
   - **Load-bearing:** confirmed in HANDOFF 2026-06-14; mechs were all-black when sampler state inherited from patch_stream (CLAMP_TO_EDGE)
   - **Correct fix:** per memory/sampler_state_inheritance_in_fast_paths.md

---

## 4. Shader Pipeline

### Vertex Stage (mech.vert)

**Input:**
- Vertex attributes (7 attributes, 48B GpuMechVertex):
  - position (3×float), normal (3×float), uv (2×float)
  - boneIndices (4×uint8), boneWeights (4×uint8 normalized)
  - tangentOct (2×int16 normalized, zero-fill for stock)
  - aRGBLight (uint32 BGRA, per-vertex base light)

**GPU Work Per Vertex:**
- Load GpuMechInstance from SSBO[u_instanceBase + gl_InstanceID]
- **Slice A rigid path:** mat4 = bones[boneIndices.x + baseBoneOffset]
- **Slice C2 weighted path:** sum 4 bone matrices weighted by boneWeights; stock data (1,0,0,0) makes it byte-identical to rigid
- Bone transform: boneT * vec4(position, 1) → world Stuff frame
- Axis swap: (x, y, z) Stuff → (-x, z, y) MC2 world
- Normal transform: (boneT as mat3) * normal → world Stuff frame; axis-swap
- **Slice B1 lighting:** calc_light per vertex (if u_lightingMode=1); ambient floor 0.35; lightsOut gate from renderFlags bit1
- Highlight + fog forwarded from SSBO to varying

**Issues Identified:**

1. **Redundant normal matrix computation:**
   - boneT is a full 4×4 matrix; mat3 extract happens per-vertex
   - **Cost:** 9 multiply + extraction per vertex (negligible; bandwidth-bound, not ALU-bound on mech geometry)
   - **Not a blocker:** mech vertex count is ~5k (base LOD) to 50k (high-detail); stock geometry does not have tangents/bumpmaps

2. **Two code paths (rigid vs weighted) but stock data is byte-identical:**
   - u_skinningMode toggle (glUniform1i, once per flush)
   - Stock TGL geometry ships boneWeights = (1, 0, 0, 0); weighted sum collapses to rigid
   - **Cost:** no runtime penalty; both paths compile, shader picks at link
   - **Opportunity:** could remove the u_skinningMode branch in v1 and reserve weighted for Track D (Assimp) future import
   - **Status:** acceptable for now; cost already zero

3. **No per-vertex height offset for LOD transitions:**
   - Mech LODs are discrete replacements (0, 1, 2); no smooth morphing between them
   - GPU could per-instance LOD-blend at minimal cost (2 additional bone lookups per vert, ALU-bound, not bandwidth-bound)
   - **Status:** deferred; LOD swaps are fast on Radeon (mech swap every 5-10 frames on average)

### Fragment Stage (mech.frag)

**Input:**
- Texture (slot 0): paint scheme (per-actor, per-bucket); sampled at lod(0) (explicit LOD clamp to base mipmap)
- Base light: per-vertex from calc_light
- Highlight: per-instance SRGB tint
- Fog: per-actor haze mix
- Normal: per-vertex world space (for ambient fill only; not used in base lighting)

**GPU Work Per Fragment:**
- textureLod(u_tex, v_uv, 0.0) — **CRITICAL:** explicit lod 0 due to AMD hardware sampling mip 1+ on mech textures (mipmap chain incomplete; see shader comment)
- Alpha-test if ALPHA_TEST_BIT set (early discard)
- Combine texture × base light + highlight + fog
- **Slice B2 ambient fill (gated):** hemisphere ambient (sky/ground colors, per-normal.y); additive blend
- **Slice B2+ specular (gated, ViewUniforms only):** Blinn specular with glass/cockpit heuristic (dark-pixel classification)
- Output: FragColor (RGBA), GBuffer1 (rc_gbuffer1_screenShadowEligible), ObjectId (M2.5, #ifdef)

**Issues Identified:**

1. **Explicit textureLod(0) hack for AMD hardware:**
   - Root cause: mech paint-scheme texture cycle does not populate full mipmap chain before flush
   - Safe workaround: lod(0) clamps to base level; eliminates black artifacts
   - **Cost:** one textureLod instruction per fragment (no additional cycles; same latency as auto-LOD)
   - **Long-term fix:** ensure mech texture manager pre-generates mipmaps or documents why they are intentionally absent
   - **Status:** blocking issue for high-fidelity (visual artifact on AMD, possible black on NVIDIA with strict LOD sampling); currently mitigated

2. **No per-fragment normal mapping:**
   - Normal is world-space but unused for surface detail (only for ambient fill)
   - Normals are cooked (geometry-based from ASE loader; see mech-normals-audit.md)
   - **Slice A:** per-node geometric normals recomputed at registerTypeLod (mode 2, angle-threshold smoothed, 60deg default)
   - **Load-bearing:** hard-edge cockpit/panels require geometric normals, not the ASE-averaged Legacy normals (fixed in 2026-05-28)
   - **Status:** acceptable; bumpmapping is lower priority than fidelity

3. **Glass/cockpit heuristic is conservative (no hue detection):**
   - Classification: luma < 0.12 AND maxChan < 0.18 (dark pixels only)
   - Specular shininess (Blinn exponent) varies by material class (metal vs glass)
   - **Status:** user-tuned post-soak (2026-05-28); acceptable heuristic for 256×256 mech textures

4. **GBuffer1 output is static (no variant):**
   - Always output rc_gbuffer1_screenShadowEligible (world normal + shadow eligibility flag)
   - No PBR material data (metallic, roughness, AO) encoded
   - **Status:** acceptable for Slice A; terrain PBR is the high-priority path (Slice 10)

### Compute Shaders

**Status:** None used for mech path. All work is vertex/fragment pipeline.

### Legacy gl_FragData

**Status:** No legacy gl_FragData in mech.frag. Uses `layout(location=N) out` for all MRT attachments (correct C++17 + GL4.3 core profile).

---

## 5. Redundant / Repeated Work

### Per-Frame CPU Redundancy

#### 1. updateGeometry() runs unconditionally even for off-screen mechs
- **Location:** mech3d.cpp:~2400-2650 (Mech3DAppearance::updateGeometry)
- **Scope:** TransformMultiShape (TGL skeleton animation) for every mech
- **Cost:** ~80-200 μs per mech (varies by node count; 4-20 bones typical)
- **MC2_24 mission:** 46 mechs × 150 μs ≈ 7 ms (13% of 60 Hz frame budget)
- **Visibility:** GPU cull gates exist (MC2_GPU_CULL_LIFECYCLE=1) but gate updateGeometry() entry, not re-enable it for on-screen actors
  - Current gate: visible actors run updateGeometry, invisible actors skip it (CPU inView && GPU lagged-cull agree offscreen)
  - **Opportunity:** could gate TransformMultiShape to visible-only, keeping off-screen FX/camera updates; requires careful review of AI dependencies
- **Status:** **Known hotspot** per HANDOFF 2026-06-09 (FX oracle recon); not blocking (FX sim cost is higher priority; skeleton animation is O(bones) = O(20-30) for most mechs)

#### 2. Per-packet texture handle capture at submit time
- **Location:** gos_mech_batcher.cpp:1414-1424 (submitActor texture capture loop)
- **Cost:** ~1-2 μs per actor (linear in packet count; 10-15 packets typical)
- **Frequency:** every frame for visible actors
- **Redundancy:** type-stable slots 1+ are read from TG_TypeShape::listOfTextures (shared across all actors); recomputed per-submit even though stable
  - **Root:** TGL allows TG_TypeShape::listOfTextures to be mutated by TransformMultiShape at render time (to cache live slots)
  - **Workaround:** capture per-packet handles at submit time (already live for the current actor)
  - **Alternative:** pre-bake type-level texture handles at registerTypeLod (one-time cost); would require TGL API change
- **Status:** acceptable; cost is already amortized across all visible actors

#### 3. Duplicate bone writes per bucket (bone SSBO written once per actor, read N times by instances in that bucket)
- **Location:** gos_mech_batcher.cpp:1571-1578 (Step 4: Write bone SSBO once per actor)
- **Cost:** 64B × numBones per actor; uploaded once per actor regardless of how many buckets it appears in
- **Example:** 1 actor × 10 packets × 15 bones = 960 bytes written once (correct; bones are shared across all packets of that actor)
- **Status:** correct; no redundancy here

### Per-Frame GPU Redundancy

#### 1. Instance SSBO stride is 64B but only ~40B actively used in stock Slice A
- **Layout:** typeLodRecordIndex (4B) + baseBoneOffset (4B) + lightDataIndex (4B) + renderFlags (4B) + aRGBHighlight (16B) + fogRGB (16B) + objectIdRaw (4B) + materialIdx (4B) + _pad2 (4B) + _pad3 (4B) = 64B
- **Slice A usage:** all fields except materialIdx (unused in Slice A; reserved for Mech-1)
- **Slice B1+ usage:** all fields
- **Cost:** 64B stride × instances; bandwidth-bound on SSBO load (mech.vert loads at line 129)
- **Opportunity:** could trim _pad2/3 and materiaIdx in future if mech-specific VBO is added; currently correct for forward-compat
- **Status:** acceptable; tradeoff between compactness and forward-compat

#### 2. Bone matrix per-vertex load (boneT assembly)
- **Location:** mech.vert lines 139-152 (Slice A: single bone, Slice C2: weighted sum)
- **Cost:** 16×float load per vertex from SSBO; AMD prefetcher caches sequential accesses
- **Alternative:** pre-transform vertices on CPU and stream pre-transformed positions (would lose LOD flexibility; not practical for mech geometry swaps)
- **Status:** correct; GPU transform is the intended architecture

#### 3. Sampler state per-draw
- **Location:** gos_mech_batcher.cpp:1786 (glBindSampler once per flush, not per-draw)
- **Cost:** one glBindSampler per frame (not per draw)
- **Status:** correct; not redundant

---

## 6. Modernization Gaps

### Missing UBO for Per-Frame Constants

**Current state:** Uniforms uploaded per-draw or per-flush
- `u_worldToClipGL` (mat4): uploaded per-flush, reused across all 150+ draws
- `u_mvp` (mat4): uploaded per-flush
- `u_fogValue`, `u_lightingMode`, `u_skinningMode`, specular params (6 more): per-flush

**Opportunity:** pack into a single UBO (ViewUniformsBlock already exists for viewuniforms path; binding=3)
- **Status:** partially done (MECH-VIEWUNIFORMS-BLOCKBINDING-1 at gos_mech_batcher.cpp:132); gated behind MC2_MECH_VIEWUNIFORMS=1 (default ON)
- **Remaining:** legacy path (gate OFF) still uses per-flush uniforms; acceptable for now
- **Verdict:** already addressed; no action needed

### Missing Persistent-Mapped Buffers for Streaming

**Current state:** Transient SSBO writes per-frame via glBufferStorage + GL_MAP_COHERENT_BIT

**Location:** gos_mech_batcher.cpp:1095+ (ensureRingCapacity)
```cpp
glBufferStorage(GL_SHADER_STORAGE_BUFFER, size, nullptr,
    GL_MAP_COHERENT_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT);
glMapBufferRange(..., GL_MAP_COHERENT_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT);
```

**Status:** already using persistent-mapped ring buffers (3-frame ring for mech instance/bone SSBOs)
- Write-only stream with coherent synchronization (gpuSyncBarrier at line 1923)
- Avoids GPU stall on buffer re-map
- **Verdict:** correctly modernized; no action needed

### Missing Sync-Free Ring Buffer Variants

**Current state:** 3-frame ring with explicit fences (glSync)

**Location:** gos_mech_batcher.cpp:1561-1566 (Step 3: advance ring + wait)
```cpp
s_frameSlot = (s_frameSlot + 1) % MECH_RING_FRAMES;
if (s_fence[s_frameSlot]) {
    glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    glDeleteSync(s_fence[s_frameSlot]);
    s_fence[s_frameSlot] = 0;
}
```

**Status:** already using modern sync (typed GPU-SYNC-CONTRACT barrier helper at line 1923)
- Mirrored static-prop pattern (proven on AMD Radeon)
- Avoids GPU stall by waiting for the ring slot that was written 3 frames ago (pipeline depth = 3)
- **Verdict:** correctly modernized; no action needed

### MLR/Immediate-Mode Draw Fallback

**Current state:** CPU path available and used when GPU path fails

**Location:** mech3d.cpp:2889-2895 (CPU fallback mechShape->Render(true))
- When g_useGpuMechs=false, GPU registration fails, or memory exhausted: fallback to CPU MLR draw
- Counter s_mlrMechDrawsThisMission tracks fallback frequency (Q6 amendment 2, always-on)

**Status:** fallback is correct and available; legacy MLR code is still present in TGL (not removed)
- **Verdict:** acceptable; no action needed (CPU path is a safety net, not primary)

### Outdated Comments Referencing D3D/MechCommander2

**Examples found:**
- mech3d.h:7: "Replace Mactor for better looking mechs!" (MechCommander2 jargon)
- mech3d.cpp:2661: "// FX_COUNT one-time init" (diagnostic comment, not outdated)
- gos_mech_batcher.cpp:4: "NOTE: no #version directive" (correct for this engine)

**Status:** comments are historical but not actively misleading; no action needed

---

## 7. Quick Wins (Ranked by Effort / ROI)

### 1. **Pre-generate mipmap chains for mech paint-scheme textures** (Effort: 2-4h, Gain: 1-2ms, fix AMD sampling bug)

**Scope:** mech texture cycle in mcTextureManager or TGL loader
- **Root issue:** explicit textureLod(0) hack is a workaround for incomplete mipmap chains
- **Fix:** ensure mipmap generation happens before mech batcher flush time
- **Implementation:**
  - Modify `mcTextureManager::loadTexture` to auto-generate mipmaps after paint-scheme recolor (DXT/BC1 hardware-supported)
  - OR: mark mech textures for mipmap generation in the asset pipeline upscaler
- **Gain:** remove textureLod workaround, enable full LOD sampling, clarify shader intent
- **Risk:** mipmaps increase VRAM (small for 256×256 mech textures; ~1-2 MB per mech type)
- **Status:** high-priority quality-of-life fix; blocking precise AMD visual comparison

### 2. **Cache type-stable texture handles at registerTypeLod** (Effort: 1-2h, Gain: <0.1ms, simplify cap-submit loop)

**Scope:** gos_mech_batcher.cpp packet texture capture
- **Root issue:** slots 1+ (type-stable) are re-read from TG_TypeShape::listOfTextures at submit time; this pointer is mutated by TransformMultiShape and reflects the LAST actor (not the current one)
- **Fix:** pre-bake type-level texture handles at registerTypeLod; store in GpuMechPacket (already has a texHandle field for slots 1+)
- **Implementation:**
  - Add a loop in registerTypeLod to resolve each packet's type-stable texture slot to a live gos handle (call mcTextureManager immediately)
  - Update submitActor to use pre-baked handles for slots 1+ instead of querying TGL pointers
- **Gain:** simplify capture loop, reduce per-submit work, clarify ownership
- **Risk:** assumes texture handles are stable per-mission (true; only paint scheme slot 0 can change per-actor)
- **Status:** low-priority optimization; current cost is negligible

### 3. **Collapse small draw calls via MDI or subranges within a bucket** (Effort: 4-8h, Gain: 0.2-0.5ms, reduce CPU overhead)

**Scope:** gos_mech_batcher.cpp flush draw loop
- **Current state:** 100-150 glDrawElementsInstanced calls per frame
- **Issue:** each draw call incurs CPU validation + GPU submission overhead (< 2μs each on Radeon, but cumulative)
- **Option A - MDI (glMultiDrawElementsIndirect):**
  - Pre-build indirect dispatch buffer (one DrawElementsIndirectCommand per bucket)
  - Upload at flush entry (after bucket sort)
  - Issue single glMultiDrawElementsIndirect call
  - **Cost:** indirect buffer reallocation + format translation (not worth it; current cost already low)
  - **Status:** deferred; CPU cost is not a bottleneck

- **Option B - Bucket subranges (keep separate draws, reduce bucket count):**
  - Relax bucket key to merge small draws sharing the same (type, packet, flags) even with different textures
  - Use separate per-draw texture bind instead of per-bucket
  - Reduces bucket count by 20-30% (typical 150 buckets → 100-120)
  - **Cost:** one extra glActiveTexture + glBindTexture per bucket subrange (offset: merged bucket count savings)
  - **Status:** acceptable incremental improvement; not blocking

### 4. **Gate updateGeometry() to visible-only (with fallback for AI dependencies)** (Effort: 6-12h, Gain: 3-7ms, large CPU savings)

**Scope:** mech3d.cpp updateGeometry + visibility gates
- **Current state:** updateGeometry runs for all mechs; visibility gates happen at submit time
- **Opportunity:** skip TransformMultiShape (bone animation) for off-screen actors
- **Challenge:** game AI may depend on updated node positions even when not rendered
  - Example: weapon fire uses getWeaponNodePosition (line 756), which reads mechShape->listOfShapes[i].shapeToWorld (set by TransformMultiShape)
  - Solution: separate path for AI queries (skeleton update) vs render (geometry transform); gate only the latter
- **Implementation:**
  1. Add `bool Mech3DAppearance::updateSkeletonOnly()` — advances animation frame but skips geometry transform
  2. In updateGeometry, check visibility gates (inView || GPU lagged-cull); if off-screen and no pending weapon fire, call updateSkeletonOnly
  3. Fallback: on-demand geometry transform if weapon fire or special case (selection, special effects) needs current node positions
- **Gain:** ~5 ms per frame on mc2_24 (46 mechs × 120 μs save × 80% off-screen ratio = 4.4 ms)
- **Risk:** careful review of AI dependencies; gate must be conservative (fail-safe to full update if any dependency suspected)
- **Status:** **HIGH-PRIORITY long-term optimization; requires architecture review with game team to confirm AI contract**

### 5. **Inline per-bucket material table lookups into shader compile (pre-bind at registerTypeLod)** (Effort: 2-3h, Gain: 0.1-0.2ms, reduce SSBO reads)

**Scope:** mech.vert materialIdx lookup from Mech-1 table (gos_mech_batcher.cpp Material Gpu SSBO at binding 2)
- **Current state:** Material Gpu SSBO optional (Mech-1 in progress); all instances upload materialIdx
- **Issue:** SSBO read per-instance (4B); one more L1 miss on dense instance load
- **Opportunity:** if material table is stable per-packet (no per-instance material variance), pack materialIdx into packet-level uniform instead of per-instance SSBO
- **Implementation:** track whether a bucket has uniform material across all instances; if yes, use glUniform1i instead of SSBO read
- **Gain:** small (one fewer SSBO load per vertex)
- **Status:** deferred; Mech-1 is still in early phase; revisit after stabilization

---

## Summary

The mech render path is **well-structured and modern** for a C++17 OpenGL 4.3 core-profile system:

- **Strengths:**
  - GPU-batched instanced draws with bucket sorting (not per-draw CPU overhead)
  - Persistent-mapped ring SSBOs with typed sync barriers (efficient streaming)
  - Proper GL state save/restore + cache invalidation (no state inheritance bugs)
  - Comprehensive killswitches for slices (A/B1/B2/C1/C2/C3; all default-on, opt-out semantics)
  - ViewUniforms UBO support for modern uniform management

- **Known Issues (Non-Blocking):**
  - AMD texture LOD sampling bug (mitigated with textureLod(0)); need mipmap generation
  - 100-150 separate glDrawElementsInstanced calls per frame (not MDI; CPU cost absorbed)
  - updateGeometry runs for all mechs including off-screen (salvageable with AI contract review)

- **Modernization Status:**
  - UBO for per-frame constants: ✓ (ViewUniforms partial, legacy path acceptable)
  - Persistent-mapped buffers: ✓ (3-frame ring, already implemented)
  - GPU sync barriers: ✓ (typed gpuSyncBarrier helper, proven on AMD)
  - MDI: ✗ (deferred; cost already low)
  - Texture handle pre-bake: ✗ (incremental optimization, low ROI)

- **Recommended Next Steps:**
  1. **Immediate (blocking visual quality):** Mipmap generation for mech textures → fix AMD sampling
  2. **Short-term (CPU performance):** Profile updateGeometry visibility gate with game AI team → potential 5ms frame-time win
  3. **Long-term (rendering fidelity):** Mech-1 Material Gpu table finalization → enables per-material properties (metallic, roughness, AO)
