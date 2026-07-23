# DEFERRED-BATCH-7 status — 2026-07-23

Branch `claude/deferred-sweep-1`, worktree A:/Games/mc2-deferred-1.
Deployed to releases/mc2-win64-deferred-test @ src_commit `ae2b4811` (clean).
Evidence: `.claude/captures/batch7/` (zoomed crops `crop_*.png` per the new
full-resolution evidence rule). Mid-batch the verification fleet added items
6-9; this file records batch-7 agent scope only.

## Shipped commits

| Commit | Content |
|---|---|
| `c41e82d2` | Items 1+2: SHADOW-FADE-RAMP0-1 (fade = fadeMax*dNorm², no 0.45 onset window/seam) + SHADOW-ALT-SCALE-1 (penumbra/fade gains scale with lightDir.z: grazing sun = night dissolve, high sun = radius-gain 1 + fade ×0.25). All 3 sampler paths, all 4 macro-inject sites. Gates MC2_SHADOW_FADE_RAMP0 / MC2_SHADOW_ALT_SCALE, default ON. |
| `209073e3` | Items 4+5 diagnostics: MC2_LIGHT_CONE_DIAG ([CONE_AIM_DIAG]/[CONE_QUEUE_DIAG]), MC2_LIGHT_CONE_AIM_FLIP probe, MC2_LIGHT_POOL_DUMP (per-light slot/owner/active dump; spotlight_diag tags record owner appearance ptrs). |
| `09dafc9f` | GHOST-CONE-RECIPE-SKIP-1: spotlight leaves excluded from BOTH bdactor registerStatic recipe loops; QueueConeGlow drops zero-shapeToWorld early-frame queues (garbage cones at origin). Gate MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP default ON. |
| `ae2b4811` | GHOST-CONE-RECIPE-SKIP-1 root ext: registerType() registers SpotLight_-prefixed TYPE shapes as EMPTY types (geometry never enters the VBO/packet stream). |

check-env-registry PASS (871), flags.auto.yaml regenerated.

## Per-item verdicts (honest, full-res zoomed grading)

### Item 1 (shadow transition seam) — FIXED (code) / VERIFIED via A/B
Root cause: opacity fade used smoothstep(0.45,1.0,dNorm) while the radius
ramp started at dNorm=0 — mismatched onsets drew the crisp/soft seam. Now
fade = fadeMax*dNorm² (continuous from contact, no piecewise boundary).
Evidence: crop_i2_dayshadow_OLD.png (killswitches, screen-door stipple, no
solid core) vs crop_i2_dayshadow_NEW.png (defaults; solid connected umbra +
soft penumbra) at mc2_01 saved_205644 100% zoom. Palm-grove-specific crop
still owed; same term drives it.

### Item 2 (day shadows too soft) — FIXED (code) / VERIFIED via A/B
Sun-altitude scaling read in-shader from lightDir.z: at high sun the
radius gain drops 4→1 (16 taps over ¼ the disk = dense = solid, kills the
"screen-door" stipple the fleet root-caused) and fade depth ×0.25. Night
(sin alt ≤0.21) keeps the exact night dissolve. Same A/B evidence as item 1.

### Items 4+5 (light cone anchor / ghost light) — ROOT-CAUSED, FIX PARTIAL
- The "ghost light"/wrong-side artifact is NOT the additive cone glow and
  NOT the pool light: it is an OPAQUE cream folded-plane CONE MESH drawn by
  the GPU-objects static-prop pipeline. A/B ladder at mc2_10 saved_212019
  (cream-px detector over the wedge): unchanged with MC2_LIGHT_CONE_GLOW=0;
  GONE with MC2_GPU_OBJECTS=0, MC2_STATIC_PROP_PERSISTENT_BUCKETS=0, and
  MC2_SUBSTRATE_COALESCE_LEGACY=1 (coalesce multidraw is the drawing path).
  World-anchored and time-stable (identical at trigger 120/150/500); apex
  sits at the LEFT guard tower's searchlight arm — it is that tower's
  authored cone. Lighting-debug-view captures (normal view) leave the wedge
  untinted → drawn by a program family that ignores the debug views
  (the coalesce prop variant / overlay family).
- Fixes shipped: recipe loops + registerType now exclude SpotLight_*
  geometry entirely (hygiene, closes the class for correctly-named cones) +
  early-frame zero-transform cone-glow queues dropped.
- REMAINING HOLE (verified on ae2b4811: wedge still renders, crops
  crop_i5_ghost_ROOTFIX_mid.png / crop_i5_ghost_f500_mid.png): this tower's
  cone node evidently does NOT carry the exact "SpotLight_" 10-char prefix
  (all name-based skips miss it — including the LEGACY per-instance skip,
  yet the legacy/per-leaf path does NOT draw it; only the coalesce path
  does). NEXT: dump the watchtower type's node names (MC2_BLDG_REG_TRACE /
  registry inspector or aseconv on the tower .ase) to learn the actual
  name; then either widen the name test (e.g. "SpotLight" prefix without
  underscore, "SearchLight_") or find why coalesce draws a leaf that
  per-leaf draw skips (suspect: coalesce draws whole-type packet ranges
  where the per-leaf path draws per-instance leaves).
- Fleet note supporting closure: with the cones made non-opaque the
  "anchor on the wrong side" perception is expected to resolve (verifier
  graded anchoring CORRECT at 204907); MC2_LIGHT_CONE_AIM_FLIP probe is
  available if a residual mirror remains.

### Item 6 (distant black band — CRITICAL, added mid-batch) — CHARACTERIZED, NOT FIXED
Reproduced deterministically at mc2_10 saved_205952 (and visible at the
mc2_01 day pose too — NOT night-specific). Facts established:
- With MC2_OOB_FOG=0, every pixel above screen-y≈330 is EMPTY (max 0-1):
  nothing renders there. The grey bank above is fog_oob's cloud bank; the
  "black hills + trees" between bank and terrain are REAL geometry drawn
  PURE BLACK (fog_oob's depth test outlines them correctly).
- The lit/black boundary is razor-straight (slightly tilted; camera is 2.1°
  off world-axis) — a world-space straight edge, consistent with the map's
  far edge or an iso-distance/clip line, NOT a lighting window.
- NOT changed by: MC2_SHADOW_MIN_SUN_ALT=45, MC2_SHADOW_DIST_FADE=0,
  MC2_SSAO=0, MC2_TERRAIN_LIGHT_POOLS=0, MC2_EDGE_FOG=0, MC2_OOB_LETTERBOX=0,
  MC2_CAMERA_PARK=0, MC2_TERRAIN_LOD_CHUNK_NO_CULL=1, MC2_IMPOSTOR_DIST=big,
  water debug, ALL batch-5/6/7 feature gates off at once. Frame-time ruled
  out (f120 == f700 profile).
- MC2_LIGHTING_DEBUG_VIEW=albedo repaints ALL map terrain but NOT the black
  region → the black hills are NOT chunk-terrain fragments. They carry tree
  impostors. Best remaining hypothesis: the off-map/border backdrop family
  (same family as item 8's fullbright horizon impostors — unlit cards read
  white where texture is bright, black where dark/at night).
- v0.5.1 exe at the same bookmark renders those hills LIT (crop
  crop_i6_band_v051.png) — but its camera framing differs (bookmark restore
  predates CAMERA-PARK), so build-vs-build comparisons at this pose are
  partially confounded; the verifier's matched-framing crops
  (final vs final2) remain the authoritative build A/B.
NEXT: attribute the black hills' draw call (RenderDoc or a draw-pass
bisect: sky/theSky ring, vegetation cards, border backdrop) and give that
pass the mission light + haze. All probe captures in
.claude/captures/batch7/i6_205952_* with crops crop_i6_band_*.png.

### Item 3 (shoreline jaggedness) — RECON ONLY (deprioritized behind 6/4/5 by fleet directive)
Before-capture banked at the worst-water pose (i3_before_204523). Fleet
spec confirms tile-boundary geometric clip; only (a) vertex displacement
along the cooked mask SDF or (b) wide SDF alpha-feather are viable. The
mask feather (WATER-SHORE-MASK-1) exists and is ON; the residual staircase
is the polygon outline itself. mc2_01 has NO cooked mask
(cook_shoreline.py install covers mc2_10 only in this lane). NOT CODED
this session — needs its own slice (widen feather window from the mask SDF
so alpha reaches 0 before the mesh edge on all shore tiles + cook masks
for mc2_01/05/18; or the vertex-displacement variant).

## New/changed gates (all registered + run_smoke passthrough)
MC2_SHADOW_FADE_RAMP0 (ON), MC2_SHADOW_ALT_SCALE (ON),
MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP (ON),
MC2_LIGHT_CONE_DIAG / MC2_LIGHT_CONE_AIM_FLIP / MC2_LIGHT_POOL_DUMP (traces/probe, OFF).

## Watch-items / caveats
- SHADOW-ALT-SCALE changes NIGHT far-tip fade shape slightly via the
  RAMP0 curve (dNorm² vs windowed onset) — night look re-grade owed
  (fleet's #7 "night shadows harder than day" polarity item may interact;
  check whether MC2_SHADOW_MIN_SUN_ALT's clamped vector is what the shader
  uniforms carry per mission).
- tier1 smoke not re-run this session (captures only) — run before merge.
