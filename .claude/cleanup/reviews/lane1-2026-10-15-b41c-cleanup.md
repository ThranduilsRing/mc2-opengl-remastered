# Cleanup review: lane 1, B4.1c session (1bf7b786c 2d049e588 85b6a283f edb806c55 e21a336b3)

Read-only. Pinned to the five commits. Blockers 0, should-fix 3, nits 3.

## Verified clean
- Views stay valid: every `ownZeroed` call (reassign) happens before the views into that store are taken. Pak load:
  doorInfos pooled before `areas_doors[i]=&doorInfos[..]`; doorLinks pooled once before the `doors_links[i][s]` loop;
  `ownDoorLinkViews` called once per load/build, before the loop that fills it. Build: calcDoorLinks sizes the pool up
  front (`linkTotal`, asserted `nextLink == linkTotal`), so no regrow while views are held. destroy() swaps all eleven
  stores; copy deleted. `areas`/`doors` pointers read by claimed files are reassigned together with their store.
- No per-solve allocation: move_globalmap_path.cpp has no Malloc/new/vector growth outside destroy().
- Counts range-checked with one MC2_LOG line plus badLoad: header counts, each area's door count, each door side's link
  count, packets above 16x (three tolerantBytes sites). Doctest: header count table covers both bounds of all eight
  counts (named result), per-area, per-side, slack, ownZeroed.
- 2d049e588 is include-only: header adds only <cstddef>/<vector> (already in move.h), no uses; the static_assert is
  compile-time only. 214 inserted lines, no object change claimed and plausible.
- edb806c55 message: states the attribution (6d4ff1bb3 exe passes the old baselines, so sim-s4b2b is not a cause), the
  differing bytes by class, and the stability pair per mission with digests. lane1-state records `check` x5 PASS on the
  B4.1c exe, so the zero-fill left every digest alone.
- test_authored_movement_fixture.cpp: one-token change (`GameMap->map` -> `!GameMap->map.empty()`), no behaviour change.
- 1bf7b786c: TestStateGuard restores stop switch and stream override, closes capture, removes scratch pak; path in
  temp_directory_path(); zlib packet case with a size-mismatch check. The N6/N4/N5 comments now state the invariant.
- Ratchet message names own rises (LG06 +1, move.h 1837 -> 1860); deltas reconcile with the diff (LG01 -23/-1 = -24,
  LG09 -8/-4 = -12). No CRLF damage: added lines carry CR consistently (move.h 1860/1860, build.cpp 1075/1075, new
  header and test too).

## Should-fix
1. Header-count refusal returns the wrong packet count (move_globalmap_build.cpp, the rejectHeaderCounts block). It
   zeroes the counts and returns `whichPacket - startPacket`, while every other badLoad path returns
   `13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2` so MOVE_readData can step to the next map. A refused first
   map now misaligns the caller's cursor for the second and third maps (and the special-area packet). Either return
   the layout size from the raw header counts when they are non-negative, or document that a refused header ends the
   whole movement load and make MOVE_readData stop on it. Needs a malformed-pak case through GlobalMap::init (the
   doctests only drive the pure helpers; no test reaches init, the views or the return value).
2. `numDoorLinks` ceiling is 5.1M links and `numDoorInfos` 2.55M (kMaxAreas*255): bounded, but a hostile header still
   sizes a multi-hundred-MB zeroed vector before any packet is read. Tie the link and info counts to the header's own
   numAreas/numDoors (e.g. links <= (numDoors+2)*2*257) so the bound is the map's, not the design maximum.
3. Stale comment in globalmap_limits.h: "move.cpp pins them equal with static_asserts"; the assert is in
   move_internal.h (2d049e588), single assert, not move.cpp.

## Nits
- Header comment "a packet more than 16x" bound adds a 4096 byte floor not mentioned in the comment; say so (WHY: tiny
  structs).
- 85b6a283f message "proofs follow in lane1-state" is a deferred claim; the proof exists (state lines 5, 8) but the
  commit alone does not carry it. The message also narrates HOW (pooling mechanics) at length; fine for the body.
- tolerantBytes returning 0 as the refusal sentinel is easy to misuse; a doctest-visible std::optional would not be
  worth the churn, but name the constant `kRefused` at call sites.

## Open from lane (not re-litigated)
Decode-failure fixture for readExact, S3 claims, tempSpecialAreaFootPrints rows, pathfinder scratch in the digest mask.

## Efficiency retrospective
- Biggest saving: the commit bodies and lane1-state carried the proof, so no builds or reruns were needed.
- Waste: first diff dump was truncated; targeted greps of one build.cpp snapshot were enough, start there.
- Next time: ask for a malformed-pak case through init up front, since helper-only doctests miss return-value bugs.
