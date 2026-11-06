# Merge queue v1 lane state (claude/merge-queue-v1, base 5d2e62701)

Done: staging (--stage/--promote/--bisect in lane_merge, merge_gate accepts claude/stage-*), fail-fast syncval
prerequisite (--keep-going for exhaustive), durable gate-verdict.json before cleanup, last-full-gate only on PASS,
r7 fixes 1-3 (no swap/snapshot when no capture-before row, save_deployed no second snapshot, per-worker aux device,
box determinism/saveload CPU-only via grh), semantic precheck before regeneration, ABL include-path doctest,
land_queue.py and the pre-gate headless-smoke row deleted, per-stage timings.

Not done: informational inventories as artifacts (see docs/merge-queue-v2.md); the real staging dry run with a
Windows smoke-lite (lane ran out of tool budget). Next lane: build the stage candidate in a throwaway clone, run
merge_gate --install E:/mc2-installs/merge-queue-v1 with the syncval prerequisite, check gate-verdict.json timings.
