<!-- doc-status: current | reviewed:2026-10-12 -->
# ABL string-safety audit (2026-10-12, lane 1)

WHAT: which ABL builtins write text into a script `char[N]` variable, how each is now bounded, and what the stock scripts do to those
variables (runtime and static). WHY: the mc2_15 heap corruption (76204ae18) was a builtin appending past a script string's declared size; a
script variable is a heap block of exactly N bytes, so any unbounded write is a heap overwrite. Readers reject or clamp malformed content with a
log line, never crash (owner ruling 2026-10-11 #5).

## 1. Builtins that write script arrays

| Builtin | Writes | Bound before | Bound now |
|---|---|---|---|
| `concat(dest, item)` (`execStdConcat`) | text, append | unbounded `strcat` (76204ae18 bounded it by the declared element count) | `abl::appendBounded(std::span<char>, std::string_view)`, `ABLi_popCharBuf` supplies the span, `[ABL v1] event=concat_truncated` |
| `getModule(curBuf, fsmBuf)` (`execStdGetModule`) | module and FSM file names | two unbounded `strcpy` (no size known at the call) | `ABLi_popCharBuf` (declared element count) + `abl::copyBounded`, `[ABL v1] event=getmodule_truncated module= line= capacity= dropped=`, 16 lines max per routine |
| `getRelativePositionToObject(..., real[3])` | 3 reals | wrote nothing for a non-mover or unknown id: the script kept its stale value | writes the zero vector (documented at the site; `abl_out_params.h`) |

Every other `ABLi_popCharPtr` user only READS the array: `print`, `fatal`, `assert`, `fileopen`, `filewrite`, `getstatehandle`, `getfunctionhandle`, `setDebugString`
(copies with `strncpy` into a 64-byte pilot member, terminated by the member's own size), `playVideo`, the roster loaders (`ablmc2_roster.cpp`).
`grep` for `strcpy|strcat|sprintf|strncpy|memcpy` in `mclib/ablxstd*.cpp`, `code/ablmc2*.cpp`: the only writes into script memory were the two above;
`code/ablmc2_state.cpp:198-200` fills a C++ struct (`moverData.pilotFileName`), not a script array. The rest of the hits in `mclib/abl*.cpp`
(`abldbug*`, `ablenv`, `ablexec`, `abldecl`, `ablrtn`) are the compiler/debugger/loader and write their own buffers, not script variables.

### Open class (not fixed here): integer / real script arrays written by queries
`ABLi_popIntegerPtr` / `ABLi_popRealPtr` return a bare pointer; the builtin writes however many elements it has. Fixed-size outputs match the documented
sizes (`getObjectPosition` real[3], `getFireRanges` real[4], `getWeaponRanges` real[3], `statusCount` int[9]); the VARIABLE-length ones depend on engine
state and trust the script's declaration: `getContacts` (`ablmc2_queries.cpp:116`, up to the number of contacts), `getWeaponsReady/Locked/InRange`
(`:336,354,372`), `getAttackers` (`:548`), `getAlarmTriggers` (`:476`, `getEventHistory`); `getContactStatus` (`:243`) and `getContactRelativePosition` (`:261`) write single values through int/real pointers. The same cure applies
(`ABLi_popIntBuf` / `ABLi_popRealBuf` returning a span, the builtin clamps its count to `span.size()` and logs `event=<name>_truncated`); it was not done
in this pass because every site needs its own count rule and a stock-script check of the declared sizes. Queue it as its own slice.

## 2. Runtime audit: `concat_truncated` / `getmodule_truncated` lines at HEAD 180f6f4f9

tier2, exe 180f6f4f9968 (`run_smoke.py --tier tier2 --mission X --deploy 0.4 --keep-logs`), logs in `tests/smoke/artifacts/`:

| Mission | Report | Verdict | Frames | `event=*_truncated` lines |
|---|---|---|---|---|
| mc2_01 | 2026-10-12T16-46-44 | PASS | 8192 | 0 |
| mc2_15 | 2026-10-12T16-44-31 | PASS | 6849 | 0 |
| mc2_17 | 2026-10-12T16-48-57 | PASS | 8192 | 0 |
| mc2_07 (the `char[25]` brains below) | 2026-10-12T16-39-28, 2026-10-12T16-52-09 | heartbeat_freeze_play both times (3636-5908 frames, p1 2-3 fps while the host sat at 100 % CPU: host load, not a script finding) | partial | 0 |

An earlier mc2_15 run of the same exe (2026-10-12T16-29-20, freeze verdict, host load) also has no such line. Zero is the expected result: lane 3 saw the
mc2_15 overflow only on cdee50511 (before the ABL locals were zeroed and before `getRelativePositionToObject` had a defined output); the clamp is the safety
net for the next garbage source, and it is covered by doctests (`tests/unit/test_abl_concat.cpp`: fits, overflow cut, unterminated, unknown size, replace-copy,
one-byte variable).

## 3. Static audit of the stock scripts (994 `.abl/.abx` members of `mission.fst` + `data/warriors/*.abl`)

Method: `concat()` chains per destination variable (literal lengths exact; numeric parts nominal 6 characters for an integer id/count, 8 for a real printed
`{:.2f}`; worst case 11 and 12), compared with the declared `char[N]`. 219 `concat` calls in 76 files, 121 chains of two or more parts, no stock script
calls `getModule`. 113 chains fit with margin (nominal and worst case below N-1): `dstring` char[40] x65, char[60] x5, char[90] x3, `s` char[50] x20 and
char[256] x2, `debugS` char[120] x2. The 8 tight ones, all debug strings (they feed `setDebugString` and are never read by game logic):

| Script | Line | Variable | N | What it should hold | Nominal / worst | Verdict |
|---|---|---|---|---|---|---|
| `corebrain.abx` | 538 (order `coreEscort`) | `debugS` | 50 | `coreEscort: <id> (<x>,<y>,<z>)` | 47 / 59 | the mc2_15 string: fits for sane positions, 66 bytes with the garbage reals that made it overflow. Content fix: `char[80]`, or drop the z |
| `corebrain.abx` | 302 | `debugS` | 50 | `cs=<step>, d=<dir>, cyc=<cycle>/<count>` | 27 / 58 | fits (small counters); worst case needs 11-digit counters |
| `warriors/hal.abl` | 181 | `debugS` | 25 | same patrol string | 22 / 58 | tight: fits while step + direction + cycle + count together take at most 10 characters; 3-digit step or two-digit negative direction cuts it |
| `warriors/mc2_07_enfield1.abl`, `_hunchback1`, `_lrm1`, `_vedette1`, `_vedette2` | 183 | `debugS` | 25 | same patrol string | 22 / 58 | same as hal.abl (mc2_07 patrol brains) |

The other `char[50]` / `char[25]` debug strings in the sweep are the single `"cs="`-style or `"<name>: " + id` forms and fit. Content bugs for a later DATA pass (do not
edit `.abl` sources from this lane): enlarge the patrol `debugS` to `char[50]` in `hal.abl` and the five `mc2_07_*` brains, and `corebrain.abx:538` to `char[80]`.
The engine now clamps all of these instead of corrupting the heap, so the symptom of leaving them is a cut debug label.

Reproduction of the static scan: extract the members with the `.fst` layout in `tools/mod_install/fst_listing.py` (stored or zlib members), then regex the
`concat(` chains per variable as above (the scratch scripts were not committed: the numbers above are the deliverable).
