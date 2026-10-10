<!-- doc-status: stale | reviewed:2026-10-11 -->
# BT2018-SKEL-ENGINE-1B-GPU-SKINNING-RECON-2 — compatibility PROVEN

**Central question:** can the imported merged mesh use the existing skinned-GPU path,
or does that path only support stock rigid/node transforms?

**Answer: COMPATIBLE.** The imported merged mesh can be skinned as ONE draw on the
existing GPU mech path with **no vertex-struct ABI change and no shader edit**. Two
independent recons (render + shader experts) agree. The only obstacle is a per-type
bone-count that defaults to the node count (1 for a merged mesh); it is set by recipe
logic, not a structural wall.

## Evidence (verbatim citations, nifty-mendeleev)

### The lanes already exist
- `GpuMechVertex` (`gos_mech_batcher.h:15-31`, 48B, static_assert-locked): `boneIndices[4]`
  uint8 @loc3 via `glVertexAttribIPointer` (integer, **not** normalized,
  `GameOS/gameos/gos_mech_batcher_geometry.cpp:24`) → `uvec4`; `boneWeights[4]` uint8 @loc4 normalized
  (`:1193`) → `vec4` in [0,1]. Stock packs `boneIndices[0]=nodeIdx`, `boneWeights[0]=255`
  (`:1218-1227`). Writing an arbitrary 0–27 bone index here is a value change, **no
  struct change**.
- Bone SSBO (binding 1) is an **unbounded flat array** `GpuMechBone bones[]`
  (`mech.vert:65-68`, `shadow_mech.vert:21`). No `MAX_BONES`. Per-actor slice is
  **contiguous, variable-length**, written at `GameOS/gameos/gos_mech_batcher_geometry.cpp:541-548`; the
  shader indexes `bones[a_boneIndices[i] + inst.baseBoneOffset]`
  (`mech.vert:144/150`, `shadow_mech.vert:36/41`). 28 bones ≪ the only cap (uint8 ⇒
  numBones ≤ 255, `gos_mech_batcher.cpp:1109-1116`).
- Weighted skinning is **fully implemented, default ON**: `u_skinningMode=1`
  (`g_useGpuMechSkin = envFlagDefaultOn("MC2_GPU_MECH_SKIN")`, `:91`,`:1762-1763`);
  `mech.vert:139-152` does `Σ w[i]·bones[idx[i]]`; normals skinned by `mat3(boneT)`
  (`:161`). `shadow_mech.vert:31-43` mirrors it. Built explicitly for the Track D
  Assimp pipeline (shader comment `mech.vert:86-91`). For rigid-per-part, mode 0
  (`bones[boneIndices.x + base]`) also suffices.
- Per-actor base = `inst.baseBoneOffset` (per-instance SSBO, `gos_mech_batcher.h:37`,
  `mech.vert:128-129`).

### The one deciding constraint
`rec.numBones = GetNumShapes()` (`gos_mech_batcher.cpp:1133` ← `:1108`), and
`submitActor` gathers one matrix per node from `listOfShapes[i].shapeToWorld`
(`:1515-1525`). A **merged single-node** mesh → `GetNumShapes()==1` → `numBones==1`
→ one matrix. That single coupling (recipe field + gather loop) is what limits the
merged mesh — not the buffer or shader.

### Matrix packing contract (must match exactly)
`submitActor` packs `GpuMechBone` from Stuff `LinearMatrix4D.entries`
(`GameOS/gameos/gos_mech_batcher_geometry.cpp:350-355`): `row0=(e0,e4,e8,0)`, `row1=(e1,e5,e9,0)`,
`row2=(e2,e6,e10,0)`, `row3=(e3,e7,e11,1)`. Shader `mat4(row0..row3)` is the
**transpose** of the Stuff matrix (row-vector math, `mech.vert:6-7,131-133`). My
`EvaluateClipGpuBones` output (row-major float[16]) must be converted to this exact
packing — verify one vertex via the parity oracle.

### Axis / space contract
Vertex positions stay in **Stuff space**; the shader applies the `(-x,z,y)` Stuff→GL
swap itself (`mech.vert:160`). Do NOT pre-swap on CPU (double-swap = mech in the sky).
`slot0TexHandle` is a slot index, `aRGBLight` is BGRA — unchanged from the current import.

## Implementation plan (next slice, default-OFF `MC2_MECH_IMPORT_GPU`)
**Route B (recommended) — keep ONE geometry shape, no empty nodes:**
1. Import provides a per-vertex BT-bone-index side table (from the existing
   `ImportPartRec` vOff/vCount + boneIndex), keyed by the imported `TG_TypeShape`
   (imported-mech-only side buffer — no `TG_TypeVertex` ABI change).
2. `registerTypeLod`: for an imported-skinned type, set `rec.numBones = 28` (override
   `GetNumShapes()`), and pack `boneIndices[0]` = the side-table BT bone, `boneWeights[0]=255`,
   instead of `nodeIdx`.
3. `submitActor`: for an imported actor, push the 28 per-frame joint-globals (converted
   to the `GpuMechBone` packing above) into `ps.bones`, bypassing the `listOfShapes`
   gather. Source = the matrices `TickImportedMechs` already computes
   (`EvaluateClipGpuBones`) — expose them to the batcher keyed by type.
4. Drop the CPU re-bake for imported mechs when on the GPU path (keep it as the
   `MC2_GPU_MECHS=0` fallback).

**Route A (alt) — 28-node TG_MultiShape** (27 empty nodes + 1 geometry node), let the
stock gather read 28 `shapeToWorld`s. Rejected for now: empty nodes risk the
per-mesh-empty GPU crash the merge was created to avoid, and shapeToWorld would still
need overwriting from the clip.

**Parity gate:** `scripts/mech_bone_parity.py` + a new check that the GPU-packed
matrices == CPU re-bake for one vertex/clip/frame. tier1 PASS; stock mechs byte-identical
(gate off). RenderDoc: confirm 28 matrices land at `baseBoneOffset`.

## Residual to verify at implementation (not blockers)
- Exact float→GpuMechBone index mapping (do the one-vertex parity check first).
- Where to source the 28 matrices in `submitActor` without per-frame recompute (cache
  the GpuMechBone-packed set in the imported-anim registry each tick).
- Confirm imported type takes the GPU mech draw path (not excluded by any recipe gate).

**Conclusion: proceed to implementation (BT2018-SKEL-ENGINE-1B-GPU-SKINNING-1) — the
SSBO/layout compatibility this recon was gating on is proven.**
