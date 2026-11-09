# m3-intro-spawn state

Assignment: slot04 / claude/m3-intro-spawn / base 8fc0c6c8957673ab0be789403a10b91b852c59b2.

Inspected: camera goal interpolation already runs once per render frame. GameOS rounds
the delta to integer milliseconds; zero becomes frameRate=0 and a 250 ms camera step.
Spawns precede terrain loading; no spawn-footprint contact pass exists. Stock tree
contact sets FALLING and clears tangibility, and normal static contacts search only
the mover's object block.

Implemented at 19ca85459 (clock, diagnostics, initial clear) and fd3a23891
(campaign addMover clear). Both spawn paths use the stock tree collision handler.
The campaign path does no presentation work on the simulation worker.

Green: cxx17 ratchet; static_prop_family contract; box engine build at 19ca85459.
Box unit/smoke/headless determinism/lint at 19ca85459 still running. Windows build
is queued for host tokens; it has not compiled yet. The final source includes
fd3a23891 and must be proved after the first box job completes.

PC baseline on retained E:/mc2-installs/pc-lane-04 timed out during load (270.5 s,
67.9 s lease wait, no gameplay samples). Its pre-existing exe is f269b312d832,
not the assignment base; fingerprint mismatch is recorded. Do not call this a
valid before distribution. Artifact: tests/smoke/artifacts/2026-11-08T18-40-32-870417-4716.
Scout produced no result and was stopped. The original mc2_03 ABL uses timed
camera goals, not per-tick hard positions. Quantized time is a confirmed defect,
but the owner's reported intro jank root cause remains unproven without samples.

Next: final-tip box build/unit/smoke-lite/determinism:mc2_03/lint;
finish the Windows build, checking embedded identity against final HEAD;
deploy to retained pc-lane-04; PC intro log and spawn capture.
Archive evidence under E:/mc2-archive-2026-10/proofs/m3-intro-spawn. Clean own scratch.
