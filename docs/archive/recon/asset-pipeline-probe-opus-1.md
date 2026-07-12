# Asset-pipeline probe opus — summary

`TRACKG-ASSET-PIPELINE-PROBE-OPUS-1` (slice `ASSET-PIPELINE-PROBE-DOC-1`).

Reference doc. Branch: `claude/asset-pipeline-probe-opus-1`.

## 1. Purpose

This opus turned the asset-pipeline validation *scaffold* into a concrete,
validated, **offline** probe pipeline — with **zero runtime rendering changes**.
The Python gates are deterministic and machine-independent. A later, explicitly
scoped extension added two **offline C++ host tools** (`assimp_probe`,
`meshopt_lod_probe`) under a default-OFF CMake option — no runtime renderer,
material binding, asset-loading, or `mech3d.cpp` importer wiring changed.

## 2. What shipped

| Slice | Commit | Summary |
|---|---|---|
| ASSET-MANIFEST-0-EXTEND | `f5e9e71a` | Optional manifest sections, backward compatible |
| MATERIAL-AUTHORING-VALIDATION-1 | `d6948f6b` | Material authoring invariants + colorSpace conventions |
| KTX2-BAKE-PROBE-1 | `3ef71573` | Deterministic offline KTX2 bake+inspect probe |
| ASSIMP-IMPORTER-PHASE-0 | `5e82583f` | Offline `assimp_probe` C++ host tool → geometry summary |
| MESHOPT-LOD-PROBE-1 | `a9bb6127` | Offline `meshopt_lod_probe` C++ host tool → LOD stats |

Commits `f5e9e71a`…`f83f81a1` (Python phase) are merged to `claude/nifty-mendeleev`
via `afede628`. The two C++ slices land on `claude/asset-cpp-probes-1`.

### ASSET-MANIFEST-0-EXTEND (`f5e9e71a`)

Added optional, shape-only manifest sections to `tools/validate_asset_manifest.py`.
A manifest without them stays valid (the minimal fixture still round-trips).

- `geometry` — the ASSIMP-IMPORTER-PHASE-0 output contract
  (`meshCount`/`vertexCount`/`indexCount`/`materialSlotCount` ints,
  `hasNormals`/`hasTangents` bools, `bounds{min[3],max[3],radius}`).
- `lods[]` items — LOD-stat shape `{level, vertexCount, triangleCount, error?}`
  (plain/opaque entries still allowed).
- `textureRefs[]` items — cooked fields `format`/`vkFormat`/`mips`/`dims`.
- `provenance` — `{tool, toolVersion, generatedAt, sourceHash}`.
- `generatedOutputs` — `[{path, kind}]` pointers to regeneratable artifacts.

Fixture: `tests/fixtures/assets/extended_asset_manifest.json`.

### MATERIAL-AUTHORING-VALIDATION-1 (`d6948f6b`)

Material/texture authoring invariants:

- `materials[].name` / `.shader` required non-empty strings.
- `alphaMode` ∈ {`opaque`, `alphaTest`, `blend`}; `alphaTestThreshold` only valid
  with `alphaMode='alphaTest'`, number in [0,1].
- `doubleSided` boolean.
- `pbr.baseColorFactor` / `metallicFactor` / `roughnessFactor` in [0,1].
- Slot → colorSpace convention: `albedo`/`emissive` = `srgb`;
  `normal`/`orm`/`mask` = `linear`.
- Cross-check: any `normal` slot ⇒ `capabilities.hasTangents = true`.

The validator gained `--expect-fail` (invert: exit 0 only if invalid) to drive
negative fixtures. Gate: `scripts/check-asset-manifests.py` (3 valid + 3 invalid
fixtures). Pass fixture `material_validation_pass.json`; negatives under
`tests/fixtures/assets/invalid/` (`material_fail_alphamode`,
`material_fail_colorspace`, `material_fail_normal_no_tangents`).

### KTX2-BAKE-PROBE-1 (`3ef71573`)

Deterministic offline bake+inspect probe `tools/asset_probe/ktx2_probe.py`:

1. Generates a fixed 8×8 RGBA source (no randomness) via Pillow.
2. Drives the existing `tools/mc2texcook/mc2texcook.py` → `.ktx2` for the
   `albedo` and `normal` presets (subprocess, `sys.executable`).
3. Parses the KTX2 header with a self-contained struct reader.
4. Asserts `vkFormat` / `mips` (levelCount) / `dims`; nonzero exit on mismatch.
5. Emits a manifest-ready `textureRef` provenance block to the summary.

CI wrapper `scripts/check-ktx2-probe.py` skips gracefully (exit 0) when Pillow is
unavailable. Artifacts land under gitignored `out/asset-pipeline-probe/` and are
**not** committed.

### ASSIMP-IMPORTER-PHASE-0 (`5e82583f`)

Offline C++ host tool `tools/cpp_probes/assimp_probe.cpp`, linking **only**
`assimp::assimp` (not mclib/gameos — fully decoupled from the engine-coupled
runtime importer body on `claude/assimp-testing`, which binds `TG_TypeMultiShape`
/ `tglHeap`). Reads a model via `Assimp::Importer::ReadFile` with flags
`Triangulate | GenSmoothNormals | ValidateDataStructure | SortByPType` (no
`CalcTangentSpace`, so `hasTangents` reflects the source) and emits the
manifest `geometry` contract: `meshCount`, `vertexCount`, `indexCount`,
`materialSlotCount`, `hasNormals`, `hasTangents`, `bounds{min,max,radius}`.

Fixture: `tests/fixtures/assets/cube.gltf` (hand-authored 24-vertex / 12-triangle
ASCII glTF, POSITION+NORMAL, no tangents, embedded base64 buffer). On the cube:
`meshCount=1, vertexCount=24, indexCount=36, hasNormals=true, hasTangents=false`,
`radius=√3`. Note `materialSlotCount=2` — Assimp's glTF importer always appends a
default material; the tool reports the actual `scene->mNumMaterials`.

No runtime call site, no game asset-loading path.

### MESHOPT-LOD-PROBE-1 (`a9bb6127`)

Offline C++ host tool `tools/cpp_probes/meshopt_lod_probe.cpp`, linking the
vendored `meshoptimizer` static lib (wired ONLY for this tool, gated, never into
mc2). Generates a deterministic procedural mesh (33×33 grid, 1089 verts / 2048
tris / 6144 indices, sine/cosine height field — no file input, no randomness),
runs `meshopt_simplify` at target ratio 0.25, and `meshopt_buildMeshlets` for a
cluster count. Emits the manifest `lods[]` contract
`{level, vertexCount, triangleCount, error}` plus a `probe` block
`{inputVertices, inputIndices, outputVertices, outputIndices, targetRatio,
resultError, clusterCount}`. Deterministic result: L1 = 452 verts / 868 tris,
`resultError≈0.00996`, `clusterCount=23`, `outputIndices=2604 < 6144`.

No runtime LOD behavior, no DrawPacket change, no culling/impostor work, no
runtime cdag/meshpack consumption.

## 3. Toolchain truth table

From `docs/toolchain-bom.md` + `scripts/check-toolchain-bom.py`.

| Tool | Vendored | Build-integrated | Callable now | Status |
|---|---|---|---|---|
| Assimp importer | yes (`3rdparty/assimp/`) | yes — linked into mclib + offline `assimp_probe` | yes (offline `assimp_probe`) | REQUIRED_NOW |
| shader_reflect | n/a (Python) | active CI gate + goldens | yes | REQUIRED_NOW |
| mc2texcook | n/a (Python) | KTX2 producer | yes | REQUIRED_NOW |
| KTX2 runtime loader | n/a | `KtxLoader.cpp` / sidecar loader | runtime (Phase-0 RGBA8) | REQUIRED_NOW |
| validate_asset_manifest | n/a (Python) | shape gate | yes | REQUIRED_NOW |
| meshoptimizer | yes (`3rdparty/meshoptimizer/`) | yes — offline only, gated by `ENABLE_ASSET_CPP_PROBES` (`EXCLUDE_FROM_ALL`, never into mc2) | yes (offline `meshopt_lod_probe`) | REQUIRED_NOW (offline) |
| gltfpack | no (dead code inside meshoptimizer) | no | no | FUTURE |

Key facts:

- **Assimp** is vendored and *linked* into mclib (`ENABLE_ASSIMP_IMPORTER=ON`,
  default ON). The runtime `ImportGeometryFromFile()` is still a `return -1;`
  **stub** (real body on `claude/assimp-testing`, wiring touches `mech3d.cpp` —
  still deferred). The offline `assimp_probe` does NOT use that body; it reads
  Assimp directly.
- **mc2texcook.py** is a complete pure-Python KTX2 writer — REQUIRED_NOW,
  callable today.
- **KtxLoader.cpp** runtime loader exists (Phase-0 RGBA8) — runtime load deferred.
- **meshoptimizer** is now wired **for the offline probe only**
  (`ENABLE_ASSET_CPP_PROBES`, default OFF, `EXCLUDE_FROM_ALL`). The legacy
  `ENABLE_CDAG_COOKER` option remains unused; no runtime consumption.
- **gltfpack** is dead code inside vendored meshoptimizer; not wired.

## 4. Commands (offline gates)

```sh
python scripts/check-toolchain-bom.py
python tools/validate_asset_manifest.py tests/fixtures/assets/minimal_asset_manifest.json
python scripts/check-asset-manifests.py
python scripts/check-ktx2-probe.py
sh scripts/check-contracts.sh
python tools/asset_probe/ktx2_probe.py
```

### Offline C++ host tools (default-OFF; build validation)

```sh
cmake -B build64-probe -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_PREFIX_PATH=<repo>/3rdparty/3rdparty -DENABLE_ASSET_CPP_PROBES=ON
cmake --build build64-probe --target assimp_probe     --config RelWithDebInfo
cmake --build build64-probe --target meshopt_lod_probe --config RelWithDebInfo

build64-probe/out/tools/cpp_probes/RelWithDebInfo/assimp_probe.exe \
    tests/fixtures/assets/cube.gltf --out out/asset-pipeline-probe/cube_geometry.json
build64-probe/out/tools/cpp_probes/RelWithDebInfo/meshopt_lod_probe.exe \
    --out out/asset-pipeline-probe/meshopt_lod_summary.json
```

`ENABLE_ASSET_CPP_PROBES` defaults **OFF** — the normal game build is unaffected.
Both subdirs (`3rdparty/meshoptimizer`, `tools/cpp_probes`) are added only inside
that gate and `EXCLUDE_FROM_ALL`.

## 5. Generated-output locations

- Generated artifacts (`.png`, `.ktx2`, JSON summaries) → `out/asset-pipeline-probe/`
  — **gitignored, regeneratable, never committed**.
- Committed = `tools/` + `scripts/` + `tests/fixtures/` + `docs/` only.
- Note: `tools/` and `tests/fixtures/` are gitignored at repo root, so committed
  files there are **force-added** (`git add -f`).

## 6. Deferred (still out of scope)

The offline probes shipped; what remains deferred is strictly **runtime**
integration, which stays out of scope.

| Deferred | Why | Offline contract shipped | Concrete next step |
|---|---|---|---|
| Runtime Assimp mech import | requires `mech3d.cpp` `[Import] Source=` wiring + the engine-coupled importer body on `claude/assimp-testing` | `assimp_probe` + the `geometry` manifest section | ASSIMP-MECH-IMPORT-1: cherry-pick the 8-commit sequence + a tiny `.glb` fixture, runtime-gated |
| Runtime cluster-LOD | needs DrawPacket/cull/impostor changes + runtime `.cdag` consumption | `meshopt_lod_probe` + the `lods[]` manifest fields | MESHOPT-CLUSTERLOD-PROBE-1 runtime: see `docs/superpowers/specs/2026-05-19-static-prop-cluster-lod-poc-design.md` |
| KTX2 runtime load | `KtxLoader.cpp` is Phase-0 RGBA8 (mip-0 only); full mip + Basis transcoding not wired | KTX2 bake + inspect probe | KTX2-RUNTIME-LOAD-1 |

## 7. Next recommended slices (ordered)

1. **ASSIMP-MECH-IMPORT-1** — runtime-gated importer body + `.glb` fixture
   (offline `assimp_probe` already proves the geometry contract).
2. **MESHOPT-CLUSTERLOD-PROBE-1 (runtime)** — consume LOD/cluster data at runtime
   (offline `meshopt_lod_probe` already proves the stats contract).
3. **KTX2-RUNTIME-LOAD-1** — full mip-chain + Basis transcoding in `KtxLoader`.
4. Wire the offline gates (+ optional gated C++ tool build) into CI alongside
   `check-toolchain-bom.py`.
