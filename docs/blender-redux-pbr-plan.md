# Blender redux: Vulkan prop PBR reconnaissance and implementation plan

Task date: owner 2026-10-22. Inspected base `a52697077` on
`claude/blender-redux-pbr`, created from `claude/fence-procedural` in
`A:/Games/mc2-fence`. Reserved install: `E:/mc2-lane5`.

## Decision

Owner approved implementation on 2026-10-22, superseding the original
stop-after-plan decision below. Worktree: `A:/Games/mc2-fence`, branch
`claude/blender-redux-pbr`; install: `E:/mc2-lane5`. Acknowledged pre-existing
scratch files remain untouched and must never be staged.

Original reconnaissance decision: use the stop-after-plan fallback. Vulkan static props
sample albedo only. Normal, roughness, metalness and AO maps cannot be delivered
by merely adding glTF material nodes or cooking texture sidecars. Implementing
them crosses the asset-to-runtime seam, CPU material assembly, Vulkan resource
lifetime/descriptors, per-packet identity and the lighting shader. This is a
multi-step renderer change, rather than a modest material mapping CLI patch.

No engine, asset, install or material changes were made during reconnaissance.
No source assets were copied or downloaded. No game was launched. No before/after
montages exist. The requested contracts, unit harnesses and smoke-lite have not
been run: this checkpoint contains documentation only, not an engine change.

## Why earlier attempts failed / what we do differently

Read-only archaeology on 2026-10-22: inspected the following branch logs and
commit diffs without checking them out or merging them. Historical smoke passes
establish execution, not material quality; no historical montage was re-viewed
in this pass. Do not treat an implementation or a branch name as visual proof.

| History inspected | Finding and consequence for this attempt |
|---|---|
| `559714587`, `e9e3f19d6`, `d18fd8ef1` | StaticPropPbr went ON, then OFF by owner ruling 2026-10-13: most props lacked PBR data; fallback sheen looked like metallic cloth and clustered in one screen corner. The OFF evidence was GL day 25 changed pixels against 20 pixels noise, night zero; this is not strict byte equality and is not Vulkan proof. No fallback specular or sheen is allowed on untagged props in the new path. |
| `ee3016909`, `9a0b0ac35`; BUGS rows 41-44 at `e98f0e3be` | Vulkan gained the old scalar sun lobe, then GL's roughness override 0.95 instead of 1.0. Matching that effect did not supply material maps. Row 42 was a culling mismatch, 43 an unpinned GL shadow-compare issue, 44 the scalar roughness gap; row 41 concerned movement-map refusal, not PBR. Keep geometry/culling/shadow classes unchanged and do not confuse a light artifact with asset quality. |
| `claude/building-materialgpu-v1` (`9ebca1129`) | WIP, default-OFF, tier1 5/5. Shader reads material row zero, uses hardcoded light/view assumptions, adds fixed ambient and multiplies diffuse by AO. Its derivative frame normalizes unchecked vectors. A working GL draw is not a Vulkan material transport. Use per-packet IDs, actual mission inputs, ambient-only AO and a degenerate/mirrored-UV-safe frame. |
| `claude/mech-pbr-material-v1` (`b7cd138c6`); `codex/p1g-building-pbr-rc1` (`f75d8a112`, `bc31d6f32`) | Generic layered paint/metal and triplanar detail were enabled; later defaults returned to passthrough, with a subtle ORM-only profile explicit. The latter branch's name is not evidence of a finished building renderer. Material response must come from reviewed prop/atlas-region mappings, never colour-derived metal or one generic metal coat. Preserve stock albedo hue, pattern and alpha. |
| `claude/mech-material-ssbo-owner-1` (`8cd14c796`); `claude/staticprop-material-ssbo-owner-1` (`14dccb59a`) | GL buffer ownership wrappers preserve calls and lifetime; they do not add missing texture consumers. Reuse current Vulkan resident-resource lifetime mechanisms, updating both descriptor parity sets and unload cleanup. |
| `claude/staticprop-material-orm-normal-recon-1` (`be0443480`, `135ad8bc1`, `1eb534061`); `rescue/mc2-staticprop-material-orm-normal-recon-wip` (`8455d9e5f`) | ORM roughness/metal sampling depended on BC7/coalesce gates. A later fix explicitly disabled sampling outside the bound-array path. Normal mapping remained a design; rescue added capture artifacts, not normal implementation. The review correctly found that moving all Gouraud lighting per fragment cannot preserve unmapped pixels, and called out handedness, degenerate UVs, ABI and double lighting. Only valid authored IDs enter new shading; absent IDs retain old arithmetic and GBuffer output. A neutral material is a separate case from absent material. |
| `claude/sp-material-m0-1` (`360b0815e`, `c448477b6`) | Roughness defaults were reconciled to 1.0; texture identity semantics were documented without rewiring consumers. Keep the 32-byte MaterialGpu ABI if used; explicitly distinguish albedo layers, material IDs and detail layers. Deduplicate by full override/provider/slot identity, not albedo stem. |
| `claude/road-pbr-abandon-1` (`bd7e64f83`, `b2b39d5b7`) | Owner rejected the asphalt look and permanently restored stock roads. Earlier unconditional material selection also made missing-map installs black. Presence must mean successfully decoded usable resources; malformed optional packages fall back safely. Do not revive the road experiment. |
| `claude/water-material-v1` (`d9516f3a4`, `2d9fed308`) | Shoreline noise experiments were parked after blurry wash/speckle failed the visual gate; a binary quad silhouette was a structural limit. Extra texture detail cannot repair an incorrect rendering seam. Judge full gameplay frames before celebrating close-up detail. No water changes here. |
| `terrain-pbr-mod` (`baeb0ab8a`, `b9d8eda24`) | Stale GL terrain/POM/material-splat experiments include fixed isometric view assumptions and sampler-unit hazards. They are not native prop implementation and must not be merged. Terrain palette classification is distinct from per-prop material identity. |
| GL deletion `ec516734a`, `17d7fac41` | Mech GL programs, buffer binding, PBR tuning/wear/back-fill UI and their dead gates were deleted. Do not resurrect those controls or mistake their removed consumers for live Vulkan support. |

Read the requested memories: `mission-data/HANDOFF_2026_06_17_asset_mod_pipeline_v0.md`,
`rendering/classify_materials_workflow.md`, `terrain/ktx2_texture_format_decision.md`
and `mission-data/nonmech_glb_probe_landed.md`. They establish useful ASE/GLB,
classification and cook tools, but explicitly describe missing non-mech material
wiring and LOD0-only import. The TGA/KTX recommendations contradict themselves
(including the nonsensical phrase "BC7 decoder for TGA"); the old proposed KTX
record also differs from today's 32-byte ABI. Follow the live, proven TGA override
route for this packet; compression is not evidence of end-to-end sampling.
`classify_materials.py` emits suggestions requiring review, never approval.

**Acceptance rules for this implementation:**

- No fallback sheen/specular for props without authored PBR. Untagged props must
  remain BYTE-IDENTICAL to today's Vulkan output, proved by deterministic day
  and night captures, not a noise allowance or low whole-frame difference score.
- Never infer metal from colour. An explicit, reviewed mapping selects material
  class per prop/slot and masked atlas region. Rust, paint, cloth and wood must
  not inherit bare-metal response.
- View matched before/after montages at gameplay zoom on `mc2_01` day and
  `mc2_10` night, plus close crops. Inspect all screen corners and camera/view
  changes for glints clustering in one corner; numeric differences alone pass
  nothing. Preserve stock silhouettes, faction colours, pattern and alpha.
- Anything not clearly better on the viewed montages does not ship default-ON.
  Report failed or inconclusive material candidates honestly and leave them out.
  Authored, accepted materials activate by presence; no new default-OFF gate.
- L6 alignment: material evaluation produces `SurfaceMaterial` with linear base
  color, render-space normal, roughness, metallic, AO, linear emission and explicit
  material class. Keep this input independent of the current lighting adapter so
  L6 can consume it unchanged. Do not implement the lighting library in this arc.

## Source evidence (live code takes precedence over historical notes)

| Seam | Finding |
|---|---|
| `mclib/assimp_importer.cpp`, `DeriveMC2TextureName` / `BuildTextureList` | Extracts base-colour/diffuse image names into `TG_Texture` slots. Does not preserve glTF normal/ORM/occlusion material inputs for props. Imported-mech package loading elsewhere in this file is a separate route. |
| `GameOS/gameos/gos_static_prop_batcher_finalize_textures.cpp`, `GpuStaticPropBatcher::buildTextureArrays` | Builds two CPU **albedo** arrays and mip chains, with per-packet layers and UV scales. KTX sidecar support here is albedo support. |
| `GameOS/gameos/gos_static_prop_batcher_finalize.cpp`, material sidecar block | Initializes `normalTex` and ORM absent. Can set an ORM flag only if `s_ormLayerHasMap` says a map exists. |
| `gos_static_prop_batcher_state.cpp` / `_internal.h` | Old ORM flags, presence vector and `deriveOrmSidecar` remain. Repository search finds no code populating that presence vector or calling the sidecar helper. They do not establish runtime map support. |
| `GameOS/gameos/vulkan_shadow_island_live.h`, `Mc2ColorPropMaterialFeed` | Exposes albedo CPU arrays, layer tables, alpha classes, UV scales, sun and hot colours. No normal/ORM arrays or PBR material ID/table. |
| `GameOS/gameos/gos_static_prop_batcher_shadow.cpp`, `mc2_color_prop_island_get_material_feed` | Publishes those albedo arrays and per-packet layer tables. Does not publish `MaterialGpu` rows or PBR textures. |
| `GameOS/gameos/vulkan_color_prop_island.cpp` | Imports the two albedo arrays into resident Vulkan textures; manages two frame-parity descriptor sets. Layout bindings 0–9 are occupied. Draw push constants are 28 bytes. No PBR-map descriptors/material table. |
| `shaders/vulkan/color_prop_tier1.vert` | Produces vertex lighting, world position and transformed geometric normal; the live vertex stream is 40 bytes without tangents. Lighting is clamped before interpolation. |
| `shaders/vulkan/color_prop_tier1.frag` | Samplers at bindings 3/6 are albedo arrays. Colour is albedo times `vArgb`, plus highlight, pool light and optional scalar specular. `propSunSpecular` uses literal metallic=0 and global/default roughness. GBuffer normal is geometric. No normal/ORM/AO sampling and no `MaterialGpu` read. |

`RenderCore/MaterialGpu.h` already defines a 32-byte record with normal and packed
ORM indices (R=AO, G=roughness, B=metalness). Its old consumer comments are not
proof that the live Vulkan shaders consume it. Keep the record ABI if reused.
The existing `rebake_pbr.py` explicitly warns that its baked maps have no proven
static-prop lighting payoff. Historical memory also contradicts itself about
TGA/KTX delivery; follow the working fence bundle's GLB + uncompressed TGA route.
Bake node transforms into vertices for override delivery.

## Smallest coherent implementation, in local commit steps

1. **Material package and CPU seam.** Add an optional, versioned per-slot material
   section to the override package, resolved relative to that mod's manifest.
   Do not change legacy TG vertex or serialized TGL layouts. Resolve by override
   identity and material slot; a texture stem alone cannot distinguish two mods
   using the same albedo. Retain stock albedo as the sole colour source. Package
   normal and ORM as linear, uncompressed TGA; omit PBR diffuse entirely.
   Validate paths, dimensions, channel packing and finite/ranged strengths.
   Set presence flags only after successful decoding. Missing package means no
   material; absent channels use neutral defaults; malformed declared inputs
   produce a diagnostic and safe legacy fallback. Reuse the existing mod merge
   and manifest-directory resolution, rather than inventing another mod search.

2. **Native material ID and resource transport.** Give each draw packet an explicit
   material ID, with a sentinel for legacy packets. Reuse `MaterialGpu` fields
   where possible; declare their Vulkan array-layer semantics at the seam.
   Deduplicate on the full material identity, not only the albedo layer. Build
   compact normal and ORM CPU arrays for mapped materials only, with normalized
   normal mipmaps and linear ORM mipmaps. Publish arrays/table/packet IDs with
   map-generation keys through `Mc2ColorPropMaterialFeed`. Add Vulkan uploads,
   neutral dummy descriptors and map-unload cleanup through the resident texture
   service. Reserve new bindings after checking binding/reflection contracts;
   update layout, pool counts, descriptor writes and both parity sets together.
   Pass material ID in an appended push-constant field shared by VS/FS/C++ and
   every core/fringe draw. Preserve existing alpha coverage and fence UV rules.

3. **Mapped shader branch.** With no valid material, execute the old arithmetic
   and GBuffer write unchanged. For mapped packets only, sample a tangent-space
   normal and ORM by material ID, keeping detail UV tiling separate from albedo's
   padded-array UV scale. Derive a cotangent frame from world-position and UV
   derivatives to avoid changing the vertex ABI; use a geometric-normal fallback
   for degenerate UVs and validate mirrored UV handedness and MC2/Stuff axes.
   A normal written only into GBuffer1 is insufficient: use it for visible
   diffuse/specular lighting too. Extract the existing lighting expression into
   a reusable shader function and evaluate it per fragment only for mapped
   materials, carrying seed/ambient/sun inputs as necessary. Do not recover
   unclamped lighting by dividing or subtracting clamped `vArgb`. Apply AO to
   ambient fill, preserve highlights/emission/pool policy, and let mapped ORM
   roughness/metalness drive the existing modest sun-specular model. Avoid a
   global BRDF rewrite. Preserve GBuffer shadow-class alpha. Mapped materials
   activate by presence, default ON; no default-OFF feature gate.

4. **Material mapping CLI and first prop.** Add
   `tools/blender_pipeline/material_map.py` over the existing extract/package
   pipeline. Input is an explicit mapping JSON (prop, slot, stock albedo,
   detail set, masks, UV scale/orientation, strengths and provenance). Output is
   a mod bundle with `mod.json`, engine-schema `models.json`, baked-transform
   GLBs, private albedo TGAs, linear normal/ORM TGAs and a reproducibility report
   with source hashes. Copy every used source into `G:/mc2-assets/pbr/` first;
   `C:/Users/Joe/Downloads/GameAsset` stays read-only. Omit PBR diffuse by design.
   Copy stock albedo pixels/alpha exactly initially; allow only bounded
   luminance detail after a viewed comparison. Keep stock geometry for the first
   quonset proof. Private names prevent shared-texture changes outside the mod.

5. **Expand only after quonset proof.** Apply hangar, lookout tower, crates and
   walls in that order, committing each validated group. Respect atlas masks,
   window regions, tarp/wood boundaries and rust-versus-bare-metal distinctions.
   Use existing modest geometry as the baseline; do not subdivide as a proxy
   for material quality. Keep gameplay bounds, collision and damage behavior.
   Override activation is intact-only in the existing pipeline; report stock
   destroyed-shape/LOD fallback explicitly rather than claiming those upgraded.

## Initial mapping targets

These are candidates, not visually approved mappings. Texture names below were
read from the stock ASEs in `A:/Games/mc2-srcdata/tgl`.

| Order | Stock prop / colour source | Detail candidate and constraints |
|---|---|---|
| 1 | `quonset2` / `A_Quonset.tga` | Local ambientCG `CorrugatedSteel006A` or `CorrugatedSteel009` normal/roughness/AO. View both previews before choosing the rusty finish. Align corrugation to the stock roof pattern; rust must reduce metalness. Do not confuse this with `quonsetclean` / `A_QuonsetClean.tga`. |
| 2 | `hangar` / `A_Hangar.tga` | Corrugated steel on roof/wall masks; painted metal on painted panels. Existing draft `G:/mc2-assets/out/asset-rework-hangar` is authoring reference, not runtime PBR proof. |
| 3 | `lookouttower` / `A_LookoutTower.tga` | Inspect atlas and stock views before selecting metal/wood/concrete per region. Do not coat the entire atlas with one guessed material. |
| 4 | `woodencrates`, `crateswithtarp` / `Crates.tga` | Wood detail only on wood regions. Both share one atlas; tarp needs an explicit exclusion/mask and rough dielectric response. Inventory a CC0 wood set before use. |
| 5 | `s_lightwall`, `s_mediumwall` / `S_Wall.tga`; `d_lightwall` / `D_LightWall.tga`; `l_lightwall` / `L_LightWall.tga` | Poly Haven `concrete_wall_003` candidate on concrete regions; exclude painted/metal/window areas. Preserve faction colours and atlas patterns. Start with the catalog's high-priority wall types, then expand. |

Local inventory confirms ambientCG corrugated/painted/bare metal packs under
`GameAsset/Materials`; Poly Haven concrete sets are recorded under
`GameAsset/Terrain` in the supplied inventory. No downloads are needed for the
first material. Some concrete sets have no AO: use neutral AO and record that
fact, rather than mislabel displacement as authored AO.

## Verification and acceptance before implementation lands

- Before engine commits: `bash scripts/check-contracts.sh`,
  `py -3 tools/build_contract_harnesses.py --run --unit`, and the documented
  `scripts/tests` pytest tier. Add meaningful cases for missing/corrupt materials,
  full material-key dedup, two slots sharing albedo but differing detail, mod
  precedence, texture channel/mipmap handling and map-unload/reload.
- Reuse this checkout's existing build, RelWithDebInfo only. Commit before build
  and deploy using the repository harness with explicit `E:/mc2-lane5` install;
  resolve actual CLI options with `--help`. Never create an A: build tree.
- Prove no-package compatibility with deterministic baseline/candidate captures
  and pixel equality on both missions; also compare CPU albedo bytes and packet
  routing. Neutral material is a separate test from absent material.
- Test a patterned normal and distinct ORM channels to show that each sampler,
  material ID, UV scale and axis convention reaches the right draw. Verify two
  materials on one prop and multiple mods, not just a single-texture quonset.
- Capture matched before/after views for every modified prop in `mc2_01` day and
  `mc2_10` night, including a close crop and normal gameplay distance. Establish
  visible prop placements/bookmarks first; mission-wide screenshots alone do
  not prove every prop. Use harness-only background launches, host lease and
  `run_visual_capture.py --no-kill` on every invocation. Run smoke-lite with
  retained logs on the explicit lane5 install and record deploy fingerprint.
- Put evidence under `G:/mc2-assets/out/blender-redux-pbr/proof/<prop>/<mission>/`;
  view each montage before acceptance. These are planned destinations, not
  existing artifacts. Inspect render state from lane5 with
  `MC2_DEBUG_STATE_DUMP=1`. Keep existing mod installs working.

No numerical whole-frame difference score substitutes for viewed material proof.
The go/no-go point is the first rusty-quonset day/night pair: retain the stock
look and demonstrate material relief before expanding the bundle.
