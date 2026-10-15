<!-- doc-status: current | reviewed:2026-10-15 -->
# C++23 step 4 B3: after-review of the batch (B3.0 - B3.4b, `ae15a4734..82cff74b1`)

WHAT: lane 1's own closing review of B3 (files and streams), in the shape of `cxx23-step4-b2-after-review.md`: what shipped, which rules held, which bent and why, what is open, the KPI.
It is the lane's account, not an independent review. The independent reads are the three Sonnet cleanup reviews `reviews/lane1-2026-10-15-b3-cleanup.md`,
`lane1-2026-10-15-b34-cleanup.md` and `lane1-2026-10-15-b34b-cleanup.md` (no blocker; every should-fix is answered in a commit named below).
VERDICT: **B3 CLOSED for lane 1's files, except the items other tracks own**: `code/mission_init.cpp` (4 rows, Sol), B3.5 `ABLFile` (8 rows, Codex), the editor rows
(4 `File` new/delete in `editor/Editor.cpp`, E-batches) and therefore B3.6, the deletion of `File::operator new/delete` and `ABLFile::operator new/delete`, which needs all three. Section 4 names them.

## 1. What shipped (oldest first)
| batch | sha | content | class |
|---|---|---|---|
| B3.0 | ae15a4734 | notes, sealed compiler census (132 sites in 55 files), `deprecate_b3.py`, `file_callsites.py`, contract tests FC1-FC5 on the OLD classes | no behaviour |
| B3.1a | f6f7a0c33 | include-only (D16) | objhash 0 DIFF |
| B3.1 | fee3887f2 | the file traffic log deleted | dead code |
| B3.1b | b2c53f243 | File-family name, child list, RAM image, seek and block tables are std containers; FC6-FC8 | internal; reader hardenings listed in notes 7b |
| B3.3a | 493ce4a3d | 22 scope-bound `new`/`delete` pairs become values | closes early-return descriptor leaks |
| review S1-S6 | 8e64db524 | save mask unmasked fixed bytes, ABL scalar slot zero-extend, packet header STOP -> log line, File image size guards | BUGS rows 34, 35 |
| B3.3b | ad097664d | six `DEBUG_MOVE_MAP` blocks deleted (compiled nowhere) | dead code |
| B3.4 | 076159051, afe0e21a7 | owners that outlive a function hold `unique_ptr` (sound, radio, object pak, terrain list, game log, ABL file callbacks) | teardown order per owner (notes 10.1) |
| F4 | abbc6afc8 | a rejected oversized FST entry releases its archive slot; crafted-entry test | reader hardening |
| fit_number.h | 7c3456d1f, 2cc50eb68, 55f62bc4d | the FIT number readers as free functions the editor can include; lockstep doctest | no behaviour |
| B3.4b | ced027df9 | gamesys.fit of the save load is a frame value; pak tool files are locals; census 41 -> 35 | no behaviour |
| debug files | 2cbd20486 | two static-duration `_DEBUG` File owners deleted, six debug reports are values | debug builds only |
| proof tooling | d4bb734a0, 110251737 | `saveload_roundtrip check` refuses other frames than the baseline's, and since 110251737 takes them from the baseline before launching | tooling |
| found by the proof | 82cff74b1 | mech CSVs parsed serially: the parallel parse raced on the shared FastFile handle (BUGS row 37) | DETERMINISM, player-visible in the mech bay |

## 2. Rules that held
D16 (include-only commits before the type change, objhash-proved), E1 (scope-bound pairs become values, `close()` where `delete` stood), E2 (an owner that outlives the function is a
`unique_ptr` member with its teardown order written), E10 (no static-duration owner was added; the statics that remain are views of a non-static owner), ruling 5 (every reader
touched rejects malformed content with one log line: packet header, File image size, FST entry size, ABL slot upper bytes, a mech file that does not open).

## 3. Rules that bent, and why
- **B3.4 ran without a before-review** (notes 10.2 item 1). The notes were never read by a before-review as B1 and B2 were; the three after-reviews stand in for it. Orchestrator's call.
- **Tier2 "before" for B3.1b was never recorded** (notes 10.2 item 2); the "after" is notes 10.3. The owner's ruling put the malformed-content tests in place of tier3.
- **`fit_number.h` is a verbatim copy** of the `FitIniFile` bodies until `editor/CMakeLists.txt` includes the header (forwarding broke `editor_document`); the dup_lines rise is named in 0f413f7a6.
- **The mc2_16 save failure was first filed as host-load flakiness** (notes 10.5). It was a real race (section 5); the mask was not widened.

## 4. Open items
1. `code/mission_init.cpp`: 4 rows (`gameSystemFile` / `missionFile` in `MissionInitFrame`), Sol's claim.
2. B3.5 `ABLFile` (`ablenv.cpp`, `ablscan.cpp`, 8 rows): Codex track. The unit test of the ABL slot writer (review S2) does NOT exist on nifty at 82cff74b1 (`git grep` of `tests/` finds no ABL slot test); it is owed with B3.5.
3. B3.6: no lane-1 site is left. `File::operator new/delete` goes when rows 1, 2 and the editor's 4 are gone; the `make_unique<File>` / `make_unique<PacketFile>` lines of B3.4 (notes 10.1) stop routing through the class allocator then, with no edit of their own.
4. The seven `_DEBUG` `pathDebugFile` blocks (B3.3b): orchestrator's call.
5. `Logistics::begin` still runs `initializeLogData` on a `std::async` thread while the main thread can read files (`code/logistics.cpp`, intro movie): the same shared-FastFile race class as row 37, not reached by the smoke path (`justStartMission`). Owed: serialise it or give FastFile a per-thread read path.
6. The oversized-FST test exists (`tests/unit/test_fst_roundtrip.cpp`, "an FST entry whose uncompressed size is implausible is rejected and released"), checked with every slot released since 110251737.

## 5. The race the proof found
`saveload_roundtrip check mc2_16` failed once under host load (notes 10.5). The packet diff of that save against the quiet save of the same exe (`%TEMP%/resave_kqyx8ep6` vs
`resave_o7n6srfg`, mask applied) leaves 6 bytes outside the mask: `MechData` offset 3808-3810 of packets 5831 and 5845, i.e. `cBills` (the record ends with the 180-byte
`MechAppearanceData` at 3812, whose `mechFrameRate` 30.0 confirms the layout). Both records are "Mad Cat Prime"; the loaded run saved 10, the quiet one 121650. `Mech::init`
writes 10 when `LogisticsData::getVariant` does not find the variant, so the Mad Cat chassis was missing from the variant list. `initVariants` parsed each mech CSV on its own
`std::async` thread (6a149c4fc), and a FastFile archive reads through one shared `FILE*` (seek, then read) and one shared LZ buffer. None of the 62 other mc2_16 / mc2_xx saves
on disk carries a 10. Not a per-frame field: the review's suspects (`lastJumpTime`, the `*ThisFrame` flags, `rotateValues`) were all equal.

## 6. KPI before / after (B3 start `ae15a4734^` to the close)
| measure | before | after |
|---|---|---|
| B3 census sites | 132 in 55 files | 35 in 21 files: 18 `FitIniFile::open(parent, size)` child views that stay (17 game, 1 editor), 4 `mission_init.cpp` (Sol), 8 ABLFile (Codex), 4 editor `File` new/delete, 1 `readRAW` (B4) |
| `own_inventory` File-family `new` | File 28, FitIniFile 17, PacketFile 6, ABLFile 6, CSVFile 1 | File 8, FitIniFile 5, ABLFile 6 (PacketFile, CSVFile gone) |
| static-duration File owners | 1 release + 2 debug | 0 |
| `cxx17_ratchet` TOTAL (21 idioms, includes merged work) | 15,993 | 15,818 |
| latent defects closed | | BUGS rows 34, 35, 37 and the reader hardenings of notes 7b |
Proofs on the final exe are in `lane1-state.md` and notes section 10.6.
