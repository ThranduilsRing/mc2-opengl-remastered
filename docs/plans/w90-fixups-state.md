# W90 fixups state

Assignment: mc2-lane10, claude/w90-fixups, base 123461d3cf98796d65f9ae21550d27b45cf4df94.
Budget: at most 150 calls. Plan: verify five findings, five separate commits,
one offline/build checkpoint, deploy one lane install, requested domain proofs.

Completed: five finding commits (P1-a e2a83830b, P1-b de10435ac,
P2-c 4c2be3b3b, P2-a 022d03da6, P2-b a158eed4f).
P1-a enforces requested ticks with zero tolerance and writes result.json.
P1-b requires all requested ticks plus infantry.tgl after tick 2404 on mc2_24.
P2-c keeps future fields; only glued alphabetic animrng console noise is stripped.
P2-a preflights actual ID membership before teardown, preserving sparse legacy IDs.
P2-b validates complete active-pool mover membership and correct side membership.
The review's exhaustive friendly/hostile partition premise is false: neutral and
teamless movers intentionally belong to neither list (objmgr_misc.cpp:18-36).

Green receipts: determinism pytest 24 tests; stress pytest 7 tests;
saved objects doctest 12 cases/45 assertions; save sections 6 cases/247 assertions.
Logs: G:/mc2-tmp/w90-{home,world,contracts,seed}.log (contracts/seed in progress).
Next: resolve checkpoint contracts/ratchets; build mc2 once; deploy own lane install
E:/mc2-installs/claude-w90-fixups; SIMID 03/10/24, continuation, overlap audit
01/03/10/16/24 and validation. Keep at most one lane install, no pushes.
