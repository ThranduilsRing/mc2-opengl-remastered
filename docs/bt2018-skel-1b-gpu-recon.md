<!-- doc-status: stale | reviewed:2026-10-11 -->
# BT2018-SKEL-1B-GPU — recon + plan (NOT yet implemented)

Goal: make the imported BattleTech mech animate on the **default GPU mech path**
(`g_useGpuMechs` on), so users don't need `MC2_GPU_MECHS=0`. Today the per-frame
CPU re-bake (1B/1C) only shows on the CPU path.

## Why the CPU re-bake doesn't reach the GPU (recon, mc2-render-expert)
The GPU mech path is **rigid per-node skinning**:
- `GpuMechBatcher::registerTypeLod` packs `listOfTypeVertices` into a CPU staging
  blob once; each vertex gets `boneIndices[0]=nodeIdx`, `boneWeights[0]=255`
  (`gos_mech_batcher.cpp:1219,1224`).
- `uploadMechGeometryVbo()` uploads it via **`glBufferStorage(... 0)` = immutable**
  (`GameOS/gameos/gos_mech_batcher_geometry.cpp:159-161`, "CANNOT be re-uploaded in place" comment
  1290-1291), once at `finalizeGeometry()` (1356, guard `s_geometryFinalized`).
- Animation happens entirely through per-node `shapeToWorld` matrices gathered in
  `submitActor` (`gos_mech_batcher.cpp:1516-1524`) → `s_boneSsbo` (binding 1) each
  frame. The GPU draw consumes the immutable rest VBO + that SSBO; it never reads
  CPU `listOfVertices`. So `TransformMultiShape` (full/`_PositionsOnly`/`_HierarchyOnly`)
  and my CPU vertex re-bake are dead weight on the GPU path.

## The blocker specific to the imported mech
The importer merges all parts into **ONE TG_TypeShape = one node** (deliberate:
per-mesh empties crash the GPU recipe path). The GPU rigid-per-node path animates
by moving *nodes* — but one node can't articulate. So "reuse the per-node path for
free" does NOT apply to the merged mech.

## Options (ranked)
**B (recommended, real fix) — true GPU skinning via the existing C2 lanes.**
`GpuMechVertex` already carries `boneIndices[4]`/`boneWeights[4]` (attribs 3/4,
`gos_mech_batcher.cpp:1350-1351`) and a skinning-mode uniform (`u_skinningMode`,
1762-1763, `MC2_GPU_MECH_SKIN`). The imported mech is rigid-per-part (each vertex
one bone), so single-weight suffices. Plan:
1. At registration, pack each imported vertex's `boneIndices[0]` = its part's bone
   index into MY skeleton (not the node index). No `TG_TypeVertex`/`GpuMechVertex`
   ABI change — reuse existing lanes.
2. Each frame, upload MY `EvaluateClipGpuBones` joint-globals (already computed for
   the CPU path) into the bone SSBO slot the shader reads for this mech, INSTEAD of
   the batcher's node-derived `shapeToWorld`. This is the surgical part: the batcher
   currently fills `ps.bones` from node `shapeToWorld` (1516-1524); the imported
   mech needs its own matrix source.
3. Set `MC2_GPU_MECH_SKIN=1` for the imported type.
Risk: re-purposing the shared batcher's per-actor bone gather for a non-standard
mech; must not perturb stock mechs. Needs the render expert + careful parity. No
relink hazard beyond the `.cpp` if no struct field is added.

**C (fallback) — per-instance CPU draw even when `g_useGpuMechs`.**
Skip `submitActor` for the imported instance; draw it via the CPU MLR path (the
current working path). Branches live in `Mech3DAppearance::render` — which currently
carries FOREIGN WIP — and the expert flagged draw-order/state risk mixing one CPU
mech with GPU-batched mechs. Lower conceptual cost, but touches a hot, contended TU.

**A (rejected) — per-frame VB re-upload.** `glBufferStorage` is immutable; re-upload
= delete+recreate the WHOLE shared mech VBO every frame (all types). Infeasible.

## Recommendation
Do **B** as a dedicated slice with `mc2-render-expert` in the loop (parity-gated,
default-OFF `MC2_MECH_IMPORT_GPU`), after the foreign `mech3d.cpp` WIP settles.
Until then the CPU path (`MC2_GPU_MECHS=0`) is the supported way to see the imported
mech animate; the engine now warns once if an imported animated mech is detected on
the GPU path (so the frozen-pose symptom is self-explaining).

## Verify (when built)
Parity oracle unchanged (`scripts/mech_bone_parity.py --clip/--frame`). Visual:
default build (`g_useGpuMechs` on) + `MC2_MECH_IMPORT_GPU=1` → mech animates like the
CPU path; tier1 PASS; stock mechs unchanged (byte-identical with gate off).
