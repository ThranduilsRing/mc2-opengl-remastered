# Merge queue v1 lane state (claude/merge-queue-v1, base 5d2e62701)

Done: staging (--stage/--promote/--bisect in lane_merge, merge_gate accepts claude/stage-*), fail-fast syncval
prerequisite (--keep-going for exhaustive), durable gate-verdict.json before cleanup, last-full-gate only on PASS,
r7 fixes 1-3 (no swap/snapshot when no capture-before row, save_deployed no second snapshot, per-worker aux device,
box determinism/saveload CPU-only via grh), semantic precheck before regeneration, ABL include-path doctest,
land_queue.py and the pre-gate headless-smoke row deleted, per-stage timings.

2026-11-06 fix-up: rebased onto nifty a1f09109a without conflicts; extracted staging, promotion and bisection into
tools/lane_merge_stage.py, preserving the CLI and queue behaviour; ABL filesystem inputs use mc2::czstring.
Next: commit final tip; record its SHA in cxx17/file-size/pytest and box build+unit/lint receipts under
E:/mc2-archive-2026-10/proofs/merge-queue-v1; clean up this branch's box lane. No merge gate in this run.
The test paths in the brief do not exist: use tools/test_lane_merge.py and scripts/tests/test_gate*.py plus
scripts/tests/test_merge_speedups.py. Informational inventories remain follow-up (docs/merge-queue-v2.md).
