# Cleanup review: lane 1 B2.4-prep (52b12e473 a11b0e5c5 ff726a83b 87e6ad408 7aabb8075)

Read-only. Verdict: code is correct; no blockers. 0 blocker, 2 should-fix, 4 nits.

## Verified clean
- getMoversWithinRadius (code/ablmc2_camera.cpp:12-88): for <= 200 movers sortCount == numValidMovers, the sort-list input (slots, ids, values, scan order) is unchanged, so tie order is untouched. For > 200, SortList::setId/setValue ignore index >= numItems (sortlist.h:50-58) and sort() runs over the 200-slot list, so the result loop is now in bounds. `handleList[MAX_MOVERS]` is a stack local, `sortValues` static gone: no static-duration state added or left.
- addMover lambda (7aabb8075): `[&]` captures handleList, Team::sortList, numValidMovers; the three call sites are in the same order as the old inline blocks (handle, setId, setValue, ++). Behaviour identical.
- Terrain::markRadiusSeen: grep over code/ mclib/ editor/ *.cpp *.h finds no remaining reference (only notes/state md). execSetRevealed still pops integer, real, real-ptr, so the ABL stack stays balanced; the `Team::numTeams` rescan loop is untouched.
- SIMID re-record (ff726a83b): diff per file is exactly the `sha` and `data_fingerprint.digest` lines (8 +/- lines over 4 files); chain, visible_chain, seed, gates, env untouched. Reason and two-run proof are in the commit body.
- logDroppedObjectivePart: MC2_LOG_ONCE is process-wide per call site, so "once per run" is true; declared in objective_flags.h, defined in the PCH-skip-safe TU, reason commented. The Append `else` branch only logs.
- Comments (camera.cpp:14-15, :40, contact.h:329-332, objective_flags.cpp) state WHAT + WHY. own_inventory_baseline rewrite: counts only fell, reason stated in the commit.

## Findings

### should-fix
1. cxx23-step4-b2-notes.md section 11 (SIM-IDENTICAL SITES line, "(a) Terrain::markRadiusSeen ... delete call and function together; (b) ... unused static sortValues"): written as NEXT work but both are done in a11b0e5c5; a later session will try to redo them. Fix: mark (a) and (b) "DONE a11b0e5c5" and add the third done item (the over-read fix, BUGS row 22).
2. cxx23-step4-b2-notes.md section 11 table row `getMoversWithinRadius (ablmc2_camera.cpp:10; callers :765, :774, :804, :805)`: the file moved, now the function is :12 and the callers are :774, :783, :813, :814; the allocation site `ablmc2_camera.cpp:14` in the "other five statics" paragraph is now :16-:19 area. Fix: update the numbers, or drop line numbers in favour of function names.

### nit
3. .claude/cleanup/lane1-state.md:49 still says B2.4 waits on the Codex ABL slice (`ABLi_popIntegerBuf`, `ablmc2_camera.cpp` static `sortValues`, `ablmc2_state.cpp:677 markRadiusSeen`); section 11 says the slice is merged and the camera/state items are done. Fix: rewrite the line to the three OPEN items only.
4. Name drift: cxx23-step4-b2-notes.md:71 (D4) and :94/:127/:284 say `ABLi_popIntegerBuf`/`popIntegerBuf`; the code is `ABLi_popIntBuf()` (ablxstd_stack.cpp:116). Section 11 notes this once, but the D4 rule text is the one implementers copy; fix: add "(code name ABLi_popIntBuf)" at D4, or rename in place.
5. mclib/terrain.h:283-285: the deleted declaration left two consecutive blank lines before `long update`; fix: remove one.
6. code/ablmc2_camera.cpp:14-15 comment says the list "keeps that size (B2.4 replaces it)"; add the reason in a few words ("qsort length decides tie order") is present, but "replaces it" should name the notes section so it is findable; fix: append "see cxx23-step4-b2-notes.md section 11".

## Not checked
No build or run (read-only). own_inventory counts and the "lane-2 merged slices" cause were taken from the commit text, not re-derived.
