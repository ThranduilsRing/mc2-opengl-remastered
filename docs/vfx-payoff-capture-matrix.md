<!-- doc-status: current | reviewed:2026-10-11 -->
# VFX payoff capture matrix (VFX-PAYOFF-CAPTURE-MATRIX-1)

Track V VFX-payoff opus, slice 5. Capture presets + commands for the
soft/lit/bloom payoff stack. Source-of-truth env sets live in
`tests/smoke/matrices/vfx.json`; this doc explains what each captures and how
to grab frames safely.

## Missions

- **mc2_10** — primary VFX combat (urban, gosFX-heavy). Preset `vfx_combat_10`
  in the retired tests/visual/baselines/presets.json (retired 2026-10-12, cull wave 1). Best for missile smoke + weapon fire.
- **mc2_24** — mech-heavy / dense urban baseline (final campaign mission).
  Preset `vfx_combat_24`. PPC/energy + explosions; also the canonical mech
  baseline shared with the mech lane.

> Particle activity is **transient** under the passive smoke seed — a captured
> frame may or may not contain on-screen particles. Confirm via the mission log
> (`GOSFX_GPU ... sprites=N`, `TRAIL_PROBE`), not by assuming the frame is
> populated. There is no deterministic scripted-fire hook yet.

## Matrix entries (`tests/smoke/matrices/vfx.json`)

| id | gates | what to look for |
|---|---|---|
| `default` | none (all payoff OFF) | byte-identical baseline |
| `gpu_particles_off` | `MC2_GPU_PARTICLES=0` | legacy gosFX CPU path |
| `post_stack` | HDR+BLOOM+ACES | post contribution alone (no VFX gates) |
| `soft_particles` | `MC2_VFX_SOFT_PARTICLES=1` | soft smoke/dust at terrain/mech intersections (no hard billboard edges) |
| `lit_particles` | `MC2_VFX_LIT_PARTICLES=1` | smoke tinted by mission sun/ambient; additive flashes unchanged |
| `bloom_vfx` | HDR+BLOOM + profile `vfxAdditiveBrightness` 2.0 (the env seed was deleted in gate amnesty wave 1) | muzzle flashes / PPC / explosions glow |
| `payoff_full` | soft+lit+HDR+BLOOM+ACES | headline "after" shot |
| `debug_kind` | `MC2_VFX_DEBUG_MODE=3` | per-kind color (diagnostic) |
| `debug_overdraw` | `MC2_VFX_DEBUG_MODE=4` | blend buildup (diagnostic) |

## Capture commands (direct invocation — no `--kill-existing`)

`run_smoke.py` takes `--mission` (singular, repeatable). Set the entry's env in
the parent shell, then run. NEVER pass `--kill-existing` (it can crash an
in-flight mission to `crash_silent`). Ensure no `mc2.exe` is already running.

Baseline vs full payoff (PowerShell):

```powershell
# default / baseline
py -3 scripts/run_smoke.py --mission mc2_10 --mission mc2_24 --duration 30 --keep-logs

# full payoff stack
$env:MC2_VFX_SOFT_PARTICLES = "1"; $env:MC2_VFX_LIT_PARTICLES = "1"
$env:MC2_HDR_POST = "1"; $env:MC2_BLOOM = "1"; $env:MC2_TONEMAP_ACES = "1"
py -3 scripts/run_smoke.py --mission mc2_10 --mission mc2_24 --duration 30 --keep-logs
# (unset the env vars afterwards to return to the byte-identical default)
```

Individual payoff gates: set just the one entry's env from the table above.

### Screenshots

- smoke_with_screenshots.py (retired 2026-10-12, cull wave 1; use `tools/visual_verify/capture.py` over the bookmarks)
  -> `tests/smoke/artifacts/screenshots-<ts>/<mission>.{png,log,result}` + `summary.md`.
  Parks the cursor at screen center to defeat RTS edge-scroll.
- capture_baseline.py --preset vfx_combat_10 --vfx-debug-mode 3 (retired 2026-10-12, cull wave 1)
  -> wrote tests/visual/baselines/vfx_combat_10_<sha>_vdm3.png + JSON sidecar.

Read back with the `mc2-render-state` MCP server (engine running with
`MC2_DEBUG_STATE_DUMP=1`): `get_feature_gates` to confirm the gate env took
effect, `summarize_latest_capture` / `list_capture_sets` for results.

## What to compare

1. `default` vs `soft_particles` on mc2_10: smoke billowing around buildings —
   hard intersection lines should soften.
2. `default` vs `lit_particles`: smoke should pick up the sun/ambient tint of
   the mission; additive weapon fire should look identical.
3. `post_stack` vs `bloom_vfx` vs `payoff_full` on mc2_24: flash/PPC glow should
   appear with bloom; terrain/sky must not haze (see
   `docs/vfx-bloom-participation.md` caveats).
4. OS-grab compositor non-determinism gives 1-3 LSB pixel variance; pixel-exact
   is not enforced for these.

## Known tooling hazards (deferred — NOT fixed in this slice)

- `scripts/run_smoke_matrix.py` injects `--kill-existing` unconditionally
  (`run_smoke_matrix.py:88`), as does the MCP `run_capture_baseline` helper
  (which also passes a non-existent `--missions`). Both violate the
  no-kill-existing policy — **prefer the direct `run_smoke.py --mission` flow
  above** for VFX captures. Fixing the shared runner is out of this opus's
  scope (it affects every matrix); logged here as a follow-up.
- `docs/VISUAL-CAPTURE-MATRIX-1-DESIGN.md:110` still shows `--kill-existing` in
  its canonical tier1 command (stale vs current policy).
