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

- **Proof ladder:** doctests, ratchet, lint, the real-scene probe run, a sync-validation row, smoke-lite on `E:/mc2-installs/claude/rt-scene-1`, and the A/B on mc2_01 and mc2_10.
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
