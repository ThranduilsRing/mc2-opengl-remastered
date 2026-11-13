# MC2 map layout generator: pixel-art phases 1b/1c and playable phase 2

**Historical results:** generated artifacts and the offline test suite were removed in `7bd96f0a9`.
The commands and measurements below describe that earlier tree; recover files with `git show 7bd96f0a9^:<repo-path>`.
**Phase 2:** recover the playable PAK/FIT writer results and viewed comparisons with `git show 7bd96f0a9^:docs/mapgen/phase2/README.md`.
The 180- and 720-cell flat demos load through the ordinary stock PacketFile path.
Road texture baking and destruction-aware native collision templates remain pending.

Offline Python 3 + numpy + Pillow tools. Source installs are read-only. No engine, game,
deployment or mission binary writes are involved. Run commands from the repo root.

```
py -3 -B tools/mapgen/extract.py --all
py -3 -B tools/mapgen/footprints.py
py -3 -B tools/mapgen/raster.py --all
py -3 -B tools/mapgen/ascii.py --examples
py -3 -B tools/mapgen/analyze.py
py -3 -B tools/mapgen/generate.py --size 300 --seed 42
py -3 -B tools/mapgen/validate.py tools/mapgen/corpus/generated/generated_300_42.json
py -3 -B tools/mapgen/ascii.py tools/mapgen/corpus/stock/mc2_03.json --cells-per-char 1 --crop 36 36 90 90
py -3 -B -m unittest discover -s tests/mapgen -v
```

Extraction defaults to `tools/mapgen/corpus/<install>/<mission>.json`; these large
local files are ignored by git. `index.json` records every source, SHA-256 digest,
missing FIT companion, error and unresolved field. It is checkpointed after each
install. Custom installs: `--install PATH --label NAME`; `--mission STEM` limits a
run. Expansion-only Carver5 uses Omnitech, then stock, for absent dependencies.
Only missions present in the selected install are enumerated, not its fallback.
Stock includes all paired missions in the install, including the 24 campaign maps.
Loose files take precedence over FST members. Inactive launcher `mods/` overlays
and backup directories are not mounted. Explicit install catalogs are recorded;
this is a survey of these installations, not a claim of pristine original media.

The owner means **top-down pixel art**, like a more detailed tacmap. Raster PNGs
are the primary output; the phase-1 ASCII tools and examples remain available.
Recover the historical raster examples with `git show 7bd96f0a9^:docs/mapgen/raster/README.md`; see [model footprint methods/coverage](footprints.md).
All 320 stock/mod corpus missions have full-map PNGs. Every full map and crop
defaults to **42.080 wu/px**, measured from stock `Quonset` type 335's 84.160-wu
long side / 2: the hut is two pixels by one. Every original `.png` has a
nearest-neighbour `_4x.png` preview. Override scale with `--wu-per-pixel N`.
Buildings use whole-pixel rectangles and snapped display rotations; walls are
one-pixel lines, gates and turrets distinct pixels, trees pixels/small clusters,
and roads one or two pixels wide. Terrain uses eight flat colours, with optional
subtle `--hillshade`. Native geometry and original rotations remain in JSON.

## JSON schema `mc2-mapgen-1`

`size.rows/cols` are movement/terrain cells (128/3 world units). North is the top
row, east the right column. All `cell` values are `[row, col]`. Horizontal world
coordinates are **X,Y**, Z is elevation. Object records preserve world coordinates,
rotation in degrees, type name/ID, engine class, editor special type, team,
damage, packed parent and resolved parent object ID. Parent ambiguity is retained
as a warning. `parts` contains FIT unit placements; `player_start` is the centroid
of active existing player parts. Objectives retain titles, marker visibility and
conditions; hidden or unavailable positions are not invented.

`terrain.cell_step` gives the sampling stride: authored Packet 0 has one vertex
per three cells. Height, slope, terrain type and overlay grids use that stride.
`layers` are cell-resolution masks encoded as sorted `[row, first_col, end_col]`
spans; the end is exclusive. They contain roads, cement, water, cliffs and runway.
Authored vertex samples cover the corresponding 3x3 cells; this is a coarse
footprint, not an interpolation or model collision footprint. Slopes are finite
differences at 128 wu spacing; cliffs start at 35 degrees. Wet means `(water & 1)`.
Cement means terrain type 10 or 13..20. Roads come from the textureData overlay
layer, decoded with the install's textures.fit; rough overlays are excluded.
Raw terrain types and overlay handles remain available for future refinements.

## ASCII previews

Committed examples live in `docs/mapgen/examples/<install>/`. Six stock campaign
missions cover an airfield, compounds/outposts, towns and late campaign bases;
mod previews include both inherited and custom maps where available. The renderer
accepts any JSON in the shared schema, including generated layouts. Set
`--cells-per-char N` to zoom out, `--crop R0 C0 R1 C1` to inspect a base, and
`--out PATH` to save. The legend and objective coordinates are included in every
file. Coarse characters show the highest-priority feature; use a finer scale to
inspect chains, gates and small buildings. Teams are retained in JSON and listed
in the preview header; ASCII class symbols do not encode ownership.

The object catalog is indexed by Packet-1 objTypeNum, and its ObjectClass resolves
engine class. Buildings.csv supplies editor classes (wall, control, generator,
etc.). Its vegetation tags override engine classes because some mods implement
trees as buildings. Decorative props/fences/rocks remain separate from buildings.
Named loose FIT files extend the Omnitech catalog beyond object2.pak, with CSV
class fallback when a named FIT is absent. Tiered wall/post names identify walls
even when a mod's CSV omits the WALL tag; original engine class is preserved.

## Measured corpus

`docs/mapgen/corpus.md` documents clustering, archetype rules, measured ranges,
road connectivity, local orientation, terrain siting and team separation.
`docs/mapgen/extraction_manifest.json` preserves source paths and hashes;
`tools/mapgen/profile.json` is the generator's measured input. Regenerate the
profile after a new extraction. Its index hash uses canonical JSON, so Git
newline conversion does not invalidate provenance. Comparisons exclude inherited stock stems from
custom mod sets; inherited campaign variants are explicitly labeled. Pooled
statistics deduplicate decoded terrain/object packets across installs. Archetype
labels and slope/proximity measurements are coarse, documented approximations.

## Generating layouts

`generate.py --size N --seed S --out DIRECTORY` writes JSON, full-map and base
PNGs at Quonset scale, their 4x previews, render sidecars, and a legacy ASCII
preview. Sizes **96..3072 movement cells** are supported: the owner's **1K editor
map is 1024 vertices = 3072 cells**, not 1024 cells. Divisible-by-three sizes
record their vertex side in `size.vertices`. Layouts above 1024 cells use
deterministic `.json.gz` with streaming output; readers accept either format.
Default `--style stock` calibrates from the 24 stock
missions; `--style mixed` uses the deduplicated stock/mod pool, or supply an install
label from the corpus. Profile hashes and terrain hashes are recorded in output.
Same seed, profile, footprint table, style and terrain produce the same JSON bytes
across processes.

The generator lays a connected road skeleton between reachable map edges, reserves
flat parcels along it, adds access spurs, buildings, walls/gates, linked turret
batteries, then tree clusters. Compounds have cement inset two cells from the wall
ring; each road entry replaces wall segments with a controlled native gate.
Open sites have no cement ring. Player starts are on the network; enemy sites
stay at least 0.25 map sides away.

First sites demonstrate a compound, an open outpost and a roadside town; later
sites sample measured archetype weights. Parcel dimensions sample p10..p90 with
practical minima and a 42-cell cap; structures sample measured mixes/counts.
Battery counts are conservatively limited to 3..5, within stock observations.
Site and tree density use measured medians, capped at 512 sites and 180,000 trees
to cover the 3072-cell domain. These choices give inspectable examples
without attempting to reproduce an entire campaign's distributions per map.
Small maps have at least three requested sites, so mission-wide density comparisons
only apply at stock map sizes and above. Difficult terrain may fit fewer sites;
failure to fit a compound plus another site produces an error, not invalid output.

`--heightfield FILE` accepts a JSON elevation matrix, JSON
`{"elevation_wu": [[...]], "water": [[false,...]]}`, numpy `.npy` (no pickle), or
an extracted layout JSON. Heights are world units, and the expanded grid must
match `--size`. Extracted coarse heights are expanded by nearest sample; steps
can therefore create steep boundaries. `--water-level WU` optionally marks low
cells wet. Supplied heights are preserved (rounded to 0.001 wu), not flattened or
bridged. Roads and placements use dry terrain below 10 degrees slope. If the land
cannot reach two map edges or fit the required sites, generation reports why.
Synthetic terrain supplies gentle hills, a lake and a steep escarpment by default.

## Plausibility and verification

The generator CLI validates before writing. `validate.py FILE` also checks an
existing generated JSON and exits nonzero on errors. Checks cover complete wall
rings, controlled gates at road crossings, two-hop turret chains with matching
packed cells and teams, measured parent-link distances and battery sizes, unique
object cells, dry flat parcels/roads/placements, connected roads reaching map
edges, tree clearances, player/enemy separation, and measured size/count/density
envelopes. The heightfield is used to recalculate slopes rather than trusting a
stored classification. Corpus envelopes are broad min/max plausibility limits,
not a claim of statistical equivalence; the small-map demonstration floor is
excluded from mission-wide density checks.

Run `py -3 -B -m unittest discover -s tests/mapgen -v`. The offline suite covers
binary packet/FST fixtures, mod catalog differences, source provenance, a 24-seed
sweep, 1024x1024 generation, supplied water/cliff obstacles, impossible terrain,
process/hash-seed determinism, population summaries, and deliberate gate/road/
parent/water/slope corruptions. These checks need no installs or engine build;
they use synthetic inputs and the committed measured profile.

Generated ASCII examples were removed in `7bd96f0a9`; recover one with
`git show 7bd96f0a9^:docs/mapgen/examples/generated/generated_180_7.txt` (substitute the filename below).
The 1024-cell ASCII example uses eight cells per character; its large JSON is
regenerable with the command below.

| Example | Sites | Artifacts |
|---|---:|---|
| 180 cells, seed 7 | 3 | `generated_180_7.txt` (git history), `generated_180_7.json` (untracked) |
| 300 cells, seed 42 | 5 | `generated_300_42.txt` (git history), `generated_300_42.json` (untracked) |
| 360 cells, seed 17 | 7 | `generated_360_17.txt` (git history), `generated_360_17.json` (untracked) |
| 1024 cells, seed 42 | 57 | `generated_1024_42.txt` (git history) |

All four pass the plausibility checker. Reproduce the large layout locally:

```
py -3 -B tools/mapgen/generate.py --size 1024 --seed 42 --cells-per-char 8
```

Phase 1b adds concrete install-specific type IDs, rotations and `position_wu`
(world X,Y,Z at the original model pivot). `cell` and packed parent cells are
recomputed from these final positions. `footprint_key` references the measured
table; embedded `footprint` records preserve unscaled bounds and outlines.
`roads_wu` contains width-bearing world X,Y polylines; `wall_edges_wu` describes
four connected edges per compound and their wall/gate IDs. `player_start_wu`,
part positions and objective suggestions support a later binary writer.

The phase-1 cell planner supplies terrain, reserved road corridors, parcels and
requested class/role mixes. A continuous placement pass replaces all abstract
objects. It samples measured native models and accepts placements using whole
rotated rectangles, four-wu clearance, dry/flat footprint coverage, road
reservations and measured neighbour/link ranges converted to world units.
Buildings stay parallel/perpendicular to road frontage. Parent-first placement
samples turret/control/generator distances from the stock corpus p10..p90,
constrained by the parcel; final distances must remain inside observed min/max.
Roles with >=20 same-role adjacency samples also enforce observed neighbour
min/max; sparse roles do not claim a robust spacing rule.

Walls use the minimum number of unscaled native segments needed to cover each
edge. End caps overlap by at most half the shorter segment; ring corners may
join. Gates occupy their actual native width where access roads cross. Buildings
and canopies have no overlap allowance. Tree clusters become continuous jittered
point sets rejected against native canopy bounds. Coarse road/cement masks remain
planning reservations; PNGs draw generated roads from their world polylines.
Main routes use 64 wu where the terrain admits their full width, narrowing to
40 wu beside obstacles; access spurs use 40 wu. These are design widths, while
authored-map road silhouettes come directly from the install's overlay masks.

The validator retains phase-1 terrain/network/team/envelope checks and adds
native-ID/dimension checks, pivot/cell agreement, convex separating-axis overlap
checks, physical wall interval coverage, world-unit parent distances, role
spacing and swept road width checks. It distinguishes permitted wall joins from
solid footprint overlaps. Conservative model rectangles include overhangs;
neither these nor visual road widths prove engine MOVE passability.

Historical raster examples: recover the index via `git show 7bd96f0a9^:docs/mapgen/raster/generated/README.md`: 180/7, 360/17,
720/23, 1024/42 and **3072/42 (1024 vertices)**, each with JSON, a full map and a
base crop at Quonset scale, plus exact 4x viewing versions. The old cell examples
above are historical phase-1 artifacts. Playable mission logic remains unassigned;
objective markers are suggestions with no FIT conditions.

## Large-map realism (phase 1c)

Phase 1c changes generation at **1024 movement cells and above**. A terrain-
conditioned macro layer assigns wilderness, farmland, industrial, military and
urban regions before placement. Separated major centres and clustered satellites
use a truncated rank-size population law, with large developed sites and many
lone outposts. Terrain-aware trunk MST routes, redundant major links and minor
branches replace the fixed six-route skeleton. Forest extent, radius and density
vary regionally. The 180/360/720 layouts retain exact baseline generator hashes.
See [scale diagnosis and measurements](scale-realism.md), including before/after
sheets and three regional detail pairs per large example at **1 wu/pixel**.

Rebuild the comparison evidence after generating both large examples:

```
py -3 -B -m tools.mapgen.scale_review
py -3 -B -m tools.mapgen.scale_metrics
```

The first command uses committed baseline `6c0b66726` from local git and the new
generated JSONs. The second recomputes group medians over the extracted corpus.
Neither command launches the game or writes to installs.

## Boundaries for phase 2

Concrete models, native bounds, pivots and parent cells are assigned for layout
planning. Phase 2 still needs collision-template/passability validation, deliberate
wall tier/corner selection and any engine-required snap policy, then packed-parent
revalidation after snapping. Buildings belong in Packet 1; units and objectives
belong in FIT. A complete writer must handle Packet 0 terrain, Packet 3 tacmap,
Packet 4 MOVE and matching FIT extents, or use an explicitly verified same-grid
template. Do not extend the divergent direct terrain initialization path. No
PacketFile writer or engine integration is part of this phase.
