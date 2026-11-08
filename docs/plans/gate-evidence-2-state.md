# Gate evidence 2

W134 follow-up: rebased 315197b6f onto f269b312d (rebased tip 83a2210f7).
Implemented separate index/worktree/untracked archives with checked cleanup;
atomic worktree-scoped registry release; validate sim head/window/main summary
before acceptance and send expected SHA to the remote guard.
Green: four targeted script modules (slot/release/guards/laptop), 30 registry
tests, gate_farm owning row (108 tests), cxx17 ratchet and diff whitespace.
Receipts: E:/mc2-archive-2026-10/proofs/gate-evidence-2/w134/.
Next: commit, one final-tip lint, cleanup and hand-back. The proof folder records
the final tip and lint result without modifying the proved commit. Python only;
no build/deploy/game required for this follow-up. Prior receipts below are historical.

Assignment: slot05, claude/gate-evidence-2. Actual starting tip eeb7edae014
(the assignment's 609d072 base was superseded by its sim-head-guard prerequisite).

Implemented: promotion consumes six explicit sim PASS rows and binds candidate,
sim manifest identity and pulled payload SHA; comparison base fails closed;
movement paths are sim sensitive; compatibility diffs promote existing mods;
capture payload bytes are verified and identities persist for compare-only.
6ac519b25 already releases every matching slot registry claim and warns on errors;
item 5 deliberately skipped, no lane_slot changes.

Green receipts under E:/mc2-archive-2026-10/proofs/gate-evidence-2:
targeted-tests-final.log (four modules), staging-tests.log (four affected tests),
laptop-sim-dry-run.log (six commands), dry-promote-final.log (12 cases including
valid evidence and eleven refusals). Doc links and diff whitespace are green.
The prior proof/status.txt records that turn's green build and lint at 315197b6f.
No game launches or lane installs needed.

Tool slowness: broad test_merge_speedups hit an unrelated contract-render
selftest without a timeout; stopped our process tree and narrowed to the four
affected stage_published tests. Broad lane_merge tests likewise replaced by
the promotion cases. Those unrelated full suites are NOT claimed as proved.
