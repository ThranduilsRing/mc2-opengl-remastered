<!-- doc-status: current | reviewed:2026-10-13 -->
# Cleanup review: Codex (Sol) merged C++ (10ae810ea..d1b437e79), read-only

Scope: editor E2 (ObjectiveDlg split, EString to std::string), ABL F out-arrays, save S3 batch 1, merge resolutions.
Nothing was built or run; findings come from reading the merged tree. Counts: **blocker 0, should-fix 9, nit 9**.

## 1. Editor E2

- **should-fix** editor/Objective.cpp:393-398 (`CObjective::Read`): the new required `ObjectiveModel`/`ModelType`/`ModelScale` keys reject the WHOLE objective when a hand-authored or mod mission omits one; the old reader defaulted them. Fix: default missing keys (log once), reject only a present-but-malformed value, and run stock + mod missions through it before keeping the stricter rule.
- **should-fix** editor/Objective.cpp:410-433 (condition/failure-condition loops): a condition `Read()` failure used to `continue`; it now returns false from the whole objective with no log line of its own. Fix: log which block/species failed (reader rule: reject with a log line) and state in the commit that the behaviour change is intended.
- **should-fix** editor/Objective.cpp:238-258 (`operator==`): `Title()` and `Description()` are compared with `mc2::str::iequals`, so a case-only edit of display text compares equal and the modified check can miss it. Fix: exact `==` for display text and message strings (CDisplayTextMessage m_message in Objective_actions.cpp); keep `iequals` for flag IDs and file paths only (H2 is about identity keys).
- **should-fix** editor/AuthoredMissionState.h:55-57 (`objectiveTextLengths`): `std::string source(file.getLength(), '\0')` has no cap and ignores the `File::read` result; a huge mod .fit allocates it all and a short read leaves NULs in the scan. Fix: reject past a named byte limit and check the read count.
- **nit** editor/Objective_internal.h:1-12,36: banner still says "Objective.cpp: Implementation of the Objective component"; the comment "Globals defined in Objective.cpp ... the split pieces share" introduces nothing. Fix: one WHAT+WHY banner, drop the dead comment.
- **nit** editor/Objective_internal.h:37-100: file-`static` helpers (`LoadObjectiveString`, `sRead*`, `areCloseEnough`) in a header give each includer its own copy and unused-function warnings. Fix: `inline`, or move each into the one TU that uses it.
- **nit** editor/Objective_internal.h:9-30 pulls stdafx.h, six dialog headers and EditorInterface.h into every Objective_*.cpp; actions/conditions do not need the dialogs. Fix: include only what each TU uses.
- **nit** editor/ObjectiveDlg.cpp is still 1329 lines after the split (only the readable-text formatting moved to ObjectiveDlg_text.cpp). Fix: do not report the split as done on the step-4 board.
- **nit** editor/Objective.cpp:344-349: `Read` pre-seeds `text.activateFlagID = m_activateFlagID`, so a missing key keeps the object's previous value (the H15 quirk class). Fix: confirm this is the intended C9 behaviour and say so in a comment.
- Checked OK: no `EString` token remains in editor/*.cpp|h; the by-value accessor dangling class (H4) is gone (the `.c_str()` uses in the diff are on persistent members, not temporaries); legacy `const char*` enters strings through `mc2::str::from_c`; no function-local `static std::string` added; writers always emit the key (no omit-on-null).

## 2. ABL F out-array slice

- **should-fix** code/ablmc2_queries.cpp:250-252 (`execGetContactStatus`): `*contactTagged = 0;` is an unconditional write through `ABLi_popIntegerPtr()` with no null test. Same shape, guarded only by the optional `s_ablArgGuard`: ablmc2_queries.cpp:268-281 (range/angle) and ablmc2_roster.cpp:390-394. Fix: unconditional null test with `abl::reportBadArgument`, or convert to the `ABLi_pop*Buf` path.
- **should-fix** `ABLi_popIntBuf/popRealBuf` and `abl::clampArrayCount/copyIntArray/copyRealArray` are forward-declared by hand in 9 TUs (ablmc2_camera/misc/objects/orders/roster/state.cpp, test_abl_outarrays.cpp) and DEFINED in code/ablmc2_position.cpp. Fix: declare once in `abl_script_events.h` / `ablxstd_internal.h` and move the definitions beside `reportTruncated` in mclib/abl_script_events.cpp; a signature drift today links to the wrong thing silently.
- **should-fix** mclib/abl_script_events.cpp:20-24 (`takeLine`): a function-local `static std::map<std::string_view,int>` is a static-duration owner (project rule), its `string_view` keys assume literal lifetime, and the budget cannot be reset, so doctests cannot assert a second log line. Fix: a fixed table keyed by an enum, with a test-only reset.
- **should-fix** mclib/ablrtn.cpp:1530-1533: `functionHeader` now makes an array return type a syntax error, a language change for any stock or mod script that declares one. Fix: grep stock ABL and the mod installs for array-returning functions and record the result in the commit; if any exist keep the copy path, bounded as ablxstd_std.cpp:97-106 already does.
- **nit** code/ablmc2_misc.cpp:461-467 (`execGetNextPilotEvent`): `getNextEventHistory(params)` already fills `params`, then `getEventHistory(eventID, params)` fills it again just to get the count. Fix: add one count-returning call; static_assert the local array equals the `alarmHistory[].trigger` extent (MAX_ALARM_TRIGGERS).
- **nit** code/ablmc2_orders.cpp:32-35 (`execGetObjects`): `numObjects` (`long`) is cast to `size_t` per append. Fix: use one `size_t` counter and clamp once at the push.
- **nit** READ-only arrays still use `ABLi_popRealPtr` with `s_ablArgGuard` (ablmc2_camera.cpp:269-294,350-375,468,753-794; ablmc2_objects.cpp:175; ablmc2_state.cpp:112,206,682; ablmc2_orders.cpp:153,389). Fix: list them in the slice notes as the remaining unbounded reads.
- Checked OK: every converted writer stages into a local array and copies once through the clamping helper (no change for in-range writes); `ABLi_pop*Buf` returns an empty span for a non-array, wrong element type or zero extent; truncation and bad arguments log; `execStdReturn` zero-fills the `StackItem` before the bounded copy and logs `return_truncated`; `execConvertCoords` and `execGetSalvage` bound both arrays.

## 3. Save S3 batch 1 (code/save_archive.*)

- **should-fix** code/save_archive.cpp:236-244 (`writeFile`): the staging name is a fixed `<path>.pending` opened `O_EXCL`; one crash leaves it behind and EVERY later save fails with `file_create`. Fix: unique staging suffix, or remove a stale `.pending` first, and say so in the design doc.
- **nit** code/save_archive.h:1 and code/save_archive.cpp:1: no file-level WHAT+WHY comment. Fix: two lines pointing at the design doc and "not wired yet".
- **nit** code/save_archive.h:168-179: `Text::operator==` ignores `limit`, and `BoundedText` sets `limit` only inside `serialize`. Fix: add a test that an over-long `Part::profileName` is rejected through `Part`, not only through `Text`.
- **nit** code/save_archive.cpp:8-12: `#define NOMINMAX` unguarded warns if the build already defines it. Fix: `#ifndef`.
- **nit** code/save_archive.cpp:236-262: no directory flush after `MoveFileExW`/`rename` on the POSIX branch. Fix: note it as accepted for the Windows-only game.
- Checked OK: magic, schema, release line, build sha, fingerprints, payload length and hashes are rejected with a `[SAVE v1] reject reason=` line; allocation and wire budgets are separate; sequence, text and nesting caps; bad_alloc/length_error caught in `operator()`, `encode`, `decode`, `readFile`; trailing bytes rejected; floats finite both directions; `Part::validate` rejects `..`, drive colons and absolute asset keys; no static-duration owners; nothing in game code references the codec yet.

## 4. Merge resolutions

- **should-fix** tests/unit/CMakeLists.txt:306-352: the 16 former harness files compile into `mc2_tests` with hard-coded `${MC2_WORKTREE_ROOT}/3rdparty/lib/x64/SDL2.lib|glew32.lib`, two DLL copy steps, and one source still in a removed tools dir (`tools/render_state_contract_harness/gos_counter_stubs.cpp`). Fix: move the stub into tests/unit/ and use the repo's SDL2/GLEW targets instead of absolute lib paths.
- **nit** tests/unit/CMakeLists.txt:350-352: `save_archive.cpp` and `test_abl_outarrays.cpp` are bare `target_sources` lines after the DLL loop with no comment. Fix: group them with a one-line header next to the other source lists.
- **nit** `class_light_band_harness`: no CMake, script or CI reference remains; the only tracked non-doc mention is scripts/unregistered-harness-debt.txt:14. Other stale paths to deleted harness dirs: scripts/raw-gl-state.allowlist.json:27,58,80,104 (`tools/glstate_guard_harness/`), scripts/check-terrain-daylight-constants.py:6, scripts/check-cxx-standard-floor.py:8. Fix: drop or retarget these rows and comments.
- Ignorable: untracked build-merge-tests/ and build64-h-class_light_band_harness/ still name the old files.

## Efficiency retrospective
- Two shell calls timed out at 120 s because a recursive `grep -r` ran from the repo root across large build dirs; `git grep` should be the first choice.
- Reading the new helpers in full (save_archive.*, abl_script_events.cpp, AuthoredMissionState.h) plus sampled diffs of converted sites covered the risk surface without reading all 97 files.
- A small grep checklist (hand forward declarations, `static` in headers, `static std::` owners, unguarded `*ptr =` after `ABLi_pop*Ptr`) would have found half of these mechanically.
