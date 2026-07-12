# Asset Modernization Pipeline — Design + Phase Plan

Generated from recon in `docs/asset-modernization-recon.md` and
`docs/animated-prop-cook-recon.md`. Canonical reference for Phase 0–1
implementation on branch `claude/asset-mod-pipeline-v0`.

---

## Scope

Extend the existing mech GLB import path to all non-mech object classes
(buildings, props, vehicles, trees), add offline ASE→GLB conversion tooling,
and define the sidecar JSON schema that accompanies every cooked GLB.

---

## Sidecar JSON schema (`.mcasset.json`)

Every GLB produced by the pipeline ships with a peer sidecar file:

```
data/tgl/AssetName.glb
data/tgl/AssetName.mcasset.json
```

Schema:

```json
{
  "schema_version": 1,
  "asset_name": "AssetName",
  "asset_class": "building|prop|vehicle|tree|mech",
  "source_ase": "path/to/AssetName.ase",
  "source_ini": "path/to/assetname.ini",
  "lods": [
    { "lod_index": 0, "ase_file": "AssetName", "distance": 0.0 },
    { "lod_index": 1, "ase_file": "AssetNameL1", "distance": 1000.0 }
  ],
  "shadow_mesh": "AssetNameX",
  "hardpoints": {
    "weapon_nodes": [
      { "name": "weapon_rightarm", "type": 5 }
    ],
    "smoke_nodes": [],
    "foot_nodes": []
  },
  "animation_node_id": "NONE",
  "collision_bounds": {
    "source": "legacy",
    "minBox": [x, y, z],
    "maxBox": [x, y, z]
  },
  "axis_mapping": 0,
  "validation_baseline": {
    "vertex_count": 0,
    "triangle_count": 0,
    "material_slots": 0
  },
  "generated_at": "ISO-8601 timestamp"
}
```

Fields:
- `asset_class`: detect from INI structure (presence of `[Nodes]` = mech; else
  building/prop; future: vehicle via gvactor INI pattern)
- `animation_node_id`: from `[AnimationNode]` section if present
- `axis_mapping`: integer 0–3 matching `MC2_GLTF_AXIS` env var (0 = default
  `-x,-y,z` mapping used by `assimp_importer.cpp`)
- `collision_bounds.source`: always `"legacy"` for Phase 0–1 (bounds from ASE
  vertex data, same as the INI `[Bounds]` block)

---

## Advisor amendments

### A1 — GLB probe is mech-only today
`TG_TypeMultiShape::LoadFromFile` is called **only** from `mech3d.cpp` via the
`[Import] Source=` opt-in gate. `bdactor.cpp`, `gvactor.cpp`, `genactor.cpp`
have no equivalent hook. Phase 0 must confirm this before picking a test asset.

**Decision:** Phase 0–1 uses a mech as the test asset. Non-mech classes need
a 3-line engine addition per class (Phase 2 work) — see Phase 2 notes.

### A2 — Axis mapping is env-selectable
`assimp_importer.cpp` supports `MC2_GLTF_AXIS=0..3`. Default (0) maps
`gltf(-x,-y,z)` → MC2 space. The converter must apply the **same** default
transform so the GLB is authored in the coordinate space the importer expects.

Default mapping (axis 0): `mc2_x = -gltf_x`, `mc2_y = -gltf_y`, `mc2_z = gltf_z`
Inverse (ASE→GLTF): `gltf_x = -ase_x`, `gltf_y = -ase_y`, `gltf_z = ase_z`
(ASE uses 3ds Max Z-up right-handed; the assimp importer comment says the Y-up
glTF mapping lands correctly for default 0 after this negation.)

UV V-flip: `gltf_v = 1.0 - ase_v` (matches `toMC2V()` in assimp_importer.cpp).

### A3 — Node name length limit
`ValidateScene()` in `assimp_importer.cpp` rejects any node name ≥ `TG_NODE_ID`
(25 chars, i.e. max 24 chars + null). The converter must enforce this.

### A4 — Texture name derivation
`DeriveMC2TextureName()` strips directory, strips extension, lowercases, sanitizes
to `[a-z0-9_-]`, appends `.tga`. The converter encodes texture `uri` as just the
bare filename; the importer will apply the same strip logic.

### A5 — Stock asset fall-through safety
The probe path in `TG_TypeMultiShape::LoadFromFile` falls through to
`LoadTGMultiShapeFromASE` if the GLB is absent or import fails. This means a
partial/broken GLB will NOT break a stock install — the engine falls back to TGL.

---

## Phase 0 — Prerequisite verification

**Goal:** confirm which asset classes support GLB today WITHOUT engine changes.

Deliverables:
1. `docs/asset-mod-glb-scope.md` — per-class GLB support table
2. `docs/asset-mod-test-asset.md` — chosen test asset + rationale

**Finding (confirmed):** GLB probe exists only for mechs. Non-mech classes
need engine additions in Phase 2. Phase 0–1 test asset = **Raven mech**.

---

## Phase 1 — Python toolchain (offline, no engine changes)

### P1-A: `tools/asset_baseline.py`
Reads ASE + INI, outputs baseline JSON with vertex/tri/material counts,
texture names, node names, bounding box, LOD list, hardpoints.

### P1-B: `tools/validate_glb.py`
Compares a GLB + `.mcasset.json` against a baseline. Checks node names,
material slot count, texture existence, bounding box, vertex/tri counts.
Exit 0 = all pass, exit 1 = any fail.

### P1-C through P1-F: `tools/ase_to_glb.py`
Converts a single ASE GEOMOBJECT to a valid GLB:
- P1-C: positions + normals + UV0 (minimal mesh)
- P1-D: material slots + texture names (separate primitives per material)
- P1-E: node transforms + hierarchy
- P1-F: sidecar JSON generation

---

## Phase 2 — Engine additions (non-mech classes, deferred)

For each class add the `[Import] Source=` hook mirroring `mech3d.cpp`:
- `mclib/bdactor.cpp` → `BldgAppearanceType::init()` + `TreeAppearanceType::init()`
- `mclib/gvactor.cpp` → `GVAppearanceType::init()`
- `mclib/genactor.cpp` → `GenericAppearanceType::init()`

~3 lines per class. Requires `claude/assimp-mech-import-1` merged to nifty first.

---

## Phase 3 — Texture compression (deferred)

See `docs/asset-modernization-recon.md` §3 for the full plan.
Static props: BC7 via `KtxLoader` extension (easiest path).
Burnin/mech/vehicle: ETC1S after GameOS compressed-upload path.

---

## Building PBR v0 status

- Hangar and rusty Quonset are the first building GLB/PBR payloads in the rc1
  deploy path.
- Quonset shell is accepted for v0. `LitWin_Quonset` remains recorded as a
  deferred `window_overlay` mesh role because the source overlay is broad
  card geometry with no useful UV/alpha source; forcing it through the shell
  texture produces smeared wall cards.
- Follow-up: add a shadow/interior darkening treatment for Quonset and similar
  buildings. With the overlay deferred, the terrain visible under/inside the
  open shell reads fully lit; this is a separate shadow/interior-occlusion issue,
  not a placement or GLB import failure.
- Footprint darkening v0 is sidecar-controlled via `footprint_shadow`. It uses
  the existing world-space decal batch with a procedural black alpha footprint;
  normal building materials are untouched. Set `MC2_BUILDING_FOOTPRINT_SHADOW=0`
  to disable the runtime path for comparison.
- WIP/default-off follow-up: building PBR metal lighting calibration. Goal is to
  recover the brighter legacy metal read without disabling the corrugated PBR
  material. Candidate sidecar fields: `pbr_lighting.enabled=false`,
  `albedo_boost`, `ambient_boost`, `legacy_light_mix`, `normal_strength`, and
  `specular_boost`. Do not enable globally until Hangar and Quonset have a
  visual pass against their non-PBR baseline.