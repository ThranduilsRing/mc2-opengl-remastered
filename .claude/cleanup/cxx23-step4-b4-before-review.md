<!-- doc-status: current | reviewed:2026-10-15 -->
# C++23 step 4 B4 (raw-buffer heaps -> owned storage): before-review of `cxx23-step4-b4-notes.md`, B4.0

WHAT: the before-review B4 starts from, in the B1/B2 shape: the sealed census, what the batch changes, the invariants every sub-batch keeps, the proof per
sub-batch, the risk list. Written by lane 1 (2026-10-15) because the brief asked for it; it changes no proof standard. Every proof below is the one the
notes and the proof-by-domain ruling already set; where a sub-batch needs a render proof lane 1 cannot run, it says so and stops.
WHY: B3 ran without a before-review (b3 after-review section 3) and its census could not be re-run (cleanup review 2026-10-15). B4 starts from a script.

## 1. Census (sealed)
- Command: `py -3 tests/analyze/b4_heap_census.py` (summary), `--check` (exit 1 when the tree differs from the list), `--write` (a sub-batch commit rewrites the
  list by intent). The list is `.claude/cleanup/cxx23-step4-b4-callsites.txt`, one line per site: sub-batch, file:line, kind, heap, source text.
- Count at the seal (tip 99e226156 plus the session's logistics commit, no B4 code yet): **331 sites in 23 files** = the notes' hand census (321 in 22) plus
  collision (10 in `code/collsn.cpp`). Per sub-batch: B4.1 move 81, B4.2 terrain 165, B4.3 txm 38, B4.4 sound 33, B4.5 crater 4, B4.6 collision 10.
- Kinds the census counts: a named heap's `Malloc` / `Free`, C `malloc` / `calloc` / `realloc`, C `free`, `new T[`, `delete[]`. Comments and string literals are
  skipped. Text, not a compiler probe: UserHeap has no entry point other than `->Malloc(` / `->Free(` and C allocation cannot be deprecated, so a probe build adds
  nothing a script misses; the class `operator new` / `operator delete` rows (MoveMap, GlobalMap, MissionMap, Radio) are counted but are B5-B7 work, not B4.
- The unequal pairs, explained (the notes asked for this first):
  - `tileRAMHeap` 18 / 1, `tileHeap` 2 / 0: the buffers are released in bulk by `delete tileRAMHeap` / `delete tileHeap` in `TerrainTextures::destroy`.
  - `soundHeap` 8 / 3: bulk release by `soundHeap->destroy()` in `SoundSystem::destroy`; the three `Free`s are the betty / support bites that are swapped mid-run.
  - `radioHeap` 5 / 11 and `msgHeap` 0 / 6: `msgHeap` is a per-message alias of `Radio::radioHeap` (`msgData->msgHeap = radioHeap`), so the pair is 5 / 17; the
    surplus is the error-path frees of `Radio::playMessage` (one per early return) and the message teardown in `GameSoundSystem`.
  - `colorMapHeap` 3 / 1, `textureCacheHeap` 11 / 6, `textureStringHeap` 1 / 3: alternative load paths (three loaders of the colour map, three of the LZ
    buffers) each allocate the buffer one destroy frees; texture nodes are freed on two teardown paths.
  - `ramHeap` 1 / 0: a parameter of the burn-in JPEG loader; the caller passes `colorMapRAMHeap` and frees through it.
  - `systemHeap` 52 / 39: per owner in the sub-batch that converts it (B4.1 lists its own in section 4).

## 2. What B4 changes
A buffer that a subsystem takes from a named heap or the C heap and frees by hand becomes owned storage: `std::vector<T>` for an array whose size is known at
the allocation, `std::unique_ptr<T[]>` only where the element type must stay uninitialised for a measured reason, a `std::string` for a name copy. The heap
object itself goes when its last buffer is gone (B4 deletes `tileHeap`, `tileRAMHeap`, `radioHeap`, `soundHeap` if their lists empty; `systemHeap` stays, it
is class-routed). Class `operator new` / `operator delete` overloads are NOT touched (B5-B7).

## 3. Invariants (every sub-batch)
1. **Zero-fill is a behaviour change only for a read-before-write.** A heap `Malloc` returns uninitialised memory; `std::vector<T>(n)` value-initialises. If
   the sim reads a buffer before writing it, the old read was undefined and host-dependent; the new read is 0. A `sim_determinism` or SIMID change after a
   B4 commit is therefore a finding (name the buffer, file a BUGS row), not noise to re-baseline.
2. **Sizes from files are checked** (B2 rule D4, ruling 5): a count read from a pak, a save or a mod file sizes the vector only after a bound check; an out-of-range
   count logs one line and rejects the load. No `gosASSERT` stands in for the check.
3. **No view outlives a resize.** Where a raw pointer into a buffer is kept (door link pools, texture node data), the owner is sized once and never grows
   after the first view is taken; the commit names each kept view.
4. **Teardown order is written down** where it is observable (a buffer freed by a heap's bulk release now dies with its owner; the commit says which object).
5. **No static-duration owner** (E10): an owner is a member of the object that frees it today, never a file-static.
6. **Save layout is untouched.** A buffer that is written to a save keeps its byte layout; B4 changes who owns it, not what is written.
7. One subsystem per commit, at most 25 edited files, CRLF kept, comments WHAT+WHY; the census list is rewritten with `--write` in the same commit.
8. An include-only commit first when a sub-batch adds a standard header to a TU or header that did not have it (B2 rule D16), objhash-compared (IDENTICAL).
   `mclib/move.h` already includes `<vector>`, so B4.1 adds none and needs no include-only commit; B4.2-B4.6 check this per file.

## 4. Sub-batches and proof (proof-by-domain ruling: lane 1 is a sim lane)
| sub-batch | content | proof per commit |
|---|---|---|
| B4.1a | `MoveMap` grid and its row / column tables (`move.h`, `move_movemap_goals.cpp`, `move_movemap_escape.cpp`, 8 sites) | build + doctest; `sim_determinism check` x4 at 30 s; `saveload_roundtrip check` x5 |
| B4.1b | `MissionMap` cell map and mine counts, the special-area footprint scratch (`move.cpp`, 9 sites) | as B4.1a (the cell map is saved: invariant 6) |
| B4.1c | `GlobalMap` areas, doors, door links, path table (`move_globalmap_*.cpp`, `move_globalmap_path.cpp` teardown, ~58 sites) | as B4.1a, plus the B2.5 SIMID equivalence (door order is the sim's), plus a malformed-pak test for every count read from the file (invariant 2) |
| B4.2 | terrain, tiles, colour map, mapdata (165) | `sim_determinism` x4 (heights feed the sim) AND day + night GL captures viewed: needs the orchestrator's render-proof assignment |
| B4.3 | txm caches (38, plus the B3 `readRAW` row) | captures (render-only): orchestrator assigns |
| B4.4 | sound and radio (33) | mechlab boot + tier1 with sound; no capture signal |
| B4.5 | crater (4) | its own commit, proven in the same run as B4.2 |
| B4.6 | collision (10) | `sim_determinism check` x4 |
Each sub-batch's last commit: tier1, ASan doctest and the campaign round trip once on its final exe (proof-by-domain checkpoint).

## 5. Risk list
| risk | where | handling |
|---|---|---|
| a buffer read before written (invariant 1) | any sim buffer; most likely the `MoveMap` grid outside the window and the door-link scratch | determinism proof per commit; a diff is a finding, not a baseline rewrite |
| a kept view into a buffer that grows | GlobalMap door links (`doors_links[i][s]` points into a pool), txm nodes | size once; name each view in the commit |
| a count from the pak sizes a buffer | GlobalMap `numAreas`, `numDoors`, `numDoorLinks` (B4.1c) | bound check + log + reject; malformed-pak doctest |
| heap exhaustion behaviour changes | a full UserHeap returned null and asserted; `std::vector` throws | none of the B4 heaps is near its size in the 0.4 missions (the sizes are fixed constants); recorded per heap when it is deleted |
| bulk release hid a leak | `tileRAMHeap`, `soundHeap` buffers allocated per reload | the owner frees on its destroy; a reload-delta (ALLOC_STATS proof build) where the plan's B0 seam reaches it |
| a render-side buffer in a sim-lane batch | B4.2 / B4.3 | stop and ask for the render-proof lane; lane 1 does not run captures on its own |
| PCH overload hazard | a new standard header in a `pch_skip` TU | invariant 8 |

## 6. Open (orchestrator)
1. Render-proof assignment for B4.2 and B4.3 (notes section 3 item 3).
2. Whether this lane-written before-review stands for B4.1c, the sub-batch with a file-format reader, or an Opus pass reads it first.
