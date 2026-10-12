<!-- doc-status: current | reviewed:2026-10-12 -->
# Lane 3 state (debug lane; worktree A:/Games/mc2-lane3, branch claude/lane3b-mc215, install mc2-win64-lane3)

Session 2026-10-12 08:20 -> (cap 14:20), resumed after the weekly-limit cut. Task: find + fix the mc2_15 heap-corruption writer.
Merged nifty d68203bf9 (386925beb). Previous trail: `git show 018c09a06:.claude/cleanup/lane3-state.md`, `git show db0477d8b:.claude/cleanup/lane3-state.md`.

## The writer (FOUND): ABL `concat()` appending past the script variable's declared size
- `mclib/ablxstd_std.cpp` `execStdConcat`: the destination is a script `char[N]` variable (a heap block of exactly N bytes:
  locals and statics of an ABL routine come from `AblStackHeap` = `UserHeap::Malloc` = `gos_Malloc`); the append was an
  unbounded `strcat`. Trigger in mc2_15: `data/missions/corebrain.abx` (inside `mission.fst`) `order coreEscort` declares
  `char[50] debugS` and builds `"coreEscort: " + guardObjectID + " (" + escortPos[0] + "," + escortPos[1] + "," + escortPos[2] + ")"`
  (then `setDebugString`). `escortPos` held astronomically large floats (garbage: before the lane-1 zeroing commits ABL
  locals came back uncleared, and `getRelativePositionToObject` writes nothing when the object is not a mover), each real
  rendered by `{:.2f}` into concat's `char buffer[20]` = a 19-character cut ("-250014989304095043"), so the text is
  12 + 6 + 2 + 19 + 1 + 4 + 1 + 19 + 1 = 65 chars + NUL = 66 bytes into a 50-byte block: 16 bytes past it, through the next
  LFH block's 8-byte header into its first bytes. That is exactly the quarantine geometry (`...553810841,-199839715553810841)` at
  A+50 of a malloc(50) block, the "`839715553810841)` at offset 0 of a freed 56-byte node", the 19:53 list node whose `_Next`
  low bytes were `29 00` = `)\0`). Every neighbour seen so far is touched each frame (objmgr_update.cpp:712 map node, the
  static-store list node, CRT fopen temporaries), so the heap trips within the same frame as the write.
- Caught live at cdee50511 (the last crashing code; HEAD never reproduced in 8 runs because the zeroing commits 1b7cbefe6 /
  66cdc9ec3 / 2946a1f59 turn the garbage positions into "0.00" and the text then fits): crash-time scan at frame 1505
  (`strhunt.log` HIT #38): `coreEscort: 141696 (-250014989304095043,0.00,-250011275483977186)` NUL, a 50-byte-block tail at
  `0x1E81F00CBFD`. An earlier scan (frame 414) caught a 56-byte `gosAudio` block whose previous owner (the freed 50-byte
  debugS) had left `225598528307)` at +52 and the following list node's `_Next` = `0x29`.
- The content varies with the garbage, the SHAPE does not; the 10 quarantine dumps of 2026-10-11 all held the same v,-v pair.

## The fix: 76204ae18 (diag script: 1d17084c4)
- `mclib/abl_string_append.h` `abl::appendBounded(dest, cap, item)`: appends within the variable's declared size, keeps the
  NUL inside the block, returns the dropped count; `execStdConcat` takes the destination's `TypePtr` (`info.array.elementCount`)
  as the bound and logs `[ABL v1] event=concat_truncated module= line= capacity= dropped=` (16 lines max). concat's render
  buffer is 32 bytes (sane large reals are no longer cut at 19 characters).
- Doctest `tests/unit/test_abl_concat.cpp` (fits / overflow cut / unterminated / unknown size); harnesses+doctest green
  (2738 cases; the one "failed" assertion is the printf corpus's expected failure).
- NOT changed (for the orchestrator): `execStdGetModule` copies file names into script char arrays with `strcpy` (no size is
  known at that site); `execGetRelativePositionToObject` leaves the output untouched for non-movers (harmless since ABL locals
  are zeroed, but the script then reads a zero position); the scripts' `char[50]` debug strings are mod content and stay.

## Runs this session (mc2_15 tier2, lane3 install; lanes 1 and 2 ran games at the same time, 27-110 fps)
| # | exe | result | fps | frame | note |
|---|-----|--------|-----|-------|------|
| 1 | sandwich diag 257f39925 (2026-10-11 20:30) | PASS | ~110 | 120 s | no hit |
| 2-9 | HEAD 386925beb + hunter v1..v3 | 8 runs, no crash | 29-110 | up to 8137 | freeze verdicts = scanner cost / host noise |
| 10-11 | cdee50511 + hunter v3/v4 | PASS; CRASH 09:35 frame 1695 (fopen free) | 44 | | scanner 53 s/pass (pinned WC pages) |
| 12-14 | cdee50511 + v5 | no crash | 29-48 | | exact-content needle only |
| 15 | cdee50511 + v6 | CRASH 09:53 frame 3000 (objmgr node delete) | | | 15-digit matcher blinded by RTTI names; 9.3 GB full dump killed by the harness |
| 16 | cdee50511 + v7 | CRASH 10:03 frame 262 | | | shape matcher blinded by pixel rows |
| 17 | cdee50511 + v8 | PASS | 53 | 6313 | |
| 18 | cdee50511 + v9 | CRASH 10:09 frame 2264 (objmgr node alloc) | | | a 1 Hz scan cannot see a same-frame write |
| 19-21 | cdee50511 + v10/v11 | PASS, PASS, CRASH 10:16 frame 3580 (length_error reinject) | 47-60 | | the C++ exception path bypassed the VEH hook |
| 22-25 | cdee50511 + v11 | freeze, freeze, PASS, CRASH 10:27 frame 414 | 29-60 | | crash-time scan: gosAudio block, previous owner lost |
| 26 | cdee50511 + v12 | CRASH 10:35 frame 1505 | | | crash-time scan: `coreEscort: 141696 (...)` = the writer |
Unfixed cdee50511: 7 crashes in 19 runs (frames 262..3580), every one a 64-byte-bucket neighbour of a 50-byte block.

## Proof tables (normal exe, no diagnostic)
cdee50511 + fix (cherry-pick c7fdb0e23, detached, 10:55-11:25): 9 runs, 0 crashes.
| run | result | fps | frames | note |
|---|---|---|---|---|
| 1 | heartbeat_freeze_play (host) | 43 | 5148 | p1 3 fps, three lanes' games running |
| 2 | PASS | 55 | 6628 | |
| 3 | PASS | 34 | 4140 | |
| 4 | PASS | 57 | 6816 | |
| 5 | PASS | 56 | 6772 | |
| 6 | PASS | 40 | 4835 | log: 12 x `[ABL v1] event=concat_truncated module=data line=543..546 capacity=50 dropped=26/1/27/1` = the overflow attempted and clamped |
| 7 | PASS | 56 | 6718 | |
| 8 | PASS | 10 | 1254 | 54 s load (host), still clean |
| 9 | PASS | 41 | 4918 | 10 x concat_truncated |

HEAD 386925beb + fix (branch tip 1d17084c4, exe fingerprint 1d17084c4 clean), 11:27-12:16, normal exe:
| run | result | fps | frames | note |
|---|---|---|---|---|
| 1 | PASS | 47 | 5688 | |
| 2 | PASS | 54 | 6494 | |
| 3 | heartbeat_freeze_play (host) | 46 | 5556 | p1 15 fps while lanes 1/2 ran games; no crash, no bundle |
| 4-8 | PASS x5 | 66-76 | 7929, 8192 x4 | |
| 9-11 | PASS x3 | 68-76 | 8192 x3 | runs 4-11 = 8 consecutive clean passes |
tier1 (2026-10-12T11-53-18): mc2_01 PASS 133 fps, mc2_03 heartbeat_freeze_play (p1 5 fps, host), mc2_10 PASS, mc2_17 PASS,
mc2_24 PASS; mc2_03 rerun alone 12:12: PASS (40 fps) -> 5/5. No `concat_truncated` line in any HEAD run (the zeroing
commits keep the positions sane; the clamp is the safety net for the next garbage source or a longer script string).

## Hunter lessons (next memory-corruption hunt; script `lane3-diag/strhunt_patch.py`, apply|revert, never commit)
- Scan only RESIDENT ordinary pages (`QueryWorkingSetEx`: Valid, not Shared, not Locked, no NOCACHE/WRITECOMBINE): reading
  committed-untouched pages balloons the working set (6 GB, 6 fps) and driver-pinned write-combined pages read at 16 MB/s.
- A victim the heap touches every frame trips the heap in the same frame as the write: a periodic scan never sees it; scan AT
  CRASH TIME from `crashbundle_filter` (the 0xC0000374 vectored path AND the C++-exception path) while the bytes are still
  there, and keep an owner table (pointer -> alloc stack, free stack, PREVIOUS owner) for the block class.
- Shape matchers ("15 digits", "digits,-digits") match RTTI names, pixel rows and shader float tables; exact content while
  live, shape only at crash time where the real string is recognisable by its context.
- Full-memory dumps (9 GB) do not finish before the harness kills the process; private-RW dumps came out truncated too.
- Heredocs in this shell turn `\\n` into real newlines inside Python/C text: write edit scripts with the Write tool.
- The host lease races: lanes 1, 2 and 3 started games within 2 s of each other several times (fps halves, freeze verdicts).
- Scripts in `mission.fst` are LZSS-packed entries; `scratch fst_grep.py` (zlib/size==real) found corebrain.abx readable.

## Hygiene
- Nothing pushed. Diagnostics applied by script only and reverted with `git checkout --`. The detached cherry-pick c7fdb0e23
  (cdee50511 + fix) is unreferenced and can be dropped. build64-ninja = HEAD + fix. `mc2-ph.exe` (page-heap copy, IFEO key still set for that image name) deleted from the install. Install `crashes/`: bundles of
  2026-10-11/12 (evidence; the 9.3 GB dump of 09:53 was deleted) and `strhunt.log` (HIT #38 of the 10:35 scan = the writer).
