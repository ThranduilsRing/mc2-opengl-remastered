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
Next on interruption: finish pending validation, fix any new red, commit locally.
No engine build, install or game needed. No lane rules or host config changed.
