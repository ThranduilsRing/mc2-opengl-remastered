<!-- doc-status: current | reviewed:2026-10-11 -->
# BT2018-SKEL-GPU-DRAW-ENGAGEMENT-RECON-1 — findings

An imported BT2018 mech, wired onto the existing GPU skinned-mech path
(BT2018-SKEL-ENGINE-1B-GPU-SKINNING-1 prototype, default-OFF `MC2_MECH_IMPORT_GPU`),
renders **nothing** even though the data path is correct. This recon localizes the
root cause. The prototype code was **reverted** (speculative `ASgpu` swap + diagnostics);
only this doc is committed. Implementation continues as a fresh slice
**BT2018-SKEL-GPU-PALETTE-PLACEMENT-1**.

## Killed theories (with the evidence that killed each)
- **Draw not issued** — KILLED. The main flush issues the draw:
  `DRAW pkt=0 indexCount=114198 instCount=1 instBase=0 tex=974 baseVtx=0`.
- **Late registration / geometry absent from the immutable VBO** — KILLED.
  `firstPacket=0` and `baseVtx=0` prove the imported type registered FIRST (before any
  stock mech), so its 114198 verts sit at the front of `s_stagingVbo` and were included
  by `finalizeGeometry`'s single `glBufferStorage` upload. `submitActor` did not reject it
  (the draw issued and the per-frame tick ran). The `s_pendingLateTypes`/`finalizePending`
  re-upload path (`gos_mech_batcher.cpp`) was never taken.
- **Zero / unfilled bone palette** — KILLED. Dumped at draw time the palette is finite and
  real: `bone0[diag]=-0.93,-0.016,0.017,t=-2.51`, `bone5[diag]=-1.0,…`. At near-rest the
  delta `D = Swap`, which matches (`-1` on x, y/z swapped) — i.e. the palette is the
  intended value, not zero, at the frame drawn.

## Measured evidence
- register → submit → packet → finalize all succeed: `numBones=28` (override 1→28),
  `vertexCount=114198`, `packetCount=1`, `finalized=1`, `submitActor pushed=28`.
- draw call: valid `indexCount`, `instanceCount=1`, live texture slot `974`.
- draw-time palette: 28 finite non-zero matrices (`≈Swap` at rest).
- one-vertex CPU-vs-GPU parity (after the shader's `(-x,z,y)` swap): diff ~1e-6 across 3
  bones — the skinning **math** is correct.
- **THE decisive datum:** the sampled transformed coordinates are `cpu≈(-7, 34, -7)` —
  **model scale (~mech height, ~30u)**, NOT map/world scale (hundreds–thousands). The mech
  is being transformed to the **map origin / model space**, not to its placement on the map.

## Root cause
The imported GPU bone palette is in **model space — it omits the actor's world placement**.
- Stock GPU mechs carry placement *inside the bone matrices*: their per-node `shapeToWorld`
  = `TransformMultiShape(xlatPosition, qRotation)` (node-local → **world**, includes the
  actor's map position + heading). The shader does `boneT * vertex` then its `(-x,z,y)`
  swap and MVP — there is **no separate per-instance model matrix** (`GpuMechInstance`
  carries only `typeLodRecordIndex / baseBoneOffset / light / flags`).
- The prototype palette was a pure per-bone *model-space* delta
  (`D_i = (Swap·A·S·C_i)·(A·S·R_i)^-1`) with **no `xlatPosition`/`qRotation`**, so every
  vertex lands near the model origin → off-screen / under terrain → invisible.

## Critical design fact (for the fix)
Placement MUST live in the bone/palette matrices for GPU mechs — there is no instance
model matrix to put it in. The imported path therefore cannot rely on a separate transform;
it must compose the actor's root transform into each uploaded bone matrix.

## Fix recommendation → BT2018-SKEL-GPU-PALETTE-PLACEMENT-1
Fold the actor placement into the imported palette using the actor's already-computed
single-node `shapeToWorld` (the hierarchy path `_HierarchyOnly` populates it with
placement, in Stuff space). Conceptually:

```
palette_i = shapeToWorld_root · model_delta_i
```

where `model_delta_i` maps the imported assembled-rest VBO vertex to the imported animated
**model** pose (Stuff space, NO axis swap — the shader's own `(-x,z,y)` then handles
Stuff→GL, exactly as for stock). **Remove the speculative `ASgpu`/`Swap` route** — with
`shapeToWorld_root` supplying both placement and the stock Stuff convention, the manual
swap is almost certainly wrong; only reintroduce it if a fresh parity re-proves it after
placement is added.

## Acceptance (for the fix slice)
- `MC2_MECH_IMPORT_GPU=1` imported mech **visible** in the targeted screenshot.
- draw-time sampled transformed coords are **map/world-scale**, not origin/model-scale.
- palette finite/non-zero at submit; rest/clip parity within tolerance (now including
  actor placement).
- `MC2_MECH_IMPORT_GPU` unset → behavior byte-unchanged; stock GPU mechs unchanged.
- build + targeted smoke/screenshot pass.

## Explicit non-goals (fix slice)
No shader edits · no `GpuMechVertex`/`TG_TypeVertex` ABI changes · no VBO re-bake · no
late-type rebuild · no broad GPU mech refactor — only a narrow imported-type branch.
