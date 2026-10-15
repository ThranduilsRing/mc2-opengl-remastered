<!-- doc-status: current | reviewed:2026-10-15 -->
# Cleanup review: lane 1, 2026-10-15 night (B4.0, B4.1a, B4.1b, logistics follow-ups)

WHAT: read-only review of ed0cc46c4 a3d8ce85a a2651bba3 2f13054fc 8e49ebe5d 889bd02fc 6a8687ce0, pinned at 6a8687ce0.
WHY: the per-lane cleanup review of the program cadence. No build or launch was run; the census script was run read-only (`--check` PASS, 317).

Result: 0 blockers, 3 should-fix, 5 nits.

## Checked and clean
- Logistics: `m_initFuture`, `<future>` and all three joins (start re-launch join, pre-`missionBegin->init` join, `update()` join) are gone; no other
  `m_initFuture` / `std::async` in `code/`. `initializeLogData()` now runs at logistics.cpp:169, before `MissionSelectionScreen::begin()`, so the
  STARTUP-INIT-ASYNC-1 null-deref ordering still holds. The intro movie now starts after the init instead of beside it (see N1).
- initVariants: one Info timing line per call (logisticsdata_init.cpp:489), one Warn per dropped variant (:481); callers are `init()` (:50) and the guarded
  `ensureMechBayDataLoaded` (:114), so once per load, never per frame. `varCount` still advances on a drop, so variant indices are unchanged.
- saveload refusal names the missing key and `--baseline`; selftest covers it. FST test REQUIREs `getNumFiles() > 0` before the loop.
- Census: script is tracked-file based (`git ls-files`), comments/strings stripped; a3d8ce85a list = 331 lines; tip list 317; `--check` PASS on the current tree.
  Exact command recorded in notes section 1 and before-review section 1. Before-review has the B1/B2 shape (census, change, invariants, proof, risk, open).
- B4.1a MoveMap: `assign` only in `init(h, w)`, which runs only when `map.empty()` (move_movemap_goals.cpp setUp both overloads); the reuse branch only
  resets width/height, so no per-solve allocation. Table sizes and index math identical to the Malloc version. `destroy` callers: none per solve found.
- B4.1b: accessors in move.h:595+ are inBounds-guarded, so an empty map after a rejected pak is a no-op, not an OOB; `tileHasMines` falls back conservatively.
  `write` emits `map.size()*sizeof(MapCell)` = the old `height*width*sizeof` for a loaded map, so save layout is unchanged (invariant 6).
- 2f13054fc: include-only, one line, objhash 84/84 claim recorded with the command. CRLF: all touched blobs are LF before and after (no CR anywhere, no
  mixed endings) in logisticsdata_init.cpp, logistics.cpp, move*.cpp/h and the touched docs; no heredoc/sed damage.

## Should-fix
- S1 mclib/move.cpp:782-787: a wrong-size height or width packet logs two Errors (the packet line, then "0 x N cells"), against invariant 2's "one line";
  skip the dimension log when a dimension read already failed (track the readExactPacket results).
- S2 mclib/move.cpp:784 + 791: a rejected planet or cell packet leaves `planet` stale from the previous mission / a clear grid, and `init` still returns
  NUM_MISSIONMAP_PACKETS so the mission plays on a passable-everywhere map; return a failure the caller (move.cpp:576) turns into a refused mission load,
  or record in the commit/BUGS why a silently empty map is the ruling-5 "not loaded" outcome.
- S3 .claude/cleanup/cxx23-step4-b4-before-review.md:52 + notes line 3: B4.1b was planned as 9 sites including the special-area footprint scratch, but
  8e49ebe5d did 6 and `tempSpecialAreaFootPrints` (move.cpp:467/472) is still in the list while the notes say "B4.1b DONE"; either mark the scratch as moved to
  B4.1c or state it is left, so the plan and the census agree. Also invariant 2 asks for a malformed-pak check on a pak reader; B4.1b added one with no test
  ("no unit seam"): carry it as owed in the notes next to B4.1c's malformed-pak doctest.

## Nits
- N1 code/logistics.cpp:166-169: the commit asserts the async "bought no overlap"; add the measured startup delta (BUGS 37 has +150 ms parse) or say the intro
  now starts that much later, so the trade is visible.
- N2 .claude/cleanup/cxx23-step4-b4-notes.md:2: the title still says "census only ... NOT yet read by a before-review, no code"; refresh it to the status line below.
- N3 mclib/move.cpp:451: "[MOVE_BD] FATAL ... (zero map size)" is a printf in a file that now has MC2_LOG; route it through MC2_LOG when B4.1c touches it.
- N4 mclib/move_movemap_escape.cpp:445: "a destroyed map is re-sized by its next solve" reads as HOW; "frees the grid now; the next solve re-sizes it" is fine,
  or drop the parenthetical about the destructor.
- N5 code/logisticsdata_init.cpp:481: the Warn says "row {}" with the CSV row stride (97 per variant); say "CSV row" so a reader does not take it as a variant index.
