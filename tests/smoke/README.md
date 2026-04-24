# Smoke Gate

This is the default "did I break it" regression gate for render/init/cull/asset-path changes.

## Default command

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Recommended local fast loop:

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --duration 8 --fail-fast --kill-existing
```

Exit `0` means:
- menu canary passed
- all selected smoke missions passed

Artifacts land in `tests/smoke/artifacts/<timestamp>/`.

## Tier ladder

- `tier1`: pre-push confidence gate. Stable hand-picked 5 missions covering different biomes/content classes. Run isolated (one mission per process, between gate runs).
- `tier2`: pre-PR / major-feature campaign sweep. Hand-maintained authoritative list of `mc2_01` through `mc2_24`. Runs back-to-back over ~30 minutes of sustained engine load.
- `tier3`: reserved for broader curated content after tier2 is stable.

## Measurement semantics (important)

**Tier1 perf numbers and tier2 perf numbers are not directly comparable, even for the same mission at the same duration.** They measure different things:

- **Tier1 baselines** = isolated single-mission perf. Each gate run starts from a clean OS state (cold GPU, untouched file cache, no driver retained state). These numbers reflect what an end-user actually experiences when launching one mission. **Use tier1 baselines as the regression-detection reference.**
- **Tier2 baselines** = sequenced stress perf. Captured during 24 back-to-back missions; the same mission late in the run sees GPU thermal load, evicted file caches, and AMD driver retained state from earlier missions. Useful as a long-session stress indicator and for surfacing intrinsically-heavy missions, but not as a per-mission perf reference.

Empirical example (mc2_21 at duration=60):
- tier1 isolated: avg 172.9 fps, p1low 152.3, peak 8.8 ms
- tier2 sequenced: avg 82.8 fps, p1low 26.8, peak 270.8 ms

Same mission, same engine, same duration — but ~5.7× sustained-fps difference and 33× peak-frame difference. Both numbers are real; they just answer different questions.

**Tier2's primary signal is pass/fail and crash detection, not perf delta.** Treat tier2 perf numbers as trend indicators, not gate thresholds.

**Tier1 reshuffle?** Previously considered: rolling tier1 from "worst from tier2." Abandoned because tier2 perf data is environmentally biased (see above). Tier1 stays hand-curated for stability.

Examples:

```powershell
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
py -3 scripts/run_smoke.py --tier tier2 --kill-existing
py -3 scripts/run_smoke.py --menu-canary --keep-logs --kill-existing
```

## What each path means

- Direct-start smoke matrix: uses `MC2_SMOKE_MODE=1` and `--mission <stem>` to load missions without menus. This is the backbone.
- Menu canary: separate pyautogui/native-input playback path to prove startup/menu flow still behaves plausibly. This is a canary, not the backbone.

## Fail buckets

- `timeout`: walltime cap hit. First check mission load hang or missing heartbeat progression.
- `crash_no_summary`: crash path with no smoke summary. First check crash output and last 50 log lines.
- `crash_silent`: exited nonzero with no explicit crash marker. First check recent renderer/init changes.
- `engine_reported_fail`: engine emitted `[SMOKE v1] result=fail`. First check the `reason=` and `stage=`.
- `heartbeat_freeze_load`: stalled before `mission_ready`. First check mission load, missing files, or startup deadlock.
- `heartbeat_freeze_play`: stalled after `mission_ready`. First check render/game loop regressions.
- `gl_error`: `[GL_ERROR v1]` observed. First check shader changes, FBO state, and draw ordering.
- `pool_null`: `[TGL_POOL v1]` exhaustion observed. First check cull/visibility changes and pool budget regressions.
- `asset_oob`: `[ASSET_SCALE v1] event=oob_blit` observed. First check asset-scale manifest coverage and caller math.
- `missing_file`: required file load failed. First check mission/content resolution and loose-file overrides.
- `shader_error`: compile/link failure in log. First check shader console output; remember bad compile can leave old shader active.
- `instrumentation_missing`: `[INSTR v1]` banner missing. First check that the instrumented build was actually deployed.
- `multiple`: more than one bucket triggered. Start with the earliest hard fault in the log rather than the summary.

## Baselines

Use `--baseline-update` only on a known-clean commit after the binary gates are already green.

Rules:
- do not baseline a failing run
- do not baseline on a dirty "maybe fixed" commit
- rotate baselines after an intentional perf or lifecycle change that genuinely moves steady-state numbers
- keep baselines keyed by profile/mission/tier/duration; do not compare unlike runs

Typical update command:

```powershell
py -3 scripts/run_smoke.py --tier tier2 --baseline-update --kill-existing
```

If tier2 exposes content-specific quirks:
- real engine bug: fix it
- content-specific but valid mission: add per-entry override in `smoke_missions.txt`
- non-playable/dev leftover: skip it with an explicit reason

## Menu canary limitations

- It is screen-coordinate-bound to the recorded environment.
- It requires an active desktop session.
- It is not CI-safe or headless-safe.
- It should not be treated as display-independent.
- It currently uses `MC2_MENU_CANARY_SKIP_INTRO=1` to bypass intro timing before replay.
- Passing means "clean UI-path run and clean exit," not "proved a specific pixel-perfect route."

If the menu path changes materially, re-record:
- source script: `tests/smoke/menu_canary_first_mission.txt`
- helper: `scripts/game_auto.py`
- tool notes: `docs/game-auto-tools.md`

## Related docs

- Spec: `docs/superpowers/specs/2026-04-23-smoke-test-matrix-design.md`
- Plan: `docs/superpowers/plans/2026-04-23-smoke-test-matrix.md`
- Tooling notes: `docs/game-auto-tools.md`
- Manifest: `tests/smoke/smoke_missions.txt`
