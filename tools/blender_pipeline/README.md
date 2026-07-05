# tools/blender_pipeline — BLENDER-ASSET-PIPELINE-1

Headless, batchable upscale/reimagine pipeline for EXISTING game assets:

```
stock .ase + .ini ──extract──▶ GLB ──transform (blender -b)──▶ GLB ──inject──▶ engine override
                 (ase_to_glb.py)      (recipe scripts)               (data/tgl ini | models.json)
```

Proven end-to-end on Blender **5.1.1** (`C:\Program Files\Blender Foundation\Blender 5.1\blender.exe`):
`mc2srcdata/tgl/quonset.ase` (46 tris) → weld+crease+subdiv (276 tris, stock
texture URI carried over) → `QuonsetHD.glb` + `quonset.ini` `[Import] Source=`
override injected into a deploy lane — no C++ changes, no game relaunch needed
to cook.

## Quick start

```powershell
# dry-run: validate manifest + print the exact commands (no Blender needed)
py -3 tools/blender_pipeline/batch_driver.py my_batch.json --dry-run

# real run (also mirrors tgl_ini outputs into a deploy lane's data/tgl)
py -3 tools/blender_pipeline/batch_driver.py my_batch.json `
    --deploy-tgl-dir "A:/Games/mc2-opengl/mc2-win64-v0.3/data/tgl"
```

Blender discovery (same pattern as `tools/mech_import/blender_runner.py`,
vendored in `blender_runner.py` here): `--blender` CLI arg >
`BLENDER_EXECUTABLE` env > known installs (5.1/5.0/4.2) > PATH.

## Manifest

See `manifest_schema.json` (documentation form; `batch_driver.py` validates in
code — no pip deps anywhere in this pipeline) and
`examples/quonset_hd.manifest.json`. One job = one asset × one recipe:

- `source`: `{ase, ini}` (stock asset — extracted via `tools/ase_to_glb.py`)
  or `{glb}` (already-converted asset).
- `recipe` + `params`: see below.
- `out.kind`:
  - **`tgl_ini`** — writes `<SourceName>.glb` + a copy of the stock ini with an
    `[Import] st Source = "<SourceName>"` block (the proven
    `data/tgl/quonset.ini` seam; consumed by `mclib/bdactor.cpp` /
    `gvactor.cpp` / `genactor.cpp` / `mech3d.cpp` LOD0 GLB probe).
  - **`models_json`** — LOD chain for the model-override registry
    (`mclib/model_override_registry.h`, TREE-OVERRIDE-LOD-MVP-1): copies
    `<name>_lod0..N.glb` and merge-writes a `models.json` entry
    (`renderOnly:true`, `fallback:"stock"`, ascending distances).
  - **`glb_only`** — just the transformed GLB.

## Recipes (headless `bpy`, run via `blender --background --factory-startup --python`)

| Recipe | What it does | Key params (defaults) |
|---|---|---|
| `upscale_mesh` | weld ASE-split verts → crease hard edges → Catmull-Clark subdiv → shade smooth. Rounds bevels without collapsing silhouettes; MC2 buildings are ~76v/82t so subdiv 1–2 is cheap. | `subdiv` 1, `subdiv_type` CATMULL_CLARK\|SIMPLE, `weld` 1e-4, `crease_angle` 40, `shade_smooth` true |
| `decimate_lods` | capability-survey #2 quick win: decimate loop emitting `<name>_lod1..N.glb` (LOD0 = untouched source). Each LOD decimates from the ORIGINAL, so ratios are absolute. | `ratios` (req, e.g. `[0.5,0.2]`), `distances` (default 600·n), `weld` 1e-4, `min_tris` 8 |
| `rebake_pbr` | EXPERIMENTAL: join → Smart-UV atlas → Cycles-bake albedo/normal/ORM PNGs (feeds the `.mcasset` MC2_BUILDING_PBR sidecar path) + baked-material GLB. | `res` 1024, `samples` 16, `margin` 4, `uv` smart\|keep |

**rebake_pbr caution** (docs/asset-pipeline.md §6, capability-survey #6):
in-game static-prop normal/ORM was ruled "on ≈ off" under the current lighting
model — bake for the asset viewer / future lighting, not for an in-game payoff.
Input GLB needs resolvable textures; missing images bake flat (warned).

## Texture carry-over

Stock ASE→GLB assets reference textures as external URIs (`A_Quonset.tga`)
resolved by the engine's texture manager — the `.tga` need not exist at cook
time. Blender drops unresolvable images on export, so the driver's
`patch_textures` stage re-attaches the stock baseColor URIs into the
transformed GLB by material name (Blender `.001` suffixes handled).
Disable with `out.carry_textures: false`.

## Blender 5.x gotchas encoded here

- glTF import creates a `custom_normal` float2 corner attribute that leaks
  into `mesh.uv_layers` and silently breaks UV bookkeeping (export loses
  TEXCOORD_0). `rebake_pbr` removes it before unwrapping.
- `UVLayer` wrappers are index-based and go stale across edit-mode round-trips
  and collection changes — always re-fetch by name.
- Edge creases are the `crease_edge` float attribute (4.0+); subsurf
  `use_creases` respects them (how `upscale_mesh` keeps hard edges hard).
- `Material.use_nodes` is deprecated (removal in 6.0) — guarded here.

## Tests

```powershell
py -3 -m pytest tools/blender_pipeline/tests/ -q     # 28 tests, no Blender needed
```

Covers manifest validation, plan/dry-run, override-ini idempotence,
models.json entry shape + stale-duplicate merge, GLB read/write + texture
carry-over, runner discovery order, recipe arg parsing.

## Follow-ons (from .claude/BLENDER-CAPABILITY-SURVEY-1.md)

- **#1 tree impostors / far-LOD cards** — biggest payoff; reuse this driver +
  a new `bake_impostor` recipe (ortho ring renders → atlas + quad GLB); needs
  engine billboard-card support first.
- FST-packed `.tgl`-binary-only assets have no extractor (stock coverage via
  `mc2srcdata/tgl/*.ase` is complete — 2947 files — so not currently needed).
