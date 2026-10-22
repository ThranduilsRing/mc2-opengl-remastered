# Model footprints

Rebuild offline with `py -3 -B tools/mapgen/footprints.py` after extracting the
corpus. `tools/mapgen/footprints.json` covers every Packet-1 type actually used
by all 320 missions: **2,656 TGL footprints and 7 extentRadius fallbacks**, across
2,663 install/type pairs. All seven fallbacks are DuneBuggyProp (type 421, radius
35 wu). There are no unresolved dimensions. Install keys remain separate because
mods may replace a model behind the same numeric ID.

The resolver follows ObjectType.AppearanceName to `data/tgl/<appearance>.ini`,
then `[TGLData] FileName` or `FileName0`, selecting the intact LOD0 shape. It reads
loose overrides before FST members, with the same dependency chain as phase 1.
Source paths and SHA-256 values are recorded for geometry. An ASE reader is the
second choice if TGL is missing/unsupported; no ASE fallback was needed here.

TGL decoding follows `mclib/tgl_types.cpp`'s disk structs and
`mclib/msl_load.cpp:LoadBinaryCopy`: include stored relative node offsets; exclude
`_PAB`, `LOS_` and `SpotLight` helpers. TGL coordinates are X, vertical Y, depth Z;
the output is local world X,Y with vertical Z, as used by `bdactor.cpp` bounds.
Bounds preserve offsets from the original pivot, not just width/depth. Outlines
are convex hulls of projected visible vertices. They include roof overhangs and
canopies; these are conservative visual envelopes, not MOVE collision templates.
ASE vertices are absolute before the engine recenters them; convert their X sign
and retain ground Y/vertical Z. Dimensions and outlines are rounded to 0.001 wu.

When neither shape is readable, the object's positive ExtentRadius supplies a
square of diameter 2r and an explicitly labeled approximate height of 2r. Zero or
absent radius stays `unresolved`, with zero dimensions; the renderer marks it
magenta rather than claiming a fabricated footprint.

The same table carries each install's authored TerrainTacRGB palette and road
overlay alpha silhouettes from the 64overlays TGAs. Alpha >=128 is the clean
pixel-art edge. These masks preserve visual road width and junction shapes;
they do not establish unit traversal clearance. Source texture hashes are kept.

Installs are only read. No engine or binary mission writer is involved.
