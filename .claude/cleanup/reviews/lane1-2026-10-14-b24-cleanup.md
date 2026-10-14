# Lane 1 2026-10-14 B2.4 cleanup review (read-only; commits f539b7f6f..3c3d21e6a)

Verdict: 0 blockers, 4 should-fix, 7 nits. The rule commit matches notes section 11; the open items are recorded honestly.

## Checked and clean
- sort_scored (mclib/score_sort.h:39-52): key is_nan, directed score, fill slot; NaN last in both directions, -0 == +0, inf == inf. Strict weak order (two NaNs fall through to fillSlot). The fillSlot tie-break makes std::sort deterministic, so it equals a stable sort. No allocation (in-place std::sort over the caller's span).
- SS1-SS7 cover direction, ties both ways, NaN, -0/inf, id travel and preset fillSlot, empty/single, and 200 random lists up to n=255 against std::stable_sort. test_score_sort_ties.cpp pins 248 of 408 CRT tie rows.
- SortList removal: all five former sites sort locals (255 / 255 / 32 / 5 / 255). Mover, Team and SensorSystem statics have no readers left: `git grep sortlist` at 3c3d21e6a finds no code, CMake or include reference outside generated data, docs and tooling (see S2, N4). mclib/CMakeLists.txt, mclib.h and tests/unit/CMakeLists.txt are consistent. The new header is not in mclib.h (D11), and none of the three edited TUs is on pch_skip.cmake, so D16 did not apply.
- 153b23b2d: the diff is the `sha` line of the four SIMID baselines and nothing else.
- BUGS 21/22 text updated to "ENDED at 29f95649a"; row 27 says "NOT a regression", names the known lossy format (B7), and its status is "open (known, not a regression)". Correct wording.
- mc2_16 frame-1735 divergence is recorded as OPEN in notes section 12 ("OPEN after B2.4") and in lane1-state NEXT with the plan (MC2_SIM_IDENTITY_TRACE_DETAIL=1 pair, f539b7f6f vs new).
- Saveload tool: stale testgame.ims is removed only when not planting, inside savegame_backup.guarded (tools/saveload_roundtrip.py:248-254), and `if not first` still fails the run. --first-save-only is opt-in (store_true), its help says it is weaker, and the re-save verdict is still printed. The mask extension is documented in the mask file's `measured` text, with per-range reasons.

## Should-fix
1. S1 tools/saveload_roundtrip.py:394 (baseline_record) and tests/saveload/baselines/*.json: a baseline recorded with --first-save-only is not marked as such (the files only carry resave_identical / resave_matches_policy); a reader cannot tell a weak baseline from a strong one. Fix: write `"judged": "first-save-only"` into the record and have check print it.
2. S2 data/defs/debug/source_diagnostics.fit:6829-6950 (and resource_aliases.fit): the catalog still holds the deleted "Unable to create ... sortList" messages with `firstSourceFile = "mclib/sortlist.cpp"`, a file that no longer exists. Fix: regenerate the diagnostics/alias catalogs in the next generated-files commit, or state in lane1-state that they are routed to it.
3. S3 code/contact.cpp:942-949 (getContacts), code/mover_combat.cpp:~1180 (sortWeapons): D4 and the commit text say malformed input is "log line + reject or clamp". A null contactList is skipped silently while the valid count is still returned. A negative listSize other than -1 clamps to 0 silently, because `count < requested` is false when requested is negative. getMoversWithinRadius never checks moverList. Fix: add a MC2_LOG_ONCE for each of these three cases (or drop the claim from the commit text).
4. S4 .claude/cleanup/cxx23-step4-b2-notes.md:292-294 vs 313-325: section 11 lists OPEN 2 (the sortWeapons fixture) as a precondition of the rule commit, and the commit went ahead without it. Section 12 only repeats it as "OPEN after B2.4". Fix: add one sentence saying who waived the precondition and why (SS1-SS7 plus the ties test cover the sort, not the Mover scoring), or get the orchestrator to acknowledge it.

## Nits
- N1 lane1-state.md (TASK 3 RESULT): "New standards stated: --first-save-only ... and the mask additions". The lane introduced a weaker standard on its own; the program rule says a lane never changes a proof standard alone. Fix: add "pending orchestrator ruling" next to it.
- N2 tools/save_noise_mask.json (`measured`): the new ranges come from one baseline run and one check run per mission (two samples). Intermittent bytes can be missed. Fix: say so in the text, and re-measure on the next save-format change.
- N3 mclib/score_sort.h:15-18: the header does not say "no allocation, in place". Fix: add that clause. Also add an explicit test with more than 200 items where the best one sits at index 250, the case the 200-slot decision lifts.
- N4 tests/analyze/b2_probes/apply_probes.py:4,77 patches the deleted mclib/sortlist.cpp, and tests/unit/golden/sortlist_ties.h:1 names the deleted generator test. Fix: add a one-line "historical, needs the pre-29f95649a tree" header to each.
- N5 notes D8 / H11 say calcOptimalRange "needs NO sort", but mover_combat.cpp:944-952 still sorts 5 items. This is harmless and documented in the brief. Fix: add a one-line note in section 12 that D8 was relaxed for this site.
- N6 BUGS row 22 "this commit" has no sha (a11b0e5c5 per notes). Fix: write the sha.
- N7 mover_combat.cpp sortWeapons: `static_cast<long>(score)` of a NaN is UB, and the old code did the same. Fix: guard it (NaN to 0) or note it as inherited.

## Baseline rewrites
- Four SIMID baselines: reason in the commit title (sha only). OK.
- cxx17 baseline and own_inventory: counts only go down, and the reason is in the commit text. OK.
- saveload baselines mc2_01/03/10/16/24: new files, with the exe sha, gates and comparison policy recorded. Each was verified by a second run (lane state). OK, subject to S1.

## Efficiency retrospective
- Fetching the diffs with `git show --format= -- paths` and filtering with grep '^[-+]' kept the output small; the first unfiltered `git show` was persisted to a file and wasted a call.
- Reading the rule-commit diff once covered most checks (sites, statics, CMake); a separate grep confirmed no remaining readers.
- I did not build or run anything, so SS1-SS7 coverage and "no allocation" come from reading the code, not from running tests.
