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

Pending: run targeted offline rows and unit
cases, production compile, headless SIMID mc2_01/03/10/24 and box smoke-lite;
final lint, receipts and cleanup. No PC install or measurement planned.
