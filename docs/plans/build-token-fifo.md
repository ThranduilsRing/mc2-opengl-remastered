Build token FIFO — 2026-11-08

Assignment: A:/Games/mc2-slot04, claude/build-token-fifo.
Actual starting tip: 8fc0c6c89 (Linux fixes on assignment base f269b312d).

Implemented ordered, OS-locked tickets, holder/waiter status, and a per-build
reservation floor (optional BUILD_POOL second number; default 3). Small builds
reserve the floor but retain their requested Ninja width. Ninja keeps one ticket
through its whole wait. Existing slot locks and pool sizing remain unchanged.

Covering tests: three FIFO waiters, killed waiter, killed holder, timeout cleanup,
status CLI, configurable floor, small pools, partial width and Ninja ticket reuse.
The first test attempt passed six cases but hit 15-second worker startup limits;
worker imports were reduced and bounded startup waits raised for the loaded host.

Validation receipts: E:/mc2-archive-2026-10/proofs/build-token-fifo/ (scripts-tests,
cxx17, contracts, lint). Use run_scripts_tests.py with the full module path
scripts/tests/test_build_tokens.py (the runner does not accept bare build_tokens).
The owning scripts_tests row is absent from this base's check-contracts.sh;
record a zero-row selection as missing coverage, never a passing contract proof.
Handoff: implementation committed; validation BLOCKED, not green. Initial direct
pytest: 6 passed, 3 fixture startup errors in 148.85 s. Subsequent targeted runner
and cxx17 attempts accumulated <0.5 CPU seconds over several minutes on a 100%
loaded host. Both were stopped. The box fallback also stalled before upload
(only child: local git update-ref); it was stopped before remote execution.
The requested contract selection ran zero rows. Final-tip lint NOT RUN.
Cleanup of G:/mc2-tmp/build-token-fifo-pytest was rejected by tool policy; leave
that scratch listed as pending, do not bypass the rejection. No installs created.

Next when CPU capacity is available, from this worktree at the final tip:
1. py -3 scripts/run_scripts_tests.py scripts/tests/test_build_tokens.py --jobs 6 --report
2. py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check --worktree --jobs 6
3. py -3 tools/lane_merge.py --lint HEAD
4. Resolve the absent owning contract row with the orchestrator; no board changes
   were made in this lane. Fix any code/test red and commit locally.

No engine build, install or game needed. No lane rules or host config changed.
