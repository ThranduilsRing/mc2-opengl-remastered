<!-- doc-status: stale | reviewed:2026-10-11 -->
# GPU-BUFFER-OWNER-RECON-1

Pre-wrapper census of every GPU buffer (SSBO / VBO / EBO / UBO / DRAW_INDIRECT) in the
renderer. Read-only recon — no code changed. Inputs: 5 parallel subsystem sweeps
(2026-06-21). Goal: produce the slot / lifetime / barrier map *before* any `GpuBuffer`
wrapper exists, so the wrapper is scoped from evidence, not guesswork.

Part of the **RENDER-BACKEND-SEAMS** arc (slice 2). Prior slice: RENDER-PASS-CONTRACT-ENFORCEMENT-1 (`8d250041`).

---

## Headline findings

1. **No buffer abstraction exists.** All buffer lifetime is raw GL. There are exactly
   **three management tiers** in the tree today, in ascending sophistication:
   - **Tier 0 — raw `glBufferData`/`glBufferSubData`, no helper:** `gos_terrain_lod_chunk.cpp`
     (9 buffers), `gpu_cull_compute.cpp` (all C1b staging), static-prop shadow SSBOs,
     HUD immediate-mode meshes, postprocess quad, surface VB/IB/TB, light SSBO.
   - **Tier 1 — `MC2_GL_BufferData`/`MC2_GL_BufferSubData` hitch-accounting macro:** all of
     `gos_terrain_indirect.cpp` + `gos_terrain_water_stream.cpp`. This is a logging wrapper,
     **not** an ownership/RAII object.
   - **Tier 2 — `glBufferStorage` + persistent-coherent map + 3-frame fenced ring:** mech
     batcher (instance/bone SSBOs) and static-prop batcher (instance/color/coalesce/base-instance).
     This is the most mature pattern and the de-facto template a `GpuBuffer` should generalize.

2. **[MOOT / FIXED — corrected 2026-06-22 RENDER-CONTRACT-INDEX-1]** The
   unfenced-water smell described below is **CLOSED**: WATER-THINRING-FENCE-1
   shipped the missing fence. The water thin-ring now fences and waits exactly
   like the solid path — `glFenceSync` at `gos_terrain_water_stream.cpp:2154`
   (`EndThinRingFrameFence()`), waited/cleared at
   `GameOS/gameos/gos_terrain_water_stream.cpp:205` (`WaitAndClearThinFenceForCurrentSlot()`).
   The original (now-historical) finding text follows.

   **Ring/fence discipline is inconsistent — a live correctness smell, not just a Vulkan-prep
   issue.** Fenced 3-frame rings: mech instance/bone, static-prop instance/color/coalesce,
   terrain **solid** thin-record. **UNFENCED** (relies on slot-depth + a single
   `glMemoryBarrier(SHADER_STORAGE)` only): terrain **water** thin-record ring
   (`g_thinBuffer`, gos_terrain_water_stream.cpp). Same ring shape as the solid path but no
   `glFenceSync`/`glClientWaitSync`. This is the kind of latent CPU/GPU race that surfaces as
   intermittent corruption on a different driver (NVIDIA) — flag for the correctness lane,
   independent of any wrapper.

3. **Binding-slot namespace is dense and mode-dependent — a wrapper must own a slot registry.**
   Slot 0 is reused by every instance SSBO (mech, static-prop legacy, coalesce, popsplit,
   shadow). Slot 9 means *cull-debug* in C1a mode but *visibleIds* in C1b mode. Slot 11 is
   *bucket-caps* (compute read) and *indirect-cmd patch* (different context). Slot 2 is
   *terrain thin-record range* AND *per-type hot-color* AND *cull frustum UBO* in different
   subsystems. No single source of truth for binding assignment exists.

4. **Diagnostic buffers masquerade as live state.** `g_thinCanarySSBO` (slot 7, written every
   frame, never read by the bridge), `g_solidBucketHeaderSsbo` / water `s_spikeThin` /
   `s_spikeHeader` (trace-gated, `s_spikeThin` explicitly "NOT fed to draw"). A wrapper census
   should exclude or gate these so they don't inflate residency.

5. **DSA is absent.** Everything uses `glGenBuffers` (bind-to-edit), never `glCreateBuffers` /
   named-buffer DSA. A `GpuBuffer` could uniformly adopt DSA + immutable `glBufferStorage`,
   which is also the closest GL semantic to a Vulkan `VkBuffer` allocation.

---

## Master inventory (deduplicated, grouped by owner)

Slot = `glBindBufferBase`/`Range` index (N/A = VBO/EBO/indirect bound by target).
Lifetime: **persistent** (mission/session), **ring-N** (N-frame fenced unless noted), **transient** (per-frame/per-draw realloc), **static** (mission-load once).

### Terrain — indirect solid path (`gos_terrain_indirect.cpp`) — Tier 1
| Buffer | Slot | Stride/Size | Update | Consumer | Sync | Lifetime |
|---|---|---|---|---|---|---|
| `g_recipeSSBO` (dense recipe) | 0 | 144B | mission-load + dirty SubData | solid.comp, thin VS | none | persistent |
| `g_thinRecordSSBO` | 2 (ranged) | 32B × 65536 × 3 | per-frame ranged write @slot | comp(w)/gos_terrain.frag(r) | **fence** + 10ms wait | ring-3 |
| `g_indirectCmdBuffer` | 8 / indirect | 16B × 16 | cmd_patch.comp | glMultiDrawArraysIndirect | `MemoryBarrier(SSBO\|CMD)` | persistent |
| `g_terrainHandleLutSSBO` | 2 | MC_MAXTEXTURES × 4B | per-frame full SubData | comp | none | persistent |
| `g_solidQuadWindowSsbo` | 9 | kMaxThinRecords × 4B | per-frame SubData | comp | none | persistent |
| `g_mineStaticVBO_GL` | N/A | MineVert × N | dirty/orphan-realloc | glDrawArraysIndirect | none | persistent |
| `g_decalStaticVBO_GL` | N/A | decalVert × N | dirty/orphan-realloc | glDrawArraysIndirect | none | persistent |
| `g_solidBucketHeaderSsbo`* | 6 | 16B | trace-only | readback | (diag) | persistent* |
| `g_thinCanarySSBO`* | 7 | 2×kMaxThinRecords×4B | per-frame comp | **never read** | none | persistent* |

### Terrain — water fast path (`gos_terrain_water_stream.cpp`) — Tier 1
| Buffer | Slot | Stride/Size | Update | Consumer | Sync | Lifetime |
|---|---|---|---|---|---|---|
| `g_recipeBuffer` (water recipe) | 0 | WaterRecipe × N | mission-load | water cull comp | none | static |
| `g_thinBuffer` | 3 (ranged) | 48B × cap × 3 | per-frame ranged write | comp(w)/water VS(r) | **barrier only — NO FENCE** ⚠ | ring-3 (unfenced) |
| `g_waterBucketHeaderSsbo` | 6 | 16B | per-frame comp | comp + readback | `MemoryBarrier(BUFFER_UPDATE)` | persistent |
| `g_waterIndirectCmdBuffer` | 1(patch)/indirect | 32B | cmd_patch.comp | drawn in bridge | `MemoryBarrier(SSBO\|CMD)` | persistent |
| `g_quadWindowSsbo` | 2 | per-frame indices | per-frame SubData | water cull comp | none | persistent |
| `s_spikeThin`* / `s_spikeHeader`* | 3/6 | probe | diag-gated | not-fed-to-draw | barrier | persistent* |

### Terrain — chunk LOD path (`gos_terrain_lod_chunk.cpp`) — **Tier 0 (raw, cleanest wrapper target)**
| Buffer | Slot | Stride/Size | Update | Consumer | Sync | Lifetime |
|---|---|---|---|---|---|---|
| `ps.vbo` / `ps.ibo` (per-patch) | N/A | LocalVertex / u16 | mission-load | glDrawElements | none | persistent |
| `ps.skirtVbo` / `ps.skirtIbo` | N/A | LocalVertex / u16 | mission-load | glDrawElements | none | persistent |
| `s_heightSsbo` | 23 | float × N | mission-load + dirty SubData | tess/VS | none | persistent |
| `s_typeSsbo` | 24 | u8/u32 × N | mission-load | shader | none | persistent |
| `s_cementSsbo` | 25 | word × N | mission-load | shader | none | persistent |

### Mech batcher (`gos_mech_batcher.cpp`) — Tier 2
| Buffer | Slot | Stride/Size | Update | Consumer | Sync | Lifetime |
|---|---|---|---|---|---|---|
| `s_sharedVbo` | attribs 0-6 | 48B GpuMechVertex | finalize, immutable | mech.vert | none | persistent |
| `s_sharedIbo` | element | u32 | finalize, immutable | drawElements | none | persistent |
| `s_instanceSsbo` | 0 | 64B × cap × 3 | coherent-map memcpy/frame | mech.vert | **fence** @2273 / wait @1701 | ring-3 |
| `s_boneSsbo` | 1 | 64B × cap × 3 | coherent-map memcpy/frame | mech.vert skinning | shares instance fence | ring-3 |
| `s_mechMaterialSsbo` | 2 | MaterialGpu | orphan-realloc on grow | mech.frag | **none (un-fenced)** | persistent |
| *consumed not owned:* LightsData(20), named-material(7), ViewUniforms UBO(3) | | | | | | |

### Static-prop batcher (`gos_static_prop_batcher.cpp`) — Tier 2 + Tier 0 (shadow)
| Buffer | Slot | Stride/Size | Update | Consumer | Sync | Lifetime |
|---|---|---|---|---|---|---|
| shared VBO / IBO | attribs/elem | 40B / u32 | finalize, immutable | static_prop.vert | none | persistent |
| per-type hot-color SSBO | 2 | 48B × types | finalize | static_prop.vert | none | persistent |
| instance SSBO (legacy) | 0 | 112B × cap × 3 | coherent-map/frame | static_prop.vert | **fence** | ring-3 |
| color SSBO (legacy) | 1 | 4B × cap × 3 | coherent-map/frame | (debug-only read) | fence | ring-3 |
| coalesce instance SSBO | 0 | 112B × 3 | coherent-map/frame | coalesce.vert | **fence** (own slot) | ring-3 |
| base-instance-by-cmd SSBO | 16 | u32 × cmds × 3 | coherent-map/frame | cull patch | fence | ring-3 |
| permutation SSBO | 15 | u32 × types | finalize | cull patch | none | persistent |
| per-draw entry SSBO | 4 | 32B × types | finalize | coalesce.frag | none | persistent |
| cmd-to-bucket SSBO | 7 | u32 × packets | finalize | cull patch | none | persistent |
| MaterialGpu SSBO | 5 | MaterialGpu × N | finalize | coalesce | none | persistent |
| static popsplit instance SSBO | 0 | 112B | dirty-fill | M1 frozen-cull | fence(`s_staticDrawFence`) | persistent |
| static indirect cmd buffer | indirect | 20B × cmds | CPU-written | glMultiDrawElementsIndirect | none | persistent |
| static-building shadow SSBO | 0 | by-type | per-render glBufferData | shadow_static_prop.vert | none | **transient** |
| dynamic-prop shadow SSBO | 0 | by-type | per-frame glBufferData | shadow_static_prop.vert | none | **transient** |

### GPU cull compute (`gpu_cull_compute.cpp`) — **Tier 0 (all raw)**
| Buffer | Slot | Update | Consumer | Sync |
|---|---|---|---|---|
| debug SSBO | 9 (C1a) | clear+atomicAdd/frame | readback | `MemoryBarrier(SSBO)` |
| staging SSBO | 8 | CopyBufferSubData/frame | cull comp | `MemoryBarrier(CLIENT_MAPPED→SSBO)` |
| frustum UBO | 2 | SubData/frame | cull comp | none |
| visibleIds SSBO | 9 (C1b) | atomicAdd scatter | patch | `MemoryBarrier(SSBO)` |
| bucket-counts SSBO | 10 | clear+patch/frame | patch→indirect | `MemoryBarrier(SSBO\|CMD)` |
| bucket-caps SSBO | 11 | build-once | cull comp | none |
| actor-vis SSBO | 12 | atomicOr/frame | rollup | `MemoryBarrier(SSBO)` |
| block-vis SSBO | 13 | sticky atomicOr | C3 temporal | `MemoryBarrier(SSBO)` |
| indirect cmd buffer (C1b) | 11/indirect | CPU-build + patch | glMultiDrawElementsIndirect | `MemoryBarrier(SSBO\|CMD)` |
| readback SSBO (C2 opt) | 14 | atomicAdd/frame | glGetNamedBufferSubData | `MemoryBarrier(SSBO)` | ring-3 |

### Postprocess + renderer-core scatter (`gos_postprocess.cpp`, `gameos_graphics.cpp`) — Tier 0
| Buffer | Slot | Size | Update | Consumer | Lifetime |
|---|---|---|---|---|---|
| `quadVBO_` (fullscreen) | N/A | 96B | init static | postprocess draws | persistent |
| `s_lightDataSsbo` | 4 (LIGHT_DATA) | dynamic grow | full/split upload/frame | all lit shaders | persistent (grows) |
| `s_surfaceVB/IB/TB` | 20/21/22 | epoch-tracked | glBufferData/epoch | terrain surface VS | static (epoch) |
| `s_indirectCmdBuf` (solid mask) | indirect | 16B | SubData/frame | glDrawArraysIndirect | persistent |
| `s_waterIndirectCmdBuf` | indirect | 16B | SubData/frame | glDrawArraysIndirect | persistent |
| `s_perCmdSsbo` (water MDI) | 7 | ~128B | SubData/frame | water MDI VS | per-frame |
| `vb_`/`ib_` (gosMesh) | attribs/elem | dynamic | updateBuffer/draw | drawArrays/Elements | per-draw |

### HUD / immediate-mode (`gameos_graphics.cpp`) — Tier 0
6 `gosMesh` pairs (quads/tris/indexed_tris/lines/points/text), each VBO+IBO, `glBufferData`
per-draw-batch. ~11 live buffers. `txmmgr.cpp` owns **zero** GPU buffers (textures only).

> **Update-path naming clarification (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).**
> Two SIMILARLY-NAMED, DIFFERENT functions — do not conflate them:
> - `gos_UpdateBuffer` (defined `GameOS/gameos/gameos_graphics_capi.cpp:613`, decl
>   `GameOS/include/gameos.hpp:2864`) is **near-dead**: exactly ONE runtime
>   callsite, `mclib/txmmgr.cpp:2368` (scene-data UBO update). Not the HUD churn
>   source.
> - `updateBuffer` (the private 5-arg overload at
>   `GameOS/gameos/utils/gl_utils.cpp:431`, declared in `utils/gl_utils.h:120`)
>   is the **real per-draw orphan-realloc churn**: it is the gosMesh VBO/IBO/
>   instance update path (HUD / gosMesh, the `vb_`/`ib_` row above), driven from
>   the templated `updateBuffer(this->vb_, ...)` callers in `utils/gl_utils.h`.
>   This churn is **NOT hitch-accounted** (does not route through the
>   `MC2_GL_BufferData` macro), so it is invisible to Tier-1 hitch telemetry —
>   the relevant target for §1.3 accounting-parity when HUD migrates to
>   `GpuBuffer` (adoption slice B, TIER0-HUD).

\* = diagnostic/trace-gated; exclude from residency accounting.

---

## Vulkan mapping (consistent across the census)
- 3-frame fenced ring → 3 frames-in-flight; `glFenceSync`/`glClientWaitSync` → per-frame `VkFence`.
- `glBufferStorage(MAP_PERSISTENT|COHERENT)` + map → `HOST_VISIBLE|HOST_COHERENT VkBuffer` sized
  `3 × perFrameCap`, per-frame dynamic descriptor offset (`slot*cap*stride`).
- `glMemoryBarrier(SHADER_STORAGE\|COMMAND)` in the compute→draw paths → `vkCmdPipelineBarrier`
  (compute→indirect/vertex). These already exist and are explicit — the cull/indirect paths are
  the *most* Vulkan-ready buffer code in the tree.
- DRAW_INDIRECT buffers → `INDIRECT` usage; SSBOs → `STORAGE`; VBO/EBO → `VERTEX`/`INDEX`.

---

## Recommended next slices (do NOT start the wrapper blind)
1. **GPU-BUFFER-WRAPPER-DESIGN-1** — design a `GpuBuffer` + `GpuRingBuffer` API from the Tier-2
   pattern (the mech/static-prop fenced coherent ring is the proven template). Must include a
   **binding-slot registry** (finding #3) — a single header enumerating every slot so collisions
   are compile-visible.
2. **WATER-THINRING-FENCE-1** (correctness lane, can ship independently) — add the missing
   `glFenceSync`/`glClientWaitSync` to the water thin-record ring to match the solid path
   (finding #2). Candidate cause of intermittent driver-specific water corruption.
3. **GpuBuffer adoption order** — convert Tier-0 first (lowest risk, no existing wrapper to
   reconcile): `gos_terrain_lod_chunk.cpp` (9 static buffers) → HUD meshes → postprocess quad.
   Defer Tier-1/Tier-2 until the wrapper subsumes the ring+fence semantics.
4. Gate or drop the 4 diagnostic buffers (finding #4) so residency reporting is honest.

---

## Cross-references (added 2026-06-22 RENDER-CONTRACT-INDEX-1)
- `docs/render-backend-seams/render-contract-index-1.md` — arc index for the RENDER-BACKEND-SEAMS / render-contract work; start there for current status of every finding in this recon.
- `docs/render-backend-seams/binding-slot-occupancy.{md,json}` + `scripts/check-binding-slots.py` — authoritative buffer-binding occupancy (supersedes finding #3's "no single source of truth"; the multiplexed-per-pass model now has tooling).
- `docs/render-backend-seams/gpu-buffer-wrapper-design-1.md` — the wrapper design scoped from this census (§3 flat-enum is superseded; §4 adoption order stands).
