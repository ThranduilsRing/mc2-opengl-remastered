<!-- doc-status: current | reviewed:2026-10-12 -->
# Cleanup review: lane 1, 2026-10-12 afternoon, range 642019a64..b12d894f9

WHAT: read-only review of the 6 commits (c8f6fa3df, 180f6f4f9, f4ab40f30, 7dbcb45f0, 2a1007f56, b12d894f9). WHY: the range changes how
engine code writes into ABL script memory. Method: every C++/Python hunk read; claims checked against code, `git show`, a throwaway
`clang++ -std=c++23` build of `abl_string_append.h` (nothing committed) and fresh runs of the ratchet, log-tag census, coverage selftest,
`callers.py --check`, `check-background-launch.py`.

## Verdict: ACCEPT-WITH-FIX
No overflow, underflow or NUL bug in the new bounded writers. One regression in a hardening path (F1), proof gaps (F3), an overstated
audit claim (F2), and B1-notes numbers that do not reconcile (F6). None needs a revert.

## Findings (by severity)

F1 MEDIUM, new null write on a bad script. `code/ablmc2_misc.cpp:96-98`. The `else` branch added for non-movers writes `relPos[0..2]`
unconditionally. The only null guard is `s_ablArgGuard && !relPos` (:84), and `MC2_ABL_ARG_GUARD` defaults OFF (`Gates/gate_table.cpp:24`).
Before this range a non-mover call with a null/wrong-type `real[3]` returned silently; now it faults, which breaks "reject with a log
line, never crash". Fix: make the null test unconditional for this builtin (`if (!relPos) { abl_arg_guard_log(...); return; }`, other
builtins keep the gate) so both branches are covered.

F2 LOW-MEDIUM, audit scope hides executor-level writers. `abl-string-audit.md` section 1 says the only writes into script memory were the
two it fixed, but its grep covered only `mclib/ablxstd*.cpp` and `code/ablmc2*.cpp`. Not in scope and not mentioned:
 - `mclib/ablxstmt.cpp:268-280` string assignment: `memcpy(dest, src, min(strlen+1, size))` leaves the target UNTERMINATED when the source
   outgrows it (bounded, but `print`/`fileopen`/`DEBUGWINS_print` readers later `strlen` it).
 - `mclib/ablxstmt.cpp:458-470` and `mclib/ablrtn.cpp:936-948` by-value array parameter: `memcpy(dest, src, formalType->size)`; the compile
   check `isAssignTypeCompatible` (`mclib/ablexpr.cpp:148`) allows formal count >= actual count, so a char[25] actual into a char[50]
   formal reads 25 bytes past the actual block (read overrun, an ASan finding, not a write).
Fix: add an "executor writes" section to the audit with these three sites and a verdict each (bounded / terminate-on-truncate / read
overrun); queue them with the int/real out-array slice. Do not say "only" until `mclib/ablx*.cpp` and `ablrtn.cpp` are in the grep.

F3 LOW, doctest gaps for the edge cases this slice is about. `tests/unit/test_abl_concat.cpp` has no case for: exact fit (`len + item ==
cap - 1` must drop 0, one more must drop 1), `appendBounded` on a span of size 1 (only `copyBounded` has the one-byte case, :59-80), an
empty item, and `concat(s, s)` aliasing. I checked all four with a throwaway build and they behave (exact fit: 0 dropped, byte after the
span untouched; size 1: dest becomes "", dropped = item size; alias: "abc" -> "abcabc", tight cap cuts at "abca", 2 dropped), so this is
test debt, not a bug. Also untested: `ABLi_popCharBuf` itself (`mclib/ablxstd_stack.cpp:88-99`: count 0, count -1 open array, non-char
array, null address) and the relpos non-mover branch; `:83-96` tests `writeZeroVec3`, a 3-assignment helper, not the builtin. No script
exercises `*_truncated` end to end (the tier2 audit saw zero lines by design). Fix: add the four append cases; extract the accept test of
`ABLi_popCharBuf` into a pure function over (address, form, element-is-char, count) and doctest it; a tier2 mission with a seeded
`char[8]` plus a long `concat` and a `getModule` into `char[4]` would close the loop (data change: ask the owner).

F4 LOW, simplification.
 - `abl_string_append.h:22-32`: `appendBounded` repeats `copyBounded`'s copy + NUL + dropped logic. After clamping `len` to `size-1` the
   body is `return copyBounded(dest.subspan(len), item);` (same results on the cases above).
 - `abl_out_params.h` (21 lines, one production caller, `writeVec3` wraps three assignments): drop it and write
   `std::ranges::fill(std::span<float, 3>{relPos, 3}, 0.0f)` or the plain assignments at the site. It is also inconsistent with the
   sibling `execGetRelativePositionToPoint` (`ablmc2_misc.cpp:55-60`), which still writes raw. Rule: no wrappers around quirks.
 - `ablxstd_std.cpp:355`: `CurFSM ? abl::textOf(CurFSM->getFileName()) : std::string_view{"none"}` is `abl::textOf(CurFSM ? CurFSM->getFileName() : "none")`.
 - `ablxstd_stack.cpp:92`: `static_cast<char*>(tos->address)` on a `char*` is a no-op cast.
 - `reportTruncated(event, int& lines, ...)` with a function-local `static int` at each call site (:247, :352) works; a per-event counter
   inside the function would drop the out-parameter.

F5 LOW, comments and greppability.
 - `abl_out_params.h:4` calls the zero vector "the documented no-position answer"; nothing but this header documents it (grep of docs/ and
   the ABL headers). Say "chosen". Design note for the Opus: (0,0,0) is a valid world position, so a script that feeds the result to a
   move order now walks to the origin where it kept a stale point before; only the 4 determinism missions and mc2_15 were run.
 - `event={}_truncated` (`ablxstd_std.cpp:206`): `git grep concat_truncated` finds only docs, and the census
   (`tests/unit/log_tag_baseline.json`, keys event/module/line/capacity/dropped) cannot see the value. Pass the whole literal
   (`"concat_truncated"`) and print `event={}`: same tag, keys and stream (census stays green) and the line is greppable.
 - All other new comments are WHAT+WHY, none narrates HOW, none stale. `abl.h:142-143` states the heap-block invariant, which holds: VAR
   array params need the identical type pointer (`ablrtn.cpp:1720-1722`), by-value copies are allocated at the formal size.

F6 LOW, `cxx23-step4-b1-notes.md` internal consistency (design is the Opus reviewer's).
 - "67 files in 8 batches" (end of section 3) and lane1-state "8 batches": B1.0..B1.8 is nine rows; the 67 files sit in B1.1..B1.7 (seven).
   The rows sum to 68, not 67: B1.1 lists `code/mechcmd2_frame.cpp`, which has no `EString` token at 642019a64 (it only `delete`s the
   pointer, :302). The 67 are 48 code + 18 editor + `tgainfo.cpp`; I matched every one to a batch (none missing, none duplicated).
 - B1.4 says 14 files, the list names 15. Section 7 Q1 says "the 15 front-end/data-model batches": no such count exists.
 - B1.3 "2 + 6 includers": `git grep` finds 6 includers of `objective.h` including `objective.cpp` itself; the named list has the other 5.
 - H11 "no call site outside estring.cpp (grep)" is false: `tests/unit/alloc_probe.cpp:89` calls `CreateMBCS()` (B1.8 migrates it; say so).
   "175 occurrences" in objective.{h,cpp} counts 179 (71 + 108). The three `tools/*.py` files that mention EString (`own_inventory.py`,
   `tidy/printf_buffers.py`, its test) are strings, not consumers: say so in section 2.
 - lane1-state step 5 says "2750 passed / 1 skipped", the 180f6f4f9 message "2750/2750": pick one.

## Confirmed correct (verified, not taken from messages)
- `appendBounded`/`copyBounded`: NUL always inside the span; no unsigned underflow (`dest.empty()` guard precedes `size()-1`); unterminated
  dest clamps to `size-1`; capacity 1 stores only the NUL; empty span stores nothing and reports the full length; null item is empty text.
- `ABLi_popCharBuf` accepts only `FRM_ARRAY` of `CharTypePtr` with `elementCount > 0`; returns `{}` for count 0, open arrays (-1), rows of
  other element types, non-arrays and a null address; `char` is 1 byte (`ablsymt.cpp:698`). `concat` into an empty span logs `capacity=0`
  rather than staying silent. getModule keeps the original pop order (cur, then fsm).
- Log tag census: `tools/log_tag_census.py --check` OK (1391 emitters); one stderr ABL line, same keys.
- Ratchet: `cxx17_ratchet.py --check` at HEAD reports 0 fewer legacy sites than baseline, so the baseline is exactly current: LG09 2190 ->
  2188, `ablxstd_std.cpp` 27 -> 25, nothing raised (the baseline diff is those two numbers plus the `head` sha); LG06 stays 670 on purpose.
- `check-contract-coverage.py --selftest` and the live scan green; `tests/callers.json` current (`callers.py --check`: 554 scripts); the
  exclusion is a one-file special case with a WHAT+WHY comment. `strhunt_patch.py:337-340` uses `console_child.child_kwargs()`;
  `check-background-launch.py` OK.
- Deploy hygiene: `build64-ninja/mc2.exe` carries fingerprint `7dbcb45f0c49` (contains 180f6f4f9); later commits are docs and data only.
- B1 numbers grep confirms at 642019a64: code 48, editor 18, gui 7, mclib 3, tests 2; fan-outs logisticsdata.h 33, EditorData.h 22,
  Objective.h 14, logisticspilot.h 11, logisticsmech.h 10, logisticsdialog.h 10, missionbriefingscreen.h 7, logisticsvariant.h 7,
  objective.h 6; `Compare(` 35; MakeUpper/Lower 27 (mechlopedia 9); Description overrides 46 (31+15); `objective.cpp:2549`,
  `tgainfo.cpp:223,332`, `estring.h:39`, `estring.cpp:711`.

## Next lane 1 session, first
1. F1: unconditional `relPos` null guard in `execGetRelativePositionToObject`; build, doctest, commit by pathspec.
2. F3: the four `appendBounded` boundary doctests and the `ABLi_popCharBuf` predicate extraction with its test.
3. F4/F5: fold `appendBounded` onto `copyBounded`, drop `abl_out_params.h`, literal event names; re-tighten the ratchet if a count drops.
4. F2 audit addendum (executor writers) and F6 corrections in the B1 notes, before the Opus before-review reads them.
5. Then the queue already in lane1-state: Opus review of the B1 notes, the int/real out-array slice, mc2_07 tier2 on an idle host.
