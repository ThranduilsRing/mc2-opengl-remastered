# Asset modernization workbench

Offline tools only; Python 3, NumPy and Pillow. OBJ import also uses installed
trimesh; pytest runs the tests. No game launch, downloads or install writes.

`py -3 tools/assets/catalog.py --sheets` rebuilds [catalog.json](catalog.json),
[catalog.md](catalog.md) and [contact sheets](sheets/index.md). Source paths and
per-corpus errors stay in the JSON. Stock FSTs are read from
`A:/Games/mc2-opengl/mc2-win64-v0.4`; pristine `mc2srcdata-fresh` supplies object2
and Buildings.csv because the dev install's loose versions include mods.

The mapgen `Install`, FIT parser, PacketFile decoder and type catalog are reused
from `claude/mapgen-ascii` commit `e322675cec833f1926caed91e08ecdc830e68f58`
in `tools/assets/mapgen_data.py`. Local changes: import location, an archive-only
switch and loose TGL/texture indexing. The 40-byte mission object layout comes
from that branch's `extract.py`. Carver5 falls back through Omnitech to stock.

Usage counts represent installed missions, including potential scripted parts;
they are not simultaneous visible counts. Re-exported mod models sharing a
stock selection key count toward that key, with their changed hashes reported.
Unmatched custom keys do not count. Each corpus is counted separately even if
it contains missions also shipped elsewhere. The priority score is a projected
size and quality heuristic; vegetation and mechs have separate lists.

TGL data uses the serialized Win32 layout in `mclib/msl_load.cpp` and
`mclib/tgl_types.cpp`. The renderer assembles node-local vertices using absolute
node centers, preserves per-corner UVs, and uses TG +Y up. It intentionally does
not copy the viewer's historical Z-up mapping. Helpers/LOS/spotlights are omitted.
Previews approximate materials and lighting; they do not certify runtime pixels.

Run checks: `py -3 -B -m pytest tests/assets -q -p no:cacheprovider`.
