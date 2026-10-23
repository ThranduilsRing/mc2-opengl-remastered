# Suburban forest preset

`suburban_forest` composes temperate field parcels, hedgerows, forest cores and
thinned edges, clearings, river banks, and road-facing villages. The landscape
recipe follows the owner's references and WARNO authoring study; all models,
terrain formats and movement rules remain MC2's own.

Generate three ordinary stock-format missions and feature masks:

```powershell
py -3 -m tools.mapgen.playable_demo --biome suburban_forest --size 360 --seed 7 --seed 17 --seed 23 --out E:/mc2-proof/suburban-forest
py -3 -B -m unittest discover -s tests/mapgen -q
bash scripts/check-contracts.sh -j2
```

The generic generator also accepts `--biome suburban_forest`. Supported preset
sizes are 180..720 cells, divisible by 60, using stock native footprints.
The preset owns its low-relief terrain; external heightfields are rejected.

The existing `mc2-mapgen-1` layout includes additive `regions` metadata:
parcel IDs/bounds, cultivation direction/tint, the river centreline and road
recipe. Shared span layers describe shoulders, verges, ditches, forest
cores/edges, clearings, parcel borders, banks, crossings and exclusion zones.
Full rotated tree footprints must avoid the shared exclusion mask, including
settlements, roads, water/banks and objective/player zones. Native building
footprints and parent links retain their existing validators.

Emission writes the usual PAK/FIT/ABL and a 1024-square **unlit** terrain TGA.
Copy those three mission files to the lane install's `data/missions/` and the
TGA from `missions/textures/` to `data/textures/`. The engine's ordinary terrain
loader owns lighting/shadow burnin. Parcel fills, cultivation rows, borders and
road corridors appear on the playable terrain, not only the preview/tacmap.
PAK vertices contain the same material, height and water fields. Water blocks
movement; roads reserve dry causeways. MOVE retains stock 10/15 link costs and
diagonal eligibility. All unit spawns and every planned objective's attack
approach must be reachable from the player before serialization.

For background Vulkan proof use `scripts/run_visual_capture.py` with the lane
install, generated bookmark JSON, `--duration 30 --no-kill`, and declared
`MC2_SCREENSHOT_SOURCE=backbuffer` / `MC2_FORCE_ALL_VISIBLE=1`. Keep deterministic
RNG enabled. Generated `.features.png` files show parcels and semantic layers;
`.plan.png` also depicts native object silhouettes.

Limits: crossings use dry causeways, with bridge geometry reserved for the
native baker. Ground and hover currently share a conservative water-blocked
graph. Existing rectangle collision remains blocked after destruction, and
gates use initially passable apertures. This adds no editor UI or campaign,
victory/save/reload proof.
