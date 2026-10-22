# Pixel-art tacmaps at Quonset scale

The default is **42.080 world units per source pixel**, for both full maps and
crops. Stock type 335, `Quonset`, measures **42.503 x 84.160 wu** in the native
footprint table: its long side / 2 sets the scale, making the hut **1x2 pixels**
(or 2x1 after a quarter turn). The measured value takes precedence over the
owner's approximate 25..35-wu estimate. A terrain cell is 128/3 = 42.6667 wu.

Every `.png` is the original **1x** raster. Its sibling `_4x.png` is enlarged
exactly four times with nearest-neighbour sampling. Images contain only map
pixels, with north up; legend and scale information live in `.render.json`
sidecars. No smooth resampling, roofs, large shadows or antialiasing are used.

All 320 phase-1 corpus missions are under their install label. Start with
[mc2_03 at 4x](stock/mc2_03_4x.png), [its base crop](stock/mc2_03_base_4x.png),
[mc2_17](stock/mc2_17_4x.png), [cityassault](MC2X-CVE-G/cityassault_4x.png),
or [generated examples, including the 1024-vertex map](generated/README.md).
The [index](index.json) records every original/preview pair and their hashes.

Reproduce all existing images:

```
py -3 -B tools/mapgen/raster.py --all
py -3 -B tools/mapgen/raster.py --generated
```

`--examples` limits corpus output to the phase-1 ASCII preview selection.
Supply a mission JSON or `.json.gz` to render one map. `--crop R0 C0 R1 C1`
takes cell bounds; `--base-crop 0` selects a structural cluster. Override the
world scale with `--wu-per-pixel N`. `--hillshade` enables subtle three-level
height shading; flat colours are the default. Both original and preview are
written for every render, including overrides. Preview dimensions must stay
within 16,384 pixels per side and 200 million pixels in total; use a coarser
override or a crop if necessary. The largest default map (3072 cells) needs
3115x3115 source pixels and a 12460x12460 preview.

| Feature | Source-pixel style |
|---|---|
| Buildings | Whole-pixel rectangles, nearest 0/90-degree orientation; military grey, industry tan, town cream, HQ violet, airfield blue-grey |
| Walls / fences | One-pixel Bresenham lines, including diagonal authored chains |
| Gates | One gold pixel at the gate centre |
| Turrets | One pixel in the owning team's colour |
| Trees | One dark green pixel; unusually wide canopies use a three-pixel cluster |
| Roads | One or two pixels wide; authored alpha masks retain bends/junctions |
| Terrain | Eight flat colours: grass, scrub, dirt, sand, rock, snow, cement, water |
| Teams | Player cyan/blue; enemies red, orange or violet; buildings receive a mild team tint |
| Markers | Cyan player chevron, small gold objective diamond; unresolved footprints magenta |

Pixel snapping is a display convention. JSON retains native unscaled footprints,
continuous world pivots, original rotations and type IDs. The symbolic gate
pixel does not depict the physical gate opening width. Coarse terrain samples
and visual envelopes do not establish engine MOVE passability.
