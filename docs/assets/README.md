# Asset modernization workbench

Offline tools only; Python 3, NumPy and Pillow. OBJ import also uses installed
trimesh; pytest runs the tests. No game launch, downloads or install writes.

`py -3 tools/assets/catalog.py --sheets` rebuilds [catalog.json](catalog.json),
[catalog.md](catalog.md) and [contact sheets](sheets/index.md). Source paths and
per-corpus errors stay in the JSON. Stock FSTs are read from
`A:/Games/mc2-opengl/mc2-win64-v0.4`; pristine `mc2srcdata-fresh` supplies object2
and Buildings.csv because the dev install's loose versions include mods.

The mapgen `Install`, FIT parser, PacketFile decoder and type catalog are reused
from `claude/mapgen-ascii` commit `e322675cec833f1926caed91e08ecdc830e68f58`
in `tools/assets/mapgen_data.py`. Local changes: import location, an archive-only
switch and loose TGL/texture indexing. The 40-byte mission object layout comes
from that branch's `extract.py`. Carver5 falls back through Omnitech to stock.

Usage counts represent installed missions, including potential scripted parts;
they are not simultaneous visible counts. Re-exported mod models sharing a
stock selection key count toward that key, with their changed hashes reported.
Unmatched custom keys do not count. Each corpus is counted separately even if
it contains missions also shipped elsewhere. The priority score is a projected
size and quality heuristic; vegetation and mechs have separate lists.

TGL data uses the serialized Win32 layout in `mclib/msl_load.cpp` and
`mclib/tgl_types.cpp`. The renderer assembles node-local vertices using absolute
node centers, preserves per-corner UVs, and uses TG +Y up. It intentionally does
not copy the viewer's historical Z-up mapping. Helpers/LOS/spotlights are omitted.
Previews approximate materials and lighting; they do not certify runtime pixels.

Run checks: `py -3 -B -m pytest tests/assets -q -p no:cacheprovider`.

## Select, compare and package

```powershell
py -3 tools/assets/replace.py spec hangar --out docs/assets/examples/hangar.spec.json
py -3 tools/assets/replace.py search wirefence --out docs/assets/examples/wirefence.search.json
py -3 tools/assets/replace.py spec hangar --state destroyed
py -3 tools/assets/replace.py package hangar --candidate G:/mc2-assets/candidates/hangar.glb --id modern-hangar --allow-stock-damage
py -3 tools/assets/replace.py rework hangar --id texture-hangar
```

Selection accepts a catalog appearance, type name or type ID. `wirefence` is the
stock chain-link fence; all other fence variants remain separate selections.
Candidate search never fetches anything. It reads `G:/mc2-assets/candidates`
recursively and ranks GLB/glTF/OBJ by category, footprint, shape ratios and
triangle cost. Missing categories are ranked with a penalty; FBX files get an
actionable conversion message. Supply `<model>.asset.json` alongside each model:

```json
{
  "category": "building",
  "up_axis": "y",
  "forward_axis": "-z",
  "wu_per_unit": 100,
  "license": "CC0-1.0",
  "source_url": "https://the-approved-source/asset",
  "orientation_reviewed": true
}
```

`wu_per_unit` only establishes a reference size for ranking; packaging fits the
geometry to measured stock world units. Package authoring requires provenance
and a matching category. Defaults are glTF +Y up and +Z front; explicit axes are
strongly recommended. No numeric test can recognize the semantic front of a
hangar: the review flag records the author's declaration, and a shared-camera
stock/proposed PNG supports inspection. `--yaw` makes a deliberate further turn.
Uniform scaling is the default; `--stretch` permits at most 25% relative axis
distortion. Poor shape matches fail the final 10%/0.5 WU bounds tolerance.

Packages always go to a new `G:/mc2-assets/out/<id>` folder and refuse overwrite.
They contain engine-schema `mod.json`, `data/model_overrides/models.json`, baked
identity-node glTF/buffers/PNG, TGA textures in `data/tgl/{32,64,128,256}` and next
to the model, target spec, validation report and preview. The runtime resolves
textures under `data/tgl/ObjectTextureSize`, not the manifest directory.
Registry keys use the INI's LOD0 filename, which may differ from AppearanceName.
The cook inverts the engine's default prop axis0 **without auto-grounding**;
exported authoring glTF is intentionally inverted so runtime TG has +Y up.
Changing global runtime axis/yaw settings invalidates that assumption.

The output gets re-imported and transformed through the documented runtime
mapping for validation. This checks geometry, pivot, orientation math, texture
binding names and budgets, but is not the C++ registry's authoritative parse or
a runtime screenshot. Alpha materials use `a_` names and keep structural casters.

**Current runtime limits:** intact staticProp replacement only. Building damage
keeps stock geometry, so packaging requires `--allow-stock-damage`. A destroyed
candidate can use `--state destroyed --review-only`, which emits no `mod.json`
and no active `models.json`. Trees, vehicles and mechs keep their existing
specialized import workflows. Animated gates/turrets require a rig/node review;
this static cooker cannot preserve their moving parts. Stock LODs are specified;
the building override runtime currently has one render shape and cannot activate
a replacement LOD chain. No engine changes were made to hide these limits.

Local FBX conversion, if needed (installed Blender, no download):

```powershell
py -3 tools/assets/replace.py convert-fbx G:/mc2-assets/candidates/source.fbx G:/mc2-assets/out/fbx-review/model.glb --blender "C:/Program Files/Blender Foundation/Blender 5.1/blender.exe"
```

The native glTF reader supports static triangle scenes, embedded/external local
buffers and images, accessor strides, scene TRS/matrices and repeated instances.
It rejects skins, animations, morphs, required extensions and sparse accessors
rather than quietly mis-cooking them. OBJ uses installed trimesh; convert
multi-material OBJ to GLB first. This path retains base color and alpha, not PBR
normal/metal/roughness binding or vertex colors. Use the modeller brief to author
the missing material work.

## Texture rework

The cheap path uses premultiplied-alpha Lanczos resizing and mild RGB sharpening,
then writes alpha-coverage-aware mip images and luminance-derived normal maps.
These normals are an editing starting point, not recovered surface geometry.
Mips/normals are authoring artifacts; the generic override loader does not enable
them automatically. The report inventories PATH tools and known local weight
locations; it does not install or fetch an upscaler.

Rework clones only the selected intact/LOD/damage TGL files, replacing fixed-size
texture-name fields with private names. Every geometry/UV/material index is
checked unchanged. This prevents a shared `sprubble.tga` from changing other
destroyed buildings. A package's selection follows the TGL key, so appearance
aliases using that same TGL are affected together. Stock shadow meshes remain
available. Existing runtime model overrides for the same selection may take
precedence; test packages independently after owner review.
