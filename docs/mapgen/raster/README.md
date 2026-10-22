# Pixel-art tacmaps

Every one of the 320 phase-1 corpus missions is rendered here, under its install
label. Start with [mc2_03](stock/mc2_03.png), [its base at 1 wu/px](stock/mc2_03_base.png),
[mc2_17](stock/mc2_17.png), [its base](stock/mc2_17_base.png),
[cityassault](MC2X-CVE-G/cityassault.png), or [generated examples](generated/).

Reproduce with `py -3 -B tools/mapgen/raster.py --all`. `--examples` limits this to
the original ASCII preview selection. Supply a mission JSON to render one map;
`--crop R0 C0 R1 C1` takes cell bounds, `--base-crop 0` selects a structural
cluster, and `--wu-per-pixel 1` sets scale. Crops default to 1 wu/px; whole maps
use 2, 4 or 8 wu/px. `--no-hillshade` removes the heightfield shading.

The style uses install TerrainTacRGB colours, nearest-neighbour terrain pixels,
road overlay alpha silhouettes, conservative convex model outlines, roof edges,
small directional shadows, and canopy blobs. Class colours are in the image
legend; team 0 adds blue, hostile ownership adds red. Cyan arrows indicate player
starts; gold diamonds indicate visible objectives. Unknown footprints are magenta
crosses (none of the current corpus types are unresolved). No game screenshot or
AI image generation is involved.

Each PNG has a `.render.json` sidecar containing its crop, world scale, dimensions,
header offset and geometry source. The top 68 pixels are legend, outside the map.
The lower-left bar spans 256 wu. No full-map image exceeds 16,384 pixels; an
additional 100-million-pixel guard prevents pathological allocations.

Road masks preserve authored visual widths; coarse terrain and water boundaries
remain the phase-1 vertex samples. Foliage is drawn from canopy extents. Geometry
envelopes do not prove collision/passability or represent exact wall openings.
