<!-- doc-status: current | reviewed:2026-10-13 -->
# Lane 1 cleanup review, B2 review fixes + B2.2a/b + B2.4-pre (57b20bbb7 .. 10ae810ea)

Scope: the 11 commits (oldest first) 57b20bbb7 01a1f866d 7b07e16d3 e12f3c3ba d26dc8891 164fe55d5 ece48d502 a92eeff85 78d63070c 72f2628fe 10ae810ea.
Read-only: diffs and current-tree greps only; no build, no run. Rules D1-D16 from `cxx23-step4-b2-review.md`.

VERDICT: no blocker. 3 should-fix, 5 nit. The vector/deque conversions, the erase sites, the deleted lineOfSight overload and the
calcPath helper are correct as far as the diffs show.

## Verified clean (what was checked, so it is not re-checked)
- **Logistics erase sites** (ece48d502): `sellMech` `erase(std::next(rit).base())` is the right reverse erase; `removeMechsInForceGroup` / `clearVariants`
  `--iter; delete; iter = erase(iter)` visits every element once, back to front, same order as EList; `removeVariant` erases `vIter` only after the
  not-found return; the `acceptMechModifications` forward `delete + erase` loop is correct. D15 held.
- **deque contract** (a92eeff85): only `push_back` / `pop_back` / `clear` touch components, pilots, buildings; `clearPilots` is the single emptier and
  detaches mech pilot pointers before freeing. D1 and D6 hold; `getInventory()` returns a by-value snapshot.
- **lineOfSight** (01a1f866d): the (cellRow, cellCol, bool) overload passed endLocal one slot short; its only caller `calcLineOfSightView` had no caller,
  and grep now finds neither. No derived class overrides `lineOfSight`; remaining call sites match the two surviving overloads. The removed `markSeen`
  bodies were `return;` (Terrain) or one-line forwarders, so dropping the callers and their LOS locals changes nothing.
- **reKeyOpenDoor** (57b20bbb7): both A* sites route through one helper; a miss skips `change`; H18b pins the old `change(0, k)` corruption.
- **getContacts** (d26dc8891): `handleList`/`sortValues` are `MAX_CONTACTS_PER_TEAM`; every caller found by grep sizes its array `MAX_MOVERS`
  (warrior_target.cpp:106/152/349) or `MAX_CONTACTS_PER_TEAM` (brain_special_dispatch.cpp:1442/1460); the static scratch is gone (D7).
- **Baseline rewrites** each carry a reason: 7b07e16d3 (LG09 -5, LG12b -2, total 16,171 -> 16,164), 78d63070c (LG02 -3, head sha), ece48d502
  (own_inventory SortList.new 6 -> 4, delete 2 -> 1), 57b20bbb7 (log tag census + DISK_GUARD drop). The census/doc rewrites in 72f2628fe state the stale-row reason.

## Should-fix
1. **should-fix** `code/logisticsmissioninfo.h` (MissionGroup::mission, added in ece48d502): the bounds check is `gosASSERT`, compiled out in release, and the
   comment says the index "is always a position of this group". It is not: `currentMission` starts at -1 (logisticsmissioninfo.cpp:23/65) and the -1
   sentinel is tested at progress.cpp:150, query.cpp:50/152, so the unguarded `mission(currentMission)` calls (query.cpp:15, 74, 100, 122, 249, 290,
   339-373; progress.cpp:310, 376) are still `infos[size_t(-1)]` in release. Fix: make `mission()` return `at()` with a one-line `MC2_LOG` and a safe
   default (or early-out on -1 as query.cpp:50 does), and correct the comment.
2. **should-fix** `code/contact.cpp:951-962` (TeamSensorSystem::getContacts): the buffers are fixed but `SensorSystem::sortList` is still
   `init(MAX_CONTACTS_PER_SENSOR = 200)` and `getId(contact)` reads slots 200..254 when a team holds more than 200 valid contacts, so the read-side
   overrun survives. The commit message says this is deliberate until B2.4 (tie order depends on the qsort length), but the "BIG ASSUMPTION" comment
   still reads as if it held. Fix: until B2.4 lands, clamp the copy-out loop at `MAX_CONTACTS_PER_SENSOR` with one log line and rewrite the comment.
3. **should-fix** `code/brain_special_dispatch.cpp:1442,1459` (164fe55d5): the follow-up reverted the safer `std::array<int, MAX_CONTACTS_PER_TEAM>` to plain
   `int[]` only because `file_size_ratchet` failed by 3 lines (2772 -> 2775). That is code reshaped for a ratchet, which the standing rules forbid.
   Fix: restore the std::array form and take the 3-line baseline bump with a stated reason, or keep the plain arrays and say in the commit that the
   ratchet, not the code, was the driver.

## Nit
4. **nit** `mclib/move_globalmap_path.cpp` (reKeyOpenDoor): `static bool reported` is a function-local static-duration flag (the D7 pattern), and the "once per
   run" line hides a second mission's misses. Fix: count misses in the solver and print the total at mission end, or say "first miss per process" in the comment.
5. **nit** `mclib/move_globalmap_path.cpp` (reKeyOpenDoor comment): "B2.5-pre" and "the A* open-list corruption" name a batch, not the invariant. Fix: keep
   WHAT (re-key a door's open-list entry) and WHY (change() takes a heap position, 0 = miss) only.
6. **nit** `code/componentlistbox.h:19-20`: `#ifndef ELIST_H` / `#endif` is left empty after the `#include "elist.h"` removal. Fix: delete the empty guard pair
   (and any twin in the other edited headers).
7. **nit** `code/logisticsdata_inventory.cpp` (`removeVariant`): the in-use scan walks the whole inventory in reverse for a yes/no answer. Fix: a forward
   `std::any_of` or loop; no behaviour change.
8. **nit** `code/logisticsdata.cpp` (`startNewCampaign`, `startMultiPlayer`): `inventory.clear()` drops raw `LogisticsMech*` without deleting them while
   `clearInventory()` (logisticsdata_modify.cpp:34) deletes them; the leak is old, but these are the callers `clearPilots` assumes are clean. Fix: call
   `clearInventory()` there, or record it in the notes for B2.2c/B6.

## Rule scorecard
| rule | result |
|---|---|
| D1 vector vs deque | pass; contract written at the declaration |
| D2 composition | n/a in these commits |
| D3 loop shape | pass; loops keep their `break` / `continue` shape |
| D4 checked index | pass for save-supplied indices (`at()` + log line); see should-fix 1 for the internal one |
| D5 sizes | pass; `std::ssize` with explicit casts at int/long targets |
| D6 snapshot getters | pass |
| D7 no static owner/scratch | pass for scratch; nit 4 for the log flag |
| D13 CRLF / comments | spot-checked comments are WHAT+WHY (nit 5 aside); CRLF not re-verified |
| D16 include-only | pass: e12f3c3ba is include-only with an objhash summary (107 IDENTICAL, 7 LAYOUT-ONLY, 4 fingerprint DIFF) |

## Efficiency retrospective
- Reading commit messages first (they state the proof and the claimed behaviour) let me aim greps at the claims; the logistics diffs were the bulk of the cost and mostly mechanical.
- The highest-yield checks were "does the new comment hold at every caller" (mission() with -1, caller array sizes), not diff reading.
- Not done: no build, doctest, ASan, CRLF or census-seal run, so those lines rest on the commit messages.
