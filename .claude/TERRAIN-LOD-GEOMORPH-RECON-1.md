# TERRAIN-LOD-GEOMORPH-RECON-1

RECON ONLY. No source changes, no build, no launch. Worktree
`A:/Games/mc2-controlmap-sample-1` @ HEAD `1b3f6055` (TERRAIN-VISUAL-HEIGHT-S2:
displacement at all LOD bands with stitch-safe edges).

User report: LOD chunk **geomorph was abandoned** in a past arc. Symptom =
silhouette sometimes lost at LOD transitions — a coarser band misses
peak/ridge vertices, so the skyline "morphs" as the camera moves. Higher stakes
now: `1b3f6055` runs displacement at ALL LOD bands (coarse bands Z-swap to bake
at their own vertex density), so band transitions change effective height detail
and the skyline pop is amplified on high-relief maps (gaea_peaks_01, 1200wu).

---

## 1. Archaeology — the "abandoned geomorph" attempt

**Verdict: there is no literal `geomorph`/`morph` code that shipped and was later
ripped out.** Searched three ways:

- `git log --all --grep=geomorph` in the worktree → **0 hits**. `--grep=morph`
  hits only alpha-stage EffectAdapter commits (`123b8f7f`, `5861ee3f`,
  `ced9e40b`, `15a7df3d`) — unrelated (particle `MakeEffect` refactor).
- `grep -rin geomorph` across `.cpp/.h/.vert/.frag/.md` in the worktree AND the
  root repo `A:/Games/mc2-opengl-src` → **0 hits** (the root grep timed out at
  2 min but returned no matches before timeout; worktree grep is clean).
- The only `silhouette` mentions in the LOD path are cosmetic/edge, not vertex
  morph: `gos_terrain_lod_chunk.cpp:1331` ("silhouette acceptance pass has hard
  numbers per band" — a *counter/acceptance* comment, S2 per-band displaced
  counts) and `terrain_lod_chunk.frag:964` (map-perimeter edge-feather, the
  "hard, perfectly-straight map-perimeter silhouette dissolves" backdrop).

**Interpretation.** "Geomorph was abandoned" is almost certainly **conceptual,
not a reverted branch** — geomorphing (vertex-shader lerp between own-LOD and
parent-LOD height across the band boundary) was *never implemented*. What EXISTS
today is the LOD-crack machinery that people conflate with geomorph:

- **Edge-stitch** (Phase 10.4, `terrain_lod_chunk.vert:113-141`): a fine edge
  vertex that is a T-junction against a coarser neighbour is snapped onto the
  coarse segment (`h = mix(h0,h1,t)`). This kills *horizontal* cracks at the
  seam; it does **not** morph the interior across a band change, and it does
  **not** preserve peaks.
- **Skirts** (Phase 6 / 10.2b, `isSkirtFlag`, `u_skirtDepth`,
  `skirtEdgeMasks`): vertical aprons that hide the *gap* under a seam. Also not
  a morph.
- **Hysteresis** (`chooseLodLevel`, `terrain.cpp:238-274`): 10% linear demotion
  band. Reduces *flicker* (rapid band toggling in place) but does nothing for
  the one-time pop when a chunk genuinely crosses a threshold.

So the "abandonment" is best read as: the crack/skirt/hysteresis defenses were
built and shipped; the actual **height geomorph (temporal + LOD lerp) was
scoped out** and never landed, leaving the silhouette pop unmitigated. No
tombstone commit or design doc explains a removal because there was nothing to
remove — confirm with the arc owner, but the code + history support "never
built."

---

## 2. Current LOD band anatomy (cite)

**Band count = 6.** `LOD_STEPS[6] = {1,2,4,5,10,20}` (`terrain.cpp:205`).
`lodStep` = vertex stride baked into the patch VBO; a block is 20 quads
(`blockOriginX = blockX*20`, `gos_terrain_lod_chunk.h:9-12`).

**Distance thresholds** (`terrain.cpp:210-217`, doubled 2026-06-09; runtime-
scaled by `MC2_TERRAIN_LOD_CHUNK_DIST_SCALE`, default 1.0):

| lodLevel | lodStep | upper distance (wu) |
|---|---|---|
| 0 | 1  | < 6 000 |
| 1 | 2  | < 14 000 |
| 2 | 4  | < 30 000 |
| 3 | 5  | < 60 000 |
| 4 | 10 | < 120 000 |
| 5 | 20 | ≥ 120 000 |

**Selection & frame-to-frame change** (`chooseLodLevel`, `terrain.cpp:240-275`):
per block, from squared distance. Promotion (finer) is **immediate**; demotion
(coarser) requires `distSq < thresh² * 1.21` to *stay* fine → **10% linear
hysteresis, demotion-only** (comment line 238-239). `MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=k`
pins every block for camera-isolation testing.

**Vertex density per band = point decimation, NOT max-preserving.** Sample
positions are built by pure stride stepping:
`makeSamplePositions(quadCount, lodStep)` = `for p in 0..quadCount step lodStep`
(`gos_terrain_lod_chunk.cpp:343-347`). Each unique `(qcX,qcY,lodStep)` gets one
VBO/IBO (`:291`, `patchKey :333-337`). The vert then does a **point sample** of
the coarse SSBO: `h = heights[mapX + mapY*u_mapSide]` (`vert:111`) where
`mapX = blockOriginX + localOffset.x` and `localOffset` is stride-baked. There
is **no max / ridge-aware filter anywhere** — it is every-Nth point sampling.

**S2 all-LOD displacement (the new amplifier)** — `vert:143-194`,
`u_visualDisplace==2`: for a coarse/LOD vertex, `h = mix(h, hv, u_visualDisplaceFar)`
where `hv = heightsFine[mapX*4 + mapY*4*u_visualSide]` (`:189-191`). Because the
bake is a **uniform 4× subdivision of the same grid**, `mapX*4` is always an
integer bake index landing **exactly on the coarse vertex** — and the bake there
is bilinear-corner-pinned, so `heightsFine[mapX*4] == elev[mapX]` (see §3). Net:
the coarse band displaces to the *same coarse silhouette* it already had — the
inter-vertex ridge is still gone. S2 changes *interior detail feel* between
bands (different bake sampling density visually) but does **not** rescue peaks.

---

## 3. Pop root cause — VERDICT

**Point-sample decimation drops ridge maxima; confirmed.** Two independent
confirmations:

1. **Runtime path is every-Nth point sampling** (§2): `makeSamplePositions`
   steps by `lodStep`, `vert:111` point-reads the coarse SSBO. A ridge/peak
   vertex whose map index is not a multiple of `lodStep` **vanishes** from the
   coarse lattice → the silhouette drops to the lower of its two surviving
   neighbours (max-height loss). Classic LOD silhouette collapse.

2. **The bake cannot rescue it as authored.** `tools/terrain_beautify/
   visual_heightfield.py` produces the 4× "visual" heightfield by
   `upsample_corner_pinned` = **separable bilinear upsample with exact corner
   preservation** (`visual_heightfield.py:48-67`; docstring: "bilinear weight is
   exactly 1 at coarse vertices"; `max_corner_error_wu == 0`). There IS a
   `nearest_coarse` helper (`:130`) and a `_box3` smoother (`:67`) but **no
   max-downsample mip chain**. So `heightsFine` at any coarse index equals the
   original coarse elevation — displacement mode 2 reads exactly `elev` at coarse
   verts and gains nothing for silhouette. The bake only adds detail *between*
   coarse verts, which the coarse lattice never samples.

**Why it's worse now (`1b3f6055`).** Before S2, only LOD0 near band displaced
(`u_visualDisplace==1`, 4×-finer interior). Now coarse bands also swap Z to their
bake sample (`==2`), so as the camera moves and a chunk changes band, the
*effective height field it reads changes character* (fade `u_visualDisplaceFar`
scales it), stacking a bake-detail pop on top of the pre-existing decimation
pop. On high-relief maps (gaea_peaks_01) the coarse decimation error is large
(tall thin ridges), so both effects are visually loud.

**Hysteresis does not fix this** — it only prevents *toggling in place*; a real
threshold crossing still snaps the silhouette in one frame with no temporal lerp.

---

## 4. Options matrix

### (a) Geomorph proper — VS lerp own-LOD ↔ parent-LOD by fractional distance
Add a per-block morph factor `m∈[0,1]` (0 at band entry, 1 near next threshold),
computed from distance the same way `chooseLodLevel` already has distSq. In the
vert, sample BOTH the own-LOD height and the parent (one-coarser) height for this
vertex and `mix(hFine, hCoarse, m)`. Smooths the *temporal transition* so the pop
becomes a slow slide instead of a snap. **New leverage (bake SSBO):** parent
height is *free* — `heightsFine`/coarse SSBO already give ANY-resolution sample at
any grid point; the parent-LOD value at a fine vertex is just the coarse-lattice
interpolation the stitch code already computes (`vert:118-141`). No second VBO,
no CPU pre-pass. **Cost:** one extra SSBO read + a uniform `m` per draw. **Caveat:**
morph alone does NOT restore a dropped peak — it lerps between two silhouettes,
both of which may already miss the ridge. Pair with (b).

### (b) Max-preserving decimation — coarse vertex takes MAX (or ridge-aware) of covered fine samples
Bake-side change: emit a **per-LOD mip chain with a MAX (or signed-max /
ridge-aware) reduction** so the coarse SSBO sample at stride N already carries the
peak of the N×N fine cell it represents. `visual_heightfield.py` is the natural
home (`reshape_visual`/mip section). Then `heightsFine[mapX*4]` for a coarse band
returns the *ridge max* under that footprint, not the corner value → **the
silhouette never drops peaks**, at any band. Cheap, CPU/bake-side, zero per-frame
cost, rides the existing SSBO plumbing (add mip levels to binding 26 or a
sibling SSBO indexed by lodStep). **This is the direct fix for silhouette LOSS.**
Caveat: a pure MAX filter inflates valleys' apparent floor near ridges (peaks
bleed outward); a *ridge-aware* / percentile reduction is the refined version —
prototype MAX first, measure.

### (c) Distance-band widening / hysteresis tuning — hide the pop farther out
Bump `LOD_DIST_THRESH` (or set `MC2_TERRAIN_LOD_CHUNK_DIST_SCALE > 1`) so
transitions occur where the pop is sub-pixel. Zero-code (env-var) mitigation,
already thresholds were doubled once (`terrain.cpp:206`). **Palliative only** —
pushes the artifact off-screen at the cost of triangle count; does not fix the
mechanism. Good as an *interim* knob and as an A/B baseline.

### (d) Skirt / apron tweaks — hide cracks, NOT skyline
Skirts (`isSkirtFlag`, `u_skirtDepth`, `skirtEdgeMasks`) seal the vertical *gap*
under a seam. They do nothing for a lost peak on the skyline. **Out of scope for
this symptom** — mention only to rule out.

### Recommended ladder
1. **(b) Max-preserving bake mip first** — kills the silhouette-LOSS root cause,
   bake-side, cheap, rides binding-26 SSBO. This alone removes "peak vanishes."
2. **(a) Geomorph VS lerp second** — smooths the *remaining* transition so any
   residual detail change slides instead of snaps; parent height is free from the
   SSBO. Together (b)+(a) = correct silhouette + smooth transition.
3. **(c) DIST_SCALE as the env-var safety valve** during bring-up / for low-end.
   Ship default 1.0.

---

## 5. Interactions / landmines

- **Stitch (S2) edge agreement** (`vert:118-141`, `156-194`): the stitch snaps a
  fine T-junction vertex onto the coarser neighbour's segment by re-deriving the
  same coarse index and mixing two bake samples. A max-mip (b) MUST feed the
  stitch the SAME reduced values on both sides of a seam, else edges disagree and
  cracks reappear. Because both sides read the same SSBO by integer bake index
  (`mapX*4`), a per-lodStep max-mip keyed identically on both sides stays
  crack-free *by construction* — but the mip index used on the fine side and the
  coarse side must match the neighbour's lodStep, exactly as `u_edgeStitch`
  already encodes the coarser stride per edge. Verify the mip lookup uses
  `u_edgeStitch` stride, not the local lodStep, on stitched edges.
- **Corner-pin invariant** (`visual_heightfield.py` `max_corner_error_wu==0`;
  `vert:56-62` corner verts stay on the coarse line): a MAX mip would break
  corner-pin if applied naïvely (corner would jump to a neighbour's peak). Keep
  corners bilinear/exact; apply the max reduction only to *interior* coarse
  samples, or accept that corners define the seam and max only fills between.
  Geomorph (a) must also pin `m` so shared verts across two blocks with different
  `m` still meet — safest is `m` constant per block and equal on shared edges
  (blocks at a seam are adjacent LOD, so drive `m` off the *coarser* block).
- **`MC2_TERRAIN_VISUAL_DISPLACE_FAR`** (`gos_terrain_lod_chunk.cpp:908-917`,
  `u_visualDisplaceFar`, default 1.0): scales mode-2 displacement 0..1. A geomorph
  factor could reuse/compose with this knob, but note it currently fades the
  *whole* bake toward the coarse `h`; a max-mip changes what "coarse `h`" means,
  so re-baseline the FAR fade after (b) lands (0.0 must still equal the
  byte-identical legacy coarse path — which after (b) is the max-preserved coarse,
  not the point-sampled one; decide whether FAR=0 means "legacy point sample" or
  "max-preserved coarse" and gate accordingly).
- **Integer bake-grid coincidence** is the load-bearing property: bake is a
  uniform 4× of the map grid, so every coarse/LOD vertex hits an exact bake index
  (`vert:143-148` comment). Any mip must preserve this integer coincidence
  (power-of-2 or lodStep-aligned reductions) or interpolation error creeps in.

---

## 6. Debug viz proposal — LOD-band + morph-factor color view

`u_diag` is an established int channel in `terrain_lod_chunk.frag`: a **bitmask**
for compositing modes AND **exact-value escapes** for standalone views
(`frag:904-913`, e.g. `u_diag==30`, `==31`; `==40` is the shadow-tier tint,
`frag:428-452`). Selected by `MC2_TERRAIN_LOD_CHUNK_DIAG`. Propose two new
exact-value modes (no bitmask collision):

- **`u_diag==41` LOD-band tint** — flat color per band from lodStep:
  0=green, 1=cyan, 2=blue, 3=yellow, 4=orange, 5=red. Needs `lodStep` (or
  `lodLevel`) reaching the frag. Today `u_lodStep` is a **vert** uniform
  (`gos_terrain_lod_chunk.cpp:568` `s_locLodStep`) and there is NO lodStep
  varying to the frag — add `flat out int v_lodLevel;` in the vert (cheap) OR
  pass a `u_lodLevel` frag uniform (per-draw, simplest, matches how
  `u_shadowTier` already feeds the `==40` view).
- **`u_diag==42` morph-factor heat** — grayscale/heat of the geomorph `m` (once
  (a) exists); shows exactly where and how fast transitions slide. Reuse the same
  varying/uniform seam as the band tint.

Wiring is one-file (frag + one uniform upload in `gos_terrain_lod_chunk.cpp`
next to `s_locShadowTier`/`s_locLodStep`); mirrors the `==40` shadow-tier
precedent exactly. A band view also unblocks the pile of other needs-eyes terrain
items (stitch/skirt/overlay debugging) — high leverage.

---

## 7. Acceptance — exercising band transitions repeatably

Smoke camera is **static** (fixed spawn, 30 s), so it never crosses a band
threshold on its own → the pop is invisible to the current gate. Cheapest
repeatable exercises, in order of preference:

1. **`MC2_TERRAIN_LOD_CHUNK_DIST_SCALE` sweep against a static camera.** With the
   camera fixed, running two captures at DIST_SCALE ≈ 0.3 vs 1.0 forces the SAME
   on-screen chunks into DIFFERENT bands (thresholds shrink under the camera). A
   pixel-diff between the two isolates the silhouette delta a band change causes —
   no camera motion needed, fully scriptable via env var. Best acceptance proxy;
   pairs with the `==41` band view for eyeballing.
2. **`MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=k` A/B** (`terrain.cpp:246-251`): capture at
   FORCE_LOD=0 (finest, ground-truth silhouette) vs =3/=4 (coarse). Diff = the
   exact silhouette loss the fix must close. Deterministic, static camera, already
   plumbed. Use as the **regression oracle**: (b) should shrink the FORCE_LOD=0
   vs =4 silhouette diff toward zero.
3. **Far-spawn / zoom-out mission** on gaea_peaks_01 (1200wu, high relief) so the
   static camera already sits far enough that visible chunks span 2-3 bands —
   picks up the interaction under real distances. Heavier to author; use once (1)
   and (2) pass, as the realistic confirmation.

Acceptance bars: (b) → FORCE_LOD=0-vs-4 skyline pixel diff on gaea_peaks_01 drops
below the S2 noise floor at ridge pixels; (a) → DIST_SCALE-sweep transition diff
is temporally monotone (no single-frame snap) in a short dolly capture; both →
stock maps with gate OFF byte-identical.

---

## 8. Open rulings

- **Confirm "abandoned" = never-built** with the arc owner. History/code show no
  reverted geomorph; if a private/foreign branch had one, it's not in this
  worktree or root repo. If truly never built, the ladder is greenfield (good).
- **MAX vs ridge-aware reduction for (b):** start with pure MAX (simplest, proves
  silhouette recovery) or go straight to percentile/ridge-aware to avoid valley
  inflation? Recommend MAX-first, measure valley bleed on gaea_peaks_01.
- **FAR=0 semantics after (b):** does `u_visualDisplaceFar==0` mean legacy
  point-sample coarse (byte-identical to pre-S2) or the new max-preserved coarse?
  Affects the byte-identical-when-off guarantee — needs a ruling before coding.
- **Geomorph `m` ownership at seams:** drive `m` off the coarser block of a seam
  pair so shared edges agree, OR keep `m` per-block and rely on stitch to pin
  edges? Prototype both; stitch-pin is safer.
- **SSBO layout for the mip:** extend binding-26 with mip levels, or a sibling
  SSBO indexed by lodStep? Sibling is cleaner for the FAR-fade re-baseline but
  costs a binding slot — check free slots (binding 26 is the current visual SSBO).
- **Debug seam:** `flat out int v_lodLevel` varying vs `u_lodLevel` per-draw
  uniform — uniform matches the `u_shadowTier`==40 precedent and avoids a vert
  attribute change; recommend uniform.
