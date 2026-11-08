# rt-scene-2 state (2026-11-08)

Lane `claude/rt-scene-2`, base 2d90723cf. Part 2 of the RT shared ray-visible scene: skinned movers and refit. Foundation only, no RT effects.

## Done (code, builds clean at -j6)
- `vk_rt_acceleration.h/.cpp`: GeometrySignature + refitAction (kRefitsBeforeRebuild=120), compactionAllowed (no compaction for ALLOW_UPDATE on AMD
  proprietary), instanceCustomIndexFits / kMaxInstanceCustomIndex (makeInstance refuses past 24 bits), frameRebuildBarrier, BlasJob + recordBlasJobs,
  recordTlasRebuild, recordBarrier(Barrier).
- `vk_rt_scene.h/.cpp`: Kind::Mover, kMaskMover, mover slots (custom index kMechIdFirst + slot), leave rules (handle dead, or 600 unseen frames),
  recordMovers: per-actor ALLOW_UPDATE|PREFER_FAST_BUILD BLAS over the skin output, refit every frame, rebuild on signature/flags change or after 120
  refits; per-frame mover TLAS = static instances + every mover with a BLAS; frame-fenced retire (bytes passed to retire); byte ledger line
  `[VK_RT_SCENE v1] movers live= ... retired= left= dropped= pending_retire= pending_retire_bytes=`; MC2_VK_RT_SELFTEST probes `probe movers=hit/n`
  and `probe despawn_clean=1|0`.
- `vulkan_mech_skin.h/.cpp`: setRayInput (SDA + AS build input on the skinned buffers), region(), skinnedAddress().
- `vulkan_mech_island.cpp`: record_ray_movers after the skin dispatch, outside the render pass; mech IBO gets SDA + AS build input when accel is present.
- Doctests appended to tests/unit/test_vk_rt_layout.cpp and test_vk_rt_scene.cpp (refit matrix, compaction rule, 24-bit limit, barrier bits, mover
  slot limit 0x3FFFF, stuffToWorld/moverTransform, leave rules).

## Known limits (by design, for part 3 to revisit)
- Off-screen movers keep their last pose. Instances drawn from the bind-pose VBO (not pre-skinned) are not ray-visible.
- Vehicles (bit-31 keys) and handles with generation >= 0x800 (bit 31 set) leave by the unseen timeout only.
- No mech feed in a frame means no mover update that frame. The lab preview path is not hooked.
- The static `s.tlas` is kept beside the mover TLAS; the per-frame instance upload copies all static instances (fine at current counts).

## Open
- cxx17 ratchet: LG05 +2 (moverCustomIndex bool+out-param, vk_rt_scene.h:47; MechSkinPass::region -1 sentinel, vulkan_mech_skin.h:83). Convert both
  to std::optional before merge. LG08 fixed.
- Doctests not run: `build_contract_harnesses.py --only` did not match test_vk_rt_layout / test_vk_rt_scene / case names; find the registered name.
- NOT RUN: RT selftest on XTX and 2070 under MC2_VULKAN_VALIDATION=sync, smoke-lite on E:/mc2-installs/rt-scene-2 (install never created), A/B.
