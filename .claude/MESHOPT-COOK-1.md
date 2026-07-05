# MESHOPT-COOK-1 — Offline LOD-simplification cook (Truth-First arc P2 #4)

An **offline** CLI that generates lower-poly LOD `.glb` variants of MC2 props/mechs
using `meshopt_simplify`. LOD simplification **only** — reversible, opt-in, and
byte-isolated from the engine build.

- Source: `tools/cpp_probes/meshopt_cook.cpp`
- CMake: `tools/cpp_probes/CMakeLists.txt` (target `meshopt_cook`, gated by the
  existing `ENABLE_ASSET_CPP_PROBES` option; links **only** `meshoptimizer` +
  the vendored `cgltf.h` — never `mclib`/`gameos`/renderer, same isolation as
  `meshopt_lod_probe`).

## HONEST SCOPE — what is and is NOT delivered

- **DELIVERED: LOD simplify only.** Per-primitive `meshopt_simplify` at one or
  more target ratios, all vertex attributes preserved (POSITION / NORMAL /
  TEXCOORD_n and, for skinned mechs, JOINTS_n / WEIGHTS_n).
- **OUT: vertex-cache / vertex-fetch reorder GPU wins.** A prior recon confirmed
  the MC2 runtime re-welds GLB verts via Assimp `aiProcess_JoinIdenticalVertices`
  on import, so any baked reorder would not survive to the GPU. The cook therefore
  does **not** reorder and makes **no** reorder/vcache claim. (It does compact the
  vertex set to only what the simplified index buffer references — that is LOD
  byproduct, not a cache-locality claim.)
- No runtime/engine TU changed; no mc2/EditRel build touched; no game behavior
  change. Originals are never mutated.

## How to invoke

```
meshopt_cook --in <src.glb> [--out-dir <dir>] [--ratios 0.5,0.25]
             [--target-error 0.01] [--lod-suffix _lod]
meshopt_cook --self-test-skinned      # synthetic skinned mesh; writes nothing
```

- Defaults: `--ratios 0.5,0.25`, `--target-error 0.01`, `--lod-suffix _lod`.
- Outputs: `<out-dir>/<stem><suffix><N>.glb` (e.g. `tree_lush_lod1.glb`,
  `tree_lush_lod2.glb`). `--out-dir` defaults to the source's directory.
- Reports per-primitive before/after vertex+index counts and achieved meshopt
  error to stdout.

## Build (via the ENABLE_ASSET_CPP_PROBES path — NOT mc2)

Canonical wiring lives in `tools/cpp_probes/CMakeLists.txt` behind
`ENABLE_ASSET_CPP_PROBES=ON` (which also requires `ENABLE_ASSIMP_IMPORTER=ON`,
because the sibling `assimp_probe` links assimp). In a full worktree with the
unpacked `3rdparty/` (SDL2 present), configure the root project with those two
options ON and build only the `meshopt_cook` target — no mc2 target is built.

In THIS agent worktree the full `3rdparty/` (SDL2) was not unpacked, so the root
configure can't complete. `meshopt_cook` links only `meshoptimizer` + `cgltf`, so
it was built through an isolated mirror project (`build64-cook-iso/`) that uses
the identical link/include wiring:

```
cmake -G "Visual Studio 17 2022" -A x64 -S build64-cook-iso -B build64-cook-iso/out
cmake --build build64-cook-iso/out --config Release --target meshopt_cook
```

Build tail:
```
meshoptimizer.vcxproj -> ...\build64-cook-iso\out\meshopt-build\Release\meshoptimizer.lib
meshopt_cook.cpp
meshopt_cook.vcxproj -> ...\build64-cook-iso\out\Release\meshopt_cook.exe
```

## Skinned-binding proof

No skinned mech `.glb` exists in-repo, so bone-binding survival was proven with a
synthetic 16×16 two-bone skinned grid (`--self-test-skinned`):

```
[self-test-skinned] in: 256 verts / 1350 idx  ->  out: 135 verts / 747 idx  err=0.009660
[self-test-skinned] JOINTS/WEIGHTS present=yes  bad-joint=0  bad-weight-sum=0
[self-test-skinned] PASS — bone bindings preserved through simplify.
```

Every surviving vertex kept a valid joint index (∈{1,2}) and weight-sum ≈ 1.0.

## Sample run (real repo asset)

Input: `data/model_overrides/source/trees/tree_lush.glb` (29,047,368 bytes,
3 primitives, ~508k tris).

```
--- LOD1  ratio=0.500 ---
  prim 0: verts 15937 -> 8498    idx 84879   -> 42438    error=0.000809
  prim 1: verts 62772 -> 33457   idx 284442  -> 142218   error=0.000259
  prim 2: verts 354527 -> 212863 idx 1156881 -> 578439   error=0.000286
  wrote tree_lush_lod1.glb  (13,247,976 bytes  — 45.6% of source)
--- LOD2  ratio=0.250 ---
  prim 0: verts 15937 -> 4628    idx 84879   -> 21219    error=0.001337
  prim 1: verts 62772 -> 19535   idx 284442  -> 71109    error=0.000668
  prim 2: verts 354527 -> 125428 idx 1156881 -> 289218   error=0.000810
  wrote tree_lush_lod2.glb  (7,512,692 bytes  — 25.9% of source)
```

Outputs validated as spec-conforming GLB v2 (magic/version/length/JSON+BIN chunk
lengths checked) and round-trip cleanly back through cgltf (`meshopt_cook --in`
re-parses its own output). Achieved index reduction matches the requested ratios
(~0.50 / ~0.25), meshopt error well under the 0.01 target.

## Assets in scope

7 GLBs under `data/` + `tools/` (excluding `3rdparty/` and test fixtures):

| Asset | tris (approx) |
|---|---|
| data/model_overrides/source/props/bigbox.glb | 12 |
| data/model_overrides/source/trees/tree_light.glb | 4,284 |
| data/model_overrides/source/trees/tree_lush.glb | 508,734 |
| data/model_overrides/source/trees/tree_lush_lod1.glb | 235,546 |
| data/model_overrides/source/trees/tree_small.glb | 42,885 |
| data/tgl/HangarGLB.glb | 100 |
| data/tgl/QuonsetGLB.glb | 46 |

No skinned mech GLB is currently checked in (mech import outputs are produced by
`tools/mech_import/` at build time). The cook is attribute-generic and handles
JOINTS/WEIGHTS whenever such an asset is present (proven via self-test).

## Estimated full-cook time

The one heavy asset (`tree_lush`, ~508k tris) cooks both LODs in well under a
second on this host; all other in-scope assets are ≤43k tris. A full 2-LOD sweep
of all 7 assets is a **few seconds** total, single-threaded, one process per file.

## Rollback / safety

- **Originals are never mutated.** The cook only ever writes NEW files with a
  `_lodN` suffix. Rollback = delete the generated `*_lodN.glb`; there is nothing
  to revert in the source assets.
- Re-runnable and deterministic (no randomness).
- Nothing here is wired into any default build or cook; `trackg_cook.py` still
  passthrough-copies GLBs. Adopting these LODs into a pipeline is a separate,
  future step.
```
