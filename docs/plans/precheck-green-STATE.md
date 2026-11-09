# precheck-green

Symptom: "the shared merge precheck ... is red on nifty itself".
Assigned base: b26fc9bfe90a94a60a163d3bc5e2399ab5cbd16d.
Worktree: A:/Games/mc2-slot04, branch claude/precheck-green.

Evidence: E:/mc2-archive-2026-10/proofs/precheck-green/.
Baseline precheck still running (Python suite shell fixtures); other rows PASS,
tools_tests FAIL. Full baseline named-module run also awaiting shell fixtures.
before-fast.log: 15 failures; before-path.log: path selftest failure;
before-campaign.log: campaign fixture missing czstring include.
after-fast.log: 190 PASS (all corrected fast cases plus campaign fixture).
before-timeout.log: 2 new regression tests FAIL; after-timeout.log: 6 PASS.

Causes/fixes:
- capture: sibling ab import failed when imported as a driver; resolve sibling
  directory and mock install identity in backend-only unit fixtures.
- save inspector: nested layout records are not standalone save packets.
- determinism: both streams now check the baseline, so expect two checks (8 cases).
- GPU_SPLIT: unknown keys intentionally fail closed; assert rejection separately.
- nightly: missing tier denotes a legacy verdict; modern tier=0 is refused.
- resume-log and publish-crash fixtures need an explicit comparison base.
- campaign fixture needs the production czstring alias header.
- path guard predates VFS lookup funnel; recognize VFS and pin its six read opens.
- stock-copy fixture uses a permitted-drive fixture even when pytest defaults to C:.
- shell render selftests ignored -jN because pool setup was in the real-board arm.
- timeout retries repeated hangs; stop at first rc=124 and skip diagnostic rerun.
Shell fixtures have 120s markers using the existing watchdog (child-tree cleanup).

Next: finish baseline receipts; full scripts suite including direct-owned modules,
owning contracts, final ratchet/lint/precheck at committed tip; cleanup/release.
Offline only. No engine build or deploy needed for Python/test changes.
