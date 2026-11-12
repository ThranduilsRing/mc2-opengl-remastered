<!-- doc-status: current | reviewed:2026-10-11 -->
# Toolchain bill-of-materials

`TOOLCHAIN-BOM-CHECK-1` (VALIDATION-SCAFFOLD-PREFLIGHT-1).

Inventory of the build & asset tools the upcoming pipeline lanes
(`TRACKG-ASSET-PIPELINE-PROBE-OPUS-1`, `TRACKRV-HZB-VISIBILITY-OPUS-1`) depend
on. Checkable via `scripts/check-toolchain-bom.py` (repo-relative detection; no
machine-only paths, no build cache). The script fails only on a missing
`REQUIRED_NOW` tool; missing `FUTURE` tools warn.

| Tool | Status | Present | Detection (repo-relative) |
|---|---|---|---|
| Assimp importer | REQUIRED_NOW | yes | `3rdparty/assimp/CMakeLists.txt` + `ENABLE_ASSIMP_IMPORTER` in `CMakeLists.txt` (default ON) |
| shader_reflect | REQUIRED_NOW | yes | `tools/shader_reflect/reflect.py` (active shader-reflection CI gate + goldens) |
| mc2texcook | REQUIRED_NOW | yes | `tools/mc2texcook/mc2texcook.py` (KTX2 producer) |
| KTX2 runtime loader | REQUIRED_NOW | yes | `KTX2` sidecar loader in `GameOS/gameos/gos_static_prop_batcher.cpp` |
| validate_asset_manifest | REQUIRED_NOW | yes | `tools/asset_cook/validate_asset_manifest.py` (ASSET-MANIFEST-SCHEMA-SCAFFOLD-1) |
| meshoptimizer | FUTURE | vendored | `3rdparty/meshoptimizer/` (v1.3, 9e1f07b; cooker FUTURE, engine already links a subset); GameOS `mc2_meshopt` links 5 core files (vertex cache/overdraw/fetch/index analyzer); `ENABLE_CDAG_COOKER` still OFF, `cdag_cooker` target not built yet |
| gltfpack | FUTURE | no | not wired at repo level (source exists only inside vendored meshoptimizer) |

## Status meaning
- **REQUIRED_NOW** — exists today and is load-bearing for the current build/CI or
  the validation scaffold. The check FAILS (exit 1) if missing.
- **FUTURE** — planned for an upcoming lane; vendored-but-dormant or not yet
  present. The check WARNS only (exit 0).

## Notes for the asset-pipeline lane
- `meshoptimizer` core is wired into the engine only as the `mc2_meshopt` subset; the cooker is unwired (`ENABLE_CDAG_COOKER` default OFF, no
  `cdag_cooker` target / `add_subdirectory`). Wiring it (the CDAG cooker) flips
  it to REQUIRED_NOW — update both this table and the check's `TOOLS` list.
  Vendored gltf/, js/, demo/, tools/ are stale dormant upstream copies, not upgraded; only src/ + demo/clusterlod.h refreshed to v1.3.
- `gltfpack` is not a repo tool today (its source is dead code inside vendored
  meshoptimizer). If a glTF mesh-cook step lands, add a repo-level wiring and
  flip it to REQUIRED_NOW.
- `validate_asset_manifest` is the shape gate for asset manifests; see
  `docs/asset-manifest-schema.md`. Extend it rather than inventing a new format.
