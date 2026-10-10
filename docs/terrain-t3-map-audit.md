<!-- doc-status: stale | reviewed:2026-10-11 -->
# Terrain T3 Map-Compatibility Audit (Phase 8z Gate)

**Purpose:** Identify any production or modded terrain map that hits the T3 condition
(no colormap atlas AND no tile-handle nodes), which would render as black or crash if
the legacy `setupTextures`/`makeLists`/`geometry`/`slimReduce` path is deleted in Phase 8z.

**Date:** 2026-06-10  
**Branch:** `claude/terrain-gen-pcg` (nifty-mendeleev worktree)  
**Auditor:** Claude Code (read-only recon)

---

## PART A — Engine Predicate

### A.1 `ShouldArmGpuTerrain` signature

Source: `GameOS/gameos/gos_terrain_arm_logic.h:42-45`

```cpp
inline bool ShouldArmGpuTerrain(bool solidEnabled,
                                bool hasTileNodeIds,
                                bool hasColormapAtlas) {
    return solidEnabled && (hasTileNodeIds || hasColormapAtlas);
}
```

Called from `GameOS/gameos/gos_terrain_indirect_atlas.cpp:707-710`:
```cpp
if (gos_terrain_arm::ShouldArmGpuTerrain(
        gpu_driven::IsTerrainSolidEnabled(),
        !g_uniqueTerrainNodeIds.empty(),
        g_atlasGLTex != 0)) {
```

**`hasTileNodeIds`** (`!g_uniqueTerrainNodeIds.empty()`) — true when at least one terrain
quad has a real tile-texture handle (cement/overlay tile), i.e. `_wp2 != 0 && _wp2 != 0xFFFFFFFF`
and `< MC_MAXTEXTURES`. Derived by `CollectUniqueNodeIds()` scanning baked quad `_wp2` fields.

**`hasColormapAtlas`** (`g_atlasGLTex != 0`) — true when the GPU atlas texture was successfully
uploaded by `BuildColormapAtlas()` (`GameOS/gameos/gos_terrain_indirect_atlas.cpp:179`).

The comment at `gos_terrain_arm_logic.h:40-41` explicitly names the T3 condition:
> "A legacy non-colormap map has neither signal and falls back to the CPU path."

### A.2 Atlas load chain — how `<name>` is derived

Source: `mclib/terrain.cpp:619-645`

At map load, `Terrain::init()` extracts the base name from the `.pak` filename:

```cpp
_splitpath(pakFile->getFilename(), NULL, NULL, name, NULL);   // mclib/terrain_init.cpp:8
terrainName = new char[strlen(name)+1];
strcpy(terrainName, name);

if (colorMapName)           // overrides name if set (mclib/terrain_init.cpp:12-13)
    strcpy(name, colorMapName);
```

`colorMapName` is populated at `terrain.cpp:523-526` from the `.fit` file field `ColorMapName`:
```cpp
if (file->readIdString("ColorMapName", mapName, 1023) == NO_ERR)
    colorMapName = new char[strlen(mapName)+1];
    strcpy(colorMapName, mapName);
```

**Naming rule (in priority order):**

1. If `.fit` has `ColorMapName = "X"`, atlas key = `X`
2. Otherwise, atlas key = pak basename (stem of `.pak` filename)

The engine probes (in order, first match wins):
- `<texturePath>/<key>.burnin.jpg`  (mclib/terrain_raycast.cpp:105 / terrtxm2.cpp BURNIN-DISK-JPEG)
- `<texturePath>/<key>.burnin.ktx2` (terrtxm2.cpp COLORMAP-BC7-KTX2-1, requires `GLEW_ARB_texture_compression_bptc`)
- `<texturePath>/<key>.burnin.tga`  (terrtxm2.cpp legacy TGA path)
- `<texturePath>/<key>.tga`         (legacy fallback, no burnin suffix)

`terrainTextures2 != NULL` is set at `mclib/terrain_init.cpp:26` **only if** at least one of those
files exists. `BuildColormapAtlas()` at `GameOS/gameos/gos_terrain_indirect_atlas.cpp:179` then uploads the data
to `g_atlasGLTex`.

### A.3 `terrainTextures2 == NULL` and T11

`terrainTextures2` is the `TerrainColorMap*` at `mclib/terrain.cpp:100`, declared NULL.
It is allocated at `mclib/terrain_init.cpp:26` iff a colormap file exists on disk.

T11 (`terrainTextures2 == NULL` at `terrain.cpp:3658`) is part of the `fullyArmed`
fast-path-skip gate (`mclib/terrain_render.cpp:554-561`). A map with no colormap has
`terrainTextures2 == NULL` → `fullyArmed == false` → the `skipSetup` path (which skips
`setupTextures`) cannot fire → legacy `setupTextures` runs unconditionally.

T11 is a **downstream symptom of T3**: if T3 fires (map never arms GPU path), the water
fast-path gate at `mclib/terrain_render.cpp:559` also blocks. Fixing T3 for all maps resolves T11.

---

## PART B — Data Sweep

### B.1 Deployment scope

| Deploy target | Path | Role |
|---|---|---|
| Game (v0.4) | `A:/Games/mc2-opengl/mc2-win64-v0.4/` | Production game exe + missions |
| Editor (0.4c) | `A:/Games/mc2-opengl/mc2-win64-0.4c/` | Mission editor + mod hosting |

The v0.4 game install has **no `mods/` directory** — all mods are hosted in 0.4c.

### B.2 Terrain map identification

Of the 259 entries in `data/missions/`, terrain maps are identified as files with `.pak`
extension that are NOT `.pak.old` backups. Non-terrain entries (.fit, .abl, .abx, .abi,
.fst, .h, purchase_*.fit, etc.) are excluded.

**57 unique terrain map basenames found** in `A:/Games/mc2-opengl/mc2-win64-v0.4/data/missions/`:

```
1kbasicmap  area16      area41      area77      bigbattle   camouflage
cityassault citycleanup citysearch  coldstone   corona      crow
deadfly     delta       demarcation down        eastpointbridge fortfowler
harbor      ironearth   ironrock    mc2_01      mc2_02      mc2_03
mc2_04      mc2_05      mc2_06      mc2_07      mc2_08      mc2_09
mc2_10      mc2_11      mc2_12      mc2_13      mc2_14      mc2_15
mc2_16      mc2_17      mc2_18      mc2_19      mc2_20      mc2_21
mc2_22      mc2_23      mc2_24      mc2xquickdummy metropolitan mudhole
oildrill    paradise    redrockoutpost saratoga spaceport   terragen
torrin      ugly        wastelands
```

### B.3 Mod map identification

Mods hosted in `A:/Games/mc2-opengl/mc2-win64-0.4c/mods/`:

| Mod | Terrain paks | Notes |
|---|---|---|
| **DarkRain** | 24 paks (area16…wastelands) | Override/reskin paks; same names as base maps |
| **PicturesOfARebeliion** | 31 paks (poar_01…poar_31) | New campaign maps |
| **TangoMaster** | 20 paks (blackisland…zebra) | New campaign maps |
| **cveg** | 8 paks (cityassault, corona, delta, mc2xquickdummy, metropolitan, mudhole, paradise, redrockoutpost) | Override/variant paks; same names as base maps |
| **mc2x-compat** | 0 terrain paks | Compatibility shims only (no .pak) |

### B.4 Atlas file locations

- Base game atlases: `A:/Games/mc2-opengl/mc2-win64-v0.4/data/textures/`  (84 burnin files)
- DarkRain atlases: `A:/Games/mc2-opengl/mc2-win64-0.4c/mods/DarkRain/data/textures/` (24 jpg)
- PicturesOfARebeliion atlases: `A:/Games/mc2-opengl/mc2-win64-0.4c/mods/PicturesOfARebeliion/data/textures/` (31 files)
- TangoMaster atlases: `A:/Games/mc2-opengl/mc2-win64-0.4c/mods/TangoMaster/data/textures/` (20 jpg)
- cveg: **no mod-local atlases** — relies on base-game atlases (confirmed present for all 8 maps)

### B.5 ColorMapName override analysis

Maps in the base install whose `.fit` file specifies a `ColorMapName` override
(atlas key differs from pak name — all confirmed matching atlases exist):

| Map (pak name) | ColorMapName override | Atlas key used |
|---|---|---|
| area16 | area16 | same |
| area41 | area41 | same |
| area77 | area77 | same |
| bigbattle | bigbattle | same |
| camouflage | camouflage | same |
| citycleanup | citycleanup | same |
| citysearch | citysearch | same |
| coldstone | coldstone | same |
| crow | crow | same |
| deadfly | deadfly | same |
| demarcation | demarcation | same |
| down | down | same |
| eastpointbridge | eastpointbridge | same |
| fortfowler | fortfowler | same |
| harbor | harbor | same |
| ironearth | ironearth | same |
| ironrock | ironrock | same |
| oildrill | oildrill | same |
| saratoga | saratoga | same |
| spaceport | spaceport | same |
| terragen | terragen | same |
| torrin | torrin | same |
| ugly | ugly | same |
| wastelands | wastelands | same |

All `.fit`-specified `ColorMapName` values are identical to the pak name; no indirection
to a different atlas name was found.

Maps without `ColorMapName` in their `.fit` (or without a `.fit`):
`1kbasicmap`, `cityassault`, `corona`, `delta`, `mc2_01`–`mc2_24`, `mc2xquickdummy`,
`metropolitan`, `mudhole`, `paradise`, `redrockoutpost` — all use pak basename as atlas key.

---

## PART C — Classification Table

### C.1 Base game maps (v0.4) — 57 maps

| Map | Atlas key | jpg | tga | ktx2 | Classification |
|---|---|---|---|---|---|
| 1kbasicmap | 1kbasicmap | YES | – | – | **ARMED** |
| area16 | area16 | YES | – | – | **ARMED** |
| area41 | area41 | YES | – | – | **ARMED** |
| area77 | area77 | YES | – | – | **ARMED** |
| bigbattle | bigbattle | YES | – | – | **ARMED** |
| camouflage | camouflage | YES | – | – | **ARMED** |
| cityassault | cityassault | YES | YES | – | **ARMED** |
| citycleanup | citycleanup | YES | – | – | **ARMED** |
| citysearch | citysearch | YES | – | – | **ARMED** |
| coldstone | coldstone | YES | – | – | **ARMED** |
| corona | corona | – | YES | – | **ARMED** |
| crow | crow | YES | – | – | **ARMED** |
| deadfly | deadfly | YES | – | – | **ARMED** |
| delta | delta | YES | YES | – | **ARMED** |
| demarcation | demarcation | YES | – | – | **ARMED** |
| down | down | YES | – | – | **ARMED** |
| eastpointbridge | eastpointbridge | YES | – | – | **ARMED** |
| fortfowler | fortfowler | YES | – | – | **ARMED** |
| harbor | harbor | YES | – | – | **ARMED** |
| ironearth | ironearth | YES | – | – | **ARMED** |
| ironrock | ironrock | YES | – | – | **ARMED** |
| mc2_01 | mc2_01 | YES | – | – | **ARMED** |
| mc2_02 | mc2_02 | YES | – | – | **ARMED** |
| mc2_03 | mc2_03 | YES | – | – | **ARMED** |
| mc2_04 | mc2_04 | YES | – | – | **ARMED** |
| mc2_05 | mc2_05 | YES | – | – | **ARMED** |
| mc2_06 | mc2_06 | YES | – | – | **ARMED** |
| mc2_07 | mc2_07 | YES | – | – | **ARMED** |
| mc2_08 | mc2_08 | YES | – | – | **ARMED** |
| mc2_09 | mc2_09 | YES | – | – | **ARMED** |
| mc2_10 | mc2_10 | YES | – | – | **ARMED** |
| mc2_11 | mc2_11 | YES | – | – | **ARMED** |
| mc2_12 | mc2_12 | YES | – | – | **ARMED** |
| mc2_13 | mc2_13 | YES | – | – | **ARMED** |
| mc2_14 | mc2_14 | YES | – | – | **ARMED** |
| mc2_15 | mc2_15 | YES | – | – | **ARMED** |
| mc2_16 | mc2_16 | YES | – | – | **ARMED** |
| mc2_17 | mc2_17 | YES | – | – | **ARMED** |
| mc2_18 | mc2_18 | YES | – | – | **ARMED** |
| mc2_19 | mc2_19 | YES | – | – | **ARMED** |
| mc2_20 | mc2_20 | YES | – | – | **ARMED** |
| mc2_21 | mc2_21 | YES | – | – | **ARMED** |
| mc2_22 | mc2_22 | YES | – | – | **ARMED** |
| mc2_23 | mc2_23 | YES | – | – | **ARMED** |
| mc2_24 | mc2_24 | YES | – | – | **ARMED** |
| mc2xquickdummy | mc2xquickdummy | YES | YES | – | **ARMED** |
| metropolitan | metropolitan | YES | YES | – | **ARMED** |
| mudhole | mudhole | YES | YES | – | **ARMED** |
| oildrill | oildrill | YES | – | – | **ARMED** |
| paradise | paradise | YES | YES | – | **ARMED** |
| redrockoutpost | redrockoutpost | YES | YES | – | **ARMED** |
| saratoga | saratoga | YES | – | – | **ARMED** |
| spaceport | spaceport | YES | – | – | **ARMED** |
| terragen | terragen | YES | – | – | **ARMED** |
| torrin | torrin | YES | – | – | **ARMED** |
| ugly | ugly | YES | – | – | **ARMED** |
| wastelands | wastelands | YES | – | – | **ARMED** |

**Base game result: 57/57 ARMED. 0 T3 blockers.**

### C.2 DarkRain mod (24 maps — override paks, same names as base)

DarkRain paks are overrides of base maps; atlas files are both in the mod's own
`data/textures/` AND in the base game textures. Both locations confirmed present.
All 24 maps: **ARMED**.

### C.3 PicturesOfARebeliion mod (31 maps — poar_01…poar_31)

| Map | Atlas (mod-local) | Classification |
|---|---|---|
| poar_01…poar_08 | jpg | **ARMED** |
| poar_09 | tga (only — no jpg) | **ARMED** |
| poar_10…poar_31 | jpg | **ARMED** |

All 31 maps: **ARMED**.

### C.4 TangoMaster mod (20 maps)

All 20 maps have `.burnin.jpg` in `mods/TangoMaster/data/textures/`: **ARMED**.

### C.5 cveg mod (8 maps — override paks, no mod-local atlases)

cveg supplies no `data/textures/` overrides. Atlas lookup falls through to base game
`A:/Games/mc2-opengl/mc2-win64-v0.4/data/textures/`:

| Map | Base atlas | Classification |
|---|---|---|
| cityassault | jpg + tga | **ARMED** |
| corona | tga | **ARMED** |
| delta | jpg + tga | **ARMED** |
| mc2xquickdummy | jpg + tga | **ARMED** |
| metropolitan | jpg + tga | **ARMED** |
| mudhole | jpg + tga | **ARMED** |
| paradise | jpg + tga | **ARMED** |
| redrockoutpost | jpg + tga | **ARMED** |

All 8 maps: **ARMED**.

### C.6 mc2x-compat mod

No terrain `.pak` files. Not a map source. **Not applicable.**

---

## BLOCKERS

### T3 BLOCKER LIST

**None found.**

> **0 T3 blockers found — T3 gate CLEARS.**

Every terrain map in every deployed location has at least one `.burnin.jpg`, `.burnin.tga`,
or `.burnin.ktx2` atlas file accessible via the engine's probe chain. No map hits
`ShouldArmGpuTerrain(solid, false, false) == false` due to absent atlas.

---

## Unknown / Unclassified

### Orphaned atlas files (atlases with no corresponding terrain .pak)

The following burnin atlas files exist in the texture stores but have no matching terrain
`.pak` in any `data/missions/` directory (prod or mod):

- `mc2_m01`–`mc2_m10` — multiplayer map atlases; no `.pak` deployed
- `tut_01`–`tut_05` — tutorial map atlases; no `.pak` deployed
- `e3demo`, `genmap`, `newmap`, `dtg` — development/demo atlases; no `.pak` deployed
- `poar_09.burnin.tga` in v0.4 base — superseded by the PicturesOfARebeliion mod copy (identical name)
- Various dev/test names (`aaaa`, `dtest`, `desert_test60x60`, `mc2-01-test`, `mc2_01_testing`,
  `1kswamp`, `dawfaefqwdfqwduhiivukbwne`, etc.) — no `.pak` deployed

These orphaned atlases represent zero blocker risk (no `.pak` → no terrain to render → no
T3 path can be hit). They are noted here so the list is complete; silent omission is
forbidden.

### Maps not audited

None — all discovered terrain `.pak` files have been classified.

---

## Method Limitations

1. **Filename cross-reference only.** Atlas presence was verified by checking for
   `<key>.burnin.{jpg,tga,ktx2}` files on disk. The `.pak` files were NOT opened/dumped to
   verify that tile-node `_wp2` fields are actually baked (i.e., `hasTileNodeIds` was not
   independently confirmed). The atlas-presence check is sufficient: if an atlas file exists,
   `terrainTextures2` will be non-NULL → `hasColormapAtlas` = true → `ShouldArmGpuTerrain`
   returns true regardless of tile-node presence.

2. **`ColorMapName` cross-reference.** All `.fit` files in `data/missions/` were searched for
   `ColorMapName` overrides. Every found override resolved to an atlas that was verified on
   disk. No indirection to a missing atlas was discovered.

3. **Mod search scope.** Only `A:/Games/mc2-opengl/mc2-win64-0.4c/mods/` was searched.
   If additional mod directories exist outside this path (custom user mods, development
   worktrees, etc.), they were not audited. The v0.4 game install has no `mods/` directory.

4. **`colorMapName` runtime override.** `Terrain::setColorMapName()` (`mclib/terrain.cpp:408`) can
   be called programmatically to override the atlas key at runtime. No call sites in the
   codebase scripts were found that pass a different name (all `.fit` files use `ColorMapName`
   matching the pak name). This path was not exhaustively audited via callsite analysis.

5. **`skipColormapTiles()` fast-path.** Some maps invoke the `skipColormapTiles()` path
   (`mclib/terrtxm2_load.cpp:243`), which skips per-tile GL upload but still allocates `terrainTextures2`
   and uploads the atlas. This means `hasColormapAtlas` is still true for such maps; they
   are correctly classified as ARMED.

---

## Summary

| Scope | Maps audited | T3 blockers | Unknowns |
|---|---|---|---|
| Base game (v0.4) | 57 | 0 | 0 |
| DarkRain mod | 24 | 0 | 0 |
| PicturesOfARebeliion mod | 31 | 0 | 0 |
| TangoMaster mod | 20 | 0 | 0 |
| cveg mod | 8 | 0 | 0 |
| mc2x-compat mod | 0 (no paks) | 0 | 0 |
| **TOTAL** | **140** | **0** | **0** |

**T3 gate status: CLEARS. Phase 8z legacy deletion is not blocked by any T3 non-atlas map in the audited deployment scope.**
