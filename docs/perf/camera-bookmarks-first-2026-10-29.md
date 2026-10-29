# First camera-envelope measurements, 2026-10-29

All measurements are **NOISY and provisional**. Entries are **mean / p95 / worst
milliseconds**, CPU ; GPU. CPU uses full Tracy Frame scopes, including waits;
GPU is the timestamp span, not a sum of passes. Resolution: 1920x1080,
RX 7900 XTX, Vulkan, uncapped, vsync off. Engine build `7b7f7907ad1a`
(clean claude/cxx23-finish), executable SHA-256:
`93f7b50d8367fb2e956d87f160b405c91cc173f09b261098648bc23fe48c2ef3`.

| Bookmark | mc2_01 CPU ; GPU | mc2_10 CPU ; GPU | mc2_24 CPU ; GPU |
|---|---|---|---|
| low_horizon_0 | 2.33/2.69/3.69 ; 0.95/1.36/2.03 | 3.73/5.22/10.79 ; 2.14/4.81/6.40 | 4.18/5.24/42.13 ; 2.09/3.08/5.72 |
| low_horizon_90 | 2.38/2.79/5.92 ; 0.98/1.38/3.20 | 6.42/7.98/154.37 ; 4.31/6.89/151.15 | 1.99/2.81/5.24 ; 0.88/1.70/4.08 |
| low_horizon_180 | 2.59/3.28/4.52 ; 1.10/1.96/2.67 | 3.57/4.94/9.80 ; 1.76/4.68/7.15 | 3.98/4.83/11.75 ; 1.92/2.52/4.35 |
| low_horizon_270 | 2.33/2.69/4.93 ; 0.98/1.45/2.47 | 4.61/5.56/6.91 ; 2.30/3.01/3.49 | 2.28/3.23/9.97 ; 1.04/1.89/8.96 |
| cinematic_close | 2.42/2.78/12.72 ; 1.02/1.39/2.12 | 4.61/5.33/6.47 ; 2.41/4.73/5.81 | 2.19/3.08/5.73 ; 0.98/1.79/4.44 |
| oblique_default | 2.21/2.50/4.28 ; 0.98/1.34/2.37 | 3.74/4.42/12.89 ; 1.73/2.33/9.71 | 2.21/3.03/4.92 ; 0.94/1.79/3.69 |
| wolfman | 2.45/2.92/4.91 ; 1.04/1.47/3.25 | 5.31/6.31/7.47 ; 2.69/3.59/4.20 | 2.63/3.57/8.61 ; 1.12/1.98/6.54 |
| topdown | 2.17/2.48/3.87 ; 0.93/1.27/2.43 | 3.85/4.83/5.77 ; 1.84/2.23/3.15 | 3.75/3.19/796.27 ; 1.03/1.82/3.93 |
| orbit | 2.57/3.19/23.20 ; 1.21/1.66/20.68 | 4.40/5.76/263.91 ; 2.28/3.78/261.80 | 3.06/3.26/2516.78 ; 1.01/1.96/7.40 |
| sun_facing_low | 2.82/3.40/12.98 ; 1.30/1.87/10.35 | 5.08/6.23/14.00 ; 2.59/5.19/6.87 | 3.44/3.96/5.57 ; 1.69/2.31/3.46 |

External CPU p95 was 99.69% / 99.65% / 39.56% on mc2_01 / mc2_10 / mc2_24;
host CPU p95 was 100% / 100% / 44.5%. Large maxima are observed elapsed stalls
under this load; these runs do not establish their cause. The 4K GPU budget and
isolated 200-mover tick budget remain **unmeasured**.

All 40 PNGs were viewed in four labeled contact sheets, with the day close-up
also opened at full size. The accepted thirty sidecars match requested positions,
angles and altitudes, including wolfman 6000 WU. Low cameras show land and units;
negative-elevation close-ups expose cropped mechs and flat grey foreground gaps.
High-altitude mc2_24 exposes a grey map edge. These findings are recorded without
engine changes. Orbit PNGs show the starting pose; timings cover the acknowledged
360-degree, ten-second command sweep.

The first mc2_24 fortress set at (-400,1200) was rejected because nearby walls
blocked the low views. Its replacement uses clear land 300 WU north of the player
spawn. Four authored sets received one smoke each. One replacement attempt
expired at the lease before launching a game; the normal lease wait now uses
the lane rules' twenty-minute bound. Profiler PNGs use wall-clock simulation;
deterministic per-bookmark goldens are wired through run_visual.py's record workflow.

Proof roots:

- Initial day/night and rejected fortress: E:/mc2-archive-2026-10/proofs/camera-bookmarks/20261029T215950.772543Z
- Replacement fortress: E:/mc2-archive-2026-10/proofs/camera-bookmarks/20261029T222047.918217Z
- Accepted raw profile, per-bookmark zone tables and cockpit: E:/mc2-archive-2026-10/proofs/camera-bookmarks/20261029T215950.772543Z/accepted
- Unlaunched lease attempt: E:/mc2-archive-2026-10/proofs/camera-bookmarks/20261029T221723.279483Z

Validation: 46 targeted Python tests; perf_budget_schema and
visual_regression_schema rows only; git diff --check. No C++ changes or builds.
Disk freed: 0 GB. One created install remains:
E:/mc2-installs/claude-camera-bookmarks (private mutable files; shared read-only data).

## Time and token budget

Approximate wall minutes overlap; token shares are rough.

| Work | Minutes | Token share |
|---|---:|---:|
| Compile/link | 0 | 0% |
| Contracts/tests | 1 | 3% |
| Game runs, including load/trace drain | 11.1 | 6% |
| Lease waits, including expired attempt | 6.3 | 2% |
| Install clone/verification | 17 | 2% |
| Reading code/docs | 5 | 23% |
| Tool output, including image review | 1 | 20% |
| Thinking/editing | 12 | 44% |

Biggest avoidable cost / tool slowness: the install clone walked 50,012 files and
copied 6.17 GiB, taking about 17 minutes instead of the documented seconds.
A current seed on faster G: would allow same-volume hardlink clones there.
