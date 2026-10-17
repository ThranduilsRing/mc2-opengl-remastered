# S1 source include firewall

Run `py -3 scripts/check_sim_include_firewall.py`. The board invokes
`py -3 -B scripts/tests/test_sim_include_firewall.py`, which runs the same pytest
suite as `py -3 -B -m pytest scripts/tests/test_sim_include_firewall.py -q -p no:cacheprovider -p no:tmpdir`.

The design's **Include and link firewall** section requires a future explicit
CMake `Sim` target manifest, but none exists yet. This tooling-only step uses the
narrowest named gameplay scope in `sim_include_scope.json`: `ablmc2_orders.cpp`,
`mission_update.cpp`, `sim_identity.cpp`, `tacordr.cpp`, `warrior.cpp`,
`warrior_orders.cpp`, and `mclib/cmponent.cpp`. They are the named ABL command
host, tick/update path, authoritative collector, order execution, AI cadence,
and component simulation data. This is an extraction starting point, not a
claim that those seven files contain all simulation. Mixed presentation hosts
(`ablmc2_camera.cpp`, `weather.cpp`, frame/UI/order adapters, appearance and
audio implementation) remain outside this narrow TU set. Their owned headers
are checked whenever reached. Expanding this manifest belongs to extraction.

Include resolution uses the quoted include's directory, then the current
CMake common/game include directories in their declared order. Owned headers
are traversed breadth first; a forbidden dependency is recorded at the boundary
and need not be traversed further. Every owned include edge that reaches a
forbidden rule also carries a transitive finding with a shortest witness. This
rejects a new route even when another path already reached that dependency.
External/standard headers are terminal.
Missing owned inputs and unknown include operands fail with exit 2 and cannot
be baselined. All local macro definitions are considered, so a later definition
cannot hide an earlier forbidden include. Literal macros are resolved; other include
metaprogramming fails closed. Generated headers, if reached, must exist and must
be exposed by explicit include directories in the manifest; none are required
by the present seven-file closure. The current shared PCH is a synthetic include
edge for every TU except those explicitly exempted in `pch_skip.cmake`.

Rules cover camera, timing/wall clocks, GameOS, RenderCore/RenderWorld, SDL,
Windows/GPU, input/audio device access, and the banned global/CRT RNGs. Forbidden
tokens in declarations, aliases, macros and function pointers count. Explicit
extern callable declarations are conservatively forbidden as opaque shims;
clock-named extern data also counts. Comments and string/character/raw literals
are excluded. All conditional branches count, including disabled diagnostics.

This source checker does **not** claim compiler dependency, AST reference or
link-graph validation. It cannot establish the semantics of innocently named
external accessors or arbitrary token-pasting macros. Those stronger gates in
the design require the extracted target and its real compiler inputs; no Sim
target, compilation database or build is introduced here. The existing render
firewall and sim-no-audio checker remain intact under this task's file claim.

`sim_include_baseline.json` records TU, offending file, include chain, rule,
subject, normalized source evidence and duplicate occurrence. Include chains
use deterministic shortest owned paths, with a visible PCH marker. The witness
chain is descriptive, not identity: deleting a route can change a remaining
witness without creating a new violation. Each violating include edge has its
own identity, so additional routes still fail. No line
numbers are identities, so unrelated inserted comments do not churn the ledger.
Adding another occurrence, include edge or prohibited dependency produces a new
finding. Counts alone never authorize a violation.

The normal command reports additions **and removals**, failing on either.
`--rebaseline` prints the delta and accepts only removals from an existing
baseline. It refuses growth and scope changes. A missing baseline can only be
created with the same explicit option; the committed initial census is the
one-time migration capture. Extraction must tighten the baseline after each
removal, eventually reaching zero.

The pytest fixtures cover direct/transitive Camera, a timing alias, VM wall
seed, PCH leakage, and an extern clock shim, each paired with a clean twin.
Additional tests cover all forbidden header families, include-name camouflage,
aliases/pointers, macro includes, cycles, missing inputs, and CLI ratchet deltas.

## Accepted growth

2026-10-16: `code/mission_update.cpp -> GameOS/gameos/utils/mc2log.h` was added to the
baseline by hand (one `gameos` row, a growth the `--rebaseline` command refuses on purpose).
The include came in with b230be8dc (mission combat clock) and was never pinned. mc2log.h is
the logger: it includes only standard headers and takes no sim input (its clock is an
injectable rate limiter for log output), so it adds no nondeterminism, and the same TU already
carries baselined GameOS diagnostic includes (diagnostic_trace.h). It is debt like those rows:
the extraction removes it with them.
