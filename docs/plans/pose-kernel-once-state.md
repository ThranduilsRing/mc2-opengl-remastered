# pose-kernel-once state

Assigned slot03 / claude/pose-kernel-once; actual base 8fc0c6c895.
Implemented memoized parent-first serial pose evaluation, retaining multiplication
grouping, cached revision semantics, and immediate canonical publication.
Added evaluated-once/inverse counters and old-recursion negative control;
immutable pose bits and serial/parallel cases cover 0/1/3 workers.

Inverse audit: the only TransformSimPose callers are the private simulation
shapes in mech3d_geometry.cpp and gvactor_update.cpp. Anchor queries consume
shapeToWorld; inverse readers are CpuDraw/preview_draw.cpp on presentation
shapes. CopyImportedActorPose copies importer animation state, not TG matrices.
Production therefore skips all simulation inverses; the generic kernel retains
per-node inverse flags and tests both paths. BindMaterials remains untouched.

Initial proof at 016ce7dab: production compile, lint, smoke-lite (day/night),
headless SIMID baseline equality mc2_01/03/10 passed. Receipts are under
E:/mc2-archive-2026-10/proofs/box/mc2-slot03/016ce7da.
Unit compilation found a single-argument CAPTURE macro; corrected.
Contract regex lost its quotes through PowerShell; use literal single quotes.
mc2_24 harness rejected overlapping --env/--env-b keys; use --env-a/--env-b.
Pending: final-tip compile, targeted unit cases, corrected contracts and mc2_24
worker pair, plus final-tip runtime receipts to satisfy the assigned SHA rule.
Final receipts/state and box cleanup remain.
No PC install or performance measurement created. Scout timed out; its orphaned
child PIDs were identified by their slot03 command line and stopped.
