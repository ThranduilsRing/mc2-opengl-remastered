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
Main baseline reproduction is running via f10-baseline.py; retains its one
temporary mod stage for the subsequent candidate run and fixture extraction.
Next: finish baseline, capture actual type125 tick100 building as fixture,
run nine class tests plus covering offline rows; build/deploy committed
candidate; validate legacy stamps and pinned speed before sequential full
acceptance (first-save 01/03/10/24, W87/W90 loads, continuation, SIMID,
audit/validation, CVE-G main-produced quickload). Remove mod staging at end.
