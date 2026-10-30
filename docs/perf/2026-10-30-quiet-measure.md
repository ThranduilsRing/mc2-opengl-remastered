# W86 to W89 quiet measurement attempt — 2026-10-30

**NOISY; no accepted CPU/GPU timings and zero valid alternating pairs.**
Both serial CPU sweeps and both GPU matrices exceeded the brief's 15% external-CPU p95 limit. A no-game window reached three consecutive ten-second samples below 15%, but the subsequent captures again exceeded the limit. The serial W86 payload-swap/profile command was rejected by automatic approval review as `blocked by policy`; that step was skipped without a workaround. The required three alternating pairs and a ranking of remaining milliseconds are unproven.

## Inputs and evidence

- Assignment: `A:/Games/mc2-lane4`, `claude/quiet-measure2`, base `586327c7904eabf942d12d5835c72a2d43e2248a` (W89, temporal removed, present thread ON).
- W89 executable: clean embedded fingerprint `586327c7904e`; SHA256 `84c45090b8a4bb288f9644e7bf0c5caf208de3b10949d3ae95f42ec17156f023`. Reused the matching committed `A:/Games/mc2-main/build64-ninja` payload; no compilation.
- W86 input: clean embedded fingerprint `c7ef9a66699e`; SHA256 `91e5bccdca001e15ae9c04e2afba9d80a452d2810e3a56d112285479f502e5de`, from `E:/mc2-archive-2026-10/proofs/w86-fixups/before.exe`. No valid W86 profile was obtained.
- Proof root: `E:/mc2-archive-2026-10/proofs/quiet-measure2/`. Fingerprints and `current.exe` preserve the inputs. Large captures remain outside git.
- Exclusive lease `quiet-measure2`: approximately 17:48–18:22 CDT. Lease waits were negligible. Games used reserved affinity `0xf000`; GPU captures explicitly selected the RX 7900 XTX, verified in all eight logs.
- First serial CPU sweep: `serial-pair1-current/20261030T225441.461157Z/`. Retry after idle quiet: `quiet-current/20261030T231044.216831Z/`. Each contains `profile.json`, `profile.md`, per-mission traces, camera measurements and `host-load.json`.
- The retry initially used the profiler's default output directory; its completed run was moved into this lane's proof root and its profile trace paths updated. Raw capture logs retain their original output paths.
- `post-gpu-external-cpu.csv`: 15.05%, then **13.36%, 14.55%, 13.69%** with no game running. This qualified an idle quiet candidate, rather than acceptance of a game capture.

## CPU results

Frame mean/p95, GameLogic, DrawScreen, SwapWindow and present-worker timings are **withheld: NOISY** for every completed mission. External CPU is sampled during the run with this tool's descendant game CPU subtracted. The brief's 15% limit is stricter than the profiler/cockpit's built-in 25% cutoff.

| Mission | First serial external CPU p95 | Retry external CPU p95 | Accepted timings |
|---|---:|---:|---|
| mc2_01 | 32.61% | 31.79% | none |
| mc2_03 | 33.20% | 27.70% | none |
| mc2_10 night | 33.54% | 29.84% | none |
| mc2_24 | 31.44% | 33.79% | none |
| battle | 29.43% | not repeated | incomplete; no 200-mover proof |

Both stock sweeps completed all four missions. The first requested 60 seconds; the retry requested 20 seconds. The existing profiler enforces at least 60 seconds for its mc2_01/mc2_10/mc2_24 camera envelopes. Timings are elapsed zones including waits; nested scopes overlap, and present-thread work can overlap the main thread. `SimOverlap.Tick` is a batch of one or more ticks, so its p95 does not establish an individual 200-mover tick.

Top ten inclusive zones by total elapsed time in the retry, in order (descriptive under load; no independent cost or savings ranking):

- **mc2_01:** `Frame`; `DrawScreen`; `Camera.UpdateRenderers`; `UpdateRenderers`; `UpdateRenderers missionRender`; `Camera.UpdateRenderers Mission.render`; `Camera.UpdateRenderers eye.render`; `GameCamera::render activeScene`; `GameLogic`; `VKIsl.Present.Frame`.
- **mc2_03:** `Frame`; `DrawScreen`; `Camera.UpdateRenderers`; `UpdateRenderers`; `UpdateRenderers missionRender`; `Camera.UpdateRenderers Mission.render`; `Camera.UpdateRenderers eye.render`; `GameCamera::render activeScene`; `GameLogic`; `GameCamera::render textureManagerRenderLists`.
- **mc2_10:** `Frame`; `DrawScreen`; `GameLogic`; `Camera.UpdateRenderers`; `UpdateRenderers`; `UpdateRenderers missionRender`; `Camera.UpdateRenderers Mission.render`; `Camera.UpdateRenderers eye.render`; `GameCamera::render activeScene`; `GameCamera::render textureManagerRenderLists`.
- **mc2_24:** `Frame`; `GameLogic`; `DrawScreen`; `Camera.UpdateRenderers`; `UpdateRenderers`; `UpdateRenderers missionRender`; `Camera.UpdateRenderers Mission.render`; `Camera.UpdateRenderers eye.render`; `GameCamera::render activeScene`; `GameCamera::render textureManagerRenderLists`.

## RX 7900 XTX: 1080p and 4K

All eight captures completed successfully and verified the requested GPU. Every capture failed the brief's quiet threshold, including retry samples below the tool's looser 25% cutoff. VK island span and fps are therefore **withheld: NOISY**.

| Mission | Resolution | First external CPU p95 | Retry external CPU p95 | Accepted span / fps |
|---|---|---:|---:|---|
| mc2_01 | 1920×1080 | 32.13% | 22.94% | none |
| mc2_01 | 3840×2160 | 26.06% | 35.31% | none |
| mc2_10 night | 1920×1080 | 25.48% | 22.20% | none |
| mc2_10 night | 3840×2160 | 38.57% | 30.74% | none |

Evidence: `gpu/` and `gpu-quiet-candidate/`, each with four `capture.tracy` files, game logs, host samples and `summary.json`. Runs used the existing Tracy harness, 20-second recordings, free timestep, uncapped fps and explicit width/height. The run-local invocation recipe is `gpu_measure.py` in the proof root.

Raw GPU summaries retain timestamp-span window means; the first 240 frames are excluded from that descriptive aggregate. FPS evidence is the game's completed 8,192-frame `[PERF v1]` window in each log. No heartbeat windows were emitted; the first summary retains `fps: null` alongside the subsequently extracted `perf_log_windows`. The retry summary also records the PERF-window fps. These raw values are rejected measurements. GPU span excludes present, and its reciprocal is not measured end-to-end fps.

## Target gaps and next source checks

No remaining gap in milliseconds can be ranked from the rejected captures. Historical timings and the cockpit's legacy baseline are not substituted for fresh W86 pairs.

| Target | Remaining gap |
|---|---|
| Stock Frame mean below 1 ms | unmeasured under acceptable load |
| DrawScreen below 0.1 ms | unmeasured under acceptable load |
| 4K at 200 fps / 5 ms throughput | unproven on both requested missions |
| 200-mover individual tick | unproven; observed battle workload was 70 movers |

Three cheapest performance wins were not established. These are source checks with unmeasured savings and implementation cost:

1. `GameOS/gameos/engine_present_vk.cpp:2504`: attribute inclusive `VKIsl.Present.TailRecord` to its existing Submit (`:2632`) and QueuePresent (`:2661`) children before targeting CPU recording. The tail scope runs until function return and includes both calls.
2. `code/mission_update.cpp:375`: inspect the per-render `updateAppearancesOnly(true, true, true)` path for unchanged static-appearance work. Preserve mover interpolation and establish dirty-state/call-count evidence before narrowing updates.
3. `GameOS/gameos/vulkan_vfx_island.cpp:653` and `:660`: inspect whether independent particle-feed preparation can precede the reused-slot wait. Verify frame-allocation retirement and ownership first; moving writes into an in-flight allocation would be invalid.

## Incomplete work, checks and cleanup

- Battle: a temporary fixture requested 178 clones, but the profiler reported `battle spawn/attack was not acknowledged`. All 110 world samples contained **70 movers**. `commands.txt.ack`, `frames.jsonl` and the trace remain under the first serial sweep's `battle/`. No battle top-ten or 200-mover tick is accepted. The fixture was restored byte-for-byte; its requested variant is preserved as `battle-fixture-200.json` in the proof root.
- An initial orchestration attempt overlapped profiles and crashed. Its `pair1-current/` and `pair1-baseline/` captures were discarded; only this lane's process tree was stopped, and W89 was restored before the serial sweeps.
- Skipped: automatic approval review rejected the later serial W86 payload-swap/profile command as **`blocked by policy`**, with no more specific reason. That rejected command did not execute. No alternate route was used; **0/3 valid alternating pairs** were completed.
- `budget_check.py --validate-schema`: PASS. Both profiles were processed by the cockpit; all missions are INCONCLUSIVE. See `cockpit/index.html` and `cockpit-retry/index.html`. Their legacy baseline is not a fresh paired W86 measurement.
- Tool slowness: camera profiles require 60-second envelopes and minutes of event export per mission. This campaign spent roughly eleven minutes exporting/processing traces in addition to game time. A future profiler task should provide the requested bounded 15–20-second measurement window; no tooling changes were made here.
- Final `deploy_payload.py --check-only`: **install complete**. The installed executable still matches the W89 fingerprint and SHA256 above. The exclusive lease was released and the host reported no running engines.
- One existing install was reused: `E:/mc2-installs/claude-quiet-measure`. No extra installs remain. Disk freed: **0 bytes**. No compilation, engine changes or pushes. Required documentation proof: `git diff --check`.

The largest avoidable cost was full-trace event export. A bounded measurement window would reduce both CPU work and elapsed turnaround; quiet acceptance still requires per-game external-CPU evidence.
