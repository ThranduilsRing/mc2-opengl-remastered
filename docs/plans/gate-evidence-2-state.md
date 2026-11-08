# Gate evidence 2

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
Next: finish serial owning rows and the C++ ratchet, commit, one final Ninja
compile and lint, then cleanup. The final proof/status.txt records their
verdicts, proved-at SHA and cleanup without changing the proved commit.
No game launches or lane installs needed.

Tool slowness: broad test_merge_speedups hit an unrelated contract-render
selftest without a timeout; stopped our process tree and narrowed to the four
affected stage_published tests. Broad lane_merge tests likewise replaced by
the promotion cases. Those unrelated full suites are NOT claimed as proved.
