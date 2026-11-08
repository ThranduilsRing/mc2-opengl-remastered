# Slot keyed installs (2026-11-08)

Assignment: mc2-slot02, claude/slot-keyed-installs; actual task base c230ada02.
Plan: retain PC installs by slot, reuse box src/build/install by slot, distinguish
absent pueue from unreadable pueue, preserve branch/SHA proof identity.
Implemented: PC acquire records/prints pc-lane-NN; GC retains it unless disk is
low and never removes claimed slots. Box slot paths retain src/build/install,
branch/SHA stay in logs and receipts; dry-run is offline. Missing pueue permits
cleanup; unreadable pueue refuses; live GPU owners protect their lane. Daily
merged-branch cleanup retains fixed box slots (disk eviction still applies).
Updated the box-run sentence in A:/Games/mc2-ops/lane-rules.md section 5 (external
ops document, outside this commit).
First live box build: c230ada02, box-lane-02, PASS, 47.5 s wall / 17 s build;
evidence E:/mc2-archive-2026-10/proofs/slot-keyed-installs-box-first.log and
proofs/box/mc2-slot02/c230ada0 under that archive.
Next: final-tip targeted pytest, gate_farm row, lint, Ninja, second box build.
Final evidence uses E:/mc2-archive-2026-10/proofs/slot-keyed-installs-*;
update the external proof note after proof without moving the proved tip.
No covering native doctest exists for these Python/Bash changes.
No engine/runtime changes; no PC deployment or game smoke needed for tooling.
Call budget: <=150; stop and commit at 120 if scope remains.
