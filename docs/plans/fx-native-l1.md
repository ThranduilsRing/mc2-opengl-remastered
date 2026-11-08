# Native FX data boundary (L1)

Assignment: `claude/fx-native-l1`, base `f269b312d8328aaafccec0b53a15f23ce5775efb`.
Game loading stays on gosFX/MLR. Neither legacy directory may be edited.

Frozen manifest (22 files, ceiling 25):
- `mclib/fx/fx_{format,catalog,curve,mesh_data}.{h,cpp}` (8)
- root `CMakeLists.txt`, `tools/mc2fx/CMakeLists.txt`,
  `tools/mc2fx_preview/CMakeLists.txt`, `tests/unit/CMakeLists.txt` (4)
- `tools/mc2fx/mc2fx_core.{h,cpp}`, `tools/mc2fx/main.cpp` (3)
- `tests/unit/test_fx_native.cpp`, `tools/mc2fx/fx_compare.cpp`,
  `tools/mc2fx/native_checks.py` (3)
- `tests/unit/test_vfx_frozen_clouds.cpp` (existing non-stock oracles, 1)
- `scripts/check-contracts.sh`, `tools/mc2fx/README.md`, this note (3)

Completed: bounded reader and field-based serializer; immutable catalog ownership;
versioned class/event/curve/material/mesh records; native dump/rebuild CLI; legacy
comparison oracle; truncation/corruption and synthetic nested-version fixtures.
Tool compile, 11 CLI tests and both covering contract rows passed. Stock v17:
920265 bytes, 904 effects, SHA256 0fc3f12eec5245a7544429c93fc891f0f770a4131cd4a8ca95616bf4b5361b3d;
856 events, 18151 curves, 2697 materials, 1793 meshes, 3567 bounds comparisons.
All six mod providers passed: CVE-G (both names, loose/archive), REAP3
(mc2x loose/archive), Exodus, MagicExpansion, Omnitech and Carver5 (inherits
Omnitech). All resolved payloads match the stock hash. Eight synthetic v9-17
catalogs passed byte rebuild and legacy comparison, each with 12 concrete
classes and 19 mesh families. Multipass count is one byte; legacy empty MString
is nullable. No legacy-directory edits, installs or game processes.

Next: finish covering doctests (including existing B6 multipass/debris fixtures),
commit locally, compile final tip and run its one lint. Record final evidence and
cleanup. Initial standalone build found a missing contract_process.h include;
corrected. Its cold VS build is slow; reuse the completed tree for the rerun.
Keep evidence under `E:/mc2-archive-2026-10/proofs/fx-native-l1`.
