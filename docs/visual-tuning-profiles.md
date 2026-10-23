<!-- doc-status: current | reviewed:2026-10-11 -->
# Visual Tuning Profiles — MISSION-VISUAL-TUNING-1

Per-mission renderer tuning profiles. Lets specific missions set their own
default values for exposure, shadow softness, terrain lighting, and other
renderer tunables without changing global engine defaults.

## File location

`data/visual_tuning.json` relative to the game working directory.

Override with env var `MC2_VISUAL_TUNING_FILE=path/to/file.json`.

Missing file: silent no-op (current behavior unchanged).

## Precedence

```
1. Engine hardcoded defaults
2. Profile "defaults" block         ← lowest profile level
3. Profile "missions".<name> block  ← mission-specific overrides
4. Env vars (MC2_*)                 ← always override profile
5. ImGui live sliders               ← highest priority
```

Env vars that cover a specific key are checked at load time; if set, the
profile value is skipped and the env var value remains authoritative.

## Schema

```json
{
  "defaults": {
    "exposure":                   1.0,
    "shadowSoftness":             0.9,
    "staticPropIblStrength":      0.5,
    "waterSkyTintStrength":       0.0,
    "vfxBrightness":              1.0,
    "vfxAdditiveBrightness":      1.0
  },
  "missions": {
    "mc2_24": {
      "exposure": 1.1
    }
  }
}
```

Unknown keys: one-time warning to stderr, no crash.
Invalid values: clamped to safe range where applicable.

## Supported keys (v1)

| Key | Type | Range | Env var (overrides) | Notes |
|-----|------|-------|---------------------|-------|
| `exposure` | float | ≥0 | — | HDR composite exposure multiplier |
| `shadowSoftness` | float | 0–1 | — | Shadow PCF softness |
| `staticPropIblStrength` | float | 0–3 | — | IBL SH contribution |
| `waterSkyTintStrength` | float | ≥0 | `MC2_WATER_SKYTINT` | Sky horizon tint on water |
| `vfxBrightness` | float | 0–8 | — | Particle overall brightness |
| `vfxAdditiveBrightness` | float | 0–8 | — | Additive blend brightness |
| ~~`bloomThreshold`~~ | — | — | — | REMOVED 2026-06-22 (DEAD-POST-FX-CLEANUP-1): bloom deleted as wrong-for-RTS; key now ignored on load |
| ~~`bloomIntensity`~~ | — | — | — | REMOVED 2026-06-22 (DEAD-POST-FX-CLEANUP-1): key now ignored on load |
| `aoRadius` | float | ≥0 | — | SSAO sample radius (visible only with `MC2_SSAO`) |
| `aoStrength` | float | ≥0 | — | SSAO darkening strength (same gating) |
| `aoBias` | float | ≥0 | — | SSAO depth bias (same gating) |
| `mechSpecularStrength` | float | 0–4 | `MC2_MECH_SPECULAR_STRENGTH` | Mech Blinn specular strength (gate `MC2_MECH_SPECULAR_V1`, default-ON) |
| `vfxLitStrength` | float | 0–1 | — | Per-mission lit-smoke strength (gate `MC2_VFX_LIT_PARTICLES` still controls whether lighting is applied at all) |
| `terrainClassGrassGmRLo` | float | — | — | Terrain grass classifier: green-minus-red lower threshold |
| `terrainClassGrassGmRHi` | float | — | — | Terrain grass classifier: green-minus-red upper threshold |
| `terrainClassGrassBrLo` | float | — | — | Terrain grass classifier: brightness lower threshold |
| `terrainClassGrassBrHi` | float | — | — | Terrain grass classifier: brightness upper threshold |
| `terrainClassDirtRmGLo` | float | — | — | Terrain dirt classifier: red-minus-green lower threshold |
| `terrainClassDirtRmGHi` | float | — | — | Terrain dirt classifier: red-minus-green upper threshold |
| `terrainClassDirtBrLo` | float | — | — | Terrain dirt classifier: brightness lower threshold |
| `terrainClassDirtBrHi` | float | — | — | Terrain dirt classifier: brightness upper threshold |

Keys not in this list are silently skipped (warn once). The shipped
`data/visual_tuning.json` `defaults` block intentionally lists only a subset;
any key absent from the file falls back to the engine default (current behavior).
Both the reader (`applyKey`) and the writer (`visualTuning_saveCurrentToMission`,
the "Set as Mission Defaults" button) cover this full list, so live edits to any
of these round-trip.

## Load timing

Profile is applied once at mission load after `missionScriptName` is set
(`mission.cpp: Mission::init`).  On mission reload, profile is re-applied
(ImGui edits from previous session reset to profile values for that mission).

## ImGui panel

Graphics Options window → **Visual Tuning Profile** collapsing section shows:
- Current profile file path
- Load status (green = loaded, orange = missing)
- Active mission name and key count
- **Reset to Profile** button — re-applies current profile mid-session

## Adding a new mission entry

Open `data/visual_tuning.json`, add an entry under `"missions"` with the
scenario script name (lowercase, e.g. `"mc2_05"`):

```json
"missions": {
    "mc2_05": {
        "exposure": 0.9,
        "shadowSoftness": 0.7
    }
}
```

Restart mission or press **Reset to Profile** in Graphics Options.

## Adding a new tunable key (future)

1. Add a `strcmp` branch in `applyKey()` in `visual_tuning_profile.cpp`.
2. Call the existing `gos_Set*` function or write directly to the extern float.
3. Add env var check via `envIsSet()` if a corresponding env var exists.
4. Add a row to the table above.

## Modding / overlays

`data/visual_tuning.json` is read via a raw `fopen()` against the process's
current working directory (`GameOS/gameos/visual_tuning_profile.cpp:301-303`,
`317`) — it does **not** go through the mod VFS / file-resolution ladder
(`mclib/file.cpp`'s mod-overlay layer) the way `data/objects/*.csv` and other
mod-visible assets do. A mod's own `data/visual_tuning.json` will **not** be
picked up automatically just by activating the mod with `MC2_ACTIVE_MOD`.

To point the engine at a mod-shipped tuning file, set
`MC2_VISUAL_TUNING_FILE=<path-to-your-file>` explicitly (verify at
`GameOS/gameos/visual_tuning_profile.cpp:313-315`: `pathEnv` from
`getenv("MC2_VISUAL_TUNING_FILE")` wins over the `data/visual_tuning.json`
default when set and non-empty; `visualTuning_saveCurrentToMission`'s
read-modify-write at line 400 uses the same resolved `s_profilePath`, so "Set
as Mission Defaults" also writes to whichever file `MC2_VISUAL_TUNING_FILE`
named). Since `mc2.exe` is normally launched with CWD at the deploy root, a
relative path here is relative to that root, not to the mod folder.

The `docs/modding/examples/imgui-editing/README.md` example mod ships exactly
this pattern: a `launch-with-tuning.ps1` script that sets both
`MC2_ACTIVE_MOD` and `MC2_VISUAL_TUNING_FILE` before launching `mc2.exe`. See
that doc's "Running the launch script" note for the Windows `.ps1`
double-click gotcha (default file association is Edit, not Run).

## Showcase graded pass — LIGHTING-STAGE1 SHOWCASE-TUNING-1 (2026-07-01)

Deliberate per-mission grade across all of tier1 (`mc2_01/03/10/17/24`) plus the
`gaea_peaks_01` generated-terrain showcase mission, seeded from the `mc2_17`
profile that shipped with GROUND-CONTACT-BLOB-1. Goal per
`.claude/LIGHTING-MODERNIZATION-PROPOSAL-1.md` Stage 1: floor lifts (kill
shadow black-crush at RTS camera distance), a small IBL/exposure dial per
mission's dominant lighting mood, and mech ambient fill consistent with the
terrain floor bump — all data-only, no code changes, no gate flips.

**Global `defaults` change:** `terrainLightingV2Floor` (since retired; installs that still carry it ignore it) 0.3 -> 0.32 and
`staticPropIblStrength` 0.5 -> 0.55. Small baseline lift so *every* mission
without an explicit override (not just the 6 curated ones) gets a touch less
crush in fully-shadowed terrain and a touch more prop IBL fill — the two
cheapest, safest global levers per the proposal's "calibrate" framing.

Per-mission intent (all values additive tweaks off the new defaults; keys not
listed for a mission fall back to `defaults`):

| Mission | `staticPropIblStrength` | `exposure` |
|---|---|---|
| `mc2_01` | 0.6 | default 1.0 |
| `mc2_03` | 0.55 | default 1.0 |
| `mc2_10` | 0.6 | default 1.0 |
| `mc2_17` | default 0.55 | default 1.0 |
| `mc2_24` | 0.6 | default 1.0 |
| `gaea_peaks_01` | 0.65 | default 1.0 |

The legacy terrain-floor, shadow-softness and mech-ambient profile controls are retired.

This is a "calibrate" pass, not a final grade: values are chosen to be visibly
different from the pre-2026-07-01 flat profile (only `mc2_24.exposure` and the
`mc2_17` seed existed before) without touching any gate, shader, or C++ file.
Stage 2+ (`TERRAIN-SH-AMBIENT-1` etc., see the proposal doc) is where the
underlying lighting model changes; this pass only re-points the existing
dials.