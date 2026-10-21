# MC2 map layout generator: ASCII phase

Offline Python 3 + numpy tools. Source installs are read-only. No engine, game,
deployment or mission binary writes are involved. Run commands from the repo root.

```
py -3 -B tools/mapgen/extract.py --all
py -3 -B tools/mapgen/ascii.py --examples
py -3 -B tools/mapgen/analyze.py
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

## Boundaries for phase 2

Choose concrete models from each target install's object2.pak/Buildings.csv and
measure their footprint, pivot, wall tier and corner/diagonal variants. Validate
parent cells after snapping. Buildings belong in Packet 1; units and objectives
belong in FIT. A complete writer must handle Packet 0 terrain, Packet 3 tacmap,
Packet 4 MOVE and matching FIT extents, or use an explicitly verified same-grid
template. Do not extend the divergent direct terrain initialization path. No
PacketFile writer or engine integration is part of this phase.
