# Hitch path 2: objective proof hold and laptop handoff

Starting candidate: fdcaf0ed5dfb09e3f275b8ce60887690fba4caca (C1+C2).
No engine changes or PC repins in this continuation. Runtime receipts are
archived under E:/mc2-archive-2026-10/proofs/hitch-path-2/continuation-20261111.

## Hard requirement remains blocked

The requested all-stock objective OLD/NEW reachability comparison is NOT
implemented and has no passing count. No actual objective-unreachable flip
has been established or excluded. Do not merge based on decoder unit tests,
an objective census, campaign autowin completion, or smoke alone.

The missing input is an objective-linked production movement oracle:

- mclib/move_globalmap_build.cpp:551 allocates a zeroed runtime path cache;
  stock MOVE packets do not contain populated packed reachability slots.
- code/mover_path.cpp:515 resolves the current mover area and candidate goal
  areas; lines 557-575 populate the cache through production calcPath and
  apply a 50000 goal-score penalty for FALSE. C1 corrects this equality for
  destination slots 1-3; it does not directly return an objective verdict.
- mclib/move_globalmap_path.cpp:325 and 413-426 use live gate callbacks,
  door openness, mover team and useClosedAreas. A static door flood-fill
  is not an equivalent replay of these decisions.
- code/objective.cpp:895-928 reads a reach radius and tests mover positions;
  target centers alone do not describe all satisfying movement endpoints.
- editor/tests/objective_census.py:273 inventories stock objectives only.
  tests/unit/test_globalmap_init_refusal.cpp:210 tests packed slots only.
  Neither supplies the missing objective-linked movement observations.

Per the lane's no-new-infrastructure rule, do not silently add a second
pathfinder or claim an all-bits proof covers stock targets. The orchestrator
must route a production-path objective oracle follow-up (including capture,
destroy, escort and radius endpoints, source mover/team, live door state,
and cache history). Run its OLD/NEW comparison for all stock missions;
any actual reachable-to-unreachable objective flip stops the lane for owner
review. Until then objective reachability is UNPROVEN.

## Orchestrator-only laptop commands

Do not submit these from the lane. First stage the C1+C2 candidate under
claude/stage-hitch-path-2 and let the normal laptop gate build/deploy it,
retaining its stock candidate. The window summary, deployed manifest and
stage ref must identify the same staged SHA. If a different window name is
assigned, replace hitch-path-2 consistently below. W149's base receipt is
not candidate proof.

```powershell
py -3 tools/laptop_gate.py hitch-path-2 --sim --sim-rows determinism-mc2_03,determinism-mc2_10,saveload-mc2_03,saveload-mc2_10 --sim-pair --dry-run
py -3 tools/laptop_gate.py hitch-path-2 --sim --sim-rows determinism-mc2_03,determinism-mc2_10,saveload-mc2_03,saveload-mc2_10 --sim-pair
```

The dry-run was inspected locally: it removes --single for both determinism
rows; 03 uses --headless-both, 10 uses --sim-speed 2. Both save rows retain
--mask tools/save_noise_mask.json --first-save-only and private save dirs.
Evidence is collected by laptop_gate under its printed window/sim folder;
require summary verdict PASS and matching staged head. This is command
validation only, not a laptop run.

The prior brief also names mc2_15. scripts/merge_gate.py:878 does not expose
a determinism-mc2_15 SIM_STAGES row; passing it to --sim-rows is invalid.
The orchestrator must run this additional pair inside its laptop queue/lease,
from the staged source checkout, with LAPTOP_CANDIDATE_INSTALL set to the
verified stock candidate and LAPTOP_PROOF_DIR to its window proof directory:

```powershell
py -3 tools/sim_determinism.py check mc2_15 --headless-both --install $env:LAPTOP_CANDIDATE_INSTALL --out "$env:LAPTOP_PROOF_DIR/determinism-mc2_15"
```

No --single and no baseline-write option are used. If candidate save pins
fail, compare with the unchanged base on that same laptop/install policy
before attribution. Both pins already failed on the unchanged PC base;
C1 matched that base on 03/10, whereas C2 changed 10. Only laptop output
may support the separately reviewed C2 repin. Never repin from PC output.

## Remaining acceptance

Campaign liveness and final smoke are separate runtime checks, to be recorded
at this continuation's committed SHA. Campaign autowin is not objective
completion proof. No C3 and no millisecond performance claim are in scope.
