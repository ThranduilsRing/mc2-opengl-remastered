# Stock inventory and original blend drafts, 2026-10-22

Art worktree only; no engine changes, game launches, install writes or pushes.
Current round: `E:/mc2-ref/scifi-kit/`. See `scifi-kit-content-handoff.md` for the
content IR, material and light-socket contract. The G: sections below are the
historical six-draft round at `975cc2c41`, not the current delivery.

## Revised round: mc2-r5

* Inventory/mapping: `E:/mc2-ref/scifi-kit/mapping/`; 269 declarations, 44 freshly
  scanned stock missions, 306 BT2018 families, exact part-donor notes per row.
* Eight selections / seven distinct geometry sets: the six original selections,
  three-barrel prop and rusty quonset palette. Clean/rust geometry, tangents and
  UVs match in all 15 condition/LOD pairs.
* Five states each: pristine, worn, abandoned, intermediate partial, destroyed.
  Four building selections fully collapse; fence topples; wall retains the
  stock short jagged remnant. Two crates have two draped canvas covers.
* `art/mc2-r5/pbr/`: 120 GLBs, three LODs each, authored 2048 basecolour,
  OpenGL tangent normal and ORM atlases baked from the high-detail kit source.
* `content/mc2-r5/`: eight validated IR files, cooker recipe, source-validation
  hashes and `light-sockets.json`. There are 42 light positions across the
  pristine selections, repeated for worn; abandoned/partial/destroyed have none.
* `mapping/counts/<id>/`: stock evidence and signed-off four-side render counts
  for all eight drafted selections. Other inventory entries explicitly retain
  unknown feature counts until their own pre-authoring audit.
* `mapping/drafts-contact-sheet.png` and `mapping/drafts/*-review.png`: stock,
  references, baked mesh, common-scale RTS proxy, partial and final destruction.
* `mapping/proof.json`, `mapping/drafts/REVIEW.md`, `studies/final-*.png`: export
  validation and visual review. The existing 64 art tests pass.

The repair facility has one centred entry on each stock end face, clear of the
four roof ribs. Its four elevated side service closures remain in the source
positions. The lookout's stock texture repeats a door on all four base faces
and three cabin windows on each face; the delivered model matches those counts.
Stock models/textures are read only as count/bounds evidence, never used as
production mesh, UV, texture or tracing data.

```powershell
py -3 tools/scifi_kit/art/inventory.py
py -3 tools/scifi_kit/art/run.py build --pass mc2-r5 --samples 32
py -3 tools/scifi_kit/art/content.py --source E:/mc2-ref/scifi-kit/art/mc2-r5 --out E:/mc2-ref/scifi-kit/content/mc2-r5
py -3 tools/scifi_kit/art/review_drafts.py --pass mc2-r5
```

Remaining weaknesses: dense LOD0 meshes (the quonset is about 85k triangles),
procedural abandoned wear and thin rubble sheets need art polish. RTS fence
contrast remains faint. The source/IR delivery is validated; engine appearance,
runtime LOD transitions and R3 socket consumption are not verified here. The
parked cooker drops empty-node light metadata; the authoritative source GLBs and
sidecar must be consumed by its owning lane. No cooked install is claimed.

## Historical six-draft round

## Inventory and mapping

`G:/mc2-assets/out/scifi-kit/mapping/` contains `mapping.csv`, `mapping.md`,
`inventory.csv`, `mission-usage.csv`, `mission-scan.json`, `model-details.json`,
`QUESTIONS.md` and 45 paginated mapping sheets (`mapping-01.png` onward).
`mapping-contact-sheet.png` is the first-page entry point.

269 placed stock declarations: 268 have decoded base geometry (252 used in stock
missions, 16 only in mods). The extra `dunebuggyprop` declaration has 19 stock
placements but no usable mesh. It is retained as missing, not claimed visible.
The fresh archive-only scan reads all 44 stock mission PAKs plus FIT parts and
resolves every type ID. All 11 differences from the archived census are those
missing dune-buggy placements. Mod counts retain the archived seven-corpus scan.
Counts include potential scripted alternatives, not simultaneous screen visibility.

Rank = stock count times square root of estimated isometric projected area.
Stock TG XZ is the footprint, +Y is height. Authoring metres are stock WU / 4.
Exact model bounds and collision fields remain in the JSON; bounds are not lot
collision masks. Identity notes combine inspected stock crops with lit sRGB
swatches. These are editorial colour notes, not recovered material albedos.

14 questions have recommended defaults applied. The owner per-type folders and
inbox were empty. The supplied 75 MW5 sheets and the expanded BT2018 install
families are indexed in `reference-snapshot.json`; re-read the source catalog and
regenerate this snapshot when continuing extraction adds families.

## Six draft selections

| Stock ID | Rank | Stock placements / missions |
|---|---:|---:|
| razorwirefence | 1 | 1779 / 19 |
| s_lightwall | 3 | 901 / 12 |
| rpbuilding | 11 | 162 / 31 |
| quonsetclean | 12 | 272 / 22 |
| crateswithtarp | 14 | 463 / 25 |
| lookouttower | 17 | 174 / 25 |

Generators in `tools/scifi_kit/art/drafts.py` use original kit geometry. No source
mesh, texture, UV, tracing or reference decal is read by them. Review sheets use
private source thumbnails separately. Stock silhouette and colour take precedence
over reference mass/detail vocabulary. Five palettes plus wood/canvas material
classes retain silver quonset, grey cast wall, green tarp/brown timber, sand repair
facility and sage/khaki lookout identities.

Shared parts now have denser curves and arch straps, three-segment micro bevels,
angle-limited smooth normals, louvres, conduits, junction boxes and fasteners.
Quonset windows have actual apertures. Repair gables close the roof/wall gap.
Damage uses local breaches, exposed supports, matching grounded rubble, folded
wire and collapsed timber/canvas. Unsupported roof windows fall as complete
modules; roof rubble lands on the ground. Evenly spaced rubble vertices prevent the old
chamfer algorithm from producing long spikes.

Final renders/24 GLBs: `G:/mc2-assets/out/scifi-kit/art/blend-d3/` (`glb/` below it).
Six review montages: `G:/mc2-assets/out/scifi-kit/mapping/drafts/*-review.png`.
Combined sheet: `G:/mc2-assets/out/scifi-kit/mapping/drafts-contact-sheet.png`.
Each draft has pristine, worn, abandoned and destroyed beauty views plus an
orthographic RTS proxy at a common 0.24 authoring metres per pixel (360 x 180).
This is an offline readability comparison, not a game capture.

## Reproduce

```powershell
py -3 tools/scifi_kit/art/inventory.py
py -3 tools/scifi_kit/art/run.py build --pass blend-d3 --only razorwirefence,s_lightwall,rpbuilding,quonsetclean,crateswithtarp,lookouttower --samples 32
py -3 tools/scifi_kit/art/review_drafts.py --pass blend-d3
py -3 -B -m pytest tools/scifi_kit/tests/test_art_kit.py -q -p no:cacheprovider
```

Runners pin CPUs 2-3, below-normal priority. One Blender at a time, threads 2.
Validation and visual notes live in `mapping/proof.json` and
`mapping/drafts/REVIEW.md`; no binary outputs are committed.

## Remaining art work

Mappings are first-pass editorial analogues; vehicles, cloth, signs, wreckage and
scrap fences need closer reference choices. Some material reads remain plain and
worn is subtler than abandoned. Thin fence wires need a baked game LOD for RTS
contrast. Destroyed variants are static art poses; they do not prove runtime
collapse or collision. GLBs retain material factors/classes; procedural beauty
weathering needs PBR/atlas baking and LODs. Engine pivot/orientation, placement and
runtime appearance remain for the content-pipeline lane.
