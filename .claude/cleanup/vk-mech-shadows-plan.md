<!-- doc-status: current | reviewed:2026-10-11 -->
# VK shadow island: mech casters plan

Reverted: be08b5575 (feat), reverted by 7cd819aff. Read-only analysis; nothing built.

## Reusable once stride fixed
Nearly all of it (restore with `git revert 7cd819aff`, then re-check):
- shaders/vulkan/shadow_mech.{vert,frag} + CMake basename `shadow_mech`.
- Mc2MechShadowFeed + mc2_mech_island_get_shadow_feed (gos_mech_batcher.cpp, vulkan_mech_island.h), s_vkVboNativeGen.
- ShadowIsland m* members, build_mech_pipeline, upload_mech_geometry, stage_mech_casters, record_mech_casters; calls in dyn-map and fullmap paths (CSM array path was also covered by the commit).
- subGate umbrella conversion of the 3 shadow gates.
NOTE: the reverted VK shader struct is already 64B (4 uint + 2 vec4 + 4 uint). Verify the real stride bug is in the *shadow_prop* instance struct or the C++ side (static_assert sizeof==64, vertex stride 48), not just this shader. Lattice may also be from a bad lightSpaceMatrix/instBase.

## fps 140->27 suspects
1. stage_mech_casters runs per pass (dyn, CSM array, fullmap = up to 3x/frame): vmaMap/memcpy of all instances+bones each time, into single-buffered host buffers.
2. Each pass ends with inline vkWaitForFences (lines ~1262, 1398, 1464, 1532) + vkQueueWaitIdle (~1012): CPU stalls on the whole shadow pass per cascade.
3. ensure_mech_buf / upload_mech_geometry call vkDeviceWaitIdle on growth/geomGen change (1.5x headroom helps, but geomGen bump on any normals rebuild reuploads full VBO).
4. Mech draws add instanced skinned work to each cascade (4 bone fetches/vertex).
Measure first with Tracy zones (VKIsl.Shadow.RecordSubmit).

## Steps
1. Re-apply: revert the revert; assert sizeof(GpuMechInstance)==64 and vertex stride 48; build; props-only unchanged.
2. Perf: stage once per frame (cache by frame id) and share across passes; double-buffer inst/bone SSBOs per parity (fs.parity) or one persistently mapped buffer; no vkDeviceWaitIdle in steady state; replace inline fence waits with parity-based deferred wait (wait on previous frame's fence at next BeginFrame) or timeline semaphore handoff; cull mech instances to cascade bounds; skip upload when snapshot unchanged.
3. Gates: keep mech casters behind subGate("MC2_VK_SHADOW_MECH") default ON under MC2_VK_SHARED_GBUFFER=1, explicit =0 off; shadow-interop gates stay umbrella members.
4. Verify: MC2_VK_SHADOW_MATRIX_DIAG=1 depth PNG dumps (mech silhouette coverage, no lattice, all parts, buckets with instanceBase>0 / multiple mechs); ab.py (not found in repo; locate) day + night GL vs VK; fps vs 140 baseline with and without gate; Tracy zone times.
