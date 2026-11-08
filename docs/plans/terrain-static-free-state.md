# terrain-static-free

Assigned slot08 / claude/terrain-static-free, base 8fc0c6c89.
Step 1: added disjoint building full-presentation-update reason counters to the
existing PERF_COUNTERS receipt. The census build passed at 452f25972; the initial
custom smoke rows failed before game launch because the child shell did not
inherit `$I`. Corrected rows use `../install/mc2`.

Fixed one definite false rejection: stopActivity kills its effects but retains
their handles for reuse. Only running activity requires the full Execute pass;
retained stopped handles must not disqualify otherwise-static geometry. Both
eligibility and the census use the extracted predicate. New doctest
`building_static_eligibility/stopped effects do not keep geometry dynamic`
covers stopped, restarted and absent effects (8 assertions).

Next: read the corrected mc2_10/mc2_24 census, then final box build/unit,
covering static/light/MVP rows, lint and smoke-lite. Evidence lives under
E:/mc2-archive-2026-10/proofs/box/mc2-slot08/<commit-prefix>/.
Step 2 (dirty queue and shadow walk) remains deferred. No sim tick order changes.
Do not restore the old Building::update touch gate suggested by scout: the
unconditional update now advances simulation animation/pose and is intentional.
One box lane install exists; no PC installs or copied executables were created.
