<!-- doc-status: current | reviewed:2026-10-11 -->
# Asset modernization recon — geometry import + texture compression

Recon for extending the (mech) glTF import path to all object classes, and for
compressing textures to save space. Read-only investigation; no implementation.

## TL;DR
- **Every object class shares `TG_TypeMultiShape` + loads via `LoadTGMultiShapeFromASE`** (26 call sites). The strict-opt-in `[Import] Source=` glTF/.fbx hook we built for mechs (branch `claude/assimp-mech-import-1`, `TG_TypeMultiShape::LoadFromFile` probe) is **directly reusable** for vehicles, buildings, props, and trees — ~1 day per class (INI parse + call-site `if (importBase) LoadFromFile else LoadTGMultiShapeFromASE`).
- **Trees are low-poly 3D `.ase` meshes with alpha-test (NOT billboards).** Replacing the "ugly trees" = author/procure modern glTF trees + add the `[Import]` hook to `TreeAppearance`. No engine change needed beyond the import hook.
- **Compression:** static props are the only class with an easy path (the `KtxLoader` consumer already exists). **BC7 for static props** is the cheap first slice (~4×, GPU-native, no transcoder; tgl/128 dims are all multiples of 4 so block-alignment is already satisfied). Mech/vehicle/burnin/UI textures route through `MC_TextureManager`/GameOS (not direct GL) → need a GameOS compressed-upload path first (bigger). **Burnin = ETC1S** (~20×) lands in that later phase.

## 1. Geometry: per-class asset format + loader

| Class | File | AppearanceType | Mesh container | Load call | glTF-extensible? |
|---|---|---|---|---|---|
| Mech | `mclib/mech3d.cpp` | `Mech3DAppearanceType` | `TG_TypeMultiShape` (LOD/shadow/arms/dmg) | `LoadTGMultiShapeFromASE` | **DONE** (strict opt-in, branch `claude/assimp-mech-import-1`, unmerged) |
| Vehicle | `mclib/gvactor.cpp` | `GVAppearanceType` | `TG_TypeMultiShape gvShape[]` +shadow/dmg | `LoadTGMultiShapeFromASE` (lines ~198/216/230/245) | yes — same hook |
| Building | `mclib/bdactor.cpp` | `BldgAppearanceType` | `TG_TypeMultiShape bldgShape[]` +dmg | `LoadTGMultiShapeFromASE` (~217/235/252) | yes — same hook |
| Generic prop | `mclib/genactor.cpp` | `GenericAppearanceType` | `TG_TypeMultiShape genShape` +dmg | `LoadTGMultiShapeFromASE` (~112/134) | yes — same hook |
| Tree/foliage | `mclib/bdactor.cpp` | `TreeAppearanceType` | `TG_TypeMultiShape treeShape[]` +stump | `LoadTGMultiShapeFromASE` (~3391/3414/3434) | yes — same hook |

All meshes: source `.ase` → cached `.tgl`/`.agl` binary. Source glTF conversions
exist **only for mechs** (`A:/Games/mc2-opengl/MC2_Conversions_GLB/<Mech>/<Mech>.glb`);
vehicles/props/trees are `.ase`-only (not a blocker — import is opt-in per INI).

### Extension recipe (per non-mech class), strict opt-in
1. In the class's `init()` INI parse, read optional `[Import] Source=<base>` (strip ext) — mirror `mech3d.cpp` `309cbac2`.
2. At each LOD0 `LoadTGMultiShapeFromASE` call-site: `if (importBase[0]) shape->LoadFromFile(importBase); else shape->LoadTGMultiShapeFromASE(aseName);`
3. Stock assets (no `[Import]`) take the verbatim original path — byte-identical.
Prereq: merge `claude/assimp-mech-import-1` (the importer body + `LoadFromFile` live there; on nifty `assimp_importer.cpp` is still the `-1` stub).

## 2. Trees — replacement options
Trees render as 3D `.ase` meshes (alpha-test + filter, `bdactor.cpp:~3395/3418`), LOD-swapped, via the static-prop batcher + `static_prop.{vert,frag}`. Replacement paths:
- **A. Modern 3D meshes (recommended end state):** procure/author glTF tree models (LODs + 512² albedo), add `[Import]` to tree INIs. Full 3D fidelity, consistent with the import pipeline. Effort: asset work (weeks), engine work minimal (the import hook).
- **B. Better textures/impostors (fast win):** keep `.ase` meshes, author higher-res albedo (+ normal/AO), optional shader depth. Days.
Recommend B for a quick visual lift, A as the real modernization.

## 3. Texture compression — consumer landscape

| Consumer | Source | Current GL upload | BC7 path | ETC1S path |
|---|---|---|---|---|
| **Static props** | `.ktx2` sidecar (RGBA8) via `KtxLoader` | `glTexSubImage3D` RGBA8 into `GL_TEXTURE_2D_ARRAY` + mip overwrite | **Easy** — `glCompressedTexSubImage3D(BC7)`; consumer exists | offline-transcode→BC7, then BC7 path |
| Terrain splat | `mat*_normal.tga` (`terrtxm2.cpp`) | `glTexImage2D` RGBA8, individual 2D | medium (per-tex refactor); low priority (5 textures) | same |
| Burnin/craters | `.tga` via `MC_TextureManager` (`crater.cpp`) | GameOS layer (not direct GL) | needs GameOS compressed path | **target for ETC1S** (after GameOS path) |
| Mech/vehicle | `.tga` via `MC_TextureManager` (`mech3d.cpp`) | GameOS layer | needs GameOS compressed path | — |
| UI/HUD | `.tga` atlases via GameOS (`mechicon.cpp`) | GameOS layer | needs GameOS compressed path | — |

Measured (toktx 4.4.2, tgl/128 sample): **ETC1S ~20× (105→~5MB), UASTC ~6× (→17MB), BC7 ~4× (→26MB)**.

### `KtxLoader` BC7 extension (minimal)
- Accept `VK_FORMAT_BC7_UNORM(145)`/`_SRGB(146)` in addition to RGBA8 (37/43); keep supercompression rejected.
- Make per-level byteLength validation format-aware: BC7 = `ceil(w/4)*ceil(h/4)*16` (vs RGBA8 `w*h*4`).
- Expose `vkFormat`/`isCompressed`; consumer branches to `glCompressedTexImage3D`/`glCompressedTexSubImage3D`.
- **Block-alignment:** BC7 sub-region uploads need 4-px-aligned dims. tgl/128 dims (128/64/32/16/72) are all multiples of 4 → **already satisfied**; a cook-time guard should enforce it for future sets.

### ETC1S (max compression, for burnin)
Simplest: **offline transcode ETC1S→BC7** at cook time (`toktx --encode etc1s` then transcode, or store ETC1S and transcode via vendored libktx/`ktx.dll` at load). Either way the burnin consumer is `MC_TextureManager`-routed, so it needs the GameOS compressed-upload path first.

## 4. Recommended roadmap
1. **COMPRESSION-BC7-STATICPROP-1** — `KtxLoader` BC7 support + batcher `glCompressedTexImage3D`; re-cook tgl/128 as BC7. ~4× (105→26MB), GPU-native, consumer exists, dims pre-aligned. Validate: build + gate-ON smoke (0 GL, visual parity).
2. **ASSIMP-VEHICLE/PROP/TREE-IMPORT** — merge `claude/assimp-mech-import-1`, then add the strict-opt-in `[Import]` hook to `gvactor`/`genactor`/`bdactor` (incl. `TreeAppearance`). ~1 day/class.
3. **TREES-UPLIFT** — option B (textures) now / option A (glTF tree meshes) as modernization.
4. **TEXMGR-COMPRESSED-UPLOAD** — GameOS/`MC_TextureManager` compressed path → unlocks BC7 for mech/vehicle/UI and **ETC1S for burnin** (max compression policy).

## Policy (user-decided)
- Burnin textures → **ETC1S** (max compression, low visibility).
- Everything else → **BC7** (simplest, GPU-native).
Both gated on the respective `KtxLoader`/GameOS compressed-upload work above.
