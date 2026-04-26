# PROJECTZ Stress Capture Checklist

**Purpose:** Drive each scenario manually with `MC2_PROJECTZ_TRACE=1 SUMMARY=1
HEATMAP=1` so the per-vertex/per-tri trace log is captured. The aggregator
needs the trace log to produce per-callsite × per-predicate CSVs (summary
mode only gives 2 of 5 predicates per callsite).

**Helper:** `scripts/projectz_capture_stress.py <mission> <scenario>`

That script:
- wipes any stale `mc2_projectz.log` in the deploy dir
- exports the three env vars
- launches `mc2.exe --profile stock --mission <mission>`
- streams + writes stdout to `docs/superpowers/specs/projectz-captures/<mission>__<scenario>.stdout.log`
- on game exit, copies `mc2_projectz.log` →
  `docs/superpowers/specs/projectz-captures/<mission>__<scenario>.trace.log`

Trace mode runs at ~35 FPS (per [projectz_trace.cpp:184](../../../mclib/projectz_trace.cpp)) — that's expected; the slowdown is float
formatting in the per-vertex sidecar writer, not anything wrong.

## Per-scenario action plan

### Run for one scenario

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/projectz-hunting
py -3 scripts/projectz_capture_stress.py mc2_01 stress_zoom_out
```

Then perform the camera action below for ~30-60s. **Close the game (Esc → Quit
to Desktop)** when done — that copies the trace log and runs the aggregator
hint at the bottom.

After exit, run the aggregator (the script prints the exact command):

```bash
py -3 scripts/projectz_aggregate.py \
    docs/superpowers/specs/projectz-captures/mc2_01__stress_zoom_out.trace.log \
    --out docs/superpowers/specs/projectz-captures/mc2_01__stress_zoom_out.csv \
    --tri-out docs/superpowers/specs/projectz-captures/mc2_01__stress_zoom_out.tri.csv
```

### Screenshots

For each scenario, **press RAlt+P to toggle the predicate overlay** and take
a screenshot if you see RED or PURPLE triangles. Save to:

```
docs/superpowers/specs/projectz-captures/<mission>__<scenario>.<colour>.png
```

(filenames freeform; the report references them.)

The five overlay colors (per [spec §Color-Coded Debug Overlay](../2026-04-25-projectz-containment-design.md)):
- Green: legacy + chosen predicate agree
- Yellow: legacy accepts, candidate rejects (over-cull risk)
- Orange: candidate accepts, legacy rejects (wedge risk if we switch)
- Red: submitted triangle contains a legacy-rejected vertex (the wedge condition)
- Purple: NaN/Inf in pipeline OR projected triangle area > 250000 px

We need at least one screenshot of each color, ANY scenario, ANY mission. If
orange is unobserved (commit-4 analysis suggested it may be unreachable),
that's documented; do not fabricate.

## Scenario matrix

Priority levels: ★★★ = please do, ★★ = if time, ★ = nice to have.

Spec §Capture Methodology asks for each tier1 mission × each scenario. That's
25 runs and unrealistic. Recommended minimum coverage:

| Mission | baseline | zoom_out | edge_pan | rotate_pan | occlusion | altitude |
|---------|----------|----------|----------|------------|-----------|----------|
| mc2_01  | (auto ✓) | ★★★      | ★★★      | ★★★        | ★★★       | ★★ if launches |
| mc2_03  | (auto ✓) | ★★       | ★        | ★          | ★         | —        |
| mc2_10  | (auto ✓) | ★★       | ★★       | ★          | ★★        | —        |
| mc2_17  | (auto ✓ — but baseline timed out at 180s; see report) | ★ | ★ | — | — | — |
| mc2_24  | (auto ✓) | ★★       | ★        | ★          | ★         | —        |

**Minimum-acceptable run set: 5 stress runs on mc2_01.** Anything beyond is
gravy and tightens the report, not load-bearing for the verdict.

## Scenario actions

For each scenario, perform these camera actions for ~30-60s after the mission
loads. Standard MC2 RTS controls assumed (F-keys = camera, mouse-edge =
pan, mouse wheel = zoom).

### `stress_zoom_out`
Mouse-wheel zoom all the way out (or hold the zoom-out key). Hold at max
zoom for 30s. Slowly zoom partway in and back out twice.

### `stress_edge_pan`
Pan camera to **all four map edges** in sequence (N → E → S → W). At each
edge, dwell 5-10s. Watch the overlay as terrain at the map edge enters/exits
the viewport — the cull boundary is where wedge candidates appear.

### `stress_rotate_pan`
Spin the camera 90°+ in one direction, then back. Then 360° rotation
(keyboard or middle-mouse-drag). 30s total.

### `stress_occlusion`
Find a hill or ridge. Drive the camera AT it from a low angle so the ridge
occludes the far terrain. Focus on the moment the ridge crosses the bottom
edge of the viewport.

### `stress_altitude` (only mc2_01, only if launchable)
Push camera to max altitude (Wolfman-mode if hotkey is wired on this build —
see [memory mc2x_wolfman_reference.md](../../../../memory/mc2x_wolfman_reference.md)). If altitude clamps at the
default ceiling, label this scenario `stress_altitude_default_ceiling` and
note the cap.

## When you're done

Tell me which scenarios you ran and which ones you skipped. I'll re-aggregate
into the per-scenario CSVs and finalize the report's per-scenario findings +
verdict section.
