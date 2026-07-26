# PERF-HARNESS-TRUTH-1 — status

Lane: `PERF-HARNESS-TRUTH-1` · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Commits: `1d8b0248` (harness + evidence-integrity), `5d398133` (the real fix), `<this>` (docs/registry)
Deploy lane: `0.4c` → `A:/Games/mc2-opengl/mc2-win64-v0.4c`, exe `5d398133ed92`

## Verdict up front

The harness is fixed, it now **proves** it can see fragment-bound cost before it
reports anything, and the owed frame-time table is delivered. The light grid
lands **at or slightly below** the cap=64 baseline. `MC2_LIGHT_REALISM` is
**affordable — measurably free** with the grid on.

## Meta review — the inherited diagnosis was WRONG

`LIGHT-CLUSTER-PERF-1-STATUS.md` attributed the blindness to `MC2_SMOKE_MODE=1`
minimizing the window and tripping the 10 ms background throttle. Checked
against reader source, that is false in both halves:

| Premise (as briefed) | Verdict | Source |
|---|---|---|
| `MC2_SMOKE_MODE=1` minimizes the window | **FALSE** | no window consumer reads it; the only consumers are the banner, input suppression, `gos_smoke.cpp` arg parsing, vis-semantics |
| The 10 ms throttle caused the flat table | **FALSE** | throttle comes solely from the OS show-state; harness set no `STARTUPINFO` at all |
| Throttle explains cap=64 ≈ cap=256 | **DISPROVEN EXPERIMENTALLY** | with a visible window + confirmed `[CAMERA_PARK]` receipt, the two arms still measured within **0.2%** |
| `MC2_ROAD_PBR` is a ghost needing doc deletion | **OVERTURNED** | already an accurate `[RETIRED 2026-07-18]` tombstone + a SHIPPED `active_campaigns.md` entry. Left alone deliberately |
| `run_visual_capture.py` has the mechanism to reuse | CONFIRMED | `STARTUPINFO.wShowWindow` 4 vs 7 |

**The actual root cause:** the harness reported the **CPU duration** of the
`DrawScreen` zone (`exec_time_ns`). GL is asynchronous — the CPU finishes
recording commands long before the GPU shades a fragment — so GPU fragment cost
*cannot* appear in a CPU zone. Raising the per-fragment light-pool loop 4×
genuinely does not change CPU-side submission, hence +0.2%.

The signal was in the old data all along, unread: the prior run's frame **counts**
were 1305 vs 1143 (−12%) while its reported p50s were 2.590 vs 2.574. And since
`csvexport` aggregate rows carry only mean/min/max/std, **p99 was never
derivable** from that path either.

A second, independent integrity defect: `MC2_LOG` was never set, so the engine
sent stdout to NUL and **all nine `.game.log` files were zero bytes**. No receipt
that the pose, the gate under test, or the mission ever applied. Those runs were
not merely wrong, they were unverifiable.

## What shipped

**`tools/tracy_capture_compare.py`**
- `frame_intervals_ms()` — true frame times from consecutive `DrawScreen` start
  timestamps (`ns_since_start` deltas via `csvexport --unwrap`), dropping a 40%
  warmup prefix and >200 ms hitch outliers. This is the correction that makes
  fragment cost visible. Old CPU-zone p50 retained per arm as `cpu_zone_p50_ms`.
- `--show-window` (**default ON**) → `SW_SHOWNOACTIVATE`: genuinely visible,
  never steals focus. `--no-show-window` warns loudly and is refused downstream.
- `assert_window_was_visible()` — **hard refusal** if the trace contains any
  `Frame.BackgroundThrottle` zone (the engine's own throttle marker). `baseline`
  and `regress` funnel through `measure_arm`, so no path can emit throttled numbers.
- `MC2_LOG=1` always set; `assert_receipt()` refuses on a zero-byte log or a
  missing `[CAMERA_PARK]` receipt.
- **`selfcheck`** — the ratchet. Runs cap=64 vs cap=256 (grid off) and **fails**
  if they land within 8%. Currently passes at **26.3%** separation.
- **`matrix`** — arms × poses table with real p50/p99; poses may carry a per-pose
  mission so the day/no-pool control can live on another map.

**Evidence integrity**
- Allowlisted `MC2_TERRAIN_LIGHT_POOLS` — the **master** gate of the light-pool
  feature was strippable while its own dependants were allowlisted, so the OFF
  arm of every light-pool A/B silently ran the ON path.
- Registered + documented + allowlisted `MC2_LIGHTING_DEBUG_VIEW`.
- Registered + documented `MC2_LIGHT_REALISM` (was allowlisted but had no
  discoverable contract, despite holding ~6 reported visual items).
- Documented `MC2_LIGHT_POOL_MAX`, `MC2_LIGHT_GRID`, `MC2_LIGHT_GRID_DIM`.
- **New `scripts/check-harness-gate-drive.py`** — fails the build when a harness
  sets a gate `run_smoke.py` would strip. Wired into `check-contracts.sh`.
  On its **first run** it caught two more real cases:
  `MC2_RENDER_FRAME_PLAN_TRACE` and `MC2_MDI_SUBMIT_TRACE`, both set by
  `tools/run_nvidia_readiness_smoke.py` and both stripped — the NVIDIA readiness
  runbook's "live pass/path identity" and "indirect submitters" traces emitted
  nothing, and the silence read as "nothing to report".

## The frame-time table (owed by LIGHT-CLUSTER-PERF-1)

mc2_10 night poses + mc2_01 day control · 3840×2160 · 15 s · uncapped,
free-timestep · visible non-throttled window · every arm receipt-verified.

| pose | arm | p50 ms | p99 ms | vs baseline (p50) |
|---|---|--:|--:|--:|
| night_busy | `cap64` baseline | 6.679 | 10.702 | — |
| night_busy | `cap256` grid=0 (**regression**) | 8.708 | 17.005 | **+30.4%** |
| night_busy | `cap256` grid=1 (**fix**) | 6.481 | 10.204 | **−3.0%** |
| night_mid | `cap64` baseline | 9.185 | 12.293 | — |
| night_mid | `cap256` grid=0 (**regression**) | 10.612 | 20.278 | **+15.5%** |
| night_mid | `cap256` grid=1 (**fix**) | 8.423 | 16.175 | **−8.3%** |
| day_nopool | `cap64` baseline | 5.215 | 7.163 | — |
| day_nopool | `cap256` grid=0 | 5.197 | 7.072 | −0.3% |
| day_nopool | `cap256` grid=1 | 5.207 | 6.907 | −0.2% |

The day/no-pool control is flat across all three arms (spread 0.3%), which is
what a valid measurement of a night-lighting change must look like — it doubles
as proof the rig is not just reporting scene-independent noise.

**The fix meets its target:** the grid lands the raised cap at or slightly below
the cap=64 baseline at both night poses, and p99 improves too at night_busy
(17.005 → 10.204).

## `MC2_LIGHT_REALISM` — the decision this unblocks

**YES — enable it, provided `MC2_LIGHT_GRID` stays on (it is default-ON).**

mc2_10, grid active, realism OFF vs ON:

| pose | realism OFF p50 | realism ON p50 | delta |
|---|--:|--:|--:|
| night_busy | 6.454 | 6.405 | −0.8% |
| night_mid | 8.519 | 8.607 | +1.0% |

Both inside run-to-run noise; means are near-identical (6.898/6.886 and
7.976/7.979). **It costs nothing measurable.**

Proof the gate actually engaged (not an inert-gate artifact — the exact failure
class this lane exists to fix), from `[LIGHT_GRID v1]` receipts at the same pose:

| | realism OFF | realism ON |
|---|--:|--:|
| stamped | 1962 | 6200–6945 |
| peak_cell | 17 | 32–33 |
| avg_cell | 0.48 | 1.51–1.70 |

The reshape **triples** the work the grid has to stamp, and frame time still does
not move — because `avg_cell 1.70` is trivial next to the ~200-iteration linear
loop it replaced. **The grid is precisely what makes realism affordable:** with
`MC2_LIGHT_GRID=0`, realism ON costs `8.732 ms` at night_busy, **+30.7%** over
baseline.

Caveat: this is a **cost** verdict only. The visual grading of realism ON
(cone transparency, beacon discs, pool continuity) remains the user's call, and
no captures were taken in this lane.

## Gates run

- contracts **68/68** (67 + the new `harness_gate_drive` checker)
- harnesses **16/16**
- tier1 **5/5 PASS**, `DEPLOY_FINGERPRINT OK` on all five (exe `5d398133ed92`)
- deployed exe md5-verified against `build64/RelWithDebInfo/mc2.exe`; gate
  strings `MC2_LIGHT_GRID` / `MC2_TERRAIN_LIGHT_POOLS` / `MC2_LIGHTING_DEBUG_VIEW`
  / `MC2_LIGHT_REALISM` all confirmed present in the deployed binary
- `selfcheck` **PASS** (26.3% separation, requirement ≥8%)

## Known-remaining, recorded not fixed

- **`run_smoke.py` tier1 has the same blindness by design.** Its report says so
  ("game window starts minimized … Avg FPS / p1% reflect that cap, not GPU
  throughput"), so tier1 FPS remains a liveness signal, not a perf measurement.
  Deliberately unchanged — flipping tier1 to a visible window would move every
  historical FPS baseline. Use `tracy_capture_compare.py matrix` for perf.
- **101 registered+read gates still missing from the passthrough allowlist**
  (`docs/tier1_env_vars.md` debt section). Only the ones blocking measurement
  were fixed here. The new `check-harness-gate-drive.py` covers the subset that
  a harness actually tries to drive, which is the part that silently corrupts
  evidence; the rest are latent.
- Three `[ENV-DROP]` warnings fire from the operator's ambient shell
  (`MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE`, `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`,
  `MC2_STATIC_POP_SPLIT`). Pre-existing, unrelated to this lane, left alone.
- `MC2_LIGHT_GRID` is default-ON so the **fix** is live on a default launch;
  `MC2_LIGHT_REALISM` is default-OFF, so its visual items are **not** yet
  user-visible. Flipping it is the user's decision — this lane only cleared the
  cost objection.

## Lane hygiene

Serialised correctly: a **fresh foreign smoke lease** held `0.4c` on arrival
(pid 39080, tier1, actively refreshing). Waited for release rather than
deploying over it. `docs/tier1_env_vars.md` and `scripts/run_smoke.py` are
claimed by other lanes (PAVED-OVERLAY-CLASS-1 live, EDGE-LIGHT-EMISSION-1 and
FORCE-ALL-VISIBLE-1 ~10h stale); all were clean in-tree and this lane's edits
are additive (allowlist entries, new doc bullets), disjoint from their work.
