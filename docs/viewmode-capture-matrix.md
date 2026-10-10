<!-- doc-status: current | reviewed:2026-10-11 -->
# ViewMode capture matrix (TACTICAL-PRESENTATION-CAPTURE-MATRIX-1)

Track V tactical-presentation opus (`TRACKV-TACTICAL-PRESENTATION-OPUS-1`).
Capture presets + commands for each ViewMode. Source-of-truth env sets live in
`tests/smoke/matrices/viewmode.json`; this doc explains what each captures and
how to drive it.

## How ViewMode is selected

ViewMode is a fullscreen **postprocess composite** concern. Two env vars:

- `MC2_VIEWMODE_FRAMEWORK` — master gate, **default OFF**. When OFF the
  composite forces mode 0 (Visual) and the output is byte-identical to
  baseline; the ImGui selector is not drawn.
- `MC2_VIEW_MODE` — seeds the startup mode. Accepts a number `0..5` matching
  `RenderCore::ViewMode`, or a name: `visual|objectid|tactical|thermal|infrared|lowlight`.

Mode values: `0 Visual`, `1 ObjectIdDebug`, `2 TacticalOverlay`,
`3 Thermal (placeholder)`, `4 Infrared (no implementer)`, `5 LowLight`.

Live mid-session switching is via the Graphics Options → **Post-Process →
View Mode** combo (only shown when the framework gate is ON). Env seeds the
startup mode; the combo overrides it.

> **TacticalOverlay (2) is NOT a composite mode.** The range-ring / facing
> overlay is drawn in-scene by the debug renderer and is gated separately by
> `MC2_DEBUG_RENDERER=1` + `MC2_TACTICAL_ARC_OVERLAY=1`. Selecting composite
> mode 2 is a pass-through (Visual).

## Missions

- **mc2_03** — salvage / rocky outcrop. Open terrain, sparse props; best for
  sensor-mode silhouette contrast (thermal placeholder, low-light).
- **mc2_17** — combined-arms stress (~12 mechs + dense props). Best for
  ObjectIdDebug palette coverage and overlay density.
- **mc2_24** — steep urban + final campaign; shadow-cascade stress. Good
  full-scene comparison shot.

## Per-mode capture commands (PowerShell — NEVER `--kill-existing`)

Ensure no `mc2.exe` is running, then set env in the parent shell and launch.
`scripts/run_smoke.py --mission` runs an isolated capture; `--keep-logs`
preserves artifacts under `tests/smoke/artifacts/<timestamp>/`.

**Visual baseline**
```powershell
py -3 scripts\run_smoke.py --mission mc2_03 --mission mc2_17 --mission mc2_24 --duration 30 --keep-logs
```

**ObjectIdDebug**
```powershell
$env:MC2_VIEWMODE_FRAMEWORK="1"; $env:MC2_VIEW_MODE="1"; $env:MC2_OBJECT_ID_BUFFER="1"
py -3 scripts\run_smoke.py --mission mc2_17 --mission mc2_24 --duration 30 --keep-logs
Remove-Item Env:\MC2_VIEWMODE_FRAMEWORK,Env:\MC2_VIEW_MODE,Env:\MC2_OBJECT_ID_BUFFER
```

**Thermal (luminance placeholder)**
```powershell
$env:MC2_VIEWMODE_FRAMEWORK="1"; $env:MC2_VIEW_MODE="3"
py -3 scripts\run_smoke.py --mission mc2_03 --mission mc2_17 --duration 30 --keep-logs
Remove-Item Env:\MC2_VIEWMODE_FRAMEWORK,Env:\MC2_VIEW_MODE
```

**LowLight / NightVision**
```powershell
$env:MC2_VIEWMODE_FRAMEWORK="1"; $env:MC2_VIEW_MODE="5"
py -3 scripts\run_smoke.py --mission mc2_03 --mission mc2_24 --duration 30 --keep-logs
Remove-Item Env:\MC2_VIEWMODE_FRAMEWORK,Env:\MC2_VIEW_MODE
```

**TacticalOverlay (in-scene; select a unit to see it)**
```powershell
$env:MC2_DEBUG_RENDERER="1"; $env:MC2_TACTICAL_ARC_OVERLAY="1"
py -3 scripts\run_smoke.py --mission mc2_17 --duration 60 --keep-logs
Remove-Item Env:\MC2_DEBUG_RENDERER,Env:\MC2_TACTICAL_ARC_OVERLAY
```

## What to compare

1. `default` vs `visual_explicit` (mc2_24): must be pixel-identical — proves the
   framework adds no Visual-path cost.
2. `default` vs `object_id_debug` (mc2_17): every mech/static a distinct color;
   terrain/sky neutral dark grey.
3. `object_id_debug_fallback` (`MC2_OBJECT_ID_BUFFER=0`): output == Visual, one
   `[VIEWMODE v1] ObjectIdDebug requested but sceneObjectIdTex_=0 ... Visual`
   line in the log, no GL error, no crash.
4. `thermal_placeholder` vs `low_light` (mc2_03): thermal = iron palette keyed
   on brightness; low-light = green-tinted luminance boost.
5. `low_light_high_gain` (gain 5.0): no blown-white frame (soft-knee holds).

## Env-selectable vs interactive

| Mode | Env-selectable | Notes |
|------|----------------|-------|
| Visual / ObjectIdDebug / Thermal / LowLight | yes | `MC2_VIEW_MODE` at startup |
| TacticalOverlay | partial | env enables it, but a unit must be **selected in-game** to draw anything |
| Live mode switch | ImGui only | Post-Process → View Mode combo |

## Regression gate (tier1)

```powershell
# Confirm no mc2.exe is running first. NEVER pass --kill-existing.
py -3 scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```

Then the framework-armed (gate ON, Visual) byte-identical check + a mode smoke:
```powershell
$env:MC2_VIEWMODE_FRAMEWORK="1"; $env:MC2_VIEW_MODE="1"; $env:MC2_OBJECT_ID_BUFFER="1"
py -3 scripts\run_smoke.py --mission mc2_17 --duration 30 --keep-logs
Remove-Item Env:\MC2_VIEWMODE_FRAMEWORK,Env:\MC2_VIEW_MODE,Env:\MC2_OBJECT_ID_BUFFER
```

## Tooling notes

- `MC2_VIEWMODE_FRAMEWORK`, `MC2_VIEW_MODE`, `MC2_VIEWMODE_LOWLIGHT_GAIN`,
  `MC2_VIEWMODE_LOWLIGHT_TINT`, and `MC2_TACTICAL_ARC_OVERLAY` must be in the
  `run_smoke.py` subprocess env passthrough for matrix runs to propagate them.
- `run_smoke_matrix.py` historically injects `--kill-existing`; prefer the
  direct `run_smoke.py --mission` flow above for ViewMode captures.
