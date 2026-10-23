<!-- doc-status: current | reviewed:2026-10-22 -->
# Content pipeline: one IR, one cooker, one material schema

Every imported model (prop, building, tree, mech) goes **frontend -> IR -> validated cook -> mod
package -> Vulkan**. Code: `tools/content_pipeline/` (Python) and the override registry /
static-prop material path in the engine. Entry point:

    py -3 tools/content_pipeline/cook.py tools/content_pipeline/recipes/hangar_fence.json

## Frontends (thin adapters; they only produce IR)

| Source | Frontend |
|---|---|
| Stock ASE + reviewed CC0 detail rows (was `blender_pipeline/material_map.py`) | `frontends/stock_ase.py` |
| Any glTF/GLB: Blender recipes (fence_gen), sci-fi building kit, FBX via Blender, catalog candidates | `frontends/gltf_file.py` |
| Catalog selection / fitting (`tools/assets/replace.py package`) | fits, then `gltf_file` |
| BattleTech 2018 GLB + `bt2018_mech_package.json` | `frontends/bt2018.py` (IR only; runtime stays `[Import] Source=`) |

The `stock_ase` asset may list `excludeNodes`: exact reviewed mesh names (for example `LitWin_Quonset2`) that keep
legacy lighting. A lit window can reuse a roof texel, so an atlas mask cannot exclude it; the named meshes move to an
appended legacy copy of their material (same albedo, unauthored). Unknown or duplicate names fail the cook. Meshes with
no texture coordinates keep TG's `(0,0)` default (alpha included) instead of the extractor's flipped corner texel.

A model-override record that sets `lateCoverageFringe: true` (the procedural fence bundle) composites its thin coverage
after vegetation; the depth-writing core stays mip-aware. Overrides do not pick a hard alpha cutoff by provenance; an
authored `MASK` slot also uses the late fringe.

## The IR (`ir.py`, `mc2-content-ir/1`)

TG space (X lateral, +Y up, Z forward, world units), column-vector node matrices, TG texture v.
An `Asset` holds: node hierarchy with local transforms and **instancing** (a mesh shared by
nodes), primitives with positions/normals/tangents/UV0/COLOR_0 (stock lighting tags), per-slot
`Material`s, LOD chains per state (`intact`, `destroyed`), condition variants (`worn`,
`abandoned`; `pristine` is the default and must stay stock-equivalent), shadow stand-ins,
skeleton/clip/clip-event references, sockets (muzzle, foot, jumpjet, ...), parts/damage regions,
and provenance (role, path, sha256, licence) for every input byte. `ir.save/load` round-trips
losslessly; `ir.fingerprint` is the content identity.

## Runtime material schema (`RenderCore/SurfaceMaterial.h`, `shaders/include/surface_material.hglsl`)

`SurfaceMaterial { linear baseColor, render-space normal, roughness, metallic, ao, linear emission,
alphaMode, materialClass }`. Classes: `legacy_prelit` (stock art, keeps the vertex-lit look, zero
new specular), `dielectric`, `painted_metal`, `bare_metal`, `rust`, `concrete`, `glass`, `grating`,
`wood`, `fabric`, `rubber`, `emissive`. Only `bare_metal`/`grating` may be metallic; scalars absent
from the package take the class defaults; **metal and roughness never come from colour**. Alpha:
`STOCK` (stock coverage policy), `OPAQUE`, `MASK` (depth-writing cutout). A pytest pins the IR,
C++ and GLSL lists together.

Input evaluators: `prop_material_vk.glsl` (props/buildings: package row + normal/ORM arrays) and
`mech_material_vk.glsl` (imported mech AO/normal). Composition is not here:
`prop_lighting_compat_vk.glsl` is the lighting lane's pre-R2 adapter; R2/R5 replace it and consume
`SurfaceMaterial` unchanged (AO multiplies ambient only; specular only for authored classes).

## Package (`package.py`) and runtime contract

A package is a mod directory, mounted through the normal VFS (content packs ride `MC2_MOD_DEPS`
beside any campaign mod; every mounted mod's `models.json` merges in priority order). Missing
package = stock. `data/model_overrides/models.json` record:
`source` (+`lods`) = pristine intact; `destroyed` = the shape the engine renders once the building
is destroyed (collision/passability stay stock); `conditions.{worn,abandoned}` with optional own
`destroyed`; `materials` v2 slots `{slot, class, alphaMode, normal, orm, scalars, uvScale}`.
`conditions.json` picks a condition per mission (`"*"` or per appearance; default pristine).
Runtime GLBs bake every node transform (the engine's static import also applies node transforms
and instancing now), carry identity nodes with unique names and are validated (`validate.py`:
Khronos-style structure + MC2 runtime profile). Stock-policy base colour is referenced by name,
never copied. Engine-special nodes (`SpotLight_`, `LitWin`, `_PAB`, `LOS_`) keep a legacy slot.
Building cooks report outer-hull faces that point inward (culled from outside = see-through
wall); `repair_inward_hull` flips them on authored intact chains. Collision, passability and LOS
always come from the stock shapes, whatever renders.

## Texture cook contract (`texcook.py`, `mc2-texcook/1`)

| Role | Colour space | Block | Mips |
|---|---|---|---|
| base_color, emissive | sRGB | BC7_SRGB | linear-light, alpha-premultiplied; MASK keeps level-0 coverage |
| normal | linear (OpenGL +Y) | BC5 (Z rebuilt at runtime) | averaged + renormalised |
| orm (R AO, G rough, B metal, A coverage) | linear | BC7 | box |

Base colour tiers `data/tgl/{32..1024}` are real downscales (never enlarged); a unique-bake atlas
sets `texel_scale` (tier t holds t x scale) to match stock on-screen texel density. Detail maps share
one 512 extent per mission (one array). Blocks are encoded with the installed DirectXTex texconv
(CPU codec, deterministic); the KTX2 container is written here. Cache (`cache.py`): key = contract
+ params + sha256 of **resolved bytes**, entries `<prefix><16 hex><ext>`, temp-file + atomic
rename, per-kind LRU - the same rules as the engine's `cook_cache` (shadow/terrain cooks).

## How things plug in

- **New asset:** add a recipe entry (frontend + explicit per-slot classes + provenance), run
  `cook.py`, mount the output under `mods/<id>`, capture day/night at the play camera.
- **Sci-fi building kit:** export standard glTF with `extras.mc2_material_class` per part, pure
  base colour (no AO baked in; AO goes in ORM.R), destroyed/worn/abandoned as sibling GLBs; call
  `cook.py` instead of writing bundles or install `models.json` itself.
- **BT2018 mechs:** `frontends/bt2018.py` keeps per-part slots, joints/clips, sockets; the chassis
  profile supplies reviewed classes, parts and units. P1 compiles from this IR; P11/P12 extend
  `mech_material_vk.glsl` with an ORM/emissive row exactly like props.
- **Terrain cookers** (`cook_concrete_detail.py`, `cook_terrain_layers.py`): call
  `texcook.cook_ktx2` for their KTX2 output to share colour-space, mip and cache rules.
