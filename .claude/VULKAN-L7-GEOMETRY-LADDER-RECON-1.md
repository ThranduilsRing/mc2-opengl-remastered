# VULKAN-L7-GEOMETRY-LADDER-RECON-1 — sequenced plan for L7.1–L7.6

Read-only recon (2026-07-02), worktree `A:/Games/mc2-vulkan-backend` @ `ab8cfc7b`, base
`c18618c4` (terrain-v2 day-1 merged). Companion to `VULKAN-BACKEND-L6-LADDER-1.md` +
`VULKAN-NEXT-SLICES.md`. The L6 postprocess family (fog/ssao/hzb) + L6.6 present are the
*cheap tenth*; L7 is the geometry mountain (~80% of a frame's passes + all descriptor/SSBO
complexity).

## Verified premises
- **L6.1 `backendHandle` rename DONE** — `RenderCore/GpuBufferOwner.h:43` `uint64_t backendHandle`. No rename work remains for L7.
- **H2 slot-2 IS a true SAME-pipeline collision** and is **NOT tracked** by `check-vulkan-bindings.py` (which curates only slot-14/20). L7.1 must add it.

## H2 — the three binding collisions
- **slot-2 (TRUE same-pipeline, must split):** VS `LightingBuf@2` (`shaders/gos_terrain_mask_solid.vert:33`, `gos_terrain_mask_water.vert:41`) vs FS `ThinRecordBufFrag@2` (`shaders/gos_terrain.frag:114`) — both linked in `mask_solid_prog_` (`gameos_graphics.cpp:2409`, built `:5307-5308`). Benign in GL, hard blocker for a Vk descriptor set.
- **slot-14 (disjoint-pass, GL-mitigated, Vk-renumber):** `READBACK_SSBO_BINDING=14` (`gpu_cull_readback.h:18`) vs particle `binding=14` (`gos_particle_bridge.cpp:1181`).
- **slot-20 (disjoint-pass, benign, defer):** `LIGHT_DATA_SSBO_BINDING=20` (`gameos.hpp:2888`) vs `SurfaceVertexBuf@20` (`gos_terrain_surface.vert:60`).

## Region → buffers that must be OWNED+registered first (JIT per rung)
- **L7.2 Shadow:** mech `s_instanceSsbo`/`s_boneSsbo`, prop `Instances`, `DynamicPropShadowSsbo`(id36). Rings are raw file-static globals today.
- **L7.3 Terrain:** height(id14)/type(id26)/cement(id27), recipe(id10)/thin(id11), TL-compute in/out(id31/32/33), indirect cmd. ★UPDATE 2026-07-02 (verified twice): ownership inversion is ALREADY CLOSED — all LIVE LOD-chunk buffers (14/26/27/30) + TL-compute (31/32/33) + ViewUniforms(25) are registered-on-create; DEAD indirect buffers (10/11) also registered. No ownership prep remains for terrain. Live solid renderer = gos_terrain_lod_chunk.cpp (glDrawElements, default-ON); gos_terrain_indirect.cpp (MDI) is DEAD (reference only). First L7.3 slice = descriptor-table authoring, NOT ownership.
- **L7.4 Water:** water thin(SSBO3 fenced), water recipe(SSBO5), reflection RT(id7/8 External).
- **L7.5 SP+Mech:** ★cheapest win = `ViewUniformsUbo`(id25, 1 site); then material tables (id5/28/29), instance/color/base rings, LightData(id34).

## Sequenced plan (JIT prep folded in)
```
L7.1 VK-H2-BINDING-SPLIT   [GL-side, byte-identical, INDEPENDENT of L6.6] — START NOW-ABLE
L7.2 VK-REGION-SHADOW      [needs L7.2 descriptor-table recon first]
L7.3 VK-REGION-TERRAIN     [gated by L7.1 slot-2 split; terrain-v2 fence]
L7.4 VK-REGION-WATER       [depends on L7.3]
L7.5 VK-REGION-SP-MECH     [MRT; depth-EQUAL order-preserve, NOT barrierable]
L7.6 VK-DESCRIPTOR-CONSOLIDATION  [after 7.2-7.5 live]
```
Dependency edges: L7.1→L7.3→L7.4; L7.2 (Shadow) independent of L7.1, parallelizable; L7.6 needs all. **L7.1 does NOT depend on L6.6.**

## L7.1 fully specified (startable now, GL-side byte-identical)
Split slot-2: move `LightingBuf` off binding=2 in the two mask VS (fewer link sites than moving the shared frag block); update the matching `glBindBufferBase`/`glShaderStorageBlockBinding` in lockstep; extend `check-binding-slots.py` + `check-vulkan-bindings.py` (add slot-2, `--strict` clean). Optional/deferred: slot-14 particle + slot-20 SurfaceVertexBuf renumbers.
- **Proof:** two GL captures pre/post identical `pixel_sha` (plain-GL golden); tier1 5/5; `build64` byte-identical + shader-deploy lockstep.
- **Preflight (terrain-v2 STOP-gate):** `repo_query.py slice-preflight --slice VK-H2-BINDING-SPLIT-1 --symbols "mask_solid_prog_,ThinRecordBufFrag,LightingBuf,SurfaceVertexBuf" --paths shaders/gos_terrain.frag shaders/gos_terrain_mask_solid.vert shaders/gos_terrain_mask_water.vert shaders/gos_terrain_surface.vert GameOS/gameos/gameos_graphics.cpp --base c18618c4`. Rebase to latest terrain-v2 immediately before coding. Size M / Risk M.

## Barrier IDs per rung (from VULKAN-BARRIER-PLAN-RECON-1)
L7.2 Shadow: B1-B4 (Shadow→SP/Mech/Terrain/Veg, DEPTH_WRITE→SHADER_READ, compare). L7.3 Terrain: B7 (MainDepth, ContentConditional) + compute→draw SSBO barrier (TL id33 write→read) + compute→indirect. L7.4 Water: B7 + reflection External. L7.5 SP+Mech: B5,B6 (MainDepth), B8/B9/B10 (MainColor/MainNormal/SceneObjectId, last ContentConditional). L7.6: none (validation-clean proof).

## Risks (top)
1. **terrain-v2 contention (HIGHEST)** — L7.1/L7.3 edit hot terrain shaders under ~15 concurrent lane worktrees. Rebase-immediately-before + slice-preflight STOP-gate + `check-binding-slots.py` as drift tripwire.
2. **Descriptor explosion** — each geometry family gets ONE convention table specced at its recon (§5a postprocess table does NOT stretch over geometry). 4-set model (global/pass/material/compute-prepass) at L7.6.
3. **depth-EQUAL order hazard (L7.5)** — StaticProp↔Mech, NOT barrierable (reorder candidate was REJECTED, ORACLE-STRENGTHEN-1). Bake pass order into cmd-buffer + assert non-reorder.
4. **Readback-bridge retirement** — production geometry must NOT ship a CPU readback; convert regions in contiguous blocks from the frame boundary inward; shared-image interop deferred to L8.1.
5. **compare-sampler + CSM 2D_ARRAY (L7.2 novelty)** — spec at the L7.2 recon before code; new compare-sampler fixture.
6. **LM2 mid-frame compute** (`ComputeDispatchAndBindThinRecords` `gameos_graphics.cpp:3362`) — reproduce dispatch→draw barrier, don't float draws across it.

## Effort ranking (cheapest→most)
1. L7.1 slot-split (M/M, now) · 2. L7.2 Shadow (L/H) · 3. L7.4 Water (L/H) · 4. L7.3 Terrain (L/H) · 5. L7.5 SP+Mech (L/H) · 6. L7.6 consolidation (L/H).
