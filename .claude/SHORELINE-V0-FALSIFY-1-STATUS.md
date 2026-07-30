# SHORELINE-V0-FALSIFY-1 — offline falsification of the B-spline-residual shoreline cure

**Lane:** SHORELINE-V0-FALSIFY-1 · worktree `A:/Games/mc2-main` · branch
`claude/nifty-mendeleev`. **Offline only** — no build, no deploy, no launch, no
push. Everything below is Python over the shipping heightfields on disk.

**VERDICT: NO-GO. Do not implement.** The design's own decision rule
("if median reach is under ~40 wu the design must be abandoned") is failed on
all four maps at the design's own parameters — mc2_17, the map the complaint is
about, comes in at **5–6 wu**, roughly 1/20th of the threshold and 1/20th of a
128 wu riser.

And a second, larger result: **mc2_17's waterline is not stair-stepped in the
shipping heightfield at all**, at either the water level or the drawn
silhouette level. The design's root-cause story does not describe the map it
was written for. A negative result here saves an implementation, a build, and a
fourth rejected shoreline change — and it redirects the search.

Artifacts (images + JSON + the scripts, all reproducible offline):
`.claude/shoreline-v0-falsify-1/`.

---

## 0. META REVIEW — premises checked against reader source, not the brief

Six premises checked. **Two were wrong, one materially.**

| # | Premise | Verdict |
|---|---|---|
| 1 | mc2_17 waterElevation ≈ 120 wu | **CONFIRMED, exactly 120.0** — `[Water] f Elevation` in the shipping `mc2_17.fit` |
| 2 | alphaDepth: mc2_17=10, mc2_10=10, mc2_01=15, mc2_05=20 | **ALL FOUR CONFIRMED** from `[Water] f AlphaDepth` in the shipping `.fit`s |

Both of those are read by the engine from the **same `.fit` block I parsed** —
not from the pak, which `editor/EditorInterface.cpp` writing
`Terrain::mapData->alphaDepth` might suggest. `mclib/terrain.cpp:4633-4648`:

```
result = fitFile->readIdFloat( "Elevation", mapData->waterDepth );
waterElevation = mapData->waterDepth;
...
result = fitFile->readIdFloat( "AlphaDepth", mapData->alphaDepth );
```

Also checked that no **loose `.pak`** shadows the FST for these maps: lanes
`0.4` and `0.4c` each carry exactly one loose pak and it is `gaea_peaks_01.pak`,
a custom map. mc2_01/05/10/17 come from `mission.fst` in every lane.
| 3 | "reuse `mission_terrain_analyzer.py` rather than writing a new decoder" | Done — but its **default `--missions-dir` points at data the game does not ship** (see below) |
| 4 | Silhouette is the iso-line of bilinearly interpolated `WaterThickness` | **CONFIRMED from source** — but the level is **not** `waterElevation` (below) |
| 5 | per-map shore \|grad\| mc2_17 0.101 / mc2_10 0.219 / mc2_05 0.259 / mc2_01 0.366 | **PARTIALLY REPRODUCED** — ordering and the mc2_17-is-flattest claim hold; the mc2_10 value does not (below) |
| 6 | Staircase is caused by the *water-tile admission* being tile-quantized | Not in the brief, but I checked it as a rival: **REFUTED**, admission and discard coincide exactly (below) |

### 0a. DATA TRAP — the analyzer's default mission dir is not the shipped data

`tools/terrain_beautify/mission_terrain_analyzer.py:450` defaults
`--missions-dir` to `A:/Games/Carver5-feasibility/data/missions`, and four other
tools in that directory plus three test files hard-code the same path as "stock
mission data". **It is not what the engine loads.** Every one of the four maps
differs there:

| map | Carver5-feasibility | mc2-srcdata | **shipped (`mission.fst`)** |
|---|---|---|---|
| mc2_17 | `33e3056c470d` (2.9 MB) | `f3bdd2387b02` (394 KB) | **`f3bdd2387b02`** |
| mc2_10 | `58d1c2f5aa5f` | `c603169bd555` | **`c603169bd555`** |
| mc2_05 | `3dc43dc6fe50` | `799755c37ca0` | **`799755c37ca0`** |
| mc2_01 | `27c5c3530871` | `0d70a00acd0a` (edited!) | **`7c00c5ae7ded`** (= `mc2_01.pak.old`) |

I therefore extracted the paks/fits **out of the deployed `mission.fst`** and
ran everything on those. Confirmed byte-identical across lanes `0.4`, `0.4c` and
`0.5.1` (`.claude/shoreline-v0-falsify-1/fst_probe.py`). Note mc2-srcdata's
working `mc2_01.pak` is locally modified and does **not** ship — using it would
have silently graded a different map.

**Had I taken the brief at its word and used the analyzer's default, this
report would be a confident go/no-go computed on the wrong terrain.**

### 0b. The silhouette is NOT the `waterElevation` iso-line

`shaders/gos_terrain_water_mdi.frag`:

```
PREC float shoreBlend = max(alphaDepth, 1.0);
PREC float shore = smoothstep(-shoreBlend * 0.5, shoreBlend, shoreWT);
if (shore <= 0.0) discard;            // kill tiles too far above waterline
```

with `WaterThickness = waterElevation - velev`
(`shaders/gos_terrain_water_fast_mdi.vert:202`). So the **drawn edge** is where
`shoreWT = -alphaDepth/2`, i.e. the iso-line of

> `terrain z = waterElevation + alphaDepth/2`   (mc2_17: **125**, not 120)

I measured at **both** levels. It matters — see §3, where the two levels give
qualitatively different answers on mc2_10.

### 0c. Rival hypothesis killed: admission is not the staircase

`EffShoreExt()` (`gos_terrain_water_stream.cpp:154`) = `alphaDepth*0.5`, and the
admission test keeps any tile with **any** corner below `waterElevation +
shoreExt`. That threshold is *exactly* the discard level in §0b, so the admitted
tile set always circumscribes the discard contour and never clips it. The
silhouette is genuinely sub-tile. **The design's premise survives this test.**

### 0d. Reconciling the brief's \|grad\| numbers

The brief's values trace to the `_note` in
`tests/visual/bookmarks/mc2_17_water_wide.json`. Mine, on shipped data:

| map | median \|grad\| on contour-crossing cells | median \|grad\| over the \|z−W\|<25 band | brief |
|---|---|---|---|
| mc2_17 | 0.1185 | 0.0441 | 0.101 |
| mc2_10 | 0.3321 | 0.0168 | 0.219 |
| mc2_01 | 0.3489 | 0.2106 | 0.366 |
| mc2_05 | 0.3067 | 0.2488 | 0.259 |

The load-bearing claim — **mc2_17 is the flattest shore by roughly 3×** — is
reproduced. The mc2_10 figure is not (0.332 vs 0.219); the brief's band
definition differs from either of mine. Use the definition, not the number.

---

## 1. Method

Read-only, deterministic, no RNG. Scripts in `.claude/shoreline-v0-falsify-1/`.

1. Heightfield `Hc` decoded from the FST-extracted `.pak` via
   `mission_terrain_analyzer` (`read_packets` / `locate_mapdata` /
   `extract_layers`). 128 wu per vertex.
2. `Hs` = C2 cubic B-spline reconstruction. The basis `B3` has support radius 2
   — the brief's "footprint 2 tiles" — so `scale=1` is the literal reading; I
   also swept `scale` up to 8 (footprint radius 16 tiles) to find the wall.
3. **Both fields evaluated on an 8–16× supersampled grid**, because the engine
   interpolates `Hc` bilinearly per fragment and the proposed `Hs` is a
   *continuous* reconstruction. Sampling only at lattice vertices understates
   the operator — my first pass did that and I redid it. Reported numbers are
   the supersampled ones.
4. `P = C·tanh((H − Hs)/C)`, `C = k·alphaDepth/2`; silhouette field
   `H_eff = H − P`.
5. Reach measured **two ways**: first-order `|P|/|grad|`, and — decisively —
   **geometrically**, by extracting both contours and KD-tree querying every
   proposed-contour sample against the stock contour. The geometric number has
   no division by a near-zero gradient and is the one I quote.

---

## 2. THE DECISION RULE — failed on all four maps

Design parameters: footprint radius 2 tiles (`scale=1`), `k=1` so `C =
alphaDepth/2`. **Geometric median displacement of the waterline, world units:**

| map | C (wu) | disp @ z=W | **disp @ silhouette (z=W+b/2)** | p90 | max | rule: ≥40 wu? |
|---|---|---|---|---|---|---|
| **mc2_17** | 5.0 | 6.0 | **5.1** | 12.6 | 55.8 | **NO — 8× short** |
| mc2_10 | 5.0 | 8.2 | **36.3** | 79.8 | 117.5 | NO (marginal) |
| mc2_01 | 7.5 | 5.1 | **5.1** | 12.2 | 48.4 | **NO — 8× short** |
| mc2_05 | 10.0 | 6.8 | **5.8** | 14.2 | 51.9 | **NO — 7× short** |

**Against the design's own predictions:**

| prediction | mc2_17 actual | factor short |
|---|---|---|
| median \|P\| ≥ ~5 wu | **0.33 wu** (shore band) | **15×** |
| median horizontal reach ≥ ~100 wu | **5.1 wu** | **20×** |

median `|P|` in the shore band at design parameters: mc2_17 **0.33**, mc2_10
**0.28**, mc2_01 **1.23**, mc2_05 **1.79** wu. The residual is roughly an order
of magnitude smaller than the design assumed.

### Why more kernel does not rescue it

Sweeping the footprint (`.claude/shoreline-v0-falsify-1/sweep.json`) shows the
clamp, not the kernel, is binding. On mc2_17 at `k=1`:

| footprint radius | median disp | tanh saturation | staircase index |
|---|---|---|---|
| 2 tiles | 6.0 wu | 1.3% | 0.240 |
| 4 tiles | 19.0 wu | 17.2% | 0.274 |
| 8 tiles | 27.4 wu | 47.4% | 0.268 |
| **unclamped, 8 tiles** | **90.5 wu** | — | 0.195 |

Growing the kernel raises the raw residual, but `C = k·b/2` saturates it — at
footprint 8 tiles nearly half the shore band is clamped, and a saturated region
is exactly `H_bilinear ∓ C`, i.e. **the original bilinear facets, offset by a
constant**. Reach asymptotes around 27 wu while the staircase index gets
*worse*, not better. The only arm that reaches ~90 wu is the **unclamped** one —
which is the terrain-smoothing dead end already documented as the V3 demotion in
`tools/terrain_beautify/cook_shoreline.py` (the waterline floats up-slope; max
displacement 292 wu on mc2_17, and components collapse 12 → 6, i.e. islands are
eaten).

`C` is not an incidental safety margin. It is the whole constraint, and it is
set by `alphaDepth`, which is 10 wu on mc2_17. **You cannot move a contour 128
wu with a 5 wu budget on a 0.12 slope.** That is arithmetic, not tuning.

---

## 3. THE BIGGER RESULT — mc2_17's waterline is not stair-stepped

Shape of the **stock** silhouette (`z = W + b/2`), supersampled. "Staircase
index" = length-weighted fraction of contour within ±10° of axis-aligned; **an
isotropic curve scores 0.222**, so that is the floor, not zero.

| map | staircase index | corners / 1000 wu | components | contour length |
|---|---|---|---|---|
| **mc2_17** | **0.260** | 2.9 | 15 | 94,124 wu |
| **mc2_10** | **0.737** | 8.7 | 53 | 127,967 wu |
| mc2_01 | 0.226 | 3.1 | 17 | 65,296 wu |
| mc2_05 | 0.297 | 3.4 | 7 | 51,201 wu |

**mc2_10 is massively tile-locked (0.737 vs a 0.222 floor). mc2_17 is not
(0.260) — it is within noise of an isotropic curve.**

The pictures agree, and they are the artifact worth grading:

- `mc2_17_zoom_intro_s1_k1.png` — the mission's own authored camera
  (`[Cameras] PositionX/Y = −6656, −185`), 2800 wu window, **128 wu tile
  lattice drawn in blue**. The stock waterline is a smooth sinuous curve that
  ignores the lattice. The proposed contour is glued to it.
- `mc2_17_zoom_shelf_center_s1_k1.png`, `..._shelf_east_...` — the three
  WATER-MEANDER-CLAMP-1 verdict bookmarks. Same story.
- `mc2_10_zoom_center_s1_k1.png` — **contrast case.** Here the dotted line (the
  real silhouette, `z=85`) is a textbook right-angle staircase on the tile
  grid, while the solid `z=80` line beside it is smooth. This is what the
  design describes, and it is on mc2_10, not mc2_17.
- `mc2_17_fine_s1_k1.png` — whole map, stock vs proposed vs unclamped.

On mc2_10 the operator does bite: staircase index **0.737 → 0.330**, corners
8.7 → 3.5, at 36.3 wu median displacement. That is a genuine effect on the map
that genuinely has the defect. It still does not clear the 40 wu rule, and
mc2_10 is not the map under complaint.

**So the mechanism cannot be what the user is looking at on mc2_17.** The
heightfield there does not contain the staircase the cure is designed to
remove.

---

## 4. What else could produce it — one lead, measured, NOT proven

Ruled out already, by other lanes and by me:

- `MC2_WATER_SHORE_MASK` (default-**ON**): `.claude/WATER-MC2-17-1-STATUS.md`
  proves it **byte-identically inert on mc2_17** — mask on/off and
  `MC2_TERRAIN_SHORELINE=0` all give the same frame hash.
- `MC2_WATER_SHORE_MEANDER`: default reverted to **0** in `67b25f47`, so the
  fBm term is off.
- Tile admission: refuted in §0c.

Remaining lead, which I *can* measure offline. `WaterThickness` is bilinear, so
it is C0 but **not C1** — its slope jumps at every tile boundary. The shore ramp
spans `1.5·alphaDepth` of thickness, which on a flat shelf is *hundreds* of
world units wide, so those slope kinks get smeared across a large screen area.
Metric: mean |∂²(alpha)| on tile-boundary lines ÷ same in tile interiors
(1.0 = no lattice preference):

| map | ramp covers | **facet index, stock** | facet index, proposed |
|---|---|---|---|
| mc2_17 | 5.4% of map | **4.16** | 1.58 |
| mc2_10 | 2.4% | 3.65 | 1.93 |
| mc2_01 | 3.0% | 1.93 | 1.07 |
| mc2_05 | 2.2% | 4.39 | 1.65 |

The stock shore ramp **is** strongly lattice-faceted (4× more curvature on tile
lines than off them on mc2_17), and the proposed operator would cut that by
~60% — because in the unsaturated regime `H_eff ≈ Hs`, which is C2.

**I am not claiming this is the defect.** Honest limits:

- At the mc2_17 intro view the ramp is narrow and the two alpha renders
  (`mc2_17_alpha_intro.png`) look near-identical — in one spot the proposed is
  marginally *blockier*. The facet-index gain is dominated by wide-ramp
  flat-shelf regions I have not tied to any pose the user has looked at.
- The metric is **partly true by construction**: a bilinear field is linear
  along a row inside a tile, so its interior second difference is small
  regardless of whether the result is visually objectionable. The ratio proves
  the curvature is lattice-concentrated; it does **not** prove it is visible.

This is a lead for a runtime A/B, not a finding.

---

## 5. Honest framing, as required

- The design's author is right that an operator which can never delete water is
  a pure dilation, and can only fill notches, never shave seaward tongues. The
  two-sided weaker guarantee is the honest trade. **None of that is the reason
  it fails here.** It fails on magnitude: the perturbation budget `C = k·b/2`
  is 5–10 wu, the terrain slope at the shore is 0.12–0.35, so the contour can
  move ~5–36 wu, and a tile riser is 128 wu. Even a *perfect* operator inside
  that budget cannot cure a 128 wu step.
- This is the **third** shoreline mechanism to die on the same structural
  point, and the pattern is worth naming: SHORELINE-STAIRCASE-1 widened the
  band without moving the risers; WATER-MEANDER-CLAMP-1's `|grad|` clamp
  suppressed the cure exactly where the cure was needed. This design's `C`
  clamp does the same thing — it binds hardest on the flattest shore, which is
  where the defect is worst. **Any knob whose budget is specified in elevation
  units inherits `1/|grad|` and will be weakest on flat shelves.** A cure has
  to be specified in *horizontal distance* or it will keep failing this way.
- The operator is not useless: it demonstrably de-facets the contour (mc2_17
  corners/1000 wu 3.0 → 0.8) and de-facets the alpha ramp (facet index 4.16 →
  1.58) — it converts a piecewise-linear silhouette into a C2 curve *without
  moving it*. If the complaint were "the coastline is polygonal", this is the
  right shape of fix. The complaint is "stair stepping", and the line does not
  move.

## 6. Residual risk / what I did NOT do

- **No engine involved.** Nothing here is verified against a rendered frame. A
  runtime A/B could still disagree with the geometry — in particular I have not
  seen the user's actual mc2_17 intro frame, only the mission's authored camera
  position from `[Cameras]`, which is not proven to be the intro camera.
- Terrain **LOD decimation** at gameplay altitude is a live rival hypothesis I
  cannot test offline: if shore blocks render at `lodStep>1` the land/water
  edge would step at 256/512 wu granularity, which would look exactly like the
  complaint and is invisible to a full-resolution heightfield analysis.
  **This is the first thing I would test next.**
- I modelled the tile interior as bilinear; the engine rasterizes two triangles
  per quad, so the true contour is piecewise-linear per triangle. Displacement
  magnitudes are unaffected; fine shape details near quad diagonals differ.
- Contours come from marching squares on a 8–16× grid (8–16 wu). Displacements
  of a few wu are near that resolution; the 5–6 wu medians should be read as
  "single-digit", not as three significant figures. It does not change any
  verdict — the threshold is 40.
- `alphaDepth` is per-mission authored data; nothing here says raising it is
  safe. Widening the band was tried and rejected already
  (`SHORELINE-STAIRCASE-FIX-1`).
