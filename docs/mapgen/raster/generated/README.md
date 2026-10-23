# Generated layouts at Quonset scale

Full maps and crops all use **42.080 wu per source pixel**. The Quonset is
**1x2 pixels** (2x1 when rotated). Each original has an exact **4x
nearest-neighbour preview**. Images have no legend padding; scales, palettes,
markers and preview sizes are recorded in their `.render.json` sidecars.

The owner's **1K editor map means 1024 vertices = 3072 movement cells**.
The earlier 1024-cell example is retained as a separate, smaller size.

| Cells / seed | Sites | Objects | Full original | Full 4x preview | Base original | Base 4x preview | Layout |
|---|---:|---:|---|---|---|---|---|
| 180 / 7 | 3 | 416 | [183x183](generated_180_7.png) | [732x732](generated_180_7_4x.png) | [36x35](generated_180_7_base.png) | [144x140](generated_180_7_base_4x.png) | `generated_180_7.json` (untracked) |
| 360 / 17 | 7 | 1,473 | [366x366](generated_360_17.png) | [1464x1464](generated_360_17_4x.png) | [43x42](generated_360_17_base.png) | [172x168](generated_360_17_base_4x.png) | `generated_360_17.json` (untracked) |
| 720 / 23 | 28 | 5,962 | [731x731](generated_720_23.png) | [2924x2924](generated_720_23_4x.png) | [34x42](generated_720_23_base.png) | [136x168](generated_720_23_base_4x.png) | `generated_720_23.json` (untracked) |
| 1024 / 42 | 41 | 12,112 | [1039x1039](generated_1024_42.png) | [4156x4156](generated_1024_42_4x.png) | [59x97](generated_1024_42_base.png) | [236x388](generated_1024_42_base_4x.png) | `generated_1024_42.json` (untracked) |
| 3072 / 42 (1024 vertices) | 174 | 105,709 | [3115x3115](generated_3072_42.png) | [12460x12460](generated_3072_42_4x.png) | [77x58](generated_3072_42_base.png) | [308x232](generated_3072_42_base_4x.png) | `generated_3072_42.json.gz` (untracked) |

Re-render existing layouts without changing their geometry:

```
py -3 -B tools/mapgen/raster.py --generated
```

Reproduce generation (PNG pairs, JSON and legacy ASCII):

```
py -3 -B tools/mapgen/generate.py --size 180 --seed 7 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/generate.py --size 360 --seed 17 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/generate.py --size 720 --seed 23 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/generate.py --size 1024 --seed 42 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/generate.py --size 3072 --seed 42 --cells-per-char 24 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/validate.py docs/mapgen/raster/generated/generated_3072_42.json.gz
py -3 -B -m unittest discover -s tests/mapgen -v
```

All five layouts pass the corpus plausibility and native geometry validator.
The 3072-cell layout also passes after a compressed-JSON readback. Its heightfield
retains all 3072x3072 samples, with no reduction in geometry fidelity. The 37.5 MB
`.json.gz` is a complete, lossless layout, emitted with a reproducible gzip header
and a streaming JSON encoder. All mapgen readers accept it through `load_json`.

The **42-test** offline suite includes Quonset pixel dimensions, single-pixel
team turrets, equal default scale for maps/crops, exact 4x replication, marker
XYZ coordinates, deterministic gzip round trips, 3072-cell bounds, native overlap/
ring/spacing checks, supplied terrain, the 24-seed sweep and process determinism.
The full 3072-cell generation is an additional integration check. No game was
launched, and source installs remain read-only.

Phase 2 still needs PacketFile/FIT serialization, MOVE/collision templates, a
snap policy with parent revalidation, concrete units and objective conditions.
Pixel snapping only affects display; JSON retains native world-unit placements.

Phase 1c large maps use regional land use, a settlement hierarchy and terrain-aware
transport. The 180/360/720 examples retain their baseline layouts. Diagnosis,
statistics, before/after sheets and 1 wu/pixel regional crop pairs are in
[scale-realism.md](../../scale-realism.md).

The full layout files (`generated_*.json`, `generated_3072_42.json.gz`, 87 MB in all) are not tracked: regenerate them
with `tools/mapgen/generate.py` (deterministic per size and seed). The PNG, ASCII and `.render.json` sidecars stay tracked.
