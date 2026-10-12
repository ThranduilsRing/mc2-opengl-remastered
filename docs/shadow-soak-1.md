<!-- doc-status: current | reviewed:2026-10-11 -->
# SHADOW-SOAK-1: First Debug/Tuning Soak

**Branch:** `claude/shadow-lane`  
**Date:** 2026-05-28  
**Purpose:** Collect pre-tuning baseline observations before any shadow quality changes.  
**Status:** READY — commands below, captures not yet run.

---

## What exists at soak time

| Shipped slice | Commit | What it added |
|---|---|---|
| SHADOW-VIEW-1 | d939de67 | EngineViews for ShadowDirectional0-Static + ShadowDynamic registered |
| SHADOW-RESOURCE-1 | cf7c6bbd | ShadowStaticMap + ShadowDynamicMap in RenderResourceRegistry |
| SHADOW-DEBUG-VIEWS-1 | f4581822 | Registries TreeNode in Shadow Debug panel |
| SHADOW-TUNING-1 | d7ac228c | shadowBiasFactor_/Units_ + Shadow Tuning collapsing panel |
| Sub-panel fix | 8436a282 | Shadow Tuning is now CollapsingHeader, not flat SeparatorText |

Shadow debug mode is **ImGui/hotkey-only** — no env var path exists yet. This is a gap
(candidate SHADOW-ENV-1 follow-up: add `MC2_SHADOW_DEBUG_MODE` env var).

---

## Hotkey reference

| Hotkey | Action |
|---|---|
| `Ctrl+Shift+G` | Toggle Graphics Options panel |
| `RAlt+F2` | Cycle shadow debug: OFF → STATIC → DYNAMIC → OFF |
| `RAlt+F4` | Cycle screen shadow factor: OFF → NORMAL → VISUALIZE → OFF |
| `[` / `]` | Decrease / increase shadow softness by 0.1 |

---

## Capture matrix

### Row 1 — Default render (no debug)

Deploy first, then run each command. Screenshots land in
`tests/smoke/artifacts/diag-shots/`.

```powershell
# From shadow-lane worktree root:
py -3 scripts\quick_shot.py mc2_03 28 soak1-default
py -3 scripts\quick_shot.py mc2_17 28 soak1-default
py -3 scripts\quick_shot.py mc2_24 28 soak1-default
```

**Observe:** terrain shadow acne / peter-panning / contact hardness.

---

### Row 2 — Static shadow map debug view (RAlt+F2 once)

The wait is 45s to give time to enable debug after the mission loads (~10s).
Press `RAlt+F2` once to switch to STATIC mode. Shadow map should appear
256×256 in the lower-left corner: magenta=unwritten, grayscale=depth.

```powershell
py -3 scripts\quick_shot.py mc2_03 45 soak1-static-debug
py -3 scripts\quick_shot.py mc2_17 45 soak1-static-debug
py -3 scripts\quick_shot.py mc2_24 45 soak1-static-debug
```

**Observe:** coverage (does the ortho frustum cover the full playable area?),
depth distribution, aliasing in high-slope terrain.

Or via ImGui: Ctrl+Shift+G → Shadow Debug ☑ → ● Static.

---

### Row 3 — Dynamic shadow map debug view (RAlt+F2 twice)

Press `RAlt+F2` twice (STATIC → DYNAMIC). Dynamic map is camera-centered;
expect tighter depth range than static.

```powershell
py -3 scripts\quick_shot.py mc2_24 45 soak1-dynamic-debug
```

mc2_24 is mech-heavy — best mission for dynamic shadow evaluation.

**Observe:** mech depth footprint, fidelity vs static, texel density.

---

### Row 4 — Screen shadow factor overlay (RAlt+F4 twice)

Press `RAlt+F4` twice (OFF → NORMAL → VISUALIZE). Visualize mode writes
the raw shadow factor to the screen: white=lit, dark=shadowed.

```powershell
py -3 scripts\quick_shot.py mc2_03 45 soak1-screen-shadow-viz
py -3 scripts\quick_shot.py mc2_17 45 soak1-screen-shadow-viz
```

**Observe:** shadow leaks onto sky/water, self-shadow on flat roofs,
agreement with static shadow map coverage.

---

### Row 5 — Softness sweep (manual ImGui or keyboard)

Via keyboard (`[` / `]`) or Ctrl+Shift+G → Shadow Tuning → Softness slider:

| Label | Softness | Command |
|---|---|---|
| low | 0.4 | `py -3 scripts\quick_shot.py mc2_03 45 soak1-soft-low` |
| default | 0.9 | `py -3 scripts\quick_shot.py mc2_03 45 soak1-soft-default` |
| high | 3.0 | `py -3 scripts\quick_shot.py mc2_03 45 soak1-soft-high` |

Reset to 0.9 after: Ctrl+Shift+G → Shadow Tuning → Reset.

**Observe:** shadow edge character, PCF bleeding onto lit areas at high values.

---

### Row 6 — Polygon offset sweep (ImGui only)

Via Ctrl+Shift+G → Shadow Tuning → Factor / Units sliders.
No keyboard shortcut. Adjust then run quick_shot.

| Label | Factor | Units | Command |
|---|---|---|---|
| no-offset | 0.0 | 0.0 | `py -3 scripts\quick_shot.py mc2_03 45 soak1-bias-zero` |
| default | 2.0 | 4.0 | `py -3 scripts\quick_shot.py mc2_03 45 soak1-bias-default` |
| heavy | 4.0 | 8.0 | `py -3 scripts\quick_shot.py mc2_03 45 soak1-bias-heavy` |

**Observe:** at zero: shadow acne should appear on terrain slopes.
At heavy: contact shadow may detach from casters (peter-panning).

---

### Row 7 — Debug-state JSON validation

With game running and `MC2_DEBUG_STATE_DUMP=1`:

```powershell
$env:MC2_DEBUG_STATE_DUMP = "1"
py -3 scripts\quick_shot.py mc2_03 60 soak1-state-dump
```

After 60s, check `debug_state\latest_render_state.json` for:
- `registeredViews` contains `ShadowDirectional0-Static`, `ShadowDynamic`
- `renderResources` contains `ShadowStaticMap valid=true 4096x4096 Depth24`
- `renderResources` contains `ShadowDynamicMap valid=true 4096x4096 Depth24`

---

## Tier1 smoke (run when ready to gate)

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\shadow-lane\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

---

## Observation log (fill in after running)

### Static map coverage
- mc2_03: _____
- mc2_17: _____
- mc2_24: _____

### Dynamic map coverage
- mc2_24 mechs visible in depth map: _____

### Screen shadow factor agreement with inline terrain shadow
- Leaks onto sky/water: _____
- Self-shadow on flat roofs: _____

### Acne / peter-panning at current defaults (factor=2.0, units=4.0)
- mc2_03 terrain slopes: _____
- mc2_24 mech contact: _____

### Softness character at defaults (0.9)
- Edge crispness: _____
- Bleed onto lit areas: _____

### Candidate tuning values (fill after sweep)
| Parameter | Current | Candidate | Rationale |
|---|---|---|---|
| shadowBiasFactor_ | 2.0 | | |
| shadowBiasUnits_ | 4.0 | | |
| terrain_shadow_softness_ | 0.9 | | |

---

## Gaps found / follow-up slices

- **SHADOW-ENV-1** (recommended): Add `MC2_SHADOW_DEBUG_MODE=0/1/2` env var so debug
  mode can be pre-set without ImGui interaction. Enables automated capture.
- **SHADOW-TUNING-2** (deferred): Change defaults only after soak confirms candidate
  values. Requires re-running this soak matrix with candidates before commit.
- **SHADOW-CAPTURE-PRESET-1** (deferred): Add shadow-specific presets to presets.json
  (e.g., `shadow_static_mc2_03`, `shadow_dynamic_mc2_24`) for repeatable baseline
  captures via capture_baseline.py (retired 2026-10-12, cull wave 1; use tools/visual_verify/capture.py).
- **Dynamic shadow polygon offset** (deferred): `beginDynamicShadowPass` has no
  `glPolygonOffset` call; may cause mech contact acne. Evaluate from soak Row 6 + Row 3.

---

## Recommended next slice after soak

If soak confirms acne at default bias or softness character needs adjustment:
→ **SHADOW-TUNING-2**: change defaults to soak-confirmed candidate values.

If soak reveals dynamic shadow has no polygon offset but shows contact acne:
→ **SHADOW-TUNING-2** scope expands to include `beginDynamicShadowPass` glPolygonOffset.

If everything looks acceptable and gaps are tooling only:
→ **SHADOW-ENV-1** first, then **SHADOW-CAPTURE-PRESET-1**.
