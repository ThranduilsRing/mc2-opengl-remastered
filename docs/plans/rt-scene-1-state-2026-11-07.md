# rt-scene-1 (RT scene part 1 of 3): state at hand-back, 2026-11-07

The lane stopped at its tool-call budget. The code is committed on `claude/rt-scene-1`, based on `claude/vk-rt-infra` at 981e85479. See the hand-back for the commit and build status.

## Done (in the diff)

### Infra fixes (Astra p34)

- **F1:** the barrier chain emits neither the AS-COPY stage nor the ray-tracing-shader stage. Copies sync at the AS-build stage, and the consumer is compute.
- **F2:** Upload is `Transfer|Compute -> AsBuild` with `SHADER_READ`.
- **F3:** `recordBuild` creates into a local and assigns only on success. `recordCompactCopy` refuses a non-empty destination or a size of 0.
- **F5:** handled by design. The custom index is the bounded object ID, checked by `customIndexFits`. Movers will use a table slot in the mech range.
- **F4** (refit geometry signature): part 2.
- Selftest stage 6 refits a compacted `ALLOW_UPDATE` BLAS and logs `update_compact=`.
- The `canUpdate()` refusal in `recordCompactCopy` is lifted TENTATIVELY. It must be restored if stage 6 fails.

### Scene

- `vk_rt_scene.h`: policy header covering keys, full-registry membership, fog of war, geometry and lifetime.
- `vk_rt_scene_math.cpp`: the pure half, covered by doctests in `tests/unit/test_vk_rt_scene.cpp`.
- `vk_rt_scene.cpp`: the Vulkan half, in `IslandSlot::RtScene`. Static BLASes are fast-trace plus compaction.
- The TLAS is rebuilt into a new owner and published only on success. The old TLAS retires, then the old BLASes.
- A failed build keeps the last snapshot.
- Under `MC2_VK_RT_SELFTEST`, a probe logs `[VK_RT_SCENE v1] probe ...` lines.

### Hooks

- The colour-prop VBO gets BDA and AS-input usage when `rt().accel` is set.
- `updateProps` runs after `prepare_mission_resources`.
- `invalidateProps` runs in `invalidate_geometry`.
- `noteTerrain` runs in the terrain island run.

## Not done

- **Astra p35 terrain review** (A:/Games/mc2-ops/studies/astra-review-p35-2026-11-07.md, last third). Received at the tool budget; do these FIRST in the next lane:
  1. `kTerrainChunkCells=64` counts intervals of whichever grid is active (`vk_rt_scene_math.cpp:58`): 2048 wu with the bake, 8192 wu without. Replace it with stable world-space tiles of 16 coarse intervals (2048 wu), independent of the bake and of gameplay blocks. Keep shared border samples, and give each tile a version and bounds.
  2. RT always emits the TL-BR diagonal (`vk_rt_scene_math.cpp:119`), but raster picks the flatter-pair diagonal when the bake is present (`vulkan_terrain_island_island.cpp:93-99`). Factor raster's choice into one pure function that both call. Add a doctest: in a saddle cell, raster and RT triangles give the same interior height. The existing chunk test that expects `idx 0,66,65` must follow the shared rule.
  3. Build fixed, camera-independent, error-bounded RT meshes. Do not follow raster LOD or morphing (the current mesh already complies).
  4. Picking and line of sight stay on the CPU path (unchanged here).
- **Proof ladder:** the Ninja build is green (rc=0). Not run: ratchet, lint, the real-scene probe run, a sync-validation row, smoke-lite on `E:/mc2-installs/claude/rt-scene-1`, and the A/B on mc2_01 and mc2_10.
- **`log_tag_baseline.json`:** needs a `VK_RT_SCENE` entry.

### Orchestrator asks after the Fable review (A:/Games/mc2-ops/studies/fable-review-p34-2026-11-07.md), not started

1. Build the selftest matrix:
   - {1 tri, 512-tri grid} x {C, U|C fast-trace, U|C fast-build} x {COMPACT, CLONE} x {as copied, after a refit of the copy}.
   - Log the AS size, the compacted size and the hit counts.
   - Run it in one smoke with `MC2_VK_RT_SELFTEST=1` on the XTX and on the 2070, with sync validation.
2. Decide from the matrix:
   - CLONE hits and COMPACT misses only for U|* on the XTX: gate the refusal on `VK_DRIVER_ID_AMD_PROPRIETARY`.
   - The 512-tri or fast-build variants hit: it is a corner case, so set a primitive floor or a FAST_BUILD exemption.
   - CLONE or the 2070 also misses: our copy path. Check the R:103 vs R:110 sizing.
3. Batch the prop BLAS builds: N infos in one `vkCmdBuildAccelerationStructuresKHR` with scratch sub-ranges. Today each build is serialised on one `ScratchArena` buffer with a barrier between builds.
4. Build the TLAS with PREFER_FAST_BUILD. It is fast-trace today, through the default `BuildFlags`.
5. Add a scene byte ledger with a hard budget and a summary line. `logPublish` already logs the real `AccelStructure::size()` values. Missing: the budget and a refusal above it.

## Open questions to check in the first run

- Does `GpuStaticPropRegistry::getRegistryGeneration()` stay put between frames? Count the publish lines.
- Do `getBuildingShadowInstances` plus `getDynamicPropShadowInstances` cover every placed static prop?
- Do the probe prop rays hit their own object ID? That confirms `propTransform` and the space convention.
- Does the terrain `t` match `20000 - h`? That confirms the terrain space.
