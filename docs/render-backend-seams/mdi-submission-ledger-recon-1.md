<!-- doc-status: current | reviewed:2026-10-11 -->
# MDI-SUBMISSION-LEDGER-RECON-1

Read-only recon. The pass contracts say what a pass writes/reads; this catalogues **how
geometry enters the pass** — every indirect-draw submitter: who fills the command buffer,
how many draws/instances, which buffers, CPU vs GPU producer, barrier, fallback, and
zero-draw behavior. Source-verified vs nifty `b0ebc527`. Call-site lines confirmed by grep
of real `gl*Indirect(` statements (not comments).

## The 10 indirect-draw call sites

| # | Site | Family / variant | Draw fn | Indirect buffer | Cmd struct (stride) | drawcount source | Producer | Barrier before | Fallback |
|---|---|---|---|---|---|---|---|---|---|
| 1 | `GameOS/gameos/gameos_graphics_bridge.cpp:410` | **Terrain solid** (GPU-driven) | `glMultiDrawArraysIndirect` | `g_indirectCmdBuffer` (ring) | DrawArraysIndirect (16B) | `cmdCount` (=1) | **GPU** (atomicAdd in cull/pack) | `SSBO\|COMMAND` (terrain_indirect.cpp:3558) | CPU legacy tessellation |
| 2 | `GameOS/gameos/gameos_graphics_water.cpp:690` | **Water fast-path** | `glMultiDrawArraysIndirect` | `WaterStream::GetIndirectCmdBuffer()` | DrawArraysIndirect (16B) | `drawCount` (1 or 2: base±detail) | **GPU** (WaterStream compute) | UNKNOWN (likely in WaterStream dispatch) | none (path is optional) |
| 3 | `gameos_graphics.cpp:4493` | **Terrain mask solid** | `glDrawArraysIndirect` | `s_indirectCmdBuf` (static) | DrawArraysIndirect (16B) | 1 (count=quadCount*6) | **CPU** (`glBufferSubData`) | none (CPU-filled) | none (mask optional) |
| 4 | `gameos_graphics.cpp:4652` | **Terrain mask water** | `glDrawArraysIndirect` | `s_waterIndirectCmdBuf` (static) | DrawArraysIndirect (16B) | 1 (count=recipeCount*6) | **CPU** (`glBufferSubData`) | none | none |
| 5 | `GameOS/gameos/gos_static_prop_batcher_submit.cpp:708` | **StaticProp M2a alpha-OFF** | `glMultiDrawElementsIndirect` | `s_staticIndirectCmdBuf` | DrawElementsIndirect (20B) | `s_alphaOffCmdCount` (CPU packets) | CPU coherent map; counts CPU | `gpuSyncBarrier` @6052 | legacy per-type loop |
| 6 | `GameOS/gameos/gos_static_prop_batcher_submit.cpp:717` | StaticProp M2a alpha-ON | `glMultiDrawElementsIndirect` | `s_staticIndirectCmdBuf` | (20B) | `s_alphaOnCmdCount` | CPU | shared @6052 | legacy |
| 7 | `GameOS/gameos/gos_static_prop_batcher_flush.cpp:177` | StaticProp coalesce BC7 bucket | `glMultiDrawElementsIndirect` | `gpu_cull::compute_getIndirectCmdBuf()` | (20B) | `s_bucketCmdCount[b]` | **GPU** (patch shader) | `SSBO\|COMMAND` (gpu_cull_compute.cpp:1297) | V5 / legacy |
| 8 | `GameOS/gameos/gos_static_prop_batcher_flush.cpp:48` | StaticProp coalesce v6 alpha-OFF | `glMultiDrawElementsIndirect` | `compute_getIndirectCmdBuf()` | (20B) | `s_alphaOffCmdCount` (packets); instanceCount GPU | **GPU** (patch) | @1297 | V5 / legacy |
| 9 | `GameOS/gameos/gos_static_prop_batcher_flush.cpp:70` | StaticProp coalesce v6 alpha-ON | `glMultiDrawElementsIndirect` | `compute_getIndirectCmdBuf()` | (20B) | `s_alphaOnCmdCount` | **GPU** (patch) | @1297 | V5 / legacy |
| 10 | `GameOS/gameos/gos_static_prop_batcher_flush.cpp:571` | StaticProp C1b per-type | `glDrawElementsIndirect` | `compute_getIndirectCmdBuf()` | (20B) | 1 per typeID (instanceCount GPU) | **GPU** (patch) | @1297 | CPU `glDrawElementsInstancedBaseVertex` |

## Producer / consumer split

- **GPU-compute producers:** terrain (atomicAdd in pack shader), water (WaterStream compute),
  static-prop coalesce v6/BC7/C1b (gpu_cull patch shader). `gpu_cull::compute_dispatch()`
  is a **pure producer** — it runs cull→patch→rollup kernels and a
  `glMemoryBarrier(SSBO|COMMAND)` @1297, then RETURNS; it issues NO draw. The consumer
  (`flush()`) issues the `glMultiDrawElementsIndirect` after the barrier.
- **CPU producers:** terrain mask solid/water (`glBufferSubData` each frame), StaticProp M2a
  (coherent-mapped instance buffer, gate default OFF).

## Defaults (which path is live)

- Terrain solid: GPU-driven default-on. Water fast-path: armed default. Terrain mask: gated.
- StaticProp live default = **coalesce v6** (sites 8/9, MC2_DRAW_PACKET_COALESCE_V6 on via
  `IsCoalesceEnabled`; `MC2_STATIC_PROP_LEGACY_DISPATCH=1` kills it). M2a (5/6) default OFF;
  BC7 (7) opt-in; C1b (10) when `compute_isEnabled` (GPU-cull substrate).

## Barriers / sync (Vulkan-critical)

Both GPU-producer families gate the indirect read behind
`glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)` — terrain at
`GameOS/gameos/gos_terrain_indirect_pack.cpp:499`, static-prop at `gpu_cull_compute.cpp:1297` (documented
AMD command-processor reorder mitigation). CPU-filled paths need no barrier. **Water's
barrier site is UNKNOWN** (not in the render block; presumed inside WaterStream dispatch) —
flag for the scaffold to confirm.

## Counters & the zero-draw lead

`gpu_drawn_instances` / `cpu_fallback` are accumulated per-population and reported by the
`[OBJBATCHER v1]` trace (`gos_static_prop_batcher.cpp:1759`); `cpu_fallback` incremented at
`recordCpuFallback()` @4490 (an actor that fell to the legacy `TG_Shape::Render` CPU path).
**The exact per-frame `gpu_drawn_instances` increment site is UNKNOWN** across v5/v6/BC7 —
a gap the scaffold trace should close. The mc2_10 `gpu_drawn_instances=0 + cpu_fallback=4`
observation is the lead for **OBJBATCHER-ZERO-GPU-DRAWN-RECON-1** (classify, don't fix).

## Notable absences

- **No mech indirect** — `gos_mech_batcher.cpp` uses direct (non-indirect) draws.
- **No vegetation / VFX indirect** in the searched TUs.

## Recommended arc

1. **MDI-SUBMISSION-SCAFFOLD-1** — `[MDI_SUBMIT] pass= drawKind= commands= instances=
   indirectBuffer= producer= fallback=` trace at each of the 10 sites (gated) + a
   submission descriptor; no behavior change.
2. **check-mdi-submission-ownership.py** — every real `gl*Indirect(` call site has a ledger
   row with PassId, producer (CPU/GPU), drawcount source, fallback; every default-on path
   emits `[MDI_SUBMIT]` when gated. Negative test: remove a row → FAIL.
3. **OBJBATCHER-ZERO-GPU-DRAWN-RECON-1** — classify mc2_10 `gpu_drawn_instances=0`
   (expected non-batchable? disabled by flags? regression? scope?). Read-only.

## Do NOT
Buffer/ring rewrite, command-layout change, scheduling change, barrier removal, fix the
zero-draw blindly, Vulkan backend.
