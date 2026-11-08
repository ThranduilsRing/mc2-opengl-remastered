Build token FIFO — 2026-11-08

Assignment: A:/Games/mc2-slot04, claude/build-token-fifo.
Actual starting tip: 8fc0c6c89 (Linux fixes on assignment base f269b312d).

Implemented ordered, OS-locked tickets, holder/waiter status, and a per-build
reservation floor (optional BUILD_POOL second number; default 3). Small builds
reserve the floor but retain their requested Ninja width. Ninja keeps one ticket
through its whole wait. Existing slot locks and pool sizing remain unchanged.

Next: subprocess FIFO/death/status tests, targeted module runner and owning row,
cxx17 check, local commit and final-tip lint. No engine build, install or game
needed for this tooling-only change. No lane rules or host config changed.
