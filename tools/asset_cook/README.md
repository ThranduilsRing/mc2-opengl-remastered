# tools/asset_cook — Track G offline static-prop cook

The cooked-asset contract + validator. Slice `TRACKG-OFFLINE-GLB-COOK-MANIFEST-1`.
Plan: `docs/superpowers/plans/2026-06-04-trackg-offline-glb-cook-manifest-plan.md` (file not present in tree).

**G3a DONE:** the manifest contract (data before behavior).
**G1 DONE:** offline GLB staging — `trackg_cook.py stage` computes geometry in the runtime
importer's convention and emits a `staged.json` fragment + cooked glb.
**G2 DONE:** KTX2 material cook — `trackg_cook.py textures` cooks discovered materials' albedo
to stored-BC7 KTX2 tiers (`ktx.exe` uastc->bc7) and emits `materials.json` with the tier map.
**G3b DONE:** assemble — `trackg_cook.py assemble` merges staged.json + materials.json into the
full schema-valid `manifest.json`, derives `capabilities{}`, and projects the runtime
`models.generated.json` (registry-resolvable; central `models.json` never written).

## Files
- `trackg_cook.py` — cook driver.
  - `stage <src.glb> <out_dir> --id --class --appearance`: replicates `assimp_importer.cpp`
    default-env transform (axis0 `X=-x,Y=-y,Z=z` + auto-ground GROUND=2) to compute MC2-space
    bounds/pivot/counts; passthrough-cooks the glb (meshopt later); discovers material names +
    alphaClass. Emits `staged.json`. RUNTIME convention, NOT workbench `GlbMeshLoader`.
  - `textures --staged --texture-dir --out-root --out-json [--tiers]`: per discovered material,
    finds `<base>.{png,tga}`, cooks albedo to `data/tgl/<tier>/<textureName>.ktx2` (stored BC7,
    sRGB, Lanczos down, never upscale — reuses `cook_tgl_tiers.py`'s `ktx.exe` recipe), verifies
    vkFormat 145/146, emits `materials.json` (tier map + `a_`/alpha_test/double_sided flags).
  - `assemble --staged --materials --out-dir [--override-source --casts-shadow --has-legacy]`:
    merges geometry + cooked materials → full `manifest.json` (re-validated against the schema),
    derives `capabilities{}` (alphaTest/hasLodChain from content), projects the registry subset
    to `models.generated.json`, and runs a Python mirror of the registry accept rules
    (`registry_resolves`) + refuses to write a file named `models.json` (central-manifest guard).
- `asset_manifest.schema.json` — `mc2-asset-manifest-v1` (draft-07). Superset of the
  override `models.json` (identity/geometry) and `material_manifest` (textures). Geometry +
  materials + capability + provenance for one cooked static-prop/tree override asset.
- `validate_asset_manifest.py` — schema + cross-field coherence the schema can't express:
  `slot==index`, `replaces == '<class>:<appearanceName>'`, **lowercase `class`** (registry
  canonical), `alphaClass ↔ flags.alpha_test ↔ 'a_' prefix` agreement, `capabilities.alphaTest
  == any alpha material`, `capabilities.hasLodChain == lods non-empty`, bounds ordering, LOD
  ascension, `deps.stockFallback == appearanceName`. `--check-files` verifies referenced
  glb/ktx2 exist (off by default; cooked artifacts don't exist until G1/G2).
- `tests/golden/bigbox/manifest.json` — golden (bigbox→`staticprop:hangar`, the R0-proven map).
- `tests/broken/*.json` — 5 fixtures, one load-bearing invariant each.
- `tests/run_tests.py` — golden must PASS, all 5 broken must FAIL **for cause**.

## Validate / test
```
py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json>
py -3 tools/asset_cook/tests/run_tests.py
```

## Contract invariants (load-bearing)
- **Class spelling = lowercase `staticprop`/`tree`** (registry `normalizeKey` lowercases;
  compare `model_override_registry.cpp:77`). Input case-insensitive at runtime; manifest emits
  lowercase. (Schema `enum` + coherence both enforce.)
- **`scale == 1.0`** (bake scale into the glb).
- **Convention = audit-only**: cooked glb baked to the runtime importer's default env
  (`assimp_importer.cpp` axis0 + `1-v`); no `MC2_GLTF_*` in the contract.
- **`albedo_ktx2` = tier map** → MaterialGpu `albedoTex` (texture-array layer at runtime).
- **`a_` prefix iff alpha** — the resolver naming convention; mismatch leaves the runtime
  texture handle unresolved.

## Pipeline (one asset, end to end)
```
[path B] workbench --export-tgl-meshdump <deploy> <tgl> <dump.json>   # stock .tgl -> MeshData JSON
[path B] tglmeshdump_to_glb.py <dump.json> <out.glb>                  # axis-inverted glb (importer round-trips)
stage <src.glb>        -> cooked glb + staged.json (geometry + materials_discovered)
textures               -> data/tgl/<tier>/<tex>.ktx2 (BC7) + materials.json
assemble               -> manifest.json (schema-valid) + models.generated.json (registry subset)
```
**Path B (stock source):** for a stock prop with no authored glb, dump it from the engine's
own `.tgl` via the workbench (`MeshData` = GL-space `(-sx, sz, sy)`), then `tglmeshdump_to_glb.py`
writes a glb with the **calibrated `x,-z,y` axis map + UV `none`** so the override importer
(`axisMap0`, auto-ground OFF) yields the engine's **raw Stuff coords `(sx,sy,sz)`** — the exact
space stock props bake through `TransformShape` (so each building's per-instance facing rotation
applies correctly). Plus an engine change: `ImportGeometryFromFile(..., autoGround=false)` for
staticprop overrides (they carry the stock pivot; trees still ground).

**Calibration story (4 wrong turns, all caught by `--screenshot`, none by the offline oracle):**
`-x,-y,z` → giant warped sails (Y/Z swap); `-x,-z,y` → 90°/180°/45° mess (X-mirror + grounding
shift); `x,-z,y`+UV `1-v` → geometry right but "backside had front textures" (UV double-flip);
**`x,-z,y` + UV `none` + ground-off → correct** (user-confirmed, hangar/quonset/dome).
**Lesson (load-bearing): the extents oracle is mirror/rotation-blind — necessary, not
sufficient. The screenshot is the real gate.** Evidence in `docs/assets/trackg-r0/`.
Authoritative round-trip is the C++ `ExportBundle`/`ModelOverrideRegistry` at integration;
`registry_resolves` is the offline Python mirror. Promotion of `models.generated.json` into
central `models.json` stays a separate reviewed merge.

**G5 DONE** (override branch `adc01edd`): engine `[RENDER_PATH v1]` descriptive log
(`MC2_RENDER_PATH=1`) — `key=staticprop:<name> isOverride=<0|1> path=<override_multidraw|
legacy_static> pools=<skipped|legacy>`. Read-only, zero behavior change.
**G-E2E bigbox DONE** (2026-06-04): cooked `cooked/bigbox/bigbox.glb` resolved + imported +
rendered via the override path on a free v0.4 `--validate` run (gl-clean, exit 0); G5 log +
case-insensitive accept confirmed live. Evidence: `docs/assets/trackg-r0/restamp_g5_e2e_evidence.txt`.

## Batch — cook the whole stock library
```
workbench --export-tgl-meshdump-all <deploy> <dumpdir>     # 2030 .tgl -> 2015 meshdumps (15 load-fail skipped)
cook_all_stock.py --meshdumps <dumpdir> --deploy-root <deploy> --out <lib>
```
`cook_all_stock.py` runs glb->stage->assemble in-process per prop, resolving existing
`data/tgl/<tier>/<name>.ktx2` tiles by sanitized name (no re-cook). Full-set run:
**2015 cooked, 0 errors — 1629 full textured manifests, 386 untextured geometry-only,
493 multi-submesh** (`docs/assets/trackg-r0/batch_cook_all_report.json`). Textured glb
materials carry the stock texture URI so the override importer binds existing tiles
(verified live: `[MODOVERRIDE_TEX] a_2civliving.tga -> gosHandle, alpha=1`).

## Next (not yet built)
G-E2E **2civliving** — author `2civliving.glb` from the stock `.ase` (content step), cook,
register, render with stock-parity via the `TglMeshLoader` footprint/pivot oracle.
Follow-ups: meshopt in `stage`; wire the in-game C++ `ExportBundle` round-trip to this manifest;
registry mixed-case-accept C++ doctest.

## Follow-up
Registry-side C++ unit test asserting mixed-case class input resolves (the case-insensitive
accept) — the manifest side is enforced here; the runtime accept is proven by R0's registry
grep but not yet a doctest.
