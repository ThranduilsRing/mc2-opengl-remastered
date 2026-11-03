# Terrain regeneration for mc2_01 (exploration, lane claude/terrain-regen-mc01)

Status: exploration. Nothing here is wired into the game. Proof images live outside git in
`E:/mc2-archive-2026-10/proofs/terrain-regen-mc01/`.

Owner problem: the 100x100 sim heightmap (128 wu per vertex, 12800 wu across) gives mc2_01 stair-stepped shorelines and
"terrible ugly cliffs". Goal: keep the silhouette, get real cliff geometry (UE Megascans style), driven from existing data.

## 1. Export (tools/terrain_export/)

`export_mission.py mc2_01 --out DIR` writes, for the STOCK data only (verified byte-for-byte against mission.fst by
`tools/mc2data/stock_missions.py`; it refuses Carver5-style non-stock data unless `--allow-nonstock`):

| file | content |
| --- | --- |
| `mc2_01_plain.glb` | full-resolution sim mesh (100x100 verts, 19602 tris), baked colour (`mc2_01.burnin.jpg`, 4096), water plane at the .fit `[Water] Elevation` (350), 1000 prop proxy boxes (node extras: object type, rotation) |
| `mc2_01_normalmap.glb` | same plus a tangent-space (OpenGL) normal map baked from the 4x beauty visual heightfield (`mc2_01.beauty/visual_height_4x.r32`), the "normal maps ON" case |
| `mc2_01_normal.png`, `mc2_01_height.r32`, `mc2_01_export.json` | the normal map, raw sim heights (float32 100x100, row 0 = north) and the metadata |

Units: glTF node position = (x, height, -y) * 0.1 (1 unit = 10 wu), so Blender (Z-up) sees x east, y north.
Not baked: the engine's per-class detail normals are shader-tiled, so no single image can represent them; the baked map is
the beauty visual height only (the large-scale relief), which is the part that matters for sculpting.

`render_blender.py` renders either GLB with Cycles on the RTX 2070 (OPTIX), sun pitch 50 / yaw 11 from the mission .fit.
Run it at BELOW_NORMAL (see lane-rules); `G:/mc2-tmp/rend.ps1`-style launcher is a few lines of `Start-Process`.

## 2. Terrain classes

`classify_terrain.py mc2_01 --out DIR` (1024^2 raster, bicubic height upsample, slope + water level + overlay handles +
colormap colour + object footprints). Output `mc2_01_class.png` (map plus legend), `mc2_01_class_id.png` (ids),
`mc2_01_legend.json`.

| id | class | what it mimics | share of map |
| --- | --- | --- | --- |
| 0 | water | open water | 76.7% |
| 1 | beach / water edge | sand, shoreline band (stair-stepped today) | 5.2% |
| 2 | cliff / rock face (slope >= 38 deg) | vertical rock; the replacement target | 1.0% |
| 3 | steep rocky slope (>= 24 deg) | scree, broken rock | 1.0% |
| 4 | road / concrete | runway, pads (overlay tiles) | 0.2% |
| 5 | structure footprint | buildings, props (96 wu radius, deliberately generous) | 7.1% |
| 6 | dirt / bare ground | trails | 0.6% |
| 7 | grass | open grass | 5.3% |
| 8 | jungle / forest floor | dark vegetation | 2.9% |
| 9 | other | unclassified | 0.0% |

Reading the map: the cliff class (2) is a thin rim, mostly the south and west coasts of the three big islands, plus a few
rock stacks. That is about 1% of the map, so a cliff regeneration is a handful of strips, not a whole-map job. Most of
the map is water, and the beach band (class 1) is where the visible stair-stepping lives.

## 3. Options, ranked

Common constraint: the sim reads the 100x100 heightmap in about 42 source files (pathing/movemap, LOS, unit and building
z-snap, water, overlays, decals); `sim_determinism.py` and `saveload_roundtrip.py` baselines key off it.

### (A) Visual-only hi-res layer (recommended first step; this is the fallback and the proven route)

Keep the sim pak untouched. Add a visual heightfield (already exists: `visual_height_4x.r32` per mission, produced by
`tools/terrain_beautify/visual_heightfield.py`) at higher resolution plus relief authored on the cliff/shore classes,
pinned to the sim surface at the class borders. Sim, saves, determinism baselines and mods: unaffected. Effort: low to
medium (the sidecar path and the beauty pipeline already exist; raising 4x to 8x/16x and the cliff relief are new).
Quality: good on shorelines and cliff faces seen from the default top-down camera; a heightfield cannot make overhangs.
Risks: terrain displacement on the live CPU regular-grid LOD-chunk path needs LOD/stitch/skirt work (high risk if the
mesh itself becomes finer); cheaper is baked normal+displacement-in-shader on the existing grid, and prop-mesh dressing.

### (B) Megascans-style cliff meshes placed as props (best visual quality per cliff)

Place real cliff rock meshes along the class-2 rim as `asset_class:building` mcassets via a packet-1 writer driven by the
analyzer's cliff mask, contours and placements (design already recon'd in the terrain_beautify arc). Local scans exist
(section 4) but the raw GLBs are 259-360 MB each: they must be decimated/retopo'd to game budget first (LODs, 2k-4k
textures). Effort: medium-high (asset prep per scan, placement tool, LOD, shadows, collision stays on the sim grid).
Quality: highest, gives overhangs and real silhouette. Risk to sim: low if the props have no footprint
(`passable`/`blocking` flags unchanged); risk to art: seams where the rock meets the terrain (needs a skirt or blend
decal); mods: unaffected unless a mod re-uses the same cliff coordinates.

### (C) Tessellation / displacement of the terrain mesh

Raises the visual mesh resolution on the GPU. Needs the LOD/stitch/skirt rework of the CPU-grid path; HIGH RISK, large
effort, and gives only heightfield relief (same limit as A). Not recommended until (A)'s shader-side displacement has been
tried.

### (D) Gaea or other offline regeneration of the heightfield

`import_gaea_height.py` + `write_clean_pak` exist. Gaea builds are available locally (section 4). Used as a generator of
the high-res visual heightfield (A) or of a new gameplay heightmap (E), driven from the exported mc2_01 height plus the
class map as masks. Effort: medium per map; quality high; risk follows (A) or (E).

### (E) Re-author the GAMEPLAY heightmap (owner steer: "almost certainly worth it")

Cost of changing the gameplay heightmap, split by route.

**E1: same grid, new values** (100x100 at 128 wu, edited heights only; keeps map extent and vertex layout).
- Re-derive: everything that is computed from heights at load: movemap/pathing cost and passability, slope-based
  blocking, LOS/visibility height tests, building and unit z-snap (objects store x,y and snap z from terrain at load, so
  they follow automatically; verify nothing stores a baked z), overlays/decals (tile flags are per-vertex and stay valid
  because the grid is the same), water elevation (constant 350 in the .fit; shorelines move because the intersection with
  350 moves, so water-edge tiles, beach flags, `shore_field.r16f` and the shoreline mask must be re-derived).
- The pak is rewritten via `write_clean_pak` (patch_pak only swaps packet 0 and inherits the template's buildings).
- Determinism/saveload baselines: the mc2_01 rows change; this needs an approved re-record (owner-approved baseline
  repin, not a silent one). All other missions unchanged.
- Old saves: a save stores unit positions plus mission state; units on a changed surface get re-snapped on load, but a
  unit standing on a cell that becomes impassable can be stranded. Mitigate by constraining new heights near existing
  unit start/trigger points, and by a load-time "nudge to nearest passable" pass; mc2_01 saves from before the change
  should be flagged incompatible unless tested.
- Mods: MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech, MCO-Carver5 ship or reuse mission data; the
  ones that overlay their own mc2_01 pak keep their own terrain (unaffected) but any mod that patches
  objects/triggers relative to stock mc2_01 geometry (Carver5-feasibility data is non-stock) may mis-place against new
  heights. Needs a per-mod check of which ones load stock mc2_01 terrain.
- Editor round trip: the editor reads and writes the same pak; E1 imports cleanly, but cannot yet author the result.
- Effort: medium (Gaea/regen script plus re-derivation checks plus baseline re-record). Quality: the shoreline steps and
  cliff faces are limited by 128 wu cells no matter how the 10000 values are changed, so E1 only helps silhouette-scale
  shapes. It is the cheap, honest version.

**E2: higher-resolution gameplay grid** (for example 200x200 at 64 wu, or 400x400 at 32 wu).
- Everything in E1, plus: the vertex count change touches the terrain pak layout, the quad/tile/overlay indexing, movemap
  (cell size is tied to the 128 wu vertex/tile pitch), LOD chunking, SSBO layout, minimap, fog-of-war/visibility maps,
  overlay and decal tiles (a tile is a 128 wu cell today), save compatibility (any save with terrain-indexed data), and
  the editor. This is an engine-wide change touching most of the 42 height readers; it is not mission-local. Effort: very
  high; determinism baselines for every mission that shares the code path may change unless gated per mission.
- Not recommended until E1 and (A)/(B) show the visible gain is worth it.

### Recommendation

1. Now: (A)+(B) as the delivery route (visual-only high-res relief plus real cliff props on class 2). It fixes the cause
   the owner sees (shoreline steps, ugly cliff faces) without touching the sim, saves or mods.
2. In parallel, cheap and reversible: E1 on mc2_01 only when a specific gameplay-shape reason exists (for example a cliff
   silhouette that the grid cannot hold) and with the baseline re-record budgeted; do not start with E2.
3. Visual-only (A) stays the fallback if E1 hits a mod or save compatibility problem.

Owner decisions needed: (1) approve (A)+(B) as the route; (2) whether E1 on mc2_01 is wanted now and who approves the mc2_01
baseline re-record; (3) which scans are allowed into the game package (licence: Megascans via Fab is free for UE use
only, check before shipping; PolyHaven is CC0 and safe); (4) whether old mc2_01 saves may be invalidated.

## 4. Local assets found

| source | path | notes |
| --- | --- | --- |
| Megascans-class cliff scans | `GameAsset/Terrain/*_raw.glb` | Icelandic lava (259 MB), Nordic coastal ulujfanga (271 MB), massive Nordic coastal veqnci0db (360 MB); too heavy raw, need decimation/LOD |
| marble cliff | `marble_cliff_01_2k.gltf.zip`, `marble_cliff_01_disp_4k.png` | PolyHaven-style, CC0, already used for the mat5 cliff slot |
| shoreline | `sandshoreline.glb` | candidate for the beach strip |
| PolyHaven-style packs | aerial_beach_01, aerial_grass_rock, rocks_ground_02/03/05/06, dirt_aerial_03, forest_ground_04, dry_riverbed_rock, rocky_trail, gravel_road, concrete_*, snow_field, moon_macro, Grass001, Road004 | .blend or .gltf zips; material sets for terrain classes |
| Gaea | `C:/Users/Joe/Documents/Gaea/` (Builds/MC2_test, Builds/test, Projects, Presets); Gaea 2 under Program Files/QuadSpinner; installer in Downloads | terrain generator; import path exists |
| Epic | `G:/Epic Games/UE_5.7/Engine/Plugins/Bridge`, `MegascansPlugin` | plugins only, no Quixel library on disc |
| Not found | Quixel library folders, `G:/Gaea`, `G:/Terrain` | |

## 5. Way back into the game

- Heightfield relief (A): new visual heightfield in the beauty sidecar (`mc2_01.beauty/`, loaded by the existing sidecar
  path; the mod VFS lets a mod ship its own). Renderer reads it through the SSBO vertex Z (binding 23) on the existing
  LOD-chunk grid, so the first step needs no new renderer path (higher-res visual height, then shader displacement).
- Cliff meshes (B): `asset_class:building` mcassets plus a packet-1 writer (objects), placed from the analyzer mask.
  Beauty sidecars carry the overlay/shoreline masks (`overlay_v2.png`, `shoreline_mask.png`) and the blend decals at the
  rock/terrain seam.
- Editor: it does not support authoring either yet; short-term the workflow is the offline tools (export, classify,
  Gaea/Blender, import) and a sidecar the editor leaves alone.

## 6. Prototype

See the images in the proofs dir (`blender_plain.png`, `blender_normalmap.png`, `proto_original.png`, `proto_fresh.png`).
`prototype_cliff.py` builds a visual-only patch (about 1800 wu square, 24 verts per sim cell, 113k verts) on the south tip
of the north island: base = smooth bicubic of the sim heights blended to the exact triangle surface at the border, relief =
ridged fbm plus strata weighted by a slope-derived cliff mask that is 0 on the border (silhouette and seam unchanged), and a
baked albedo that blends the colormap with a procedural rock colour. It is a stand-in for the Megascans mesh (B): it
shows the pipeline and the silhouette-pinned approach, not final art quality.
