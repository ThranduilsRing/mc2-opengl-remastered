<!-- doc-status: current | reviewed:2026-10-16 -->
# C++23 step 4 B4: after-review of the batch (B4.0 - B4.6 and the extras deletion, `a3d8ce85a..e4ebb4e95`)

WHAT: lane 1's own closing review of B4 (raw-buffer heaps), in the shape of `cxx23-step4-b3-after-review.md`: what shipped, which rules held, which bent and why, what is open, the KPI.
It is the lane's account, not an independent review. The independent reads are the Sonnet cleanup reviews `reviews/lane1-2026-10-15-b41-cleanup.md`, `lane1-2026-10-15-b41c-cleanup.md`,
`lane1-2026-10-16-b42a-cleanup.md`, `lane1-2026-10-16-b42b-cleanup.md` (no blocker; every should-fix is answered in a commit named below). The review of B4.3b / B4.4
(`reviews/lane1-2026-10-16-b44-cleanup.md`) did not exist when this was written; its should-fix items are the first checkpoint work of the next session.
VERDICT: **B4 CLOSED for lane 1's files, except the items other tracks own**: 16 rows blocked on Sol claims (`tempSpecialAreaFootPrints`, `CraterManager`, the terrain block lists the object manager reads),
16 class `operator new/delete` rows that go with their classes in B5-B7, 63 rows kept with reasons (Terrain class statics, the `TerrainTextures` arena, buffers GameOS frees), 21 rows in the
texture node store that B5 takes. Notes section 4 gives every one of the 116 rows its disposition.

## 1. What shipped (oldest first)
| sub-batch | sha | content | census |
|---|---|---|---|
| B4.0 | a3d8ce85a | sealed census script `tests/analyze/b4_heap_census.py`, before-review, notes command | 331 sites in 23 files |
| B4.1a | a2651bba3 | `MoveMap` solve grid and index tables are vectors | 331 -> 323 |
| B4.1b | 2f13054fc (include-only, objhash identical), 8e49ebe5d, 889bd02fc | `MissionMap` cell grid and mine counts; a rejected map packet refuses the mission load; `mc2::pak::readExact` | 323 -> 317 |
| B4.1c | 2d049e588 (include-only), 85b6a283f, 16704d09d | `GlobalMap` areas, doors, links, path table: vectors with views; every pak count range-checked (`globalmap_limits.h`); malformed-pak doctest | 317 -> 262 |
| B4.2a | a4540d5a3 (include-only), 928a0291a | terrain texture loader and baker scratch buffers; `tga_scratch.*` refuses a truncated TGA with a log line | 262 -> 199 |
| B4.3a | b8a3028fe (include-only), d2913b12e | texture manager node arrays and vertex index table | (in 199) |
| B4.2b / B4.6 | 6fd53a32e (include-only), 785c03fd0, 1cb2d8bc4 | `TerrainColorMap` scratch, `MapData` face cache, collision alert / grid stores | 199 -> 156 |
| B4.4 | e8ed49d0a (include-only), 38cbaf90f, 5b8f838ba | radio clips, queue and playing slot, sound bites, music table, Betty / support samples: owned vectors; the sound heap is gone; one wave-to-resource path | 156 -> 116 (with B4.3b) |
| B4.3b | 7c54a0cb2 (include-only), 34d977cee | `lightData_` vector, `retireCpuCopies()` hook for lane 2 | |
| extras | e4ebb4e95 | the per-node terrain extra stores deleted: written by the legacy path, read by nothing (notes section 5) | 116 (no census row) |
Reader hardenings (ruling 5, one log line each): global-map header and counts, mission-map packet, TGA body size, radio clip 16 MiB and sound packet 64 MiB ceilings, sound bite and music counts,
a wave without a readable header (was parsed unchecked), Betty / support samples with a damaged packet return -1 instead of asserting.

## 2. Rules that held
Invariant 1 (zero-fill is a behaviour change only for a read-before-write): no `sim_determinism` or SIMID change after any B4 commit. Invariant 2 (sizes from files checked): every reader above.
Invariant 3 (no view outlives a resize): the `GlobalMap` door-link pool, texture node light store and `vertexNodeExtras` (now deleted) were sized once. Invariant 5 (no static-duration owner): the
Terrain class statics, `s_blockMeta` and friends were NOT converted for that reason. Invariant 6 (save layout untouched): saveload x5 green on every sub-batch except the known mc2_10 bytes. Invariant 8
(include-only first, objhash identical): B4.1b, B4.1c, B4.2a, B4.2b, B4.3a, B4.3b, B4.4 each have one.

## 3. Rules that bent, and why
- **Render proof under an assignment.** B4.2 and B4.3 are render-side; lane 1 ran the same-exe GL pairs (mc2_01 day, mc2_10 night, viewed) because the orchestrator assigned them (before-review section 6 item 1). The mc2_01 waterline band (35 px of noise on one exe) is the capture noise floor, not B4.
- **B4.2 and B4.5 were split further than the plan.** B4.2 became a/b (loader scratch, then colour map scratch); B4.5 crater did not run (Sol claims the construction sites).
- **The mc2_10 saveload check is red on the known bytes** (`WeaponBoltData` byte 568, `gosTextureHandle`, BUGS row 45) on every exe since the lane 2 GameOS texture work. Not B4; the digest is reported, the baseline is not re-recorded.

## 4. Open items
1. Sol: `tempSpecialAreaFootPrints` (4 rows), `CraterManager` (4), the terrain block lists (8); the `WeaponBolt::CopyTo` handle (BUGS row 45, then re-record or mask bytes 568-571 for mc2_10); the review N2 (`saveload_load.cpp:134`).
2. B5-B7: the 16 class `operator new/delete` rows (MissionMap, GlobalMap, MoveMap, MapData, Radio, CollisionSystem, CollisionGridNode); the Terrain statics (B7 mission data).
3. B4.2c (render-side): the `TerrainTextures` arena and its two scratch rows; lane 2's switch from `free(cpuColorMap)` to `retireCpuCopies()`, then the `cpuColorMap` / `cpuDispAlpha` rows.
4. The `collisionHeap` follow-up and the cleanup review of B4.3b / B4.4 (should-fix items due).
5. The test_tga_scratch log-line fixture of the earlier review.

## 5. KPI before / after (B4 start `a3d8ce85a^` to the close)
| measure | before | after |
|---|---|---|
| B4 census sites | 331 in 23 files | 116 (15 files); 0 unexplained |
| `cxx17_ratchet` TOTAL (includes merged work) | 15,818 | 15,671 (own rise: LG05 +2, `soundsys.cpp` refusal returns) |
| `dup_lines_ratchet` sum | 55,657 | 55,326 |
| hand-paired heap `Malloc` / `Free` pairs removed | | 215 rows (331 -> 116) |
| B4 code commits (`^B4`) | | 21; +1,442 / -1,288 lines in `mclib`, `code`, `tests/unit` |
| doctest `TEST_CASE(` | | 3,251 at the close |
| latent defects closed | | BUGS row 41 (global-map header cursor), a 3-vertex write past the end of a terrain-extra store (the store is now deleted), a radio message pushed out of a full queue leaked, unchecked wave header |
Proofs on the final exe are in `lane1-state.md`.
