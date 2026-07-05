# TERRAIN-TREE-SCATTER-ASSET-RECON-1 — RECON

**Slice:** TERRAIN-TREE-SCATTER-ASSET-RECON-1 (forest capability for scatter system)
**Worktree:** A:/Games/mc2-controlmap-sample-1 — read-only recon, no source changes.
**Date:** 2026-07-01
**Reference:** UE Landscape Mountains sample (instanced conifers) — user's target look.

## 1. Asset inventory — `C:/Users/Joe/Downloads/GameAsset`

No dedicated conifer/pine/fir asset anywhere in the tree (grep for `conifer|pine|fir` across the
whole tree: zero hits). What exists under `Trees/`:

| Path | Content | Verdict |
|---|---|---|
| `Trees/22-trees_9_obj/trees9.obj` | Single 33.6 MB OBJ, **9 combined broadleaf trees** (oak/walnut/mossy — see `.mtl`: `Oak_Leav`, `Walnut_L`, `Bark___0/1`), 252k verts / 247k faces **for all 9 combined**. No LODs, no per-tree split. | Too heavy to use as-is (would need splitting + decimation per tree — real work). Not conifer. |
| `Trees/tree_pack_1.1/models/*.fbx` | 36 individual `treeNN.fbx` (~16-17 KB each, tiny) + 8 `bushNN.fbx`. Textures `treeNN.png`/`bushNN.png`, no normal/spec maps seen. | Lightweight, good instancing candidate, but **not confirmed conifer** by name/texture alone — generic low-poly tree/bush silhouettes. Needs a visual check before committing. |
| `Trees/treepack1/` | Bark/branch **texture set only** (jpg/png, some 16 MB uncompressed), plus a `.blend` — no ready mesh export seen at top level. | Texture-only lead; would need Blender export work. |
| `uploads_files_5194613_tree.gltf`, `uploads_files_5194794_tree-01.gltf` | Single-tree GLTF, material name `AM113_063_Tilia_Bark` (Tilia = linden/basswood). | Broadleaf, not conifer. |
| `uploads_files_3676240_tree+asset.rar` (277 MB) | Unopened (large archive) — likely a bigger tree/forest asset pack given size. | Unexplored — flag for a follow-up if v1 wants better assets. |
| `uploads_files_3679880_forest.blend` (10.6 MB), `uploads_files_4996646_Broadleaf_GLTF` | Named "forest"/"Broadleaf" — again broadleaf-coded by name. | Not conifer; unexplored contents. |
| `Terrain/forest_ground_04_4k.blend.zip` | Ground/texture only (forest floor material), not a tree mesh. | Not usable for tree geometry. |

**Bottom line: zero ready-to-use conifer assets in GameAsset.** Everything found or named
"tree/forest" is broadleaf-coded (oak, walnut, linden/Tilia). Getting a conifer look (UE Landscape
Mountains reference) means either (a) sourcing new conifer GLBs (e.g. Polyhaven, Quaternius, or a
game-asset-store conifer pack) or (b) opening the two large unexplored archives
(`tree+asset.rar` 277 MB, `forest.blend` 10.6 MB) which may contain conifers — not yet checked.

## 2. Stock MC2 tree/forest system

- **`TERROBJ_TREE`** and **`TERROBJ_FOREST`** are both real `TerrainObjectType` subtypes
  (`code/terrobj.h:42,44`; dispatch in `code/terrobj.cpp:374-415,496-560`).
- **Trees are "magical"** (terrobj.cpp:498): a mech colliding with a `TERROBJ_TREE` knocks it down
  (`TREE_FALL_RATE`/`TREE_FALL_ACCEL`, terrobj.cpp:304-305) — a per-tree gameplay behavior, not
  purely decorative.
- **`TERROBJ_FOREST`** is a **pathfinding/move-map gate**, not an instance-count mechanism: on
  destruction it calls `me->openSubAreas()` (terrobj.cpp:559,1569-1585), which opens move-map areas
  previously blocked by the forest polygon (`subAreas0`/`subAreas1`, `MAX_SPECIAL_SUB_AREAS`). A
  "forest" object type is really one blocking-area placeholder, not N individually-placed trees.
- **`isForestClump`** (`TGLData` block, read at bdactor.cpp:793/5194) is a per-shape-node placement
  flag: for forest-clump-flagged buildings/trees, per-vertex cell marking uses `vertexPos.z <= 1.0f`
  (vs `>= 1.0f` for normal buildings) to decide passability-marking — i.e. a **single GLB/ASE mesh
  can contain a multi-tree clump** (several tree meshes combined as one prop, each contributing its
  own footprint cells). This is the stock "how do forests read as dense" trick: **author N trees
  into ONE mesh/prop**, not N separate placed objects.
- **Editor forest tool**: `editor/EditForestDlg.cpp/.h` is the **legacy MFC** forest-list dialog
  (load/save/rename a named forest "brush" file) — old editor plumbing, likely superseded by the
  modern ImGui ContextualUI/scatter_tool workflow; not confirmed wired into the current editor build.
- **Vegetation card system** (`GameAdapters/VegetationAdapter.h/.cpp`): a **separate, simpler**
  system — scans terrain "green-zone" tiles at mission load, builds a **GPU instance list of
  billboard grass/bush cards**, flushed once/frame via `GosVegetation::flush()`. This is
  billboard-card grass/bush, not a mesh-based tree system — wrong tool for "conifer patches" (no
  trunk silhouette, no mech-collision, no fall behavior); it's the right tool for ground-cover
  filler around/under trees, not for the trees themselves.

## 3. Object-class comparison for scattered trees

| Axis | TREE class | BUILDING class (MarbleCliff pattern) | Vegetation card |
|---|---|---|---|
| Slope pitch/tilt | **Yes** — `EulerAngles(pitch, yaw, 0)` (bdactor.cpp:6744); ground via single bilinear sample + tilt to match local slope normal. Matches UE-conifer-on-slope look. | **No** — yaw-only, `EulerAngles(0, yaw, 0)` (bdactor.cpp:3966); wide meshes float/clip on steep slopes (documented P0 limitation, `.claude/TERRAIN-CLIFF-MESH-DRESSING-P0-RECON.md:39`). | N/A — billboard cards, always camera-facing, no mesh tilt concept. |
| LOD | Yes — full `MAX_LODS` chain + `FileNameN`/`DistanceN` (bdactor.cpp:5192-5236); `lodDistance[i] *= 5.0f` push-out already applied for visibility. | Yes, same LOD mechanism (shared bdactor code path). | No LOD — cards are cheap by construction (billboard quads). |
| Shadows | Normal shadow path (`TreeAppearance::renderShadows`, bdactor.cpp:6142); "no shadows on fallen trees" special-case (terrobj.cpp:1083). | Normal shadow path. | Not investigated — likely no/limited shadow (typical for billboard veg). |
| Gameplay | Falls when hit by a mech (terrobj.cpp:498-527) — could read as odd for background forest dressing (mechs shouldn't be near scatter trees, but a stray shot/collision would visibly topple a "background" tree). | Impassable-toggle only (`SetImpassable=0` for pass-through dressing, per scatter recon); no fall animation. | No collision. |
| Editor round-trip | Same packet-1 record path as buildings — proven (`TERRAIN-SCATTER-MASK-1-RECON.md`); TREE catalog wiring is described as "harder" than BUILDING in that recon (object2.pak catalog entry, Buildings.csv-equivalent for trees not yet confirmed as simple). | **Proven, cheapest** — MarbleCliff (FitID 1188) + MarbleCliffScatter (1189) already installed and cooking via `tools/pak_append.py` + `data/tgl/*.ini`. | Not a placed-object system at all — no per-instance editor entry. |
| Black-tree/prewarm landmine exposure | Same static-registry substrate as buildings (`GpuStaticPropBatcher::registerMultiShape`, bdactor.cpp:6714-6725; per-LOD registration, "TREE-OVERRIDE-LOD-MVP" work already in tree path) — same proven prewarm/register/cull path, so **no additional exposure** versus BUILDING class as long as the cook only emits packet-1 records (per `TERRAIN-SCATTER-MASK-1-RECON.md` landmine #1: never add a runtime skip/frame-stamp gate). | Proven zero-landmine path (scatter recon already validated this for MarbleCliffScatter). | Different substrate (`GosVegetation`) — not exposed to the TG_MultiShape static-registry landmine at all, but also not proven at 200-500 discrete "tree" instances (it's a tile-scan system, not object-count-driven). |
| Perf at 200-500 instances | Cost driver per scatter recon is **prewarm (texture residency) + per-frame light-resubmit** (`BldgAppearance`/`TreeAppearance::touch`), not draw calls — same for TREE as BUILDING. No measured number yet for either at 500; recon recommends starting at ~300-500 conservatively either way. | Same. | Card system is a per-tile scan at load + flush, cost scales with green-zone tile count not "instance count" in the same sense — different cost model entirely, not apples-to-apples with 200-500 discrete placed objects. |

**Class recommendation: TREE class**, reversing the general scatter recon's earlier BUILDING-first
bias — that bias was calibrated for **rock/cliff dressing** (where tilt doesn't matter and
BUILDING's simpler catalog wiring won). For **trees specifically on slopes** (gaea_peaks target),
the pitch/tilt capability is the deciding factor per the user's own framing ("TREE class... gets
pitch on slopes per cliff recon finding"), and the static-registry/prewarm risk is identical between
TREE and BUILDING (both ride the same `GpuStaticPropBatcher` substrate, both already proven in
production code). The "harder catalog wiring" caveat from the rock-scatter recon needs one concrete
follow-up: confirm a TREE-class objTypeNum can be appended via the same `pak_append.py` +
`object2.pak` flow used for MarbleCliffScatter (no evidence found in this recon pass that it can't —
`countObject`/`objtype.cpp:355` dispatches by class generically, TREE is listed as a first-class
option alongside BUILDING in `objmgr.cpp:1336-1380` per the scatter recon's own citation).

## 4. Import path / GLB requirements

- **TGL .ini `Source=` GLB import already wired for TREE class** — `ASSIMP-TREE-IMPORT-1`
  (bdactor.cpp:5179-5260, `TreeAppearanceType::init`): an `[Import] Source=` key triggers
  `treeShape[0]->LoadFromFile(importSourceBase)` (LOD0 only; LOD1+ and the damage state still need
  ASE). This is a direct mirror of the mech3d.cpp/MarbleCliff `[Import]` pattern already proven for
  BUILDING-class GLB import. `TG_TypeMultiShape::LoadFromFile` is defined at `mclib/msl.cpp:444`.
- **Conifer GLB requirements for 500-instance scatter**, given the above:
  - LOD0 only strictly required (LOD1+ optional — if omitted, `MAX_LODS` loop just doesn't populate
    further bands; single-LOD trees are acceptable for v0, matching the "reuse stock trees as-is"
    option below).
  - Poly budget: no stock-enforced cap found; use the `tree_pack_1.1`-style FBX (~16 KB file,
    almost certainly a few hundred to low-thousands tris) as the sane reference — the 33 MB combined
    OBJ (252k verts total /9 ≈ 28k verts per tree) is the **anti-pattern** to avoid per-instance at
    500 count.
  - Static-prop registration hard-fails on the first non-SHAPE_NODE child in the import (per
    `TERRAIN-CLIFF-MESH-DRESSING-P0-RECON.md`, landmine #5 in the scatter recon) — any candidate GLB
    must be a clean single/flat-hierarchy mesh export.
  - **No billboard far-LOD support found in the engine.** The only distance-based behavior is the
    ASE/GLB `FileNameN`/`DistanceN` LOD-mesh swap chain (same mechanism as buildings) — there is no
    camera-facing-billboard fallback for trees at range. (The "black billboard square" comment at
    bdactor.cpp:5885 is an unrelated bug-diagnostic name for the static-registration fast-path replay,
    not a real billboard-LOD feature — do not confuse the two.) This means far-LOD cost control for
    conifer patches must come from the normal mesh-LOD chain (a cheap low-poly LOD2+), not from
    billboards.

## 5. Perf evidence — stock forest counts

No stock mission was found with a large discrete-instance forest (`TERROBJ_FOREST` is a
move-map-blocking placeholder polygon per §2, not N placed tree objects, so "how many trees does a
stock forest have" is the wrong question — stock forests are typically **one or a handful of large
forest-shaped meshes/footprints**, not hundreds of individually-registered `TerrainObject`s).
The only concrete instance-count guidance in the repo is the **scatter_tool.py `HARD_CAP = 500`**
(tools/terrain_beautify/scatter_tool.py:79) and the general scatter recon's own conservative
recommendation of **~300-800 props/map**, capped by prewarm + per-frame light-resubmit cost, not
draw cost (`TERRAIN-SCATTER-MASK-1-RECON.md` §"Caps/perf"). No stock mission analyzer run in this
pass measured an actual forest-object count — treat 300-500 as the safe v0 ceiling until a real
prewarm-cost measurement is taken on a cooked mission (a smoke run, per that recon's Acceptance
section, is the cheapest way to get a first real number).

## 6. Recommendation — cheapest chain to "conifer patches on gaea_peaks slopes"

1. **Asset**: no usable conifer asset exists in `GameAsset` today (§1). Two options, cheapest first:
   - **(a) v0 fallback — reuse stock MC2 tree types AS-IS.** If any stock TREE-class `.ini`/appearance
     already ships with the game (not confirmed present/absent in this recon pass — no stock tree
     `.ini` files were enumerated; would need a `data/tgl/*.ini` listing pass restricted to TREE-class
     appearances as a fast follow-up), reusing it needs **zero new asset work** and **zero import-path
     risk** (skips ASSIMP-TREE-IMPORT-1 entirely, uses the proven ASE loader). This is very likely the
     cheapest possible v0 if a stock evergreen-ish tree exists — check before sourcing anything new.
   - **(b) New conifer GLB.** Cheapest external source = a small existing free conifer pack
     (Polyhaven/Quaternius-style, single low-poly conifer, LOD0-only, flat hierarchy) imported via the
     already-wired `[Import] Source=` TREE path. Do **not** use `Trees/22-trees_9_obj/trees9.obj`
     (too heavy, needs splitting) or the unconfirmed-conifer `tree_pack_1.1`/gltf files without a
     visual check first (they read as broadleaf by name/material).
2. **Class**: **TREE**, for the slope-pitch/tilt capability (§3) — the deciding factor for a
   mountain-slope conifer look; catalog-wiring risk is a same-day verification step (append via
   `pak_append.py`, mirror the MarbleCliffScatter 1189 install), not a structural blocker.
3. **Chain**: reuse the proven `scatter_tool.py` masks/blue-noise/packet-1-cook machinery unchanged
   — only the objTypeNum/prop changes (a new dedicated TREE-class scatter objTypeNum, reserved like
   1189, e.g. next free ID), slope-gate direction flipped (trees likely want to *favor* moderate slope
   bands rather than exclude them, per `.claude/TERRAIN-SCATTER-MASK-1-RECON.md` §"Masks wanted").
4. **Budget**: start at **300 instances/map** (below the tool's existing 500 hard cap), one shared
   objTypeNum so all instances batch into one substrate record set (recon §"Caps/perf"), and raise
   only after a real smoke-tested prewarm/frame-cost measurement.

## Open follow-ups (not resolved by this recon)

- Enumerate `data/tgl/*.ini` for any existing stock TREE-class appearance (fast — answers the
  "reuse stock trees as-is" question definitively; not done in this pass).
- Confirm TREE-class objTypeNum append actually works end-to-end via `pak_append.py` (the "harder
  catalog wiring" caveat from the rock-scatter recon is asserted, not re-verified here).
- Open `uploads_files_3676240_tree+asset.rar` (277 MB) and `uploads_files_3679880_forest.blend`
  (10.6 MB) — both unexplored, either could contain a usable conifer pack.
