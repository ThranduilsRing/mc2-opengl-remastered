# Model Override System — Recon & Design (`MODEL-OVERRIDE-SYSTEM-RECON-1`)

**Status:** Recon / design only. No runtime replacement implemented in this slice.
**Branch:** `claude/model-override-system-recon-1` (worktree `A:/Games/mc2-model-override-recon`, from `claude/nifty-mendeleev`).
**Date:** 2026-06-02.
**Goal:** Design the first modder-friendly model override system for MC2 — drop a validated glTF/GLB into a mod folder, declare which stock asset it replaces, keep stock gameplay/collision. Priority targets: **trees + static props**.

---

## 0. Executive summary

The single most important recon finding: **the engine already imports glTF/GLB into the runtime mesh format.** `mclib/assimp_importer.cpp` (~416 lines, gated `ENABLE_ASSIMP_IMPORTER=ON`) converts an Assimp scene directly into a `TG_TypeMultiShape`, and `TG_TypeMultiShape::LoadFromFile()` (`mclib/msl.cpp:438-479`) already probes `.glb → .fbx → .ase` in priority order. The current consumer is the mech import path (`mclib/mech3d.cpp:311`, opt-in via `[Import] Source=` in the `.ini`); stock mechs with no `[Import]` block take the legacy `.ase` path unchanged.

This means the model-override MVP is **not** a "build a glTF importer" project. It is a **"point the existing glTF importer at trees/static props, keyed by a mod manifest, with stock gameplay frozen"** project. The hard work (geometry conversion, coordinate transform, UV flip, bounds, texture name extraction) is done. The missing work is identity resolution, the manifest layer, and the render-only guarantee.

**Recommended MVP: `TREE-MODEL-OVERRIDE-MVP-1`** — one tree or static prop, render mesh only, glTF/GLB source via existing Assimp path, stock collision/gameplay preserved, stock fallback on failure, mod-folder manifest. Asset-viewer validation deferred to a follow-on.

---

## 1. Replacement identity key

### Identity chain (evidence)
```
appearance .ini "FileName" → .ase basename → .tgl compiled name → runtime TG_TypeMultiShape
```
- `.ase` filename read from appearance INI: buildings `mclib/bdactor.cpp:188,199`; trees `mclib/bdactor.cpp:3362,3373` (`iniFile.readIdString("FileName", aseFileName, 511)`).
- `.tgl` name is **derived** from `.ase` basename — no independent mapping: `mclib/msl.cpp:586` (`_splitpath`), `mclib/msl.cpp:594` (`binaryFileName.init(tglPath, name, ".tgl")`). So `warehouse01.ase → data/tgl/warehouse01.tgl`.
- `AppearanceType::name` is set once at type load (`mclib/apprtype.cpp:68-69`, `strcpy(name, fileName)`), looked up via case-insensitive compare (`mclib/apprtype.cpp:230`), and captured into the static-prop registry (`GameOS/gameos/gos_static_prop_registry.cpp:251-256`, `RecipeRange::shapeName[128]`). Stable across all instances; never mutates.

### Candidate ranking for `"replaces"`
| Candidate | Stability | Uniqueness | Verdict |
|---|---|---|---|
| `AppearanceType::name` (appearance config name) | High — set once at load, in registry + diagnostics | Per appearance config | **Primary key** |
| `.ase` basename | High — but only meaningful for shapes sourced from `.ase` | Per source file | Effectively equal to appearance name in practice |
| `.tgl` name | High — but derived from `.ase`, redundant | Per compiled file | Avoid (couples mod to compiled artifact) |
| FIT entry | Indirect — references appearance, not model | n/a | Not a model key |
| Static prop numeric type id | Unstable — assigned at registration order | Per session | **Reject** for manifests (use only internally) |

### Decision
Manifest `"replaces"` uses a **typed appearance-name key**:
```
"<class>:<appearanceName>"   e.g.  "tree:oak_big"   "building:warehouse01"   "prop:barrel"
```
The `<class>` prefix disambiguates across appearance categories and lets the loader scope the hook (a tree override never collides with a building of the same name).

### Stop-condition guard: alias registry
Because the appearance-name → on-disk-asset mapping is implicit (INI `FileName`), and a single appearance can spawn multiple shapes (base + damage states, helpers), identity is *not* guaranteed unambiguous for every asset. **Ship an explicit alias registry** (`overrides/aliases.json` or a generated `stock_asset_index.json`) that maps stable `<class>:<appearanceName>` keys to the resolved stock `.ase`/`.tgl` and lists the damage-state variants. The asset viewer (later) populates this index by scanning loaded appearances. The MVP can hand-author the index for the single target asset.

---

## 2. Loader hook (smallest blast radius)

### Recommended hook: `TG_TypeMultiShape::LoadFromFile()` — `mclib/msl.cpp:438-479`
This is the correct hook, **not** the raw `LoadTGMultiShapeFromASE`. `LoadFromFile` is the existing format-probe funnel (`.glb → .fbx → .ase`) and is already the point where import-vs-stock is decided. Inserting override resolution here means:
- **Per-type, once at appearance init** — not per-instance, not per-frame. Buildings call it via `mclib/bdactor.cpp:217,235,252` (base + damage), trees via `mclib/bdactor.cpp:3391,3414,3434`.
- **All paths converge** — stock `.ase`, binary `.tgl` cache, and `.glb`/`.fbx` import all flow through this one function.
- **Full identity available** — the caller passes the resolved name; the appearance subclass (`BldgAppearanceType` / `TreeAppearanceType`, `mclib/bdactor.h:48-450`) determines the class.
- **Immediate return contract** — caller only inspects the return code, not internal structure, so substituting the source file is transparent upstream.

### Hook behavior
```
LoadFromFile(name, ...):
  base = basename(name)
  cls  = caller class (tree | building | prop)
  ov   = ModelOverrideRegistry::resolve(cls, base)     # manifest lookup, built at mod-mount time
  if ov and ov.cookedOrSourceExists:
      try ImportGeometryFromFile(ov.path, this)         # existing assimp path
        on success → recompute render bounds (see §4) → return OK
        on failure → log, fall through to stock
  # stock path unchanged:
  probe .glb → .fbx → .ase as today
```
The registry is built once when mods are mounted (directory scan + manifest parse) so the hook is a cheap map lookup. Blast radius: one function, one new registry module, zero changes to per-instance or per-frame code.

---

## 3. glTF / cook strategy

### What already exists (do not rebuild)
- **Assimp vendored** at `3rdparty/assimp/`; `ENABLE_ASSIMP_IMPORTER=ON` (`CMakeLists.txt:204`).
- **Importer** `mclib/assimp_importer.{h,cpp}`: `ImportGeometryFromFile(path, TG_TypeMultiShape*)` (`assimp_importer.h:35`, impl `assimp_importer.cpp:349-413`). Does: `ReadFile` with `aiProcess_Triangulate | aiProcess_GenSmoothNormals`; node-name validation (≤24 chars, unique); per-mesh vertices+normals+triangles+UVs; diffuse material-name extraction with V-flip; coordinate transform (`MC2.x=-src.x, MC2.y=src.z, MC2.z=src.y`); bounding box (`ComputeBoundingBox`, loose, node-center based).
- **Probe** `LoadFromFile` `.glb→.fbx→.ase` (`msl.cpp:438-479`).
- **Standalone geometry probe** `tools/cpp_probes/assimp_probe.cpp` (deleted by DI-7; recover from git history) emits JSON (`meshCount, vertexCount, indexCount, materialSlotCount, hasNormals, hasTangents, bounds`) — reuse for asset-viewer/validation.
- **Manifest schema + validator** already drafted: `docs/asset-manifest-schema.md`, `scripts/check-asset-manifests.py`, `tools/asset_cook/validate_asset_manifest.py`.

### What is missing / explicitly deferred (MVP scope statement at `assimp_importer.cpp:3-6`)
Animation, LOD swap, shadow mesh, palette swap, `.tglc` cook cache. None of these block a static-prop/tree render-only MVP (props don't animate; trees are static). The binary cook path (`SaveBinaryCopy/LoadBinaryCopy`, `msl.cpp:482-503`) exists but is legacy-ASE-only and **not** wired to the importer — cook-at-startup is a Phase-2 optimization, not an MVP requirement.

### Decision: **(a) direct glTF runtime import**
The MVP is **direct glTF/GLB runtime import**, because the path is ~95% implemented, isolated, opt-in, and already falls back to stock on miss. Rejecting the alternatives:
- **(b) cooked artifact** — requires building an offline `mc2_assetcook` tool + cache-freshness logic. Good Phase-2 perf/distribution win, unnecessary for proving the system.
- **(c) manifest-only** — higher risk (runtime JSON + mod-tree integration with a *stub* mesh), and pointless given (a) already renders real geometry.

**Stop-condition check (glTF → compatible mesh):** satisfied — the importer already terminates at `TG_TypeMultiShape` and renders. No import-substrate slice needed first.

---

## 4. Render-only vs gameplay/collision policy

### Core finding: collision is derived from **visual mesh vertices**, cached at spawn, then frozen
- `TerrainObject::calcCellFootprint` (`terrobj.cpp:1369-1415`) → `appearance->calcCellsCovered(pos, cellList)`.
- `BldgAppearance::calcCellsCovered` (`bdactor.cpp:2763-2827`) loops every shape's vertices (`GetShapeVertexInEditor`), converts each to a world cell (`land->worldToCell`), writes the passability grid. `_PAB`-marked verts are skipped.
- Result cached in `TerrainObjectData::cellsCovered[81]` + `cellFootprint[4]` (`terrobj.h:124,115`), populated once (`terrobj.cpp:1442-1479`), **never re-queried** by movement/pathfinding/LOS.

### Render bounds vs gameplay bounds (clean split)
| Data | Source | Consumer | Evidence |
|---|---|---|---|
| `OBBRadius` | mesh `GetMinBox`/`GetMaxBox`, 8-corner max dist | frustum cull, HZB cull, shadow XY | `bdactor.cpp:693-738`, `bdactor.h:370-374` |
| `highZ` | mesh max Z in world | shadow vertical / `getTopZ()` | `bdactor.cpp:696-744` |
| `getRadius()` | `max(OBBRadius, GetExtentRadius())`, fallback `30.0f` | GPU cull record | `bdactor.h:370-374`, `objmgr.cpp:271,286-291` |
| `cellsCovered[81]` | mesh **vertices** → cells (cached, frozen) | pathfinding, passability, LOS | `bdactor.cpp:2763-2827`, `terrobj.cpp:1442-1479` |
| `collisionOffsetX/Y`, type `extentRadius` | `.FITS` type data | collision-grid bucketing | `terrobj.cpp:396-402`, `collsn.cpp:245` |

### Policy
**Render-only override = recompute render bounds from the replacement mesh; keep all gameplay data stock.**
- **Recompute** (automatic, at appearance init, from new mesh): `OBBRadius`, `highZ`, `GetExtentRadius()` → flow into `getRadius()` → GPU/HZB/shadow cull use new bounds automatically. This is the **required mitigation** for the clip/pop stop-condition: without recompute, a larger replacement mesh pops out of frustum/HZB cull at the stock radius.
- **Keep stock (do NOT touch):** `cellsCovered`, `cellFootprint`, type `collisionOffsetX/Y`, type `extentRadius`. Movement, pathfinding, and LOS read only the frozen `cellsCovered`, so they are unaffected as long as we never recompute it from the replacement mesh.

### Residual risk
If a replacement mesh is **smaller** than stock, the frozen `cellsCovered` still blocks stock-sized cells — units route around empty space (cosmetic only, gameplay-safe). If **larger**, render bounds grow (handled by recompute) but collision stays stock-sized — units may visually clip the new geometry's overhang. **MVP asset constraint:** document "replacement footprint ≈ stock footprint" and warn in validation when the replacement AABB deviates materially from stock. Re-deriving `cellsCovered` from the replacement is explicitly **out of MVP scope** (that would be a gameplay change).

---

## 5. Material / texture mapping

### MC2 static-prop material model (`RenderCore/MaterialGpu.h:88-107`)
4 texture slots — `albedoTex` (live, TEXTURE_2D_ARRAY layer), `normalTex` (slot exists, **not yet wired** / needs tangents), `metallicRoughnessTex` = ORM R=AO/G=rough/B=metal (live when `s_ormSlotsEnabled`), `emissiveTex` (designed, slice 6). Plus a flags word (`kAlphaTest, kNormalMap, kMetallicRoughness, kEmissive, kDoubleSided, kWindow`) and scalar factors (`baseColorFactor, metallicFactor, roughnessFactor`). Population at `GameOS/gameos/gos_static_prop_batcher.cpp:3390-3414`.

### glTF → MC2 slot map
| glTF | MC2 slot | Status |
|---|---|---|
| `baseColorTexture` (+`baseColorFactor`) | `albedoTex`, sRGB | **LIVE** |
| `metallicRoughnessTexture` (+factors) | `metallicRoughnessTex` G/B, linear | **LIVE (ORM)** |
| `occlusionTexture` (R) | `metallicRoughnessTex.r`, into ambient/IBL only | **LIVE** |
| `normalTexture` | `normalTex` + `kNormalMap` | **NOT wired** — requires tangents (location 5, not generated for static props) |
| `emissiveTexture`/`emissiveFactor` | `emissiveTex` + `kEmissive` | **Designed (slice 6)** |
| `alphaMode=opaque` | flags clear | **LIVE** |
| `alphaMode=mask`/alphaTest | `kAlphaTest`, **fixed 0.5** threshold | **LIVE** (`shaders/static_prop.frag:215-216`; glTF `alphaCutoff` ignored) |
| `alphaMode=blend` | — | **UNSUPPORTED → error/warn** (no blend path in static-prop renderer) |
| `doubleSided` | `kDoubleSided` | **Designed, not wired** → warn |
| clearcoat / transmission / sheen / IOR / specGloss | — | **UNSUPPORTED → warn, ignore** |

### Alpha / leaves
Foliage uses the single hard 0.5 alpha-test cutout on the albedo alpha channel (`static_prop.frag:215-216`); the shadow pass also alpha-discards. No alpha-blend, no smooth AA falloff, no per-leaf normal mapping. **Leaf rule for MVP:** author leaf cards with `alphaMode="mask"`, hard-edged alpha, cutoff baked to ~0.5. "foliage alpha-discard" remains tracked debt (`docs/active_campaigns.md:88`).

### Color space
Authority `tools/asset_cook/validate_asset_manifest.py:79-85`: albedo/emissive = **sRGB** (BC7 vkFormat 146 → `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM`), normal/orm = **linear** (vkFormat 145 → `GL_COMPRESSED_RGBA_BPTC_UNORM`). Binding at `mclib/txmmgr.cpp:3486-3488`; HW linearizes sRGB on sample.

### Texture sidecar reuse — yes, cleanly
- **Loose TGA override:** `File::open` strips the `/{size}/` subdir and retries (`mclib/file.cpp`, commit `5327c4b1`); a loose `data/tgl/<name>.tga` wins over `data/tgl/{size}/<name>.tga` and sets `uvScale=4`. Model-override textures reuse this verbatim.
- **BC7 KTX2 sidecar:** `<name>.ktx2` next to the TGA; loader auto-detects `vkFormat`/`isSrgb` (`txmmgr.cpp:3481-3512`). Reuse for albedo; needs per-slot linear-vs-sRGB plumbing for normal/ORM sidecars (currently `batch_cook.py --bc7` hardcodes sRGB).
- **ORM array feed gap:** batcher currently sources albedo-only per type (`gos_static_prop_batcher.cpp` ~2536); feeding a 2nd/3rd texture (normal/ORM) per overridden type is the one greenfield material wiring task — **defer past MVP** (MVP ships albedo-only + factors, which is already live).

### Unsupported-feature error/warn list
**Error:** unknown texture slot; color-space mismatch (e.g. normal tagged sRGB); `normalTexture` present but `hasTangents=false`; `alphaMode` not in `{opaque,mask,blend}`; `alphaCutoff` set with non-mask mode.
**Warn:** `alphaMode=blend` (renders opaque); clearcoat/transmission/sheen/specGloss (ignored); `doubleSided` (not wired); normal map present (slot not wired yet).

---

## 6. Mod folder + manifest shape

### Folder
```
mods/better_trees/
  mc2mod.json            # mod identity: name, version, author, mc2 min version
  overrides/
    models.json          # the override declarations (below)
    aliases.json          # OPTIONAL stock-asset alias index (see §1 stop-condition)
  source/trees/oak_01.glb # authored glTF/GLB
  textures/               # albedo/normal/orm TGA or .ktx2 sidecars
  cooked/                 # OPTIONAL Phase-2 .tgl/.tglc cook output (empty in MVP)
  reports/                # validation reports from asset viewer
```

### `overrides/models.json`
```json
{
  "schemaVersion": 1,
  "overrides": [
    {
      "type": "model",
      "class": "tree",
      "replaces": "tree:oak_01",
      "source": "source/trees/oak_01.glb",
      "scale": 1.0,
      "pivot": "base",
      "renderOnly": true,
      "fallback": "stock",
      "textures": {
        "albedo": "textures/oak_01_albedo.ktx2"
      }
    }
  ]
}
```
Notes vs the prompt's draft shape: added `schemaVersion`, `renderOnly: true` (explicit MVP guarantee — collision/gameplay frozen), and a `textures` slot map. `replaces` is the typed `<class>:<appearanceName>` key from §1. `pivot` values `base|center|origin` map the glTF origin onto the MC2 root-node convention; `scale` is a uniform multiplier applied post-import before bounds recompute. `fallback: "stock"` is the only MVP value and is also the implicit behavior on any import failure.

---

## 7. Asset-viewer integration (deferred, scoped here)

**Current state:** `mc2_asset_viewer` (`tools/asset_viewer/`) is **texture-only** — SDL2/GLEW/ImGui standalone, loads PNG/JPG/BMP/TGA/KTX2, shows dims/channels/format/mips. No mesh, no glTF, no manifest parsing. The `PreviewSurface` seam (`PreviewSurface.h:4-6`) is pre-designed for a future `ModelPreviewRenderCore` but unimplemented in this worktree.

**What override validation must add (follow-on, not MVP):**
- Load + preview the replacement glTF (reuse `assimp_probe` / `ImportGeometryFromFile`) and the resolved stock asset side-by-side.
- Report: triangle/vertex count, AABB bounds, pivot, scale, material slots, alpha-mask presence, texture readiness (slot ↔ colorspace ↔ format), tangent presence.
- Flag: bounds deviation from stock (clip/footprint risk per §4), unsupported material features (per §5), missing fallback.
- Show resolved cooked-output path (Phase 2) and the stock fallback path.
- Emit a `reports/<asset>.json` validation report.

These map onto the existing manifest validator (`tools/asset_cook/validate_asset_manifest.py`) — the viewer becomes its interactive front end.

---

## 8. MVP recommendation — `TREE-MODEL-OVERRIDE-MVP-1`

**Scope:** one tree or static prop, **render mesh only**, glTF/GLB source via the existing Assimp import path, stock collision/gameplay preserved, stock fallback on any failure, mod-folder manifest. Asset-viewer validation deferred.

### Slice breakdown
1. **Override registry module** — parse `mods/*/overrides/models.json` at mod-mount, build `(class, appearanceName) → override` map. Hand-author `aliases.json` for the one target asset.
2. **Loader hook** — insert resolution in `TG_TypeMultiShape::LoadFromFile` (`msl.cpp:438-479`): on match, `ImportGeometryFromFile(override.source)`; on miss/failure, stock path. (§2)
3. **Render-bounds recompute** — after import, recompute `OBBRadius`/`highZ`/extent so cull/HZB/shadow track the new mesh (§4). Verify gameplay `cellsCovered` is **not** recomputed.
4. **Material MVP** — albedo + factors only (live path); leaf cards use `alphaMode=mask` @ 0.5; textures via existing loose-TGA / KTX2 sidecar resolution (§5).
5. **Smoke gate** — stock install with no mods loads byte-identical (no `[Import]`/no manifest → unchanged `.ase` path); with the mod mounted, the target renders the replacement and gameplay/pathfinding is unchanged (cell footprint identical).

### Out of scope (constraints honored)
No `tgl.fst` mutation, no stock-asset mutation, no collision replacement, no mech/vehicle override, no mission/editor changes, no `git add -A`, no broad replacement, no asset-format commitment beyond this manifest shape, no cook tool (Phase 2), no normal/emissive material wiring (Phase 2).

---

## 9. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Replacement larger than stock → frustum/HZB pop | Med | **Required** render-bounds recompute (§4); without it, MVP visibly fails |
| Replacement footprint ≠ stock → visual clip vs frozen collision | Med | Asset constraint "footprint ≈ stock"; validation warns on AABB deviation; never recompute `cellsCovered` in MVP |
| Identity ambiguity (appearance name ↔ multiple shapes/damage states) | Med | Typed `<class>:<name>` key + explicit alias registry (§1) |
| glTF import failure mid-mission | Low | Stock fallback already built into `LoadFromFile` probe chain |
| Material features beyond albedo (normal/emissive/blend) | Low | Warn/error in validation; MVP ships albedo+factors only |
| Color-space mis-tag (normal as sRGB) | Low | Validator enforces slot↔colorspace (`validate_asset_manifest.py:79-85`) |
| ORM/normal per-type texture feed is greenfield | Low | Deferred to Phase 2; MVP albedo path is live |

## 10. Stop conditions (status)
- **Identity ambiguous** → handled: typed key + alias registry mandated (§1).
- **glTF can't produce compatible mesh** → not triggered: importer already yields `TG_TypeMultiShape` and renders (§3).
- **Bounds/culling clip replacement** → handled: render-bounds recompute is a required MVP task (§4).
- **Any path touches mission/editor** → none required; explicitly out of scope (§8).

---

## 11. Recommendation
Proceed to plan **`TREE-MODEL-OVERRIDE-MVP-1`** as scoped in §8. The system rides on an already-working glTF→`TG_TypeMultiShape` importer; the net new code is a small override-registry module + a single hook in `LoadFromFile` + a bounds-recompute call + the stock-frozen-collision guarantee. First target: pick one static prop (simpler than a tree — opaque, no alpha) to prove the render path and the gameplay-freeze invariant, then a tree to exercise the `alphaMode=mask` leaf path.
