<!-- doc-status: current | reviewed:2026-10-26 -->
# Object materials v2: per-object PBR textures

Lane `claude/object-material-v2`. Replaces the "shared 512 detail array + global ObjectTextureRes tier" route for authored
kit, vehicle and imported objects. Owner direction 2026-10-26: do it the way current RTS engines (WARNO) do.

## Current path (before)

- Albedo: `data/tgl/<ObjectTextureRes>/<name>` (default 128), packed into group `sampler2DArray`s at the group's max extent.
  `texel_scale` (kit-intent 84d4debe1) only widens what a tier holds; the tier choice is still global.
- Normal and ORM: the cooker resizes every authored map to 512x512 (`package.DETAIL_EDGE`); `gos_prop_material_cpu.h` packs
  them as layers of two shared arrays (bindings 10/11; one extent per mission, others rejected) sampled at `vUv * uvScale`.
- Team colour (movers): `GVAppearance::setPaintScheme` recolours whole albedo regions on the CPU by dominant channel, one
  texture per paint key.

## Target (after)

Per-object textures, one image per file with its own extent, format and complete mip chain:

| Map | Format | Notes |
|---|---|---|
| albedo | BC7 (sRGB-encoded bytes, UNORM view like the tier arrays) | optional; replaces the tiered tgl texture |
| normal | BC5 (XY, Z rebuilt in shader) | tangent space |
| ORM | BC7 linear | R = AO, G = roughness, B = metal |
| team mask | BC7 linear (movers) | R = base, G = highlight, B = secondary |

- **Manifest** (`models.json`, `materials.version 2`): a slot may name `albedo`, `normal`, `orm` (all `.ktx2`). The row sets
  `MaterialFlags::kPerTexture`; its `albedoTex/normalTex/metallicRoughnessTex` fields are indices into one descriptor table.
  Maps of a slot need not share a size. TGA or mixed inputs (older mods) keep the legacy array route unchanged.
- **Vulkan:** one `sampler2D uPropTex[1024]` binding (13) in the existing colour-prop descriptor set, PARTIALLY_BOUND
  (descriptor indexing is already a required device feature). Written once per map load next to the material table; no
  per-draw descriptor churn, the per-draw datum stays the existing `materialId` push constant. Sampling uses
  `nonuniformEXT`. `static_prop`/colour-prop is the only consumer; mech/terrain sets are untouched.
- **Shader:** `propSurface` / `propMaterialCoverage` pick the table when `kPerTexture`; `propAlbedoOverride` replaces the tier
  albedo in the alpha-OFF branch. Lighting composition is unchanged.
- **Cooker:** `_detail` keeps each map's own power-of-two extent (cap 2048, resample only when it must) instead of 512;
  authored opaque albedo is emitted as `albedo.ktx2`; the validator accepts any pow2 <= 2048. The old shared-extent array
  route stays only as the reader for TGA/legacy inputs; the cooker no longer feeds it.
- **Team mask (mover slice):** tint in the shader, never a recolour: `lum = dot(albedo, w)`; `tinted = teamColour/lum(teamColour) * lum`;
  `albedo' = mix(albedo, tinted, maskWeight)`. The albedo's luminance, stripes and dirt survive under the mask.
- **Residency / LOD:** the sampler selects mips from screen derivatives, so size on screen picks the mip; LOD0/1/2 packets use
  the same texture. Potato drops the top mip from staging and residency. Low/Potato (`lowCost`) skip per-object maps and keep
  the albedo-only legacy route. Real streaming (dropping resident mips by distance) is not built: vehicles draw LOD2 at RTS zoom
  so the working set is the 2048 chain; budget below.
- **Batching:** no change. Draws still group by type; the material index is per packet already.

## Movers (status)

Override vehicles do not reach the static-prop batcher: their paint-variant bodies are imported on first render, after
`finalizeGeometry`, so `registerType` drops them (`[GPUPROPS] late registerType ... CPU-fallback`) and they draw through the
legacy shape island (albedo + vertex light). Consequences:
- **Shadow (fixed):** `captureMoverShadowShape` found no recipe for the override body, so override vehicles cast none. The
  capture now uses the stock shape, which is registered at load and carries the same pose.
- **Per-object normal/ORM/team mask for vehicles (not built):** needs the variant bodies in the batcher. Either pre-register
  each vehicle's (LOD, paint) variants before finalize, or give the batcher an append region after finalize. Team paint then
  becomes per-packet colours (three RGB8 in the material row) tinted in the shader as above.

## Stock decision: Option B (manifest-gated fallback)

Option A (cook stock into the new format) is rejected: the six mods and every mod-authored asset ship legacy TGAs in
`data/tgl/<tier>/` that load at runtime, so the legacy path cannot be deleted anyway, and cooking stock would only add risk to
"stock captures identical". A slot takes the new path only when its manifest names KTX2 maps; everything else is byte-identical.

## Memory budget

Per asset at 2048: BC7 albedo 5.6 MB, BC5 normal 5.6 MB, BC7 ORM 5.6 MB, mask 5.6 MB (with mips). A mission holding 10 distinct
authored assets is about 170-220 MB. Shipping assets should use 1024 for most kit pieces (about 1/4); 2048 is for hero assets
(vehicles at close zoom). Table cap 1024 textures (about 250 authored assets).
