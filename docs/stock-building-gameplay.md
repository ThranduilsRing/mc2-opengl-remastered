# Building gameplay and replacement art

The original appearance remains gameplay authority. `BldgAppearanceType::init`
loads its regular LOD and damage shapes independently of render overrides
(`mclib/bdactor_type.cpp:70`). This includes mod-owned shapes: the bundled kit
declines an appearance or mesh provided by another mod
(`mclib/bdactor_type.cpp:12`, `bundledOverrideShadowed`). Explicit mod override records keep their existing
render-only contract. Classic does not mount the bundled kit.

The loader already retains original type geometry for simulation poses and
fallback rendering. No additional full mesh or permanent per-placement vertex
array is needed. Bounds and terrain marking use temporary `CreateFrom`
hierarchies sharing that geometry; terrain marking leaves the remake instance
intact. Instances without render overrides follow their existing paths.

## Geometry consumers

| Consumer | Source and consequence |
| --- | --- |
| `BldgAppearance::calcCellsCovered` (`mclib/bdactor_terrain.cpp:56`) | Original LOD0 or damage shape; excludes `_PAB`, tests local Z >= 1 WU (forest clumps reverse the test), rotates and converts individual vertices to cells. Pathing footprint and LOS target corners. |
| `TG_MultiShape::GetShapeVertexInEditor` (`mclib/msl_shape.cpp:137`) | Original vertex plus relative/root centres, TGL-to-world axes, placement rotation. This is the placement-survey rule, without polygon filling. |
| `BldgAppearance::markTerrain` (`mclib/bdactor_terrain.cpp:130`) | Editor MOVE/LOS map generation. Original intact cells get special gate/wall/bridge IDs; original rubble is permanently blocked. Forest and resource special cases retain their existing rules. |
| `BldgAppearance::markMoveMap`, `markLOS` (`mclib/bdactor_terrain.cpp:352`, `:478`) | Runtime passability and cell LOS height changes; existing dual-shape paths use original shapes under overrides. The active move-map caller is generic resource object 357 (`code/bldng.cpp:803`); destruction calls to this helper are disabled (`code/bldng.cpp:1518`, `:1541`). Actual destruction uses the cached mission cells below and stock `markLOS`. |
| `TerrainObject::calcCellFootprint` (`code/terrobj.cpp:1386`) | Consumes coverage into cell/world bounds used by `getLineOfSightNodes`. |
| `TerrainObject::calcSubAreas`, `openSubAreas`, `closeSubAreas` (`code/terrobj.cpp:1455`, `:1544`, `:1555`) | Cached mission cells and MOVE areas drive opening gates and destruction; art does not provide these cells. |
| `BldgAppearance::init`, `getRadius`, `getTopZ` (`mclib/bdactor.cpp:310`, `mclib/bdactor.h:452`, `:467`) | Original bounds/radius/height under overrides. Includes target/hotspot heights and pick bounds. Previously initialization derived these from the remake. |
| `Building::getLOSPosition`, `getAppearRadius` (`code/bldng.h:242`, `:312`) | Stock height and radius feed `Team::lineOfSight` through `GameObject` queries (`code/gameobj.cpp:1931`, `:1957`, `:1981`). Camera-selected remake LODs cannot change these inputs. |
| `BldgAppearance::updateSimPose` (`mclib/bdactor.cpp:861`) | Already original intact/damage hierarchy; authoritative weapon/hit nodes and animation are independent of art. Presentation-only node queries (`mclib/bdactor.cpp:106`, `:152`, `:532`, `:560`) still use the visible mesh. |
| `Building::update` (`code/bldng.cpp:723`) | Negative INI collision extent falls back to original LOD0 bounds. `GameObject::getExtentRadius` (`code/gameobj.cpp:2033`) and collision (`code/collsn.cpp:227`, `:555`) use the type extent, not remake triangles. |
| Building explosion (`code/bldng.cpp:211`, `1285`, `1514`) | Radius/damage read from INI; explosion position uses authoritative hit node. No mesh-derived splash radius. |
| Terrain building picker (`code/objmgr_pick.cpp:132`, `:672`) | Projects the original `typeUpperLeft/typeLowerRight` box. Uses box containment because GPU static props have no CPU visible-face list. Legacy `PerPolySelect` is a presentation geometry test, not this active terrain picker. |
| Render/cull/light paths (`mclib/bdactor_render.cpp:304`, `mclib/bdactor_static.cpp:156`, `:458`, `mclib/bdactor.cpp:751`) | Remake LOD, bounds, materials, shadows and presentation effects remain visual. `calcAdjCell` (`mclib/bdactor_terrain.cpp:546`) computes no output and has no gameplay effect. |
| Bridge overlays (`code/bldng.cpp:844`) | Object damage state changes the terrain overlay. No replacement geometry read. No building geometry-based terrain flattening was found. |

The survey and audit are external evidence at
`E:/mc2-archive-2026-10/proofs/placement-survey/REPORT.md` and
`E:/mc2-archive-2026-10/proofs/map-conflict-audit/REPORT.md`.

## Reproducing the proof

The `[BLDG_GAMEPLAY]` dump and `tools/stock_blocking.py` comparison harness were
removed (W67 review: the dump swapped the live GameMap and allocated shapes under
a diagnostic gate). The evidence below was gathered with them before removal.

## Validation evidence

The tested engine is `be2016a00`, based on `3e7300f0e` (which already contains
the kit pipeline). Evidence lives under
`E:/mc2-archive-2026-10/proofs/stock-blocking/`. The continuation after the host
reboot preserves the earlier engine changes and proof artifacts; subsequent
documentation commits do not change the tested engine code.

- All 24 missions have zero Classic/Remastered differences in returned blocking
  cells, stock intact/rubble cell sets, LOS samples and gameplay radius/height.
  `cells-verified/results.json` covers mc2_01..19 before the reboot;
  `cells-reboot/results.json` covers mc2_20..24 on NVIDIA. The combined inventory
  is `cells-all-results.json`; each source directory retains both modes' raw
  observations and successful smoke-runner logs. Remastered override counts
  establish that this was an actual art comparison.
- Classic/Remastered simulation pairs: `determinism-summary.json` and
  `determinism/<mission>/{a,b}/manifest.json` record opposite art options for
  mc2_03, mc2_10 and mc2_24. Their shared prefixes contain respectively 2,822,
  2,430 and 2,716 identical simulation frames, including full hashes and RNG
  counters. This checks simulation identity, not raw save-file byte identity.
- Day/night A/B images: `captures/mc2_03/Classic`,
  `captures-warm/mc2_03/Remastered`, and `captures/mc2_10/{Classic,Remastered}`.
  Inspected both pairs: industrial remakes remain visible. The warm day capture
  replaces the first incomplete Remastered capture.
- `mod-battery.json`: all six mod runs passed with the tested engine's matching
  fingerprint. These runs and the determinism pairs were completed before the
  reboot and reused without changing engine code.
- Post-reboot `pytest tests/smoke`: 177 passed. The requested contract rows
  `script_callers|tools_tests` both passed (147 tool modules green), in 428 s
  at `-j6`; their 16 s / 426 s row times exceed the advertised 5 s budget.
- Post-reboot file-size, duplicate-lines, axis-swizzle and C++ idiom ratchets
  passed with `--worktree`. File-size warnings concern existing growth in
  `engine_present_vk.cpp`, `assimp_importer.cpp`, `camera.h` and `tgl.h`.
- The existing `build64-ninja` game tree is warm. `build-all-reboot.log` and
  `build-all-reboot-confirm.log` show all-targets success, including the editor,
  standalone tools, doctest build and production seam build. The initial shell
  reported failure despite the tool's success summary; the incremental confirm
  explicitly propagated the Python exit code (0). The earlier actual doctest
  run is in `stock-blocking-doctest.log` (3,519 passed, two skipped).

The post-reboot runs identify an NVIDIA RTX 2070 SUPER, Vulkan 1.3.280,
with no required capabilities missing or Vulkan validation errors. No GPU
performance comparison is claimed against the earlier AMD runs.
