# Generated footprint layouts

Each example includes an auto-scaled full map, a 1 wu/px crop of its first
compound, JSON with concrete stock model IDs and world-unit pivots, and a legacy
ASCII preview. Image dimensions below exclude the 68-pixel legend header.

| Cells / seed | Sites | Objects | Full map | 1 wu/px base crop | Layout JSON |
|---|---:|---:|---|---|---|
| 180 / 7 | 3 | 416 | [1920x1920, 4 wu/px](generated_180_7.png) | [1494x1451](generated_180_7_base.png) | [JSON](generated_180_7.json) |
| 360 / 17 | 7 | 1,473 | [1920x1920, 8 wu/px](generated_360_17.png) | [1792x1750](generated_360_17_base.png) | [JSON](generated_360_17.json) |
| 720 / 23 | 28 | 5,962 | [3840x3840, 8 wu/px](generated_720_23.png) | [1408x1750](generated_720_23_base.png) | [JSON](generated_720_23.json) |
| 1024 / 42 | 57 | 12,144 | [5462x5462, 8 wu/px](generated_1024_42.png) | [1707x1366](generated_1024_42_base.png) | [JSON](generated_1024_42.json) |

Reproduce from the repo root (repeat for each size/seed pair):

```
py -3 -B tools/mapgen/generate.py --size 180 --seed 7 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/generate.py --size 360 --seed 17 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/generate.py --size 720 --seed 23 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/generate.py --size 1024 --seed 42 --out docs/mapgen/raster/generated
py -3 -B -m unittest discover -s tests/mapgen -v
```

All four JSONs pass `validate.py` using the committed corpus profile. The 33-test
offline suite covers native geometry, corrupted overlaps/rings/spacing, model
resize rejection, raster limits, the phase-1 24-seed sweep, supplied terrain,
1024-cell generation and cross-process determinism. Additional one-seed checks
passed for stock, all six mod styles and mixed mode. No engine was launched.

Phase 2 needs PacketFile/FIT serialization, MOVE/collision templates, an explicit
snap policy with parent revalidation, concrete units and playable objective
conditions. These images describe layouts, not tested playable missions.
