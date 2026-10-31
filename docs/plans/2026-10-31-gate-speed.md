# Gate speed lane, 2026-10-31

## Group B
Assignment: mc2-art3 / claude/gate-speed-b, inherited 6d480016d. Budget <=150 calls.
Plan: batch claim verification, packets 2/5/6/7 with focused tests and individual commits; final five check-only ratchets. Ops edits live in A:/Games/mc2-ops and must be committed there separately.
Verified reviewer corrections: kit_payload.finish already skips cook on verified manifest/importer/recipe identity; farm reseed already packages an installed tree (no deploy/cook). mission_cpu_profile enforces 60s for a full multi-view/orbit schedule; run_visual_capture already exits upon captures (~15s default). No ETL export in either named profiler/capture path.
Packet 6: missing-result exit trap, killed/dead-worker watcher emission, <=1.5KB delta + state JSON, bounded gate classification, pending lane-status listing. Six ops tests pass in 0.021s. Real mkmerge --continue 90 correctly refused mismatched journal HEADs; no bypass.
Packet 7: protected dry-run cleanup for branch/job siblings and labelled result paths, active LANES/budgets checks, ledger suggestion only. Dry-run: 0 dirs. No deletions/install changes.
Packet 5: default 20s single sun-facing-low view; --full-camera-sweep retains the 60s multi-view/orbit schedule. 19 profiler tests pass in 0.27s. visual_verify capture default 150 ->20s; run_visual_capture already 15s/early-exit. No ETL exporter exists in named paths (claim false); required Tracy statistics export retained. Robust A/B branch is not confirmed merged; no new statistical acceptance claims.
Packet 2: owner seed helper verifies payload and private mutable inodes, uses cached shared-data hashes, copies across A:/E: volumes; preserves verified kit recipes and avoids recook. Farm includes verified recipes and verifies every transferred file remotely. Existing cook-skip identity preserved (manifest+importer+arguments+recipe hashes, stronger than manifest-only). Tests: 53 PASS 8.51s; after recipe/hash hardening, 22 PASS 3.99s. Fixture timing in G:/mc2-tmp/gate-speed-b-seed-proof.log; not production deployment timing.
Next: packet commits, contracts + five check-only ratchets; real seed/farm/continuation receipts if inputs permit. This worktree has no built mc2.exe, so cold/warm engine deploy before/after cannot be proven here without a build. Never deploy lane changes to v0.4. Owner-test exists and must remain. Ops repo has pre-existing uncommitted/untracked tools; commit only requested tool paths and preserve unrelated files.

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
Packet 3 commit: 56cfbab8b; 58 targeted tests PASS (28.61s); farm_runner/background_launch contracts PASS (34.12s wall).
Packet 4 commit: 9248305c3; 65 lane_merge tests PASS (214.56s), added combined artifact/tightening followup test PASS (11.44s). Tool slowness: throwaway Git suite exceeds 2-minute rung; avoid full-suite repeats, use targeted tests for final packet.
Packet 8 now implementing: kit visibility nightly --full; dup_lines check-only; Classic compare already absent (no deletion needed).
Remaining: final targeted tests/contracts, five check-only ratchets, baseline/current gate dry-run wall timings, preflight receipts, diff check, packet 8 commit and final note. No engine builds/game runs/lease waits/deploys or installs created.
Final proof so far: five ratchets PASS, no baseline changes (file_size, cxx17, dup_lines, axis_swizzle, split_map). Packet 8 targeted tests 2 PASS (0.88s); final preflight/queue tests 14 PASS (0.23s).
Dry-run baseline 0.369s -> candidate 0.322s, all runtime stages 0.0 by definition; this is plan-generation timing, not evidence of runtime savings.
W89/W90-number preflight on current lane inputs: 0.03s/0.00s, both FAIL fast only on missing/stale game + viewer fingerprints, with exact rebuild command. Historical binaries/candidate installs are unavailable; historical replay and green final-HEAD executable preflight remain unproven.
Found and fixed farm pending-cancel/claim race: runner now tolerates a cancelled file after directory enumeration (2 targeted tests PASS, 0.15s). Farm host/runner/harness must be reseeded and its scheduled runner restarted before using this new queue-cancellation protocol; do not stop another active job.
Safety retained: all determinism rows, host mc2_10 validation pair, tier1 crash buckets, current-policy saveload references, mod mission coverage, reviewer montage/adversarial review. Shared quicksave lock limits packet 3's saveload speedup.

Completed all four packets. Final contracts: background_launch/farm_runner/mod_smoke 3 PASS, 9.64s wall (19 farm tests; 64 mod tests + 1 optional-probe skip). git diff --check green. No ratchet baseline changed. Conflict-specific generated outputs are still repaired inside their merge commits; full regeneration and artifact/baseline follow-up happen once per green batch.
Proof receipts: E:/mc2-archive-2026-10/proofs/gate-speed-a-*.log and gate-speed-a-dry-run-timings.json. No installs left behind, 0 GB freed. No live child processes at handoff.
Orchestrator next: review/adversarial review; merge this tip, build final clean HEAD with `py -3 scripts/ninja_build.py mc2 mc2_asset_viewer -j 6`; run `py -3 scripts/merge_gate.py --preflight --window N`, then the real gate with --window N and original window clock. Renew expired W92 known failures only with a fresh owner note. Update/restart the farm runner when idle before exercising pending cancellation. Real before/after stage savings and W89/W90 historical replay are NOT claimed by these offline receipts.
Then packet 3 scheduling, packet 4 batch tightening, optional packet 8, each with targeted proof and local commit.
No game runs, installs or large artifacts created yet. Historical W89/W90 preflight inputs will be checked read-only; never deploy to owner installs.
