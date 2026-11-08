# rt-scene-1 (RT scene part 1 of 3): state at hand-back, 2026-11-07

Branch `claude/rt-scene-1` (worktree A:/Games/mc2-slot01). First pass was based on `claude/vk-rt-infra` at 981e85479 (tip b96a24f8d); the second pass
(this note) applies Astra p34, Fable p34 and Astra p35 section 6. RT FOUNDATION ONLY: no AO, GI, reflections or ReSTIR.

## Done

### Infra fixes (Astra p34, first pass)

- **F1:** the barrier chain emits neither the AS-COPY stage nor the ray-tracing-shader stage. Copies sync at the AS-build stage; the consumer is compute.
- **F2:** Upload is `Transfer|Compute -> AsBuild` with `SHADER_READ`.
- **F3:** `recordBuild` creates into a local and assigns only on success. `recordCompactCopy` refuses a non-empty destination or a size of 0.
- **F5:** by design: the custom index is the bounded object ID (`customIndexFits`). Movers will use a table slot in the mech range.
- **F4** (refit geometry signature): part 2.

### Compaction matrix (Fable p34)

- Selftest stage 6 is the matrix {1 tri, 512-tri 16x16 grid} x {C, U|C fast-trace, U|C fast-build} x {as copied, after a refit of the copy}.
  One line per cell: `matrix geo= flags= as_bytes= compacted_bytes= copied= refit=`; the PASS line ends `matrix_cells=6 matrix_missed=N`.
- CLONE mode was not built (the engine has no clone path to protect); the COMPACT cells decide the refusal. See the result section.

### Terrain tile contract (Astra p35 section 6)

- RT terrain is cut into stable world-space tiles of 16 coarse intervals (`kTerrainTileWorld` = 2048 wu), IDs `ty * per + tx`, count independent of the
  camera and of the visual bake (`terrainTilesPerSide`, `terrainTileId`, `buildTerrainTile` in `vk_rt_scene_math.cpp`). Border samples are shared.
- One triangle-diagonal rule `mc2terrain::quadSplitsTrBl` (`GameOS/gameos/terrain_diagonal.h`: the flatter diagonal, TL-BR on ties). The raster island
  (`vulkan_terrain_island_island.cpp`, one call site) and the RT tiles call it. Doctest: a saddle quad gives the TR-BL split in both, and the traced interior
  height uses the same triangle.
- Picking and line of sight stay on the CPU coarse path (unchanged). Terrain lane 1 does not need to adopt anything for raster: the switch is done.
- Not done: per-tile version and bounds fields (the tile key is the terrain key today; a per-tile dirty version comes with terrain deformation).

### Batched builds and byte ledger

- `recordBlasBuildBatch`: N BLAS builds per `vkCmdBuildAccelerationStructuresKHR`, each on its own aligned scratch sub-range, grouped under a 64 MiB
  per-call budget (`scratchBatchPlan`, pure, doctested). Prop and terrain BLASes use it.
- TLAS is built PREFER_FAST_BUILD. It is rebuilt on every publish (registry generation or terrain key change); a per-frame rebuild arrives with movers (part 2).
- `[VK_RT_SCENE v1] publish` logs the ledger: `blas_bytes built_bytes compacted_bytes tlas_bytes scratch_bytes instance_bytes total_bytes blas_build_calls
  batch_scratch_bytes`. Not done: a hard budget with refusal.
- `VK_RT_SCENE` lines go through `MC2_EVENT(Vk, ...)`, so `log_tag_census` tracks them; `tests/unit/log_tag_baseline.json` has the VK_RT_SCENE entries.

## Runtime result

Stage 6 matrix at 2d48a7167, mc2_01, MC2_VULKAN_VALIDATION=sync (0 errors and 0 SYNC-HAZARD on both GPUs):

| geo | flags | XTX bytes (built->compact) | XTX copied | XTX refit | 2070 copied | 2070 refit |
|---|---|---|---|---|---|---|
| tri1 | C | 720->720 | hit | n/a | hit | n/a |
| tri1 | U\|C trace | 724->660 | MISS | hit | hit | hit |
| tri1 | U\|C build | 724->660 | MISS | hit | hit | hit |
| grid512 | C | 83724->27468 | hit | n/a | hit | n/a |
| grid512 | U\|C trace | 84748->35156 | hit | hit | hit | hit |
| grid512 | U\|C build | 84748->31756 | hit | hit | hit | hit |

Applied the Fable rule: the misses are only U|C, only AMD, and only at 1 triangle. Fix: `recordCompactCopy` refuses an ALLOW_UPDATE source only when `ctx.driverId() == VK_DRIVER_ID_AMD_PROPRIETARY` (new `VulkanContext::driverId()`). No new gate. A primitive floor would need a sweep (sampled only at 1 and 512).

Scene probes: on the 2070, props 7/7 and terrain 4/4. On the XTX, props 7/8 and terrain 4/4. The one XTX miss was ray 1, which hit a neighbouring prop: custom 0x10037e where 0x10036b was expected, and expect_t=-1, so there is no distance check. Probably a geometric occluder, not a structure fault (unconfirmed). Publish ledger on the XTX: 49 terrain tiles, 918 props/197 types, 222 dropped, total BLAS 27.5 MB; the TLAS rebuilds each frame with built_bytes=0 after the first.

## Open

- Hard byte budget plus refusal; per-tile version and bounds; F4 refit signature; movers (part 2).
- `cxx17_ratchet --check` is red at the base (LG05/06/09/17 vs the stale 7158f679b baseline, from vk-rt-infra); this lane adds no site (counts equal HEAD~1).
