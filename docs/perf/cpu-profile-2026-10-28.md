# Mission CPU profile and beacon default remeasurement

Measured 2026-10-28 CDT / 2026-10-29 UTC on the owner PC during the requested quiet window. Source: `claude/cpu-profile-tool`, `2a08308fa`; beacon install: `E:/mc2-installs/claude-nvidia-dynamic-v2`. No engine changes, builds, or deployments were needed.

## Method

- Exclusive host batch lease `cpu-profile-20261028`, TTL 1,200 seconds. All game launches use the existing offscreen, no-cursor-grab harness. Temporary files use `G:/mc2-tmp`.
- Mission profiles use the requested `mission_cpu_profile.py` command: mc2_01, mc2_03, mc2_10, mc2_24, battle; 20 seconds of gameplay per run; top 10 output. Each NOISY mission gets exactly one repeat. Both attempts remain in the proof archive.
- Zone times are inclusive elapsed scope time, including waits; nested zones overlap. Top-zone shares use total `Frame` elapsed time as the denominator, so they must not be added. Sim/render/present shares use GameLogic plus SimOverlap.Tick worker time / DrawScreen / SwapWindow; worker time overlaps present. These are scope-work shares, not exclusive wall-time fractions or scheduled CPU consumption.
- Beacon A/B uses the lane4 install with normal mission visibility, 1920x1080, uncapped FPS, vsync off, wall-clock simulation, and the stock camera. `set_light night` selects mc2_10 night; `set_light day` selects mc2_24 day. The command channel arms the existing `bench_output` producer without spawning a battle fixture.
- Each mission alternates default, `MC2_FORCE_DYNAMIC_BUILDINGS=1`, three times. Default leaves that variable unset. CPU frame measurements cover the existing benchmark scope through present, before the frame cap; percentiles use the benchmark's nearest-rank estimator. The first frame that arms the producer has no valid frame-start timestamp and is naturally omitted. No additional warmup frames are discarded.
- Per-run host-load samples and command acknowledgements are retained. External host CPU p95 above 25% marks a run NOISY; such measurements remain provisional even under an exclusive game lease.

## Results

Profile executable SHA256: `341c0be3ad63f419a38aa5f056f98d2d960a28f2dc67da14e89e53093c597235`.

Initial evidence: `E:\mc2-archive-2026-10\proofs\cpu-profile\20261029T031907.027302Z`. Repeat evidence: `E:\mc2-archive-2026-10\proofs\cpu-profile\reruns\20261029T032436.918480Z`.

### Initial and repeat host load

| Mission | Attempt | Noise | Host CPU p95 % | External CPU p95 % | Frame mean / p95 ms | Game-run seconds | Lease wait seconds |
|---|---|---|---:|---:|---|---:|---:|
| mc2_01 | initial | NOISY | 53.1 | 47.2 | 1.657 / 2.294 | 33.9 | 0.01 |
| mc2_03 | initial | NOISY | 64.4 | 58.9 | 1.706 / 2.743 | 51.4 | 0.00 |
| mc2_10 | initial | NOISY | 51.8 | 49.7 | 5.893 / 11.765 | 40.2 | 0.00 |
| mc2_24 | initial | NOISY | 39.7 | 33.1 | 2.051 / 2.888 | 47.9 | 0.00 |
| battle | initial | NOISY | 39.6 | 39.6 | 10.515 / 13.178 | 42.3 | 0.00 |
| mc2_01 | repeat | NOISY | 33.6 | 27.9 | 1.644 / 2.225 | 39.3 | 0.01 |
| mc2_03 | repeat | NOISY | 45.9 | 39.5 | 1.592 / 2.523 | 40.4 | 0.00 |
| mc2_10 | repeat | NOISY | 26.8 | 25.7 | 5.800 / 11.635 | 37.6 | 0.00 |
| mc2_24 | repeat | NOISY | 34.2 | 27.7 | 1.941 / 2.672 | 41.7 | 0.00 |
| battle | repeat | quiet | 29.2 | 23.7 | 10.426 / 12.943 | 41.9 | 0.00 |

### Five largest zones per mission

The root Frame scope is excluded from the ranking. Each share is the inclusive zone total divided by Frame total. ms/frame divides the zone total by complete Frame count, rather than by zone-call count.

| Mission | Zone | Total ms | ms/frame | Frame share | Event p95 ms |
|---|---|---:|---:|---:|---:|
| mc2_01 | DrawScreen | 13451.423 | 1.105 | 67.2% | 1.671 |
| mc2_01 | Camera.UpdateRenderers | 7040.462 | 0.578 | 35.2% | 0.652 |
| mc2_01 | UpdateRenderers | 7010.024 | 0.576 | 35.0% | 0.649 |
| mc2_01 | UpdateRenderers missionRender | 6993.842 | 0.574 | 34.9% | 0.647 |
| mc2_01 | Camera.UpdateRenderers Mission.render | 6991.756 | 0.574 | 34.9% | 0.647 |
| mc2_03 | DrawScreen | 11737.292 | 0.933 | 58.6% | 1.597 |
| mc2_03 | Camera.UpdateRenderers | 9772.670 | 0.777 | 48.8% | 1.052 |
| mc2_03 | UpdateRenderers | 9755.136 | 0.776 | 48.7% | 1.050 |
| mc2_03 | UpdateRenderers missionRender | 9738.923 | 0.774 | 48.7% | 1.048 |
| mc2_03 | Camera.UpdateRenderers Mission.render | 9736.585 | 0.774 | 48.6% | 1.048 |
| mc2_10 | DrawScreen | 15737.951 | 4.558 | 78.6% | 9.595 |
| mc2_10 | Render.PostProcess | 10653.468 | 3.085 | 53.2% | 6.561 |
| mc2_10 | Camera.UpdateRenderers | 5045.690 | 1.461 | 25.2% | 3.459 |
| mc2_10 | UpdateRenderers | 5041.337 | 1.460 | 25.2% | 3.458 |
| mc2_10 | UpdateRenderers missionRender | 5036.671 | 1.459 | 25.2% | 3.457 |
| mc2_24 | DrawScreen | 10022.352 | 0.972 | 50.1% | 1.037 |
| mc2_24 | Camera.UpdateRenderers | 8964.668 | 0.870 | 44.8% | 0.931 |
| mc2_24 | UpdateRenderers | 8897.725 | 0.863 | 44.5% | 0.925 |
| mc2_24 | UpdateRenderers missionRender | 8881.842 | 0.862 | 44.4% | 0.923 |
| mc2_24 | Camera.UpdateRenderers Mission.render | 8880.030 | 0.861 | 44.4% | 0.923 |
| battle | DrawScreen | 11318.646 | 5.874 | 56.3% | 7.081 |
| battle | Camera.UpdateRenderers | 10953.144 | 5.684 | 54.5% | 6.900 |
| battle | UpdateRenderers | 10912.038 | 5.663 | 54.3% | 6.876 |
| battle | UpdateRenderers missionRender | 10908.458 | 5.661 | 54.3% | 6.874 |
| battle | Camera.UpdateRenderers Mission.render | 10908.095 | 5.661 | 54.3% | 6.874 |

### Sim/render/present split

| Mission | Sim ms/frame | Render ms/frame | Present ms/frame | S/R/P share % |
|---|---:|---:|---:|---|
| mc2_01 | 0.406 | 1.105 | 0.194 | 23.8 / 64.8 / 11.4 |
| mc2_03 | 0.593 | 0.933 | 0.159 | 35.2 / 55.4 / 9.5 |
| mc2_10 | 1.540 | 4.558 | 0.209 | 24.4 / 72.3 / 3.3 |
| mc2_24 | 0.892 | 0.972 | 0.316 | 40.9 / 44.6 / 14.5 |
| battle | 6.054 | 5.874 | 0.988 | 46.9 / 45.5 / 7.7 |

Worst by Frame event p95: **battle**, 12.943 ms. The quiet battle repeat spends 5.874 ms/frame in DrawScreen, including 2.957 ms/frame in VKIsl.Vfx.SlotWait (28.4% of Frame elapsed), plus 5.039 ms/frame in GameLogic and 1.015 ms/frame in overlapping worker ticks. This is a mixture of render-slot waiting and game update/presentation, not evidence of 12.943 ms of scheduled CPU computation. mc2_10 is next at 11.635 ms Frame p95; its Render.PostProcess accounts for about 3.09 ms/frame (53.2% of Frame elapsed). Inclusive parent and child scopes must not be counted as independent costs.

### Beacon default versus forced dynamic

Beacon executable SHA256: `9e51259659b451ae060e84e07cad984017ee172d9385fe00025ec9bfcc89b1b3`. Evidence: `E:\mc2-archive-2026-10\proofs\cpu-profile\beacon-20261029`.

| Run | CPU frame p50 / p95 / p99 ms | Noise | Host / external CPU p95 % | Seconds |
|---|---|---|---|---:|
| mc2_10-night-1-default | 4.545 / 11.177 / 14.101 | NOISY | 29.2 / 26.3 | 49.3 |
| mc2_10-night-1-forced | 4.689 / 11.785 / 14.678 | quiet | 30.0 / 23.8 | 36.6 |
| mc2_10-night-2-default | 4.638 / 11.411 / 14.030 | quiet | 24.2 / 20.1 | 36.5 |
| mc2_10-night-2-forced | 4.689 / 11.562 / 14.522 | NOISY | 32.0 / 26.0 | 36.7 |
| mc2_10-night-3-default | 4.626 / 11.323 / 13.969 | NOISY | 42.5 / 37.0 | 37.3 |
| mc2_10-night-3-forced | 4.693 / 11.611 / 14.633 | NOISY | 46.1 / 41.3 | 38.2 |
| mc2_24-day-1-default | 1.680 / 1.855 / 2.124 | quiet | 25.0 / 19.9 | 46.8 |
| mc2_24-day-1-forced | 1.691 / 1.905 / 2.180 | NOISY | 36.8 / 30.7 | 40.8 |
| mc2_24-day-2-default | 1.697 / 1.871 / 2.095 | quiet | 27.4 / 21.2 | 40.6 |
| mc2_24-day-2-forced | 1.685 / 1.841 / 2.037 | quiet | 26.3 / 20.1 | 40.7 |
| mc2_24-day-3-default | 1.694 / 1.852 / 2.049 | quiet | 27.3 / 24.4 | 41.1 |
| mc2_24-day-3-forced | 1.694 / 1.923 / 2.309 | NOISY | 35.7 / 30.4 | 40.7 |

Means of the three run percentiles (no pooling); p95 spread is (max-min)/mean. Paired p95 changes are forced minus default.

| Mission | Arm | Mean p50 / p95 / p99 ms | p95 min-max ms | p95 spread % |
|---|---|---|---|---:|
| mc2_10 | default (3 runs) | 4.603 / 11.304 / 14.033 | 11.177-11.411 | 2.1 |
| mc2_10 | forced (3 runs) | 4.690 / 11.653 / 14.611 | 11.562-11.785 | 1.9 |
| mc2_24 | default (3 runs) | 1.690 / 1.859 / 2.089 | 1.852-1.871 | 1.1 |
| mc2_24 | forced (3 runs) | 1.690 / 1.889 / 2.175 | 1.841-1.923 | 4.4 |

mc2_10 paired p95 changes: +0.609 ms (+5.4%), +0.151 ms (+1.3%), +0.287 ms (+2.5%).

mc2_24 paired p95 changes: +0.050 ms (+2.7%), -0.031 ms (-1.7%), +0.071 ms (+3.8%).

All runs retain the command acknowledgement and exact requested overrides. NOISY beacon runs limit the A/B conclusion; an exclusive game lease does not eliminate other host CPU work.

**Verdict:** retain the beacon default. Forced dynamic has no demonstrated CPU-frame advantage: its mean run p95 is 3.1% higher at night and 1.6% higher by day. The night direction agrees across all three pairs, but the day pairs disagree and its forced-arm spread (4.4%) exceeds the mean difference. Six of twelve beacon runs are NOISY. Treat this as a provisional direction, not a clean acceptance result or proof that the default is materially faster. No beacon-only enabled/disabled comparison was measured.

## Completion and deviations

- Five initial mission captures plus exactly one repeat for each NOISY mission completed without profiler errors. Battle's repeat is quiet; the other four repeats remain NOISY. There are no additional mission-profile retries.
- All twelve beacon runs have passing smoke summaries and successful lighting/output acknowledgements. The forced-arm engine receipt confirms `MC2_FORCE_DYNAMIC_BUILDINGS=1`; the harness receipts confirm offscreen operation and cursor-grab suppression.
- The lease window was approximately 22:19:06–22:38:21 CDT (under 19.3 minutes including a gap). A conservative orchestration deadline released the first hold after ten beacon runs. The same batch tag was reacquired exclusively for the final two, with a 135-second TTL; the host was free at reacquisition and after completion. **The requested single continuous lease was therefore not maintained.** Both holds and all measurements fit within the original 20-minute wall-clock window.
- No build, deploy, engine edit, or install creation occurred. Only this report is committed. `git diff --check` passes. Repo preflight confirmed the branch/root and safe documentation change; its old documentation parser reported ambiguous deploy/smoke command extraction.
- Tool slowness: scout produced no answer after about four minutes and was stopped; the existing harness was used directly. PowerShell marked native stderr diagnostics as `NativeCommandError`, yielding a shell failure despite the final twelve successful run receipts; the earlier deadline exception was real and is recorded above.
- Disk freed: 0. Existing installs left in place: `E:/mc2-installs/claude-cpu-profile-tool` and `E:/mc2-installs/claude-nvidia-dynamic-v2`. No extra installs were created.

## Time and token budget

Approximate, overlapping elapsed categories; game-run time includes loading/capture, not just the requested gameplay seconds.

| Activity | Minutes | Token share |
|---|---:|---:|
| Compile/link | 0 | 0% |
| Contracts/tests (documentation check) | <0.1 | 1% |
| Game runs | 15.0 | 3% |
| Waiting on leases/other lanes | <0.1 | 0% |
| Deploy | 0 | 0% |
| Reading code/docs | ~4 | 20% |
| Tool output | ~2 | 12% |
| Thinking/editing | ~5 | 64% |

Biggest avoidable cost: host CPU noise forced five repeat captures and still left four mission results provisional. Let unrelated system/background CPU work settle before the next acceptance window; a game-exclusive lease alone is insufficient.
