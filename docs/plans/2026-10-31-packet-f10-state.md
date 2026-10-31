Assignment: A:/Games/mc2-lane9, claude/cxx23-packet-b, base 32f03ddbe.
Entry aa4c918f2 retained. F10 in progress; no acceptance verdict yet.
Reapplied preserved flip as c52ceb0ff and exact 140bce296 baseline bytes as
18e59ac01. Roll back those two checkpoints if acceptance is not all green.

Root cause: runtime TerrainObject::numCellsCovered is a byte, with a dynamic
array of row/column pairs. The legacy record has only 81 shorts (40 pairs).
CopyTo checks >=162 cells, then copies count*2 shorts into that fixed array.
The tagged terrain validator correctly prevented that overread, but Building
needs a counted dynamic sequence for mod footprints, not a wider fixed ABI.
Current fix gives BuildingRecord owned overflow cells and passes them through
the building save/load path; stock tagged bytes and native ABI stay exact.

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
Next f10-accept.py (32 sequential receipts: nine classes, first-save checks,
eight legacy loads, continuation, SIMID, audit/validation, planted CVE load).
Next: inspect completion receipt, commit fixture/test assertions if green;
otherwise revert flip/migration and retain only independently green codec
work. Remove temporary f10 mod staging; sole permanent lane install remains
E:/mc2-installs/claude-cxx23-packet-f. No pushes, no other worktree edits.
