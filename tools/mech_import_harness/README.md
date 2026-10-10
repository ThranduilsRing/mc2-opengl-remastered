<!-- doc-status: current | reviewed:2026-10-11 -->
# mech_import_harness — game-free mech importer harness

Exercise the mech GLB/FBX importer from the CLI **without launching MC2**. Turns
the dev loop from `edit → build game → deploy → smoke → maybe-see-broken-mech`
into `edit → run harness → inspect/validate (→ export-pose) → only then launch`.

Meta-fix `MECH-IMPORT-HARNESS-1`. The harness is the rig the skeletal importer
(Stage 2) is developed against.

## Slices

- **1A (this) — inspect + validate.** Assimp-only; links nothing but
  `assimp::assimp`. Reads the raw `aiScene` (skeleton, bones, inverse-bind
  matrices, weights, animation clips) and reports / validates source-data
  correctness. No MC2 coord conversion, no FK, no TG types, no export.
- **1B — FK + pose dump** (later): links the mclib TG subset + a `stubs.cpp`
  (copied from `tools/tgl_loader_standalone_spike`), runs the real MC2
  skeleton/FK conversion, and `export-pose` writes a posed **OBJ** to open in
  Blender. Cross-checked against `tools/mech_import/fk_bake.py` (numeric oracle).
- **1C — game parity** (later): dumps the exact `GpuMechBone[]` the game uploads
  to `GpuMechBatcher`, so a green harness guarantees a correct in-game skin.

## Build (standalone — does NOT touch the game's build64/)

```
cmake -S tools/mech_import_harness -B build64-harness -G "Visual Studio 17 2022" -A x64
cmake --build build64-harness --config RelWithDebInfo --target mech_import_harness
```

## Use

```
build64-harness/RelWithDebInfo/mech_import_harness.exe inspect  <model.glb|.fbx>
build64-harness/RelWithDebInfo/mech_import_harness.exe validate <model.glb|.fbx>
```

`inspect` prints geometry, materials, skeleton (bones / root / parent hierarchy /
inverse-bind det+finite), animation clips (duration / tps / channels), and weight
stats (weighted-vertex count, max weights/vertex, per-vertex weight-sum range).

`validate` exits **nonzero** on: no skeleton; no weights; bone→missing node; NaN
bind matrix; non-invertible bind matrix (det~0); per-vertex weight sum off 1.0;
clip channel → missing node; NaN/extreme vertex positions; load failure.

## Notes

- Loads with **no post-processing** so it reports the RAW authored data
  (Triangulate / JoinIdenticalVertices would mutate counts and weights).
- Source GLBs come from `FBX2glTF -i <fbx> -o out -b` (see `tools/mech_import`).
  FBX2glTF can drop the center torso in its own output, but the assembled mech is
  reconstructed via skeleton FK downstream — `inspect` reports what's actually in
  the file so converter bugs are visible here first.
