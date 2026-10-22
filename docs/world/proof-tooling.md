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
