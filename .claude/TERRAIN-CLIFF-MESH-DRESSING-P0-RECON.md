# TERRAIN-CLIFF-MESH-DRESSING-P0 — RECON

**Slice:** TERRAIN-CLIFF-MESH-DRESSING-P0 (visual dressing only, no C++ if possible)
**Worktree:** A:/Games/mc2-controlmap-sample-1 (branch claude/controlmap-sample-1, base 85182be1) — read-only; another agent edits terrain_lod_chunk.frag/terrain.cpp (unrelated).
**Date:** 2026-07-01

## Executive summary

Goal chain: cliff GLB → cooked building-class asset → hand-placed in editor → renders grounded on a stock/Gaea map, breaking up smooth heightfield slope.

The proven, **C++-free** path is the existing **building `[Import] Source=` GLB-override mechanism**, exactly as `data/tgl/quonset.ini` already does for the Quonset building (proof it works today). A cliff mesh authored as a building appearance (`.ini` with `[Import] Source="…GLB"`, LOD `FileName0`, `ShadowName`, `[Bounds]`) loads through `TG_TypeShape::InitFromImportedMesh` (mclib/tgl.cpp:1640), registers as a static prop, and is placed via the editor's existing building brush. **No new object *class* is needed** — it rides the BUILDING type slot.

**CRITICAL FACT CORRECTION.** The prompt's "known fact" that `cook_cliff_material.py` cooks the GLB → building mcasset is **WRONG**. `tools/terrain_beautify/cook_cliff_material.py` (read in full) cooks the cliff GLB's *normal + ARM textures* into `mat5_normal.tga`, a **terrain triplanar material-splat layer** (MAT_LAYER_MARBLE_CLIFF, slot 5). It never emits a mesh or an mcasset. It is a **different, parallel dressing approach** (paint cliff *material* onto the heightfield), not the mesh-dressing path this slice wants. The GLB→mesh sidecar tool is `tools/ase_to_glb.py` (`--class building`), but it ingests **ASE**, not GLB directly (see Open Questions).

**No prior "GEOMETRY-DRESSING" recon doc exists in this worktree** (grepped `.claude/` + `docs/` for GEOMETRY-DRESSING / cliff mesh / mesh.dressing — no hits). Its conclusions are recorded only in the prompt; treat as unverified. The building `Source=` path IS confirmed here independently, which matches its "author cliff GLB as asset_class:building, no C++" conclusion in spirit.

## Cook chain (GLB → placeable building)

Two candidate chains; P0 recommends **Chain A (Source=ini override, no C++)**.

**Chain A — building `[Import] Source=` override (PROVEN, no C++):**
1. Convert `marble_cliff_01_2k.gltf.zip` → an engine-loadable GLB registered under an import key (e.g. `"MarbleCliffGLB"`). Model after `data/tgl/quonset.ini` `[Import] st Source = "QuonsetGLB"`. The import key resolves through the Assimp import registry (mclib/assimp_importer.cpp) consumed by `TG_TypeShape::InitFromImportedMesh` (mclib/tgl.cpp:1640).
2. Author `data/tgl/marblecliff.ini` (copy quonset.ini structure): `[Import] Source=`, `[Bounds]` (footprint half-extents — sets editor selection box, keep modest), `[TGLData] FileName0`/`Distance0`, optional `FileName1`/`Distance1` LOD, `ShadowName` (shadow-proxy mesh; can point at the same/simplified mesh).
3. No texture-normal cook needed for the mesh path — the GLB carries its own PBR textures via the imported-mesh material map. (The `mat5_normal.tga` cliff-material cook is orthogonal terrain-splat dressing, usable *in addition* but NOT required here.)

**Chain B — ase_to_glb sidecar (heavier, ASE-gated):** `tools/ase_to_glb.py --ase … --class building --out-glb … --out-sidecar X.mcasset.json` emits GLB + `.mcasset.json` (schema_version 1.0; fields `asset_class`, `lods`, `shadow_mesh`, `hardpoints`, `collision_bounds{minBox,maxBox}`, `material_map`, `validation_baseline`) — ase_to_glb.py:865-894. But the engine sidecar-consumer is not yet wired for buildings (bdactor.cpp:3939 note: "Full MeshCapability bits come once the engine reads the cooked manifest.json (later integration)"). **Prefer Chain A for P0.**

## Registration (so editor can place it)

Editor building catalog is loaded by `EditorObjectMgr::init(bldgListFileName, objectFileName)` (editor/EditorObjectMgr.cpp:365):
- **`bldgListFileName`** — a line-based text list; each line: GroupName, nameID (string-table id), **TYPE** (`BUILDING`/`TREE`/`VEHICLE`/else→MECH — line 435-442), **fitID** (packet index). Add one line: `… BUILDING <fitID>` for the cliff.
- **`objectFileName`** — PacketFile (`.pak`); packet `fitID` holds a FIT block read at init (lines 448-472): `LowTemplate`/`HighTemplate` (→ `impassability` mask), `BlocksLineOfFire` (bool), `[ObjectClass] ObjectTypeNum`, `[General] HoverCraft`, `[ObjectType] Name`. Add a packet whose FIT points the appearance at `marblecliff` and sets `impassability=0` + `BlocksLineOfFire=false` for pass-through dressing.

Minimal registration steps: (a) add the `data/tgl/*.ini` appearance, (b) add a packet to the objects `.pak` with a fresh fitID + a distinct `ObjectTypeNum`, (c) add one line to the building-list file. All are **data edits, no C++**.

## Grounding / orientation

- Buildings ground to a **SINGLE terrain sample** at the object origin: `xlatPosition.y = TerrainRuntime::groundElevation(position)` (bdactor.cpp:3969, also :1306, :6747; runtime update bldng.cpp:829). `groundElevation` = `land->getTerrainElevation` bilinear sample (terrain_runtime.cpp:28-40).
- **Consequence:** a wide cliff mesh on a steep slope pins its origin to one height and applies only a **yaw** rotation (`EulerAngles(0, yaw, 0)`, bdactor.cpp:3966) — **no pitch/roll to match slope**. On steep slopes the far edges of a large mesh will **float above / clip through** the heightfield. Mitigations, all data-side: (1) keep meshes small relative to slope curvature and hand-tune per-instance position/yaw; (2) author the GLB with its base *below* origin (self-sinking) so edges bury into terrain (hides float, accepts clip); (3) place along slope breaks where the tilt mismatch reads as intentional rock. Per-instance pitch/roll would need C++ — out of scope for P0.

## Shadow & lighting (CSM)

- Building appearances carry a `ShadowName` shadow-proxy mesh (quonset.ini `ShadowName="QuonsetX"`); the cliff ini should supply one (same or simplified mesh) so it casts.
- Placed buildings register into the GPU static-prop substrate (`registerStatic`, bdactor.cpp:3939+) which feeds the CSM/shadow path used by all static props — a large cliff will cast and receive like any building. **Landmine:** static-prop registration **aborts on the first non-SHAPE_NODE child** unless helpers are skipped (bdactor.cpp fix at ~:3990, "skip non-SHAPE_NODE children"); a GLB with helper/empty nodes must import clean or registration silently drops the whole prop.

## LOD / cull / perf

- Ini supports 2 LODs (`FileName0/Distance0`, `FileName1/Distance1`) — provide a low-poly `…L1` for distance.
- Static props flow through GPU compute-cull → indirect multidraw (bdactor.cpp:621, gos_static_prop_batcher.cpp). Frustum-culled normally; `MC2_SNAP_CULL` is an optional extra cull gate (default OFF). N cliff meshes cost N instance records in the substrate — cheap per-instance; keep triangle count and LOD sane. No evidence of a "distant props never distance-culled" hard rule for buildings — they honor `Distance` LOD switch, so distant instances get the cheap LOD.

## Collision / pathing

- **asset_class:building does NOT universally block movement.** Impassability is marked only for `GENERIC_DESTRUCTIBLE_RESOURCE_BUILDING_OBJNUM` via `appearance->markMoveMap(false,…)` (bldng.cpp:796-799). Gates/walls/goal-objects mark move-map explicitly (gate.cpp:766, goal.cpp:450). A generic building whose FIT sets `impassability=0` and `BlocksLineOfFire=false` and a benign `ObjectTypeNum` is **pass-through visual dressing** — no pathing/collision change. This is the desired P0 config.

## Placement authoring

- **P0 = hand-place only.** Editor building brush → select cliff from catalog group → click on map. Persisted into mission `.pak` packet-1 (terrain objects). Editor pre-warms textures on place via `Bldg_ForceRenderShapeTexturesResident` (editor/EditorObjectMgr.cpp:2427/2457 → mclib/bdactor.cpp:747) — the known invisible-placed-building fix (c628646b); confirm it fires for the new type.
- **Later (P1) = mask→auto-place:** analyzer reads cliff-mask/contour from the heightfield, emits placements into a mission `.pak` packet-1 via a ~40-line writer extending `write_clean_pak`; veg-suppress under cliffs via overlay MASK (not runtime skip). Pure-Python, out of P0 scope.

## Files / tools to touch (P0, data-only)

- `data/tgl/marblecliff.ini` (new; model on `data/tgl/quonset.ini`).
- Building-list text file + objects `.pak` (add line + packet; names TBD — find the concrete paths passed to `EditorObjectMgr::init`).
- GLB import registration key wiring (how `"QuonsetGLB"` resolves — see Open Questions).
- `tools/terrain_beautify/cook_cliff_material.py` — only if *also* doing terrain-splat dressing (optional, orthogonal).

## Landmines

1. **`cook_cliff_material.py` is the WRONG tool for mesh dressing** — it makes a terrain normal texture, not a mesh/mcasset. Do not build the slice on it.
2. **Single-point grounding + yaw-only** → large cliff floats/clips on steep slopes; only data-side mitigations available in P0.
3. **Static-prop registration aborts on first non-SHAPE_NODE (helper) child** — GLB must import with clean SHAPE nodes or the prop silently vanishes.
4. **Invisible-placed-building prewarm** (`Bldg_ForceRenderShapeTexturesResident`) — verify it fires for the new type or the cliff renders invisible until it enters normal residency.
5. No prior GEOMETRY-DRESSING recon in-repo — its claimed conclusions are unverified.

## Acceptance (P0 done)

Editor: cliff appears in building catalog, places on a stock/Gaea map with a visible selection box. Save mission. Game load: cliff renders **grounded** (origin flush to terrain), casts a shadow (CSM), breaks up the smooth slope visually, and does **not** block a mover walking near it (impassability=0). Proof = static-cam screenshot in-game showing a grounded, shadowed cliff on the slope; smoke tier1 unaffected (data-only).

## Open questions (need user ruling)

1. **GLB import registration:** how does the `[Import] Source="QuonsetGLB"` key resolve a GLB to `InitFromImportedMesh`? Is there a manifest/registry that must list `"MarbleCliffGLB"`, and does adding an entry require a C++ or data change? (Determines whether P0 is truly C++-free.)
2. **GLB vs ASE:** `ase_to_glb.py` ingests ASE, not GLB. Is there a GLB-normalize step (e.g. a `normalize_broadleaf_glb.py`-style pass) for the marble cliff, or does the Source= path bypass sidecar cooking entirely?
3. **Concrete file paths** for the editor building-list `.lst`/text and the objects `.pak` passed to `EditorObjectMgr::init` — needed to write the registration edits.
4. Confirm terrain-splat cliff-material (`mat5_normal.tga`) is out of scope for P0 (mesh-only), or bundled as combined dressing.

## Follow-up recon answers

**1. Source= resolution — NO registry, VERDICT: P0 IS C++-free (YES).**
`Source="QuonsetGLB"` is read at `mclib/bdactor.cpp:772-774` (`BldgAppearanceType::init`):
```
if (iniFile.seekBlock("Import") == NO_ERR &&
    iniFile.readIdString("Source", importSourceBase, 255) == NO_ERR &&
    importSourceBase[0])
{
    char* dot = strrchr(importSourceBase, '.');
    if (dot) *dot = '\0';           // strip extension if present
}
```
`importSourceBase` ("QuonsetGLB") is then passed **directly** as a basename to `loadBuildingImportSourceWithSidecarAxis(shape, importSourceBase)` (bdactor.cpp:307-341, called at :862-863/:886-887), which calls `shape->LoadFromFile(importSourceBase)` (bdactor.cpp:335 → `TG_TypeMultiShape::LoadFromFile`, mclib/msl.cpp:444-493). `LoadFromFile` probes `{tglPath}/QuonsetGLB.glb` then `.glb`/`.fbx` on disk (msl.cpp:462-480, `FullPathFileName::init(tglPath, baseName, ext)`) and calls `ImportGeometryFromFile` (assimp_importer.cpp) directly — no lookup table, no manifest, no C++ enum/switch keyed by name. **The string "QuonsetGLB" is literally a filename stem, resolved purely by filesystem probe.** `TG_TypeShape::InitFromImportedMesh` (tgl.cpp:1640) is populated by the importer as a downstream step, not part of the name-resolution chain.
Two unrelated name-keyed things exist and are NOT required: (a) `mclib/model_override_registry.cpp` — a separate JSON-driven "replace stock appearance with a GLB" system keyed `"<class>:<appearanceName>"` reading `data/model_overrides/models.json`; irrelevant to authoring a brand-new appearance via its own `.ini`. (b) The `.mcasset.json` sidecar (`bdactor.cpp:307-341`, `extractBuildingPbrAxis`/`extractBuildingPbrYaw`) is **optional metadata** (axis/yaw hints consumed via env vars during import) gated by `buildingPbrGateEnabled()` (`getenv("MC2_BUILDING_PBR")`, bdactor.cpp:160-162) — if absent, `LoadFromFile` still runs with defaults. **Conclusion: dropping `data/tgl/marblecliff.ini` + `data/tgl/MarbleCliff.glb` (or whatever basename it names) is sufficient. No table anywhere needs a new ini name added.**

**2. Editor building registration — files/formats confirmed, but no caller of `EditorObjectMgr::init(...)` with concrete literal filenames was found in this worktree slice** (grepped for `objectMgr.init`, `EditorObjectMgr::instance()->init`, `.lst`/`BUILDING_LIST`/`objects.pak` call sites — `editor/EditorInterface.h:431` declares the `objectMgr` member but no `.init(...)` call site with the two path literals is present in the read files; likely constructed in a TU not grepped, e.g. resource-driven from an .exe-relative config or `EditorInterface.cpp` init chain not fully traced — **treat file paths as UNCONFIRMED, follow-up needed**). What IS confirmed from `editor/EditorObjectMgr.cpp:365-477` (unchanged from prior recon):
  - `bldgListFileName`: line-based text list, one line per object: `FileName GroupNum NameStrId TYPE fitID [specialType...]` (`ExtractNextString` calls at :407/411/424/433/445). `TYPE` string must be exactly `"BUILDING"` (case-insensitive, :435) else it falls through `"TREE"`/`"VEHICLE"`/else-MECH.
  - `objectFileName`: a `PacketFile` (`.pak`) — `objectFile.seekPacket(bldg.fitID)` (:448) then `FitIniFile bldgFile.open(&objectFile, fileSize)` (:454) reads `LowTemplate`/`HighTemplate` → 64-bit `impassability` bitmask (:456-460), `BlocksLineOfFire` (:461), `[ObjectClass] ObjectTypeNum` (:462-463), `[General] HoverCraft` (:464-466), `[ObjectType] Name` (:467-470). A likely candidate for the objects pak's name is `objects.pak`, referenced in comment `code/unitprofile_fit.cpp:25` ("Repacking objects.pak in-session is infeasible") — this is corroborating but not a direct proof of the exact deployed path/arg; **needs one more grep pass over `code/` init sequence or the deployed `data/` tree to nail down the literal path and an existing minimal BUILDING-class example packet to copy** (prior recon's "cite an existing minimal example object" item is still open — none located in the files read this pass).
  - fitID collision: `seekPacket(bldg.fitID)` + `gosASSERT(false)` on failure (:448-449) means fitID **must** already exist in the `.pak` (packet indices are pre-allocated, not free-form) — a new BUILDING entry needs a **new packet appended to the `.pak`** with a fresh index, not just a `.lst` line pointing at a stray number. This is a **data-pack edit**, still C++-free, but requires a `.pak`-writing tool (e.g. `write_clean_pak`-style, already referenced in this doc's P1 section) rather than a hand-text-edit.

**3. GLB handling — confirmed no cook step for Source= buildings; texture conventions unconfirmed in detail.**
`LoadFromFile` → `ImportGeometryFromFile` (assimp_importer.cpp) consumes the GLB **directly at runtime via Assimp** — no `ase_to_glb.py` / cook step in this path (msl.cpp:451-486, `ENABLE_ASSIMP_IMPORTER` compile gate + `MC2_ASSIMP_IMPORT` runtime killswitch, default probes `.glb` then `.fbx`). `ase_to_glb.py --class building` (referenced in prior recon, ase_to_glb.py:865-894) is the **ASE→GLB sidecar cooker for a different, heavier pipeline** (emits `.mcasset.json` with `asset_class`/`lods`/`shadow_mesh`/`hardpoints`/`collision_bounds`/`material_map`) — for Source=-style buildings, the `.mcasset.json` sidecar is **optional and only used for axis/yaw hints** (`extractBuildingPbrAxis`/`extractBuildingPbrYaw`, bdactor.cpp:268-341), gated behind `MC2_BUILDING_PBR` env (bdactor.cpp:160-162, :897). If `MC2_BUILDING_PBR` is unset/0 and no sidecar exists, `LoadFromFile` still runs with Assimp defaults — **the GLB can be dropped with zero sidecar and zero env gate for a first-pass P0**. Exact embedded-texture vs external-texture conventions for Assimp's GLB import were **not traced in this pass** (would require reading `assimp_importer.cpp`'s material-mapping code, not opened here) — flagged as an open item.

**4. Quonset ini minimal contents — already fully itemized in prior recon; RE-CONFIRMED, nothing to add.**
`data/tgl/quonset.ini` (read again this pass, full text): `[Import] Source="QuonsetGLB"`, `[Bounds]` 4 floats (footprint box), `[TGLData] FileName0="Quonset"`/`Distance0=0.0`, `FileName1="QuonsetL1"`/`Distance1=1000.0` (LOD1), `ShadowName="QuonsetX"`, plus optional night-light color fields (`HotPinkRGB` etc., defaulted if omitted per `bdactor.cpp:784-819` read pattern — every field has a `if (result != NO_ERR) default=...` fallback). **If `ShadowName` is omitted**, `BldgAppearanceType::init`'s later `readIdString` for it will fail and fall back to a default (not traced to its exact default this pass, but the pattern at :784-819 for every other optional field is "log nothing, silently default" — consistent with prior recon's claim). **If `[Bounds]` is omitted**, editor selection-box sizing would use zeroed/garbage floats since no fallback branch was seen guarding that specific read in the excerpt reviewed — recommend always supplying `[Bounds]` explicitly for marblecliff.ini to avoid an unverified zero-box footprint bug.

## Install record

**Executed against `A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0` on 2026-07-01.**

- **FitID chosen: 1188** (next free packet index — `object2.pak` had 1188 packets, 0..1187, before install; highest FitID referenced from any `Buildings.csv` line was 940, so 1188 is unused by construction).
- **Names:** appearance ini `data/tgl/marblecliff.ini` (`AppearanceName="marblecliff"`), FIT `[ObjectType] Name="MarbleCliff"`, `Buildings.csv` `File Name="MarbleCliff"`, `Group ID=11` (same decorative group as Quonset), `NameID=30434` (reused `ExtractionMarker`'s existing string-table id — no new localization entry needed).
- **GLB import key:** `Source="MarbleCliffGLB"` → probes `data/tgl/MarbleCliffGLB.glb` (confirmed via msl.cpp `LoadFromFile`/`FullPathFileName::init(tglPath, baseName, ext)`, no registry). `[TGLData] FileName0="MarbleCliff"` is a required-but-functionally-ignored placeholder for LOD0 when `Source=` is set (bdactor.cpp:862-863: `if (i==0 && importSourceBase[0]) loadBuildingImportSourceWithSidecarAxis(...)` — the `aseFileName`/`FileName0` value is never used in that branch). Single LOD only for P0 (no LOD1/Distance1).
- **Appearance-field finding (Appearance=0x02000272 in the prompt's example FIT template):** NOT present in this engine's actual FIT schema — that field does not appear anywhere in `mclib/bdactor.cpp`, `code/objtype.cpp`, or the real packet-335 (Quonset) / packet-289 (ExtractionMarker) dumps. The prompt's template was generic/hypothetical. The real, verified minimal BUILDING FIT (dumped from `object2.pak` packet 289, "ExtractionMarker") uses `[ObjectClass] ObjectTypeNum=1`, `[ObjectType] Name/AppearanceName/ExplosionObject/DestroyedObject/ExtentRadius`, and a `[BuildingData]` block with `SetImpassable`, `blockLineOfFire`, `LowTemplate`/`HighTemplate`, etc. — no separate "Appearance" bitfield exists; the ini-file lookup is purely by `AppearanceName` string → `data/tgl/<name>.ini` filename stem. The MarbleCliff FIT packet models packet 289 with `SetImpassable=0`, `blockLineOfFire=FALSE`, `LowTemplate=0`, `HighTemplate=0` for pass-through dressing, per the acceptance bar.
- **ShadowName** in `marblecliff.ini` is present (`ShadowName="MarbleCliff"`) for consistency with `quonset.ini`'s convention, but the field is **not read anywhere in this build** (`grep` for "ShadowName" across `mclib/bdactor.cpp` found zero hits) — the CSM shadow pass reuses the same loaded `bldgShape` geometry directly (`BldgAppearance::renderShadows`, bdactor.cpp:2610). No shadow-proxy mesh is required or consumed; harmless no-op field.
- **GLB build:** source `C:/Users/Joe/Downloads/GameAsset/Terrain/marble_cliff_01_2k.gltf.zip` (gltf + .bin + 3 external JPGs, single mesh `sphere_gltf`, single material `marble_cliff_01`, identity node TRS). Converted to a single self-contained GLB (`data/tgl/MarbleCliffGLB.glb`, 10.6 MB) via a one-off scratch script: embedded all 3 textures + the .bin buffer into one GLB (matches how `QuonsetGLB.glb`/`HangarGLB.glb` ship — no external file deps), renamed the baseColor image to the material name (`marble_cliff_01`) per the existing `normalize_broadleaf_glb.py` convention (DeriveMC2TextureName is image-filename-based; cook/runtime binding expects image stem == material name). Verified via `pygltflib.GLTF2().load(...)` — parses cleanly, 1 mesh / 3 images / 1 material / binary blob length matches.
- **Grounding sink:** mesh Y range was `[-2.2127, 2.1968]` (height 4.41 units) around origin; sunk down by 15% of height (0.6614 units) so post-sink range is `[-2.8742, 1.5354]` — base now sits below Y=0, per the mitigation in this doc's "Grounding / orientation" section (single-point + yaw-only grounding; self-sinking hides float/clip on slopes).
- **`[Bounds]`:** `UpperLeftX/Y=-220`, `LowerRightX/Y=220` (mesh footprint is roughly ±2.2 units at its native scale before any in-editor placement scale is applied by the mesh/importer's own unit conversion — sized generously to give the editor a visible, clickable selection box; not a physics/collision extent).
- **Backups made (originals untouched, `.bak_cliffp0` suffix):**
  - `data/objects/object2.pak.bak_cliffp0` (457,438 bytes, pre-install)
  - `data/art/Buildings.csv.bak_cliffp0`
  - `data/art/gui/Buildings.csv.bak_cliffp0`
- **Verification performed:** round-tripped `object2.pak` — all 1188 original packets are byte-identical pre/post append (`pak_dump.decode_packet` compared per-packet); new packet 1188 decodes back to the exact FIT text written. `tools/test_pak_append.py` (4 tests, includes a synthetic-pak round trip + a real-object2.pak-copy round trip) — all pass.
- **Install tooling:** `tools/pak_append.py` (generic PacketFile append/replace, pytest-covered) + `tools/install_cliff_dressing.py` (one-shot installer wrapping pak_append + CSV append + asset copy, idempotent backups) + `tools/marblecliff_fit_template.txt` (the FIT text installed).
- **Revert:** copy each `*.bak_cliffp0` back over its original (`object2.pak`, `Buildings.csv`, `gui/Buildings.csv`), then delete `data/tgl/marblecliff.ini` and `data/tgl/MarbleCliffGLB.glb` from the deploy tree.
- **Not done (per instructions):** no C++ build, no editor/game launch, no in-engine screenshot proof. Manual verification is left to the user (steps below).

### Manual test steps for the user

1. Launch the editor (`EditRel.exe`) against the 0.5-testing deploy.
2. Open the building catalog / object placement panel; find group 11 (Quonset's group) — "MarbleCliff" should appear alongside Quonset/Quonset2/QuonsetClean.
3. Select MarbleCliff, click on a stock or Gaea-generated map, ideally on a sloped area near a cliff edge.
4. Expected: a selection box appears sized ~440x440 (the `[Bounds]` footprint); the cliff mesh renders grounded (bottom buried into terrain, no floating gap at the placement point) with its own PBR textures (marble diffuse/normal/roughness — not a bright pink/missing-texture placeholder).
5. Save the mission, reload it in-game (`mc2.exe`) if desired: the prop should still render, cast a shadow under CSM, and a mover unit should be able to walk through/near it without being blocked (impassability=0).
6. On steep slopes, expect the far edge(s) of the mesh to visibly float or clip through the heightfield (this is the known, documented P0 limitation — single-point + yaw-only grounding; no pitch/roll match). This is expected, not a bug.

### Open risks

- GLB texture-name-to-material-binding convention was validated by structural analogy (matches `normalize_broadleaf_glb.py`'s documented fix for a *different* asset class/tool) but **not proven against this exact cliff mesh through the actual Assimp import path** (no C++ build/launch performed here) — if the diffuse texture doesn't bind, the cliff may render with a fallback/NULLTXM material. Verify visually per step 4 above.
- `Bounds` sizing (±220) is a rough guess for a comfortable editor selection box; may need adjustment once seen in the editor next to the actual imported mesh scale.
- No `.mcasset.json` sidecar was authored (deliberately, per the follow-up recon answers — it's optional and only used for axis/yaw hints under `MC2_BUILDING_PBR`); if the mesh imports with wrong orientation/scale, a sidecar with `axis_mapping`/`yaw_degrees` (or the `MC2_GLTF_AXIS`/`MC2_GLTF_YAW_DEG` env vars directly) is the documented fix path — no C++ needed.
- Static-prop registration's "abort on first non-SHAPE_NODE child" landmine (bdactor.cpp, per this doc's Landmines section) was not re-verified against the new GLB's node graph beyond confirming it has exactly one node with a mesh and no other child nodes (checked via pygltflib dump above) — should be safe, but only a live editor placement test proves it definitively.

**5. Grounding sink — CONFIRMED C++, no data field exists.**
`xlatPosition.y = TerrainRuntime::groundElevation(position);` appears verbatim at **bdactor.cpp:1306, :3969, :6747** — a single bilinear terrain height sample, with **no added/subtracted offset term anywhere in the assignment or surrounding lines** (read bdactor.cpp:3958-3972 in full this pass). Buildings get `rot = Stuff::EulerAngles(0.0f, yaw, 0.0f)` (bdactor.cpp:3966) — **yaw-only, hardcoded pitch/roll=0**, confirming prior recon. (Trees, by contrast, get `EulerAngles(pitchAngle, yawAngle, 0.0f)` at :6744 — trees CAN tilt, buildings CANNOT — an asymmetry worth noting if a future slice wants tilt-matching for the cliff without going through the BUILDING class.) **No FIT/ini sink or z-offset field was found anywhere in `BldgAppearanceType::init` or the grounding call sites** — burying the cliff base requires either (a) authoring the GLB mesh itself with its origin above its lowest visible vertex (self-sinking via mesh authoring, data-only) or (b) a new C++ field, out of P0 scope. This matches the prior recon's mitigation (2).
