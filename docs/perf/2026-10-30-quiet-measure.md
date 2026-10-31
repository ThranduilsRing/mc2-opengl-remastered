# Quiet measurement: main 586327c79 versus W86 c7ef9a666

**Result: NOISY: three valid pairs were not established for every workload.** Valid pairs: mc2_01=1, mc2_03=1, mc2_10=1, mc2_24=1, battle=0. Timings with fewer than three pairs are provisional; absent accepted timings are withheld.
Completed CPU sweep attempts: 5/6; evidence exists for 6/6 arms. The forty-minute quiet-host check ran 01:01–01:41 UTC. No relocated-core capture qualified, so the owned controller and remaining game/recorder processes were stopped; normal main deployment completed successfully.
The third W86 CPU sweep was skipped by the existing game precheck, which reported this lane’s prior GPU game PID 64340. No check was bypassed. That PID was absent at final cleanup. The final main sweep completed mc2_01/03 before the bound stopped the remaining work.

Three alternating W86/main pairs were requested; accepted counts and missing work are reported below. CPU and GPU use the RX 7900 XTX; the first accepted stock pair uses logical cores 12–15 (`0xf000`); stock pairs 2–3, the later GPU attempts and 200-mover battle attempts use 8,9,12,13 (`0x3300`). Both masks are verified by the harness. Sampler affinity is recorded; its CPU is subtracted from reserved load only while it is pinned inside that set. The exclusive harness lease covers each measurement. Raw traces and invocation scripts remain outside git.

## Noise policy and evidence

External reserved-core CPU = (sum of reserved-core busy percentages − game CPU − sampler CPU) / 4. Process CPU percentages use one-core units; subtraction is valid because affinity is checked. Intervals with sampler-affinity changes are discarded, and a sampler spanning both sets is rejected. Whole-machine external CPU uses all 16 logical cores and the same exclusions. Both values are sampled every 0.5 seconds during the run. In-game reserved external p95 must be **below 15%**. NOISY captures contribute no accepted timing or paired delta. The first per-process sample primes its counter.

Proof root: `E:/mc2-archive-2026-10/proofs/quiet-measure2/reserved-core-rerun2/`. W86 and main are deployed through `deploy_payload.py --target game --ninja --require-clean` from their committed checkouts into the single lane install; no executable payload swap is used.

Cockpits are written under `cockpit-pairN-main/`. Their **previous** column is the fresh W86 arm of the same pair; the separately labelled baseline column remains the repository’s legacy budget reference. The paired deltas below use only fresh W86 captures.

| Build | Executable SHA256 |
|---|---|
| W86 c7ef9a666 | de8241d4a525ee933a32a618cf304b7a35553d0b4cd0a9e3ea5a2065e499f236 |
| Main 586327c79 | 84c45090b8a4bb288f9644e7bf0c5caf208de3b10949d3ae95f42ec17156f023 |

| Pair/build | Mission | Whole external p95 % | Reserved external p95 % | Verdict |
|---|---|---:|---:|---|
| 1/w86 | mc2_01 | 31.47 | 10.57 | quiet |
| 1/w86 | mc2_03 | 28.72 | 5.68 | quiet |
| 1/w86 | mc2_10 | 31.34 | 8.22 | quiet |
| 1/w86 | mc2_24 | 28.18 | 8.52 | quiet |
| 1/w86 | battle | unmeasured | unmeasured | corrected 200-mover capture not completed |
| 1/main | mc2_01 | 31.39 | 7.60 | quiet |
| 1/main | mc2_03 | 25.37 | 6.55 | quiet |
| 1/main | mc2_10 | 30.62 | 8.80 | quiet |
| 1/main | mc2_24 | 34.27 | 13.45 | quiet |
| 1/main | battle | unmeasured | unmeasured | corrected 200-mover capture not completed |
| 2/w86 | mc2_01 | 36.62 | 20.53 | NOISY |
| 2/w86 | mc2_03 | 45.83 | 23.68 | NOISY |
| 2/w86 | mc2_10 | 38.14 | 20.68 | NOISY |
| 2/w86 | mc2_24 | 38.46 | 22.88 | NOISY |
| 2/w86 | battle | 36.44 | 29.42 | FAILED; NOISY |
| 2/main | mc2_01 | 41.94 | 16.72 | NOISY |
| 2/main | mc2_03 | 39.96 | 20.57 | NOISY |
| 2/main | mc2_10 | 35.74 | 19.88 | NOISY |
| 2/main | mc2_24 | 33.16 | 16.90 | NOISY |
| 2/main | battle | 38.44 | 33.40 | FAILED; NOISY |
| 3/w86 | mc2_01 | unmeasured | unmeasured | SKIPPED: precheck reported prior GPU game 64340 |
| 3/w86 | mc2_03 | unmeasured | unmeasured | SKIPPED: precheck reported prior GPU game 64340 |
| 3/w86 | mc2_10 | unmeasured | unmeasured | SKIPPED: precheck reported prior GPU game 64340 |
| 3/w86 | mc2_24 | unmeasured | unmeasured | SKIPPED: precheck reported prior GPU game 64340 |
| 3/w86 | battle | unmeasured | unmeasured | SKIPPED: precheck reported prior GPU game 64340 |
| 3/main | mc2_01 | 37.71 | 19.33 | NOISY |
| 3/main | mc2_03 | 28.97 | 16.15 | NOISY |
| 3/main | mc2_10 | unmeasured | unmeasured | not completed |
| 3/main | mc2_24 | unmeasured | unmeasured | not completed |
| 3/main | battle | unmeasured | unmeasured | not completed |

## CPU results

Values are median across accepted captures with [min–max] capture spread, in ms. Frame p95 is the median of each capture’s event p95. Other columns are inclusive elapsed zone time per rendered frame; they include waits, overlap, and 0..N logic ticks per frame. Nested scopes must not be summed. The full-capture table includes the initial frozen PNG verification phase. The CPU profiler retains its existing 60-second camera envelope on mc2_01/10/24 and 20 seconds on mc2_03/battle. Live-window results follow separately.

| Mission/build | Accepted N | Frame mean | Frame p95 | GameLogic/frame | DrawScreen/frame | SwapWindow/frame | VK present/frame |
|---|---:|---:|---:|---:|---:|---:|---:|
| mc2_01/w86 | 1 | 1.620 | 1.848 | 0.571 | 0.711 | 0.238 | 0.231 |
| mc2_01/main | 1 | 1.678 | 1.918 | 0.583 | 0.769 | 0.005 | 0.574 |
| mc2_03/w86 | 1 | 1.517 | 1.886 | 0.555 | 0.758 | 0.184 | 0.151 |
| mc2_03/main | 1 | 1.511 | 1.814 | 0.566 | 0.785 | 0.005 | 0.325 |
| mc2_10/w86 | 1 | 2.307 | 3.245 | 1.036 | 1.077 | 0.244 | 0.205 |
| mc2_10/main | 1 | 2.294 | 3.205 | 1.045 | 1.100 | 0.006 | 0.564 |
| mc2_24/w86 | 1 | 3.147 | 4.258 | 1.453 | 1.406 | 0.305 | 0.294 |
| mc2_24/main | 1 | 3.306 | 4.816 | 1.573 | 1.497 | 0.005 | 0.820 |
| battle/w86 | 0 | unproven | unproven | unproven | unproven | unproven | unproven |
| battle/main | 0 | unproven | unproven | unproven | unproven | unproven | unproven |

### Live Frame timing used for camera-envelope targets

For each accepted capture, choose the live bookmarked view with the largest Frame mean and report that same view’s event p95. mc2_03 has no frozen camera sweep and uses its complete captured Frame scopes. These are view-specific distributions, not a pooled p95 across views.

| Mission/build | View | Accepted N | Frame mean ms | Frame p95 ms |
|---|---|---:|---:|---:|
| mc2_01/w86 | sun_facing_low | 1 | 1.792 | 2.060 |
| mc2_01/main | sun_facing_low | 1 | 1.794 | 1.986 |
| mc2_03/w86 | mission | 1 | 1.517 | 1.886 |
| mc2_03/main | mission | 1 | 1.511 | 1.814 |
| mc2_10/w86 | sun_facing_low | 1 | 3.505 | 5.393 |
| mc2_10/main | wolfman | 1 | 3.265 | 3.834 |
| mc2_24/w86 | low_horizon_180 | 1 | 3.882 | 4.375 |
| mc2_24/main | low_horizon_0 | 1 | 4.078 | 4.889 |

### Fresh paired full-capture Frame deltas (main − W86)

| Mission | Valid pairs | Mean delta ms |
|---|---:|---:|
| mc2_01 | 1 | 0.059 |
| mc2_03 | 1 | -0.006 |
| mc2_10 | 1 | -0.013 |
| mc2_24 | 1 | 0.158 |
| battle | 0 | unproven |

### Individual 200-mover ticks

The existing battle command file is preserved instead of being replaced by the camera driver. The run-local invocation uses the existing `MC2_MOVER_FIXTURE=200` mixed mech/vehicle fixture, with the scenario’s separate clone count set to zero. The following uses individual `battlebench::Tick` rows, not `SimOverlap.Tick` batches; a tick is accepted only after world evidence establishes 200 movers and all subsequent world samples remain 200. Mover count includes dead units; this does not prove 200 simultaneously active movers.

The original all-clone attempts hit the 128-new-mech allocation limit and had 198 movers; they are excluded and preserved in the original raw profiles. The corrected first alternating battle pair was planned under `battle200-pair1-{w86,main}/` but was not reached; the resumed sweeps attempted the mixed fixture. Derived cockpit profiles use these corrected runs. Startup ticks before the counter first reaches 200 are excluded.

The W86 mixed-fixture attempt logged `requested=200 base=70 added=130 movers=200` and sim-worker calls to the main-thread-only appearance RNG. Its capture failed before usable world/tick evidence was flushed. The resumed main battle capture also failed. These failures leave the 200-mover tick cost unproven; no engine change was made to address this separate failure.

| Build | Quiet 200-mover N | Tick mean ms | Tick p95 ms | Observed counts (all captures) |
|---|---:|---:|---:|---|
| w86 | 0 | unproven | unproven | [None, None, None] |
| main | 0 | unproven | unproven | [None, None, None] |

## Top ten inclusive CPU zones

Ranked by median total elapsed time per frame in quiet captures. Each cell is mean per-frame ms / event p95 ms. These are overlapping scopes, not independent savings.

### mc2_01

| Zone | W86 mean/p95 | Main mean/p95 |
|---|---:|---:|
| Frame | 1.620 / 1.848 | 1.678 / 1.918 |
| DrawScreen | 0.711 / 0.810 | 0.769 / 0.862 |
| Camera.UpdateRenderers | 0.619 / 0.710 | 0.675 / 0.761 |
| UpdateRenderers | 0.615 / 0.706 | 0.672 / 0.757 |
| UpdateRenderers missionRender | 0.614 / 0.704 | 0.670 / 0.755 |
| Camera.UpdateRenderers Mission.render | 0.614 / 0.704 | 0.670 / 0.755 |
| Camera.UpdateRenderers eye.render | 0.588 / 0.674 | 0.640 / 0.725 |
| GameCamera::render activeScene | 0.585 / 0.671 | 0.637 / 0.721 |
| GameLogic | 0.571 / 0.340 | 0.583 / 0.364 |
| VKIsl.Present.Frame | 0.231 / 0.272 | 0.574 / 0.686 |

### mc2_03

| Zone | W86 mean/p95 | Main mean/p95 |
|---|---:|---:|
| Frame | 1.517 / 1.886 | 1.511 / 1.814 |
| DrawScreen | 0.758 / 0.920 | 0.785 / 0.949 |
| Camera.UpdateRenderers | 0.680 / 0.836 | 0.707 / 0.865 |
| UpdateRenderers | 0.679 / 0.834 | 0.706 / 0.863 |
| UpdateRenderers missionRender | 0.677 / 0.833 | 0.704 / 0.862 |
| Camera.UpdateRenderers Mission.render | 0.677 / 0.833 | 0.704 / 0.862 |
| Camera.UpdateRenderers eye.render | 0.676 / 0.832 | 0.703 / 0.861 |
| GameCamera::render activeScene | 0.673 / 0.828 | 0.699 / 0.857 |
| GameLogic | 0.555 / 0.318 | 0.566 / 0.334 |
| GameCamera::render textureManagerRenderLists | 0.465 / 0.596 | 0.484 / 0.614 |

### mc2_10

| Zone | W86 mean/p95 | Main mean/p95 |
|---|---:|---:|
| Frame | 2.307 / 3.245 | 2.294 / 3.205 |
| DrawScreen | 1.077 / 1.483 | 1.100 / 1.483 |
| GameLogic | 1.036 / 0.950 | 1.045 / 0.979 |
| Camera.UpdateRenderers | 0.990 / 1.397 | 1.013 / 1.396 |
| UpdateRenderers | 0.987 / 1.396 | 1.010 / 1.394 |
| UpdateRenderers missionRender | 0.985 / 1.395 | 1.008 / 1.393 |
| Camera.UpdateRenderers Mission.render | 0.985 / 1.395 | 1.008 / 1.393 |
| Camera.UpdateRenderers eye.render | 0.971 / 1.393 | 0.993 / 1.391 |
| GameCamera::render activeScene | 0.969 / 1.390 | 0.990 / 1.388 |
| GameCamera::render textureManagerRenderLists | 0.674 / 0.907 | 0.692 / 0.903 |

### mc2_24

| Zone | W86 mean/p95 | Main mean/p95 |
|---|---:|---:|
| Frame | 3.147 / 4.258 | 3.306 / 4.816 |
| GameLogic | 1.453 / 1.081 | 1.573 / 1.312 |
| DrawScreen | 1.406 / 2.013 | 1.497 / 2.220 |
| Camera.UpdateRenderers | 1.302 / 1.905 | 1.391 / 2.094 |
| UpdateRenderers | 1.297 / 1.899 | 1.386 / 2.089 |
| UpdateRenderers missionRender | 1.295 / 1.898 | 1.384 / 2.087 |
| Camera.UpdateRenderers Mission.render | 1.295 / 1.897 | 1.384 / 2.086 |
| Camera.UpdateRenderers eye.render | 1.249 / 1.848 | 1.331 / 2.027 |
| GameCamera::render activeScene | 1.246 / 1.845 | 1.328 / 2.024 |
| GameCamera::render textureManagerRenderLists | 0.788 / 1.098 | 0.851 / 1.202 |

### battle

| Zone | W86 mean/p95 | Main mean/p95 |
|---|---:|---:|
| unproven: no qualified capture | unproven | unproven |

## RX 7900 XTX: 1080p and 4K

GPU span is the VK island timestamp span (present excluded), not end-to-end frame time. FPS comes from complete game PERF windows; it is not the reciprocal of GPU span. GPU aggregates exclude the first 240 frames and use the mission’s default view, rather than the CPU camera envelope. Each capture verifies the device and applies the same reserved-core noise threshold. Accepted counts include the initial GPU sweep on 12–15 and any qualified later captures on 8,9,12,13; all rejected captures remain in the noise ranges. Repetition below three is provisional.

| Mission/resolution/build | Accepted N | GPU span ms | FPS | Whole external p95 median [range] % | Reserved external p95 median [range] % |
|---|---:|---:|---:|---:|---:|
| mc2_01/1920/w86 | 1 | 0.707 | 756.300 | 27.294 [25.675–32.369] | 18.250 [5.400–20.100] |
| mc2_01/1920/main | 1 | 0.733 | 760.500 | 25.288 [25.044–25.531] | 11.125 [5.950–16.300] |
| mc2_01/3840/w86 | 1 | 1.355 | 629.400 | 32.719 [28.694–56.519] | 16.925 [10.000–29.250] |
| mc2_01/3840/main | 1 | 1.066 | 626.700 | 28.062 [23.988–32.138] | 11.688 [5.825–17.550] |
| mc2_10/1920/w86 | 1 | 1.613 | 379.400 | 30.906 [25.119–31.275] | 17.975 [6.450–23.900] |
| mc2_10/1920/main | 1 | 1.637 | 382.300 | 31.237 [28.956–33.519] | 14.625 [11.575–17.675] |
| mc2_10/3840/w86 | 0 | unproven | unproven | 36.700 [29.944–39.950] | 19.625 [18.400–22.625] |
| mc2_10/3840/main | 1 | 2.612 | 347.800 | 29.934 [27.475–32.394] | 13.075 [10.200–15.950] |

## Ranked remaining target gaps

Rows are independent target gaps and overlap; do not add them. Camera mission gaps use the cockpit’s worst bookmarked view per capture, while the CPU table above describes the full captured mission. Tick has no numeric budget in the brief.

| Rank | Target | Budget ms | Over budget ms |
|---:|---|---:|---:|
| 1 | mc2_24 Frame (cockpit worst view) | 1 | +3.078 |
| 2 | mc2_10 Frame (cockpit worst view) | 1 | +2.265 |
| 3 | mc2_24 DrawScreen (cockpit worst view) | 0.1 | +1.862 |
| 4 | mc2_10 DrawScreen (cockpit worst view) | 0.1 | +1.468 |
| 5 | mc2_01 Frame (cockpit worst view) | 1 | +0.794 |
| 6 | mc2_01 DrawScreen (cockpit worst view) | 0.1 | +0.699 |
| 7 | mc2_03 DrawScreen (cockpit worst view) | 0.1 | +0.685 |
| 8 | mc2_03 Frame (cockpit worst view) | 1 | +0.511 |
| 9 | mc2_10 4K throughput | 5 | -2.125 |
| 10 | mc2_01 4K throughput | 5 | -3.404 |

## Three cheapest next wins

These are three small implementation candidates, not measured promises of savings. No optimization was implemented in this measurement lane. Inclusive zone cost is an upper bound, not the saving from one small change.

1. **Reuse ImGui recording scratch capacity** — `GameOS/gameos/engine_present_vk.cpp:1499–1517`. The recording path builds a per-frame `saved` vector. Reuse per-thread capacity while preserving per-frame clearing and texture-ID restoration. This is a local change with no class-layout or buffer-lifetime change. First-main mc2_24 TailRecord was 0.777 ms/frame, but Submit plus QueuePresent were only 0.089 ms: the 0.688 ms residual is CPU recording/other unscoped work including `finishFrame`, not proof that this allocation accounts for all of it. Savings from scratch reuse remain unmeasured.
2. **Skip unchanged binding-9 descriptor writes** — `GameOS/gameos/vulkan_color_prop_island.cpp:1747–1752`. Compare the exact descriptor-set slot, buffer, offset and range before `vkUpdateDescriptorSets`; invalidate on HZB buffer/map-generation changes. A rotating mask can eliminate the opportunity. This affects a small part of the measured static-prop recording scope (0.434 ms/frame in first-main mc2_24), not its entire cost.
3. **Hoist the frame-constant home-team lookup out of the terrain-object loop** — `code/objmgr_misc.cpp:605`. Retain per-object relation/visibility/animation semantics. This is the smallest code change but likely the smallest gain; the full terrain-presentation scope was 0.442 ms/frame in the first main mc2_24 capture. Do not equate that scope with the saving from one lookup.

## Defender, host interference, validation and cleanup

Live `Get-MpPreference` confirms exclusions for `E:/mc2-installs`, `A:/Games/mc2-lane4` and `A:/Games/mc2-main` (therefore their build trees). The script expands the currently existing `A:/Games/mc2-*` directories; it is not a permanent wildcard exclusion for future worktrees. `G:/mc2-tmp` is absent from both the script and live exclusions. Live exclusions also miss existing Ninja build worktrees `mc2-lane10`, `mc2-lane10-baseline`, `mc2-lane8`, `mc2-lane8-baseline`, and `mc2-lane9`. Owner-only elevated command (not run): `Add-MpPreference -ExclusionPath "G:\mc2-tmp","A:\Games\mc2-lane10","A:\Games\mc2-lane10-baseline","A:\Games\mc2-lane8","A:\Games\mc2-lane8-baseline","A:\Games\mc2-lane9"`.

A native Windows CPU trace under an exclusive lease confirms actual reserved-core activity from Plex Transcoder, WmiPrvSE and MsMpEng. `offenders/offenders.etl`, `offenders.csv`, `idle.json` and `attribution.json` preserve processor numbers, sampled thread/process mapping and physical-core topology. The ten-second recording ranked physical cores 8–9 and 12–13 quietest: mean busy CPU 20.77% versus 23.23% on 12–15, including diagnostic recording overhead. These are placement measurements, not profile acceptance numbers. After the first stock pair, `A:/Games/mc2-ops/owner_cores_guard.py` was changed to reserve 8,9,12,13 and assign the other non-desktop cores to tools. The guard was gracefully replaced before the corrected battle pairs, and `core_plan.json` plus affinity `0x3300` verify the applied reservation. Desktop cores 0–1 and all desktop services remain running. The guard configuration lives outside this git checkout.

Validation: 93 metric, profiler, cockpit and lease unit tests; covering `mission_cpu_profile|perf_budget_schema` contract rows; `git diff --check`. No engine code was changed or rebuilt. W86’s existing clean build was used; missing runtime DLL dependencies were supplied from the verified main build before normal deployment. Both build caches specify RelWithDebInfo and C++23.

Repo preflight verified a clean worktree and the assigned branch/root. Its overall `ok=false` is due to ambiguous deploy/smoke/tier1 command extraction from CLAUDE.md. Those parser-derived commands were not used; the canonical file and explicit assignment were read directly. No check was bypassed.

Cleanup: one install remains, `E:/mc2-installs/claude-quiet-measure`, restored to main via normal deploy. No extra installs created; disk freed 0 bytes. No push. Game/recorder/exporter processes have finished; the replacement core guard remains active to maintain the requested reservation.

Tool slowness / largest avoidable cost: the existing camera profiler requires 60-second captures and full event export for three missions per arm, and normal deployments reverify large asset sets. A separate future task should bound the profile window to the requested 15–20 seconds without weakening the camera proof.

Actual CPU samples on the original reserved cores (not whole-machine process totals):

| Process | PID | CPU samples on cores 12–15 |
|---|---:|---:|
| Plex Transcoder.exe | 68484 | 4038 |
| Plex Transcoder.exe | 64848 | 3136 |
| WmiPrvSE.exe | 6904 | 554 |
| MsMpEng.exe | 48788 | 550 |
| System | 4 | 393 |
| claude.exe | 50920 | 158 |
| firefox.exe | 53324 | 150 |
| firefox.exe | 27172 | 121 |
