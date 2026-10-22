<!-- doc-status: current | reviewed:2026-10-22 -->
# Sci-fi building kit: owner driving loop

Run from `A:/Games/mc2-study2`, branch `claude/scifi-building-kit`. Local commits
only. Outputs and Blender intermediates stay on G:. No engine build is needed
for these intact visual overrides. Pre-existing `.scratch/` is unrelated.

```powershell
# First setup / regenerate the 19 reusable, parameterized part prototypes + LODs
py -3 tools/scifi_kit/kit.py library

# Generate, bake, validate and render all THREE types, TWO seeds each
py -3 tools/scifi_kit/kit.py review

# Fast art loop: edit JSON, then rerun just one draft (about 30-45 sec here)
py -3 tools/scifi_kit/kit.py review --type mech_hangar --seed 17

# Export an isolated model-override bundle, or merge/install it into the test lane
py -3 tools/scifi_kit/kit.py export --type mech_hangar --seed 17
py -3 tools/scifi_kit/kit.py install --type mech_hangar --seed 17

# Two background-only views of the real mc2_01 hangar; always --no-kill internally
py -3 tools/scifi_kit/kit.py capture
```

Open `G:/mc2-assets/out/scifi-kit/contact-sheet.png`, then an individual
`<type>_<seed>/montage.png` at full resolution. Every montage shows the high
source, **baked game LOD0** at an MC2-style RTS angle, the catalog stock thumbnail,
the local BT2018 industrial reference rendered in clay, and both owner Chicago
references. The stock/reference panels are explicitly labelled: their cameras
and lighting differ, and the Blender RTS view is not an engine screenshot.
`in-game/mc2_01_scifi_hangar_close.png` and `..._rts.png` are the engine evidence.

Edit `tools/scifi_kit/style/battletech.json` for dimensions, palettes, edge depth,
budgets and presentation; edit `tools/scifi_kit/rules/buildings.json` for stock
mapping, roof, opening category/count, buttress spacing and roof plant. JSON was
chosen so neither Python nor Blender needs a YAML dependency. Style values cite
local evidence and distinguish measured bounds from chosen design dimensions.
Vocabulary is descriptive; implemented rule switches are the fields in each
building rule. New vocabulary or roof algorithms go in `parts.py`.

Feedback example: "mech_hangar 17: keep the two recessed bays and roof profile;
make front shoulder armour twice as broad, reduce roof rib contrast, warm the
concrete; keep the current RTS silhouette." Name the type, seed, view and the
one or two changes you want. Re-run the same command/seed to compare. Keep a copy
of the previous montage outside its draft folder before rerunning: generated
files are deliberately replaced. Blender may retain its normal `.blend1` backup.

To fit an L-shaped or irregular lot, save a JSON array of polygon XY vertices
in **centred metres** and pass `--footprint G:/path/lot.json` to `review`. It must
fit inside the selected stock rectangle. The planner tiles full 3m modules and
merges them into rectangular wings; diagonal boundaries become stepped setbacks.
Example for the hangar: `[[-20,-20],[20,-20],[20,0],[0,0],[0,20],[-20,20]]`.
Holes, self intersections and sub-module lots are unsupported. The export checks
every actual vertex against the requested polygon and the stock envelope.

Each draft retains `<name>_source.blend`, `job.json`, `stats.json`,
`validation.json`, high-to-low Cycles bakes (base colour, tangent normal, AO,
roughness, packed ORM) and identity-node engine GLBs. Source is Z-up metres;
game vertices are baked to inverse runtime axis0, including the stock pivot.
Runtime TG is `(-glTF.x,-glTF.y,+glTF.z)`. Never export with a transform-only root:
the current importer drops node transforms. LOD0 is the registry `source`; only
LOD1 goes in its `lods` array. Triangle ceilings are 180k source, 12k LOD0 and
3.5k LOD1; actual exported counts are in the validation report. Those are first
draft limits, not performance acceptance. Static-prop runtime LOD switching is
not established by this task; both affordable meshes are supplied.

The bundle is `<draft>/bundle/`, with the fence's engine-schema
`data/model_overrides/models.json` and unique uncompressed TGA names in all
`data/tgl/{32,64,128,256,512,1024}` tiers. The installer merges only its target
LOD0 key, preserves other records and saves the original registry under
`install-backup/models.json`. Installation is restricted to
`G:/mc2-installs/mc2-win64-mapgen`. To restore, merge the previous record from
that backup (or remove the `staticProp:hangar` record if originally absent).
Unique unused scifi-kit files can remain harmlessly on disk.

The selection frontend remains the `claude/asset-modernize` catalog: the harness
consumes its measured stock IDs, LOD0 keys, SHA256s and thumbnails rather than
inventing another inventory. Candidate `.asset.json` metadata accompanies bundle
GLBs, but these are **already engine cooked**: do not inverse-transform them
again in the general replacement packager. Use the kit's installer for these.

Only original procedural geometry and local ambientCG / Poly Haven CC0 maps
enter the bundle. Copied maps and hashes are in `G:/mc2-assets/pbr/scifi-kit/`.
Read-only BT2018/MW5 content and Chicago images are review references only.
MW5 mech exports and mod paks were found; no readily viewable building images
were found, so no Unreal setup or large extraction was attempted.

The current game draft uses **base colour only**. Normal/ORM channels are ready
in the GLB and an explicitly kit-owned `material.json`; this does not claim
compatibility with the PBR branch's evolving sidecar schema. There is no fallback
sheen. AO contributes a restrained 28% modulation to the legacy TGA. Baked normal
and ORM are kept separate so the PBR integration can remove that approximation.
Stock collision, hit points, destruction and stock ruined meshes remain intact;
the visual transition to stock damage is still an art limitation.

Quick validation: `py -3 -B -m pytest tools/scifi_kit/tests -q -p no:cacheprovider`,
`py -3 tools/scifi_kit/kit.py validate`, and `bash scripts/check-contracts.sh --quiet`.
Review `G:/mc2-assets/out/scifi-kit/first-draft-report.json` for the viewed art
critique, measurements, evidence paths and remaining limitations.
