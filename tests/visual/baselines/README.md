<!-- doc-status: current | reviewed:2026-10-11 -->
# tests/visual/baselines

V-BASELINE-0 controlled visual reference corpus for objective before/after
comparison of StaticPropOpaque-lane visual changes.

## Layout

```
tests/visual/baselines/
  README.md              this file
  presets.json           camera-preset registry (mission + warmup tuples)
  <preset>_<sha>.png     captured frame
  <preset>_<sha>.json    sidecar metadata
  <preset>_<sha>.log     mc2.exe stdout/stderr captured during the run
```

## Naming

`<preset-name>_<commit-short>.{png,json,log}`

- `preset-name` — key from `presets.json` (e.g. `staticprop_baseline_01`)
- `commit-short` — `git rev-parse --short HEAD` at capture time

Both files in a pair share the same stem so the sidecar always travels with
its PNG.

## Sidecar JSON schema (v1)

```json
{
  "commit": "11f7f089",
  "mission": "mc2_24",
  "cameraPreset": "staticprop_baseline_01",
  "resolution": "1920x1080",
  "flags": {
    "MC2_VIEW_UNIFORMS": "default",
    "MC2_SNAPSHOT_STATIC_PROP_BUILD": "default",
    "...": "..."
  },
  "capture": {
    "warmup_s": 28,
    "duration_s": 30,
    "smoke_seed": "0xC0FFEE",
    "captured_at": "2026-05-27T...",
    "png_sha256": "...",
    "png_bytes": 1234567
  },
  "preset_description": "..."
}
```

The top-level keys `commit`, `mission`, `cameraPreset`, `resolution`, and
`flags` match the V-BASELINE-0 brief verbatim. `capture` and
`preset_description` are additive metadata for traceability.

## Adding a new preset

1. Edit `presets.json`: add a new entry under `presets` with `mission`,
   `warmup_s`, `duration_s`, and a human-readable `description`.
2. Run `py -3 scripts/capture_baseline.py --preset <new_name>`.
3. Commit the resulting `.png` + `.json` (and optionally the `.log`).

## Capturing baselines at a new commit

```
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 scripts/capture_baseline.py                # all presets
py -3 scripts/capture_baseline.py --preset staticprop_baseline_01
```

Each invocation reads `git rev-parse --short HEAD` and embeds it in the
filename and sidecar so multiple commits' baselines coexist side by side.

## Comparing before/after

See `docs/visual-baseline-howto.md` for the full workflow.
