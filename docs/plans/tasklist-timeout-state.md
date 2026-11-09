# tasklist-timeout

Assignment: slot10 / claude/tasklist-timeout; requested base f269b312d.
Starting HEAD was 8fc0c6c89 (contains requested base and Linux build fix).

Implemented: trim sccache base-directory whitespace; bound all unbounded
tasklist calls in scripts/tools to 15 seconds; shorten lint pin inventory
to 15 seconds. Cache restart defers on inventory timeout or launch failure.
Real PC sccache 0.18.0 stats captured as a regression fixture.
Snapshot reuse rejected: existing helper conflates failure with no processes,
which cannot safely authorize a shared cache restart.

Next: focused pytest, owning contract row, cxx17 check, local commit,
final-tip lint and timed Ninja no-op build. No deploy/install/game needed.
