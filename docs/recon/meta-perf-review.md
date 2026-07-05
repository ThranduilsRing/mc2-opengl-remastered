# MC2 OpenGL — Cross-Path Performance Meta-Review

**Date:** 2026-06-16
**Scope:** All seven render paths: mech, static props, vehicles, terrain, shadows, water, gosFX
**Auditor role:** Greybeard OpenGL (15+ years GPU/graphics, OpenGL 3.x-4.x, pre-Vulkan)
**Source audits:** docs/recon/{mech,static-props,vehicles,terrain,shadows,water,gosfx}-render-path.md

---

## 1. Cross-Cutting Patterns

### 1.1 State Inheritance (the most pervasive bug class)

The single most repeated mistake across the codebase is render passes that **do not explicitly set the GL state they depend on, relying instead on whatever the prior pass left behind.** This is the classic D3D-era port hazard: D3D had a tracked state machine that reset between passes; OpenGL does not.

**Confirmed instances:**

| Path | File | State Not Set | Consequence |
|------|------|---------------|-------------|
| Static props | gos_static_prop_batcher.cpp flush() | depth test, depth mask, blend, cull face, depthFunc | Props render transparent or on top of UI when prior pass (e.g., transparent FX) left glDepthMask=FALSE |
| Vehicles / TG_Shape | tgl.cpp TG_Shape::Render() | depth test, cull face, blend | Vehicles render without depth write if water or alpha FX ran before them |
| Terrain LOD chunk | gos_terrain_lod_chunk.cpp | depth test, blend, cull (pre-Phase10.3) | **WAS FIXED** in Phase 10.3 (f375e0ba): terrain was transparent/see-through. Fix: explicit glEnable/glDepthMask/glDisable/glDepthFunc before draw. |
| Water MDI | gameos_graphics.cpp water-pass entry | gos_InvalidateRenderStateCache() not called before draw | Fragile state inheritance from terrain-solid dispatch; explicit depth/blend/cull state IS set but cache not invalidated |

**The Phase 10.3 fix is the reference pattern.** Every path that does not do what gos_terrain_lod_chunk.cpp does after the fix is at risk. Static props and vehicles are the two remaining unfixed cases with confirmed potential for artifacts.

### 1.2 Missing gos_InvalidateRenderStateCache() After State Mutations

The `applyRenderStates` cache (see render-perf-snapshot.md: "applyRenderStates state-equality cache shipped 2026-05-08") tracks GL state. When any code mutates GL directly (not through `applyRenderStates`), it must call `gos_InvalidateRenderStateCache()` or subsequent `applyRenderStates` calls silently no-op on stale state.

**Audit results:**

| Path | Calls gos_InvalidateRenderStateCache()? | Risk |
|------|-----------------------------------------|------|
| Mech batcher (gos_mech_batcher.cpp) | YES — after full restore (line ~1183) | CORRECT |
| Static props batcher (gos_static_prop_batcher.cpp) | NO — missing entirely | MEDIUM RISK (MDI path does not mutate state directly, but missing after any future direct-GL call) |
| Vehicles / TG_Shape::Render (tgl.cpp) | NO — not present | HIGH RISK (gos_SetRenderState called per-triangle; cache can drift) |
| Terrain LOD chunk (gos_terrain_lod_chunk.cpp) | NO — missing post-draw | LOW (non-destructive; noted in terrain audit; active_campaigns S20 GlStateGuard slice 2 added RAII guards but did not add the invalidation call per audit) |
| Water MDI (gameos_graphics.cpp) | PARTIAL — water audit recommends adding before draw | MEDIUM RISK |
| GOSFX bridge (gos_particle_bridge.cpp) | YES — at lines 1183, 813, 556 | CORRECT |
| Shadows (gos_postprocess.cpp) | NOT NEEDED — GL state is fully explicit per-cascade | CORRECT |

### 1.3 Redundant applyRenderStates() / gos_SetRenderState() Per Object Instead of Per Batch

The vehicle path (TG_Shape::Render) calls `gos_SetRenderState()` once per triangle when the texture changes (tgl.cpp:3037-3046). For 50 vehicles with 3 textures each and 50 triangles per vehicle, this is potentially 150 state-check calls per frame — within a path that already has no inter-vehicle batching. Each gos_SetRenderState() call touches the legacy state cache; if state is unchanged, cost is low (~5ns), but the calls still stack up.

The mech and static prop paths avoid this correctly: state is set once at flush entry, not per draw.

### 1.4 Per-Pass MVP/Uniform Uploads That Could Be Shared via UBO

**Current state across paths:**

| Path | Per-Frame Uniforms | UBO? |
|------|--------------------|------|
| Mech | 6+ glUniform calls per flush | Partial (MC2_MECH_VIEWUNIFORMS, default-ON) |
| Static props | ~15 glUniform calls per flush | No UBO |
| Vehicles | Per-shape fog/light re-upload | No UBO |
| Terrain indirect | ~25 glUniform calls per-frame | No UBO (recommended in terrain audit) |
| Terrain LOD chunk | 20+ glUniform calls per block | No UBO |
| Shadows | Matrix array per cascade | No UBO |
| Water | ~15 glUniform calls per dispatch | No UBO |
| GOSFX | 15+ uniforms per flush | No UBO |

Every path except mechs (partially) uploads the same or similar per-frame constants (MVP/worldToClipGL, camera position, fog params, lighting direction) as individual glUniform calls. A single `FrameConstants` UBO bound at frame start would eliminate 80-90% of these calls.

**Cross-path redundancy:** `u_worldToClipGL` is uploaded independently in at least five places: terrain compute dispatch, terrain LOD chunk SubmitDrawCommands, water stream, mech batcher flush, and GOSFX particle bridge. All use the same camera matrix. One UBO at frame start, bound to a fixed slot (e.g., binding=0), eliminates all redundant uploads.

### 1.5 Paths That Could Merge Into One MDI Submit But Are Not

**Current MDI usage:**
- Terrain solid: glMultiDrawArraysIndirect (ONE call, optimal)
- Terrain water: glMultiDrawArraysIndirect (ONE call, optimal)
- Static props (v6 default): glMultiDrawElementsIndirect (ONE call, optimal)
- Mech: 100-150 glDrawElementsInstanced (bucket-sorted but not MDI)
- Vehicles: per-triangle gos_DrawTriangles() (no batching)
- Shadows (all): per-type glDrawElementsInstanced (loop, not MDI)
- GOSFX billboards: per-group glDrawArrays (loop, not MDI)
- GOSFX ribbons: per-record glDrawElements (loop, not MDI)

The shadow path issues 5-20 glDrawElementsInstanced calls for static props plus 1-5 for mechs per cascade. With CSM (3 cascades) this multiplies to 18-75 calls. These could be collapsed into 1-2 MDI calls per cascade by pre-building the command buffer at caster-set-rebuild time (already done in the static prop v6 path; the shadow sub-path does not reuse it).

---

## 2. Batching Gap Analysis

### 2.1 Which Paths Still Use Per-Object Draw Calls

| Path | Current Model | Draw Call Count | MDI Feasible? |
|------|--------------|-----------------|---------------|
| Terrain solid | glMultiDrawArraysIndirect | 1 | N/A (already done) |
| Terrain water | glMultiDrawArraysIndirect | 1 | N/A (already done) |
| Static props (opaque) | glMultiDrawElementsIndirect | 1 | N/A (already done) |
| Mech main | glDrawElementsInstanced per bucket | 100-150 | YES — see §3 |
| Mech shadow | glDrawElementsInstanced per bucket | 100-150 per cascade | YES — same bucket structure |
| Vehicles | gos_DrawTriangles() per triangle | 1k-20k | BLOCKED — heterogeneous vertex layout (D3D era CPU-projected) |
| Shadow (prop casters) | glDrawElementsInstanced per type | 5-20 per cascade | YES — command buffer already exists for main pass |
| GOSFX billboards | glDrawArrays per group | 1-50 per dense frame | YES — groups share shader + SSBO layout |
| GOSFX ribbons | glDrawElements per record | 1-20 per dense frame | YES, but blend-mode split required |

### 2.2 What Prevents Batching

**Mechs:** Paint-scheme textures are per-actor, so bucket key includes texture handle. N unique paint schemes = N buckets even for identical meshes. A texture atlas (per-type paint scheme atlas with per-instance UV offset in the instance SSBO) would collapse N buckets to 1 per mesh-packet. This is the `Mech-1` work in progress.

**Vehicles:** The CPU pre-projects vertices to screen space (TG_Shape::Render, gos_tex_vertex.vert MVP=identity). Moving to MDI requires world-space vertex submission, which is a full render contract change (Bucket B vs. A in docs/render-contract.md). Blocker is technical, not laziness.

**Shadow prop casters:** The main static prop MDI path already builds the command buffer; the shadow path rebuilds a separate per-type loop. Reusing the main command buffer (with a shadow-specific shader program) would collapse the loop to 1 MDI call per cascade.

**GOSFX billboards:** Blend mode (alpha vs additive) splits groups into two batches. Two MDI calls per flush (one per blend mode) would handle this; the texture binding could be handled via a texture array (all particle atlases in one `sampler2DArray`, indexed by per-group `atlasIndex` from a per-draw SSBO).

### 2.3 Draw Call Reduction Estimates

| Path | Current Calls | After MDI | Reduction |
|------|--------------|-----------|-----------|
| Mech main (average) | ~125 | 1-2 | 98% |
| Mech shadow (CSM 3x) | ~375 | 3-6 | 98% |
| Shadow prop casters (CSM) | 15-60 | 3 (1/cascade) | 80-95% |
| GOSFX billboards | 10-50 | 2 (alpha+additive) | 90-95% |
| GOSFX ribbons | 1-20 | 2 (alpha+additive) | 80-90% |

---

## 3. MDI Opportunity Map

### 3.1 Mech Main Pass (gos_mech_batcher.cpp)

**Current:** 100-150 glDrawElementsInstanced per flush, one per (typeLodIdx, globalPacketIdx, texHandle, materialFlags) bucket.

**Blocker:** Paint scheme textures. Each unique texHandle forces a separate bucket because the shader reads `sampler2D u_tex` not a sampler2DArray. The bucket key includes texHandle; same-type mechs with different paint schemes cannot share a draw command.

**Path to MDI:**
1. Move paint schemes into a `sampler2DArray` (Texture2DArray, one slice per registered paint scheme handle).
2. Store the slice index in the per-instance GpuMechInstance SSBO (a new `uint16_t paintSlot` field, 2 bytes).
3. Shader reads `texture(u_texArr, vec3(v_uv, inst.paintSlot))` instead of `texture(u_tex, v_uv)`.
4. Bucket key drops texHandle — all buckets sharing (typeLodIdx, globalPacketIdx, materialFlags) collapse to one.
5. Pre-build DrawElementsIndirectCommand array at flush entry; issue `glMultiDrawElementsIndirect`.

**Technical classification:** Not-done-yet (the instancing and SSBO infrastructure is already in place; paint-scheme atlas is the missing piece). This is the `Mech-1` campaign.

**Effort:** L (8-16 hours: texture atlas pipeline + instance SSBO layout change + shader rewrite + AMD validation)
**Gain:** ~125 fewer draw calls per frame; ~0.2-0.5ms CPU overhead on dense missions

### 3.2 Shadow Prop Casters (gos_postprocess.cpp / gos_static_prop_batcher.cpp)

**Current:** Per-cascade per-type glDrawElementsInstanced (flushShadow loop, shadow-specific per-type SSBO bind).

**Path to MDI:**
1. At caster-set-rebuild time (`getDynamicPropShadowInstances()`), build a shadow-specific DrawElementsIndirectCommand buffer (reuse the same type-sorted instance SSBO as the main MDI path but with shadow-specific byte ranges).
2. Upload cascade light-space matrix to a per-cascade UBO.
3. Issue `glMultiDrawElementsIndirect(shadowCmdBuffer, shadowTypeCount)` per cascade.

**Technical classification:** Not-done-yet. The infrastructure (per-type instance SSBOs, `shadow_static_prop.vert`) is complete; only the command buffer generation is missing.

**Effort:** M (4-6 hours per cascade pass)
**Gain:** 15-60 fewer calls per CSM frame on prop-heavy missions; most visible at mc2_24 with 14K+ props

### 3.3 GOSFX Billboards (gos_particle_bridge.cpp)

**Current:** Per-group glDrawArrays with per-group SSBO upload, texture bind, and uniform set.

**Path to MDI:**
1. Pack all GpuParticle records into a single large SSBO (already effectively done per-group; extend to contiguous across groups).
2. Store per-group metadata (atlasTexArrayLayer, uvOffset, atlasColumns, startParticle, count) in a per-draw SSBO indexed by `gl_DrawIDARB`.
3. All particle textures loaded into a `sampler2DArray`; per-group layer index in per-draw SSBO.
4. Sort groups: alpha-blend groups first, additive groups second.
5. Issue two `glMultiDrawArraysIndirect` calls (one per blend mode).

**Technical classification:** Not-done-yet. The per-group SSBO architecture is a stepping stone; the atlas conversion is the real work.

**Effort:** M-L (6-10 hours: atlas pipeline + per-draw SSBO + shader change)
**Gain:** 10-50 fewer calls on dense FX frames; eliminates per-group glBlendFunc override

### 3.4 GOSFX Ribbons (gos_tube_ribbon_flush_deferred)

**Current:** Per-record separate SSBO uploads and glDrawElements. Cannot share an IBO (each ribbon has variable vertex count).

**Path to MDI:**
1. Pre-allocate a large position/color/UV SSBO (e.g., 16K vertices). Accumulate all ribbon records contiguously.
2. Build an indirect command buffer: one entry per ribbon, with firstIndex and count.
3. Build a combined IBO covering all ribbons (concatenated per-ribbon index arrays, offset by per-ribbon vertex base).
4. Issue `glMultiDrawElementsIndirect` (split by blend mode: alpha vs additive).

**Technical classification:** Not-done-yet, but requires ribbon batcher refactor (the current model copies mesh data per-record). The MRT fix (save/restore glDrawBuffers for AMD integer attachment suppress) still applies to the whole pass, not per-record.

**Effort:** L (8-12 hours)
**Gain:** 1-20 fewer calls per dense frame; eliminates per-record SSBO resize + MRT save/restore overhead per record

---

## 4. GL State Guard Audit

### 4.1 "GL State Orphans" — Passes That Mutate Without Restoring

An "orphan" pass sets GL state and either does not restore it or relies on the next pass to tolerate whatever it left.

| Pass | Sets | Restores? | Next Victim |
|------|------|-----------|-------------|
| Shadow pass (beginShadowPass) | GL_DEPTH_TEST=LESS, glDepthMask=TRUE, GL_POLYGON_OFFSET_FILL, glColorMask=FFFFFF, GL_CULL_FACE=off | YES (endShadowPass/beginDynamicShadowCascade) | Terrain correctly handles forward/reverse-Z switch |
| Terrain LOD chunk (post-Phase 10.3) | GL_DEPTH_TEST=GEQ, glDepthMask=TRUE, GL_BLEND=off, GL_CULL_FACE=off | YES (GlStateGuard RAII, S20) | CORRECT |
| Terrain indirect solid | Inherits depth/blend from caller | No explicit set | Water — FRAGILE |
| Static props batcher (flush) | Inherits depth/blend from caller | No explicit set | Vehicles or post-process — FRAGILE |
| Vehicles (TG_Shape::Render) | Per-tri gos_SetRenderState for texture/alpha/fog | No full restore | Next pass (usually UI) inherits whatever the last tri set |
| GOSFX billboard bridge | GL_DEPTH_TEST=GEQ, glDepthMask=FALSE, GL_BLEND=on, GL_CULL_FACE=off | YES (full restore + cache invalidate) | CORRECT |
| GOSFX ribbon bridge | GL_BLEND override per-record; MRT save/restore | YES (full restore + cache invalidate) | CORRECT |

**Most dangerous state left unrestored:** `glDepthMask(GL_FALSE)` from any transparent pass that does not restore it. The confirmed instance from HANDOFF 2026-06-14 (terrain transparent to mechs when FX pass left glDepthMask=FALSE) is the archetype. Static props and vehicles are the current un-guarded passes most likely to hit this.

### 4.2 Most Dangerous Cross-Contamination Pairs

**Shadow -> Terrain:**
- Shadow sets forward-Z (`glDepthFunc(GL_LESS)`, `glClearDepth(1.0)`)
- Terrain uses reverse-Z (`glDepthFunc(GL_GEQUAL)`, `glClearDepth(0.0)`)
- Risk: if endShadowPass() restore is skipped or partial, terrain depth test inverts → nothing passes depth → black screen or Z-fighting
- Current mitigation: beginDynamicShadowCascade() saves/restores explicitly. Non-cascade (static) path has its own glClearDepth toggle. Fragile if a new caller is added between them.

**Transparent FX / Water -> Static Props:**
- Transparent pass sets `glDepthMask(GL_FALSE)` (correct for transparency)
- Static prop flush has no explicit `glDepthMask(GL_TRUE)` call
- Risk: static props render color but write no depth → objects appear transparent, Z-order breaks
- Current mitigation: none. This is the highest-priority unfixed case.

**Alpha-blended mesh -> Vehicles:**
- Same pattern as above but for vehicles (TG_Shape::Render with no glDepthMask reset)
- If mech shadow pass (which does set glDepthMask) runs last before vehicles, vehicles are safe; if not, they inherit whatever was last set

**GOSFX (glDepthMask=FALSE, glEnable(GL_BLEND)) -> Any Opaque Pass:**
- GOSFX correctly restores after itself, so the downstream pass gets prior state
- But if a "prior state" was itself broken (inherited from something upstream), the restore perpetuates the corruption
- This is why explicit set-at-entry is better than save/restore: it breaks the corruption chain

### 4.3 Recommended Guard Pattern

**Recommendation: explicit set at entry of each pass, not save/restore.**

Save/restore is expensive (~10-12 glGetIntegerv calls = ~1.2 µs) and brittle (restores a bad prior state if the prior pass was itself buggy). The mech batcher does save/restore (inherited from early implementation); the terrain LOD chunk (post-Phase 10.3) does explicit set. The terrain approach is correct.

The pattern to follow is exactly what gos_terrain_lod_chunk.cpp does after Phase 10.3 and what the GOSFX bridge does for entry:

```cpp
// At pass entry — do NOT query prior state, just set what you need
glEnable(GL_DEPTH_TEST);
glDepthFunc(GL_GEQUAL);       // or GL_LESS for shadow, GL_GEQUAL for reverse-Z scene
glDepthMask(GL_TRUE);         // TRUE for opaque, FALSE for transparent
glDisable(GL_BLEND);          // or glEnable + glBlendFunc for transparent
glCullFace(GL_BACK);          // or glDisable for double-sided
gos_InvalidateRenderStateCache();
// ... pass draws ...
gos_InvalidateRenderStateCache();  // again after, so next pass's applyRenderStates doesn't no-op
```

For the specific case of the mech batcher (which does need to restore because it is called mid-frame at a position where callers depend on prior state), retain the save/restore but switch from glGetIntegerv to C++-tracked state via the applyRenderStates cache — cheaper and doesn't round-trip to the driver.

---

## 5. Shader Redundancy

### 5.1 CPU-Side Work the GPU Already Does in Another Shader

**Vehicle CPU projection vs. GPU:**
- `TG_Shape::Render` projects every vertex to screen space on CPU (through Camera::projectZ or equivalent), then `gos_tex_vertex.vert` multiplies by an identity MVP matrix and divides by pos.w again.
- This is documented in vehicles audit (tgl.cpp:2994, gos_tex_vertex.vert:16-17).
- The GPU does a no-op matrix multiply + a redundant perspective divide.
- Fix: change gos_tex_vertex.vert line 16 to `gl_Position = vec4(pos.xyz, 1.0)` and skip the MVP upload for this shader. Coordinate with render contract Bucket B (vehicles are intentionally projected-space).

**Terrain LOD chunk vs. terrain indirect solid — bilinear height:**
- `terrain_lod_chunk.frag` calls heightBilinear() 3 times per fragment (dhdx, dhdrow, and once inside POM ray-march).
- The indirect solid path bakes height into the thin record at compute time.
- Chunk frag could cache the 2×2 sample grid used for dhdx/dhdrow and reuse it for POM, reducing 3 separate 4-sample lookups to a single shared lookup.

**GOSFX soft-particle world reconstruction:**
- `particle_billboard.frag` reconstructs world position from gl_FragCoord.z via `u_invWorldToClip` multiply per fragment (lines 134-141).
- The vertex position is already available in VS and could be forwarded as a varying (`v_worldPos`), eliminating the per-fragment matrix multiply.
- The scene depth reconstruction still requires a texture fetch + matrix multiply (unavoidable), but the fragment world-pos reconstruction can be replaced by a VS-interpolated varying.

### 5.2 Per-Vertex Transforms That Could Be Instanced/Indirect

**Static prop normal transform:**
- `static_prop.vert` computes `a_normal * mat3(M)` per vertex for every instance of the same type.
- For axis-aligned trees (the majority of static props), all instances share the same orientation; `mat3(M)` is identical across all.
- Pre-compute the 3×3 rotation matrix per type at registration time; store in the per-type data SSBO (binding 2 already hosts hot-color data). Read in VS per-instance via typeID.
- Cost: 9 float reads vs. 9 float multiplies per vertex. At 50K visible prop vertices per frame, this is a measurable reduction.

**Mech bone matrix accumulation:**
- `mech.vert` (Slice A rigid path) loads a full 4×4 bone matrix per vertex from the SSBO.
- For the common case where all vertices in a packet share the same bone (rigid pieces), the matrix is constant per draw call.
- Could store the bone matrix in a per-draw uniform (push constant equivalent) when the bucket has uniform bone index — avoids one SSBO read per vertex.
- Low ROI given that mech geometry is already GPU-transform bandwidth-bound, not ALU-bound.

### 5.3 Fragment Shaders Sampling the Same Texture Multiple Times

**terrain_lod_chunk.frag — heights SSBO read 3×:**
Documented above. heightBilinear() called for dhdx, dhdrow, and POM ray-march entry. A single 2×2 fetch could serve all three.

**static_prop.frag — PerDrawEntry SSBO read 6×:**
`perDraw_.entries[v_drawID + uint(u_drawIDBase)]` accessed 6 separate times for materialFlags, packetID, maxLocalVertexID, texArrayLayer, uvScaleX, uvScaleY. GLSL compilers on AMD RDNA3 may not always coalesce these into a single SSBO load if they are separated by control flow. Fix:

```glsl
// static_prop.frag, after coalesce-mode check
PerDrawEntry pde = perDraw_.entries[v_drawID + uint(u_drawIDBase)];
int materialFlags  = pde.materialFlags;
int texArrayLayer  = pde.texArrayLayer;
float uvScaleX     = pde.uvScaleX;
float uvScaleY     = pde.uvScaleY;
// ... use pde.* throughout
```

**particle_billboard.frag — textureLod(uAtlas) then alpha discard:**
Texture is sampled at line 79, then discarded at line 79 (colorkey) and again at line 84 (alpha < 0.01). These are sequential; the discard logic is fine as-is. No redundant sample.

**shadow_screen.frag — scene depth + shadow maps:**
Reconstructs world position from scene depth (one texture fetch + matrix multiply). Then samples static shadow map (8 Poisson taps) and dynamic shadow map (4 taps per cascade). This is correct — no redundancy within the shader. The Poisson rotation computation (sin/cos per tap) is repeated for each sample; a precomputed 16-entry rotation table as a const array would avoid the trig.

---

## 6. Top 10 Actionable Findings (Ranked by Gain/Effort)

### Rank 1: Add explicit GL state guards to static prop flush()

**Path:** Static props — gos_static_prop_batcher.cpp::flush()
**Issue:** No explicit depth/blend/cull state set at flush entry. Inherits from prior pass (transparent FX, water, GOSFX). If glDepthMask is FALSE from a prior transparent pass, all static props render with no depth write.
**Fix:** Add before first draw in flush():
```cpp
glEnable(GL_DEPTH_TEST);
glDepthMask(GL_TRUE);
glDisable(GL_BLEND);
glDepthFunc(GL_GEQUAL);
gos_InvalidateRenderStateCache();
```
Add after flush() draws:
```cpp
gos_InvalidateRenderStateCache();
```
**Effort:** S (1-2 hours)
**Gain:** Eliminates a class of silent transparency/occlusion bugs that are present but may be masked by current frame ordering. Prevents regressions when frame ordering changes. Reference: terrain LOD chunk f375e0ba fix.

### Rank 2: Add explicit GL state guards and gos_InvalidateRenderStateCache() to TG_Shape::Render()

**Path:** Vehicles — tgl.cpp::TG_Shape::Render(), lines 2958-2987
**Issue:** Inherits all GL state from prior pass. No explicit depth, blend, or cull set at render entry. Per-triangle gos_SetRenderState() calls for texture and alpha only.
**Fix:** At entry to TG_Shape::Render (before the per-face loop at line 2987):
```cpp
glEnable(GL_DEPTH_TEST);
glDepthMask(GL_TRUE);
glDepthFunc(GL_LEQUAL);  // vehicles are forward-Z (CPU-projected, Bucket B)
glDisable(GL_BLEND);
gos_InvalidateRenderStateCache();
```
Add gos_InvalidateRenderStateCache() after SetFogRGB() (line 3967) and after any SetLightColor/setLightIntensity calls.
**Effort:** S (2 hours)
**Gain:** Fixes category of visual artifacts on vehicle rendering when a transparent or alpha-blend pass precedes them in frame order.

### Rank 3: Enable MC2_SHADOW_CASTER_LIGHTBOX_CULL as default (or expose in ImGui)

**Path:** Shadows — gos_postprocess.cpp / txmmgr.cpp
**Issue:** `MC2_SHADOW_CASTER_LIGHTBOX_CULL` gate is default OFF. It filters prop casters to those within the light-space AABB (off-map trees do not cast shadows on in-map objects, but still consume GPU rasterization time in the shadow pass).
**Fix:** Either flip default to ON in code (with kill-switch MC2_SHADOW_CASTER_LIGHTBOX_CULL=0), or expose as an ImGui checkbox in the profiler overlay. The shadow audit section 7.3 provides the 5-line code change.
The default-inversion in code (not just ImGui) is preferred because it benefits headless smoke runs too.
**Effort:** S (15 minutes for code default flip; 2-3 hours for ImGui toggle)
**Gain:** ~0.2-0.4ms per frame on dense-prop missions (mc2_24). The shadow audit measured ~50% off-map caster reduction.

### Rank 4: Formalize MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY default-ON in code

**Path:** Shadows — gos_postprocess.cpp or txmmgr.cpp
**Issue:** Currently env-var default-ON, but if env is not set (e.g., fresh run without .env file), full 14K-recipe registry walk runs every frame. The shadow audit section 7.2 shows the 3-line fix.
**Fix:**
```cpp
static const bool s_dynPropDirtyOnly = []() {
    const char* v = getenv("MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY");
    return !(v && v[0] == '0' && v[1] == '\0');  // default ON; =0 kills
}();
```
**Effort:** S (15 minutes — invert the ternary, update comment, test mc2_01/mc2_24)
**Gain:** -120µs per frame on large-map missions (mc2_24 with 14K props) when env is not explicitly set. Already measured per shadow audit and render-perf-snapshot.md.

### Rank 5: Add water state cache invalidation before MDI draw

**Path:** Water — GameOS/gameos/gameos_graphics.cpp water-pass entry
**Issue:** Water pass does not call `gos_InvalidateRenderStateCache()` before its MDI draw. Per the water audit section 3, state is inherited from prior terrain-solid dispatch; terrain-solid path changes differ between config. The water pass explicitly sets depth/blend/cull (which is good), but does not notify the cache-tracking layer.
**Fix:** Add `gos_InvalidateRenderStateCache()` immediately before the water MDI glMultiDrawArraysIndirect call (same location as the explicit depth/blend/cull sets already present). Add again after.
**Effort:** S (30 minutes)
**Gain:** Prevents silent no-ops in subsequent applyRenderStates calls after water draw. Matches terrain-solid discipline.

### Rank 6: Coalesce PerDrawEntry SSBO reads in static_prop.frag into one struct load

**Path:** Static props — shaders/static_prop.frag (coalesce mode)
**Issue:** `perDraw_.entries[v_drawID + uint(u_drawIDBase)]` accessed 6+ times separately (materialFlags, texArrayLayer, uvScaleX, uvScaleY, maxLocalVertexID, packetID). AMD RDNA3 compiler may not coalesce if accesses are split by control flow.
**Fix:** Read the full PerDrawEntry into a local struct at the top of main(), then reference local fields:
```glsl
PerDrawEntry pde = perDraw_.entries[v_drawID + uint(u_drawIDBase)];
int materialFlags = pde.materialFlags;
// ... etc.
```
**Effort:** S (30 minutes)
**Gain:** 1-3% fragment throughput on dense prop scenes; eliminates 5 potential extra SSBO loads per fragment.

### Rank 7: Move soft-particle world reconstruction to vertex shader

**Path:** GOSFX — shaders/particle_billboard.vert + particle_billboard.frag (lines 134-141)
**Issue:** Per-fragment world position reconstruction from gl_FragCoord.z via full `u_invWorldToClip` matrix multiply. For a 10-pixel billboard that's 10 matrix multiplies where 1 VS interpolation would suffice.
**Fix:**
```glsl
// VS: add output
out vec3 v_worldPos;
// in main(): v_worldPos = worldPos;  // already computed for gl_Position

// FS: remove the fragment world reconstruction, use interpolated value
// Still need scene depth fetch + world-reconstruct for the scene position (unavoidable)
// But fragment world pos = v_worldPos (free via interpolation)
float fade = clamp(distance(wScene, v_worldPos) / u_softDistance, 0.0, 1.0);
```
**Effort:** S (1 hour — VS out + FS change; AMD test for interpolation correctness)
**Gain:** ~0.3ms on dense soft-particle frames (the GOSFX audit section 7.4 gives the detailed estimate). Higher gain on high-res (4K) or particle-heavy missions.

### Rank 8: Consolidate scattered glUniform calls into per-path UBOs

**Path:** Terrain (chunk path) — GameOS/gameos/gos_terrain_lod_chunk.cpp (25+ glUniform calls per frame)
**Issue:** 20+ individual glUniform1i/glUniform1f/glUniform4f/glUniformMatrix4fv calls in gos_terrain_lod_chunk.cpp:620-757 (per-frame at draw submission time). This is the most verbose single offender.
**Fix:** Pack into a `FrameConstants` UBO (std140) containing MVP, lightDir, atlas params, and material tunables. The terrain audit section 7.1 provides the struct definition. Bind once before compute + chunk draw.
Same refactor can be applied to water (15 uniforms) and GOSFX (15 uniforms) as separate tickets.
**Effort:** M (2-3 hours per path; terrain first as highest-impact)
**Gain:** ~0.5ms per-frame CPU upload reduction at 25+ calls → 1 bind; improves shader interface clarity for future maintenance.

### Rank 9: Gate static terrain shadow under explicit env variable for headless/smoke use

**Path:** Shadows — gos_postprocess.cpp / txmmgr.cpp
**Issue:** Static terrain shadow rebuild runs unconditionally on every mission load (200-400ms stutter). Headless smoke runs do not need it. The shadow audit section 7.1 provides the 3-line fix.
**Fix:** Add `MC2_STATIC_SHADOW_ENABLE` gate (default ON) so headless/smoke can set `=0`:
```cpp
if (gos_IsTerrainTessellationActive() && !gos_StaticLightMatrixBuilt() &&
    Terrain::mapData && gos_IsStaticShadowEnabled()) {  // new guard
```
**Effort:** S (30 minutes)
**Gain:** -200-400ms per mission load in headless mode; zero impact on interactive users (default ON). Also allows future headless optimization passes to skip shadow setup.

### Rank 10: Gate mech updateGeometry() to visible actors only

**Path:** Mechs — mech3d.cpp::Mech3DAppearance::updateGeometry(), lines 2390-2656
**Issue:** TransformMultiShape (skeleton animation) runs unconditionally for all mechs regardless of screen visibility. Cost ~150µs per mech; 46 mechs on mc2_24 = ~7ms.
**Fix:** The mech audit section 7.4 outlines the two-step approach:
1. Add `bool Mech3DAppearance::updateSkeletonOnly()` — advances animation frame counter only, skips geometry transform.
2. In updateGeometry(), check inView/GPU-lagged-cull gates early; if off-screen and no pending weapon-fire AI queries, call updateSkeletonOnly() instead of the full TransformMultiShape.
**Effort:** L (6-12 hours — requires AI dependency review for weapon node positions, footprint state; fail-safe to full update if in doubt)
**Gain:** ~5ms per frame on mc2_24 at 80% off-screen ratio; ~3.5ms realistic (some mechs will always be borderline-visible). Largest single CPU frame-time win available.

---

## 7. Already In-Flight / Do Not Duplicate

The following findings from the per-path audits are already addressed by known campaigns. Do not re-derive or re-spec them.

### Addressed by active_campaigns.md (shipped or in progress)

| Finding | Campaign | Status |
|---------|----------|--------|
| Terrain GL state inheritance (LOD chunk transparent) | Phase 10.3 f375e0ba + S20 GlStateGuard slice 2 (783406a8) | SHIPPED |
| Static prop dirty-only snapshot | STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1 (cf654080) | SHIPPED, default-ON |
| Static prop persistent buckets | MC2_STATIC_PROP_PERSISTENT_BUCKETS (096d87d1/93ab852c) | SHIPPED, default-ON |
| Shadow dynamic prop caster dirty cache | MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY (b4dbaf3c) | SHIPPED, default-ON (but code default still env-driven — Rank 4 above) |
| Mech ViewUniforms UBO | MC2_MECH_VIEWUNIFORMS (gos_mech_batcher.cpp:132) | SHIPPED, default-ON |
| Mech persistent-mapped ring SSBO | 3-frame ring at gos_mech_batcher.cpp:1095+ | SHIPPED |
| Mech MaterialGpu table persistent + dirty-gated upload | TRACKV CPU quick wins (26974734) | SHIPPED |
| StaticProp light upload split | STATICPROP-PERMANENT-INSTANCE-LIGHTS-1 | SHIPPED, default-ON |
| Terrain compute dispatch / VPL retirement | VertexProjectLoop retirement COMPLETE 2026-05-18 | SHIPPED |
| Rain batch (DrawLines) | MC2_RAIN_BATCH (12ee1205) | SHIPPED, default-ON |

### Addressed by render-perf-snapshot.md decisions (already measured/decided)

| Finding | Decision |
|---------|----------|
| Lifecycle gating for mechs | PAUSED — strategic; Track D obsoletes it |
| GPU compute for full particle physics | VFX GPU sim PARITY slice — in roadmap, not active |
| makeLists / slimReduce optimization | LOAD-BEARING; hands-off until GPU-cull port |
| Static prop v5 legacy dispatch retirement | DrawPacket v8 live-builder retired (96c27c2a); v5 retained as kill-switch only |

### In-progress (do not start parallel work)

| Finding | Campaign |
|---------|----------|
| Mech paint-scheme texture atlas (MDI blocker) | Mech-1 (active, gos_mech_batcher.cpp + registerTypeLod) |
| GOSFX GPU particle simulation | VFX GPU sim Cardcloud parity (queued, spec at docs/vfx-gpu-sim-spec.md) |
| Terrain overlays/decals world-space batch | Priority 2 in render-contract.md; spec at docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md |
| Water reflection clip fix | WATER-REFLECTION-CLIP-1 (known open issue, deferred to water-v2) |
| H2 fast-path disruption recon | Hitch stability track — must complete before Phase 8z |

---

## 8. Summary Matrix

| Finding | Path | File | Effort | Gain | Risk |
|---------|------|------|--------|------|------|
| GL state guards at static prop flush entry | Static props | gos_static_prop_batcher.cpp | S | High (bug fix) | Low |
| GL state guards at TG_Shape::Render entry | Vehicles | tgl.cpp | S | High (bug fix) | Low |
| Enable lightbox cull default-ON | Shadows | gos_postprocess.cpp | S | -0.2-0.4ms | Low |
| Formalize dirty-prop-cache default-ON | Shadows | txmmgr.cpp | S | -120µs | Low |
| Water gos_InvalidateRenderStateCache() | Water | gameos_graphics.cpp | S | Safety | Low |
| PerDrawEntry single-load in static_prop.frag | Static props | shaders/static_prop.frag | S | 1-3% frag | Low |
| Soft-particle reconstruction to VS | GOSFX | particle_billboard.vert/frag | S | ~0.3ms dense FX | Low |
| Terrain glUniform → UBO | Terrain | gos_terrain_lod_chunk.cpp | M | ~0.5ms | Low |
| Static shadow gate for headless | Shadows | gos_postprocess.cpp | S | -200ms load | Low |
| Gate mech updateGeometry to visible | Mechs | mech3d.cpp | L | ~5ms mc2_24 | Medium (AI review) |

---

## 9. Cross-Path Depth-State Partition Risk

One structural risk deserves a dedicated note: the project runs **reverse-Z for scene passes and forward-Z for shadow passes.** This partition is explicit and state-captured, which is correct. The fragility is that any pass between a shadow cascade end and the next scene draw that silently flips depthFunc without being captured will cause scene depth to test backward. Current mitigation is the explicit state set in beginDynamicShadowCascade() save/restore. The recommended additional hardening is: add a debug assertion (`MC2_RENDER_CONTRACT_ASSERT=1`) that checks `glGetIntegerv(GL_DEPTH_FUNC)` == GL_GEQUAL at the start of each scene-path draw, failing loudly if forward-Z was accidentally inherited.

---

*Document generated 2026-06-16. Re-audit trigger: any new render pass added, or when any of Ranks 1-5 are fixed (state of affected paths changes).*
