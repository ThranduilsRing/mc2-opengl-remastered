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
