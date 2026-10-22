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
Continued quoted model filenames are folded before INI parsing: the stock
WireFence damage-shadow name contains a newline and must resolve to
`wirefencedamx`, rather than a truncated fictitious filename.

Run checks: `py -3 -B -m pytest tests/assets -q -p no:cacheprovider`.

## Generated coverage and examples

The snapshot accounts for all 2,030 stock TGL binaries: 1,958 referenced by
appearance INIs and 72 unassigned. There are 1,271 appearance/type records,
767 with a decoded base mesh; 504 definitions have no usable base geometry in
these installed archives (many are unused legacy definitions). All 319 mission
PAKs across stock plus six mods decoded without mission errors or unresolved
object type IDs, yielding 455,874 placements matched to stock selection keys.
Custom/missing-model placements remain counted separately in corpus coverage.

The 75 contact-sheet pages include all present TGLs, including shadow meshes,
and placeholders for 23 declared missing files and 19 empty/helper-only shapes.
There are 2,011 decoded thumbnails and 2,053 requested thumbnail slots. No absent
geometry is fabricated. Full gaps are in [coverage-details.json](examples/coverage-details.json).

[Worked examples](worked-examples.md), [top 20](top20.md), and
[download approval candidates](downloads.json) are committed separately from
mod binaries. Rebuild examples with `py -3 tools/assets/examples.py --rework`;
existing complete draft packages are read rather than overwritten.

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

The cheap path uses premultiplied-alpha Lanczos resizing and mild RGB sharpening.
Tier 256 retains the enlarged image; each lower tier halves both dimensions,
preserving alpha-test coverage as closely as discrete pixels allow. Reports list
each tier's dimensions. Material authoring belongs to `claude/blender-redux-pbr`;
this frontend does not generate normal maps, mip artifacts or tool inventories.

Rework clones only the selected intact/LOD/damage TGL files, replacing fixed-size
texture-name fields with private names. Every geometry/UV/material index is
checked unchanged. This prevents a shared `sprubble.tga` from changing other
destroyed buildings. A package's selection follows the TGL key, so appearance
aliases using that same TGL are affected together. Stock shadow meshes remain
available. Existing runtime model overrides for the same selection may take
precedence; test packages independently after owner review.
