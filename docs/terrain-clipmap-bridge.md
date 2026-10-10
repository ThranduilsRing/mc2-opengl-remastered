<!-- doc-status: current | reviewed:2026-10-11 -->
# Terrain clipmap bridge — bake-mip / morph interface contract

**Status: HAND-OFF DOC ONLY.** No engine code, no renderer, no new gate in this
slice (`GEOMORPH-NEXT-SLICES-1` Slice 3, CLIPMAP-BRIDGE). Written per the
successor notes in `.claude/GEOMORPH-NEXT-SLICES.md` §"Slice 3 —
CLIPMAP-BRIDGE". Scope is deliberately narrow: **map what
`TERRAIN-LOD-GEOMORPH-1` already built onto clipmap vocabulary** so a future
clipmap arc lifts these artifacts instead of re-deriving them. It does **not**
design or implement a clipmap renderer (toroidal ring buffers, GPU update
regions, streaming) — that is a separate, much larger arc.

Verified against this worktree's checked-in code (`mclib/terrain.cpp`,
`GameOS/gameos/gos_terrain_lod_chunk.cpp`, `shaders/terrain_lod_chunk.vert`,
`tools/terrain_beautify/visual_heightfield.py`) — the geomorph arc has shipped
here (contrary to the predecessor doc's "lane-local, 0 hits on nifty" note,
which was written before this merge).

---

## 1. Bake-mip level -> clipmap ring level, 1:1

`tools/terrain_beautify/visual_heightfield.py:1104` `MIP_STRIDES = (2, 4, 5,
10, 20)` — one max-reduced level per renderer LOD stride
(`mclib/terrain_update.cpp:14` `LOD_STEPS[6] = {1, 2, 4, 5, 10, 20}`; stride 1 needs
no mip, it reads the fine bake directly). `build_max_mips`
(`visual_heightfield.py:1116`) emits, **for every one of the `side*side`
coarse vertices** (full resolution, not decimated to `side/stride`), the MAX
of the final fine bake over that vertex's +/- `stride/2`-coarse-cell
footprint. This is already a clipmap level stack in every load-bearing sense:

| Bake mip level (`MIP_STRIDES` index) | stride | Clipmap ring level | Ring footprint (world units, 128 wu/coarse-cell) |
|---|---|---|---|
| fine bake (`visual_height_4x.r32`, not a mip) | 1 (4x sub-sample) | Ring 0 (innermost / finest) | 32 wu/sample (128/4) |
| mip[0] | 2 | Ring 1 | 256 wu/sample |
| mip[1] | 4 | Ring 2 | 512 wu/sample |
| mip[2] | 5 | Ring 3 | 640 wu/sample |
| mip[3] | 10 | Ring 4 | 1280 wu/sample |
| mip[4] | 20 | Ring 5 (outermost / coarsest) | 2560 wu/sample |

A clipmap arc should **import `build_max_mips`'s output as the ring content
generator**, not re-derive a reduction formula. The MAX-reduction property
(each ring level never drops a value below what its footprint's peak
requires) is exactly the silhouette-preservation invariant a clipmap needs
when a coarse ring approximates a region no longer covered by a finer one.

**Storage layout** (as shipped, `mclib/terrain_init.cpp:671-678`,
`shaders/terrain_lod_chunk.vert:54-63`): binding-26 SSBO holds
`[fine V*V | mip(2) mapSide² | mip(4) mapSide² | mip(5) mapSide² | mip(10)
mapSide² | mip(20) mapSide²]` — one flat buffer, all levels always resident
together (size-checked as a single `5*mapSide²*sizeof(float)` blob,
`mclib/terrain_init.cpp:690-692`). A toroidal-update clipmap would instead want each
ring level individually addressable/updatable; the **level boundaries in this
blob are already fixed offsets** (`lvl * mapSide * mapSide` floats past the
fine block, `shaders/terrain_lod_chunk.vert:77` `mipH()`), so splitting the
monolithic SSBO into per-ring buffers is a mechanical follow-up, not a
redesign.

## 2. Integer-coincidence invariant (the property that makes toroidal update exact)

**Statement:** the bake grid is a **uniform 4x supersample of the coarse map
grid** (`visual_heightfield.py:76-96` `upsample_corner_pinned`; `V = (N-1)*4 +
1`), and **every renderer-visited coarse-grid index — at any LOD stride, at
any block origin, including stitched/skirt boundary vertices — lands on an
exact integer INDEX into the bake arrays.** No renderer lookup path ever
needs a fractional bake-array index or a runtime bilinear resample of the
bake itself. This is an **index/lattice** guarantee, not a value guarantee —
keep the two separate:

- **Fine bake, INDEX:** coarse vertex `(i, j)` always maps to fine array
  index `(i*4, j*4)` — this is fixed grid arithmetic (`V=(N-1)*4+1`), true for
  every bake this tool produces, reauth or not.
- **Fine bake, VALUE at that index:** only the default (no `--reauth`) bake
  path guarantees `fine[i*4,j*4] == coarse[i,j]` exactly (`corner_err == 0`,
  printed as `corner_err=0 PASS` on every default bake — confirmed for all 24
  campaign maps' `tests/terrain/beautify/*.beauty/visual_height_4x.r32` as of
  this slice, see §7). **`--reauth` deliberately UNpins corner values**
  (user ruling "UN PIN THE CORNERS. KEEP THE SHAPE" — `visual_heightfield.py`
  module docstring) within `shape_tolerance`/`max_drift` bounds; the three
  curated bakes already shipped (`mc2_01`/`mc2_17`/`mc2_24`, all baked with
  `--reauth`) have nonzero `max_corner_error_wu` (26.3 / 14.5 / 25.0 wu per
  their `visual_height_report.json`, `corner_pinned: false`) by design — this
  is expected, not a bug, and does **not** break the index invariant above.
  A clipmap ring-boundary blend must therefore tolerate small value
  discontinuities at ring seams from reauth'd bakes (bounded by `max_drift`),
  even though the index alignment across rings stays exact.
- **Mip levels:** stored at **full `side*side` resolution**, not decimated to
  `side/stride` (`build_max_mips` docstring, `visual_heightfield.py:1119-1120`:
  "no decimation, so stitch lookups at any stride-aligned index are
  well-defined"). This means a mip level is a valid, exact lookup for **any**
  coarse index 0..side-1 — not just indices that are multiples of its stride.
  That is precisely what a clipmap ring needs: the ring's data must be
  addressable at the *current* toroidal wrap offset, which is not
  stride-aligned in general once the ring has scrolled.
- **Cross-level agreement at ring boundaries:** `parentBandH()`
  (`shaders/terrain_lod_chunk.vert:86-96`) bilinearly samples the *coarser*
  ring's own lattice (stride `P`) at a *finer* ring's coarse index `(cx, cy)`.
  Because `P` is not always a multiple of the finer stride (4 -> 5 is the one
  non-power-of-two transition in `LOD_STEPS`), this is a genuine bilinear
  read, not a lattice point-sample — but it is still exact at every point
  where the two lattices DO coincide (`(cx,cy)` a multiple of `P`), which is
  the standard Losasso/Hoppe clipmap transition-region contract: continuous
  everywhere, exact at shared lattice points.

**Why this matters for a future clipmap:** toroidal ring updates only stay
correct if "shift the ring by N cells" always lands the new edge strip on the
*same* underlying lattice the old center was sampling — i.e. the update
region's cell boundaries must be integer multiples of the ring's own stride
in the SAME coordinate space the bake used. Because the bake is a single
fixed 4x-uniform grid (not adaptive, not re-quantized per ring), this holds
by construction: **any two ring levels agree exactly at every coarse-grid
point, for any wrap offset**, so a toroidal update never needs to interpolate
across a seam it didn't already have to interpolate across in the current
non-toroidal renderer.

**Tested by:** `tools/terrain_beautify/test_visual_heightfield_clipmap_bridge.py`
(added alongside this doc) — asserts the grid-math identity (`V=(N-1)*factor+1`
and exact-index corner mapping) and the mip full-resolution/any-index-valid
property, both against synthetic fixtures and the real committed bakes in
`tests/terrain/beautify/{mc2_01,mc2_17,mc2_24}.beauty/`. A future clipmap
change that silently re-quantizes or decimates the bake grid will fail this
test.

## 3. Morph factor -> clipmap transition region

`computeMorphFactor()` (`mclib/terrain.cpp:309-330`) already **is** a clipmap
"transition region" in the Losasso/Hoppe sense: a per-block scalar `m` in
`[0,1]` that blends a finer ring's own surface toward the next-coarser ring's
surface as the camera crosses the demotion threshold, so the level switch is
geometry-continuous rather than a one-frame pop. Rules that transfer
verbatim to per-ring transition bands:

- **`m` is constant per block**, and **perimeter vertices never morph**
  (`shaders/terrain_lod_chunk.vert:355` `!onPerim` guard) — every block's
  shared edge is pinned to the same coarse stitch line regardless of that
  block's own `m`, so adjacent blocks (or, in a clipmap, adjacent ring cells)
  never disagree at a seam. This is the seam-ownership invariant a clipmap's
  ring-boundary blend must preserve.
- **Levels 0 and 5 (finest and coarsest) never morph** (`mclib/terrain_update.cpp:96`) —
  maps directly to "innermost ring has no finer neighbor to blend from,
  outermost ring has no coarser neighbor to blend to."
- **`FORCE_LOD` pins `m=0`** (`terrain.cpp:312`) — the debug isolation
  contract (pure per-ring surface, no blend) that any clipmap debug-viz mode
  should keep reachable.

## 4. Oracle reuse

`tests/visual/bookmarks/gaea_peaks_01.json` (skyline-diff oracle, sky_y45/
y135/y225 grazing poses) is renderer-agnostic — it diffs rendered skyline
pixels, not any geomorph-specific data structure. **Keep it as the
acceptance gate for any future terrain-geometry rework, including a clipmap
swap**, per the predecessor doc's explicit recommendation. No new oracle is
proposed by this slice.

## 5. Explicitly out of scope (do not let this doc's existence imply otherwise)

- No clipmap ring buffer, toroidal update, or streaming logic is designed or
  implemented here.
- No new engine gate, shader change, or C++ change ships with this doc.
- No ruling on WHEN a clipmap arc should start — this is a hand-off artifact
  only, written so that whenever it does start, it begins from a named
  contract instead of reverse-engineering `build_max_mips`.

## 7. Bake status audit (this slice, 2026-07-16)

Per the board ask ("check the visual_height_4x bake status for remaining
maps"), audited all 24 campaign maps (`mc2_01`..`mc2_24`) plus the two
generated/dev maps that also carry a bake (`gaea_peaks_01`, `terrain004`).

**Before this slice:** only `mc2_01`, `mc2_17`, `mc2_24` (+ `gaea_peaks_01`,
`terrain004`) had a `visual_height_4x.r32` sidecar anywhere — confirmed by
listing `data/missions/*.beauty/` in the `v0.5.0` and `v0.4` install lanes;
the other 21 campaign maps' `.beauty/` dirs held only `control_map.png`
(a different, unrelated bake product).

**This slice:** ran `tools/terrain_beautify/visual_heightfield.py <mission>
--missions-dir A:/Games/Carver5-feasibility/data/missions --out
tests/terrain/beautify` (default flags — no `--reauth`/`--mountainify`/
`--cliff-drift`/`--shore-*`; the tool's own "deliberately BORING + SAFE"
baseline) for the 21 previously-unbaked campaign maps: `mc2_02`-`mc2_16`,
`mc2_18`-`mc2_23`. All 21 report `corner_err=0 PASS` (exact bilinear
corner-pin, matching the tool's default-path guarantee) and now have
`visual_height_4x.r32` + `visual_height_mips.r32` (mips ship by default,
`--no-mips` not passed) committed under
`tests/terrain/beautify/<mission>.beauty/`. All 24 campaign maps now have a
bake in the repo's dev-fixture location.

**Deliberately NOT done in this slice** (out of scope — bundling it in would
have turned a small doc/content slice into an arc-sized one):
- **No `--reauth`/`--mountainify`/per-biome tuning** for the 21 new bakes —
  that is the board's separate B4-gated "reauth per-biome corner-clamp
  defaults" ruling (agent runs a ladder of candidates, user picks per
  biome); running it unsupervised here would ship 21 unreviewed terrain
  reshapes.
- **No deploy to any install lane's `data/missions/`** — these bakes exist
  only in the repo's `tests/terrain/beautify/` dev-fixture tree, the same
  location `mc2_01`/`mc2_17`/`mc2_24` originally landed in before their
  install-lane deploy. Shipping them to a live install is a separate,
  explicit deploy step (and the `MC2_TERRAIN_VISUAL_DISPLACE` consumer gate
  stays default-OFF regardless, per §5/§0 of this doc's parent recon).
- **No `visual_height_16x.r32`** — that tier only exists for `mc2_17`/`mc2_24`
  (a separate, larger-factor experiment); out of scope for a baseline
  bake-status pass.

## 8. Cross-references

- `.claude/GEOMORPH-NEXT-SLICES.md` — the slice ladder this doc fulfills
  (Slice 3). Slices 1 (ridge-aware bake reduction) and 2 (hysteresis/morph
  temporal fix) are upstream of this bridge and may change the bake-mip
  reduction formula or the morph derivation; if either lands, re-verify
  §1/§3 above still hold (the *shape* of the interface — mip levels 1:1 with
  LOD strides, morph as a per-block scalar with perimeter pinning — is not
  expected to change; the *values* the formulas produce may).
- `docs/renderworld_arc_status.md` — unrelated arc (RenderWorld identity
  substrate); terrain stays outside it per that doc's M3 decision. Not a
  dependency either direction.
- `docs/active_campaigns.md` — this doc's existence is logged there per the
  slice's step-4 ask ("flag to portfolio recon owners").
