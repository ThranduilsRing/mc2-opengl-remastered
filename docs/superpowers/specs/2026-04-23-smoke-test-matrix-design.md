# Smoke test matrix — design

**Status:** Design approved, awaiting implementation plan.
**Date:** 2026-04-23.
**Worktree:** `nifty-mendeleev`.
**Owner:** rjosephmathews.

## 1. Motivation

The renderer and surrounding systems change frequently (shaders, FBO plumbing, overlay paths, asset-scale, cull gates). "Did I break anything?" is currently answered by launching the deployed build, eyeballing one map, and hoping. This spec defines an automated, grep-driven smoke matrix that exercises the full init chain plus multiple campaign missions and asserts silence on the classes of failure this codebase is known to produce silently.

The matrix is not a correctness oracle. It is a regression gate on the failure modes captured in `memory/cull_gates_are_load_bearing.md`, `memory/tgl_pool_exhaustion_is_silent.md`, `memory/mc2_init_order_widgets_before_subsystems.md`, and the `[SUBSYS v1]` instrumentation landed under `CLAUDE.md §Tier-1 Instrumentation Env Vars`.

## 2. Scope

### 2.1 In scope (Phase 1 — passive)
- An in-engine `MC2_SMOKE_MODE=1` path that accepts `--mission <stem> --profile <name> --duration <s>` argv, runs the full init chain, auto-starts the mission, auto-quits after duration, and emits a structured `[SMOKE v1] event=summary` terminal line.
- Perf and timing telemetry (`[PERF v1]`, `[TIMING v1]`) emitted to stdout.
- A Python runner (`scripts/run_smoke.py`) that reads a tiered mission manifest, spawns the engine serially, enforces fault gates from stdout/stderr, and writes artifacts on fail.
- A tiered manifest (`tests/smoke/smoke_missions.txt`): tier1 (~5 pre-push), tier2 (full campaign), tier3 (everything known), plus explicit `skip` entries.
- A separate, opt-in menu-canary entrypoint (`scripts/smoke_menu_canary.py`) that drives a single boot→logistics→launch via pyautogui.

### 2.2 In scope (Phase 2 — active)
- `--smoke-active` flag that enables a small in-engine autopilot: at `mission_ready + 10s` issue a `MOVE` order on `mover[0]`; at `+30s` issue an `ATTACK` order on the nearest living hostile.
- No outcome assertions — Phase 2 only proves the command-input path doesn't fault.

### 2.3 Out of scope
- Screenshot diffing for visual regressions (future tier-C, opt-in per release).
- Save/load state replay.
- Multiplayer and network paths.
- Mission objective / win-loss correctness checking.
- Parallel execution across missions (single GPU, single window — serial only).
- Retries on fail.
- Tracy/RGP integration (compatible but independent).

## 3. Architecture

### 3.1 Components

| Component | Role | Lives in |
|---|---|---|
| In-engine smoke mode | Argv parsing, profile/mission auto-load, auto-quit, `[SMOKE v1]` summary emission | `mc2.exe` (new, gated by `MC2_SMOKE_MODE=1`) |
| Perf collector | Frame-time ring buffer; emits `[PERF v1]` once before summary; emits `[TIMING v1]` at lifecycle milestones | `mc2.exe` |
| Mission manifest | Tiered list with per-entry overrides | `tests/smoke/smoke_missions.txt` |
| Runner | Tier selection, serial spawn, stdout/stderr parsing, gate enforcement, artifact writing, report generation | `scripts/run_smoke.py` |
| Menu canary | Single-shot pyautogui boot→logistics→launch, separate entrypoint | `scripts/smoke_menu_canary.py` |
| Existing `game_auto.py` | Reused by menu canary only; not involved in `--mission` path | unchanged |

### 3.2 Data flow per mission

```
runner → spawn mc2.exe with argv + env
       → stdout/stderr line-captured
       → runner parses [SMOKE v1], [GL_ERROR v1], [TGL_POOL v1],
         [ASSET_SCALE v1], [HEARTBEAT], [PERF v1], [TIMING v1],
         [DESTROY v1], exit code
       → gates evaluated → pass/fail + fail bucket
       → artifacts written on fail (or always if --keep-logs)
```

The runner and the engine communicate only through the one-way stdout/stderr contract. No shared files, no IPC. This makes the runner swappable — a bash one-liner with `grep` is a viable fallback.

## 4. Fault contract

### 4.1 Fail gates (binary)

| Gate | Source signal | Behavior |
|---|---|---|
| Process alive N min | PID + walltime | **fail** if exits early and not via clean summary |
| Heartbeat advances (load phase) | `[HEARTBEAT]` cadence | **fail** if gap > `--heartbeat-timeout-load` (default 60s) before `[TIMING v1] event=mission_ready` |
| Heartbeat advances (play phase) | `[HEARTBEAT]` cadence | **fail** if gap > `--heartbeat-timeout-play` (default 3s) after `mission_ready` |
| Clean exit | `[SMOKE v1] event=summary result=pass` + exit 0 | **fail** otherwise |
| GL errors | `[GL_ERROR v1]` count | **fail** if ≥1 (with `MC2_GL_ERROR_DRAIN_SILENT` unset) |
| TGL pool exhaustion | `[TGL_POOL v1]` NULL events (not `summary`) | **fail** if ≥1 |
| Asset scale oob | `[ASSET_SCALE v1] event=oob_blit` count | **fail** if ≥1 |
| Crash-handler output | Crash writer signature lines | **fail** if any |
| Missing required file | File-load error signature | **fail** if any |

### 4.2 Logged, no-fail (baseline first)
- `[DESTROY v1]` count per mission.
- `[PERF v1]` values (avg fps, p50/p99 frame ms, p1-low fps, peak ms).
- `[TIMING v1]` milestones: `first_frame`, `profile_ready`, `logistics_ready`, `mission_load_start`, `mission_ready`, `mission_quit`.

Baselines key on `<profile>@<stem>@<tier>@<duration>` to prevent `stock mc2_01` and `magic_carver_v mc2_01` from contaminating each other. Baselines update only on runs that pass every binary gate, and only when the runner is invoked with `--baseline-update`.

### 4.3 Fail buckets (report taxonomy)

| Bucket | Meaning |
|---|---|
| `timeout` | walltime cap hit, process killed |
| `crash_no_summary` | crash-handler output present, no `[SMOKE v1] summary` |
| `crash_silent` | nonzero exit, no crash handler, no summary |
| `engine_reported_fail` | `[SMOKE v1] summary result=fail reason=…` |
| `heartbeat_freeze_load` | heartbeat gap > load timeout before `mission_ready` |
| `heartbeat_freeze_play` | heartbeat gap > play timeout after `mission_ready` |
| `gl_error` | `[GL_ERROR v1]` observed |
| `pool_null` | `[TGL_POOL v1]` NULL event observed |
| `asset_oob` | `[ASSET_SCALE v1] event=oob_blit` observed |
| `missing_file` | file-load error signature observed |
| `shader_error` | shader compile or link failure observed in stdout/stderr |
| `instrumentation_missing` | `[INSTR v1] enabled:` banner absent from the run |
| `multiple` | two or more of the above; all listed in the detail column |

The `[SMOKE v1] summary` line is best-effort. A hard crash mid-run may not produce one — that case lands in `crash_no_summary`, not treated as an engine bug.

## 5. In-engine smoke mode (`mc2.exe` side)

### 5.1 CLI surface

```
mc2.exe --mission <stem>                # required when MC2_SMOKE_MODE=1
        --profile <name>                # required; runner always passes this, default value is "stock"
        --duration <seconds>            # default 120
        --smoke-active                  # Phase 2; Phase 1 silently accepted, no-op
        --heartbeat-timeout-load <s>    # informational, runner enforces
        --heartbeat-timeout-play <s>    # informational, runner enforces
```

`MC2_SMOKE_MODE=1` is required. Argv parsing rejects `--mission` without the env var. This prevents accidental smoke-mode boots in dev runs.

### 5.2 Stem resolution

`--mission mc2_01` resolves to `missions/mc2_01.fit` + paired `.abl` + paired `.pak`. Resolver searches `data/missions/` (loose-file override path) first, then FST archives. Emits `[SMOKE v1] event=mission_resolve stem=mc2_01 source=loose|fst` so the runner logs which source won.

### 5.3 Init chain

1. Parse argv before `InitializeGameEngine`; store in smoke-mode singleton; install atexit handler for best-effort summary emission.
2. Run normal `InitializeGameEngine` (widget-load included — not bypassed; per `memory/mc2_init_order_widgets_before_subsystems.md`).
3. Load profile: stock profile by default; named profile if `--profile` provided. Emit `[TIMING v1] event=profile_ready`.
4. Corebrain + warriors + purchase/economy load via normal path. Emit `[TIMING v1] event=logistics_ready`.
5. Synthesize the "Launch pressed" entrypoint: invoke the same call the Logistics screen uses to enter the mission. Emit `[TIMING v1] event=mission_load_start`, then `event=mission_ready` when playable.
6. Frame loop runs normally: AI ticks, renderer active, input not required to progress (pause-on-no-input suppressed in smoke mode). Intros/videos skipped via existing skip path.
7. At `T = duration` (measured from `mission_ready`), set quit flag; allow one final frame for summary emission; exit 0.

### 5.4 Determinism

Single `MC2_SMOKE_SEED` env (default `0xC0FFEE`) threaded into every RNG init site that already takes a seed. Not a hunt for hidden RNGs — the gates don't require perfect determinism, only reproducibility of the major init and render paths. Nondeterminism shows up as run-to-run variance in `[PERF v1]`, which is acceptable.

### 5.5 `[SMOKE v1]` line taxonomy

```
[SMOKE v1] event=banner mode=passive|active mission=mc2_01 duration=120 seed=0xc0ffee
[SMOKE v1] event=mission_resolve stem=mc2_01 source=loose
[SMOKE v1] event=summary result=pass duration_actual_ms=120034 frames=14200
# Fault counters (gl_errors, pool_nulls, asset_oob, destroys) are NOT embedded in
# the summary line. The runner is the authoritative source: it counts the
# per-event lines in the stream. Embedding engine-side counts would create a
# second source of truth with drift potential.
[SMOKE v1] event=summary result=fail reason=init_failure stage=warriors_load
```

Summary is the last line before exit. Atexit handler emits a best-effort `result=fail reason=...` on signal-catchable exit paths; unhandled crashes produce no summary (→ `crash_no_summary` bucket on the runner side).

### 5.6 Perf emission (always on in smoke mode)
- Rolling frame-time ring buffer, default cap 8192 samples (~2 min at 60fps). Configurable via `MC2_SMOKE_PERF_SAMPLES`.
- On smoke-summary: compute avg, p50, p99, p1-low fps, peak frame time ms.
- Emit single line: `[PERF v1] avg_fps=… p50_ms=… p99_ms=… p1low_fps=… peak_ms=… samples=…` immediately before `[SMOKE v1] event=summary`.

### 5.7 Phase 2 — `--smoke-active`

Separate second flag, no implementation in Phase 1 (only argv acceptance). When implemented:
- At `mission_ready + 10s`: iterate player-team controllable movers, pick `mover[0]`, issue `MOVE` order to `(own_pos + forward * 300)` via the same command entrypoint a right-click drag uses.
- At `mission_ready + 30s`: iterate world objects for the nearest living hostile via the existing object-manager spatial query; if one is within 1500 units, issue `ATTACK` order.
- Emit `[SMOKE v1] event=autopilot_order kind=move|attack target=…`.
- No outcome assertion — the purpose is exercising the command-input path, not validating gameplay.

### 5.8 What Phase 1 does not touch
- Gameplay logic beyond "let AI tick."
- Save/load system.
- Multiplayer init.
- Mission objective / scoring checks.

## 6. Runner (`scripts/run_smoke.py`)

### 6.1 CLI

```
run_smoke.py --tier tier1|tier2|tier3 [--mission STEM ...]
             [--fail-fast] [--continue]         # default: continue
             [--keep-logs]                      # default: logs only on fail
             [--baseline-update]                # rewrite baselines from this run
             [--kill-existing]                  # default: refuse if mc2.exe already running
             [--duration SECONDS]               # override all per-entry durations
             [--jobs 1]                         # reserved; 1 only for now
             [--report PATH]                    # default: tests/smoke/artifacts/<ts>/report.md
             [--menu-canary]                    # opt-in; runs once at the end
```

`--mission STEM` is repeatable; specifying it ignores `--tier` and runs only the listed stems (per-mission overrides from manifest still apply if present).

### 6.2 Safety: existing process handling

Default behavior is **refuse to run** with a clear error if any `mc2.exe` is already running. This avoids stomping on a live dev session. `--kill-existing` opts into `taskkill /F /IM mc2.exe` for nightly/CI use. Between missions within one runner invocation, the runner always terminates its own child (and waits for PDB-lock release) — that's unconditional.

### 6.3 Per-mission execution loop

1. Pre-spawn: check for existing `mc2.exe` — abort unless `--kill-existing` set; if set, `taskkill /F /IM mc2.exe` and wait for PDB lock release.
2. Spawn `mc2.exe --profile <name> --mission <stem> --duration <d>` (profile is passed explicitly even when it's `stock`, so logs and baseline keys stay uniform) with env:
   `MC2_SMOKE_MODE=1 MC2_HEARTBEAT=1 MC2_TGL_POOL_TRACE=1 MC2_ASSET_SCALE_TRACE=1 MC2_SMOKE_SEED=0xC0FFEE`
   (`MC2_GL_ERROR_DRAIN_SILENT` explicitly unset so first-error prints are captured.)
3. Line-stream parser reads stdout + stderr, timestamped per line, maintaining counters and last-heartbeat-time per phase. Phase switches on observing `[TIMING v1] event=mission_ready`.
4. Walltime cap = `duration + 60s grace`. On expiry: taskkill → `timeout` bucket.
5. Wait for exit; add 2s PDB-lock grace before the next spawn.
6. Evaluate gates → pass/fail + fail bucket(s).
7. Write artifacts on fail (or always if `--keep-logs`). If `--baseline-update` and pass, update baseline entry for this `<profile>@<stem>@<tier>@<duration>` key.

### 6.4 Failure mode

Default `--continue`: runner proceeds through every selected mission regardless of individual results. `--fail-fast` halts after the first fail (recommended pairing with `--tier tier1` for local iteration).

Recommended defaults per use case:
- **Local pre-push (tier1, fail-fast):** `run_smoke.py --tier tier1 --fail-fast`
- **Pre-PR (tier2, continue):** `run_smoke.py --tier tier2`
- **Pre-release / nightly (tier3, continue):** `run_smoke.py --tier tier3 --kill-existing`

### 6.5 Artifact layout

```
tests/smoke/artifacts/2026-04-23T14-32-07/
├── report.md                    # summary table + per-mission fail details
├── report.json                  # machine-readable
├── mc2_01.log                   # full stdout/stderr (fail only, or with --keep-logs)
├── mc2_10.log
└── ...
```

Write-on-fail keeps the artifacts directory navigable. `--keep-logs` for deliberate debugging runs.

### 6.6 Baselines — `tests/smoke/baselines.json`

```json
{
  "stock@mc2_01@tier1@120": {
    "destroys": {"mean": 87, "stddev": 4, "samples": 5, "updated": "2026-04-23"},
    "perf":     {"avg_fps": 142, "p1low_fps": 58, "peak_ms": 34}
  }
}
```

Key composition `<profile>@<stem>@<tier>@<duration>` prevents cross-profile and cross-tier contamination. Delta-from-baseline is logged in the report but not a fail gate in Phase 1.

### 6.7 Report format (excerpt)

```
# Smoke run 2026-04-23T14-32-07  tier=tier1  profile=stock  result=FAIL (3/5 passed)

| Mission | Result | Bucket                 | Frames | Avg FPS | p1% | Load ms | Δ destroys |
|---------|--------|------------------------|--------|---------|-----|---------|-----------|
| mc2_01  | PASS   |                        | 14200  | 142     | 58  | 4300    | +2        |
| mc2_03  | PASS   |                        | 14100  | 138     | 55  | 5100    | -1        |
| mc2_10  | FAIL   | gl_error               | 8400   | 119     | 32  | 6800    | +0        |
| mc2_17  | FAIL   | heartbeat_freeze_load  | 0      | -       | -   | -       | -         |
| mc2_22  | PASS   |                        | 14000  | 140     | 56  | 4800    | +0        |

## Failures
### mc2_10 — gl_error
First GL error at T+42s: GL_INVALID_ENUM in glDrawElements (see mc2_10.log:1842)
### mc2_17 — heartbeat_freeze_load
Last heartbeat at T+48s during mission_load_start; no mission_ready emitted.
```

## 7. Mission manifest — `tests/smoke/smoke_missions.txt`

### 7.1 Format

```
# <tier> <stem> [key=value]... [reason="..."]
# Keys: duration, heartbeat_timeout_load, heartbeat_timeout_play, profile, active
# Tiers: tier1, tier2, tier3, skip

tier1 mc2_01 reason="baseline grass/desert combat"
tier1 mc2_03 reason="salvage + heavier combat"
tier1 mc2_10 reason="urban/complex objects"
tier1 mc2_17 duration=180 heartbeat_timeout_load=120 reason="large map"
tier1 mc2_22 reason="naval/water biome"

tier2 mc2_02
tier2 mc2_04
# ... full campaign (~30 entries) ...

tier3 m0101 reason="training tier"
tier3 e3demo reason="dev demo content, may be fragile"
# ... everything (~84 entries) ...

skip ai_glenn reason="dev leftover; known pathing issue"
skip gamesys reason="systems test stub, not a real mission"
```

### 7.2 Parse rules
- Lines whitespace-split; shell-style quoted values supported.
- Unknown keys emit warnings, not errors.
- A stem listed in multiple tiers runs once per selected tier.
- `skip` entries are excluded from all tiers regardless of other appearances.

## 8. Menu canary — `scripts/smoke_menu_canary.py`

Separate entrypoint. **Not invoked by `run_smoke.py` unless `--menu-canary` is explicitly passed.** Rationale: menu canary needs a focused window and intermittent user attention; it is unfit for overnight unattended runs.

1. `game_auto.py launch` (no `MC2_SMOKE_MODE`; normal boot path).
2. Wait for `[HEARTBEAT]` in stderr confirming main menu live.
3. Scripted sequence: main menu → instant action → pick first available mission → logistics → launch → 30s gameplay → Escape → quit.
4. Apply the same fault gate set (telemetry invariants).
5. Plus one canary-only gate: every click must produce a screen-change within 3s (detected via screenshot hash delta).
6. Emits `canary_report.md` alongside the main run's artifacts.

## 9. Interaction with existing instrumentation

The matrix consumes signals that already exist in the tree per `CLAUDE.md §Tier-1 Instrumentation Env Vars`:

- `MC2_HEARTBEAT` — always enabled in smoke runs.
- `MC2_TGL_POOL_TRACE` — always enabled; runner fails on any non-`summary` event.
- `MC2_ASSET_SCALE_TRACE` — always enabled; runner fails on `oob_blit`.
- `MC2_GL_ERROR_DRAIN_SILENT` — explicitly unset so first-error prints surface.
- `MC2_DESTROY_TRACE` — optional; runner tolerates its absence and reads the always-on `[DESTROY v1]` summary if present.

The `[INSTR v1] enabled:` banner is required to appear in every smoke run — a missing banner maps to the `instrumentation_missing` fail bucket and indicates a wiring regression (the signals the gates depend on can't be trusted for this run).

Shader compile/link failures (captured in stdout/stderr by the existing makeProgram path) are a distinct fail class. Per `CLAUDE.md §Critical Rules` ("Shader hot-reload fails silently: Bad compile = old shader stays active"), a broken shader can otherwise leave a run green on every other gate. The runner greps for compile/link error signatures and maps them to the `shader_error` bucket.

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Smoke mode's synthesized Launch bypasses a path real users hit | Load profile via stock path; enter mission via same call Logistics uses. Menu canary covers the Logistics UI path separately. |
| Shader hot-reload silently fails during dev, smoke passes stale shader | `[GL_ERROR v1]` gate catches most; shader compile failures already log to console and would fall under the `missing_file`-adjacent class. Consider extending to a `shader_compile_fail` bucket in a follow-up. |
| Mission fails for content reason unrelated to code change | `skip` entries document known-bad missions with reason; tier1 hand-picked to be stable. |
| `--kill-existing` wipes a live dev session | Not default. Runner refuses without the flag and prints the offending PID. |
| Baseline drift poisons comparison | Baselines only update on pass + `--baseline-update`; stale baselines produce delta noise, not false fails, in Phase 1. |
| Phase 2 autopilot enters gameplay code paths that can't safely run headless | Gated behind separate `--smoke-active` flag; Phase 1 never touches it. Phase 2 design is its own spec cycle. |

## 11. Open questions (deferred to plan/implementation)

- Exact call site for "synthesize Launch pressed" — needs a read of `mclib` Logistics to find the single entrypoint the Launch button invokes.
- Atexit handler mechanics on Windows: signal catching on unhandled exceptions needs Verification; best-effort only, not load-bearing.
- Frame-time ring buffer storage site — which subsystem owns it. Likely `gos_profiler` given its existing timing infrastructure.
- How `[TIMING v1] event=mission_ready` is triggered — the engine already knows when mission load completes, but may not have a single observable boundary event. Needs exploration.

## 12. Non-goals (again, for emphasis)

- Visual regression detection (future, separate spec).
- Cross-mission state carryover (each run is cold).
- Multi-GPU parallelism.
- Correctness verification of gameplay outcomes.
- Replacing manual QA before a release.
