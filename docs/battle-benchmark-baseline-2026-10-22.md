# First integrated battle baseline: 2026-10-22

Build `b56b340944fb60feeb91584f043dafcb3e805f42` on window 36 (`ebce7f547`), stock mc2_24, 48 force-spawned Madcats, 30-second real-time runs, two complete repetitions. RX 7900 XTX, AMD driver 26.9.2 (LLPC), native Vulkan. All 123 contracts passed. No push.

GPU per-pass numbers are the recorded baseline. CPU frame and sim tick numbers are **PROVISIONAL**: other lanes built on cores 4-7, inside the game mask. The orchestrator requested an overnight quiet rerun; no further affinity changes or runs were made.

Values below are the mean of the two run percentiles in milliseconds. GPU passes: p95; total GPU: p50/p95/p99; CPU and tick: p95/p99. Individual run distributions and spikes remain in the JSON and raw evidence.

| Metric | Day 4K | Day 1080p | Night 4K | Night 1080p |
|---|---:|---:|---:|---:|
| terrain | 1.17 | 0.33 | 4.33 | 1.23 |
| shadows | 0.37 | 0.38 | 0.45 | 0.45 |
| vegetation | 0.05 | 0.02 | 0.04 | 0.02 |
| props | 0.20 | 0.15 | 0.88 | 0.43 |
| mechs | 0.42 | 0.13 | 4.19 | 1.49 |
| water | 0.01 | 0.01 | 0.01 | 0.01 |
| post | 0.66 | 0.32 | 0.64 | 0.32 |
| gpu_frame | 4.75/6.62/7.56 | 3.85/5.92/6.77 | 7.80/13.33/13.93 | 5.51/7.35/7.93 |
| cpu_frame (PROVISIONAL) | 21.94/25.52 | 21.88/25.41 | 36.59/45.91 | 32.96/37.37 |
| sim_tick (PROVISIONAL) | 7.28/8.79 | 7.77/9.52 | 16.72/18.81 | 17.37/19.78 |
| VRAM peak MiB | 1451 | 1137 | 1451 | 1137 |
| Working set peak MiB | 2208 | 2209 | 2209 | 2209 |
| Load run1/run2 seconds | 6.375/6.838 | 6.255/6.621 | 6.220/6.846 | 6.565/6.255 |

First-process load: 6.375 s; repeated-process loads: 6.220-6.846 s. OS/driver caches were uncontrolled, so this is not an empty-cache claim. Sampled VRAM peak: 1451 MiB; OS working set peak: 2209 MiB.

Battle evidence: 118 movers, 94 mechs, 118 live units at peak; 3-5 new destructions per timed run. Every tick over 1 ms is retained; worst tick 55.244 ms. Zero catch-up bursts; current simulation is one Mission::update per render frame.

Run-to-run p95 spread: GPU 1.06-10.28%; CPU frame up to 25.76%; sim tick up to 41.22%. CPU/tick repeatability needs the overnight quiet window. Fixed-clock captures have matching simulation digests for all eight day/night bookmarks across repetitions; day and night also match. PNGs are not all byte-identical (mean absolute RGB delta below 0.084/255).

Montage viewed: tactical/close show the large battle, firing and a disabled burning mech with debris; max zoom-out frames the base; forest/concrete shows a grove, buildings and shadows. Night lights overexpose the crowded force; this remains open. Completed destructions are measured during the 30-second perf runs.

Largest measured budget overruns (CPU values PROVISIONAL):

- night-1080p cpu_frame: p95 32.963 ms vs 1.000 ms, +31.963 ms.
- day-1080p cpu_frame: p95 21.881 ms vs 1.000 ms, +20.881 ms.
- night-4k gpu_frame: p95 13.331 ms vs 5.000 ms, +8.331 ms.

GPU time is the first-to-last timestamped span; remaining scopes and raw samples are retained externally. Vegetation is nested within inclusive VFX; do not add those timings. No separate stock VRAM budget is invented.

Full JSON, Markdown, montage and raw evidence: `G:/mc2-proof/battle-bench/b56b340944fb60feeb91584f043dafcb3e805f42/`. Montage: `G:/mc2-proof/battle-bench/b56b340944fb60feeb91584f043dafcb3e805f42/montage.png`. Compact machine-readable results: [baseline JSON](battle-benchmark-baseline-2026-10-22.json).

The failed foreign SciFi hangar override and its crash log were preserved under the earlier diagnostic proof directory; that manifest was removed from the active stock install before the recorded baseline. No scratch files were committed or deleted.

Rerun the prepared install with `py -3 scripts/run_battle_bench.py`. Each new build writes under its full SHA. For another run on the same SHA, pass a fresh `--out G:/mc2-proof/battle-bench/<sha>/overnight-2026-10-23` directory. Compare using the command in [the runbook](battle-benchmark.md).

## CPU follow-up: 2026-10-23

Branch `claude/perf-cpu` starts at `8d0112c85` and merges the battle tooling with `b92e087a2` (`f7e97f21e`). Engine fixes are separate commits:

- `9a46904f5`: hierarchy-only GPU recipes stop before unused CPU per-node light-vector and projection work. Hierarchy matrices and animation state are retained. In the intermediate night Tracy capture, `TG.MultiShape.PerShapeLoop` self time fell from 52.204 to 4.895 microseconds per invocation (11.146 to 1.009 ms per frame). This was the largest measured night CPU cost.
- `4a7b95f8e`: configured music is decoded during sound initialization, with at most 256 MiB of retained PCM. Independent playback handles share PCM; cache release preserves active playback. Missing files and entries outside the budget use the existing loader. The scenario-clock script music timeline is unchanged.

The early brain hitch was traced to synchronous WAV decoding inside a native ABL call: one `mc2_01` mission-brain execution took 228.125 ms, with a nested `Audio.Stream.LoadFile` taking 222.807 ms. The original stock captures also showed mission-brain peaks of 150.272 ms at about 22.6 simulation seconds in `mc2_01` and 246.482 ms at about 38.1 seconds in `mc2_24`. Unit-brain execution and path-manager averages were much smaller; AI scheduling, path order and RNG draws were not changed.

These are **provisional shared-host wall-clock measurements**, one 90-second run per scene and build, 1080p Vulkan. The original lane baseline was sampled at 99.8% host CPU utilization; final in-run samples were 45.4% by day and 43.1% at night, with two MSBuild processes present. Aggregate differences cannot all be attributed to the fixes. Scenario SHA and all 28,577 runtime content hashes match across the three stages. Each final run had 118 movers and 94 mechs at peak, with 15 new destructions. The intermediate day run briefly reached 119 movers.

| Build / scene | CPU frame p95/p99 ms | Sim tick p95/p99 ms | Worst tick ms | First 60 simulation seconds worst ms |
|---|---:|---:|---:|---:|
| Before `5690b0175`, day | 78.841/118.556 | 30.529/53.568 | 137.132 | 137.132 |
| Before `5690b0175`, night | 109.738/191.415 | 51.131/79.912 | 201.385 | 201.385 |
| Hierarchy fix `4117ed7e0`, day | 31.027/48.237 | 10.590/16.545 | 184.730 | 184.730 |
| Hierarchy fix `4117ed7e0`, night | 79.054/265.759 | 21.590/52.749 | 422.162 | 422.162 |
| Both fixes `4a7b95f8e`, day | 21.270/25.186 | 6.926/9.048 | 24.789 | 24.789 |
| Both fixes `4a7b95f8e`, night | 24.895/29.326 | 9.807/11.807 | 18.916 | 18.916 |

The first-60-second maxima are exact: surrounding world-state samples give equal lower and upper bounds. Final mean tick time was 3.914 ms by day and 5.928 ms at night. The battle's early-spike target of 33 ms passed in these runs; an 8 ms night p95 target remains unmet by 1.807 ms. Night peak working set increased from 2,288 MiB after the hierarchy fix to 2,478 MiB with music predecode. Sound initialization now pays the decoding cost.

The benchmark retains its variable-timestep, one-mission-update-per-render-frame model. Determinism evidence uses a seeded fixed 30 Hz clock. All eight final day/night bookmark sidecars match the baseline's complete simulation identity objects, including strict digests and per-mover fields. The final day/night montage was viewed; the pre-existing crowded night overexposure remains. Tracy mission-update maxima retain the synthetic battle-fixture construction; the benchmark's tick timer starts after fixture creation. Cold render-frame waits also remain in the raw data.

Long determinism, after merging `claude/det-diverge` (`c1ff16196`, which defines the previously uninitialized skipped-blockage output): the 109.8 s divergence above was that pre-existing uninitialized value, not these commits. On the merged tip, day and night battle runs are identical over 7,842 sim frames (261 s), and the same battle with the hierarchy-lighting commit reverted (`9a46904f5`) is identical to the build with it over 8,781 frames (292 s). Music predecode touches no simulation state and was not reverted for the control.

Re-bench on the merged tip (1080p, `--runs 1/2 --duration 20|90`, processor queue 0 and 29-69% CPU at start; provisional): night sim tick p95 5.76 ms (90 s), 6.96 / 6.67 ms (20 s runs); day 5.38, 6.54, 6.70, 4.77 ms. The earlier 9.81 ms night figure was sampled at 99.8% host CPU. One 20 s night run overlapping another lane's build (queue length over 100, CPU 100%) read 43 ms and is excluded as contention. The 8 ms night target is met in quiet windows; the final acceptance bench belongs in a quiet window. Tracy attributes the remaining tick mostly to object update (about 2.3 ms with zone overhead), mech sim poses, path manager, sensors and collisions, with no single hotspot above 1 ms. Smoke-lite passes 2/2 on the lane install; an earlier lite run failed `video_floor_missing` only because the host was saturated (8 fps, 82 frames in the 10 s window), and mc2_10 passes a 30 s run (3,631 frames).

Proof root: `E:/mc2-proof/perf-cpu/`. Build: `G:/mc2-build/perf-cpu`; install: `E:/mc2-installs/mc2-win64-perf-cpu`. Each of `before/bench`, `after-hierarchy/bench` and `after-music/bench` contains `battle-bench.json`, `cpu-summary.json`, `cpu-summary.md`, raw frames, engine logs and the eight-view montage. Baseline Tracy captures cover battle day/night and stock `mc2_01`, `mc2_10`, `mc2_24`, including the first 60 simulation seconds and steady state. Raw traces, top-15 inclusive/self JSON and full CSV exports are retained per case.

Offline evidence so far: `contracts-music.log` (124/124), `unit-music.log` (3,429 passed, two skipped, including simulation/RNG replay tests), and `harness-music-production.log` (audio backend, 40 production seam cases, installed-content census and both negative controls; exit 0). Four Python contract harnesses also passed. Support commits: `c8cffe48d` (longer 1080p benchmark runs), `5690b0175` (launcher tests honor the selected Ninja tree), `551e5c5f4` (coarse audio load zone), `4117ed7e0` (generated inventory line references).
