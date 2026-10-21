# MC2 map layout generator: ASCII phase

Offline Python 3 + numpy tools. Source installs are read-only. No engine, game,
deployment or mission binary writes are involved. Run commands from the repo root.

```
py -3 -B tools/mapgen/extract.py --all
py -3 -B tools/mapgen/ascii.py --examples
py -3 -B tools/mapgen/analyze.py
py -3 -B tools/mapgen/generate.py --size 300 --seed 42
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
profile after a new extraction. Comparisons exclude inherited stock stems from
custom mod sets; inherited campaign variants are explicitly labeled. Pooled
statistics deduplicate decoded terrain/object packets across installs. Archetype
labels and slope/proximity measurements are coarse, documented approximations.

## Generating layouts

`generate.py --size N --seed S --out DIRECTORY` writes JSON and ASCII. Sizes
96..1024 cells are supported. Default `--style stock` calibrates from the 24 stock
missions; `--style mixed` uses the deduplicated stock/mod pool, or supply an install
label from the corpus. Profile hashes and terrain hashes are recorded in output.
Same seed, profile, style and terrain produce the same JSON bytes across processes.

The generator lays a connected road skeleton between reachable map edges, reserves
flat parcels along it, adds access spurs, buildings, walls/gates, linked turret
batteries, then tree clusters. Compounds have cement inset two cells from the wall
ring; each road entry replaces a wall segment with a controlled gate. Abstract
corner/straight wall shapes are explicit. Open sites have no cement ring. Player
starts are on the network; enemy sites stay at least 0.25 map sides away.

First sites demonstrate a compound, an open outpost and a roadside town; later
sites sample measured archetype weights. Parcel dimensions sample p10..p90 with
practical minima and a 42-cell cap; structures sample measured mixes/counts.
Battery counts are conservatively limited to 3..5, within stock observations.
Tree density uses the measured median. These choices give inspectable examples
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

Outputs are cell-level plans: model IDs, exact footprints and playable mission
logic remain unassigned. Objective markers are suggestions, with no FIT conditions.

## Boundaries for phase 2

Choose concrete models from each target install's object2.pak/Buildings.csv and
measure their footprint, pivot, wall tier and corner/diagonal variants. Validate
parent cells after snapping. Buildings belong in Packet 1; units and objectives
belong in FIT. A complete writer must handle Packet 0 terrain, Packet 3 tacmap,
Packet 4 MOVE and matching FIT extents, or use an explicitly verified same-grid
template. Do not extend the divergent direct terrain initialization path. No
PacketFile writer or engine integration is part of this phase.
