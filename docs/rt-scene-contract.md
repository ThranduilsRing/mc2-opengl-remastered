# RT scene contract

The ray-traced scene (`GameOS/gameos/vk_rt_scene.h/.cpp`) is one TLAS per frame that ray-query consumers bind. This page is the contract they rely on.
The code comments in `vk_rt_scene.h` are the reference; this page summarises them.

## TLAS contents

| Kind | BLAS | Instances | Mask |
|---|---|---|---|
| Terrain | one BLAS per 16x16-cell tile (coarse grid or the 4x fine grid, same diagonal rule as the raster), one geometry each | one per tile, identity transform | static |
| Static props | one BLAS per prop type, **one geometry per non-empty packet**, over the shared colour-prop VBO (40-byte vertices, position at +0, uv at +24) | one per registry instance whose raster object ID fits 24 bits | static |
| Movers (mechs, vehicles) | refit BLAS per mover, rebuilt every frame | one per live mover | mover |

Tree sway is not applied. Props without a recipe and IDs that do not fit are dropped and counted (`dropped=`), never aliased.

## IDs

- `instanceCustomIndex` is the raster **object ID**, the same value the object-ID guide writes. Terrain is `kTerrainObjectId`. Movers use the reserved mech range.
- `instanceShaderBindingTableRecordOffset` holds the instance's **first material row**. No SBT is bound (ray query only), so the field is free.
- A hit's material row = SBT record offset + geometry index.

## Material lookup

`MaterialRow` (32 bytes, std430 twin in `shaders/vulkan/rt_scene_probe.comp`): `albedoLayer, flags, alphaCut, materialId, uvScaleX, uvScaleY, firstIndex, packet`.

| Rows | Content |
|---|---|
| 0 | movers (`kMatMover`, untextured) |
| 1 .. 1+P-1 | prop rows, type by type in build order, packet order inside a type. `albedoLayer` = the colour-prop island's layer for the packet; `materialId` = the raster `PropMaterialGpu` index; `packet` = the global packet |
| 1+P .. | one row per terrain tile, row = `terrainRowBase(P) + tile ID`; `materialId` = `packet` = the tile ID (`kMatTerrain`) |

Flags: `kMatAlphaOn` (layer indexes the alpha-ON array, else alpha-OFF), `kMatCutout`, `kMatTerrain`, `kMatMover`, `kMatProp`.

The prop uv of a hit: `uvIndex[firstIndex + 3*prim + k]` gives the three VBO vertices; interpolate their uv at +24 with the hit barycentrics, then `fract(uv) * uvScale` on the row's layer at mip 0 (the raster's crop and cut, `color_prop_tier1.frag`).

The material table and the uv-index buffer are published with the TLAS and retire with it (frame-fenced). Prop rows are only appended, so a held mover TLAS keeps valid rows.

## Flags and alpha cutouts

- A packet is a **cutout** when the raster cuts it: an alpha-ON packet always, an alpha-OFF packet when it is alpha-tested and textured (`packetIsCutout`). Vegetation and fences are cutouts.
- Cutout geometries are built without `VK_GEOMETRY_OPAQUE_BIT_KHR`. Every other geometry (terrain, opaque props, movers) is OPAQUE.
- A consumer must run the any-hit equivalent in its `rayQueryProceedEXT` loop: for a candidate triangle, commit it unless its row has `kMatCutout` and the albedo alpha at the hit is below `alphaCut` (0.5). `rt_scene_probe.comp` is the reference loop.
- Shadow-style consumers may use `gl_RayFlagsOpaqueEXT` only if they accept that leaves block like solid quads.
- A prop type built before the colour-prop material feed is ready (bounded wait, `kMatWaitCalls`) is untextured and opaque; the ledger line says `why=props_untextured`.

## Budgets and ledger

Every publish logs one `[VK_RT_SCENE] publish ...` line: TLAS instances, terrain tiles, props, types, dropped, BLAS / compacted / TLAS / scratch / instance bytes, `material_bytes` (table + uv indices), `material_rows`, `cutout_rows`, `total_bytes`, build calls and build ms. `total_bytes` includes `material_bytes`. Static BLAS are compacted when that saves at least 10%.

## Selftest

`MC2_VK_RT_SELFTEST=1` runs two probes after each publish:

- `probe result props=...`: the object-ID probe, a ray straight down at each probed instance origin. It reports `occluded_by` / `behind_found` on a miss. A ray at the origin can miss the mesh (hollow or offset models) or hit another object first, so an occasional miss there is a probe-geometry limit, not a TLAS fault.
- `material result={PASS|FAIL} props= vegetation= terrain= holes= false_holes= opaque_diffs= rows= rays=`: rays aimed along the normal of each type's largest opaque and cutout triangle, upward rays under terrain tiles, and a 16x16 barycentric grid over vegetation triangles traced alpha-tested and forced opaque. PASS needs every probe to report the expected instance, row, primitive, layer and packet, at least one hole, and no difference on non-cutout rows. `scripts/rt_scene_probe_check.py` parses the line.

## AMD rules

- No SBT is bound: ray query only. The SBT offset field carries the material row and must fit 24 bits (asserted).
- Compaction is forbidden with `ALLOW_UPDATE` on the AMD proprietary driver (mover BLAS are refit, never compacted).
- Non-opaque geometry costs candidate-loop iterations on RDNA: keep cutouts to packets the raster really cuts.
- Bind the alpha arrays with a NEAREST / CLAMP sampler at mip 0 in the probe so the result does not depend on filtering.
