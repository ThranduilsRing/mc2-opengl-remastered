<!-- doc-status: current | reviewed:2026-10-11 -->
# Material M0 — GPU material contract pin

**Status:** M0 done (contract pin only). No consumer rewiring, no new PBR
features, no terrain-splat behavior change. Branch `claude/sp-material-m0-1`.

This is the "done" record for milestone **M0** in
`docs/material-abi-unification-recon.md` (the spec; that doc lives in the
`nifty-mendeleev` worktree). M0 pins identity so the GPU material contract
cannot be misread. It is the only behavior delta and it is minimal.

## 1. Roughness default — PINNED to 1.0

The fallback/default roughness disagreed across three sites. The authoritative
producer is the `MaterialGpu` record default; the other two are pinned to match.

| Site | File:line | was | now |
|---|---|---|---|
| Record producer (authoritative) | `GameOS/gameos/gos_static_prop_batcher.cpp` (`m.roughnessFactor`) | 1.0 | 1.0 |
| Static-prop frag fallback | `shaders/static_prop.frag` (`float roughness =`) | 0.6 | 1.0 |
| Asset-viewer shader fallback | `tools/asset_viewer/LocalPbrMaterialBackend.cpp` (`rough =`) | 0.5 | 1.0 |
| Asset-viewer doc comment | `tools/asset_viewer/MaterialRenderBackend.h` (`MaterialSlotTextures::orm`) | 0.5 | 1.0 |

Chosen value follows the recon recommendation: reconcile to
`roughnessFactor` (cook default **1.0**). Each site carries a comment
referencing the others.

**PENDING (user visual review):** props that lack an explicit roughness now
default fully-rough (1.0) instead of 0.5/0.6, so they shift slightly. This is a
USER end-review item; correctness of the new look is NOT claimed here.

## 2. Texture-semantic contract — PINNED (documented, not rewired)

The texture fields (`albedoTex` etc.) of `MaterialGpu` carry a uint32 whose
*meaning* differs per consumer lane:

| Lane | Meaning of texture uint | Semantic | Shader-actionable? |
|---|---|---|---|
| static props | `GL_TEXTURE_2D_ARRAY` layer index | `TextureArrayLayer` | yes |
| mechs | `mcTextureManager` slot/handle | `TextureManagerSlot` | no (compare-only) |
| asset viewer | raw GL texture id (not in SSBO) | `RawGlId` | viewer-local |

Changes (all in `RenderCore/MaterialGpu.h`):
- `MaterialTextureSemantic` promoted from a comment sketch to a real
  `enum class : uint32_t` with documented producer/consumer per value
  (`TextureArrayLayer`/`TextureManagerSlot`/`RawGlId`/`DescriptorIndex`/`BindlessHandle`).
- A canonical "texture-identity meaning per consumer lane" comment block added
  at the `MaterialGpu` record (the `albedoTex` member area).

M0 does **not** store the semantic in the record and does **not** rewire any
consumer — each lane's table stays homogeneous in one semantic; that semantic
is now *declared* rather than inferred at the sample site. Wiring a per-record
semantic is a later milestone (M2/M4, gated on the mech texture-model decision).

ABI lock: `MaterialGpu` keeps its existing `static_assert(sizeof == 32)` plus
per-field `offsetof` asserts — the record layout is already pinned.

## 3. Terrain splat is a SEPARATE palette ABI

Terrain splat material is **not** part of the unified `MaterialGpu` record. It
is a per-pixel-classified fixed-layer palette (`TerrainLayerGpu`-style, 5 fixed
semantic layers — rock/grass/dirt/concrete/snow — selected by a colormap
classifier, with palette-wide loose uniforms). It shares field *vocabulary*
(roughness/normal/tint) with `MaterialGpu` but is a **distinct ABI**: there is
no per-draw `materialIdx` to index a record, identity is computed per-pixel.
Forcing terrain into the per-surface record buys nothing. See
`docs/material-abi-unification-recon.md` §(d).1 and `mclib/terrain.h`.

A short note to this effect is also in the `MaterialGpu.h` record comment.
