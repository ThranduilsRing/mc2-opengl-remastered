# MC2 Terrain/Object Depth-Bias System Map

Authoritative map of every depth-writing surface in the MC2 OpenGL renderer.
Built by recon 2026-06-27 against `claude/nifty-mendeleev`. Read-only. Citations
are drift-prone (grep the symbol to confirm line).

Depth regime: reverse-Z, glClipControl(ZERO_TO_ONE), glClearDepth(0), compare
GL_GEQUAL (LARGER NDC z = nearer camera = WINS). A more-positive NDC z draws in
front.

Single source of bias constants: mclib/terrain_depth_bias.h +
shaders/include/terrain_depth_bias.hglsl (hand-mirrored, lockstep).

Current shipped contract (TERRAIN-DEPTH-CONTRACT-1, commit c982817e):

    TERRAIN_DEPTH_FUDGE   = 0.0      (terrain writes TRUE depth)
    VEG_DEPTH_BIAS        = 0.00003
    OVERLAY_DEPTH_BIAS    = 0.00005
    OBJECT_DEPTH_BIAS     = 0.0001
    WATER_DEPTH_BIAS      = 0.00025  (== FAST == RASTER fudge)
    ordering:  terrain 0 < VEG < OVERLAY < OBJECT < WATER

NOTE: header prose comments still describe the OLD -0.002 forward-Z values in
places (hglsl 17-38; gos_terrain.frag 1078-1090). The live const/constexpr
values are the table above. Trust the constants, not the stale prose.

---

## 1. Depth-writing surface table

Legend (Depth mechanism):
- VS clip.z = clip.z += BIAS*clip.w in vertex/tess stage, PRE perspective-divide.
  Hardware does the divide; early-Z/Hi-Z preserved.
- FS gl_FragDepth = fragment shader writes gl_FragDepth explicitly, POST-divide.
  DISABLES early-Z/Hi-Z. May DIFFER from rasterized gl_FragCoord.z.
- CPU pz = legacy CPU transform writes vertices[].pz/wz (quad.cpp).

| # | Surface / pass | Shader / TU | MVP source | Depth mechanism | Bias (NDC, +=closer) | gl_FragDepth? | Displaced? |
|---|---|---|---|---|---|---|---|
| 1 | Terrain THIN/indirect (armed) | gos_terrain_thin.vert -> gos_terrain.frag | baked tr.clipPos[] (g_dispatchMvp16) | VS clip.z THEN FS override | VS +0.0; FS min(UndisplacedDepth,gl_FragCoord.z)+0.0 | YES (1090) | no (clipPos baked from corners) |
| 2 | Terrain TESS legacy chain | gos_terrain.tese -> gos_terrain.frag | u_worldToClipGL live | tess clip; FS override | FS min(UndisplacedDepth,gl_FragCoord.z)+0.0 | YES (1090) | YES (phong; UndisplacedDepth=clip of UNdisplaced pos, tese:97-99) |
| 3 | Terrain LOD-chunk | terrain_lod_chunk.vert -> .frag | u_worldToClipGL live | VS clip.z ONLY | 2.0*TERRAIN_DEPTH_FUDGE*w = +0.0 | NO (frag:343-346 refuses) | no |
| 4 | Terrain surface (mask) | gos_terrain_surface.vert / gos_terrain_mask_solid.vert | u_worldToClipGL | VS clip.z | +0.0 | frag-dep | no |
| 5 | Cement/road (in-material) | gos_terrain.frag recipe | same as #1/#2 | rides terrain gl_FragDepth | terrain depth | YES (shares) | rides terrain |
| 6 | Cement STATIC decal | terrain_overlay.vert + DrawDecalStatic (gos_terrain_indirect.cpp:4298+) | u_worldToClipGL live | VS clip.z ONLY | OVERLAY_DEPTH_BIAS*w = +0.00005 | NO | NO - flat quad from 4 corner elevation + kOverlayElevOffset(0) |
| 7 | Dynamic decals/overlays | terrain_overlay.vert | u_worldToClipGL | VS clip.z | +0.00005 | NO | no |
| 8 | Water FAST (armed MDI) | gos_terrain_water_fast{,_mdi}.vert -> _mdi.frag | terrain MVP (Fix B) | VS clip.z | WATER_DEPTH_FUDGE_FAST +0.00025 | NO | no |
| 9 | Water RASTER (CPU/intro) | gos_terrain_mask_water.vert / quad.cpp | CPU pz / live | VS clip.z or CPU pz | WATER_DEPTH_FUDGE_RASTER +0.00025 | NO | no |
| 10 | Vegetation cards | gos_vegetation_card.vert | u_worldToClipGL | VS clip.z | VEG_DEPTH_BIAS +0.00003 | NO | no |
| 11 | Props/buildings/TURRETS/generators | static_prop.vert (GPU batcher) + building_pbr.vert | u_worldToClipGL (==gos_GetTerrainMVPMat4, max_diff=0) | VS clip.z ONLY | OBJECT_DEPTH_BIAS +0.0001 | NO | no |
| 12 | Mechs | mech.vert | u_worldToClipGL | VS clip.z | OBJECT_DEPTH_BIAS +0.0001 | NO | no |
| 13 | Shadow terrain | shadow_terrain.frag | light MVP | FS = gl_FragCoord.z (identity) | n/a | YES (identity) | n/a |
| 14 | Shadow objects | shadow_object.frag | light MVP | FS = gl_FragCoord.z (identity) | n/a | YES (identity) | n/a |
| 15 | GPU cull comps | gpu_driven_terrain_solid.comp:216, gpu_driven_water.comp:189 | dispatch MVP | cull-feed (NOT a render write) | mirrors VS bias | n/a | n/a |

### The two depth-ENCODING families (the core asymmetry)

- Family A -- FS gl_FragDepth (post-divide, early-Z OFF): #1, #2. Write
  gl_FragDepth = min(UndisplacedDepth, gl_FragCoord.z) + 0.0.
- Family B -- VS clip.z (pre-divide, early-Z ON): #3 (LOD-chunk terrain),
  #6/#7 (decals), #8/#9 (water), #10 (veg), #11 (props/turrets), #12 (mechs).
  Write rasterized gl_FragCoord.z = (clip.z + BIAS*clip.w)/clip.w.

Terrain is encoded INCONSISTENTLY between its own paths: indirect/tess (#1/#2)
writes a FS min() value; LOD-chunk (#3) writes plain rasterized z. Objects (#11)
are always Family B.

---

## 2. Displacement provenance

- Tessellation displacement happens ONLY in gos_terrain.tese (phong,
  alpha=tessDisplace.x). It moves the surface BETWEEN the 4 cell corners on
  curved/sloped terrain. Corners are not moved.
- UndisplacedDepth (tese:97-99) = clip-z of the UN-displaced corner-interpolated
  worldPos. So in gos_terrain.frag:1090, min(UndisplacedDepth, gl_FragCoord.z) =
  min(flat-corner-plane depth, actual-displaced-surface depth). Under reverse-Z
  min picks the SMALLER NDC z = the FARTHER surface = makes terrain LOSE against
  a coplanar decal when the surface bulges UP (nearer).
- Thin/indirect (#1) is NOT tess-displaced (clipPos baked from corners) but still
  runs frag:1090; with no displacement the two depths are ~equal so terrain ~=
  true corner depth.
- Decals (#6/#7), props (#11), veg (#10), water (#8) are ALL flat-anchored at
  heightmap elevation; they never see tess displacement. They sit on the corner
  plane, not the bulged surface.

---

## 3. BUG A -- turrets/generators SINK on FLAT ground, zoom-dependent

Symptom: large flat coplanar bases (turrets, generators) sink into terrain on
FLAT ground; consistent at a given zoom, varies across zoom; trees/rocks on the
SAME path/bias do NOT sink.

Ruled out (do not re-derive): matrix divergence (VIEW_UNIFORMS max_diff=0),
cement decal (NO_CEMENT_DECAL did not fix), kOverlayElevOffset (now 0, did not
fix), OBJECT_DEPTH_BIAS 1e-4->2e-4 (did not fix), 1-frame ring lag (user: steady).

Root cause = Family-A vs Family-B depth-ENCODING mismatch, NOT bias magnitude.
On flat ground (UndisplacedDepth == gl_FragCoord.z, displacement zero):

- Terrain (#1/#2) writes D_terr = min(Du,Dr)+0.0. Mathematically Du==Dr==D so
  D_terr=D. BUT the FS value is computed from UndisplacedDepth, a SEPARATELY
  interpolated varying (perspective-correct interp of clip.z/clip.w, re-divided
  in-shader) -- a DIFFERENT arithmetic path than hardware gl_FragCoord.z. Equal
  only at infinite precision; in float they differ by a few ULP that VARY with
  the triangle clip-w spread = with zoom.
- Turret base (#11) writes D_obj = (clip.z + 1e-4*clip.w)/clip.w = D + 1e-4.

GEQUAL needs D_obj > D_terr i.e. 1e-4 > (D_terr - D). D_terr - D is terrain's
encoding ULP error. A large flat base covers many fragments; wherever terrain's
min/interp rounds UP, the 1e-4 margin is eaten and the turret loses -> SINKS.
The error scales with depth precision at the fragment = f(clip.w) = zoom. So:
zoom-dependent (clip.w drives the ULP spread), flat-specific (only flat makes
Du~Dr so the ULP delta is comparable to 1e-4; slopes have a displacement gap
that dominates and terrain reliably loses there), turret-not-tree (wide flat
base = many tie attempts; a tree trunk touches ~1 fragment + canopy is well
above terrain).

Why bias bumps fail: 2e-4 did not fix it because the competitor is a
NON-CONSTANT per-fragment ULP error from a different encoding, not a constant
offset. You cannot out-bias a noise floor.

Recommendation: delete the gl_FragDepth override in gos_terrain.frag:1090; let
terrain write rasterized clip.z like LOD-chunk (#3) and every object (Family B).
Then the +0.0001 OBJECT bias compares cleanly.

---

## 4. BUG B -- ROADS (cement) DISAPPEAR on UNEVEN ground, zoom-dependent

Symptom: cement/road decals vanish on sloped/uneven ground; flat roads fine;
zoom-dependent.

Root cause = flat-baked decal height vs tess-displaced terrain height. The cement
static decal (#6) is a planar quad from the 4 cell-corner elevation values
(gos_terrain_indirect.cpp:4360-4363, kOverlayElevOffset=0), NOT tessellated. The
terrain under it (#2) IS phong-displaced between those same 4 corners
(gos_terrain.tese). On a sloped/curved cell the displaced surface bulges toward
the camera (UP) relative to the flat decal plane that linearly interpolates the
corners.

Mid-cell, displaced terrain gl_FragCoord.z is LARGER (nearer) than the decal
planar depth. The decal has only +OVERLAY_DEPTH_BIAS (+0.00005). The displacement
world-gap projects to an NDC gap that on a real slope EXCEEDS 0.00005 -> terrain
wins GEQUAL -> road occluded = disappears. Zoom-dependent because the same world
gap maps to a larger NDC delta as clip.w shrinks (zoom in); the crossover moves
with zoom. Flat ground has ~zero displacement gap so +0.00005 always wins.

frag:1090 min(UndisplacedDepth, gl_FragCoord.z) is the PARTIAL mitigation -- it
makes terrain report UNdisplaced depth so a coplanar decal can win. Removing
kOverlayElevOffset (old +0.15 world lift) removed the last slack floating the
decal above the bulge -- so flat roads still work (bias suffices) but sloped
roads now lose where the bulge exceeds the partial mitigation.

Recommendation: make the decal track the DISPLACED surface, not the flat corner
plane. (1) tessellate/subdivide the decal quad with the same phong displacement
as gos_terrain.tese; or (2) fold cement fully into the in-material recipe path
(#5) so there is no separate decal geometry to diverge (CEMENT-TRANSITION-
COMPOSITE Path-A, already prototyped). Do NOT write gl_FragDepth on the decal
(chunk-frag 343-346 warns this caused AMD tearing).

---

## 5. Recommended unified depth contract

Both bugs are faces of ONE flaw: terrain uses a special FS-gl_FragDepth
min(undisplaced,displaced) encoding while everything coplanar with it (objects,
decals, water) uses plain VS-clip.z. Mixing post-divide gl_FragDepth against
pre-divide rasterized z on coplanar surfaces is the classic zoom-dependent
tie-break failure; the min() + separate-varying re-divide adds a per-fragment ULP
noise floor that defeats any constant NDC bias.

Direction -- single contract, ALL opaque scene geometry Family B, no gl_FragDepth:

1. Remove the gl_FragDepth write in gos_terrain.frag:1090. Terrain emits
   rasterized clip.z (VS pre-divide) like terrain_lod_chunk already does.
   Restores early-Z/Hi-Z on the indirect/tess path. Terrain + objects share ONE
   encoding -> +0.0001 OBJECT bias compares cleanly -> BUG A gone.
2. Make the decal track the displaced surface (tessellate, OR fold into recipe
   #5). Removes the flat-vs-displaced gap -> BUG B gone; +0.00005 OVERLAY then
   suffices because the decal is genuinely coplanar.
3. Keep the layered ordering (VEG < OVERLAY < OBJECT < WATER) -- correct AS LONG
   AS all surfaces share one encoding and are genuinely coplanar. Biases are
   tie-breaks, not gap-closers.
4. Reverse-Z + float depth (already scoped) shrinks the residual ULP floor and is
   the long-term root-class fix; the encoding unification above is a prerequisite
   stepping stone and independently shippable.

Risk note for step 1: chunk-frag 343-346 + vulkan_aligned_depth_bias_ruling.md
warn gl_FragDepth on the cement boundary caused AMD tearing -- that argues FOR
removing the terrain gl_FragDepth write. The original reason terrain wrote
gl_FragDepth was BUG B (decal coplanarity under displacement); fixing BUG B
geometrically (step 2) removes the only justification for the override.

---

## 6. Current tree state (unverified / in-flight -- verify before building on)

- kOverlayElevOffset = 0.0f (gos_terrain_indirect.cpp:4257) -- user's unverified
  change (retired the legacy +0.15 world lift). Tipped sloped roads from "floated
  above bulge" to "lost to bulge" (BUG B).
- MC2_DIAG_NO_CEMENT_DECAL gate present (proved decal is NOT the turret sink).
- A no-op object-matrix-share in gameos_graphics / static_prop_batcher
  (VIEW_UNIFORMS byte-identical; not the cause).
- Heavy foreign WIP in bdactor.cpp, EditorObjectMgr.cpp, cloud.frag,
  gos_postprocess.cpp, golden-sets.json -- treat as NOT editable.
- Header prose still references old -0.002 forward-Z values; live constants are
  the true-depth contract (Section "Current shipped contract").

## 7. Probes / verification (in-engine; RenderDoc cannot catch zoom-ULP)

- gos_terrain.frag debug mode 1 (~488-500): R = rasterized depth, G =
  UndisplacedDepth. Green>Red = undisplaced deeper (correct); Red>Green =
  potential z-fight. See the Family-A delta directly on flat vs sloped ground at
  near/far zoom.
- MC2_DEPTH_TRANSITION_PROBE ([DEPTH_TRANSITION v1]) for live-vs-baked drift (the
  dz_* flat-vs-drifting signature).
- WARNING: [WATER_DEPTHPROBE v2] is MVP-hash only -- BLIND to gl_FragDepth/fudge/
  render-VS bugs. Do NOT cite it as proof a depth fix works.
- WARNING: smoke is FPS-CAPPED -- never use FPS/frame-count to A/B a depth
  change. Use debug-mode-1 visual + a coarse once-per-frame probe.
