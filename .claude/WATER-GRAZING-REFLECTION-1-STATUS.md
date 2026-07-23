# WATER-GRAZING-REFLECTION-1 — STATUS (master-FAIL #9, MEDIUM)

**Complaint:** at grazing/horizon camera angles the water reads as a flat dull
blue sheet and does NOT reflect the bright sky (bookmark `mc2_10_saved_205952`),
despite reflection being default-ON and skytint present.

**Verdict: FIXED (GL MDI path).** Grazing water now brightens toward the authored
sky/horizon tint instead of mirroring a dark clear-sky sample. Byte-identical for
top-down/near water and on clear (fog-less) maps' non-grazing water.

## Commits (branch `claude/nifty-mendeleev`, worktree A:/Games/mc2-main)
- `c18eda8b` fix(water): WATER-GRAZE-REFL-1 — decouple sky reflection from `waveLOD`
  (reflMix `= fres*strength*max(waveLOD, fres)`; fade wave-NORMAL by `waveLOD` so
  genuinely-far grazing water flattens to a clean mirror instead of being zeroed
  by the wave-detail fade). Byte-identical near field.
- `588c446a` fix(water): WATER-GRAZE-REFL-1 — pull reflected color toward
  `u_waterSkyTintColor` at grazing angles (the load-bearing fix for this pose).

Both edits: `shaders/gos_terrain_water_mdi.frag` only. **No `gameos_graphics.cpp`
edit** (batch-8C lane holds it) — done entirely in-shader using existing uniforms.

## Root cause (evidence-backed)
1. `reflMix = fres * strength * waveLOD`. `waveLOD` is a wave-DETAIL distance fade
   (smoothstep 9000→40000 wu → 0 at the horizon). It was erasing the reflection
   exactly at grazing/far water where Fresnel (`fres`→1) makes it strongest.
   → commit 1 (correct hardening; a no-op at THIS bookmark, see below).
2. At `mc2_10_saved_205952` the visible lake is WITHIN the `waveLOD` range, so
   `reflMix` was already maxed at `REFL_MAX=0.55` (measured: reflection-on vs -off
   differ by ~m≈0.55). The water was 55%-reflecting a **dark** sky sample.
   `MC2_WATER_DEBUG_MODE=7` (skyReflCol) on the lake = **RGB [37.5, 36.0, 34.3]**
   (lum ~36) while the visible overcast sky is ~RGB 140. The HDRI/SH reflection
   samples a dark clear-sky band that mismatches the rendered dusk/overcast.
3. `fog_color` is deliberately **vec4(0)** for water (CINEMATIC-WATER-WHITE-1,
   gameos_graphics.cpp ~3131), so the bright rendered sky is not reachable via
   fog. The only bright-sky color available in the FS is `u_waterSkyTintColor`
   (`[0.55,0.70,0.85]`, always uploaded regardless of skytint strength).
   → commit 2: `grazeW = pow(1-vdir.z, GRAZE_SKY_EXP=4)`,
   `skyReflCol = mix(skyReflCol, u_waterSkyTintColor, GRAZE_SKY_PULL(=0.7)*grazeW)`.

## Gate / killswitch
- No new env var (would need gameos_graphics.cpp; blocked). Both changes live
  INSIDE the reflection block: **`MC2_WATER_REFLECTION=0` skips the whole block**
  (existing, documented killswitch — full escape hatch).
- `GRAZE_SKY_PULL=0` in-shader restores the exact old reflected color; grazeW→0
  (top-down) is a mathematical no-op.

## Verification (deployed exe `5b12ddd7`, lane 0.5.1; captures scratchpad/graze/*)
| Check | Verdict | Evidence |
|---|---|---|
| Grazing lake reflects sky (205952, real lake y0.30–0.40 crop) | **PASS/improved** | before(old refl) lum **36.9** RGB[35.7,38,37.2] → after lum **43.6** RGB[40.4,44.7,45.8]; +18% lum, shifted sky-blue (B>R); before→after mean_delta 6.70, 29% px |
| Critical black-band sentinel (distant_terrain_horizon_band) | **PASS** | vs golden mean_delta 0.002 — distant terrain still renders, not black |
| map_edge_skirt / off-map slab (non-water foreground) | **PASS** | 0.00 delta — untouched |
| Top-down non-water pose (mc2_10_saved_212140) | **PASS** | 0.0 delta all regions — water-only change, scene unaffected |
| Shore-staircase (mc2_01_saved_205644 shore_left/central/botright) | **PASS (substance)** | waterline GEOMETRY identical (shore_left 0.0; botright shape pixel-matched) — RGB-only shift, alpha/shore ramp never touched |
| mc2_01 water (2nd map) non-regression | **benign** | horizon +[3,4,5], shore_botright +[1.5,2.2,2.6] — subtle brightening, no artifact/over-blue/banding (visual A/B) |
| tier1 smoke (my exe 5b12ddd7) | 4/5 — not mine | mc2_01 PASS + mc2_10 PASS (both WATER maps), mc2_17/mc2_24 PASS; mc2_03 crash_silent @ 0 frames (LOAD crash, pre-render). Two runs crashed DIFFERENT missions (mc2_01 then mc2_03) = non-deterministic load crash, cannot be a water-DRAW-time shader change |

Verify-by-angle (the exaggeration axis): the effect scales exactly with grazing —
grazing lake (grazeW≈1) changes strongly; top-down pose (grazeW≈0) is 0.0. As designed.

## Residual risk / notes
- **GL MDI path only** (matches SHORELINE-STAIRCASE-1 precedent). Vulkan water
  island (`shaders/vulkan/water_*.frag`, `vulkan_water_island.cpp`) not updated —
  VK parity deferred. VK fail-softs to GL in the deployed build anyway.
- `verify.py` "FAIL" verdicts on the regress run are (a) intended water
  brightening tripping medium thresholds and (b) `tree_shadows_palm` = #7
  mover-shadow lane contamination (this shared build also carries #7/#8), NOT this
  fix. mc2_10_205952 vs golden PASSES all 3 official regions.
- **Pipeline finding:** the `water_shore_grazing` region rect in
  `tools/visual_verify/regions/mc2_10_saved_205952.json` (norm y 0.42, h 0.25)
  targets the foreground off-map SLAB, not the actual lake (y ~0.30–0.40). It
  reads ~0 delta and "passes" regardless of water state. Recommend re-aiming it to
  y≈0.30 h≈0.10 so it actually grades the grazing lake.
- `GRAZE_SKY_PULL`/`GRAZE_SKY_EXP` are taste dials; 0.70/4.0 chosen to read as sky
  reflection without flattening to a blue sheet (glints preserved).
- Deployed exe fingerprint trails HEAD only because parallel lanes (#7/#8/…) kept
  committing to the shared branch during this session; `5b12ddd7` contains both of
  this fix's commits.
