# lighting-hud-residuals state

Assignment: slot06, claude/lighting-hud-residuals, base 8fc0c6c89.

Implemented: slot/thread-local HUD scratch; adjacent equivalent command merging
with complete-primitive and contiguous-range guards; registration-revision light
membership cache (replaces per-frame world-slot scan and linear membership).
Classification itself stays in the existing registration tags; the pointer cache
refreshes lazily after tag/removal/reuse/reset, including empty-slot untag.
Doctests cover state differences, alpha order, primitive boundaries, light
classification parity with the old slot scan and lifecycle invalidation.

Proof commands at the resulting source tip (results recorded outside git under
E:/mc2-archive-2026-10/proofs/lighting-hud-residuals and proofs/box/mc2-slot06):
- scripts/ninja_build.py mc2 -j6
- tools/build_contract_harnesses.py --run --unit --only test_light_pool_culling
  --only test_present_handoff --only test_world_light_tags
  --only object_light_row_harness -j6
- bash scripts/check-contracts.sh --rows 'lighting|present' -j6
- tools/lane_merge.py --lint HEAD
- tools/box_run.py --gpu 0 -- build unit smoke-lite

Remaining terrain pack work: existing CPU pack storage is already persistent.
Safe suppression of repacking needs source dirty notifications covering elevation,
normal, shadow, water, vertex XY and calcThisFrame, including camera re-entry and
mission reset. The assigned files expose no complete revision/dirty feed. Do not
add a redundant candidate-pack comparison and call it a performance improvement.
Native producers still upload the visible window each frame; GPU dirty updates
would also need changes outside the assigned files (vk_native_producers).
Next run: establish complete terrain source invalidation and retired-slot GPU
residency with that consumer's owner, then add the plan's random-edit/full-repack
equivalence case. No terrain vertex optimization or dirty-range test is claimed.

No performance claim; weekly measurement belongs to the orchestrator.
