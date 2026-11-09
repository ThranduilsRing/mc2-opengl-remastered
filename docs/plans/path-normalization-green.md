# Path normalization green

Assignment: slot11, claude/path-normalization-green, base d63c403c9715b6a664a3513b445430e722c94c10.

Design 261297676 uses per-call inline exemptions, not a whole-file exclusion.
VFS history: 04437eb7c moved file_mod.inc into vfs.cpp; 552c62e38 moved
File::open/fileExists resolution into openRead. No runtime change is needed.

Six audited read-opens in mclib/vfs.cpp:
- 351 LoadModIndexCache: physical index-cache sidecar; exempt.
- 576 ReadModJson: physical manifest of the mod being mounted; exempt.
- 713 ModFilesEqualBytes winner: specific physical comparison input; exempt.
- 714 ModFilesEqualBytes loser: specific physical comparison input; exempt.
- 1330 resolve: readability probe of a normalized ladder candidate; exempt.
- 1345 openRead/openPhysical: File::open ladder's own physical opener; exempt.

The first four must retain physical identity; the last two implement the ladder.
Routing these through File::open would substitute overlay winners or recurse.
No raw game-data bypass was found. Future untagged opens remain ratcheted at zero.

Also updated Rule 1's explicit surface for the VFS FST implementation, moved
from file_mod.inc/file.cpp in 04437eb7c and 552c62e38. FastFileFind normalizes at
its own entry. Added a self-test proving VFS is allowed for Rule 1 while raw
opens there remain counted by Rule 2. No runtime C++ statements changed.

Done: classified all six, added inline reasons, refreshed baseline (file.cpp
4 -> 2 only). Green: nightly path_normalization row (1/1), pytest
test_path_norm_selftest (1/1), cxx17 worktree ratchet, git diff --check.
Next: commit, final-tip lint and requested build/unit/smoke proof. Box mod_smoke may require
Windows install inputs (listed as PC-only in scripts/box_contracts_pc_only.txt).
Proof receipts: E:/mc2-archive-2026-10/proofs/path-normalization-green and the
standard tools/box_run.py per-SHA receipt directory. No PC install needed.
