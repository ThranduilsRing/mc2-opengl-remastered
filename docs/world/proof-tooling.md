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
