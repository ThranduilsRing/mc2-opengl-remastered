<!-- doc-status: current | reviewed:2026-10-11 -->
# Tactical-presentation visual-state capture & validation

Capture/validation matrix for `GAMEADAPTERS-VISUAL-STATE-BRIDGE-OPUS-1`.
Extends `docs/viewmode-capture-matrix.md` with the new bridge state.
Deploy target: `A:/Games/mc2-opengl/mc2-win64-v0.4`.

> **Process:** Ensure no `mc2.exe` is running, set env in the parent shell,
> then launch. **NEVER `--kill-existing`.** Smoke runs are isolated:
> `py -3 scripts\run_smoke.py --mission <m> --duration 30 --keep-logs`.

## What shipped

| Slice | Behavior | Default | How to see it |
|-------|----------|---------|---------------|
| 1 `MECH-VISUAL-STATE-BRIDGE-1` | per-mech `heat01`/`damage01`/`flags` mirrored to renderer SSBO + debug dump | OFF (no shader consumes it) | debug-state JSON, `MC2_SNAPSHOT_MECH_EXTRACT=1` |
| 3 `TACTICAL-OVERLAY-SELECTED-MECH-DATA-1` | range bands (min/opt/max) + firing-arc spokes on selected mover | OFF | `MC2_DEBUG_RENDERER=1` + `MC2_TACTICAL_ARC_OVERLAY=1`, select a mech |
| 2 `THERMAL-VIEW-MECH-HOT-1` | Thermal: engine-bearing units (mechs) read HOT, rest = luminance iron palette | ViewMode-only | `MC2_VIEWMODE_FRAMEWORK=1 MC2_VIEW_MODE=thermal MC2_OBJECT_ID_BUFFER=1` |
| 4 Sensor | DEFERRED (firewall) — recon doc only | n/a | see `sensor-contact-presentation-recon.md` |

Vehicles read cool in Thermal (they render via the static-prop batcher; no
distinct identity yet). Vehicles-hot is a follow-up — see
`thermal-view-mech-heat-mvp-defer.md`.

## Missions
- **mc2_24** — ~46 mechs; best mech density for the visual-state dump and
  overlay clutter check.
- **mc2_17** — combined arms (~12 mechs + dense props); overlay density.
- **mc2_03** — open terrain; clean single-selection overlay shot.
- **mc2_01** — light; quick gate-OFF byte-identical regression.

## Env vars

| Var | Default | Effect |
|-----|---------|--------|
| `MC2_SNAPSHOT_MECH_EXTRACT` | OFF | emit the mech section (incl. `heat01`/`damage01`/`visualFlags`) in debug-state JSON |
| `MC2_DEBUG_STATE_DUMP` | OFF | enable debug-state JSON dump (refreshes every 300 frames) |
| `MC2_DEBUG_RENDERER` | OFF | enable the world-space debug primitive pass (required for the overlay to flush) |
| `MC2_TACTICAL_ARC_OVERLAY` | OFF | draw range bands + facing + firing-arc on selected movers |
| `MC2_VIEWMODE_FRAMEWORK` | OFF | ViewMode composite framework (Thermal placeholder etc.) |
| `MC2_VIEW_MODE` | 0 | seed startup ViewMode `0..5` (`visual|objectid|tactical|thermal|infrared|lowlight`) |

## Manual validation steps

### A. Default gate-OFF — byte-identical regression (required)
No bridge env set. The Slice-1 SSBO fields are written but no shader reads
them, and the overlay/ViewMode gates are off.
```powershell
py -3 scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expect tier1 5/5 PASS, 0 GL errors. This is the merge-safety gate.

### B. Slice 1 — visual state in the debug dump
```powershell
$env:MC2_DEBUG_STATE_DUMP = "1"; $env:MC2_SNAPSHOT_MECH_EXTRACT = "1"
py -3 scripts\run_smoke.py --mission mc2_24 --duration 30 --keep-logs
```
Then inspect the dumped JSON `mech.packets[]` — each row carries `heat01`
(0.0, USEHEAT off), `damage01` (0.0 pristine .. 1.0 wrecked), and `visualFlags`
(bit0 selected, bit1 shutdown, bit2 disabled, bit3 destroyed, bits[4:5]
relation 0=own/1=ally/2=enemy). Or query live via the `mc2-render-state` MCP
(`get_render_state`) while the engine runs with the dump enabled.

Expect: own-team mechs show relation 0; damage01 climbs as a mech takes armor
damage; a destroyed mech sets bit3. No NaN/Inf in damage01 (sanitized).

### C. Slice 3 — selected-mover tactical overlay
```powershell
$env:MC2_DEBUG_RENDERER = "1"; $env:MC2_TACTICAL_ARC_OVERLAY = "1"
py -3 scripts\run_smoke.py --mission mc2_03 --duration 60 --keep-logs
```
Drive the camera, select a mech. Expect: blue max-range ring, cyan optimal
ring (inside max), amber min/dead ring (if nonzero), green facing tick, two
yellow firing-arc edge spokes at facing ± half-arc. Deselect → overlay clears.
No GL errors. Selecting multiple units draws the overlay on each.

### C2. Slice 2 — Thermal engine-hot
```powershell
$env:MC2_VIEWMODE_FRAMEWORK = "1"; $env:MC2_VIEW_MODE = "thermal"; $env:MC2_OBJECT_ID_BUFFER = "1"
py -3 scripts\run_smoke.py --mission mc2_24 --duration 30 --keep-logs
```
Log shows `[VIEWMODE v1] framework=1 initialMode=3`, 0 GL errors. For the pixel
check, launch non-minimized and confirm mechs render hot (orange/white) against
cooler terrain/props; switch ViewMode via Graphics Options → Post-Process →
View Mode. (Smoke launches minimized, so a desktop screenshot won't show the
game — drive it manually for the visual.) Vehicles read cool (known follow-up).

### D. Static gates (always)
```bash
sh scripts/check-contracts.sh                       # 8/8 PASS
cmake --build build64-tests --config RelWithDebInfo --target mc2_tests
build64-tests/RelWithDebInfo/mc2_tests.exe "--ts=RenderCore"   # 50/50
```

## Notes
- Thermal/Sensor remain placeholders; do not present them as real sensor data.
  The Slice-1 `damage01` is the substrate a future Thermal-damage lane will
  consume (see the defer doc).
- The overlay's facing-tick sign is best-effort (Stuff `x=E,y=N,z=elev` →
  debug-world `x=E,y=up,z=N`); the rings are the rotation-independent cue.
