<!-- doc-status: current | reviewed:2026-10-15 -->
# C++23 step 4, batch B4 (raw-buffer heaps): batch notes (census only; written by lane 1 on 2026-10-15, NOT yet read by a before-review, no code)

WHAT: the scope and census of B4, the batch after B3 in the ownership plan's order (`cxx23-step4-ownership-plan.md` section 6: "B4 RAW-BUFFER heaps only
(move_globalmap_* first, terrain, tile/colorMap, txm caches, crater, bitflag, sound, radio, collision)"). B4 moves buffers that a subsystem allocates from a named
`UserHeap` or the C heap and frees by hand into owned storage (`std::vector`, `std::unique_ptr<T[]>`), one subsystem per commit. Objects, shared assets and
the class-routed heaps are B5 / B7; the heaps themselves (`UserHeap`, `systemHeap`) stay until every buffer of a heap is gone.
WHY: these buffers are where size mismatches, double frees and the save-padding class of bugs live (BUGS rows 21, 22 were overruns of hand-sized buffers), and
every one of them is a `Malloc` / `Free` pair a reader must match across files. `bitflag` is already gone (B2.1).

## 1. Census (grep, 2026-10-15 at 82cff74b1; superseded by the sealed census of B4.0)
SEALED (B4.0): `py -3 tests/analyze/b4_heap_census.py --check` against `.claude/cleanup/cxx23-step4-b4-callsites.txt`: 331 sites in 23 files (the 321 below
plus collision's 10); the unequal pairs are explained in `cxx23-step4-b4-before-review.md` section 1. The hand table below is kept as the first count.
Pattern: `->Malloc(` `->Free(` `malloc/calloc/realloc(` `free(` `new T[` `delete[]`, in the B4 subsystems' files. 321 lines in 22 files.
| file | lines | | file | lines |
|---|---|---|---|---|
| mclib/terrtxm2_load.cpp | 47 | | mclib/soundsys.cpp | 11 |
| mclib/terrtxm2_bake.cpp | 37 | | mclib/move_globalmap_doors.cpp | 9 |
| mclib/move_globalmap_build.cpp | 35 | | mclib/terrtxm2.cpp | 6 |
| mclib/terrain.cpp | 29 | | mclib/move_movemap_goals.cpp | 6 |
| mclib/terrtxm.cpp | 27 | | code/gamesound.cpp | 6 |
| mclib/txmmgr.cpp | 18 | | mclib/txmmgr_lights.cpp | 4 |
| code/radio.cpp | 16 | | mclib/terrtxm2_internal.h | 4 |
| mclib/move.cpp | 15 | | mclib/move_movemap_escape.cpp | 4 |
| mclib/txmmgr_texture.cpp | 13 | | mclib/mapdata.cpp | 4 |
| mclib/move_globalmap_path.cpp | 12 | | mclib/crater.cpp | 4 |
| mclib/terrain_init.cpp | 11 | | mclib/txmmgr.h | 3 |
By kind: `Heap->Malloc` 130, `Heap->Free` 99, `malloc/calloc/realloc` 36, `free` 42, `new T[]` 7, `delete[]` 7. By heap: systemHeap 50 / 37, terrainHeap 18 / 18,
tileRAMHeap 18 / 1, colorMapRAMHeap 12 / 12, textureCacheHeap 11 / 6, radioHeap 5 / 11, soundHeap 8 / 3, msgHeap 0 / 6, colorMapHeap 3 / 1, tileHeap 2 / 0,
textureStringHeap 1 / 3, ramHeap 1 / 0. The unequal pairs (tileRAMHeap 18 / 1, radioHeap 5 / 11) are the first thing B4.0 explains: a heap reset frees in bulk, or a
buffer is freed through another name.
Also B4, not in the table: collision, `code/collsn.cpp` (10 lines, a separate small batch B4.6 with `sim_determinism`). Handed in from B3: the `File::readRAW` row of the B3 census (`txmmgr_texture.cpp:376`, handed to B4: it is in
the txm cache batch).

## 2. Proposed batches (each one subsystem, at most 25 edited files; an Opus before-review of this file first)
| batch | content | proof |
|---|---|---|
| B4.0 | compiler census (deprecate the heap `Malloc`/`Free` overloads in a probe build, like `deprecate_b3.py`), seal it, explain every unequal pair, characterization tests where a unit seam exists | none needed (no code) |
| B4.1 | move / global map (`move*.cpp`, 81 lines): the pathfinding maps and door tables | `sim_determinism check` x4 + the B2.5 SIMID equivalence (path order is the sim's) |
| B4.2 | terrain, tile, colour map, mapdata (`terrain*`, `terrtxm*`, `mapdata.cpp`, 165 lines) | `sim_determinism` (terrain heights feed the sim) + captures day and night VIEWED (lane 1 is a sim lane; captures need the orchestrator's assignment) |
| B4.3 | txm caches (`txmmgr*`, 38 lines, plus the B3 `readRAW` row) | captures (render-only): orchestrator assigns |
| B4.4 | sound and radio (`soundsys.cpp`, `gamesound.cpp`, `radio.cpp`, 33 lines) | mechlab boot + tier1 with sound (no capture signal) |
| B4.5 | crater (4 lines) | its own commit, proven in the same run as B4.2 |
| B4.6 | collision (`code/collsn.cpp`, 10 lines) | `sim_determinism check` x4 |

## 3. Open before B4.1
1. Claims: `code/radio.cpp`, `code/gamesound.cpp` and `code/collsn.cpp` are not on the lane-1 list of the brief of 2026-10-15 (code/mech*, gvehicle*, mover*, saveload* and mclib); the orchestrator assigns them.
2. A before-review of this file (B3 had none: `cxx23-step4-b3-after-review.md` section 3).
3. The render-side batches (B4.2 captures, B4.3) need a render-proof lane or an explicit ruling, since lane 1 runs no captures.
