<!-- doc-status: stale | reviewed:2026-10-11 -->
# File-size baseline (physical lines, owned dirs)

Frozen by `tools/file_size_ratchet.py --write-baseline` at HEAD `a9fd72350`; enforced by the `file_size_ratchet` row in `scripts/check-contracts.sh`. Counts are physical lines (blank and comment lines included): the metric is the read cost of opening a file. Soft target for owned files is 800-1500 lines; the ratchet only stops the numbers rising.

Gate: per-dir `files_over_1500`, `files_over_3000`, `max_lines` may not rise, and no file may newly enter the >1500 set (a split piece that is still >1500 fails). Lower the baseline after a split with `py -3 tools/file_size_ratchet.py --check --tighten`. Machine-written files that are not named `*.gen.*` go in `tools/file_size_ratchet.exclude` with a reason.

## Per-dir metrics

| Dir | Files | >1500 | >3000 | Max lines |
|---|---:|---:|---:|---:|
| GameOS | 266 | 19 | 9 | 11819 |
| code | 279 | 34 | 13 | 9979 |
| mclib | 571 | 36 | 11 | 8317 |
| editor | 207 | 5 | 3 | 7370 |
| RenderCore | 55 | 1 | 1 | 4148 |
| gui | 21 | 2 | 0 | 2901 |
| shaders | 183 | 1 | 0 | 2488 |
| GuiRuntime | 6 | 2 | 0 | 2342 |
| RenderWorld | 6 | 0 | 0 | 1196 |
| GameAdapters | 8 | 0 | 0 | 536 |
| HostServices | 3 | 0 | 0 | 136 |
| **total** | 1605 | 100 | 37 | 11819 |

## Top 30 files

| # | Lines | File |
|---:|---:|---|
| 1 | 11819 | `GameOS/gameos/gameos_graphics.cpp` |
| 2 | 11229 | `GameOS/gameos/gos_static_prop_batcher.cpp` |
| 3 | 9979 | `code/missiongui.cpp` |
| 4 | 9796 | `code/mech.cpp` |
| 5 | 9578 | `code/warrior.cpp` |
| 6 | 8497 | `code/ablmc2.cpp` |
| 7 | 8317 | `mclib/bdactor.cpp` |
| 8 | 7817 | `code/mover.cpp` |
| 9 | 7724 | `GameOS/gameos/gos_postprocess.cpp` |
| 10 | 7640 | `GameOS/gameos/vulkan_backend_skeleton.cpp` (deleted by D7 phase 1, 938ad14b2) |
| 11 | 7370 | `editor/EditorInterface.cpp` |
| 12 | 7050 | `mclib/move.cpp` |
| 13 | 6877 | `mclib/mech3d.cpp` |
| 14 | 6330 | `GameOS/gameos/gos_terrain_indirect.cpp` |
| 15 | 6042 | `mclib/txmmgr.cpp` |
| 16 | 5946 | `code/mission.cpp` |
| 17 | 5685 | `code/objmgr.cpp` |
| 18 | 5636 | `code/gvehicl.cpp` |
| 19 | 5016 | `code/controlgui.cpp` |
| 20 | 4935 | `mclib/terrain.cpp` |
| 21 | 4642 | `mclib/camera.cpp` |
| 22 | 4385 | `GameOS/gameos/gos_terrain_lod_chunk.cpp` |
| 23 | 4355 | `mclib/tgl.cpp` |
| 24 | 4261 | `mclib/quad.cpp` |
| 25 | 4148 | `RenderCore/RendererFeatureRegistry.h` |
| 26 | 3753 | `GameOS/include/gameos.hpp` |
| 27 | 3616 | `editor/EditorObjectMgr.cpp` |
| 28 | 3576 | `GameOS/gameos/vulkan_terrain_island.cpp` |
| 29 | 3537 | `editor/EditorData.cpp` |
| 30 | 3477 | `mclib/gvactor.cpp` |

## Split-plan cross-reference (top 6)

1. `GameOS/gameos/gameos_graphics.cpp` (11819): giant-file-split-plan.md section 4 (gameos_graphics.cpp): outline, diagnostic regions, 6-file split proposal; move the `gosRenderer` class to a header first.
2. `GameOS/gameos/gos_static_prop_batcher.cpp` (11229): giant-file-split-plan.md section 2 (gos_static_prop_batcher.cpp): `flush()` alone is ~2.4k lines; diag candidate `gos_static_prop_batcher_diag.cpp`.
3. `code/missiongui.cpp` (9979): not yet covered by giant-file-split-plan.md; needs a survey before splitting.
4. `code/mech.cpp` (9796): not yet covered by giant-file-split-plan.md; needs a survey before splitting.
5. `code/warrior.cpp` (9578): not yet covered by giant-file-split-plan.md; needs a survey before splitting.
6. `code/ablmc2.cpp` (8497): not yet covered by giant-file-split-plan.md; needs a survey before splitting.

Also in the plan but below the top 6: `gos_postprocess.cpp` (section 3), `vulkan_backend_skeleton.cpp` (section 1; mechanical, lowest risk, do first), `mclib/txmmgr.cpp` (section 5).
