Assignment: A:/Games/mc2-lane9, claude/cxx23-packet-b, base 32f03ddbe.
Entry aa4c918f2 retained. F10 acceptance FAIL: 31/32 receipts green.
Reapplied preserved flip as c52ceb0ff and exact 140bce296 baseline bytes as
18e59ac01. Roll back those two checkpoints if acceptance is not all green.

Root cause: runtime TerrainObject::numCellsCovered is a byte, with a dynamic
array of row/column pairs. The legacy record has only 81 shorts (40 pairs).
CopyTo checks >=162 cells, then copies count*2 shorts into that fixed array.
The tagged terrain validator correctly prevented that overread, but Building
needs a counted dynamic sequence for mod footprints, not a wider fixed ABI.
Retained codec fix gives BuildingRecord owned overflow cells. The candidate
runtime bridge was tested, then reverted with the flip after acceptance
failed; engine save/load sources now exactly equal entry aa4c918f2.

Proof root: E:/mc2-archive-2026-10/proofs/lane9/cxx23-f, f10-*.
Main c0eba1a0a, SHA256 2747d4e73e8e6d2a110b0b4e9e35b56cebc77129e7a389b4795fba3554e498c7,
reproduces metropolitan quicksave abort at tick100 with the same type125
capacity diagnostic (without terrain_cells or tagged writer). It is not a
valid flip acceptance test. Main CVE-G mc2_01 smoke passes and its native
save is readable, MC2SAVE.V2, SHA256 31114eee77b3ad62985cc1e5d258de07fa62bc8cdb1dd0e81b453c510ab4767f.
The baseline copy step reported a Windows CopyFile2 FileNotFound error after
the destination appeared; the destination parses through END and its stamp,
mission FIT and hash are pinned. The planted candidate load must prove it.
Green: four covering contract rows; 37 codec cases / 526 assertions.
f10-pins-result.json validates eight W87/W90 stamps and four tick/speed pins:
01=1, 03=2, 10=2, 24=1. Initial main recipe did not alter any baseline.
Candidate engine checkpoint 369753936a55; build fingerprint dirty=0.
One avoidable cost: default unit helper selected cold VS tree (>5 minutes);
stopped only its own process tree, reused build-cc-tests (44 seconds green).
All-target checkpoint PASS (455s), engine + viewer PASS (473s); -j6 with
Idle/tool-core parent, preserved trees. No engine code changed after build.
Initial deploy refused shared payload hardlinks: the retained baseline mod
stage referenced the lane install. Nothing was written; refusal preserved in
f10-deploy-refused-shared-baseline.log. Removed that owned temporary run
directory before deployment; the unchanged hardlink guard then passed.
No sharing waiver, skipped check, manual payload copy or guard bypass used.

Completion monitor f10-complete.py finished the build checkpoint, then runs
f10-capture.py PASS for fixture capture: all 100 pre-save F9 SIMID ticks
match exactly. Type125 slot1690 has 198 cells / 396 shorts, exceeding the
81-short native array. The 38,465-byte fixture retains all 77 type125 slots
(75 empty, one two-cell, one 198-cell); hash/provenance in fixture README.
Metropolitan engine now writes the save and exits summary=pass (15s); the
mod wrapper returns 1 for the same three weapon-node warnings seen on main.
This known native-save-debt mission is excluded from acceptance as directed.
Nine class + codec tests PASS: 46 cases, 2704 assertions (36s).
First-save mc2_01 PASS: zero-masked-bytes-v1 digest 1ca40fd86155518a;
no baseline or mask change. Its game report PASS, 2944 frames, host lease
wait 0.0s. End-to-end harness took 405s (includes about 80s waiting for the
save lock): tool slowness, not an engine timeout. Farm queue depth checked
0; no farm job submitted and no host startup timeout occurred.
Fixture/tests committed as f77c8e1ce. Full sequential acceptance finished:
four first-save checks, eight legacy loads (601 post-load ticks each), four
continuations, three SIMID pairs, five overlap audits and five zero-error
Vulkan validation witnesses PASS. Clean candidate fingerprint 369753936a55.
Exact failure: CVE main-produced native save quickload logs
`[SAVE v2] event=reject reason=unknown_required_section` after load_begin.
Smoke exits 0, but there is no rewind and zero post-load ticks; the required
60-tick comparison FAILS. f10-accept-result.json is the authoritative FAIL
receipt (31/32), not the outer monitor's successful process exit.
Baseline migration reverted in 1c69bcd35; flip and runtime bridge reverted in
14ef17ee7. Remaining source delta is two private codec headers plus fixture
and tests; no migration, mask change, runtime cutover or native layout change.
Temporary mod staging removed: 11,448,138,862 logical bytes. Sole install is
E:/mc2-installs/claude-cxx23-packet-f (contains rejected candidate 369753936a55,
not the final branch runtime). All owned jobs ended; host lease waits 0.0s.
Next: inspect the main-produced save's required-section compatibility with
this lane before scheduling another CVE quickload. Reapply the preserved
runtime bridge/flip and exact migration only for a new acceptance attempt.
Final rollback proof PASS: 58 cases / 2744 assertions, 34s, including all nine
real class fixtures, extended Building cells and restored loader guard seams.
Receipt: f10-rollback-unit.log. git diff --check PASS. No pushes or other
worktree edits. Largest avoidable cost: cold VS unit helper (>5min); use
--compile-cache to reuse Ninja (34-44s). First-save wrapper slowness: 405s
and 324s for 01/03; their game runs passed, with about 80s save-lock wait on 01.
