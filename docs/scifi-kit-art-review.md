<!-- doc-status: current | reviewed:2026-10-22 -->
# Sci-fi kit: art review of round-2 primitives, and the rework plan

Reviewed: Sol's round-2 sweep (`G:/mc2-assets/out/scifi-kit/round2/primitives-01..06.png`, parts, stock crops) against
the round-1 building drafts the owner liked, the stock MC2 textures, and the MW5/MWO artist references
(primary: Alina Godfrey's BattleTech Urban Warfare sheets and process GIFs, Viona Halim's MW5 sets).
The references are for study only. They are never copied or shipped.

## What is wrong

**1. Disjointed: no shared language.** Each primitive invents its own proportions and edges.
- `wall_panel` has a slab cap and a window shelf. `sloped_wall` is a bare trapezoid with no plinth or cap.
  `flat_roof` is a thin tray with black corner nubs. Nothing lines up at a common plinth, sill or coping height.
- Edge treatment varies at random: crisp boxes (`sloped_wall`, `tower_shaft`), 2-segment rounded bevels (`vent`, `floodlight`), none at all (`railing`, `roof_truss`).
  The references use one move everywhere: big 45-degree chamfers on the primary masses and small chamfers on trims.
- There are no connector details. Doors (`bay_door`, `infantry_door`, `gate`) are free-standing frames with no wall reveal.
  `mech_bay_door` is a bare picture frame. The `buttress` is a stick with no footing into the plinth. `window_slit` is a shelf, not an opening.
- The scale reads differently part to part. `turret` barrels are toy-sized, `pipe` and `lamp_post` are hairlines,
  and `roof_truss`, `railing`, `gantry` and `catwalk` vanish entirely at sheet size (and they would at RTS zoom).

**2. Destroyed states are not physical.** The algorithm deletes random components and squashes Z by about half.
- `tank`/`silo` become stacked discs with a lid floating in the air. `dish_antenna` floats its feed horn above a disc.
  `barrel_roof` is arc fragments in mid-air. `mech_bay_door` and `window_band` have pieces hanging unsupported.
- No damage origin, no bent metal, no exposed frame or rebar, no soot. There is no debris, and certainly none that matches the part's material or rests on the ground.
- Several "destroyed" parts just look smaller (`tower_shaft`, `buttress`, `floodlight`) or look identical (`gantry`, `ladder`).

**3. Colour and material identity are lost.** Everything is the same grey-beige paint.
- The stock quonset is rust-brown corrugated steel with olive window frames. Its `window_slit` came out grey concrete.
- The stock barracks is khaki ferrocrete with sage-green inset panels, and `sloped_wall`/`flat_roof`/`skylight` lost both.
- The stock hangar is pale beige panelling. `factorymd` is silver vertical corrugated steel. Nothing carries through.
- Material classes exist in the code, but every class renders as paint. There is no visible bare metal, corrugation, ferrocrete grain or glass.

**4. Worn and abandoned are nearly identical.** Both are a few orange specks. Abandoned shows no fade, peel, grime streaks, broken glass, missing panels, moss or debris.

## The reference language we adopt

| Reference | What we take | Primitives modelled after it |
|---|---|---|
| Godfrey props-2/-4/-7 (A1-A11 antennas) | Chamfered slab masts, splayed stepped footings, clip-on boxes on short brackets, a single orange accent band, red tip lights | `antenna`/`mast`, dish mount, turret base footing |
| Godfrey props-4 A10 (hex panels) | Faceted hexagonal panel dishes on a bracket arm | `dish_antenna` |
| Godfrey props-5 (C6 unit) | Chamfered box plus drum, recessed louvre panel, orange base band, dark gunmetal | `vent`/HVAC roof plant, turret housing |
| Godfrey props-3/-6 (F1-F5 posts) | L-bend chamfered posts, boot base with slots | `floodlight`, gantry legs, sign posts |
| Godfrey props-1/-222, screenshots | Gravel flat roofs with chamfered coping, clustered HVAC, light roofs over darker walls, hazard chevrons at ground | `flat_roof`, roof plant layout, RTS readability check |
| Godfrey buildingconstruction GIF | Modular stacking: podium, body and crown on one grid, chamfered plan corners, parapet offset | Module grid and connectors, assemblies |
| Godfrey buildingmaterials / substance GIFs | Same geometry, swapped material families; mask-driven wear: Ferrocrete colour, FerrocreteRust, Dirt, Grunge, CrackIntensity, Glass_Cracks, PaintColor, Paint_On, Edge_Wear | Palette families and condition parameters (same names) |
| Halim spacetower01-05 | Facade modules with thin accent lines and hazard bands. **03-destroyedpartial:** damage stays local, panels buckle along their own seams, cladding tears to show the dark frame and floor slabs, soot rises from the breach, rubble collects at the base | Destroyed operator for all parts |
| Halim drilltower01-04 | Stacked dark-steel modules with yellow inserts, hazard edges, beacons | `tank`/`silo`, `gantry`, towers |
| Halim setdressing a/b/c | Abandoned = faded, patched paint, streaks, moss and weeds on roofs and bases, black broken windows; clutter = barriers, crates, consoles, pipes | Abandoned state, small props and clutter |

## Rework plan (what was built)

See `tools/scifi_kit/art/`: `lang.json` (the kit language), `geo.py` (pure-Python primitives on that language),
`blend.py` (materials, conditions, destroyed operator, renders, glTF), `run.py` (CLI and sheets).
Outputs go to `G:/mc2-assets/out/scifi-kit/art/`.
