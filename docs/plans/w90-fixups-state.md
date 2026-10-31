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
Final receipts: all five ratchets pass without baseline changes; sim_determinism,
saveload_roundtrip and save_inspect contract rows pass. Gate/stress/audit pytest:
42 tests pass. This base has no scripts_tests or stress_overlap contract row;
the existing direct pytest suites cover those scripts, with no added harness.
RelWithDebInfo mc2 build passes. Shared-header invalidation scheduled 272 Ninja
actions (~7 minutes): principal tooling cost. A first deploy correctly refused
the stale asset viewer; building mc2_asset_viewer satisfied that check. Final
deploy COMPLETE and check-only pass; installed game SHA equals the build.

Live proofs (20-second SIMID budgets): 03/10/24 baseline chains unchanged;
common frames 1001/5167/2601. mc2_24 result.json records requested=2600,
tolerance=0, complete=true; the stress parser confirms the infantry marker after
tick 2404 in both runs. Continuation 03/10/24: 60 post-load frames identical.
Overlap audit=1 clean on 01/03/10/16/24; windowed Vulkan validation errors=0.
No new gates, raised baselines, or owned live children. Save backups restored.

Proofs: E:/mc2-archive-2026-10/proofs/w90-fixups. Only verified-*.log and
smokes-verified/smokes-continuation are valid game receipts; earlier unprefixed
game logs came from attempts after the refused deploy and do not count.
Disk: no extra installs created/deleted (0 GB freed); leave only our
E:/mc2-installs/claude-w90-fixups install. No pushes. Work complete; no next steps.
