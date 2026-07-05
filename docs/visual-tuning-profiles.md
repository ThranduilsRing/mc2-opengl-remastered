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
    "terrainLightingV1Strength":  1.0,
    "terrainLightingV2Floor":     0.3,
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
| `terrainLightingV1Strength` | float | 0–2 | — | Hemi ambient lighting strength |
| `terrainLightingV2Floor` | float | 0–1 | — | Shadow-aware fill floor level |
| `staticPropIblStrength` | float | 0–3 | `MC2_STATIC_PROP_IBL_SH_STRENGTH` | IBL SH contribution |
| `waterSkyTintStrength` | float | ≥0 | `MC2_WATER_SKYTINT` | Sky horizon tint on water |
| `vfxBrightness` | float | 0–8 | `MC2_TUNE_VFX_BRIGHTNESS` | Particle overall brightness |
| `vfxAdditiveBrightness` | float | 0–8 | `MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS` | Additive blend brightness |
| ~~`bloomThreshold`~~ | — | — | — | REMOVED 2026-06-22 (DEAD-POST-FX-CLEANUP-1): bloom deleted as wrong-for-RTS; key now ignored on load |
| ~~`bloomIntensity`~~ | — | — | — | REMOVED 2026-06-22 (DEAD-POST-FX-CLEANUP-1): key now ignored on load |
| `aoRadius` | float | ≥0 | — | SSAO sample radius (visible only with `MC2_SSAO`) |
| `aoStrength` | float | ≥0 | — | SSAO darkening strength (same gating) |
| `aoBias` | float | ≥0 | — | SSAO depth bias (same gating) |
| `mechAmbientStrength` | float | 0–2 | `MC2_MECH_AMBIENT_V1_STRENGTH` | Mech hemisphere ambient fill (gate `MC2_MECH_AMBIENT_V1`, default-ON) |
| `mechSpecularStrength` | float | 0–4 | `MC2_MECH_SPECULAR_STRENGTH` | Mech Blinn specular strength (gate `MC2_MECH_SPECULAR_V1`, default-ON) |

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

## Showcase graded pass — LIGHTING-STAGE1 SHOWCASE-TUNING-1 (2026-07-01)

Deliberate per-mission grade across all of tier1 (`mc2_01/03/10/17/24`) plus the
`gaea_peaks_01` generated-terrain showcase mission, seeded from the `mc2_17`
profile that shipped with GROUND-CONTACT-BLOB-1. Goal per
`.claude/LIGHTING-MODERNIZATION-PROPOSAL-1.md` Stage 1: floor lifts (kill
shadow black-crush at RTS camera distance), a small IBL/exposure dial per
mission's dominant lighting mood, and mech ambient fill consistent with the
terrain floor bump — all data-only, no code changes, no gate flips.

**Global `defaults` change:** `terrainLightingV2Floor` 0.3 -> 0.32 and
`staticPropIblStrength` 0.5 -> 0.55. Small baseline lift so *every* mission
without an explicit override (not just the 6 curated ones) gets a touch less
crush in fully-shadowed terrain and a touch more prop IBL fill — the two
cheapest, safest global levers per the proposal's "calibrate" framing.

Per-mission intent (all values additive tweaks off the new defaults; keys not
listed for a mission fall back to `defaults`):

| Mission | `terrainLightingV2Floor` | `shadowSoftness` | `staticPropIblStrength` | `mechAmbientStrength` | `exposure` | Why |
|---|---|---|---|---|---|---|
| `mc2_01` | 0.36 | 1.1 | 0.6 | 0.24 | (default 1.0) | Dense-forest mission (see `run_smoke.py` instance-count note) — canopy casts heavy terrain shadow; floor lift keeps undergrowth readable, softness+IBL bump lift the shadowed forest floor without flattening it. |
| `mc2_03` | 0.34 | 1.0 | 0.55 | 0.22 | (default 1.0) | Mildest bump of the set — used as the "restrained" reference point between the unlifted baseline and the more dramatic missions. |
| `mc2_10` | 0.4 | 1.2 | 0.6 | 0.26 | 1.05 | Bigger floor lift + slight exposure bump — mission reads darker/more enclosed at default; the pair keeps shadowed geometry legible at top-down RTS zoom. |
| `mc2_17` | 0.45 | 1.4 | (default 0.55) | 0.28 | (default 1.0) | Unchanged seed values (GROUND-CONTACT-BLOB-1 origin) — the most aggressive floor+softness pairing in the set, kept as the upper reference point the other missions grade toward. |
| `mc2_24` | 0.38 | 1.15 | 0.6 | 0.25 | 1.1 | Existing `exposure: 1.1` override kept; floor/softness/IBL/mech keys added so the mission's shadow floor and prop fill match its already-brighter exposure instead of exposure being the only lever pulling it apart from its neighbors. |
| `gaea_peaks_01` | 0.4 | 1.25 | 0.65 | 0.26 | 1.05 | Generated mountain terrain (`tools/terrain_gen/`) — tallest relief in the set, so cast shadows are longest; highest IBL of the pass compensates for large permanently-shadowed slope faces reading pure black. |

This is a "calibrate" pass, not a final grade: values are chosen to be visibly
different from the pre-2026-07-01 flat profile (only `mc2_24.exposure` and the
`mc2_17` seed existed before) without touching any gate, shader, or C++ file.
Stage 2+ (`TERRAIN-SH-AMBIENT-1` etc., see the proposal doc) is where the
underlying lighting model changes; this pass only re-points the existing
dials.
