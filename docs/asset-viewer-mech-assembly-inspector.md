<!-- doc-status: current | reviewed:2026-10-11 -->
# Asset Viewer — Mech Assembly Inspector (Recon + Design)

**Slice:** `ASSET-VIEWER-MECH-ASSEMBLY-INSPECTOR-1`
**Status:** Recon / design only. No implementation in this slice.
**Tool:** `mc2_asset_viewer` (`tools/asset_viewer/`)
**Date:** 2026-06-02

> **Terminology (load-bearing).** This is **not** a bone/skin viewer. MC2 mechs
> are **rigid hierarchical part assemblies**, not skeletal skinned meshes. Use:
> **Part Hierarchy**, **Pivots/Joints**, **Hardpoints**, **Rigid Part
> Transforms**. Never "bone", "skeleton", "skin weights", "rig".

Goal: design the first mech-assembly inspection feature. Left = final mech
preview, right = assembly graph/tree of every component/file that contributes
to the mech, bidirectional selection-highlight between the two.

This document is grounded in source recon (cited `file:line`). It deliberately
reflects the **real** engine assembly model rather than a generic
skeletal-animation assumption — see [Actual mech assembly model](#actual-mech-assembly-model).

---

## Actual mech assembly model

The MC2 engine does **not** use skeletal skinning. A mech is a **tree of rigid
parts**, each part a self-contained mesh with its own node matrix. Animation is
per-node rigid rotation/translation that cascades down the hierarchy — there are
no bone weights, skin matrices, or vertex blending anywhere in the path.

Evidence (definitive, confirmed by two independent recon passes):
- `TG_TypeVertex` carries position + normal + ARGB light only — **no bone
  indices, no weights** (`mclib/tgl.h:35-42`).
- Each part has a single rigid `Stuff::LinearMatrix4D shapeToWorld`
  (`mclib/tgl.h:392`); all of a part's vertices transform by that one 4×4.
- Hierarchy is an explicit parent-pointer tree, evaluated per frame by
  `TG_MultiShape::TransformMultiShape()` (`mclib/msl.cpp:1547-1968`):
  `child.shapeToWorld = child.localShapeToWorld ∘ parent.shapeToWorld`.
- Destruction is **part detach**, not skin deformation —
  `TG_MultiShape::Detach("joint_rarm")` removes a part + its children
  (`mclib/msl.h:632`).

### Type vs instance split

The engine separates an immutable shared **type** (template) from a per-actor
**instance**. This split is the backbone of the inspector's data model.

| Layer | Type (shared template) | Instance (per actor) |
|---|---|---|
| Whole mech | `Mech3DAppearanceType` (`mclib/mech3d.h:110-247`) | `Mech3DAppearance` (`mclib/mech3d.h:304-486`) |
| Multi-part container | `TG_TypeMultiShape` (`mclib/msl.h:56-269`) | `TG_MultiShape` (`mclib/msl.h:276-705`) |
| One part | `TG_TypeShape` (`mclib/tgl.h:568-731`) | `TG_Shape` (`mclib/tgl.h:738-1015`) |
| Part record (hierarchy node) | — | `TG_ShapeRec` (`mclib/tgl.h:388-412`) |
| Geometry (immutable) | `listOfTypeVertices`, `listOfTypeTriangles`, shared `vb_`/`ib_` | transformed `listOfVertices`, `listOfVisibleFaces` (per frame) |

Key relationships:
- `TG_TypeMultiShape::listOfTypeShapes[]` — array of part templates
  (`mclib/msl.h:76`); count via `GetNumShapes()`.
- `TG_MultiShape::listOfShapes[]` — array of `TG_ShapeRec`, one per node
  (`mclib/msl.h:298`); each record has `node` (the `TG_Shape`),
  `parentNode` pointer (NULL = root), `localShapeToWorld`, `shapeToWorld`.
- Instance → type accessor: `TG_Shape::myType` (`mclib/tgl.h:761`).
- Node name string: `TG_TypeShape::nodeId[]` (`mclib/tgl.h:426`, ≤`TG_NODE_ID`
  chars); parent name `parentId[]` (`mclib/tgl.h:427`).

### Pivots / hardpoints

- Pivot/center per part: `nodeCenter` (absolute) + `relativeNodeCenter`
  (relative to parent) (`mclib/tgl.h:424-425`).
- Weapon hardpoints are **named nodes**, not a separate system. Mech-specific
  metadata lives in `Mech3DAppearanceType::nodeData[]` (`mclib/mech3d.h:131-137`):
  each `NodeData` has `nodeId` (name e.g. `joint_rarm_w0`), `weaponType` enum
  (BALLISTIC/MISSILE/ENERGY/DIRECT, `mclib/objectappearance.h:26-32`), and
  arm-side cull flags.
- Lookup by name: `TG_MultiShape::GetNodeNameId(const char* nodeId)` —
  linear O(n) scan returning the `listOfShapes[]` index (`mclib/msl.h:649-662`).
- Cached role indices on the instance: `rootNodeIndex`, `rotationalNodeIndex`
  (torso twist), `leftArmNodeIndex`, `rightArmNodeIndex`, etc.
  (`mclib/mech3d.h:462-469`).
- World position of a hardpoint:
  `Mech3DAppearance::getWeaponNodePosition(nodeId)` (`mclib/mech3d.cpp:797-837`).

### Materials, textures, paint

- **No material struct** in the legacy path. A part's surface is per-face
  `TG_TypeTriangle::localTextureHandle` + `renderStateFlags`
  (`mclib/tgl.h:121-129`) into the shape's texture list (`TG_Texture`,
  `mclib/tgl.h:345-353`). A mech typically uses **one base texture** shared
  across all parts.
- Texture path resolution: name pulled from the shape
  (`GetTextureName(0,...)`), assembled as `data/tgl/<tier>/<name>.txm`
  with tier ∈ {128,256,512,1024} (`mclib/mech3d.cpp:1918-1924`), loaded via
  `mcTextureManager->loadTexture(...)` (`mclib/mech3d_init.cpp:534-536`).
- **Team color / paint scheme**: predefined schemes in
  `data/tgl/paintSchemata.fit` (`PaintSchemata`, `mclib/mech3d.h:53-58`).
  Applied by CPU pixel recolor of a masked texture (R/G/B dominance classifier),
  keyed by a 27-bit paint-instance hash so painted variants are cached
  (`mclib/mech3d.cpp:1712-1995`).
- **PBR (this fork):** `RenderCore::MaterialGpu` 32-byte GPU record exists
  (`RenderCore/MaterialGpu.h:88-107`) and is live for static props, but **mech
  shader sampling of normal/MR/emissive is still pending** — mechs currently set
  those tex slots to `kMaterialTexAbsent`. The inspector should *surface* PBR
  fields where they exist but must not assume mech parts have full PBR maps yet.

### Cooked vs source artifacts

| Stage | Geometry | Texture | Paint | Gestures |
|---|---|---|---|---|
| **Source** (authoring) | `<mech>.ase` / `.glb` (`mc2srcdata/tgl/`) | `<mech>.txm` | `paintSchemata.fit` | `<mech>.ini` |
| **Cooked** (engine-loadable) | `.tgl` binary, packed in `tgl.fst` FastFile | `.ktx2` BC7 in `data/tgl/<tier>/` | `.fit` (as-is) | embedded in `.tgl` |

Load entry: `Mech3DAppearanceType::init(fileName)` reads `<mech>.ini`, then
`LoadFromFile` probes `.glb` first, falls back to `.ase`
(`mclib/mech3d.cpp:245-387`, `mclib/msl.h:268`). See `docs/asset-pipeline.md`
for the canonical asset inventory (this doc adds the mech assembly view).

### Runtime → GL draw

- Legacy CPU path: `TG_Shape::Render()` walks `listOfVisibleFaces` and issues
  `gos_DrawTriangles` per primitive (`mclib/tgl.cpp:2933-3080`).
- GPU path (Slice 2 / Stage 2.C): `GpuMechBatcher::submitActor(GpuMechSubmitDesc&)`
  feeds `mechShape->listOfShapes[i].shapeToWorld` into instanced draws
  (`GameOS/gameos/gos_mech_batcher.h:35-199`).

**Answering the brief's question 4 directly:** there is **no real
skeleton/skinning** — only hierarchical rigid part transforms. The inspector's
graph is therefore a *part tree*, not a *bone tree*. Do not build skin-weight UI.

---

## Node taxonomy

The graph exposes these node kinds. Each maps to a concrete engine object.

| Node kind | Backed by | One per | Notes |
|---|---|---|---|
| **MechRoot** | `Mech3DAppearanceType` + `Mech3DAppearance` | mech | Graph root. Holds type name, LOD count, source/cooked file refs. |
| **Part** | `TG_ShapeRec` / `TG_TypeShape` | rigid node | The hierarchy backbone. Has parent Part (or MechRoot). Carries node name, pivot, tri/vert counts. |
| **Hardpoint** | `NodeData` (weapon node) | weapon mount | A Part that is also a weapon node. Shown as a badge/child marker on its Part, not a separate subtree. |
| **Material** | per-shape texture+state (no struct) | distinct surface | Usually 1 shared base material for the whole mech. PBR fields shown when present. |
| **Texture** | `TG_Texture` / `MC_TextureNode` | distinct texture file | Base color (+ future normal/MR/emissive). Links to on-disk file node. |
| **PaintScheme** | `PaintSchemata` + instance RGB | mech instance | The applied team color; not geometry. |
| **SourceFile** | on-disk authoring file | file | `.ase`/`.glb`, `.txm`, `.ini`, `paintSchemata.fit`. |
| **CookedArtifact** | on-disk engine file | file | `.tgl` (+ `tgl.fst`), `.ktx2`. Edge to the SourceFile it derives from. |

Edge kinds:
- **child-of** (Part → Part/MechRoot) — the rigid hierarchy.
- **uses-material** (Part → Material).
- **samples-texture** (Material → Texture).
- **cooked-from** (CookedArtifact → SourceFile).
- **loaded-from** (Part/Texture → CookedArtifact).
- **mounts** (Hardpoint badge → its Part).

---

## Graph data model

A read-only snapshot built once at mech load. Decoupled from engine structs so
the UI never holds raw engine pointers across frames (transforms recompute every
frame; the snapshot caches the *identity*, not the live matrix).

```
struct InspectorNodeId { NodeKind kind; uint32_t index; };   // stable key

struct InspectorNode {
    InspectorNodeId   id;
    NodeKind          kind;
    std::string       label;            // node name / file basename / "Material 0"
    InspectorNodeId   parent;           // for Part Hierarchy; invalid for roots
    std::vector<InspectorNodeId> children;
    std::vector<InspectorNodeId> refs;  // cross-edges (uses-material, cooked-from, ...)
    // payload union by kind — see "Inspector fields by node type"
};

struct MechInspectorModel {
    std::string mechTypeName;
    std::vector<InspectorNode> nodes;           // flat store, indexed by id
    // fast lookups:
    std::vector<uint32_t> partIndexByShapeIdx;  // listOfShapes[i] -> node store idx
    std::unordered_map<int, uint32_t> nodeByShapeIndex;   // shape idx -> store idx (reverse)
};
```

Build source (all read-only engine accessors):
- Parts: iterate `TG_MultiShape::listOfShapes[0..numTG_Shapes)`; `label` =
  `node->myType->getNodeId()`; `parent` resolved from `TG_ShapeRec::parentNode`
  by pointer→index.
- Hardpoints: iterate `Mech3DAppearanceType::nodeData[]`, match `nodeId` to a
  Part via `GetNodeNameId`.
- Material/Texture: pull texture name(s) from each shape's texture list; dedupe.
- Files: derive paths from the load logic (`<mech>.ase|glb`, `<tier>/<mech>.txm`,
  `.ktx2`, `<mech>.ini`, `paintSchemata.fit`).

The **shape-index ↔ node-id** maps are the crux of bidirectional selection
(below). Shape index is the engine's own array index into `listOfShapes[]`, so it
is the natural stable key for both highlight directions.

---

## UI layout

Reuses the existing 3-region ImGui shell (`AssetViewerApp.cpp:31-71`) and the
`PreviewSurface` plug-in seam (the texture/material previews already plug in this
way). Add a new `MechPreview3D` surface + a new `MechInspector` mode in the
sidebar.

```
┌──────────┬───────────────────────────────┬───────────────────────────┐
│ Sidebar  │  Mech Preview (left, large)    │  Assembly Tree (right)    │
│ (mode)   │  ─ orbit camera                │  ─ Part hierarchy tree    │
│          │  ─ rigid mech, lit             │    (ImGui::TreeNode)      │
│ Textures │  ─ selected part highlighted   │  ─ Materials / Textures   │
│ Materials│  ─ click part -> select node   │  ─ Source / Cooked files  │
│ > Mech   │                                │                           │
│          │                                ├───────────────────────────┤
│          │                                │  Inspector (fields of     │
│          │                                │  the selected node)       │
└──────────┴───────────────────────────────┴───────────────────────────┘
```

- **Sidebar**: add a "Mech" radio mode (`AssetTypeSidebar`).
- **Left preview**: new `MechPreview3D` surface. Renders the mech with an orbit
  camera (reuse the existing orbit camera math from `MaterialPreviewPBR`).
- **Right top**: assembly tree (`ImGui::TreeNode`) — Part hierarchy first, then
  collapsible Materials / Textures / Source files / Cooked artifacts groups.
- **Right bottom**: inspector panel showing fields for the selected node.

No new generic graph-editor framework, no auto-layout — a plain tree (the
hierarchy is literally a tree). Cross-edges (material/file refs) are shown as
collapsible reference lists in the inspector, not drawn as graph edges.

---

## Interaction model

- **Tree click** → set selection to that node id.
- **Preview click** → pick a part (see [selection/highlight](#selectionhighlight-behavior)),
  map to node id, set selection, scroll tree to it.
- **Selection is single** across all stages (one node at a time).
- **Hover** (stretch): transient highlight without committing selection.
- **Expand/collapse**: standard tree behavior; the Part subtree auto-expands to
  reveal the selected node when selection comes from the preview.
- The preview camera (orbit/zoom) is independent of selection.

---

## Selection / highlight behavior

The shape array index is the shared key in both directions.

**Tree → preview (graph → preview):**
1. Selected node is a Part with `shapeIndex`.
2. Highlight the matching `TG_Shape` in the preview. Engine already has a
   per-instance highlight: `TG_Shape::SetARGBHighLight(color)` (`mclib/msl.h:609-613`)
   — set on the selected part, clear on others. (Alternative for the viewer:
   render the selected part with an outline/tint pass.)
3. Non-Part selections (Material/Texture/File) highlight **all** parts that
   reference them (e.g. selecting the base texture highlights the whole mech).

**Preview → tree (preview → graph):**
- The viewer has **no picking today** (no ray-cast, no ID buffer — confirmed).
  Stage 2 picks parts via an **object-ID / part-ID buffer**: render each part to an
  offscreen R32UI target writing its `shapeIndex+1`, read back the pixel under
  the cursor on click, map id→node. The engine already proves this pattern
  (`GpuMechInstance::objectIdRaw`, `MC2_OBJECT_ID_BUFFER`,
  `GameOS/gameos/gos_mech_batcher.h:42-46`) — the viewer reimplements a minimal
  version standalone (the viewer does not link RenderCore today).
- Fallback if the R32UI ID-buffer proves troublesome in Stage 2: CPU ray vs per-part AABB
  (`nodeCenter` + type bounds), nearest hit wins. Lower fidelity but no extra
  render target.

---

## Inspector fields by node type

| Node kind | Fields shown |
|---|---|
| **MechRoot** | type name; LOD count; part count; total tri/vert; applied paint RGB; list of source + cooked file refs. |
| **Part** | node name; parent node name; child count; pivot (`nodeCenter`, `relativeNodeCenter`); template tri/vert counts; current visibility (`processMe`); is-arm/role flags; material ref; live `shapeToWorld` (read each frame, display-only). |
| **Hardpoint** | host node name; `weaponType` (BALLISTIC/MISSILE/ENERGY/DIRECT); world position (`getWeaponNodePosition`). |
| **Material** | index; base texture ref; render-state flags (alpha/double-sided); PBR scalar factors *if present* (baseColor/metallic/roughness); note "normal/MR/emissive pending for mechs" when absent. |
| **Texture** | name; resolved path + tier; dimensions/channels/format (reuse `TextureMetadata`); alpha flag; cooked `.ktx2` present? |
| **PaintScheme** | scheme name/id; instance RGB triplet; cached paint-instance hash. |
| **SourceFile** | path; exists?; size; kind (`.ase`/`.glb`/`.txm`/`.ini`/`.fit`); → derived cooked artifact. |
| **CookedArtifact** | path (incl. `tgl.fst` membership for `.tgl`); exists?; size; format (BC7/sRGB for `.ktx2`); → source it was cooked from. |

Texture metadata reuses the viewer's existing `TextureMetadata` /
`TextureDecoderRegistry` so the Texture node's preview/format display is free.

---

## Staged scope

The original single MVP is split into three independently shippable stages so
the graph/dependency value lands before any 3D render integration. Each stage
has its own slice id and validation gate.

### Task 0 spike (gate before Stage 1) — load-path bring-up

Before Stage 1, prove the viewer can **load one mech type and enumerate
`TG_MultiShape::listOfShapes[]` without starting Mission / GameObjectManager**.
The viewer is standalone today and does not link RenderCore or the engine mech
path; this spike de-risks the single largest unknown.

- Pass: viewer links the minimal TG_* load path (`mech3d`/`msl`/`tgl` + texture
  manager) — or a thin cooked-`.tgl` reader — and walks `listOfShapes[]`
  outside any Mission/GOM bootstrap.
- **Fail → fallback:** keep **Stage 0 as file/dependency-only** — build the
  `MechInspectorModel` from on-disk artifacts (parse `.ase`/`.glb` part list,
  `.ini`, texture/file paths) without instantiating engine runtime objects.
  Stages 1–2 then block on solving runtime bring-up separately.

### Stage 0 — `ASSET-VIEWER-MECH-ASSEMBLY-GRAPH-0`

Goal: show the mech assembly/dependency graph **without 3D render integration**.

**In:**
- Load/inspect one known mech type.
- Build the read-only `MechInspectorModel`.
- Part **hierarchy tree** (rigid part assembly).
- **Hardpoint** badges/nodes on their host parts.
- Flat Material / Texture / SourceFile / CookedArtifact groups.
- Inspector panel for the selected node (per-kind fields above).

**Out:** no 3D preview, no picking, no highlight (no preview to highlight into).

**Validation:**
- Known mech loads.
- Part count matches engine data (`numTG_Shapes` / `listOfShapes[]` length,
  or on-disk part count in fallback mode).
- Hardpoints resolve to parts (every `nodeData[].nodeId` maps via
  `GetNodeNameId`).
- Texture/source paths resolve on disk.
- No crash on missing **optional** files (e.g. absent `.ktx2` or `.glb`).

### Stage 1 — `ASSET-VIEWER-MECH-PREVIEW-STATIC-1`

Goal: render the assembled **rigid** mech in the viewer.

**In:**
- Load one mech.
- Orbit camera (reuse `MaterialPreviewPBR` orbit math).
- Lit, static pose (rigid part transforms evaluated once; no animation).
- **Tree → preview** highlight: selecting a part in the tree highlights that
  part (`SetARGBHighLight`, or outline/tint pass).

**Out:** no preview→tree picking yet.

**Validation:**
- One mech renders.
- Selecting a part in the tree highlights that part in the preview.
- No GL errors.

### Stage 2 — `ASSET-VIEWER-MECH-PART-PICKING-1`

Goal: add **preview → tree** selection.

**In:**
- R32UI part-ID buffer (writes `shapeIndex+1` per part).
- Click a part in the preview.
- Map shape index → graph node id.
- Select + scroll the tree to that node.

**Out:** still no editing, animation, or damage sim.

**Validation:**
- Clicking major parts selects the correct node.
- ID-buffer readback happens **only on click**, not every frame.

### Explicit non-goals (all stages)

- Editing anything (geometry, materials, paint).
- Live reimport / hot reload.
- Full animation system / gesture playback (preview is a static rigid pose).
- Generic graph-editor framework or force-directed auto-layout.
- Multi-select, search/filter, drag-rearrange.
- Damage/detach simulation (engine *can* `Detach` parts — follow-on).
- Mech PBR map authoring (normal/MR/emissive sampling is pending engine-side).

---

## Follow-on slices

1. **Animation pose** — drive node rotations from a selected gesture (`.ini`
   blocks) so the preview can pose/animate; scrub timeline.
2. **Damage/detach preview** — toggle parts via `Detach`/`StopUsing` to preview
   destruction states; show arm-off cull behavior.
3. **Full mech PBR** — once engine mech shader samples normal/MR/emissive, light
   the preview with the real material and expand Material/Texture nodes to the
   full slot set.
4. **Multi-LOD inspection** — switch `currentLOD`, compare part/tri counts across
   LODs.
5. **Paint scheme picker** — live re-tint via `resetPaintScheme` to preview team
   colors (read-only of the engine pipeline).
6. **Cross-asset linking** — jump from a Texture node into the existing texture
   inspector; from a SourceFile into a file viewer.
7. **Shared tool shell** — extract `tools/common/MC2AppShell` (UI bring-up) so
   the mech path and texture/material paths share window/GL/ImGui init
   (stage-1.5 debt already noted).

---

## Answers to the brief's 8 questions (index)

1. **How assembled** — type/instance split, `Mech3DAppearanceType` →
   `TG_TypeMultiShape` → `listOfTypeShapes[]`; rigid Part Hierarchy; named Hardpoints;
   one shared base texture + paint recolor; cooked `.tgl`/`.ktx2`. See
   [Actual mech assembly model](#actual-mech-assembly-model).
2. **Source files** — `.ase`/`.glb`, `.txm`, `.ini`, `paintSchemata.fit`.
3. **Runtime objects** — `Mech3DAppearance` / `TG_MultiShape` /
   `TG_ShapeRec` / `TG_Shape`; per-frame `shapeToWorld`.
4. **Skeleton?** — **No.** Rigid hierarchical part transforms only.
5. **Minimum graph model** — read-only snapshot: MechRoot + Part hierarchy +
   Material + Texture + Source/Cooked file nodes, keyed by shape index. This is
   exactly **Stage 0** (`ASSET-VIEWER-MECH-ASSEMBLY-GRAPH-0`). See
   [Graph data model](#graph-data-model).
6. **Per-node metadata** — see [Inspector fields by node type](#inspector-fields-by-node-type).
7. **Selection plumbing** — shared `shapeIndex` key; tree→preview via
   `SetARGBHighLight` (**Stage 1**); preview→tree via a minimal R32UI part-ID
   buffer, engine pattern `MC2_OBJECT_ID_BUFFER` (**Stage 2**). See
   [Selection / highlight behavior](#selectionhighlight-behavior).
8. **Scope omits** — editing, reimport, animation, graph framework, auto-layout,
   damage sim, mech PBR authoring. See [Staged scope](#staged-scope).
```