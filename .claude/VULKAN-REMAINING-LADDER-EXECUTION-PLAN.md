# VULKAN REMAINING-LADDER EXECUTION PLAN

**Authoritative execution plan for the remainder of the MechCommander 2 Vulkan-backend arc.**
Synthesis of five parallel recon outputs (L7.3 terrain, L7.4 water, L7.5 static-prop+mech, L7.6 descriptor consolidation, L8.1 endgame). Worktree `A:/Games/mc2-vulkan-backend` @ `95095a94` (branch `claude/vulkan-backend-l6`). All file:line cites carried verbatim from the recons that verified them.

> **Recon gap on record:** the **L8.0-ui-2d** recon FAILED (agent returned nothing). The UI/2D region (`UIOverlay` in `kFramePassOrder`) therefore has **no verified port plan**. It is carried below as a **known-unstarted region requiring its own recon** (see §2 slice U0 and §6). Do not schedule UI port work until that recon lands.

---

## 1. Current state

**Done (the ALREADY-DONE spine):** The vehicle + proof machinery of the arc is complete and local-only (branch not pushed, all default-OFF, `build64` GL-byte-identical). L1 vehicle (VMA/volk/validation-presets/pipeline-cache/swapchain-probe), L2 proof harness (golden-parity + synthetic fixtures), L3 two pixel-parity islands (EdgeFog + OOB-fog) + hardened template, L4 native subgraph (fog passes fused into one render pass, Vulkan-owned intermediate), L5 swapchain-present (engine owns a **separate** Vulkan window, presents+resizes, validation-clean). Proof machinery has already caught 5 real bugs (row/col-major, cross-target ODR, ImGui-capture, framecount-false-fail, present-semaphore-reuse). **Ported regions today** = postprocess family (Fog subgraph, SSAO island, HZB island) — all as **GL→VK→GL CPU-readback oracles**, each owning its own command buffer + fence + descriptor pool. L6.1 done (`GpuBufferOwner.backendHandle` is 64-bit). Compile-enforced invariants live: Subgraph Contiguity Rule, `check-vulkan-bindings.py` GL-slot classifier + allowlist (slot-2 split by L7.1, slot-14/20 classified benign-disjoint). **In flight (parent-agent branches, NOT on `95095a94`):** VK-SHADOW-CONSUME-C (wires the L7.2 shadow producer to a live consumer — currently fixture-only), WATER-MVP-ORACLE-1 (rules the water depth-fudge/reverse-Z convention), and WATER-REGISTRY-0 (owns+registers water buffer ids 43–46 — **not yet in `95095a94` history**; a hard predecessor to all water work). **Remaining ladder:** L7.3 Terrain-solid, L7.4 Water (3 sub-slices), L7.5 StaticProp+Mech (4 sub-slices), L7.6 descriptor consolidation, then the endgame — which the recon proved is **not one L8.1 slice but four orthogonal capabilities** (interop, fusion, barrier-DAG, present) plus five still-unported regions.

---

## 2. The complete remaining slice sequence

Ordered. Each slice: **id/name · goal · size · risk · key files · proof · hard deps · fan-out?**

### L7.3 — TERRAIN SOLID

**T0 · VK-REGION-TERRAIN-SOLID-CONTRACT**
- Goal: add `RenderRegionId::TerrainSolid` + neutral `TerrainSolidRegionContext` + `runRegion` overload + resolver mappings (add colormap-atlas `RenderResourceId` if missing); extract `runTerrainSolidRegionGL()` as a pure code-move.
- Size **S** · Risk **L**
- Files: `RenderCore/RenderRegionId.h`, `GameOS/gameos/gos_terrain_lod_chunk.cpp:782` (entry), `IRenderBackend`.
- Proof: `build64` byte-identical; GL-vs-GL byte-identical.
- Deps: none. **NOT gated on L7.1** (correction below) or L7.2.
- Fan-out: **yes** (isolated worktree; enum-serialization caveat §5).

**T1 · TERRAIN-UBO-GATHER (GL-side precondition)**
- Goal: gather the loose `glUniform*` uploads (`:944-1351`) into ViewParams+TerrainParams UBOs, convert per-block `glUniform1i` → push constants, add explicit `layout(set=,binding=)`. Still pure GL.
- Size **M** · Risk **M** (terrain-v2 contention — this edits the hot uniform block `:1206-1351`).
- Files: `gos_terrain_lod_chunk.cpp`, `shaders/terrain_lod_chunk.{vert,frag}`.
- Proof: two GL captures pre/post identical `pixel_sha` (plain golden).
- Deps: T0. **slice-preflight STOP-gate mandatory** immediately before (§5).
- Fan-out: no (hot shared shaders; keep on one committer).

**T2 · TERRAIN-SHADER-BAKE**
- Goal: two-target bake of `terrain_lod_chunk.{vert,frag}` (opengl + vulkan1.1, no auto-map-bindings); extend `check-vulkan-bindings.py` for the geometry-family table.
- Size **S** · Risk **L**
- Files: `shaders/terrain_lod_chunk.{vert,frag}` → `<build>/shaders/vulkan/`, `scripts/check-vulkan-bindings.py`.
- Proof: GL output byte-identical GL-vs-baked-spv.
- Deps: T1.
- Fan-out: no (same shader files as T1).

**T3 · TERRAIN-FIXTURE**
- Goal: synthetic heightfield fixture (17×17 ramp + 1 cement tile), **sheared** `worldToClipGL` (LM1 detector), shadow compare-sampler + CSM 2D_ARRAY reuse; probe token `terrain_solid_fixture`.
- Size **M** · Risk **M**
- Files: `vulkan_backend_skeleton.cpp`, `run_vulkan_probe.py`.
- Proof: fixture byte-exact, validation sync 0.
- Deps: T2.
- Fan-out: **yes** (fixture TU is isolated).

**T4 · VK-REGION-TERRAIN-SOLID-ISLAND**
- Goal: `vulkan_terrain_solid_island.cpp` — 4-set layout, main-patch + skirt-strip draw port, guard-decline on `u_visualDisplace!=0` / missing buffers (SSAO enable-guard precedent).
- Size **L** · Risk **H**
- Files: new `vulkan_terrain_solid_island.cpp`; consumes T0 context.
- Proof: validation 0; island runs; guard-decline verified.
- Deps: T3.
- Fan-out: **yes** (new island TU).

**T5 · TERRAIN-BACKEND-COMPARE**
- Goal: backend-compare on terrain bookmarks (mc2_01 overview, mc2_24 ridge) + REGION_SPEC + health block + fallback proof + tier1 5/5.
- Size **M** · Risk **M**
- Files: `run_golden_parity.py` REGION_SPEC, health block.
- Proof: compare exit 0 non-fallback (LM8-bracketed); fallback→GL proven; tier1 5/5; `build64` byte-identical.
- Deps: T4.
- Fan-out: no (integration/proof gate).

> **★ L7.3 correction (carried from recon, load-bearing for the whole ladder):** the LIVE terrain region is the **LOD-chunk** path (`terrain_lod_chunk.{vert,frag}`), NOT `gos_terrain.frag`/the mask VS-FS/the indirect path. Consequences: **(a)** the slot-2 `LightingBuf@2` vs `ThinRecordBufFrag@2` collision (H2/L7.1) is **terrain-indirect-path-only** and does **NOT** block L7.3 — L7.1 is decoupled from the live terrain region. **(b)** The LM2 terrain-lighting compute (`gos_terrain_lighting.cpp:897`) output is consumed **on the CPU** (readback into `lightRGB` vertex pool), NOT via an SSBO the chunk draw reads — so **there is no compute→draw barrier in this region** (set3 is empty). This removes the two hardest pieces the recon originally feared; L7.3 is a large-but-mechanical opaque geometry region. **(c)** The 5× `matNormal0..4` "unenumerated samplers" are already collapsed to one `sampler2DArray matNormalArray` in the live frag (`terrain_lod_chunk.frag:182`) — no bindless descriptor-array needed.

> **LM1 for terrain = `column_major`** (uploads `GL_FALSE`, no pre-transpose) — the **OPPOSITE** of fog (row_major, double-transpose GL_TRUE) and of the SSAO fix. A copy-paste of the SSAO UBO qualifier silently transposes. The sheared fixture (T3) is the guard.

---

### L7.4 — WATER (3 sub-slices, strictly linear)

> **Precondition:** WATER-REGISTRY-0 (ids 43–46 owned+registered) is **NOT on `95095a94`** — confirm via slice-preflight before ANY water coding; if absent it is a hard predecessor. L7.4 as a whole is **gated behind L7.3** (water depth-tests against `MainDepth` written by terrain — barrier B7).

**W-A · WATER-COMPUTE-CHAIN-1**
- Goal: port the 2 authoritative dispatches — `gpu_driven_water.comp` (cull/pack) + `gpu_driven_cmd_patch.comp` (cmd-patch). Retire the GL slot-6 header/thin multiplex (LM2) by giving header + thin **distinct** `(set,binding)` in the compute pre-pass set (set3).
- Size **M** · Risk **M** (LM2 mid-frame compute is the top risk).
- Files: `gos_terrain_water_stream.cpp:1489-1503,1792,1928-1941,1950-1959`; `gpu_driven_water.comp`, `gpu_driven_cmd_patch.comp`.
- Proof: bit-exact fixture (compute output is integer/packed → byte-exact achievable); reuse `MC2_GPU_DRIVEN_PARITY` (`:1977`) as GL truth; thin-record bytes + indirect-cmd `.count`/`.first` bit-identical; validation sync 0. **Do NOT port the `MC2_WATER_FULL_RECIPE_SPIKE` scratch dispatch (`:1810-1922`)** — dead measurement harness, TIER2-EXCLUDED.
- Deps: L7.3 (TL-output id33 compute→compute ordering), WATER-REGISTRY-0.
- Fan-out: **yes** (compute-fixture TU isolated).

**W-B · WATER-MDI-DRAW-1**
- Goal: port `glMultiDrawArraysIndirect` (`gameos_graphics.cpp:3706`) → `vkCmdDrawIndirect` (bit-compatible cmd layout). Own geometry-family descriptor table; split slot-5 recipe vs static-prop-material into distinct `(set,binding)`. In-VS depth-fudge (`WATER_DEPTH_FUDGE_FAST=0.00025`, reverse-Z GEQUAL).
- Size **L** · Risk **H**
- Files: `gameos_graphics.cpp:3658-3660,3705-3706`; `gos_terrain_water_fast_mdi.vert:284-286`, `gos_terrain_water_mdi.frag`, `terrain_depth_bias.hglsl:65`.
- Proof: backend-compare `--region Water` — **byte-exact WILL FAIL** (heavy FBM `h21`/`vnoise`/`fbm3` `:95-109`) → declare a **§5b per-region `tolerance_floor`** (≤1 LSB / ≤0.1% px), **supervisor sign-off required — do NOT self-weaken**. Split proof: geometry/depth via `debugMode` bookmarks = byte-exact; shading = floor. Validation clean, tier1 5/5, `build64` byte-identical.
- Deps: **W-A** (reads thin + indirect cmd it produces); **WATER-MVP-ORACLE-1** (depth-fudge convention — do NOT hardcode the fudge math until it rules).
- Fan-out: no (depends on W-A output + oracle ruling).

**W-C · WATER-REFLECTION-EXTERNAL-1**
- Goal: cross-frame reflection RT (ids 7/8, `External` lifetime). Producer (`gos_terrain_indirect.cpp:3861`) re-runs the terrain-solid path with a mirror MVP; consumer samples next frame. One produce-side barrier (COLOR_ATTACHMENT→SHADER_READ); the read is `ExternalNonEdge` (no in-frame edge).
- Size **L** · Risk **H**
- Files: `gos_terrain_indirect.cpp:3861,3899-3957`; consumer `gameos_graphics.cpp:3610-3622`; `RenderResourceRegistry.h:18-19`.
- Proof: `mirroredCmdCount` readback reuse; consumer parity folds into W-B backend-compare with `MC2_WATER_REFLECTION_RT=1`; assert resolver returns a **stable persistent VkImage** across frames (External ≠ frame-recycled); gate-OFF byte-identical.
- Deps: **L7.3** (re-enters the terrain-solid Vk path), **W-B**.
- Fan-out: no (structurally coupled to terrain-solid + W-B).

> **★ CORRECTION (ORACLE-PROVEN 2026-07-02, `4d30d93e`): water depth-fudge MVP `u_worldToClipGL` = `column_major`** (std140 default, GL_FALSE raw pack, NO transpose) — NOT `row_major` as first reasoned here. WATER-MVP-ORACLE-1's asymmetric-matrix fixture proved wrong-convention diverges by 69.5 (column_major = max_abs_diff 0). Same rule as edge_fog/ssao. This is why the convention oracle runs BEFORE the port. MVP snapshot save/restore in W-C stays load-bearing — fail to restore `g_dispatchMvp16` and the water fast path draws with the mirror matrix and vanishes.

---

### L7.5 — STATIC-PROP + MECH (4 sub-slices)

> **Signature risk:** this is the **only** region whose defining hazard is not a barrier but the **depth-EQUAL last-writer-wins commutativity** invariant. StaticProp runs depth-prepass→GL_EQUAL color; Mech runs GL_GEQUAL. At a depth-tie the MRT output is order-dependent. This is the **REJECTED** `MC2_FRAMEGRAPH_REORDER_SPMECH` candidate (0.10–0.32% pixel diff on mc2_24). A `VkPipelineBarrier` cannot fix it. **Gated on L7.2 Shadow being live** (inbound compare-sampler set0/binding2). Touches **no** terrain shaders → no terrain-v2 fence needed.

**M0 · SP-MECH-REGION-CONTRACT-1**
- Goal: `RenderRegionId::StaticPropMechOpaque` + neutral context (per-family instance/companion/material ids, `objectIdEnabled` bool, MRT dims, ring slot/cap/stride, viewUniforms) + `runRegion` overload; extract `runSpMechRegionGL()` pure code-move around `txmmgr.cpp:3265-3315`, **preserving the order-preserve block verbatim**.
- Size **S** · Risk **L**
- Files: `RenderCore/RenderRegionId.h`, `mclib/txmmgr.cpp:3259-3318`.
- Proof: GL-vs-GL byte-identical; `build64` byte-identical.
- Deps: none structurally, but sequence after L7.2 live.
- Fan-out: **yes**.

**M1 · SP-MECH-DESCRIPTOR-TABLE-RECON**
- Goal: author the 4-set geometry-family table (§normative below) + extend `check-vulkan-bindings.py`. (Do NOT stretch the postprocess §5a table.)
- Size **S** · Risk **L** (recon/doc + checker)
- Files: normative doc, `scripts/check-vulkan-bindings.py`.
- Proof: checker self-test GREEN.
- Deps: M0.
- Fan-out: **yes** (doc/tooling).

**M2 · VK-REGION-SP-MECH-STATICPROP**
- Goal: rigid static-prop MRT pipeline (2-and-3-attachment variants keyed off `objectIdEnabled`), set0/set1(instance+color)/set2, `vulkan_sp_mech_island.cpp`. Reproduce depth-prepass→GL_EQUAL (load-bearing `:5293-5341` — do NOT collapse to single GEQUAL).
- Size **L** · Risk **H**
- Files: new `vulkan_sp_mech_island.cpp`; `gos_static_prop_batcher.cpp:851-857,5293-5341,7239`; `gos_postprocess.cpp:222-261` (MRT 2↔3 flip); `static_prop.frag:148-153`.
- Proof: synthetic MRT fixture (sheared viewProj, 3-attachment) + backend-compare (prop-only forced).
- Deps: M1.
- Fan-out: **yes** (island TU).

**M3 · VK-REGION-SP-MECH-MECH**
- Goal: add skinned mech pipeline variant (bone ring id40, PBR samplers), record **SP then Mech in ONE cmd buffer** with a non-reorder assert mirroring `SAME-ORDER-EXECUTOR-SLICE-2` (`txmmgr.cpp:2478`); emit B8/B9/B10.
- Size **L** · Risk **H** (depth-EQUAL)
- Files: `gos_mech_batcher.cpp:1900,2022-2036,2226-2300`; `mech.frag:119-129`; `vulkan_shadow_island.cpp` bone-ring pattern.
- Proof: full backend-compare **including depth-tie bookmarks (mc2_24 coplanar, mc2_03/mc2_17 SP+Mech overlap)** — a 0.1–0.3% diff = a REORDER symptom and must be **zero** (order preserved), distinct from any transcendental ±1-LSB floor. Non-reorder assert active + REORDER gate confirmed default-OFF; validation sync-clean; fallback→GL; tier1 5/5.
- Deps: M2.
- Fan-out: no (final integration + order assertion).

---

### L7.6 — DESCRIPTOR CONSOLIDATION (cross-cutting, LAST rung of L7)

> **Hard gate: after L7.2–L7.5 are ALL live.** The 4-set "set0 bound once, reused by all" value is unprovable until ≥2 geometry regions share ViewUniforms/LightData/shadow globals in one Vulkan cmd buffer. **Not startable now.** It is a **refactor with ZERO pixel change**; its proof is invariance of every existing proof.

**D0 (optional early carve-out) · VK-BINDING-CONVENTION-DOC + CHECKER**
- Goal: write the normative per-family set/binding convention doc (geometry 4-set + postprocess §5a + the seam) + a **Vulkan-side** set/binding conformance checker over `shaders/vulkan/*` (today `check-vulkan-bindings.py` reasons over GL occupancy ONLY). Pure additive tooling/docs — can land NOW so every geometry region is authored to the table from day one.
- Size **S** · Risk **L**
- Files: new doc, `scripts/check-vulkan-bindings.py` (Vulkan conformance mode). Reference: `vulkan_shadow_island.cpp:56-95` already conforms.
- Proof: checker `--self-test` GREEN, `--strict` over `shaders/vulkan/*`.
- Deps: none. **Recommend to supervisor as a prep slice** — de-risks L7.6 from L/H to "wire shared layouts + prove invariance."
- Fan-out: **yes**.

**D1 · VK-DESCRIPTOR-CONSOLIDATION**
- Goal: introduce shared descriptor infra — ONE process-lifetime `VkDescriptorSetLayout` **handle** per set-tier (not per-region copies), one shared `VkDescriptorPool` sized for all regions × 3 frames, a set0 write-once-per-frame path; re-home every live region's resources into the correct set. Retire each island's private single-set layout + private pool.
- Size **L** · Risk **H**
- Files: every live island TU (`vulkan_ssao_island.cpp:761-779`, `vulkan_postprocess_subgraph.cpp:647-674`, `vulkan_hzb_island.cpp`, all L7.3-5 islands); shared-infra TU.
- Proof: **exhaustive all-regions parity re-run** (identical pixels required — byte-exact where it was, within §5b floor for SSAO/Water); sync-validation clean (descriptor validation is the rung most likely to trip — mismatched set index, pool exhaustion, layout-vs-shader mismatch); new checker GREEN; tier1 5/5; `build64` byte-identical.
- Deps: **L7.2–L7.5 all live**; D0 (if taken).
- Fan-out: no (touches every region TU — single committer).

> L7.6 **preserves** the benign-disjoint GL-slot classification: slot-14 (cull/particle/tube) and slot-20 (SurfaceVertexBuf/LightData) need **no split** — they land in disjoint pipeline layouts naturally. The new checker must assert disjoint roles stay in disjoint layouts (never co-name both roles in one shared set).

---

### L8.0 — ENDGAME PREP (the four capabilities the region ladder never builds)

> **The recon's central finding:** porting all 11 regions through `runRegion` does **NOT** yield a Vulkan frame. Every ported region is a GL→VK→GL **CPU-readback oracle** with its own cmd buffer + fence. `GL_EXT_memory_object` appears in **zero** engine TUs. No frame-level cmd-buffer orchestration exists. L6.6 present targets a **separate** window. Five regions are unported. The endgame = 4 orthogonal gaps (G1 interop → G2 fusion → G3 barrier-DAG → G4 present), head-to-tail gated on one unproven capability.

**U0 · VK-REGION-UI-RECON  ⚠ (fills the FAILED recon)**
- Goal: recon the `UIOverlay` / 2D region port (the L8.0-ui-2d recon returned nothing). Identify draw path, ImGui vs game-UI split, blend/ordering (barrier B16 = `SoftState` present-ordering), descriptor needs.
- Size **S** (recon) · Risk **M** (unknown scope)
- Files: TBD by recon (UI draw path unmapped).
- Proof: builder-ready plan produced.
- Deps: none (can run anytime).
- Fan-out: **yes** (read-only recon).

**U1 · PORT 5 UNSTARTED REGIONS**
- Goal: TerrainOverlay, TerrainDecal, VegetationCards, VFX, and UIOverlay each get a live (non-fixture) `runRegion`. Plus wire the Shadow **consumer** (VK-SHADOW-CONSUME-C) — Shadow is producer-only/fixture today.
- Size **L** (5 regions) · Risk **M–H** per region
- Files: per-region (VFX copies B11/B12 are the trickiest — 3-layout transfer chains).
- Proof: per-region backend-compare (the L7 discipline).
- Deps: U0 (for UI); the same geometry-family descriptor table (D0).
- Fan-out: **yes** — these 5 are mutually independent, ideal parallel worktrees.

**L8.0-probe · VK-INTEROP-PROBE-1  ★ (highest-risk unknown in the whole arc)**
- Goal: confirm `GL_EXT_memory_object_win32` + `VK_KHR_external_memory_win32` round-trip a D32_SFLOAT depth + RGBA16F color shared image **zero-copy** on the 7900 XTX GL+VK combo, with a signaled cross-API semaphore. Headless.
- Size **M** · Risk **H**
- Files: new probe harness, `run_vulkan_probe.py` token `interop_shared_image`.
- Proof: token GREEN, sync-clean, zero-copy verified (no `glGetTexImage`/`glTexSubImage`).
- Deps: none. **The entire endgame is gated behind this.** If it fails on the target driver (format/tiling mismatch — DEPTH24 vs D32, linear vs optimal is the classic failure), the readback bridge is **permanent** and "full-frame Vulkan" is a legitimate ledgered dead-end — say so honestly.
- Fan-out: **yes** (isolated harness).

**L8.0-interop · VK-SHARED-IMAGE-BRIDGE-1**
- Goal: retire the CPU readback for ONE region (fog) using shared images + GL↔VK semaphore (mixed era = GL owns allocation, option A). Then roll across all ported regions (interop-N).
- Size **L** · Risk **H** (first) / **M** (roll-out)
- Files: fog island first, then all; FBO images (id1/2/15/6/19) become externally-shared `VkImage`.
- Proof: backend-compare byte-identical to the readback path; **no** `glGetTexImage`/`glTexSubImage` in the vk route; sync-clean semaphore.
- Deps: **L8.0-probe** (must pass first).
- Fan-out: partial (first region single-committer; roll-out fan-outs per region).

**L8.0-fuse · VK-FRAME-EXECUTOR-1**
- Goal: thread ONE frame `VkCommandBuffer` through `runRegion` (signature change: region records into a shared buffer, does not own one); a frame executor walks `kFramePassOrder`, one submit + one fence per frame-in-flight; frame-scoped descriptor sets (the D1 4-set model — set0 bound once).
- Size **L** · Risk **H** (biggest structural change to the region seam since it was built)
- Files: `IRenderBackend`/resolver seam, every island TU, new frame-executor TU. Pass order `RenderPassContract.h:398-412`.
- Proof: 2+ regions in one cmd buffer, parity + sync-clean; per-region island submit/fence retired.
- Deps: **L8.0-interop-N** (can't fuse CPU-readback handoffs) + **L7.6/D1** (stable set model to bind).
- Fan-out: no (touches the shared seam — single committer).

**L8.0-barriers · VK-BARRIER-DAG-1**
- Goal: emit B1–B17 + the 4 hand-added classes (intra-pass ping-pongs B13/B14, compute→compute B15, transfer sub-steps B11/B12, SSBO compute→draw incl. terrain-lighting id33 + LM2 `ComputeDispatchAndBindThinRecords` `gameos_graphics.cpp:3362`) inside the fused buffer. Bake the SP↔Mech order + non-reorder assert.
- Size **M** · Risk **M** (the DAG is fully recon'd — transcription, not discovery)
- Files: frame-executor; `VULKAN-BARRIER-PLAN-RECON-1.md` (B1–B17); `scheduler_legal_reorder.h`, `RenderPassContract.h`.
- Proof: sync-validation 0 on the fused frame; per-region parity still holds; full-frame parity within-floor.
- Deps: **L8.0-fuse**.
- Fan-out: no (single fused buffer).

**L8.0-present · VK-GAMEWINDOW-PRESENT-1  ⚠ SUPERVISED**
- Goal: GL relinquishes the game window; VK creates the surface on it; blit the fused MainColor (shared image, option B at the present edge) → `vkQueuePresentKHR` (B17). Reuse the L6.6-lite acquire/present/resize/per-image-semaphore machinery (present-semaphore-reuse bug already fixed — reuse verbatim).
- Size **L** · Risk **H** (changes who owns the game window — highest blast radius)
- Files: `vulkan_present_object.cpp:11` (the separate-window wall), swapchain/surface path.
- Proof: present N frames sync-clean, resize survives, GL fallback byte-identical.
- Deps: **L8.0-fuse + L8.0-barriers**. **Do not attempt unsupervised** (matches L6.6 ruling).
- Fan-out: no (present ownership surgery — single committer, supervised).

---

### L8.1 — DEFAULT FLIP / FULL FRAME (the real endgame slice)

**L8.1 · VK-FULL-FRAME-DEFAULT-FLIP-1  ⚠ SUPERVISED**
- Goal: master gate `MC2_FULL_FRAME_VULKAN` (default **OFF**) selecting the fused executor over the GL frame loop. (The current doc's `MC2_POSTPROCESS_BACKEND=vk` flip is under-scoped — it only covers postprocess.)
- Size **M** · Risk **H**
- Flip criteria (ALL required): (1) **every** kFramePassOrder region live + non-readback (Shadow consumer wired, Terrain/Water/SP+Mech ported, **the 5 unstarted regions ported**); (2) G1 interop proven, G2 fused, G3 barriers sync-clean; (3) **full-frame** backend-compare (GL frame vs fused-VK frame) all tier1 within-floor, `region_impl` never FallbackGL (LM6 exit-5); (4) **30s soak all tier1** (`mc2_01 mc2_03 mc2_10 mc2_17 mc2_24`), exit 0, no `crash_silent` — canonical smoke, **NEVER** `--kill-existing` / `--duration>30`; (5) **rollback = the gate** — unset → GL frame loop byte-identical (`build64` still GL-only; fused executor `#ifdef MC2_VULKAN`). Any soak/parity failure → gate stays OFF **forever** (frame-graph reorder precedent). **Never self-weaken the oracle to pass.**
- Deps: **all of L8.0** + every region live.
- Fan-out: no (single committer, supervised).

---

## 3. Dependency graph (ASCII)

```
                         ┌─────────────────────────── PARALLELIZABLE FRONT (independent) ───────────────────────────┐
                         │                                                                                          │
 L7.1 slot-2 split ······│(decoupled from live terrain — indirect-path only)                                       │
                         │                                                                                          │
 L7.2 Shadow ── live ────┤                                                                                          │
 (VK-SHADOW-CONSUME-C)   │                                                                                          │
        │                │                                                                                          │
        │        ┌───────┴────────┐                    ┌──────────────────┐        ┌──────────────────────────┐    │
        │        │  L7.3 TERRAIN  │                    │  L7.5 SP+MECH    │        │  D0 conv-doc+checker     │    │
        │        │  T0→T1→T2→T3   │                    │  (needs L7.2)    │        │  U0 UI recon             │    │
        │        │   →T4→T5       │                    │  M0→M1→M2→M3     │        │  (both start NOW)        │    │
        │        └───┬───────┬────┘                    └────────┬─────────┘        └──────────────────────────┘    │
        │            │       │                                  │                                                  │
        │   ┌────────┘       └──────────┐                       │                                                  │
        ▼   ▼                           ▼                       │                                                  │
   ┌─────────────────────────┐   ┌──────────────┐               │                                                  │
   │  L7.4 WATER (linear)     │   │ W-C needs    │               │                                                  │
   │  W-A → W-B → W-C         │   │ L7.3 too     │               │                                                  │
   │  (W-B needs WATER-MVP-   │   └──────────────┘               │                                                  │
   │   ORACLE-1; all need     │                                  │                                                  │
   │   WATER-REGISTRY-0)      │                                  │                                                  │
   └───────────┬──────────────┘                                 │                                                  │
               │                                                 │                                                  │
               └───────────────┬─────────────────────────────────┘                                                 │
                               ▼                                                                                    │
                    ┌──────────────────────┐                                                                        │
                    │  L7.6 / D1           │◄──── needs L7.2 + L7.3 + L7.4 + L7.5 ALL LIVE                          │
                    │  descriptor consol.  │◄──── D0 (optional, de-risks)                                           │
                    └──────────┬───────────┘                                                                        │
                               │                                                                                    │
        U1 (5 unported regions + Shadow consumer) ─── parallel, but all must land before L8.1 ────────────────────┘
                               │
                               ▼
   ┌───────────────────────────────────────── L8.0 ENDGAME (strictly linear) ─────────────────────────────────────┐
   │  L8.0-probe (interop viability) ★ GATES EVERYTHING BELOW                                                       │
   │       │                                                                                                       │
   │       ▼                                                                                                       │
   │  L8.0-interop → interop-N (retire ALL readbacks)                                                              │
   │       │                                                                                                       │
   │       ▼                                                                                                       │
   │  L8.0-fuse (one cmd buffer; needs interop-N + L7.6/D1)                                                        │
   │       │                                                                                                       │
   │       ▼                                                                                                       │
   │  L8.0-barriers (B1–B17 + 4 hand-added classes)                                                               │
   │       │                                                                                                       │
   │       ▼                                                                                                       │
   │  L8.0-present ⚠ (VK owns game window; supervised)                                                            │
   └───────┬───────────────────────────────────────────────────────────────────────────────────────────────────┘
           ▼
   ┌──────────────────────────┐
   │  L8.1 default-flip ⚠     │◄── ALL L8.0 + every region live (incl. U1's 5 + Shadow consumer)
   │  (supervised, soak,      │
   │   rollback = gate)       │
   └──────────────────────────┘
```

**What parallelizes:** L7.3, L7.5, D0, U0, and (once L7.3 lands) L7.4 and the U1 region ports run as independent worktrees. **What is strictly serial:** within L7.4 (W-A→W-B→W-C); within each geometry region's sub-slices; and the **entire L8.0 endgame chain** (probe→interop→fuse→barriers→present→flip) — no fan-out inside it.

---

## 4. Cross-cutting risks + standing discipline

**Standing discipline (non-negotiable across every slice):**
- **Parity oracle is the arbiter.** Every region proves GL-vs-VK pixel parity. A legal verdict (scheduler says a reorder is allowed) ≠ adoption — parity proof beats armchair reasoning. (Frame-graph reorder precedent: legal-but-rejected, gate OFF permanently.)
- **Don't-loosen / never self-weaken the oracle.** A residual ±1-LSB is a **supervisor tolerance-floor question** (§5b per-region floor with sign-off), NOT a self-authored weakening. The SSAO ±1-LSB floor is the only precedent and it was characterized+escalated, not assumed.
- **Captured-behavior-over-comments.** Port what the code *does* (depth-prepass→GL_EQUAL, MVP snapshot save/restore, the 0.00025 fudge), not what a comment claims. Load-bearing behaviors are cited by file:line, not paraphrased.
- **Asymmetric-matrix fixtures.** Every fixture uses a **sheared** viewProj whose inverse is exact-binary, so a transpose regression re-diverges. A symmetric matrix cannot catch LM1.
- **LM1 transpose trap.** Per-region matrix convention is NOT uniform: **fog = row_major** (double-transpose GL_TRUE), **terrain = column_major** (GL_FALSE), **water depth-fudge MVP = column_major** (ORACLE-PROVEN `4d30d93e` — the reasoned "row_major" was WRONG), **SP+Mech ViewUniforms = row_major claimed but UNVERIFIED — build its own oracle first**, **UI ortho = row_major** (GL_FALSE per U0). NEVER trust a reasoned convention; every region gets a bit-tight asymmetric-matrix oracle before its port. Copy-pasting a UBO qualifier across regions silently transposes.

- **U0 UI CORRECTION (recon 2026-07-02):** the ladder's "UI = delete-not-port" is WRONG — it's a **SPLIT**. The live shipping HUD (`gos_DrawQuads/Lines/Triangles` + `gos_TextDraw` → `flushHUDBatch` `gameos_graphics.cpp:7675`, force-group/minimap/text, 160 call sites, pass already tagged `PassIdentity::UI`) is a **byte-exact PORT** (`UIOverlay` region, deterministic replay list, vertex-geometry descriptor convention, alpha-blend, B16, depends on L6.6 present-ownership). Only the DEAD `gl_utils.cpp` fixed-function trio (`draw_quad`/`draw_in_2d`/`draw_textured_cube`, **zero callers**) is the delete. ImGui (Path 2) is separate dev tooling, not the shipping frame.
- **LM2 mid-frame compute.** The fused executor must not float draws across a compute dispatch; reproduce the dispatch→draw SSBO barrier explicitly (terrain-lighting id33; water `ComputeDispatchAndBindThinRecords` `gameos_graphics.cpp:3362`). In Vulkan the descriptor-set model retires the GL slot-6 header/thin multiplex — do NOT reintroduce a shared binding.
- **Depth-EQUAL order-preserve.** SP↔Mech coplanar ties are last-writer-wins; a barrier cannot fix them. Record SP-then-Mech in one cmd buffer with a non-reorder assert (`SAME-ORDER-EXECUTOR-SLICE-2`, `txmmgr.cpp:2478`); `MC2_FRAMEGRAPH_REORDER_SPMECH` stays default-OFF. A 0.1–0.3% diff = reorder tell, must be zero.
- **Readback-bridge = oracle, not production.** Every current ported region is a CPU-readback validation oracle (`vulkan_ssao_island.cpp:42`), not a shipping design. It must die (G1) before fusion.
- **The L6.6 game-window / interop wall.** L5/L6.6 present to a SEPARATE Vulkan window; the GL context owns the real game window (`vulkan_present_object.cpp:11`). `GL_EXT_memory_object` is 0% built. Full-frame is blocked behind this wall until L8.0-probe proves interop on the target driver.

**Cross-cutting risks (ranked):**
1. **Shared-image interop may not work on the target driver (HIGHEST).** Gate the entire endgame behind L8.0-probe. If it fails, readback is permanent — a legitimate ledgered dead-end, reported honestly.
2. **terrain-v2 shader contention.** L7.3-T1 and any water shader edit hit hot files. **Rebase-immediately + slice-preflight STOP-gate** before coding (§5). SP+Mech touches no terrain shaders → exempt.
3. **Fusion churns the region seam.** `runRegion` signature change touches every island. Land L7.6/D1 (stable set model) first.
4. **5 unported regions + fixture-only Shadow.** The flip's "every region live" criterion is far off. VFX copies (B11/B12) are the trickiest (3-layout transfer chains). UI has NO recon (U0 must run).
5. **Present ownership surgery (G4).** Highest blast radius; supervised only; GL fallback byte-identical.
6. **LM8 deploy-lane contention.** Bracket EVERY proof with island-exe `cmp` before+after + `[VK_*]` health-line verification in the run's own log. Island lane = `A:/Games/mc2-opengl/mc2-win64-v0.4c`. **LM5**: spv deploy lockstep from `<build>/shaders/vulkan/*.spv` with `--build64-root build64_island`. **LM3**: never wire to `shaders/spv/` (GL-consumer bakes).

---

## 5. Recommended parallelization plan for the next session

**Constraints honored:** one-committer-per-worktree; lane-lease hygiene (register each worktree in the lane registry before coding, release on merge); the **RenderResourceId/RenderRegionId enum-serialization constraint** — enum *values* are serialized into fixtures/REGION_SPEC/golden captures, so **two worktrees must NOT both append new `RenderRegionId`/`RenderResourceId` enumerators concurrently** (they'd collide on the next integer and silently mis-key captures). Mitigation: **pre-allocate the enum block on the shared branch first** (one tiny commit reserving `TerrainSolid`, `Water`, `StaticPropMechOpaque`, `UIOverlay`, + water/UI resource ids), then fan out — each worktree fills in an already-reserved value.

**Session-0 (serial, ~1 commit): ENUM-RESERVE.** On `claude/vulkan-backend-l6`, land the enum reservations + the D0 convention-doc/checker prep-slice. This unblocks all downstream fan-out without enum collisions and gives every region a table to build against.

**Then fan out — 5 concurrent isolated worktrees:**

| Worktree | Slices | Committer scope | Notes |
|---|---|---|---|
| `wt-terrain` | L7.3 T0→T5 | terrain island + `gos_terrain_lod_chunk.*` + terrain shaders | **slice-preflight STOP-gate** on symbols `gos_TerrainLodChunk_SubmitDrawCommands,s_heightSsbo,s_typeSsbo,s_cementSsbo,s_terrainProgram` before T1. Rebase-immediately vs terrain-v2. |
| `wt-spmech` | L7.5 M0→M3 | `vulkan_sp_mech_island.cpp` + batchers | Independent of terrain (no shared shaders). Gated on L7.2 shadow live. |
| `wt-ui-recon` | U0 (recon only) | read-only | Fills the FAILED recon. No enum churn (uses reserved `UIOverlay`). |
| `wt-regions` | U1 subset — start VegetationCards + VFX (independent of each other) | per-region islands | VFX B11/B12 transfer chains are the hard part; keep separate committers if split further. |
| `wt-probe` | L8.0-probe (VK-INTEROP-PROBE-1) | isolated harness | **Run this EARLY, in parallel** — it gates the entire endgame; a negative result reshapes the whole remaining plan. No dependency on any region. |

**Do NOT parallelize:** L7.4 water sub-slices (linear W-A→W-B→W-C, and W-B waits on WATER-MVP-ORACLE-1); L7.6/D1 (single committer, needs everything live); the L8.0 endgame chain (strictly serial); L8.0-present + L8.1 (supervised, single committer).

**Sequencing note:** start `wt-terrain`, `wt-spmech`, `wt-ui-recon`, `wt-probe` immediately after Session-0. `wt-water` (a 6th worktree) opens **only after L7.3 lands** (W-A needs terrain's TL-output ordering; W-C re-enters the terrain-solid path). Merge-wave discipline: integrate terrain and SP+Mech first (they unblock water and consolidation respectively), re-run the affected regions' parity after each merge.

---

## 6. The endgame: what "full Vulkan frame" (L8.1) requires, and the gap

**What L8.1 requires (all simultaneously true):**
1. **Every one of the 11 `kFramePassOrder` regions has a live, non-fixture, non-readback `runRegion`.** Today: 3 postprocess regions ported (as readback oracles), Shadow fixture-only, and **5 regions entirely unstarted** (TerrainOverlay, TerrainDecal, VegetationCards, VFX, **UIOverlay**). Terrain/Water/SP+Mech are the L7.3–7.5 work above. UIOverlay has **no recon** (U0 must run first).
2. **The CPU-readback bridge is fully retired (G1).** Zero-copy GL↔VK shared images via `GL_EXT_memory_object_win32` / `VK_KHR_external_memory_win32` — **0% built, unproven on the target driver.** This is the make-or-break unknown.
3. **All regions record into ONE frame command buffer (G2)**, one submit, one fence-per-frame — not N per-region islands. Requires the `runRegion` signature change + the L7.6 stable set model.
4. **B1–B17 + the 4 hand-added barrier classes are assembled inside that fused buffer (G3)** — recon-complete, transcription not discovery.
5. **Vulkan owns the game window and presents (G4)** — GL relinquishes it (the L6.6 wall), fused MainColor blits to the swapchain. Supervised.
6. **Full-frame backend-compare + 30s tier1 soak pass, non-fallback, gate-OFF byte-identical, rollback = the gate.**

**The gap from "regions ported via readback oracle" to production:** porting all 11 regions gets you to step 1 only. Steps 2–6 are the four capabilities the region ladder **never builds** — and they are **serially gated on a single unproven capability (interop, L8.0-probe)**. The order is immovable: **probe interop → retire readbacks → fuse into one cmd buffer → assemble the barrier DAG → VK-owned game-window present → default-flip + soak.** Six endgame slices, all *after* the last region ports, plus the five unstarted regions and the Shadow consumer as hard prerequisites. The current doc's single-line "L8.1 default-flip" conflates all six and is **not executable as written** — it presumes fusion/interop/present already solved. The two genuinely hard unknowns are **interop viability** (must be de-risked first, in parallel, via L8.0-probe) and **present ownership surgery** (supervised). Everything else — the barrier DAG, the descriptor-set model, the pass order, the present machinery — is already recon'd or banked. **Honest ledger clause:** if L8.0-probe fails on the 7900 XTX GL+VK combo, the readback bridge becomes permanent and "full-frame Vulkan" is unreachable — a legitimate documented dead-end, not a failure to hide.
