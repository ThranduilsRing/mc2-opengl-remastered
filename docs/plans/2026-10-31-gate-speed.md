# Gate speed lane, 2026-10-31

Assignment: A:/Games/mc2-art3, claude/gate-speed-a, base 47631b0b4cefeb9aa21f311a2404a7e64035606b.
Run budget: <=150 tool calls. Plan: inspect (20), packet 1 (35), packet 3 (25), packet 4 (25), optional packet 8 and final proof (25); reserve 20.

Verified: gate has no preflight; mod content budgets are aggregate (CVE-G=2).
Review claim stale: current plan creates no Classic capture or saveload twins; existing tests explicitly expect none.
Deploy already consumes the lane_merge build without rebuilding. Precheck links a separate virtual-merge checkout, so that link cannot be reused as the final committed-HEAD build.

Packet 1 implemented: offline preflight (clean game/viewer embedded HEAD, camera fixtures in candidate/seed, current save mask policy, expiring known failures); mkmerge instructions; canonical day/night saved fixtures.
Known classes verified from W90 retained engine logs: CVE-G metropolitan ammo=7; REAP3 reap3m01 ammo=1/movetype=1, reap3m13 movetype=1; Carver5 mc2_01 texture=100. Accepted W89-W91, expires W92.
Targeted tests initial pass: 103 passed, 1 skipped (missing optional C++ probe). W89/W90 window-number preflight runs <1 s, correctly reject absent lane binaries; historical candidate installs were removed, so these are current-input runs, not historical replay.
Packet 1 commit: 249f84d3c. Mod contract PASS (64 passed, 1 skipped, 6.71s); background inventory stale row regenerated without raising its baseline. Preflight mask normalization fixed; 13 preflight tests pass.
Packet 3 implemented, validating: farm queue snapshot selection, 20-minute pending-job cancellation with local fallback, mc2_10 validation pair stays local, stock takes half save rows after tier1/mods. Shared quicksave lock deliberately retained: splitting workers does not parallelize unsafe shared-file writes.
Packet 3 initial test pass: 66 green, one stale expectation (Classic shader name used to force saveload; narrowed serialization selection now omits it). Expectation corrected; added split-worker proof.
Packet 4 edits underway: per-tip check only, final tighten/regeneration combined follow-up. Final HEAD still built once with default mc2+viewer targets; virtual precheck remains independent.
Then packet 3 scheduling, packet 4 batch tightening, optional packet 8, each with targeted proof and local commit.
No game runs, installs or large artifacts created yet. Historical W89/W90 preflight inputs will be checked read-only; never deploy to owner installs.
