# cpu-split-sole-game

Assignment: slot02, claude/cpu-split-sole-game, base d63c403c9715b6a664a3513b445430e722c94c10.
Implemented: sole PC lease borrows game0|game1; queue-locked grant/release updates
registered live child identities; concurrent leases retain their split reservations.
Engine launches stay BELOW_NORMAL; CPU_SPLIT grammar and compile policy unchanged.
Offline: 148 targeted tests passed (host lease, two GPU lanes, nofocus env,
lease lock order, desktop safety). Fixed a stale test expecting unknown GPU keys
to be ignored; parser already rejects them. Explicitly clear the test-wide pin
kill-switch in the existing affinity test now that the switch is honoured.
Follow-up: removed 5933c3306's ABOVE_NORMAL priority override; live transition
test checks the actual BelowNormal priority, and spawn tests cover caller flags.
Next: final targeted tests, commit, build/lint, mission_cpu_profile row, one PC
smoke-lite. Receipts/status: E:/mc2-archive-2026-10/proofs/cpu-split-sole-game/.
Only intended install: E:/mc2-installs/cpu-split-sole-game (retain for orchestration).
Remove lane-owned G:/mc2-tmp/cpu-split-* scratch after archiving receipts.
