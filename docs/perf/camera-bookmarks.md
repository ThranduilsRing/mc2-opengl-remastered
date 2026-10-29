# Camera envelope proofs

The camera supports elevation -20..88 degrees, altitude 10..6400 WU,
360-degree azimuth and ABL cinematics. A mid-elevation result does not cover it.
`tests/visual/bookmarks/mc2_{01,10,24}_camera.json` names ten required views per
mission: four low horizons, cinematic close, oblique default, wolfman, topdown,
orbit and the baked sun azimuth. Positions have mission-specific provenance.
Altitude is a request: retain the PNG sidecar's actual `cap_alt` when the engine
clamps it. The orbit PNG grades its starting pose; its live performance interval
covers a complete 360-degree sweep in ten wall-clock seconds.

Run `py -3 tools/perf/mission_cpu_profile.py --install <private-install>
--missions mc2_01 mc2_10 mc2_24 --out <proof-directory>` (one command line).
Each set uses one standard host perf lease and one background game launch.
Lease acquisition is bounded at twenty minutes, as required by the lane rules.
It captures every pose, restores simulation, then profiles each static pose for
1.5 seconds and the orbit for ten seconds. The 60-second mission allowance covers
the ten PNG readbacks plus these intervals. No spawns or battle orders are sent.
Static intervals omit four teleport/pipeline frames; orbit intervals retain the
moving frames. `bench_view` acknowledgements and the buffered frame evidence
delimit intervals. Tracy Frame and the benchmark frame counter share a main-loop
iteration, allowing CPU zones to be assigned to the same intervals.

Per bookmark, `profile.json` retains zone totals, p95 and worst event durations,
CPU frame mean/p95/worst, timestamp GPU span and individual GPU pass distributions.
The benchmark `cpu_frame` scope ends before the cap; the full Tracy `Frame`
distribution is in `zones` and supplies budget comparisons and the first-number table.
Nested GPU pass times must not be summed. Missing GPU instrumentation is
`unmeasured`. Profiles use live wall-clock simulation at 1920x1080. Consequently
the 4K GPU budget remains unmeasured; these numbers do not attest 4K performance.
Host CPU samples and competing process evidence determine NOISY status. All
single-run numbers remain provisional even when the sampled host is quiet.

`docs/perf-budget.json` version 2 records explicit bookmark baselines. The first
CPU baselines are NOISY; 4K GPU and 200-mover ticks remain `unmeasured`.
Old single-camera baselines remain labeled legacy and are never
used for envelope deltas. Each metric is judged on its worst required bookmark;
missing coverage or NOISY evidence is inconclusive. The HTML cockpit names the
worst view and shows each bookmark's measurements.

`scripts/run_visual.py` includes all thirty camera scenes, with one golden PNG
and sidecar per mission/bookmark, and groups each set into one capture launch.
Use its existing `--record-golden` workflow for deterministic goldens. Profiler
PNGs are review evidence only: they use wall-clock simulation and cannot be
blessed as deterministic regression goldens. They must be viewed before claiming
that the requested subject or camera angle is shown. Schema contracts validate
recipes and budgets only; they neither launch games nor bless images.
