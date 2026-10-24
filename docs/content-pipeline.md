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
| Stock TGL viewer meshdump | `frontends/stock_meshdump.py` |
| BattleTech 2018 GLB + `bt2018_mech_package.json` | `frontends/bt2018.py`; cooker preserves skin source/clip bytes and cooks atlas tiers; runtime stays `[Import] Source=` |

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
Recipes requesting `repair_inward_hull` report and flip inward outer-hull faces on authored intact chains
(culled from outside = see-through wall). Collision, passability and LOS
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

## Runtime loader inventory (import-unify)

`mclib/content_ir.h` is the native view of one selected condition/state/LOD of
`mc2-content-ir/1`. It retains TG corner UVs and vertex order for compatibility
with stock animation and rigid skins. It owns CPU data, with no runtime handles.
`TG_TypeMultiShape::BuildContentIR` validates/caches it, builds runtime nodes,
applies the source's bounds/ground policy, and binds texture names. Rendering
never changes the stock collision/passability shape or simulation RNG.

| Consumer/source | Frontend and common consumer |
|---|---|
| Stock buildings, trees, mechs, vehicles, generic actors, damage, arms, shadows | `msl_load.cpp`: `DecodeStockASE` / `DecodeStockBinary` -> native IR -> `BuildContentIR` |
| Kit and `models.json` overrides, all conditions/destroyed/LODs | `bdactor_type.cpp` -> Assimp frontend -> native IR -> same cooker |
| `[Import] Source=` on building/tree/mech/vehicle/generic appearances | `LoadFromFile` -> Assimp frontend -> same cooker; failed imports retain stock fallback |
| BT2018 rigid skins | Assimp retains the existing bind/animation math and registers animation after common runtime publication |
| Missing-required-shape placeholder | `tgl_load_guard.cpp`: synthetic geometry, no imported source |
| Asset viewer stock preview | Same stock frontend/cooker; its glTF preview is an authoring viewer, not a second game asset cooker |
| Modern tree pack / stock batch | Recipe adapters to `cook.py`; Track G staging/texture/assembly/promotion and GLB surgery deleted |
| Grass/fern population | Procedural shader strips and card textures; no imported model/shape loader |

Loose ASE is authoritative even when its timestamp is older than an existing
TGL. Stock-only installs retain packed TGL decoding. Native cache entries live
under `cache/content-model`, using the shared atomic/LRU `cook_cache`; identity
includes resolved VFS source bytes, import parameters and cooker version.
Self-contained sources can skip decoding on a hit; glTF/FBX resolve their external
geometry before computing identity. Offline geometry/texture caches additionally
include source dependency hashes, material policy and cooker versions.

Destroyed chains may use `{"intact": "rubble_lod0.glb", "lods": [["rubble_lod1.glb", 350]]}`.
`LIGHT_*` extras survive IR and runtime GLBs; the native shape's `ContentSockets()`
exposes TG/WU position, direction, colour, range, cone, activity and destruction
policy to lighting. Stock `SpotLight_`/`LitWin` attachment pivots survive static
baking. Source conditions/damage with no emitters stay empty; no lamps are invented.

## Approved industrial intent set

`tools/scifi_kit/industrial_package.py` reads the 34 approved industrial bakes
from `E:/mc2-ref/scifi-kit/art/intent-pass/_work`: round 1 `t1`, round 2 `r2`,
transformerstation `e1` (`7809b383d`), and both generators `e5` (`f57d04c6a`).
It uses the common IR cooker, keeps LOD0/1/2, and shares BC7 albedo and BC5/BC7
normal/ORM atlases across material classes. Generated payloads stay outside git.

Cook with `py -3 tools/scifi_kit/industrial_package.py --out <proof>/package`.
Copy only `package/data` and `package/mod.json` into the lane install's
`mods/mc2-industrial-kit/`. The reproducibility IR and report under `content/`
are proof artifacts, not runtime payload. The installed kit mounts automatically
as the lowest mod layer. Campaign/add-on manifests merge above it; a mod-owned
appearance INI or source mesh also suppresses the bundled record for that shape.
The stock install's meshes, INIs, archives and collision data are not rewritten.

The remakes are pristine only. No damaged, destroyed, worn or abandoned chain is
emitted. Both disabled and destroyed buildings keep their stock damage geometry.
Imported LODs use the existing building distance thresholds where present, or
the bake's 350/700 WU distances when stock supplies only one mesh. Each LOD's
geometry and textures register before GPU finalization; swaps invalidate the
old static recipe. Stock building LOD policy is unchanged.

Stock base-shape animation is still read from `[Animation:N]` and advances in
the ordinary building update. Binding resolves node names on each destination,
including the independent stock simulation shape and imported render LODs.
Authored pivot metadata survives the importer. An override missing any stock
animation track's node falls back to the stock render chain. **The supplied e5
generator bakes contain one flattened node and no `Coil` child: their entries
are packaged, but both generators retain stock visuals and animation. This is
an unresolved source-bake gap, so the complete 34-remake goal is not accepted.**
The same binding seam supports retained stock node names such as `LOS_Radar`,
`Spinner` and `Sensorpivot`; those military assets still require their own proof.

Proof directory: `E:/mc2-archive-2026-10/proofs/kit-ingame-2/`. Mission inventory
finds 48 approved-set instances on `mc2_10`, 50 on `mc2_03` (fuel tanks plus a
turret generator), and none on stock `mc2_01`. The day mission is a regression
check, not industrial visual coverage. Runtime acceptance, crops and performance
results must be read from that directory; an SDL window-creation failure does
not count as a rendered pass.

Lane proof (implementation `bf3a8dbfa`): the game build, all-target checkpoint,
contract harnesses, model-override tests, 21 content-pipeline tests, 1296 scripts
tests plus 270 subtests, five selected contract rows, and worktree ratchets pass.
Day/night A/B and the mc2_03 fuel scene produced captures. The command-channel
damage test acknowledged `animate 122301 damaged`; its capture shows the stock
damaged large fuel tank beside remade pipework. `index.html` and
`phone-sheet.jpg` in the proof directory contain the actual unaltered crops.
The night transformer is visibly darker than stock; this is exposed for review,
not treated as visual acceptance of the entire set.

Interleaved cost is provisional under other host runs. Last corresponding GPU
bookmark-sweep summaries: mc2_01 props 0.044 to 0.045 ms, mc2_10 0.051 to
0.138 ms. Night load-end process VRAM was 610 to 872 MiB; engine startup was
11.0 to 22.3 s (first cold kit startup took about 46 s). Day VRAM stayed
740 MiB and startup was 11.1 to 9.7 s. Full sample ranges, CPU samples,
approximate active-window CPU load and observed other mc2 process IDs are in
`performance/measurements.json`; lease-wait observations are distinguished.
Runtime payload is 486 MiB: 102 GLBs, 304 KTX2s, no TGA duplicates.

The mod battery initially passed 3/6; retained resource-failure/cold-timeout
stages then passed warm retries, yielding passing coverage for all six mods
without relaxing engine gates. Both initial and retry evidence is retained.
Automatic approval review rejected deletion of the three disposable failed
stages with "blocked by policy"; they remain under the proof directory's
`mod-battery/20261024-105544/`. No stock/mod source install was modified.
