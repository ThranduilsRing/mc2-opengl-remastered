# SHORELINE-BATCH-COOK-1 — batch shoreline masks + merged bakes for all water missions

Slice 1 of `.claude/SHORELINE-FOG-NEXT-SLICES.md`. Every water-bearing stock
mission gets a `shoreline_mask.png` sidecar (so wide-beach falloff / basin
exclusion works everywhere, not just hand-cooked mc2_17) plus the merged
re-authored visual bake (`visual_height_4x` + max-mips + object damp) it is
cooked against. **Preview only** — staged, NOT installed. Coordinator installs
after the user ratifies the mc2_17 look.

## Result

- **22 / 24** stock missions cooked (every mission with water). **2 skipped dry**:
  `mc2_13` (water_elev −1, 0 cells) and `mc2_21` (water_elev −300, 0 cells).
- **0 failures** — all 22 pass the `terrain_workbench.py` gate (corner_unpinned,
  shape_fidelity_corr, extrema_preserved, facet_crease_reduced,
  mountain_detail_present, blockiness_reduced all PASS).
- Staging: `staging/SHORELINE-BATCH-1/data/missions/<stem>.beauty/` (deploy-shaped;
  untracked preview payload, ~35 MB, same convention as GEOMORPH-REAUTH-MERGE-1).
  Per-mission files: `visual_height_4x.r32`, `visual_height_mips.r32`,
  `visual_damp.r32`, `visual_height_report.json`, `shoreline_mask.png`,
  `shoreline_mask.bounds.txt`.
- Machine manifest with per-file sha256 + verdicts + water elev + band params:
  `staging/SHORELINE-BATCH-1/manifest.json`.

## Method (per mission)

1. **Merged bake** — `visual_heightfield.py <m> --reauth --mountainify --seed 1337`
   (mips on, factor 4) at HEAD `67472197` → `<m>.beauty/visual_height_4x.r32` (+ mips + damp).
2. **Cook** — `cook_shoreline.py cook --pak <m>.pak
   --visual-height <m>.beauty/visual_height_4x.r32 --supersample 8
   --damp-width 28 --foam-width 6` → `shoreline_mask.png` (+ bounds).
3. **Gate** — `terrain_workbench.py <m>` (skip + report on FAIL; none failed).

Determinism: fixed mountainify seed **1337**, fixed bands **damp 28 wu / foam 6 wu**,
fixed **supersample 8**. Water level always from `[Water].Elevation` in the `.fit`
via `mission_terrain_analyzer.read_water_elevation` — never the PostcompVertex
`.water` byte (packed alpha, not a bool).

## Critical finding: supersample-8 is required, not optional

At `--supersample 1` against a 4× hi-res bake the EDT cell is **32 wu** (128/4), so
the nearest land cell sits exactly 32 wu from the drawn waterline — **beyond** the
default 28 wu damp / 6 wu foam bands → the G/B channels come out **entirely empty**
and the mask is a useless all-zero modulator. `--supersample 8` → 4 wu cells, both
bands populate. This is the same coarse-grid landmine the existing
`test_cook_coarse_fallback_narrow_foam_band_is_empty` documents; the batch just hits
it at the 4× bake resolution instead of the coarse pak grid. The new `all-missions`
subcommand therefore **defaults `--supersample 8`**.

## Tooling change

`tools/terrain_beautify/cook_shoreline.py`:
- Extracted `cook_one(...)` from `cmd_cook` (shared single-mission core).
- Added `all-missions` subcommand: enumerates `<missions-dir>/*.pak`, detects water
  (analyzer, skips dry maps + reports them), cooks against each
  `<beauty-root>/<stem>.beauty/visual_height_4x.r32` (coarse-grid fallback if
  absent), writes `<out-root>/<stem>.beauty/shoreline_mask.png` + optional census
  JSON. Default `--supersample 8`.
- `test_cook_shoreline.py`: `+2` batch tests (`test_all_missions_cooks_water_skips_dry`,
  `test_all_missions_deterministic`) over a synthetic wet+dry 2-mission dir. Full
  suite **25 passed**.

> The two-tool pipeline (bake → cook) is driven by a throwaway orchestrator
> (scratchpad, uncommitted); the durable, tested deliverable is the cook tool's
> `all-missions` mode. To reproduce the mask cook alone against existing bakes:
> `cook_shoreline.py all-missions --missions-dir A:/Games/Carver5-feasibility/data/missions
> --beauty-root staging/SHORELINE-BATCH-1/data/missions --out-root <dir> --census <json>`.

## Deploy wiring (finding, NOT changed)

`scripts/deploy_payload.py` does **not** carry `.beauty/` mission sidecars — its
`GAME_DATA_PAYLOAD` is a fixed 2-file JSON list; it is a build-output + tracked-config
deployer, not a mission-content deployer. Per-mission `.beauty/` sidecars install via
the coordinator's mission-content path (same as GEOMORPH-REAUTH-MERGE-1), not this
manifest. Wiring 22×6 uncommitted preview files into it would be wrong; left as-is.

## Smoke proof (one, non-tier, spare lane)

`mc2_20` (non-tier: tier1 = 01/03/10/17/24), gate ON via
`MC2_TERRAIN_SHORELINE=1` + `MC2_TERRAIN_SHORELINE_FILE=<staged mc2_20 mask>`,
`--allow-any-mission`, `--deploy 0.4 --duration 30`:

```
result=PASS (1/1)   mc2_20  PASS  1792 frames  load 8054ms
[TERRAIN_SHORELINE v1] uploaded handle=21 w=3816 h=3816 bytes=58247424 bounds=(-7680.0,7680.0,15360.0,15360.0)
[TERRAIN_SHORELINE v1] LOADED path=.../mc2_20.beauty/shoreline_mask.png size=3816x3816 bounds=(-7680.0,7680.0,15360.0,15360.0)
```

Clean load — no READ FAIL / PNG DECODE FAILED / EMPTY FILE. `3816 = 477×8` confirms
the supersample-8 grid reached the GPU. (0.4 deploy exe is pre-existingly stale, 36
commits behind — orthogonal to this DATA-lane cook; the shoreline loader path predates
the drift and the gate reaches the exe cleanly.) Artifacts:
`tests/smoke/artifacts/2026-07-02T03-24-03/`.

## Census (all cooked missions)

| mission | water % | water_elev | wet_cells | foam_cells | workbench |
|---|---|---|---|---|---|
| mc2_01 | 76.7 | 350 | 120032 | 42016 | PASS |
| mc2_02 | 29.3 | 35 | 196300 | 68684 | PASS |
| mc2_03 | 13.0 | −100 | 54102 | 18560 | PASS |
| mc2_04 | 30.4 | 83 | 159624 | 55808 | PASS |
| mc2_05 | 7.9 | 140 | 89822 | 31200 | PASS |
| mc2_06 | 9.7 | 0 | 62010 | 21376 | PASS |
| mc2_07 | 28.1 | 120 | 76114 | 26400 | PASS |
| mc2_08 | 35.3 | 280 | 158176 | 54672 | PASS |
| mc2_09 | 15.2 | 50 | 86224 | 29600 | PASS |
| mc2_10 | 11.2 | 80 | 105824 | 36116 | PASS |
| mc2_11 | 23.4 | 25 | 149870 | 51984 | PASS |
| mc2_12 | 9.7 | 70 | 104594 | 36272 | PASS |
| mc2_14 | 21.0 | 275 | 100166 | 34444 | PASS |
| mc2_15 | 6.8 | 115 | 37938 | 13056 | PASS |
| mc2_16 | 9.7 | −90 | 147334 | 50704 | PASS |
| mc2_17 | 54.1 | 120 | 167492 | 58176 | PASS |
| mc2_18 | 7.0 | 50 | 72884 | 25120 | PASS |
| mc2_19 | 21.5 | 73 | 132468 | 45712 | PASS |
| mc2_20 | 35.6 | 40 | 178110 | 61344 | PASS |
| mc2_22 | 4.4 | 150 | 76220 | 26176 | PASS |
| mc2_23 | 19.2 | 250 | 70546 | 24416 | PASS |
| mc2_24 | 18.0 | 0 | 33252 | 11456 | PASS |

Skipped (dry): mc2_13 (elev −1), mc2_21 (elev −300).
