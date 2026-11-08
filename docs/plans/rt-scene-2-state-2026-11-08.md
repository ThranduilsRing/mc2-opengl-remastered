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

## Closed (2026-11-08, second pass)
- cxx17 LG05: moverCustomIndex and MechSkinPass::region now return std::optional; ratchet adds 0, `lane_merge.py --lint HEAD` PASS.
- Doctests: `build_contract_harnesses.py --run --unit --only "rt *"` (case-name wildcard) 32/32 pass.
- AMD bug: the mover TLAS never built on the RX 7900 XTX. Its host-writable instance buffer got a 4-byte-aligned device address and
  recordTlasRebuild refuses instances that are not 16-aligned. The buffer is now padded by kInstanceAddressAlignment and the instances are written at
  the aligned offset. A `FAIL mover tlas` event (first 3) logs count/cap/address/sizes if the build is ever refused again.
- RT selftest (MC2_VK_RT_SELFTEST=1, MC2_VULKAN_VALIDATION=sync) at 3417587b9: XTX and 2070 x mc2_01 (movers=1/1) and mc2_10 (movers=4/4), all
  despawn_clean=1, 0 validation errors, 0 SYNC-HAZARD, 0 FAIL lines, byte ledger present.
- Smoke-lite on E:/mc2-installs/rt-scene-2 at 3417587b9: 2/2 PASS, deploy fingerprint OK.

## Open
- NOT RUN: A/B against 2d90723cf, saveload, ms numbers.
