# Game logic proof tooling

SIMID acceptance requires strict raw-bit equality as well as graded, visual,
object and ABL digests. Every observation must include n, nobj and nabl;
ticks must be consecutive. Runs compare their common prefix (at least 300
observations); baseline checks require every recorded observation.

`tools/sim_determinism.py` chain schema 2 excludes only the `rng=sim/app/wx`
draw counts. The counts remain in the log and comparison report. `chain`
still includes strict, population counts, object and ABL digests. `full`
preserves the previous full-line hash, including RNG counts. `visible_chain`
is a legacy diagnostic and can never excuse a strict mismatch.

Committed schema-1 baselines have irreversible full-line hashes, without
source observations. They still validate by exact full-line equality,
including strict and RNG counts. A count-only change therefore still fails
against an old baseline; do not silently accept or rebaseline it. Regenerate
schema 2 from the original retained log with `write-baseline` when available,
or explicitly record a new baseline with the reason in its commit. Never
derive a count-free digest from an old hash or from the candidate run.

The smoke summary's `simid_chain` remains the legacy full-line digest for
compatibility. RNG stream splitting itself belongs to D3; this packet only
separates its diagnostic counters from the graded acceptance chain.

`MC2_WORLD_ORDER_TRACE=1` is a default-OFF Trace diagnostic, registered in
the typed gate table with smoke passthrough. It buffers `[WORLD_ORDER v1]`
records for each active `Mission::update` invocation. `tick` is mission
`turn`, so repeated render frames and sim substeps cannot alias. A scope
emits `end` even on the mission brain's early return. No IDs are assigned,
lists rebuilt, RNG consumed, or world state written by the observer.

At entry to the object-manager walk (after terrain geometry), the transcript
records every ordered view, allocated mover tails, null slots, existence
bits, watch IDs without assignment, ring cursors and the dirty-collidables
flag. Terrain block records include the block activity input and each slot's
vertex-activity bit. The collidable snapshot preserves the current stored
view, even if dirty; it never calls the mutating lazy getter. Existence
transitions are observed at `GameObject::setExists`, including both FX
overrides, so direct pool activation and release cannot bypass the witness.
Mover-list mutation, mission removal and trade events retain their actual
order. Update records precede each object-manager `update()` call, including
duplicate visits. Mission scope also captures events from collisions and ABL.
Load-time creation before the first tick is represented by the initial
snapshot, rather than tick-local events. Paused mission invocations carry an
`appearance_only` marker, the same snapshot, and `appearance_*` update labels
so their refresh order remains observable without counting them as sim updates.

Compare retained harness logs with:

```
py -3 tools/world_order_diff.py a/mc2_01.log b/mc2_01.log --common-prefix --min-ticks 300
```

The diff streams one tick at a time and reports the first tick, record,
field, or slot that differs, plus bytes per compared tick. Exact length is
required by default; common-prefix mode is explicit for wall-clock runs.
Both modes reject absent observations, skipped/repeated ticks or records,
and incomplete ticks, even when both logs have the same defect. The existing
`sim_determinism` contract row runs both tools' negative controls.

## W1/W2 proof record, 2026-10-21

Tested engine commit: `8fbf183a8cd249e80b51c0b2260761d2c5b431de`,
branch `claude/gamelogic-W1W2`, clean embedded build fingerprint.
Executable SHA-256:
`21f698712e28bd6d8cc2a4cd4e7590324ffca5694890007eb5dfe330650e9719`.
This record is a subsequent documentation-only commit.

The private install is `G:/mc2-installs/mc2-win64-w1w2`, made with
`shutil.copytree` / `copy2` from the read-only source
`A:/Games/mc2-opengl/mc2-win64-v0.4`. The transient source lease was excluded:
40,413 files, 14,003,603,764 bytes copied. Every destination file was checked
for link count 1; every destination entry was checked for absence of reparse
points, both after copying and after deployment. No hardlinks were created.
The source and v0.4c were not modified. Both deployment and smoke accept
explicit paths; no named-lane restriction or shared-script change was needed.

Deployment used `scripts/deploy_payload.py` with the explicit private path,
`--target game --ninja --require-clean --allow-new-target --full-verify`.
Source and deployed executable hashes matched before each harness launch.
Each launch also passed `--verify-preflight` and the required
`DEPLOY_FINGERPRINT` gate against the tested commit.
Install data fingerprint: `9ffe5ce425438ded`, matching the committed baseline.

Offline results:

- Ninja RelWithDebInfo engine and launcher build passed with
  `MC2_BUILD_JOBS=2` and `py -3 scripts/ninja_build.py`.
- Native doctest tier: 3,358 cases passed, 2 skipped. Its 10 expected-failure
  assertions were reported as expected; overall status SUCCESS.
- Full contract board: 123 passed, 0 failed, 0 known-correct failures,
  including gates, quiet ratchet, callers and both proof-tool test suites.
  The quiet row had no default runtime log and skipped its runtime inspection.
- Focused Python proof suites: 49 tests passed, including strict-only
  divergence, absent counts, missing ticks, legacy baseline compatibility,
  counter-only changes, reordered slots, ring cursors, events and update calls.

Exactly two replacement 30-second mc2_03 harness runs were made from the
private install with deterministic RNG, fixed timestep, SIMID and WORLD_ORDER
enabled. Both smoke verdicts were PASS, with all smoke gates enabled.

| Capture | Complete ticks / SIMID frames | WORLD_ORDER bytes | Bytes/tick |
| --- | ---: | ---: | ---: |
| a | 882 | 111,961,082 | 126,940.0 |
| b | 1,922 | 244,311,097 | 127,113.0 |

The public diff tool passed over all 882 common consecutive ticks; it also
validated the longer transcript's complete tail. The average over this
identical prefix was 126,940.0 bytes/tick (about 124 KiB/tick). Wall-clock
capture lengths differ, so this is explicitly a common-prefix proof rather
than exact-length equality. Strict SIMID comparison also passed over all
882 common frames: schema-2 chain `24e7707a1c24c83b`, legacy full chain
`01e482e95ca486de` for both captures, ending RNG counts `294/44/0` for both.

Capture b passed the unchanged committed mc2_03 baseline over its first
1,000 frames, legacy full chain `3bfc7e1947c403dc`, in every digest. Capture a
correctly rejected that baseline check for insufficient length (882 frames).
No other mission baseline was run under the two-run cap. The third quiet
run was explicitly skipped by the user; no gate-OFF runtime verdict is claimed.
Two earlier starts in the superseded shared install aborted with zero mission
ticks after another owner replaced its executable; they supply no proof and
were excluded from the cap by explicit user instruction. That install was
not used for these replacement proofs.

Retained evidence under the worktree's ignored scratch directory:
`scratch/world-order-proof-isolated/{a,b}/mc2_03.log`, both `runner_*.txt`
reports, `pair.json` (including capture b's baseline verdict), and
`transcript-stats.json`. Build, unit and contract logs are respectively
`scratch/engine-final.log`, `scratch/native-final.log`, and
`scratch/contracts-green.log`; isolation and deployment provenance are in
`scratch/isolated-install.json`, `scratch/isolated-deploy-proof.json`, and
`scratch/deploy-isolated.log`.

Implementation commits, oldest first:

- `a6ce57e76` fix(simid): require strict complete observations
- `0aa4233d8` feat(proof): trace ordered world updates
- `24a685e98` test(simid): update complete observation fixtures
- `867780e90` chore(proof): refresh caller locations
- `5a490ebd8` fix(proof): observe all existence transitions
- `43f4ba206` fix(proof): bound terrain activity reads
- `777dbaf34` test(proof): register contract row ownership
- `9dac73fa8` fix(proof): preserve legacy VFX baseline checks
- `c36c4d47e` fix(proof): retain paused update observations
- `3bd7b6e81` fix(proof): decouple facade trace linkage
- `72c539173` refactor(proof): use modern trace views
- `8fbf183a8` fix(docs): check tracked Markdown only
