<!-- doc-status: current | reviewed:2026-10-14 -->
# Lane 1 cleanup review, 2026-10-14 (B2.3 and the review-fix commits), tip a282c7471

Scope: 21707add2 4a34da077 fd4b44e7e e97153f0a 0ff0e1e0c 336f18d08 29ef2f79a af3903249 7c799ce96 e794336fd 099439d86 (read-only; nothing built or run).
VERDICT: **APPROVE.** Blockers 0, should-fix 2, nits 6. The 84-site objective conversion preserves behaviour; the findings are a log flood and two contract gaps.

## Should-fix
1. **code/contact.cpp:959** The "exceed the sortable" log fires on EVERY getContacts call once a team holds more than 200 valid contacts (sentry wake check, engage-nearest, ABL
   builtins: several calls per tick), while the sibling fix logs once per group. A swarm mission would flood stderr in the sim path. Fix: use `MC2_LOG_ONCE` (exists, mc2log.h:203)
   or `MC2_LOG_EVERY`.
2. **code/contact.cpp:952-978 with code/brain_special_dispatch.cpp:1463-1468** The sorted-prefix-plus-unsorted-tail contract is not recorded where consumers read it. Consumers that
   assume a fully sorted list: `tickEngageNearest` takes `cl[0]` as "nearest first", `warrior_target.cpp:107/153/350` take the first match of a CONTACT_SORT_DISTANCE list as nearest,
   and `execGetContacts` (ablmc2_queries.cpp:125-131) documents "a sorted list" to scripts. Beyond 200 valid contacts the true nearest can sit in the unsorted tail (scan order), so
   the choice is silently wrong, not just unsorted. Not a regression (it was a heap overrun before) and it dies with B2.4. Fix: state the limit in the `getContacts` declaration comment
   (contact.h) and in BUGS-FOUND-FIXED row 21 ("nearest/strongest guarantee holds only within the first 200 in scan order"), and put the case on the B2.4 re-baseline list.

## Nits
3. **code/logisticsmissioninfo.h:194** `outOfRange` is one record per group, reset on every out-of-range `mission()` call (logisticsmissioninfo.cpp:51), so two live pointers alias
   one object and a later call rewrites what an earlier caller still holds. No current caller keeps two; fix: say "valid until the next mission() call on this group" in the
   declaration comment (`getPreviousMission`, query.cpp:298, passes this pointer on to its callers).
4. **code/logisticsmissioninfo_query.cpp:259** `setSkipLogistics` writes `playLogistics` through `mission(currentMission)`; with currentMission == -1 the write lands in the discarded
   default record (one log line, but the caller believes it worked). Fix: guard with `groups[currentStage].at(currentMission)` and skip the write when null.
5. **code/objective.h:484** `using iterator = const_iterator` yields `T*` by value (`reference = T*`), so it is an input iterator under the C++20 concepts despite the
   `forward_iterator_tag`; std::ranges algorithms over `Team::home->objectives` would not compile. Fix: declare `std::input_iterator_tag`, or return `T* const&`.
6. **code/objective.h:490** `Append` silently drops a null element with no log. Every call site passes a fresh `new` pointer, so it is unreachable today; fix: one `MC2_LOG_ONCE`
   there so a future null (the CRASH-HARDEN factory paths) is visible.
7. **code/objective.cpp (C_RemoveStructure::Execute, after ReadNavMarkers)** dereferences `m_pBuilding` from `getByWatchID` unchecked; pre-existing and outside this batch, but the
   id comes from a mission file. Fix: null-check with a log line in the next crash-hardening slice.
8. **code/ablmc2.cpp:283** The comment says initABL runs "once per mission load", but it is called from missionbegin.cpp:312, mission_init.cpp:146 and saveload.cpp:584, so
   one load can reset the budget twice. Harmless; fix: reword to "per ABL session start".

## Checks that passed (evidence)
- **MissionGroup::mission() (21707add2):** no static-duration owner (D7): `outOfRange` and `outOfRangeLogged` are non-static `mutable` members. `groups` is a `new[]` array owned by
  LogisticsMissionInfo, so the pointer lives as long as the group; callers use it at once (30 sites: field reads, one write at query.cpp:259, one pass-through at :298). No dangling
  reference. Log is once per group, WHAT+WHY comment present, D14 listed in the commit message.
- **getContacts (4a34da077):** `handleList`/`sortValues` are MAX_CONTACTS_PER_TEAM (static_assert against `contacts[]`); the sort list keeps 200 slots (qsort length decides tie
  order), `sortCount` caps setId/setValue/getId, the tail copies `handleList[contact]` for `[sortCount, numValid)`: no access past either array. With <=200 contacts the path is
  the old one. The BIG ASSUMPTION comment is replaced by the contract.
- **fd4b44e7e / e2c324f93 / e5fd0205d:** the std::array form is restored and `wcl` is MAX_CONTACTS_PER_TEAM long (the `int[8]` overrun is gone). Baseline rewrites each carry a
  reason (brain_special_dispatch.cpp 2772 -> 2775, warrior.h 2170 -> 2171, objective.cpp 2330 -> 2259); the cxx17 baseline moved down only. No code reshaped for a ratchet.
- **CObjectiveOwnedList<T> (af3903249, 7c799ce96):** `std::vector<std::unique_ptr<T>>`, copy deleted, heap objects keep their addresses while the vector grows, `Clear()` frees
  head to tail (D15) in the old order: conditions, actions, failure conditions, failure actions. `~CObjective` and `~CObjectives` call Clear(). CObjectiveCondition (objective.h:95)
  and CObjectiveAction (:355) have virtual destructors, so unique_ptr<base> deletes correctly. Only Append/Clear/Count remain as list API; nothing erases a single element.
- **Iteration invalidation:** the only Clear()/Append() callers on live objective lists are team.cpp:44/417, mission_init_load.cpp:1098, saveload_load.cpp:445 and ReadNavMarkers
  (objective.cpp:2035), all outside any objective loop; actions run from `CObjective::Status()` do not touch the lists. No erase-during-iterate exists or was introduced.
- **84 call sites, behaviour (D3):** every loop keeps its shape. `checkObjectiveSuccess/Failed` still visit all objectives with `|=`; `checkObjectiveStatus` keeps the running
  `count` and falls through to OS_UNDETERMINED; `count++`/`i++` moved from the for-increment to the body end in mission.cpp:979-1025, missionbriefingscreen.cpp:581-614 and
  gametacmap.cpp:243-272 where the bodies hold no `continue` that would skip it (gotoNextNavMarker has `continue` and no counter). `CObjective::Status` keeps its first-failure
  `break` and the UNDETERMINED `break` of the failure list. Order is insertion order as before. Null handling: the one change is the listed one (action factory null skipped in
  operator=, as the condition copies already were).
- **D16:** 29ef2f79a is the include-only parent for objective.h (objective.cpp is on the PCH skip list); `proofs/b2.3-include-only-objhash-summary.json` is the evidence.
- **336f18d08 (r4-outside-claim):** applied narrowly: `{eventID, count}` replaces the second `getEventHistory` call, the params buffer is `MAX_ALARM_TRIGGERS` with a static_assert
  against `PilotAlarm::trigger`, the omitted hunks are stated in the commit, null CurWarrior still yields -1.
- **0ff0e1e0c:** `abl::resetEventBudgets()` (mclib/abl_script_events.cpp:139) is a plain fill(0), called first in initABL; no ordering hazard.
- **e97153f0a:** `clearInventory()` replaces `inventory.clear()` before `clearPilots()`; matches the other four callers.
- **e794336fd / 099439d86:** comment is WHAT+WHY and states the once-only log; the empty ELIST_H guard is gone; rows 20 and 21 give cause, fix commit and proof.
- **D13:** `git ls-files --eol` shows uniform endings for every touched file (CRLF stays CRLF, LF stays LF).
- **D4 (malformed content logs):** mission() and getContacts log; the objective readers keep their `assert(false); delete; continue` paths with no release log line (old behaviour, nit 6/7 territory).

## Efficiency retrospective
- Reading each commit diff first and then grepping callers (30 `mission()` sites, 7 `getContacts` sites) answered the dangling/consumer questions in about ten tool calls.
- A CR-counting grep was misleading; `git ls-files --eol` is the one-call answer for D13 and should come first.
- Naming the exact questions in the prompt (static owner, erase-during-iterate, sorted assumption) let the review skip the unrelated merge 4580ba8d4 and the unlisted commits.
