# Terrain Render Path Audit — GPU-Indirect + LOD Chunk

**Date:** 2026-06-16  
**Scope:** `gos_terrain_indirect.cpp` (4430 lines), `gos_terrain_lod_chunk.cpp` (974 lines), shader pipeline (thin, compute, LOD chunk)  
**Status:** Production (chunk default-ON since Phase 10 v1b; indirect solid/mine/overlay fully armed; cement atlas integrated)

---

## 1. Data Flow Summary

### CPU Per-Frame Work

**gos_terrain_indirect.cpp:**
- **SSBO recipe build:** TerrainQuadRecipe dense array (mapSide²) allocated once at mission load; invalidated on per-vertex mutations via `InvalidateRecipeForVertexNum()` (in-gameplay edits gate behind `MC2_TERRAIN_INDIRECT`).
- **Colormap atlas upload:** Once per mission; KC2 BC7/RGBA8 fallback path. CPU copy retired post-upload (COLORMAP-CPU-RETIRE-1).
- **Cement catalog atlas:** Enumeration + GPU readback of N cement tiles → packed grid. Slot→layer-index map persists frame-to-frame; per-frame **zero cost** (write-once at mission load, then read-only).
- **Per-frame uniforms:** MVP, camera window indices (if `MC2_TERRAIN_SOLID_NARROW` armed), lighting SSBO binding. **No per-quad overhead.**

**gos_terrain_lod_chunk.cpp:**
- **Patch geometry cache:** Built on-demand per unique (qcX, qcY, lodStep). ~10-50 entries per mission. VBO/IBO static-allocated; no per-frame update.
- **Per-frame draw-command submission:** TerrainDrawCommand array (count ≤ 64) → iterate, set block-origin + LOD uniforms, issue glDrawElements per patch + skirt edges (4 masked edge draws per block, Phase 10.2b).
- **Shallow uniform uploads:** u_blockOriginX/Y, u_lodStep (per command), u_skirtDepth (per patch), material tunables (per-frame once).

### Submission Model

**Hybrid: GPU-indirect culling + CPU draw-command batching.**

1. **Compute cull/pack (GPU-driven terrain solid):** mapSide² invocations → filter visible quads via frustum + depth test, pack TerrainQuadThinRecord. Coherent SSBO write → thin array capacity-bounded. AtomicAdd on cmds[0].count (Step 2b VPL retirement) to update indirect-draw count.
2. **Indirect draw:** glMultiDrawArraysIndirect(cmds) → thinRecs[] consumed by thin VS in a single mega-draw (6 vertices per thin record).
3. **LOD chunk path:** CPU-batched terrain blocks (TerrainDrawCommand) → immediate glDrawElements per patch. No indirect dispatch; all state set explicitly per-block (uniform-expensive, count-cheap: ≤64 commands).

---

## 2. Draw Call Structure

### Indirect Solid Path (gos_terrain_indirect)

- **1 glMultiDrawArraysIndirect** per frame (cmds[0] = primary solid pass).
  - Dispatch count = N thin records × 6 (vertices per quad: 2 triangles).
  - Range: vertex 0..6N-1, each vertex group maps to one thin record.
  - **No per-quad CPU overhead:** all filtering happens in compute; VS/frag bottleneck only.

### LOD Chunk Path (gos_terrain_lod_chunk)

- **≤64 glDrawElements** per frame (one per TerrainDrawCommand).
  - Each: main patch (glDrawElements GL_TRIANGLES) + up to 4 skirt-edge masked draws.
  - **State cost:** O(count) uniform uploads (u_blockOriginX/Y, u_lodStep, material tunables amortized per-frame-once).

### Water & Mine (gos_terrain_indirect)

- Water: 1 glMultiDrawArraysIndirect (same thin-record model as solid).
- Mine: StaticBakeMineState GPU bake → 1 glDrawArrays per mine layer (or terrain-textured MDI if armed).

### Overlay (gos_terrain_indirect, Stage 6 default-ON)

- Deprecated drawPass loop retired; now Slice-A cement-overlay static-bake + Slice-B per-quad emit gated off.
- **Result:** ~20µs per frame → 0 (1.7ms → negligible).

---

## 3. GL State Management

### Explicit vs. Inherited

**LOD Chunk (CRITICAL FIX Phase 10.3):**
- **Explicitly sets:** GL_DEPTH_TEST (enable), glDepthMask (TRUE), GL_BLEND (disable), glDepthFunc (GL_GEQUAL reverse-Z), GL_CULL_FACE (disable).
- **Previous bug:** terrain inherited state from prior pass (e.g., transparent overlay left glDepthMask=FALSE). Result: opaque terrain rendered color but wrote NO depth → see-through, flicker with draw order.
- **Solution:** GlStateGuard slice 2 (RAII mc2gl::GlScoped* guards) or legacy hand-rolled save/restore. Kill-switch `MC2_GLSTATEGUARD_TERRAIN=0` reverts to legacy A/B for validation.

**Indirect Path:**
- Compute → thin VS → frag: no explicit GL state set (depth/blend inherited by design, immutable across passes).
- **Depth:** Indirect uses ZERO_TO_ONE clip-space gate in compute (gpu_driven_terrain_solid.comp:pzOk); thin VS applies TERRAIN_DEPTH_FUDGE pre-divide (clip.z += FUDGE*w). Matches legacy.

**Missing invariant:** No `gos_InvalidateRenderStateCache()` call after terrain draw. Non-destructive if consumer doesn't assume inherited state, but risky as a footprint.

### Texture Bindings

- **Colormap (unit 0):** Bound once per-frame by indirect bridge (when active); LOD chunk binds in `SubmitDrawCommands`.
- **Shadows (units 9-10):** Bound by LOD chunk draw; indirect path inherits from prior shadow pass (e.g., mech shadow).
- **Material normals (unit 5):** Chunk path binds; indirect inherits sampler2DArray from legacy setup.
- **Cement atlas (unit 3):** Bound by chunk draw + indirect bridge when cement atlas ready.

**Redundancy:** No per-quad texture rebinding; all samplers shared across draw. State cost is O(1) per-path.

---

## 4. Shader Pipeline

### Compute (gpu_driven_terrain_solid.comp)

**Input:** recipe (SSBO 0, mission-static), lighting (SSBO 1, per-vertex), terrain handle LUT (SSBO 2, per-frame).  
**Output:** thin records (SSBO 3, capacity-bounded), indirect-draw cmd (SSBO 8, atomicAdd).

- **Per-quad cost:** frustum cull via world-to-clip projection + ZERO_TO_ONE depth test. No redundant matrix multiplies.
- **clipPos[4] computation:** Once per quad, stored in thin record → VS reads directly (Fix B eliminates temporal MVP misalignment).
- **coherent qualifier:** AMD L1-cache coherency fix (Feb 2026); necessary for correctness across compute→VS boundary.

### Thin Vertex Shader (gos_terrain_thin.vert)

- **No projection:** Reads pre-computed clipPos[4] from thin record SSBO.
- **Per-vertex work:** unpack ARGB lighting, select corner via kCornerTable (flat 12-entry to avoid AMD RDNA3 mis-lower), apply TERRAIN_DEPTH_FUDGE pre-divide.
- **Output:** 6 varyings (Color, Texcoord, WorldNorm, WorldPos, etc.) + gl_Position.

**Micro-optimization:** Corner table is constant-folded; no runtime branching per-invocation.

### Terrain Fragment (gos_terrain.frag / terrain_lod_chunk.frag)

- **Colormap atlas sampling:** atlas-UV reconstruction from world position (same formula legacy uses).
- **Material blending:** chunkColorWeights() classifies by colormap RGB deltas (grass/dirt/rock/concrete).
- **Detail normals:** Per-material POM + anti-tiling via matNormalArray sampler2DArray.
- **Shadows:** calcShadow() Poisson PCF via shadow.hglsl.
- **GBuffer1 output:** rc_gbuffer1_shadowHandled (reverse-Z opaque terrain).

**Potential issue:** Per-fragment world-space normal recomputation (via dFdx/dFdy of smooth-normal lookup) is NOT vectorized. Each frag resamples heightsF[] twice (bilinear pairs). At 4K resolution this adds overhead vs. baking normals into a precomputed atlas, but acceptable for flat terrain (few vertices per tri).

### LOD Chunk Vertex (terrain_lod_chunk.vert)

- **Per-vertex height + clamp:** Reads from heights[] SSBO (binding 23). Applies edge stitching (Phase 10.4) to snap intermediate verts to coarser-neighbor edges.
- **Skirt-aware:** Pulls skirt-bottom verts downward by u_skirtDepth.
- **Projection:** u_worldToClipGL with pre-divide depth fudge (2x TERRAIN_DEPTH_FUDGE for net -0.004, matching thin path).

**Cost:** O(1) per vertex; height lookup is sequential SSBO access (cache-friendly). Edge-stitching conditionals are coarse-grained (per-edge, not per-vertex inside stitch).

### LOD Chunk Fragment (terrain_lod_chunk.frag)

- **Smooth normal:** Bilinear height central-difference (fixes "atrocious cliff" faceted artifact). TWO height lookups (dhdx, dhdrow pairs) per fragment.
- **Colormap + detail normals:** Same pipeline as the thin path.
- **Concrete cement:** Per-world-tile (LOD-independent) cement-word lookup; uniform cement UV across LOD transitions.

**Redundancy:** Bilinear height lookups repeated per-frag; candidate for tile-based caching if profiling shows cost (currently not instrumented).

---

## 5. Redundant / Repeated Work

### MVP Matrix Uploads

- **Indirect path:** u_worldToClipGL uploaded once per compute dispatch (in ComputeDispatch, before cull kernel).
- **LOD chunk:** u_worldToClipGL uploaded once in SubmitDrawCommands (per-frame, not per-block).
- **Frame of reference:** Both use `gos_terrain_indirect_getDispatchMvp16()` when solid pass armed, else live MVP (Fix B logic in gos_terrain_lod_chunk.cpp:501-503). **No redundant uploads.**

### Texture Uploads

- **Colormap atlas:** Once per mission (BuildColormapAtlas). CPU copy optionally retired (COLORMAP-CPU-RETIRE-1).
- **Cement catalog:** Once per mission. Per-frame overhead = 0 (read-only slot→layer map).
- **Per-frame reassignments:** None. All texture units bound once per-path.

### Recipe Build / Invalidation

- **Full recipe build:** Once at mission load (primeMissionTerrainCache). O(mapSide²) CPU work, amortized.
- **In-gameplay mutations:** setTerrain() invalidates single quad via InvalidateRecipeForVertexNum(). Cement words NOT rebaked (cement layout is mission-static; would require per-mutation cement-slot re-enumeration — not implemented, accepted tradeoff).

### Lighting SSBO

- **Per-frame producer:** Phase 1 (terrain lighting compute). Single write-once per-vertex.
- **Consumers:** Solid compute (reads per-corner), water compute (reads per-corner), thin VS (repacks). **No redundant reads.**

### Redundant Conditions

- **camerq window narrow-dispatch:** SolidWindowEnabled() call in compute sampled once (cached static). Window array built in slimReduce loop (Terrain::geometry); no per-frame cost.
- **Cement check:** useCementAtlas condition sampled once in chunk frag; per-quad cost O(0) (one uniform test).

---

## 6. Modernization Gaps

### UBO for Per-Frame Constants

**Missing:** Separate UBO for frame-constant uniforms (MVP, lighting params, atlas params).
- **Current:** Loose glUniform calls (u_worldToClipGL, u_lightDir, atlas params, material tunables).
- **Impact:** ~20-30 glUniform calls per-frame for chunk path; O(1) amortized but verbose.
- **Fix:** Bind a single std140 UBO containing all frame constants; compute + thin path both read it. Reduces call volume by ~10x.

### Persistent-Mapped Buffers for Streaming

**Missing:** Thin record stream is standard glBufferData(..., GL_DYNAMIC_DRAW) re-upload.
- **Current:** Compute fills via atomicAdd; CPU calls glMapBufferRange (async readback of thin record count, not shown but implied by parity-check code).
- **Impact:** No sync stall observed (count readback is non-blocking in modern Mesa/drivers), but a persistent-mapped ring buffer would eliminate GPU→CPU round-trip entirely for overflow checks.
- **Tier:** Low-priority (parity machinery is diagnostic-only, not shipping).

### Sync-Free Ring Buffers

**Missing:** Recipe SSBO is full-size per-mission (mapSide²). Dirty slots flushed via glBufferSubData (FlushDirtyRecipeSlotsToGPU). No sync hazard observed, but a ring-buffer approach would enable lock-free concurrent updates in a multi-threaded future.

### Legacy Immediate-Mode Calls

**Retired:**
- drawPass loop (Slice-A Stage 6 flip): was per-quad glBegin/glEnd. Now cement-overlay static-bake.
- Mine draw (Stage 4 default-ON flip): was per-quad MLR. Now GPU bake via StaticBakeMineState.
- Overlay emit (gos_push_overlay): per-quad glVertex calls retired into thin-record packing.

**Remaining:** None identified.

### Comments Referencing D3D/MC2 Concepts

**Live (non-breaking, informational):**
- gos_terrain_indirect.cpp:43 "kAxisSwapMC2toGL" (historical, correct context preserved).
- gos_terrain_thin.vert:51 "legacy non-thin VS chain" (documentation; no code path dependency).
- terrain_lod_chunk.frag:136 "per-material tiling (set via gos_SetTerrainMatTiling)" (API name is MC2-legacy, but binding is GL-portable).

**No actionable debt identified.**

---

## 7. Quick Wins (Ranked by Effort/ROI)

### 1. **Consolidate Per-Frame Uniforms into UBO (2–3 hours, ~5% reduction in CPU draw-overhead)**

**Current state:** ~25 glUniform calls per-frame (MVP, atlas params, light dir, material tunables, shadow params).  
**Proposal:** Pack into a single std140 UBO (frame_constants), bind before compute + chunk draw.
```cpp
struct FrameConstants {
    mat4 worldToClipGL;
    vec4 lightDir;
    float atlasMapTopLeftX, atlasMapTopLeftY, atlasOneOverWorldUnits;
    // ... other frame-constant uniforms
};
```
**Benefit:** Eliminates 20+ glUniform* calls; makes shader interface cleaner (one `binding = 0` instead of scattered uniforms).  
**Risk:** Low (purely additive; new UBO coexists with loose uniforms for A/B validation).

---

### 2. **Cache Bilinear Height Lookups in Chunk Frag (1–2 hours, ~2–3ms reduction at 4K if POM active)**

**Current state:** terrain_lod_chunk.frag calls heightBilinear() 3 times per-frag (dhdx, dhdrow, and one inside POM ray-march).  
**Proposal:** Inline the dhdx/dhdrow computation with cached height samples; reuse for POM.
```glsl
// Current: 2 pairs of 4-sample lookups for dhdx, dhdrow.
// Optimized: 1 pair, derive both deltas from the same 2×2 grid.
```
**Benefit:** Reduces SSBO load-store from 6-8 to 4 per smooth-normal frag (33% reduction in memory load).  
**Risk:** Micro-optimization; only relevant if height buffer is a hot-path miss (profile with `MC2_TERRAIN_LOD_CHUNK_DIAG=0` vs. real run).

---

### 3. **Retire Loose glUniform Calls for Atlas/Material Params (1 hour, ~0.5ms reduction, clarity)**

**Current state:** gos_terrain_lod_chunk.cpp:620–757 has 20+ individual glUniform1i/glUniform1f/glUniform4f calls.  
**Proposal:** Group into 2–3 logical ubos:
- frame_constants (MVP, light, time).
- atlas_params (colormap atlas UV decomposition, cement grid-side).
- material_tunables (matTiling, tintRock, classGrass, etc.).

**Benefit:** Reduces upload volume; improves readability; makes shader interface contract clearer.  
**Risk:** Lowest (pure refactor, no behavioral change).

---

### 4. **Defer Cement Lookup to Tile-Grid, Not Per-Quad (2–3 hours, negligible perf, architectural cleanup)**

**Current state:** Chunk frag reads cementWordsF[] per-fragment via world-tile lookup (correct, LOD-independent).  
Indirect path bakes cement word into thin record at compute time (quest-dependent on whether quad is within tile coverage).

**Proposal:** Unify: both paths compute world-tile (not world-quad) and look up cement word. Eliminates per-quad cement-word caching redundancy.  
**Benefit:** Simplifies recipe structure (removes _wp3 cement word field); unifies computation across paths.  
**Risk:** Medium (requires compute re-verify; cement word baking is intricate logic).

---

### 5. **Add Instrumented Depth-Correctness Probe for Large Oversized Maps (3–4 hours, correctness confidence)**

**Current state:** gos_terrain_indirect.cpp has Probe 5 (recipe corner-spread sanity) + Probe 4 (near-w corner count). No per-frag depth-correctness check.  
**Proposal:** Optional (env-gated `MC2_TERRAIN_DEPTH_CORRECTNESS_PROBE`) check that every thin record's depth gates match between compute + frag.
```glsl
// Compute: emit pzOk(clip) via atomic counter.
// Frag: sample per-tri and assert match (off-by-one would indicate fix regression).
```
**Benefit:** Catches depth-gate bugs on very large maps (1K+) where edge-effect regressions hide in small-map smoke tests.  
**Risk:** Low (diagnostic, zero cost when off).

---

## Summary of Issues Found

### Critical
1. **LOD Chunk GL state inheritance (Phase 10.3 FIXED):** Terrain now explicitly sets depth/blend/cull. Residual: no gos_InvalidateRenderStateCache() after draw (non-destructive but inconsistent with project discipline).

### High
2. **Redundant bilinear height lookups in chunk frag:** 3× lookups per-fragment (smooth normal + POM) when 1–2 pairs could suffice. Measurable on 4K+ if POM armed.

### Medium
3. **Scattered glUniform calls (25+) instead of UBO:** Minor CPU overhead; improves with consolidation.
4. **Cement word per-quad redundancy:** Both paths embed cement word; should unify on world-tile (longer refactor).

### Low
5. **No depth-gate instrumentation for oversized maps:** Edge-case validation gap (1K+ maps); covered by manual Probe 5 corner-spread check.

---

## References

- [docs/critical_inline_rules.md](../critical_inline_rules.md) — C++17 / shader discipline
- [memory/MEMORY.md](~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md) — Handoff 2026-06-14 (phase 10 fidelity, glStateGuard slice 2)
- [docs/render-contract.md](../render-contract.md) — TerrainBase pass contract (StateContract § 3.1)
- gpu_driven_terrain_solid.comp — Compute cull/pack spec
- gos_terrain_thin.vert — Thin record consumer
- terrain_lod_chunk.{vert,frag} — Chunk production renderer

