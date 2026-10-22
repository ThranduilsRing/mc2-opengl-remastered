# Hangar and fence modernization

Built offline on `claude/asset-modernize`, based on `a76005894`. No engine edits,
game launches, asset downloads, install modifications or pushes.

## Selection evidence

All six selections have intact/destroyed target specs, local-library search
reports and stock previews under [examples/](examples/results.json). The library
`G:/mc2-assets/candidates/` did not exist when checked, so every search has zero
results; no substitute candidate was invented.

| Selection | Instances across seven corpora | Max in a mission | Intact triangles | Footprint X/Z WU | Height WU | Proposed triangle cap |
|---|---:|---:|---:|---|---:|---:|
| Hangar | 198 | 8 | 98 | 191 × 192 | 77.92 | 12,000 |
| WireFence | 9,300 | 506 | 4 | 0.361 × 43.343 | 15.97 | 494 |
| WireFenceDiag | 684 | 63 | 4 | 0.114 × 60.943 | 16.00 | 3,968 |
| RazorWireFence | 12,853 | 489 | 8 | 8.615 × 43.111 | 27.35 | 511 |
| RazorWireFenceDiag | 292 | 31 | 8 | 8.615 × 60.955 | 27.35 | 8,064 |
| VehicleHangar | 449 | 24 | 114 | 105.320 × 113.763 | 59.48 | 10,416 |

Instance counts are potential mission placements, including inactive/scripted
parts, with each installed corpus counted separately. Modified mod exports
under the same stock key are included and flagged in catalog corpus bindings.
Destroyed specs inherit the parent's potential usage; this is **not a claim
that all those placements start destroyed**. Budgets are initial authoring caps,
not measurements of visible instances or available frame time.

The destroyed Hangar has 172 triangles and nearly the same bounds as intact.
Its roof is open and structural framing remains at 77.92 WU. In contrast, the
destroyed VehicleHangar is 16.25 WU high, and destroyed WireFence is a flat
two-triangle ground card. Treating every destroyed variant as a smaller intact
model would give incorrect fitting targets.

The Hangar's type-559 gameplay extent is **35 WU**, while its visual bounding
radius is about 145 WU. The 40×40 INI `[Bounds]` rectangle is UI space, not its
world footprint. The spec preserves the gameplay extent/occupancy bitfields and
uses actual mesh bounds for fitting. Runtime keys come from LOD0 filenames:
`staticProp:hangar` and `staticProp:wirefence`.

## Reviewable texture alternative

Two complete local draft mods were generated:

- `G:/mc2-assets/out/astra-review-e3c01ef28/asset-rework-hangar/`: intact, LOD1 and destroyed TGL clones;
  four private 512×512 textures, including isolated rubble.
- `G:/mc2-assets/out/astra-review-e3c01ef28/asset-rework-wirefence/`: intact, LOD1 and destroyed TGL
  clones; two private 256×256 textures.

Both include `mod.json`, a modeller brief, target spec, geometry-equality report,
runtime TGA texture tiers and stock/rework
comparison PNGs. They have **not** been copied into any game install.

Review [intact Hangar](examples/hangar-intact-rework.png),
[destroyed Hangar](examples/hangar-destroyed-rework.png),
[intact WireFence](examples/wirefence-intact-rework.png), and
[destroyed WireFence](examples/wirefence-destroyed-rework.png).
The full archive sheets are [here](sheets/index.md).

Lanczos plus unsharp improves sampling but creates no new modeled detail; the
comparison is modest. WireFence's texture is only 64×64 in the stock 128-tier
folder. New geometry or a UV-aware material rebake is the stronger long-term
option. Alpha wraps are preserved in the preview, including stock U values near
2.0. Texture renaming changes no vertex, UV or material-index bytes, and shared
stock rubble remains unchanged outside this selection.

The regenerated packages keep full resolution in tier 256. Hangar tiers are
64/128/256/512 pixels; WireFence tiers are 32/64/128/256 pixels. Comparisons use
the runtime's default 128 tier. Geometry-equality checks pass for all six TGLs.
Automatic normal maps, mip artifacts and upscaler discovery were removed;
material authoring belongs to `claude/blender-redux-pbr`.

## Replacement constraints

The native importer validates baked scene transforms and inverse axis0, unit
scale, target bounds/pivot, triangle cost and texture dimensions/memory, then
re-imports the cooked glTF and checks again. Tests use an asymmetric, translated,
scaled fixture and verify active manifest paths, texture files and previews.

`model_override` currently replaces **intact static props only**. Destruction
continues using the stock damage shape. The package CLI requires an explicit
stock-damage fallback; a destroyed candidate produces a review bundle with no
active manifest. Texture rework can affect both states now. StaticProp overrides
also have one runtime render shape: stock LOD expectations are recorded, but
the pipeline cannot promise an activated replacement LOD chain. Gate/turret
animation, tree, vehicle and mech replacement use specialized authoring paths.

Alpha textures do not disqualify fences from shadow casting. Keep structural
caster geometry; the depth-only shadow path may still render solid silhouettes.
The software preview is not a game lighting, shadow or authoritative C++ registry
oracle. No runtime verification is claimed.

## Downloads for owner approval

Exact URLs, license evidence, format preferences and limitations are recorded in
[downloads.json](downloads.json). Nothing has been downloaded.

| Priority | Source | Exact license | Intended use |
|---|---|---|---|
| 1 | [Kenney Space Kit](https://kenney.nl/assets/space-kit) | [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/) | Inspect hangar shell variants; author matching destruction |
| 1 | [Quaternius Metal Fence](https://poly.pizza/m/qWKhREFj7H) | CC0-1.0, explicitly listed Public Domain | 218-triangle chain-link candidate; measured fitting awaits local files |
| 2 | [Kenney City Kit (Industrial)](https://kenney.nl/assets/city-kit-industrial) | CC0-1.0 | Industrial/warehouse shell alternatives |
| 2 | [Poly Haven Modular Chainlink Fence](https://polyhaven.com/a/modular_chainlink_fence) | [CC0-1.0](https://polyhaven.com/license) | Extract/bake one panel; full 89K-triangle kit is over budget |
| 2 | [Poly Haven Worn Corrugated Iron](https://polyhaven.com/a/worn_corrugated_iron) | CC0-1.0 | UV-aware hangar roof/wall rebake, not a mesh replacement |
| Optional | [Quaternius Ultimate Modular Sci-Fi](https://quaternius.com/packs/ultimatemodularscifi.html) | CC0-1.0 | Panels/doors for hand rework; interior kit is not a verified exterior near match |
| Optional | [Quaternius Ultimate Modular Ruins](https://quaternius.com/packs/ultimatemodularruins.html) | CC0-1.0 | Ruin/rubble authoring reference; masonry style needs adaptation |
| Optional | [Kenney Graveyard Kit](https://kenney.nl/assets/graveyard-kit) | CC0-1.0 | Ornamental fence alternatives; weaker match than chain-link |

Sketchfab research found two **conflicting license labels**, so they are held
outside the verified CC0 approval list. [Military Outpost Kit 1.0](https://sketchfab.com/3d-models/military-outpost-kit-10-cc0-010dc4f0a73a48e7a53598c6f24fe9cf)
has an author CC0 dedication but a CC Attribution platform field.
[plaggy's Woode Fence](https://sketchfab.com/3d-models/cc0-woode-fence-39c5f9ac62a64259aa3478040339fa2a)
likewise states CC0 1.0 in the description while the platform says CC Attribution;
[the author's site](https://plaggy.net/) dedicates free models to CC0. The fetched
platform fields did not identify a CC BY version. Retain original license
evidence and resolve this discrepancy before treating either as an approved
CC0 download. Neither is a measured drop-in near match.
