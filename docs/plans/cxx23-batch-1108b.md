# C++23 batch 1108b state

Assignment: A:/Games/mc2-slot10, claude/cxx23-batch-1108b.
Clean starting HEAD: 8ab1d9855a1117ad8cb77069e9b8edd834d08d33, the requested previous batch.

The requested report found LG08=20, LG09=18, LG10=43, LG13=34 tree-wide.
After protected files, function-pointer false positives, coupled min/max updates,
and required platform/preprocessor macros, 150-250 sites are unavailable in two
mechanical rules. Continue LG08/LG09 without changing APIs or ownership.

Converted 17 ratchet sites (LG08 20 -> 10; LG09 18 -> 11), including
allocator diagnostic pairs and casts, stale-light diagnostic insertion,
HDRI dimension access, and the multiplayer UI's nested cast. No simulation state,
layout, comparator ordering, pointer identity, or destination cast type changes.
The disabled animation-cadence guard is left alone. Protected files are untouched.

Ratchet --check --worktree --tighten PASS; only affected baseline counts retained.
Whitespace check uses cr-at-eol because mpparameterscreen.cpp is tracked CRLF.
Next: commit source plus baseline together; run Windows Ninja once with the
existing MC2_ALLOC_STATS option enabled, then box build/unit/lint at that tip.
No SIMID, game run, deploy or PC install is required for these mechanical edits.
Allocator proof code remains behind its existing MC2_ALLOC_STATS build option;
the Windows compile explicitly includes that optional diagnostic implementation.
Receipts: E:/mc2-archive-2026-10/proofs/box/mc2-slot10/<tip8>/.
