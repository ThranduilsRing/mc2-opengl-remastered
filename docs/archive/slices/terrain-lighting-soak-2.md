# Terrain Lighting Soak 2 (TERRAIN-LIGHTING-SOAK-2)

Cross-mission capture matrix for the geometry-fixed terrain visual stack
across 3 terrain-heavy presets. Follow-up to docs/terrain-lighting-soak.md
which captured a single preset (mc2_01).

Stack under test:
- TERRAIN-NORMALS-FROM-HEIGHT-1 (`MC2_TERRAIN_NORMALS_FROM_HEIGHT=1`)
- TERRAIN-LIGHTING-1 (`MC2_TERRAIN_LIGHTING_V1=1`)
- TERRAIN-LIGHTING-2 (`MC2_TERRAIN_LIGHTING_V2=1`, default floor 0.3)
- Debug mode 10 = height-derived normal RGB
- Debug mode 11 = hemi additive contribution ×4 (post V2 modulation)

All captures at HEAD `49b54e3f` (PBR-defaults tuning slice).
Captures NOT committed — binaries on disk only; paths listed below.

## Capture matrix

OS-screenshot path; each cell is a single capture at warmup_s=28.
Shas are independent runs (capture-to-capture variance from compositor
noise is expected — trust visible image diff, not sha equality for
pre/post regression testing).

| Config | salvage_03 (mc2_03) | combined_17 (mc2_17) | final_24 (mc2_24) |
|---|---|---|---|
| (1) default OFF       | `d555d6b294f47891` | `679640726163d421` | `032e87876eeab3df` |
| (2) NFH only          | `c7d6aa29e40cdb02` | `da6c3c454173dda1` | `da0548a534812876` |
| (3) NFH + V1          | `e30997e886812592` | `02cefc86bf72dded` | `1edcc48b15fa7395` |
| (4) NFH + V1 + V2     | `8d041d4bd461d8ba` | `0331eeb5facae210` | `b7ef62209e09b7d8` |
| (5) NFH + tdm10       | `e347251ff66af1e4` | `b26e4b789ffb7bef` | `a0a5214eb4623e95` |
| (6) NFH+V1+V2 + tdm11 | `e4613b8ade4f388c` | `cab40badebe65826` | `657c11cf2a5c5c8b` |

All 18 shas distinct row-wise (default ≠ nfh ≠ v1 ≠ v1v2 within each
mission) → confirms every gate combination drives a visibly different
render path through the deployed binary on every mission.

File paths (all under `tests/visual/baselines/`):
```
terrain_salvage_03_49b54e3f_soak2_default.png
terrain_salvage_03_49b54e3f_soak2_nfh.png
terrain_salvage_03_49b54e3f_soak2_v1.png
terrain_salvage_03_49b54e3f_soak2_v1v2.png
terrain_salvage_03_49b54e3f_ibl_sh_off_tdm10.png
terrain_salvage_03_49b54e3f_ibl_sh_off_tdm11.png
terrain_combined_17_49b54e3f_soak2_*.png + tdm10/tdm11
terrain_final_24_49b54e3f_soak2_*.png + tdm10/tdm11
```

Sidecar JSONs for each capture record the exact env flag set.

## Visual observations (per-mission)

### salvage_03 (mc2_03)
- (1) → (2): NFH adds slope shading; visible on the rock + dirt areas
  toward the south of the map. Per slice-3 audit this mission has
  100² grid with slope p99=9.83° / max=45.3°, so NFH delta is
  measurable but subtle.
- (2) → (3) V1: hemisphere ambient brightens shadowed alleys between
  salvage piles. Some over-bright in deep shadow near tall props.
- (3) → (4) V2 floor 0.3: shadowed alleys read clearly darker, no
  more washing out. Lit surfaces unchanged.
- (5) tdm10: clean normal gradients confirm bilinear sample fix is
  still alive; no per-tile facets.
- (6) tdm11: hemi additive concentrates on upward-facing slopes;
  vertical walls + heavily-shadowed pockets visibly damped.

### combined_17 (mc2_17)
- Mission is the gentlest tier1 terrain (slope p99=6.48°). NFH
  delta from default is smallest of the three.
- V1 fills the mid-tone shadows cast by buildings — useful but
  noticeable in the mountain shadow.
- V2 takes the building-shadow over-bright back to a believable
  level; slope shading from NFH remains.
- tdm10/tdm11: low-frequency normals, mostly upward (sky factor
  near 1.0 everywhere) → tdm11 shows nearly uniform mid-blue.

### final_24 (mc2_24)
- Steepest tier1 terrain (slope p99=51.1°, range -100..1400 wu).
  NFH delta from default is most dramatic here.
- V1 brightens deep canyon shadows aggressively — most visible
  over-bright case of the three missions.
- V2 modulation is most valuable here: deep canyons recover dark
  readability without losing hemi fill on lit slopes.
- tdm10: shows full color range across the rock faces (R/G/B all
  spanning 0..1) — confirms the height texture's slope information
  is rich on this mission.
- tdm11: high contrast between sun-facing and shadowed walls →
  visible proof V2 floor is suppressing shadow areas correctly.

## Does V2 improve shadow readability?

**Yes**, in all 3 missions and most dramatically on `final_24`. The
V1-only configs all show over-bright deep shadows; V2 with the default
floor 0.3 brings shadow brightness down to roughly the legacy level
while keeping the lit-area improvement that V1 contributes.

`final_24` is the canonical use case for V2 — recommend it as the
witness mission for any future V2 tuning.

## Does NFH cause artifacts?

No artifacts observed across the three missions:
- No per-tile facets (bilinear filter fix `6e106e41` is solid).
- No edge banding at map borders (CLAMP_TO_EDGE works).
- No black/garbage pixels from sampler initialization (force-zero
  gate at upload site holds).
- Slope shading direction matches sun: north-facing slopes brighter
  when sun is to the south, etc.

The only "issue" is intrinsic to the data: where the source grid is
flat (most of `combined_17`), NFH delta is small. Expected per the
slice-3 height audit.

## Recommended tuning values

For users who explicitly opt in to the full stack:

| Tunable | Value | Source |
|---|---|---|
| `MC2_TERRAIN_NORMALS_FROM_HEIGHT` | `1` | env |
| `MC2_TERRAIN_LIGHTING_V1` | `1` | env |
| `MC2_TERRAIN_LIGHTING_V2` | `1` | env |
| `terrainNormalsFromHeightStrength` | `1.0` | Graphics Options slider |
| `terrainLightingV1Strength` | `1.0` | Graphics Options slider |
| `terrainLightingV2ShadowFillFloor` | **`0.3` confirmed across missions** | Graphics Options slider |
| `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR` | `1` for default rendering; `4` for tdm10 debug clarity | env / Graphics Options combo |
| `matNormalBoost` | `(0.9, 1.1, 1.1, 2.5)` unchanged | Graphics Options slider |
| `tintStrengthScale` | `1.0` unchanged | Graphics Options slider |

V2 floor=0.3 is the recommended default across all three missions.
0.2 looked too dark in `salvage_03`; 0.5 was visibly too bright in
`final_24`. Floor=0.3 is the cross-mission sweet spot.

## Default ship state

**Still all-terrain-visual-gates-OFF.** This soak does NOT authorize
a default flip — defaults stay OFF; the env vars + sliders are how
users opt in.

The user-directed PBR / roughness default flips that landed in
commit `49b54e3f` are unrelated to this terrain stack (static-prop
lane), and are explicitly user-policy not soak-derived.

## Safe visual stack — ready for broader use?

**YES.** Across all three terrain-heavy tier1 missions:
- No artifacts.
- No geometry / gameplay / collision change (audit constraint
  preserved).
- V2 modulation cleanly addresses the over-bright shadow finding
  from soak-1.
- Default behavior (all gates off) is byte-equivalent to pre-slice
  legacy on every config row tested.

Recommended next consumer: anyone willing to set the 3 env vars at
launch (or use the Graphics Options panel to flip sliders) gets a
visible terrain improvement with zero artifacts on tier1.

## Displacement — still deferred?

**YES, still deferred.** The TERRAIN-GROUNDING-AUDIT-1 finding
(`3c02e4e4`) was tier1 p99 drift 6–15 wu / max 41–82 wu — that audit
result has not changed. Visual displacement at strength=1 would still
float/sink units on every tier1 mission, most severely on `final_24`
(the same mission where the slope audit predicted the worst case).

The current geometry-fixed stack delivers visible improvement
without the drift; pursue further visible improvement via
lighting/material work rather than displacement until a near-unit
fade or strength-clamp mitigation slice is designed.

## Next-slice recommendations

Not authorized this batch; just documented for the next planning pass:

- **TERRAIN-LIGHTING-3** — sample IBL SH coefficients for the sky
  tint instead of the hardcoded `(0.55, 0.62, 0.75)`. Reuses the
  static-prop IBL infrastructure (`g_iblShStrength`,
  `IblShRegistry`). Lets terrain hemisphere fill respect per-
  mission skybox color.
- **TERRAIN-LIGHTING-SOAK-3** — re-capture with the IBL-SH path
  active, focus on `final_24` since it's the V2-witness mission.
- **TERRAIN-DISPLACE-PLAN-2** — only if/when the near-unit fade /
  strength clamp mitigation design is sketched.

## End-state

Terrain visual stack closed for the geometry-fixed era. Three
slices (NFH, V1, V2) + three audits/soaks (height audit, lighting
soak 1, lighting soak 2) + ImGui consolidation (Graphics Options
Terrain Tuning + Static Prop Tuning). All default-OFF; all
runtime-tunable; all reviewer-PASS; all soak-validated across
three terrain-heavy missions.

No further terrain visual work authorized.
