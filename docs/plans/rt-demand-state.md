# RT demand lane

Assignment: slot03, claude/rt-demand, base f269b312d8328aaafccec0b53a15f23ce5775efb.
Plan: demand-gate collection/upkeep; skip unchanged RT placements; retain unchanged
material/index buffers; retire mover TLAS when row offsets change. Preserve deferred
retirement and map invalidation. No new environment gate.

Inspected: terrain/prop/mech hooks and static/mover publication paths. Registry
generation includes lighting changes, so it cannot alone signal RT changes.
Green: `rt *` doctests (40 cases, 100363 assertions), production Vulkan shader
compile, cxx17 ratchet, diff check. Receipts: E:/mc2-archive-2026-10/proofs/rt-demand/.
Proof pending: final-tip Ninja, remaining covering layout contracts, final-tip lint,
sequential XTX/2070 self-tests with sync validation and probe checker, ordinary
2070 zero-work run, smoke-lite. Orchestrator seed became ready before deploy.
Baseline receipt: 549 unchanged publishes / 555 frames, median 2.8 ms/publish;
2.706 seconds total (existing W134 log, no new timing runs).

Follow-up before runtime: invalidate retained terrain keys when the final consumer
leaves, so terrain-only scenes rebuild their retired TLAS on renewed demand.
Retest/rebuild/lint this corrected tip. First deploy is copying shared payload;
refresh its executable/manifest after the final build, before any game proof.
