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

- `tier1`: pre-push confidence gate. Current missions: `mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`.
- `tier2`: pre-PR campaign sweep. Current missions: full `mc2_01` through `mc2_24`.
- `tier3`: reserved for broader curated content after tier2 is stable.

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
