# Building gameplay and replacement art

The original appearance remains gameplay authority. `BldgAppearanceType::init`
loads its regular LOD and damage shapes before considering render overrides
(`mclib/bdactor_type.cpp`). This includes mod-owned shapes: the bundled kit
declines an appearance or mesh provided by another mod
(`bundledOverrideShadowed`). Explicit mod override records keep their existing
render-only contract. Classic does not mount the bundled kit.

The loader already retains original type geometry for simulation poses and
fallback rendering. No additional full mesh or permanent per-placement vertex
array is needed. Bounds and terrain marking use temporary `CreateFrom`
hierarchies sharing that geometry; terrain marking leaves the remake instance
intact. Instances without render overrides follow their existing paths.

## Geometry consumers

| Consumer | Source and consequence |
| --- | --- |
| `BldgAppearance::calcCellsCovered` (`mclib/bdactor_terrain.cpp`) | Original LOD0 or damage shape; excludes `_PAB`, tests local Z >= 1 WU (forest clumps reverse the test), rotates and converts individual vertices to cells. Pathing footprint and LOS target corners. |
| `TG_MultiShape::GetShapeVertexInEditor` (`mclib/msl_shape.cpp:137`) | Original vertex plus relative/root centres, TGL-to-world axes, placement rotation. This is the placement-survey rule, without polygon filling. |
| `BldgAppearance::markTerrain` (`mclib/bdactor_terrain.cpp`) | Editor MOVE/LOS map generation. Original intact cells get special gate/wall/bridge IDs; original rubble is permanently blocked. Forest and resource special cases retain their existing rules. |
| `BldgAppearance::markMoveMap`, `markLOS` (`mclib/bdactor_terrain.cpp`) | Runtime passability and cell LOS height changes; existing dual-shape paths use original shapes under overrides. |
| `TerrainObject::calcCellFootprint` (`code/terrobj.cpp:1386`) | Consumes coverage into cell/world bounds used by `getLineOfSightNodes`. |
| `TerrainObject::calcSubAreas`, `openSubAreas`, `closeSubAreas` (`code/terrobj.cpp`) | Cached mission cells and MOVE areas drive opening gates and destruction; art does not provide these cells. |
| `BldgAppearance::init`, `getRadius`, `getTopZ` (`mclib/bdactor.cpp`, `mclib/bdactor.h`) | Original bounds/radius/height under overrides. Includes target/hotspot heights and pick bounds. Previously initialization derived these from the remake. |
| `BldgAppearance::updateSimPose` (`mclib/bdactor.cpp`) | Already original intact/damage hierarchy; authoritative weapon/hit nodes and animation are independent of art. Presentation-only node queries still use the visible mesh. |
| `Building::update` (`code/bldng.cpp:723`) | Negative INI collision extent falls back to original LOD0 bounds. `GameObject::getExtentRadius` and `code/collsn.cpp` use the type extent, not remake triangles. |
| Building explosion (`code/bldng.cpp:211`, `1285`, `1514`) | Radius/damage read from INI; explosion position uses authoritative hit node. No mesh-derived splash radius. |
| Terrain building picker (`code/objmgr_pick.cpp:669`) | Projects the original `typeUpperLeft/typeLowerRight` box. Uses box containment because GPU static props have no CPU visible-face list. Legacy `PerPolySelect` is a presentation geometry test, not this active terrain picker. |
| Render/cull/light paths (`mclib/bdactor_render.cpp`, `bdactor_static.cpp`, `bdactor_lightbake.cpp`, `recalcBounds`) | Remake LOD, bounds, materials, shadows and presentation effects remain visual. `calcAdjCell` computes no output and has no gameplay effect. |
| Bridge overlays (`code/bldng.cpp:844`) | Object damage state changes the terrain overlay. No replacement geometry read. No building geometry-based terrain flattening was found. |

The survey and audit are external evidence at
`E:/mc2-archive-2026-10/proofs/placement-survey/REPORT.md` and
`E:/mc2-archive-2026-10/proofs/map-conflict-audit/REPORT.md`.

## Reproducing the proof

`MC2_DEBUG_STATE_DUMP=1` also emits `[BLDG_GAMEPLAY]` rows at actual building
coverage calls. Each includes placement/name/rotation/status, gameplay bounds,
the actual returned cell set, and original intact/destroyed blocking cells and
per-cell LOS height samples. The second state is explicitly absent when no
damage mesh exists. These are geometry observations, not a scripted gate-opening
or destruction scenario; dynamic gate cells come from unchanged mission MOVE
data. Full logs stay outside git.

```
py -3 tools/stock_blocking.py --exe E:/mc2-installs/claude-stock-blocking/mc2.exe --out E:/mc2-archive-2026-10/proofs/stock-blocking/cells
```

The harness runs mc2_01..24 in both modes through the existing smoke runner,
requires nonempty observations, compares multisets (including duplicate
placements), and rejects a comparison that never loaded remake geometry.
The pytest cases exercise changed cells, rubble, LOS, missing observations and
duplicate placement detection. `results.json` records per-mission counts and
differences. Simulation identity pairs and day/night visual captures are
separate proofs of simulation equality and continued remake rendering.
