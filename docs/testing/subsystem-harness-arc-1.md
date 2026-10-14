<!-- doc-status: current | reviewed:2026-10-13 -->
# Subsystem contract tests

The sixteen native contract suites share the doctest binary in
[tests/unit](../../tests/unit/CMakeLists.txt). Their case names retain the former
harness prefix. The migration index in
[build_contract_harnesses.py](../../tools/build_contract_harnesses.py)
is the source for named selection and the pytest coverage check.

The corpus preserves the arithmetic, header, source/asset and production leaf-TU
checks from the standalone executables. Expected-failure demonstrations use
`doctest::should_fail()` so a broken negative control also fails the corpus.
The jobs fatal-path test starts a fresh copy of the same test binary and checks
both its abnormal exit and its diagnostic trace.

```powershell
py -3 tools/build_contract_harnesses.py --run --unit
py -3 tools/build_contract_harnesses.py --run --only icon_atlas_harness
py -3 tools/build_contract_harnesses.py --run --unit-only
py -3 tools/build_contract_harnesses.py --run --asan
```

The runner configures `tests/unit` into `build64-tests`, builds `mc2_tests` in
RelWithDebInfo and trusts doctest's exit code. `--only` is repeatable and selects
case prefixes in the shared binary. `--unit` remains accepted for existing
callers. `--asan` uses `build64-tests-asan` and omits the Python suites.

The GL state guard cases own a **hidden SDL window and real GL 4.3 context**;
they do not launch the game or a mission. SDL2 and GLEW DLLs are copied next to
the binary. The remaining contracts do not need a GL context. Renderer and job
tests link the same production leaf TUs used by the former executables.

Repo-relative scans use the CMake-defined `MC2_REPO_ROOT`, with the existing
`MC2_CONTRACT_REPO_ROOT` override. HDRI assets declared in
`docs/testing/ibl_hdri_external_pack.txt` may live in external packs;
`MC2_IBL_ASSET_STRICT=1` requires local assets.

## Retained Python contracts

Four suites retain the shared
[Python contract framework](../../tools/contract_harness_common/contract_harness.py):

- `deploy_asset_contract_harness`
- `deploy_release_tree_contract_harness`
- `mech_glb_pack_contract_harness`
- `shader_variant_matrix_harness`

The default `--run --unit` command runs all four after doctest passes. Each
suite keeps its `--list`, `--test NAME`, `--json` and `--seed N` interface;
diagnostics belong on stderr and the runner grades its exit code.

The release-tree suite accepts `--release-root` or `MC2_RELEASE_ROOT`.
Unconfigured checkout-only runs retain their informational behavior.
`MC2_RELEASE_TREE_STRICT=1` requires the complete runtime payload.
`MC2_MECH_GLB_PACK_STRICT=1` requires the imported GLB pack declared in
[mech_glb_external_pack.json](mech_glb_external_pack.json).

## Scope

New native forced-edge checks belong in doctest and link the smallest production
source set needed for the proof. The protected `brain_dispatch_harness` and
`mech_import_harness` keep their standalone CMake projects and existing runners.
The existing `test_render_contract_3` target keeps its counter-stub source path.

These tests prove deterministic forced edge cases. Tier1 smoke remains the
integration proof for real missions and frames.
