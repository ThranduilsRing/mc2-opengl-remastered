# Lane 1 step 4 B1 cleanup review (B1.0 harness, B1.1 objectives, B1.2 texture cache), range afa99eaf6..2b4e2f3f1

Reviewer: read-only Sonnet cleanup pass, 2026-10-12, worktree A:/Games/mc2-main. Method: `git diff` of every C++ and Python hunk, then re-runs that need no build:
`py -3 tests/analyze/analyze_dangling.py` (OK, 10.7 s; four shapes caught by clang-cl only, clean twin silent), the same with `--check` over objective*.cpp, missionbriefingscreen.cpp,
controlgui_objectives.cpp, gui/aedit.cpp with `build64-ninja/compile_commands.json` (0 diagnostics), `log_tag_census.py --check` OK, `own_inventory.py --check` OK,
and `tools/save_inspect.py` over the lane's own saves in `/tmp/b1save` (they still exist). No build, no launch.

## VERDICT: ACCEPT-WITH-FIX
The conversion is sound and follows C1-C14 / R1-R12 where it matters. One finding is substantive: the save-proof explanation for `TeamData` is wrong, and it points at a real
latent defect in a save writer (F1). The rest are small clean-ups. Nothing needs a revert.

## Findings (by severity)
**F1 (medium). The "TeamData differs in the old exe too" claim is false; the cause is an uninitialised array in `Team::Save`.**
Notes section 8 (B1.1 paragraph) and `lane1-state.md:43` say MechWarriorData, CommanderData, TeamData and TerrainObjectData "already differ run to run in the old exe".
Measured with `save_inspect` and a byte scan of `/tmp/b1save`: old_a vs old_b = **0** TeamData bytes differ (the old noise set is exactly TerrainObjectData 191, MechWarriorData 41,
MechData 6, CommanderData 2, text 1, as stated). new_a vs new_b = 2 TeamData packets differ, new_b vs new_c = 2, old_a vs new_a = 1. In the differing runs, `TeamData` packets 2027/2028
carry ASCII text (`1869182057, 1025529966, ...` = freed FIT/objective string bytes) in `roster[]` past `rosterSize` (offsets 112..487 of 488); new_c and both old runs have zeros there.
Cause: `code/team.cpp:501` `memcpy(data.roster, teams[i]->roster, sizeof(GameObjectWatchID) * MAX_MOVERS_PER_TEAM)` copies the whole array, `Team::init` (team.cpp:41) sets only
`rosterSize = 0`, so the unused tail is whatever the heap held. `MC2_SAVE_STRUCT` zeroes the destination but the memcpy overwrites it. B1.1 did not change any roster value; it changed
which freed blocks a later `Team` allocation reuses. This is a pre-existing writer that is not a defined format (owner ruling), and it will pollute every B1.3/B1.4 save diff.
Fix: copy `rosterSize` entries only (clamp to `MAX_MOVERS_PER_TEAM`), one line in `Team::Save`; correct both docs to say "TeamData tail garbage, old exe zero by luck". Re-take new_a/new_b to confirm TeamData is then equal.

**F2 (low-medium). `gui/alistbox.h:23-26`: `#include <string>` sits inside `#ifndef ESTRING_H`.** If `estring.h` was included first, `<string>` is only there by luck of `gameos.hpp`
(`estring.h` itself includes just `stdlib.h` and `gameos.hpp`). `aTextListItem::setText(const std::string&)` at line 75 needs it unconditionally. Move the include outside the guard.

**F3 (low). Stale includes in converted `code/objective.cpp`.** Line 14 `#include"estring.h"` (the file has no EString left; the review's "no stale EString references" check) and
line 55 `#include "mc2str.h"` (nothing in the file uses `mc2::str`; `iequals` lives in `objective_flags.cpp`). Remove both.

**F4 (low). C2 not followed in the bridge.** `gui/aedit.cpp:437-441` `aEdit::entry()` open-codes `p ? std::string(p) : std::string()`; the rule is that every C-string boundary goes through
`mc2::str::from_c`. Behaviour is correct (null-safe); include `mc2str.h` and call `from_c(text)`. `from_c` currently has no production caller at all.

**F5 (low). `mc2str.h` surface.** Production code uses `iequals` only (`code/objective_flags.cpp:18`). `ci_equal` has no consumer anywhere and is a trap alone (an unordered container needs a
matching case-folding hash, which the header does not offer): delete it, add it back with its hash when a container needs it. `ci_less`, `ci_total_less`, `ascii_lower/upper`, `from_c` are used by
B1.3 plans and tests; keep. `icompare` is the shared core, keep.

**F6 (low). `tests/analyze/analyze_dangling.py` carries a dead analyzer and a weak shape check.** `run_cl`, `RULESET`, `CL_CODES` run `cl /analyze` on both files although the script's own
docstring records that it catches none of the shapes (about half of the 10.7 s, plus an `EspXEngine.dll` dependency); drop it or demote it to `--verbose` information. `self_test` rejects only
`len(seed_shapes) < 3` while the seed has four tags (A, B, B2, C): require exactly that set so a deleted tag cannot pass. `--check` over a file that is missing from `compile_commands.json` runs without its include paths, fails to parse and yields "0 diagnostics" with exit 0 (a "did not parse" line is printed): make that exit 2.

**F7 (low). R2's "the board checks that the count only goes down" is not implemented.** `estring_callsites.py --drop FILE` deletes every row of a file on the caller's word; nothing verifies the
file is converted. Cheap mechanical check: `--check` re-reads each remaining row's `file:line` and fails when the line no longer contains the kind's token (`.Data()`, `Format(`, `Compare(`...) or the
file is gone, plus a row-count floor in the file header. Rows for `gui/aedit.cpp` and `gui/asystem.cpp` are shifted by the bridge (the list predates 3a851f497); B6 deletes them, so only note it.

**F8 (low). Reader accepts a truncated identifier.** `code/objective_text.cpp:35-41`: a value over 2000 bytes is cut and logged, but the cut text is returned. For a flag ID, file name or
species string that silently changes identity (the owner ruling is "readers reject malformed content"). Return `std::nullopt` plus the log line for the identifier keys, keep the cut only for Title /
Description if wanted; `test_objective_text.cpp` "a value longer than the reader's buffer" then pins the new rule.

**F9 (low). Duplicated rule.** The "FlagID, else decimal FlagIndex" fallback is written three times: `CBooleanFlagIsSet::Read`, `CSetBooleanFlag::Read` (`code/objective.cpp` ~985 and ~1095) and
`objective_text::readFields` (ActivateFlagID). One `objective_text::readFlagID(file, idKey, indexKey)` replaces them.

**F10 (low). Comments narrate history.** `objective_text.h:5-8` ("The old readers left...", "is now an empty optional"), `objective_flags.h:8` ("the old table compared through..."), several test
comments. The WHY is right; drop "old/now/used to" wording so the comment survives the next batch. `objective_text.cpp:37` explains the buffer terminator mechanics (HOW): keep one clause.

**F11 (info). Corpus and pins are weaker than their wording.** (a) `stock_names.txt` is the v0.4 install's file names including dev items (`_tga_slim_backup`, `a_*.ktx2`), file names only
(no pilot/mech/variant names), zero bytes >= 0x80; "agrees on 100 % of equal pairs" means adjacent pairs in the folded order plus the case variants. (b) H12 MakeUpper-on-0xE9 is asserted only when
`GetACP() == 1252` (`test_estring.cpp`, H12 case): on another host it passes without pinning. (c) `orderDisagree < 100` against a measured 20: tighten to 20 so a changed fold is noticed. None blocks.

**F12 (info). `tests/callers.json` at HEAD has one stale line (row at json line 2408, 872 vs 873) from the later lane 2 merges, so `script_callers` is red at HEAD until regenerated.**
At 2b4e2f3f1 the diff is the expected +1 line shift after the new check-contracts row plus the two `analyze_dangling.py` import rows (consistent).

## Confirmed correct (verified in code, `git show` or by running)
- No case-insensitive comparison became case-sensitive: the only comparison site in the range is the flag table, `iequals` (`objective_flags.cpp:18`); no `Compare`/`==` on an EString was touched elsewhere.
- Title fallback is explicit in `objective_text::readFields` (Description absent -> Title, an empty Description stays empty, tested); ObjectiveModel absent -> empty; ResetStatusFlagID/ActivateFlagID keep rules written in the header.
- Objective writer always writes the key (`CObjective::Save` ObjectiveModel unconditional, `objective_text::write`), no `writeIdString(nullptr)`; model fields initialised.
- Accessors return `const std::string&` (Title, Description, ActivateFlagID, ResetStatusFlagID, ModelName); Localized* return by value and every `.c_str()` of one stays in its full expression (controlgui_objectives 349/870/891, missionbriefing 601). `objectiveModels[count] = ...c_str()` copies into an EString. No `EString::view()`, no `setText(string_view)`, `estring.h` untouched, no static-duration owner added (the one removed is the TGA cache).
- Bridge: `aText`/`aTextListItem` take `const char*` + `const std::string&` next to the EString form, `aEdit::entry()` returns `std::string`; the compile test covers literal, `const char*`, `std::string`, EString, `setText(0L)`.
- Old-class pins H1 (never assigned null, `= ""` non-null, `Empty()` null, `Assign(nullptr)` null), H2/H3, H12, H13, H14 (COW pointer outlives source), H4 are real assertions on the old class.
- 46 deleted virtuals: no caller outside the definitions (`git grep` of `InstanceDescription`, `DescriptionOf*Species`, `->Description()` over code/gui/mclib is empty; the editor keeps its own copy). Behaviour change "lone trailing character kept" is real (old `sReplace` appended only when two or more characters remained) and tested.
- `/we4840`: applied to mc2, mclib, gui, EditRel (`CMakeLists.txt:859-868`, MSVC and CXX only); `GameOS/gameos/vulkan_context.cpp:109` really passes `const std::string env` to the printf-style `ctxlog` (garbage on an unrecognised preset), which is why gameos is excluded; the `(LPCTSTR)` cast at `EditorData_terrain.cpp:100` is the MFC idiom. Targets not in the list (mlr, gosfx, stuff, tools) are unprotected: say so in the notes.
- Ratchet baseline rewrites each carry a reason: LG06 +1 `from_c` (db9b1d4fa message), LG06 +1 net in code (e5600b260: four key parameters reach the C reader/writer as `const char*`, three static helpers went); LG03 -4, LG05 -1, LG09 -7 match the removed `delete tmpCondition/tmpAction`, the texture-cache `new/delete` and the four `(const int)` casts. The `ablmc2_*` LG09 lines that moved with db9b1d4fa are earlier net decreases (stated).
- own_inventory: the removed `EString` entry (new 1 in tgainfo.cpp, delete 1 in mechcmd2_frame.cpp) and `code/objective.cpp` 10 -> 8 (the two deleted `delete` in `DescriptionOf*Species`) match the code; `--check` OK at HEAD.
- Call-site list: 498 data rows in 71 files, per-kind counts equal the header, no rows remain for the converted files; json batch counts (2+2+6+14+14+10+8 = 56, +11 B6 = 67) add up.
- Save proof otherwise verified from the saves: old vs old text packet differs in exactly the four uninitialised model fields; old vs new differs by two added `st ObjectiveModel = ""` lines; new vs new text packets identical; MechData 134 / GroundVehicleData 151 differ only at bytes 2953-2955 and 3001-3003 of every record and are stable within one exe.
- Log tag census OK; the two new `MC2_LOG(Io, Warn, ...)` lines are within the facade. Gate board row `analyze_dangling` and the `background-launch-allow.json` entry are in place.

## B1.3 must do first (in this order)
1. Fix `Team::Save` (F1), re-take two saves with the new exe, confirm TeamData is equal, and correct the two doc sentences. Without it the B1.3/B1.4 save diffs cannot reach "noise set only".
2. F2/F3/F4 (five-line edits), F5 (delete `ci_equal`), F8/F9 if touching `objective_text` again; F6/F7 are tool tidy-ups for the same commit as the first `--drop`.
3. Then B1.3 as planned: `logisticsdata_availability.cpp:245` must become `mc2::str::iequals` (the probe's one stock-data fold site, pilots `PMWChopper` etc.); sort sites to `ci_total_less` with the order in a comment; `getCurrent*` null-tests per C7; run `analyze_dangling.py --check` on every edited TU.
