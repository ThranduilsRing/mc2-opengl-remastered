<!-- doc-status: stale | reviewed:2026-10-11 -->
# Disk scout: untracked and ignored content in A:/Games/mc2-main

Scan date: 2026-10-07. Read-only scout. Nothing was deleted, moved, built, or committed.
Source: `git status --ignored --porcelain` (383 ignored entries, 7 untracked entries), then `du -sk` and newest-file mtime per item.

Classes: KEEP (needed by current build or tests), REGENERABLE (rebuilt automatically), ARCHIVE (move to E:/mc2-archive-2026-10), DELETE (junk).

## Totals by class (reclaimable from A:)

| Class | Size | Notes |
|---|---|---|
| ARCHIVE | ~16.2 GB | Dominated by release_assets/0.5.2 (10.98 GB) and old visual captures (~2.0 GB) |
| REGENERABLE | ~0.77 GB | Cook staging and outputs, graphify cache, old build/ caches, run/, terrain outputs |
| DELETE | ~0.01 GB | Root logs, build_logs, pytest cache, stray .obj |
| Total reclaimable | ~17.0 GB | |
| Conditional, not counted | 0.35 GB | build64/out/ (inside build64; deleting forces a full relink) |

Free space on A: is 187 GB available of 839 GB, so cleanup is optional.

## Top 10 items by size

| # | Path | Size | Newest file | Class |
|---|---|---|---|---|
| 1 | release_assets/0.5.2 | 10.98 GB | 2026-08-02 (REVIEW) | ARCHIVE |
| 2 | tests/visual/captures | 2.71 GB | 2026-10-05 | ARCHIVE 2.05 GB, KEEP 0.66 GB |
| 3 | tests/smoke/artifacts | 2.07 GB | 2026-10-05 | ARCHIVE 1.94 GB, KEEP 0.13 GB |
| 4 | build64 | 0.79 GB | 2026-10-05 | KEEP |
| 5 | mods/dist | 0.575 GB | 2026-06 | ARCHIVE |
| 6 | tests/smoke/captures | 0.415 GB | 2026-05-20 | ARCHIVE |
| 7 | mods/modern-tree-pack-v1/_cook_staging | 0.336 GB | 2026-06-12 | REGENERABLE |
| 8 | mods/modern-tree-pack-v1/data/model_overrides/cooked | 0.242 GB | 2026-06-12 | REGENERABLE |
| 9 | tests/visual/baselines | 0.191 GB | 2026-06-22 | KEEP |
| 10 | tests/terrain/gaea | 0.170 GB | 2026-06-26 | ARCHIVE |

Next in size: graphify-out/cache (0.106 GB, REGENERABLE), build64-tests (0.088 GB, KEEP), 3rdparty/lib (0.085 GB, KEEP), tests/visual/compare (0.071 GB, ARCHIVE), tests/terrain/beautify (0.059 GB, KEEP), build/ (0.039 GB, REGENERABLE), .claude/archive-2026-07-05 (0.032 GB, ARCHIVE), run/ (0.031 GB, REGENERABLE).

## Full inventory

### release_assets/ (11.0 GB, ignored)

| Item | Size | Newest | Class | Why |
|---|---|---|---|---|
| 0.5.2/mc2-gamedata.zip | 4.39 GB | 2026-07-23 | ARCHIVE | v0.5.2 payload. Release is unshipped per memory, so keep a copy off A: |
| 0.5.2/REVIEW | 3.33 GB | 2026-08-02 | ARCHIVE | Review kit (INDEX.html, 15_user0803_sweep). Not needed by the build |
| 0.5.2/mc2-burnins-4x-pt1.zip | 1.49 GB | 2026-07-23 | ARCHIVE | Release zip |
| 0.5.2/mc2-burnins-4x-pt2.zip | 0.88 GB | 2026-07-23 | ARCHIVE | Release zip |
| 0.5.2/mc2-remastered-engine.zip | 0.62 GB | 2026-07-23 | ARCHIVE | Release zip |
| 0.5.2/mc2-movies.zip | 0.16 GB | 2026-07-23 | ARCHIVE | Release zip |
| 0.5.2/mc2-art.zip | 0.09 GB | 2026-07-23 | ARCHIVE | Release zip |
| 0.5.2/tracy, manifest, notes, report | 0.02 GB | 2026-07-23 | ARCHIVE | Small release metadata |
| tracy-baselines | 0.015 GB | - | KEEP | Perf baselines |

### tests/visual/ (about 3.0 GB, ignored)

| Item | Size | Newest | Class | Why |
|---|---|---|---|---|
| captures/vk-ff459a4 | 0.66 GB | 2026-10-05 | KEEP | Active VK parity capture, today's work |
| captures/reset-50331d5-r1, r2, r3 | 1.95 GB | 2026-10-05 | ARCHIVE | Run-to-run repeats of the reset golden. Evidence for the instability work. The golden lives in goldens/ |
| captures/mc2_* and island_* (about 25 dirs) | about 0.10 GB | 2026-06 to 2026-07 | ARCHIVE | Old captures |
| captures/*.log (2 files) | under 0.1 MB | 2026-10-05 | DELETE | Log files |
| baselines/ (216 files) | 0.19 GB | 2026-06-22 | KEEP | Visual gate references, including r2b-* subdirs |
| goldens/ | 0.016 GB | 2026-10-05 | KEEP | reset-50331d5 golden |
| compare/ | 0.071 GB | 2026-06-26 | ARCHIVE | Old diff outputs |
| macro_on, macro_off, gallery, feather_* and displace_* PNGs | about 0.015 GB | 2026-06-25 | ARCHIVE | Old one-off images |

### tests/smoke/ (about 2.5 GB, ignored plus untracked)

| Item | Size | Newest | Class | Why |
|---|---|---|---|---|
| artifacts/diag-shots (297 files) | 1.73 GB | 2026-07-30 (files from 2026-05) | ARCHIVE | Old diagnostic screenshots and logs |
| artifacts/2026-10-05T* (13 dirs), newest 8 kept | about 0.13 GB | 2026-10-05 | KEEP | Today's runs. Keep 2026-10-05T19-50-08, 19-45-38, 19-42-56, 19-25-25, 19-22-05, 19-13-26, 19-09-39, 19-06-18 |
| artifacts/2026-10-05T15-12-15, 17-08-37 | about 0.055 GB | 2026-10-05 | ARCHIVE | Older runs from today, superseded |
| artifacts/2026-07-*, 2026-08-02 and others (about 190 dirs) | about 0.2 GB | 2026-07 to 2026-08 | ARCHIVE | Old smoke runs |
| captures/ (.rdc from 2026-05-20, 412 MB, plus small probe files) | 0.41 GB | 2026-05-20 | ARCHIVE | RenderDoc capture. Large, reproducible, not needed now |
| editor/ (untracked, 14 playtest logs) | 0.0014 GB | 2026-06-11 | ARCHIVE | Old editor startup logs |
| __pycache__ | 0.0002 GB | - | REGENERABLE | Bytecode |

### Build dirs

| Item | Size | Newest | Class | Why |
|---|---|---|---|---|
| build64/ | 0.79 GB | 2026-10-05 | KEEP | Canonical build. RelWithDebInfo 0.30 GB, mc2.dir 0.11 GB |
| build64/out/ (inside build64) | 0.35 GB | 2026-10-05 | REGENERABLE (conditional) | GameOS, mclib and other object trees. Not counted in totals |
| build64-tests/ | 0.088 GB | 2026-10-05 | KEEP | Test build |
| build64-h-* (16 harness dirs) | 0.095 GB total | 2026-10-05 | KEEP | Rebuilt by tools/build_contract_harnesses.py |
| build64-asan-build.log | under 0.01 MB | 2026-05-20 | DELETE | Log file |
| build/ | 0.039 GB | 2026-06-17 | REGENERABLE | tga_extract_all 26 MB, ktx2_all_mechs 12 MB and caches. Stale June extraction output |
| build_logs/ | 0.001 GB | 2026-05-19 | DELETE | Old logs |
| build.log (879 KB), build_ppc_blend.log (512 KB), other build_*.log | about 0.002 GB total | May to June | DELETE | Old logs |

### mods/ (about 1.2 GB, ignored)

| Item | Size | Newest | Class | Why |
|---|---|---|---|---|
| mods/dist/modern-tree-pack-v1-1.0.0.mc2mod | 0.575 GB | 2026-06 | ARCHIVE | Packaged mod output. Rebuildable from source, but keep a copy |
| mods/modern-tree-pack-v1/_cook_staging | 0.336 GB | 2026-06-12 | REGENERABLE | Cook staging |
| mods/modern-tree-pack-v1/data/model_overrides/cooked | 0.242 GB | 2026-06-12 | REGENERABLE | Cook output |
| mods/modern-tree-pack-v1/data/tgl | 0.011 GB | 2026-06-12 | KEEP | Verify before touching |
| mods/magic-ballistic-weapons/data/art | 0.0015 GB | 2026-06-28 | KEEP | Mod art data |

### tests/terrain, gameassets, graphify and misc

| Item | Size | Newest | Class | Why |
|---|---|---|---|---|
| tests/terrain/gaea/ (terrain004 148 MB, two ~11 MB dirs) | 0.170 GB | 2026-06-26 | ARCHIVE | Gaea terrain test inputs and outputs. Not used by the current gate |
| tests/terrain/beautify/ (mc2_24.beauty, mc2_17.beauty, new small dirs) | 0.059 GB | 2026-07-30 and 2026-10-05 | KEEP | Beauty sidecars read by mission load (get_mission_sidecars) |
| tests/terrain/workbench/ | 0.0006 GB | 2026-06-25 | KEEP | Small |
| graphify-out/cache | 0.106 GB | 2026-06-10 | REGENERABLE | Graphify cache, rebuilt by /graphify. Keep the graph outputs |
| terrain_out/ | 0.017 GB | 2026-06-05 | REGENERABLE | Terrain generator output |
| terrain_gen_test/ | 0.007 GB | 2026-06-09 | REGENERABLE | Terrain test output |
| _harness_out/ | 0.004 GB | 2026-06-21 | REGENERABLE | Harness output |
| artifacts/ | 0.001 GB | 2026-05-19 | DELETE | Stale output |
| out/, dev_shell_out/ | under 0.001 GB | May to July | DELETE | Stale scratch |
| dev/squelch/ | 0.0003 GB | 2026-05-08 | ARCHIVE | Old scratch |
| run/ | 0.031 GB | 2026-06-15 | REGENERABLE | Old run outputs (tube_*, ww_* dirs) |
| gameassets/materials/CorrugatedSteel006A/cooked | 0.003 GB | 2026-06-17 | REGENERABLE | Cook output |
| gameassets/materials/CorrugatedSteel006A/source | 0.006 GB | 2025-10-13 | KEEP | Source |
| docs/assets/ (ignored PNGs) | 0.024 GB | 2026-06-05 | ARCHIVE | Ignored docs images. Confirm nothing references them |
| docs/superpowers/ | 0.005 GB | 2026-07-22 | ARCHIVE | Ignored brainstorm, plan and review notes |
| docs/render-backend-seams/, docs/observations/, docs/beta/, docs/archive/, docs/micro_scene_spec.md | about 0.002 GB | 2026-05 to 2026-07 | KEEP | Small, possibly still referenced. Review by hand |
| 3rdparty/lib/ | 0.085 GB | 2025-02-07 | KEEP | Prebuilt vendored libraries |
| 3rdparty/cmake/ | under 0.001 GB | 2024-12-27 | KEEP | Small |
| 3rdparty/vma, volk, vk_bootstrap (untracked) | about 0.003 GB | - | KEEP | Needed by the Vulkan build. Untracked, so decide whether to commit |
| .pytest_cache/ | 0.00007 GB | 2026-07-31 | DELETE | Cache |
| brain_task_queue_test.obj | 0.00004 GB | 2026-06-23 | DELETE | Stray build object |
| scripts, tests and tools __pycache__ dirs | small | - | REGENERABLE | Bytecode |

### Root-level ignored logs (DELETE)

Total about 9 MB across roughly 60 files. Largest: run_sliceD_1.log (3.8 MB, 2026-07-05), build.log (0.88 MB, 2026-05-28), run_sliceD_box.log (0.8 MB), build_ppc_blend.log (0.5 MB), run_sliceD_2.log (0.7 MB). The rest are small.

## Notes and caveats

- The untracked list in this worktree is short (7 entries). Large untracked items named in the root repo's git status (CODEX-HANDOFF.md, .claude scratch dirs, and so on) live in A:/Games/mc2-opengl-src, not here, so they were not measured.
- debug_state/, rt_feedback.json and sniff.dat do not exist in this worktree.
- Nothing classed DELETE is larger than 10 MB. Every large item is ARCHIVE or REGENERABLE, so moving to E:/mc2-archive-2026-10 is the safe path.
- Suggested order: (1) move release_assets/0.5.2, (2) move tests/visual/captures reset-50331d5-r1..r3 and old mc2_*/island_* dirs, (3) move tests/smoke/artifacts/diag-shots, 2026-07 run dirs and tests/smoke/captures, (4) move mods/dist and tests/terrain/gaea, (5) delete REGENERABLE cook staging and caches, (6) delete logs.
