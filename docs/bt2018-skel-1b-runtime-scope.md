<!-- doc-status: current | reviewed:2026-10-11 -->
# BT2018-SKEL-ENGINE-1B-RUNTIME — imported mech per-frame animation

Make an imported BattleTech mech MOVE: re-pose the merged TG type geometry every
frame from a looping clip (CPU re-bake), instead of one static FORCE_CLIP pose.

## Gates
- `MC2_ASSIMP_MECH_IMPORT=1` — import the BT GLB (existing).
- `MC2_MECH_IMPORT_FORCE_CLIP=<clip>` — names the clip to loop (e.g. `atlas_moveCoreIdle`).
- `MC2_MECH_IMPORT_ANIMATE=1` — **opt-in, default OFF.** Turns the static posed bake
  into a per-frame looping animation of the forced clip.
- `MC2_GPU_MECHS=0` — **REQUIRED for 1B.** The re-bake writes the shared
  `listOfTypeVertices`; only the full CPU `TransformMultiShape` re-reads it into the
  instance each frame. The GPU mech path (`_PositionsOnly`/`_HierarchyOnly` +
  GpuMechBatcher upload-once) leaves the animation frozen. GPU path = later slice.

## Design
- `mclib/mech_skel_import.cpp` `EvaluateClipGpuBones` (shared with the harness) gives
  joint globals at a clip time. Rotation-only retarget already applied (UB stays seated).
- Import (`mclib/assimp_importer.cpp`): when animating, `PopulateMergedSkinnedShape`
  records per-part `{meshIndex, boneIndex, off, vOff, vCount}` (`ImportPartRec`), and
  `RegisterImportedAnim` keeps a dedicated Assimp scene alive (session lifetime) plus
  the merged `TG_TypeShape*`, skeleton names, clip, scale, ground-Y offset, clip duration.
- Per frame (`mc2mechanim::TickImportedMechs`, called from `Mech3DAppearance::updateGeometry`):
  advance clip time by `frameLength`, loop, evaluate clip globals, and rewrite each part's
  slice of `listOfTypeVertices` with `clipGlobal(bone)·off·v·scale` + the import ground
  offset — identical math to the one-time merge. Refresh the multishape bbox. Idempotent
  on `g_mc2FrameCounter` (the second combat `update()` is a no-op), so it is safe to call
  from every mech's updateGeometry and near-free when nothing is registered.

## Known limitations (deferred)
- **Shared type → lockstep.** The merged geometry is per-chassis-type, shared by all
  instances. All actors of the imported chassis animate at the same clip phase. Correct
  per-actor animation needs per-instance vertex storage (a `TG_Shape` change). Fine for a
  single demo mech.
- **CPU path only** (see `MC2_GPU_MECHS=0` above).
- **Shadow shape** is a separate stock ASE shape — stays a frozen pose (pre-existing).
- **Triangle face normals** are not recomputed per frame (vertex normals are) — minor
  flat-shading drift; per-vertex lighting is correct.
- **One forced clip, no gesture map** — 1C maps `atlas_moveCore*` → walk/run/idle/turn.

## Verify
- Per-frame parity oracle unchanged: `scripts/mech_bone_parity.py --clip <c> --frame <n>`
  (engine bake bones == harness `gpu-bones --clip`). The re-bake reuses the same
  `EvaluateClipGpuBones`, so math parity is inherited; the runtime test is visual.
- In-engine: deploy nifty exe to `releases/mc2-win64-v0.5.0`, run mc2_24 `madcat` with
  `MC2_ASSIMP_MECH_IMPORT=1 MC2_MECH_IMPORT_FORCE_CLIP=atlas_moveCoreIdle
  MC2_MECH_IMPORT_ANIMATE=1 MC2_GPU_MECHS=0` and confirm the idle subtly moves, no collapse.
