# Material ABI Unification Recon — `MATERIAL-ABI-UNIFICATION-RECON-1`

**Status:** recon only. No code, shader, or ABI changes. Snapshot of
`claude/nifty-mendeleev` worktree, 2026-06-11.

**Question:** can one runtime GPU-facing material record shape serve static props,
mechs, VFX meshes, terrain splat, and asset-viewer previews?

**One-line answer:** **Yes for props + mechs + previews + (mesh-style) VFX — the
record already exists (`RenderCore/MaterialGpu`, 32 B std430) and only needs the
texture-identity semantic unified + a few fields (emissiveFactor, alphaMode bits,
alphaTestThreshold). Terrain splat does NOT fit the per-surface record model and
should stay on its own palette ABI; billboard particles need only a thin
"VFX material" view (texture + blendMode + atlas grid), which the unified record
can carry but does not need to.**

---

## (a) Inventory — every existing material record/struct

### 1. `RenderCore::MaterialGpu` — THE candidate unified record (LIVE)
`RenderCore/MaterialGpu.h:88-117`; GLSL mirror `shaders/include/material_gpu.hglsl:46-54`;
mirror gate `scripts/check-material-gpu-mirror.sh`; offsets pinned by
`shaders/fixtures/material_gpu_contract.frag` + `tools/shader_reflect/reflect.py`.

```cpp
struct alignas(4) MaterialGpu {           // 32 B, std430-locked, static_assert'd
    uint32_t albedoTex;             //  0  KIND-SEMANTIC (see below)
    uint32_t normalTex;             //  4  unwired (kMaterialTexAbsent everywhere)
    uint32_t metallicRoughnessTex;  //  8  unwired (R=AO G=rough B=metal)
    uint32_t emissiveTex;           // 12  unwired
    uint32_t flags;                 // 16  MaterialFlags (MaterialGpu.h:66-83)
    float    baseColorFactor;       // 20
    float    metallicFactor;        // 24
    float    roughnessFactor;       // 28
};
```
Flags (`MaterialGpu.h:66-83`): `kAlphaTest`(1<<0), `kNormalMap`(1<<1),
`kMetallicRoughness`(1<<2), `kEmissive`(1<<3), `kDoubleSided`(1<<4), `kWindow`(1<<5).
Sentinel `kMaterialTexAbsent = 0xFFFFFFFF` (`MaterialGpu.h:83`).

**The load-bearing divergence is already documented in the header
(`MaterialGpu.h:22-37`): `albedoTex` semantics are per-consumer:**
- static props: `GL_TEXTURE_2D_ARRAY` layer index — shader-actionable
  (`gos_static_prop_batcher.cpp:3144-3152` populates; `static_prop.frag:60` samples
  via MaterialTable SSBO binding 5).
- mechs: `mcTextureManager` texHandle — **compare-only**, not shader-actionable
  (`gos_mech_batcher.cpp:1549-1583` builds `s_mechMaterialTable`, SSBO binding 2
  at `gos_mech_batcher.cpp:1700-1702`; shader never reads it). Per-instance
  `materialIdx` at `GpuMechInstance` byte 52 (`gos_mech_batcher.h:49-50,68`).
- VFX/future: undefined.
A typed `MaterialTextureSemantic` enum is already sketched in the header
(`MaterialGpu.h:40-52`) as the future direction.

### 2. Static-prop sidecar/inspector records (CPU-only, not ABI)
- `StaticPropMaterialInventoryEntry` (`gos_static_prop_batcher.h:514-530`, 108 B,
  offset-asserted): materialIdx, albedoTexLayer, alphaGroup, flags, nodeIdx,
  dims, usageCount, textureName[64], metallic/roughnessFactor. Inspector mirror
  dup-decl in `GuiRuntime/EditorInspector.cpp:23`.
- `StaticPropTypeMaterialCache` (`gos_static_prop_registry.h:180-192`): per-typeID
  primary packet — texArrayLayer, materialIdx, alphaClass, packetCount.
These are *views* of MaterialGpu, not parallel ABIs — they unify "for free" once
MaterialGpu is the substrate.

### 3. Mech draw-call material state (uniform-era, pre-record)
- Bucket key carries `texHandle` + `materialFlags` (`gos_mech_batcher.cpp:1488-1516`);
  flags pushed as plain uniform `u_materialFlags` (`gos_mech_batcher.cpp:1894-1895`).
- Lighting/material data the mech shader actually uses: per-draw `u_tex` 2D albedo
  + per-vertex `calc_light`; **no MaterialGpu sampling, no roughness/metallic, team
  color CPU-baked into albedo** (`docs/mech-lighting-plan.md` table; `mech3d.cpp:1725`).

### 4. Terrain splat "material" (palette, NOT a record)
There is **no per-surface material record**. Terrain materials are **5 fixed
semantic layers** (rock/grass/dirt/concrete/snow) selected per-pixel by a
colormap **classifier**, with per-layer tuning as loose uniforms:
- samplers: `matNormal0..4` / `matNormalArray` layers `MAT_LAYER_*`
  (`shaders/gos_terrain.frag:45-55`; chunk path `shaders/terrain_lod_chunk.frag:108,182-194,270-271`).
  Alpha channel = displacement for POM.
- per-layer scalars: `matNormalBoost` vec4 + tiling + tint
  (`gos_terrain.frag:131-146`; setters `gameos_graphics.cpp:1582-1605,8307-8315`).
- mission-gated profile enum `TerrainMaterialProfile` (`mclib/terrain.h:83-94`,
  explicitly marked "disposable — until the real material-palette architecture lands").
Identity here is *classified per-pixel*, not assigned per-draw → fundamentally a
different binding model from MaterialGpu's per-draw `materialIdx`.

### 5. VFX (billboard particles + trails)
- `GpuParticle` (`mclib/particles/spec.h:48-58`, 64 B std430-asserted): position,
  color, velocity, kind_flags, lifetime, age, size, atlasIndex. **No material fields.**
- Material-ish state lives in `GroupInfo` (`mclib/particles/batcher.h:40-54`):
  `handle` (MLR→gos texture), UV sub-rect u0/v0/us/vs, `blendMode` (0=alpha,
  1=additive), `atlasColumns` (flipbook grid). Blend state set CPU-side by the
  bridge (`shaders/particle_billboard.frag:12-13` comment; `gos_particle_bridge.cpp`).
- gosFX card/cloud CPU legacy path: MLR texture pool, no record.
So the VFX "material" = {texture, blendMode, atlas grid, UV rect} — per *group*,
not per surface; no normal/ORM/lighting at all (unlit billboards).

### 6. Asset-viewer preview (Backend A/B)
- `MaterialSlotTextures` (`tools/asset_viewer/MaterialRenderBackend.h:8-13`): raw GL
  tex ids for baseColor/normal/orm/emissive, 0 = absent with defined fallbacks
  (flat normal; AO=1 rough=0.5 metal=0; no emission).
- Shader contract = loose uniforms `u_baseColor/u_normalTex/u_ormTex/u_emissiveTex`
  + `u_hasNormal/u_hasOrm/u_hasEmissive` ints (`tools/asset_viewer/LocalPbrMaterialBackend.cpp:38-50`).
- Authoring source: `FitMaterial` (`tools/asset_viewer/FitMaterialLoader.h:7-16`):
  baseColor/normal/orm/emissive paths + shader + ormPacking + alphaMode strings.
- Backend-A compile spike proves the *runtime* static_prop shaders link in the
  viewer (`docs/asset-viewer-backend-a-shader-contract.md`) — the viewer can adopt
  the runtime MaterialGpu lane directly.

### 7. Manifest / cook (authoring-side, feeds everything)
`docs/asset-manifest-schema.md` + `tools/validate_asset_manifest.py:79-310`:
`textureRefs[].slot ∈ {albedo,normal,orm,emissive,mask}` with locked slot→colorSpace
(albedo/emissive sRGB vkFormat 43/146; normal/orm/mask linear 37/145);
`materials[].alphaMode ∈ {opaque,alphaTest,blend}` + `alphaTestThreshold` +
`doubleSided` + `pbr.{baseColorFactor,metallicFactor,roughnessFactor}`; normal slot
⇒ `capabilities.hasTangents=true`. Cook presets in `tools/mc2texcook/mc2texcook.py:226-269`.

---

## (b) Field union matrix

Legend: **L**=live/wired, **S**=slot exists in record but unwired (dead data),
**U**=loose uniform/CPU state (not in a record), **—**=missing, **X**=incompatible model.

| Field | Prop | Mech | Terrain splat | VFX (billboard) | Preview |
|---|---|---|---|---|---|
| baseColor texture | **L** (array layer, `batcher:3144`) | **S** (texHandle, compare-only) | **X** (classifier picks layer per-pixel) | **U** (`GroupInfo.handle`) | **L** (`u_baseColor`) |
| baseColorFactor | **L** (=1.0) | **S** | **U** (tint colors/strength) | **U** (per-particle `color[4]`) | — (implicit 1) |
| normal map | **S** (`normalTex`, no tangents — `staticprop-material-orm-normal-recon.md` §5) | **S** (`a_tangentOct` zero-filled) | **L** but per-layer (`matNormalArray`) | — (unlit) | **L** (`u_normalTex`+`u_hasNormal`) |
| ORM map | **S** (`metallicRoughnessTex`) | — | partial: displacement in matNormal alpha; no AO/rough/metal maps | — | **L** (`u_ormTex`) |
| metallic/roughnessFactor | **L** (dead-data→PBR-2 consumer planned, `v-material-pbr-2-plan.md`) | — | — | — | **L** (from ORM defaults) |
| emissive map/factor | **S** (`emissiveTex`); **no emissiveFactor scalar in record** | — | — | — (additive blend is the "emissive") | **L** (`u_emissiveTex`) |
| alphaMode | **L** partial: `kAlphaTest` flag; threshold hardcoded 0.5; **no blend mode in record** (alpha grouping is a batcher bucket concept, `alphaGroup`) | **U** (alpha-test in shader, flag via `u_materialFlags`) | **X** (opaque only) | **U** (`GroupInfo.blendMode`, draw-state not record) | **U** (`FitMaterial.alphaMode` informational) |
| blendMode (alpha vs additive) | — | — | — | **U** (`batcher.h:48-49`) | — |
| cull / doubleSided | **S** (`kDoubleSided` flag, unconsumed) | — | n/a | n/a (billboards) | — |
| texture identity semantic | array layer | texmgr slot | classifier layer enum | gos handle (per group) | raw GL tex id |
| IBL participation | **U** (`u_iblSh`, `u_iblShStrength` — per-pass uniforms, NOT per-material; `ibl-plan.md`) | — (hemisphere planned, `mech-lighting-plan.md`) | — | — | analytic only |
| window/hot-color | **L** (`kWindow` + per-vertex aRGB tag) | — | — | — | — |
| debug fallback per slot | **L** convention (`kMaterialTexAbsent` → flat normal / AO=1 / black; `staticprop-...recon.md` §4) | sentinel idx 0 | — | — | **L** (0 ⇒ slot defaults, but **rough default 0.5 vs runtime 1.0 — mismatch**, also frag fallback 0.6 vs table 1.0 noted at `staticprop-...recon.md` §6) |

Key incompatibilities surfaced:
1. **Texture identity** — four different meanings of "texture field" (layer / texmgr
   slot / classifier layer / GL id). This is the #1 blocker and already has a
   designed fix (`MaterialTextureSemantic`, `MaterialGpu.h:40-52`).
2. **Terrain identity model** — per-pixel classified palette vs per-draw record. Not
   a field problem; a *binding-model* problem.
3. **Blend mode** lives in draw-state (props: alpha bucket; VFX: GroupInfo), not in
   the record — std430 records can carry it, but GL blend state can't be read per
   fragment, so it stays a *batching key* either way.
4. **Fallback constants disagree** (roughness 1.0 vs 0.5 vs 0.6) — trivial but must
   be pinned in the unified contract.

---

## (c) Proposed unified record ABI

Keep `MaterialGpu` as the base — it is already std430-locked, mirror-gated, and
live in two lanes. Extend to 48 B (still 16-byte friendly stride):

```cpp
struct alignas(4) MaterialGpuV2 {                  // 48 B std430
    // texture refs — uniform semantic (see texSemantic)
    uint32_t albedoTex;             //  0
    uint32_t normalTex;             //  4
    uint32_t ormTex;                //  8   (rename of metallicRoughnessTex; R=AO G=rough B=metal)
    uint32_t emissiveTex;           // 12
    uint32_t flags;                 // 16   existing MaterialFlags bits 0-5 unchanged
                                    //      + bits 8-9: alphaMode (0=opaque,1=alphaTest,2=blend,3=additive)
                                    //      + bits 12-15: texSemantic (TextureArrayLayer /
                                    //        TextureManagerSlot / DescriptorIndex / RawGlId-viewer-only)
    float    baseColorFactor;       // 20
    float    metallicFactor;        // 24
    float    roughnessFactor;       // 28
    // --- new tail (additive; old 32-B readers unaffected if stride bumped atomically) ---
    float    emissiveFactor;        // 32
    float    alphaTestThreshold;    // 36   (today hardcoded 0.5; manifest already validates it)
    uint32_t atlasGrid;             // 40   packed u16 cols | u16 rows; 0/1 = none (VFX flipbook;
                                    //      also future prop atlas sub-rects)
    uint32_t reserved0;             // 44   (future: iblProbeIdx / teamMaskTex / detail layer)
};
```

Design rules:
- **One semantic per table, declared not implied**: each consumer's table is
  homogeneous in `texSemantic`; the long-term target is everyone on
  `TextureArrayLayer` (props today) or `DescriptorIndex` (when mechs get a real
  texture model — the gating decision in
  `docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md`).
- **Per-slot absent = `kMaterialTexAbsent`** with pinned fallbacks: normal→(0,0,1),
  ORM→(1, roughnessFactor, metallicFactor), emissive→black. Reconcile the 0.5/0.6/1.0
  roughness defaults to **roughnessFactor (cook default 1.0)** everywhere, including
  `MaterialSlotTextures` (`MaterialRenderBackend.h:11`) and `static_prop.frag:346`.
- **Blend/cull remain batch keys** derived FROM the record (alphaMode/doubleSided
  bits decide which bucket/pass), never GPU-divergent state.
- ABI change requires lockstep: `material_gpu.hglsl` mirror + mirror-check script +
  `material_gpu_contract.frag` fixture + reflect goldens + manifest validator —
  exactly the existing gates, one PR.

SSBO layout: unchanged model — one `MaterialGpuV2[]` SSBO per lane (props binding 5,
mechs binding 2 per `docs/render-binding-registry.md`), indexed by per-draw/per-instance
`materialIdx`. Unification of *shape* does not require unifying the *buffer*.

---

## (d) What cannot unify, and why

1. **Terrain splat layers — keep separate (palette ABI).** Material identity is
   computed per-pixel by a colormap classifier over 5 fixed semantic layers
   (`gos_terrain.frag:45-55`, `terrain_lod_chunk.frag:150,182-194`); there is no
   per-draw materialIdx to index a record with, and per-layer knobs (boost, tiling,
   tint, POM displacement, anti-tile) are palette-wide, not per-surface. The right
   unification for terrain is the planned "real material-palette architecture"
   (`mclib/terrain.h:83-87`) — a `TerrainLayerGpu[5]` palette record that may *embed*
   MaterialGpuV2-style texture refs per layer, but it is a different table with a
   different indexer. Forcing terrain into the per-surface record buys nothing.
2. **Per-particle state stays in `GpuParticle`.** color/age/size/atlasIndex are
   per-instance animation, not material. The unified record can absorb the *group*
   material (texture + blendMode via alphaMode bits + atlasGrid), replacing the
   material half of `GroupInfo` (`batcher.h:40-54`) — but UV sub-rect (u0/v0/us/vs)
   is a frame-addressing concern that should ride atlasGrid + atlasIndex, not the
   record. gosFX CPU/MLR legacy path is out of scope (being retired toward the GPU
   bridge).
3. **GL blend/depth/cull state itself** — records describe, passes enact. A record
   field can't switch GL blend mid-draw; sorting into passes by alphaMode remains a
   CPU batcher responsibility in every lane.
4. **Mech team color** — CPU-baked into albedo at load (`mech3d.cpp:1725`); there is
   no GPU mask source, so no record field can represent it yet (reserved0 candidate
   once a team-mask arc exists; `mech-lighting-plan.md` explicitly forbids inventing
   one).
5. **Window/hot-color magic** is half per-vertex (`a_aRGBLight` tag) — the `kWindow`
   flag unifies, the vertex tag does not.

---

## (e) Migration order (least-risk first)

1. **M0 — Contract pin (docs/tests only).** Pin per-slot fallback constants (kill the
   0.5/0.6/1.0 roughness divergence), alphaMode bit assignments, texSemantic enum
   values. Extend `validate_asset_manifest.py` + fixtures. No runtime change.
2. **M1 — Viewer adopts the runtime record.** Asset viewer already compiles the
   runtime static_prop shaders (Backend-A contract doc). Replace
   `MaterialSlotTextures` + loose uniforms with a 1-entry MaterialGpu(V2) SSBO +
   the runtime `material_gpu.hglsl`. Proves the record drives a full PBR preview;
   zero game risk.
3. **M2 — ABI bump 32→48 B** (one PR, all lockstep gates): add emissiveFactor /
   alphaTestThreshold / atlasGrid / reserved0 + alphaMode/texSemantic bits. Both
   live producers (`gos_static_prop_batcher.cpp:3144`, `gos_mech_batcher.cpp:1566`)
   updated; behavior byte-identical (new fields = defaults).
4. **M3 — Static-prop ORM wiring** (already specced as
   `STATICPROP-ORM-TEXTURE-ARRAYS-1` → `STATICPROP-SHADER-ORM-GATED-1` in
   `staticprop-material-orm-normal-recon.md` §8) — first consumer of the unified
   slots beyond albedo.
5. **M4 — Mech texture-model decision** (the blocking arc): move mechs from
   TextureManagerSlot to a shader-actionable semantic (array layer or descriptor
   index). Only after this does `albedoTex` mean one thing everywhere that samples.
6. **M5 — VFX group-material adoption**: `GroupInfo` material half → materialIdx
   into a small VFX material table (texture + alphaMode + atlasGrid). Optional;
   do when flipbook/asset-table work next touches the bridge.
7. **M6 — Terrain palette record** (separate ABI, shared field vocabulary): replace
   the C1 profile enum + loose boost/tiling/tint uniforms with `TerrainLayerGpu[5]`
   whose per-layer texture refs reuse the V2 semantics. Retires
   `TERRAIN_MAT_PROFILE_*` per its own removal note.
8. **M7 — Normal-map arc** (props tangents + per-fragment diffuse; mech tangentOct
   fill) — last, per the split-granularity risk in `staticprop-...recon.md` §6.

---

## (f) How asset-cook sidecars feed it

The authoring chain is already shaped for exactly this record:

- **Manifest** (`docs/asset-manifest-schema.md`, validator
  `tools/validate_asset_manifest.py:79-310`): `materials[]` carries
  alphaMode/alphaTestThreshold/doubleSided/pbr factors → map 1:1 onto V2 fields/flag
  bits; `textureRefs[].slot` maps to the 4 texture slots with the locked
  slot→colorSpace convention (albedo/emissive sRGB, normal/orm/mask linear).
  `hasTangents` cross-check gates normalTex ≠ absent.
- **Texture cook** (`tools/mc2texcook/mc2texcook.py:226-269` presets;
  `batch_cook.py --bc7` needs the slot parameter per `staticprop-...recon.md` §4):
  emits `.ktx2` per slot; `KtxLoader.cpp:161-165` already surfaces `isSrgb`/`vkFormat`
  so the runtime array builder picks the right internalformat per usage array.
- **Runtime resolve**: cook does NOT bake `albedoTex` indices — they are
  load-time-assigned (array layer dedup at `finalizeGeometry`,
  `gos_static_prop_batcher.cpp:2536-2699`; mech handle map
  `gos_mech_batcher.cpp:2056-2063`). The sidecar feeds *names + factors + flags*;
  the lane's table builder resolves names → its texSemantic and writes the record.
  This split (cook = semantic intent, runtime = texture identity) is what lets one
  record shape serve lanes with different texture backends.
- **Viewer**: `FitMaterial` (`FitMaterialLoader.h:7-16`) is a manifest-lite; M1
  should converge it on the manifest material block so viewer previews exercise the
  same cook output the game consumes (BundleExport/CentralManifestMerge in
  `tools/asset_viewer/` are the existing plumbing points).

---

## Cross-references
`docs/staticprop-material-orm-normal-recon.md` (slot wiring plan, fallback table),
`docs/v-material-pbr-2-plan.md` (first scalar consumer), `docs/mech-lighting-plan.md`
(mech data audit), `docs/asset-viewer-backend-a-shader-contract.md` (viewer↔runtime
shader proof), `docs/asset-manifest-schema.md` (authoring contract),
`docs/render-binding-registry.md` (SSBO 2/5 ownership), `docs/ibl-plan.md`
(IBL stays per-pass, not per-material),
`docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md` (M4 gate).
