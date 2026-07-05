# GEOMORPH-NEXT-SLICES (for opus)

Successor planning notes after TERRAIN-LOD-GEOMORPH-1 (branch `claude/geomorph-1`,
rungs b/a/c = `66cdff48`/`2f0e90a8`/`4f179450`, rung d fix+oracle = `191b6b74`).
Read `.claude/TERRAIN-LOD-GEOMORPH-RECON-1.md` first; this file only carries what
the recon could not know.

## Shipped state (facts a successor must not re-derive)

- **Mip layout**: `visual_height_mips.r32` sidecar = 5 levels, strides
  {2,4,5,10,20}, each `mapSide^2` floats (gaea_peaks_01: 5x120^2 = 72000 floats),
  MAX of the FINAL fine bake over each coarse vertex's +/- stride/2 footprint.
  Uploaded APPENDED to the binding-26 SSBO (no new binding slot). Loader
  size-checks (5*mapSide^2) and silently ignores absent/mis-sized sidecars.
- **Seam ownership rule (load-bearing)**: block-PERIMETER verts never read mips
  and never morph; only interiors do. Both sides of every seam therefore sample
  identical S2 values by construction — zero new crack risk. Any v2 must keep
  this or re-prove seams.
- **Morph factor**: per-block `m` from FINAL lodLevel + center distance, ramps
  over outer `(1 - MC2_TERRAIN_LOD_MORPH_START)` (default 0.6) of the band,
  same DIST_SCALE-scaled thresholds as band selection. Levels 0/5 never morph.
  `FORCE_LOD` pins `m = 0` (A/B captures isolate the pure band surface).
- **Latent S2 bug fixed in rung d**: the mode-1 displacement branch in
  `terrain_lod_chunk.vert` tested `u_visualDisplace != 0` and swallowed mode-2
  patches -> every coarse-band block collapsed to 1/4 size at its NW corner and
  the mode-2 Z-swap block was dead code, since S2 shipped. Only visible when a
  coarse band is on screen — the static smoke camera never exercises one, so
  smoke gates are structurally blind to this class. The FORCE_LOD pixel oracle
  is the only defense; keep running it for any vert change.
- **Oracle (reusable)**: `tests/visual/bookmarks/gaea_peaks_01.json` sky_y45 /
  sky_y135 / sky_y225 grazing poses (proj 3, alt 2500) + per-column skyline-row
  diff. Terrain renders UNLIT (near-black) on this generated map — that is what
  makes the skyline binary-segmentable, but it also means interior shading
  regressions are invisible at these poses; the 3 original downward poses only
  catch catastrophic geometry loss.
- Numbers (FORCE_LOD 0-vs-4, 800x600, geomorph OFF -> ON): ridge **drop** mean
  24.7->9.9 / 7.1->2.4 / 20.5->15.6 px, max 59->35 / 31->9 / 59->52
  (y45/y135/y225); residual **inflation** mean 2.5-4.5 px, max 22/50/28.

## Slice: RIDGE-AWARE-REDUCTION-2 (highest value)

Pure MAX mips convert silhouette *loss* into silhouette *inflation* (numbers
above; y135 max inflation 50 px). v2 ideas, in test order:

1. **Percentile reduction** (e.g. p90 of footprint instead of max) — one-line
   change in `visual_heightfield.py` `build_max_mips`; sweeps cheaply offline
   against the stored skyline oracle (no engine needed to predict: the bake tool
   can rasterize predicted coarse skylines per stride).
2. **Signed / ridge-aware**: detect ridge cells (footprint max sits well above
   footprint edge samples) and lift only those; leave bowl/valley cells at
   corner value. Kills valley-floor inflation without giving back ridge drop.
3. **Directional**: skyline error only cares about the max along the view-tangent
   axis; an isotropic footprint over-lifts. Probably not worth the complexity —
   measure 1/2 first.

Bar: y135 inflation max <= 15 px while drop stays <= 12 px mean on all three poses.

## Slice: MORPH-HYSTERESIS-INTERPLAY (temporal correctness)

`chooseLodLevel` hysteresis is 10% demotion-only; the morph ramp
(`MC2_TERRAIN_LOD_MORPH_START`) is symmetric and stateless. Consequences to
verify/fix:

- A block hovering at the demotion edge sits at m ~= 1 (fully parent surface),
  then *promotion* snaps m 1 -> 0 discontinuously in one frame (promotion is
  immediate and m is derived from the NEW level). Candidate fix: derive m from
  distance alone (continuous across the threshold), or carry per-block m state
  with a slew-rate clamp.
- No dolly/temporal oracle exists. Cheapest: two captures at
  `MC2_TERRAIN_LOD_CHUNK_DIST_SCALE` k and k+epsilon (thresholds move under a
  static camera = poor man's dolly); assert skyline moves < N px per step over
  a sweep. Fully scriptable with the existing bookmark file.
- Hysteresis width (1.21 distSq factor) and MORPH_START=0.6 were never co-tuned;
  the morph window should probably *contain* the hysteresis band so a
  hysteresis-parked block is always mid-morph, never at m=1.

## Clipmap-arc bridge notes

If/when terrain moves to a GPU clipmap (candidate in the modernization
portfolio), this slice's pieces map as follows — do not rebuild them:

- The binding-26 fine bake + per-stride MAX mips ARE a clipmap level stack in
  disguise (uniform grid, integer-coincident, per-LOD reductions). A clipmap
  arc should lift `build_max_mips` output into its level textures rather than
  re-deriving reductions in-engine.
- The **integer bake-grid coincidence invariant** (bake = uniform 4x of map
  grid; every LOD vertex hits an exact bake index) is what makes toroidal
  clipmap updates exact here; preserve it in any re-tiling.
- Geomorph-in-VS (mix own/parent by band distance) is exactly clipmap
  "transition regions" (Losasso/Hoppe); `computeMorphFactor` seam rules
  (m constant per block, perimeter pinned) transfer verbatim to per-ring
  transition bands.
- The skyline oracle is renderer-agnostic — keep it as the acceptance gate for
  ANY terrain-geometry rework.

## Coordination / open rulings

- **Reauth lane**: mips are built from the FINAL fine visual array inside
  `visual_heightfield.py`, so `--reauth`-modified bakes compose automatically —
  but the mips sidecar must be REGENERATED whenever the fine bake changes.
  ✅ **DONE (MIPS-STALENESS-GUARD-1, 2026-07-03):** no hash/new-file needed —
  the engine loader (`mclib/terrain.cpp`) now enforces the data invariant
  `mip[L][v] >= fineCorner[v]` (every level MAXes a footprint that includes the
  vertex's own fine sample). A stale `visual_height_mips.r32` (right SIZE, wrong
  DATA — built from a different fine bake) violates it → loader DROPS the mips +
  warns `[VISUAL_HEIGHT v1] mips STALE …`, falling back to legacy S2 rather than
  morphing to a dead surface. Fresh bakes byte-identical. Bake side also writes
  `visual_height_sha256` + a `self_check` into `visual_height_report.json` and
  asserts the invariant at bake time. Unit tests:
  `test_visual_heightfield_mips.py` (fresh passes / cross-bake stale caught).
- **FAR=0 semantics** (recon open ruling) still unresolved: after rung b,
  `MC2_TERRAIN_VISUAL_DISPLACE_FAR=0` means "legacy point-sample coarse", i.e.
  mips are NOT applied when FAR fades to 0 at a vertex (mips ride the mode-2
  displacement mix). Fine for now (gate-consistent), revisit if FAR becomes a
  shipped quality knob.
- **gaea_peaks_01 renders unlit** (black terrain). Out of scope here, but a
  lighting/albedo pass on generator output would make the downward poses
  useful for interior regressions and the map usable for eyeball review.
- Stock maps have no `.beauty` sidecars -> mips absent -> geomorph inert; the
  whole ladder rides `MC2_TERRAIN_VISUAL_DISPLACE` (default OFF). OFF path is
  byte-identical (fix_F0 vs pre-fix F0 captures: max channel delta 0).
