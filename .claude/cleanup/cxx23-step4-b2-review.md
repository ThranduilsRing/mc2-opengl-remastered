<!-- doc-status: current | reviewed:2026-10-13 -->
# C++23 step 4 B2 (value containers -> std containers): adversarial before-review RB2 of `cxx23-step4-b2-notes.md` (167219f65)

VERDICT: **APPROVE-WITH-CHANGES.** The method is the B1 method that worked (characterization tests on the OLD classes, a compiler census, rules, a
proof table, a blind-spot section), and the three big decisions are right: SortList is deleted for an explicit total order, PriorityQueue keeps its
heap rules, DynamicArrayOf waits for B8. What is missing would let a "behaviour-preserving" swap silently change the sim or hide UB:
(1) a heap-corruption bug in the global A* (`GlobalMap::propogateCost` passes a door ID as a heap index) makes the SHARED open list's stale
slots observable, so neither B2.5 nor any ownership change of the open list is behaviour-preserving until it is fixed; (2) the 200-slot contact
overflow is wider than H8 says and the ABL overruns are worse than H7 says, and the modern fix (size to the true bound; ABL arrays as spans with
their declared length) removes the clamp in most places; (3) the SIMID ladder cannot attribute a divergence; a patched-old-exe equivalence can;
(4) three of the edited TUs are on the PCH skip list, so adding `<vector>`/`<memory>` to them can change float overloads inside a "no-change" batch;
(5) the inventory sentence is incomplete (the sim reads the logistics pilot list; one "Fatal-guarded" site is not dead by that argument);
(6) the editor E1 batch edits the same 21 files as lane 1's B1.6.
Read: the notes and files.json, ownership plan sections 5/9/10, step-4 review findings 1/8/9/10, the B1 review, RA1, B1 notes 8c-8e,
`sim-kernel-design.md`, lane1-state, the meta-review ruling memo, and the code at a76b8751c: `mclib/{elist.h,sortlist.*,pqueue.*,bitflag.*}`,
`mclib/move_{globalmap_path,movemap_path,movemap_escape,internal,solvectx}.*`, `code/{contact,mover_combat,ablmc2_camera,ablmc2_queries,
group_goals,group_orders,warrior,warrior_state,objective,mission,logisticsdata_*}.cpp`, `mclib/err.cpp`, `mclib/ablxstd_stack.cpp`,
`cmake/pch/pch_skip.cmake`. No build, no launch; "[unverified]" marks what only a run can settle.

## 1. Inventory and census
- **The claim is incomplete.** Sim-visible users of these containers, beyond the objective lists, the SortList sorts and the A* open list:
  (a) the logistics `pilots` list is read by the sim: `Mission::tradeMover` -> `getPilot` (mission_movers.cpp:508) on every captured or salvaged
  unit, fed by `getBestPilot` (an iteration over `pilots`, ties broken by list order) from missiongui_support.cpp:176, missiongui_style.cpp:157,
  mission_update.cpp:849; (b) ABL builtins reach the containers directly: `execGetContacts` (ablmc2_queries.cpp:107-133) and `execSortWeapons`
  (:645-654) write into script arrays, `checkObjectiveStatus/Success/Failed` (ablmc2_objects.cpp:519/664/669) run `CObjective::Status`, which
  EXECUTES actions; (c) two static-duration scratch arrays sit next to the sorts (contact.cpp:926 `static float sortValues[200]`, used;
  ablmc2_camera.cpp:10 `static float sortValues[MAX_MOVERS]`, never used). The movers/teams/rosters statement is right (fixed arrays and pools: B7).
- **"group_orders.cpp:60 sits behind a Fatal" is not a dead-code argument.** `Fatal` -> `STOP` -> `InternalFunctionStop` logs and RETURNS in
  release (err.cpp:21-26, gameos_debugging.cpp:51-61), so the SortList code after it runs whenever `MoverGroup::handleTacticalOrder` gets a
  MOVETO order. It is dead only because that function has no caller (grep: none; every `handleTacticalOrder` call is on a Mover). The census must
  prove that (R1); the deletion takes the MOVETO branch or the function.
- **Census: yes, B2.0 produces a compiler-derived census exactly as B1 did**, and it is the site list. The grep set already disagrees with itself
  (notes table 1 and files.json `verify`: 64 editor files; section 4 and `counts`: 66). Deprecate the public API of `EList`, `SortList`,
  `PriorityQueue`, `ByteFlag/BitFlag` and `MoverGroup::handleTacticalOrder`/`sortMovers`; `build_all_targets --root-only`; commit
  `cxx23-step4-b2-callsites.txt` with a seal (the B1 M2 method). The census cannot see inactive `#if` branches (`USE_OLD_LOS`, `LAB_ONLY`,
  `TERRAINEDIT`, `_DEBUG`): a grep over them is part of the census file.

## 2. Hazards: real?, severity, handling, before or with the swap
| H | verdict | severity | handling (modern answer) | when |
|---|---|---|---|---|
| H1 address escapes | real | high if a vector is used | `std::deque` is right for append-only by-value lists (push_back/pop_back keep references, which is all EList was relied on for here; the census confirms no mid-list Insert/Delete on these three). Make the contract STRUCTURAL: the lists are private behind an append-only API (`addPilot`, `clearPilots` that requires an empty inventory), not a comment. The escape removal (mech stores a pilot ID; `getBestPilot` returns `const std::string&`, which RA1 asked for and which is still `const char*` with three `(char*)` casts) is logistics-model work for B6 | with B2.2 |
| H2 erase while iterating | real | medium | `std::erase_if`, reverse views; when pointee destructor order is observable, destroy in an explicit loop in the pinned order (HE12; `std::vector` does not specify element destruction order) | with |
| H3 absent erase | **misstated** | none | `removeMechFromInventory` gets `pMech` from `getMech`, which only returns inventory members, under `if (pMech)` (save.cpp:635-669): `Find` cannot fail there. `std::erase(v,p)` is still the target; delete "changed edge" | - |
| H4 inheritance | real | medium | composition (D2); see H20 | with B2.3 |
| H5 saved index | real | medium (crafted save) | D4 checked accessor + log | with B2.2 |
| H6 widths | real | low | D5 | with |
| H7 ABL sortWeapons | real, **worse** | high (mod content) | `listSize == -1` writes `numWeapons` entries into the SCRIPT array of unknown length; `listSize > 50` overruns `valueList`, which is a dead output (never returned to the script); script-supplied weapon indices index `inventory` unchecked. Fix: `ABLi_popIntegerBuf()` returning `std::span<int>` with the declared element count (precedent: `ABLi_popCharBuf`, ablxstd_stack.cpp:89-98); `sortWeapons(std::span<int>, ...)` never writes past it; `listSize` clamped to the span with one log line; an index outside the mover's weapons sorts as -999 (the existing "none" value) with a log line; `valueList` deleted. Same span for `execGetContacts` | BEFORE (B2.4-pre) |
| H8 contact overflow | real, **wider** | high (UB) | `TeamSensorSystem::getContacts` loops `numContacts` (<= `MAX_CONTACTS_PER_TEAM` = `MAX_MOVERS` = 255, dteam.h:18) into `handleList[200]` and the static `sortValues[200]` (contact.cpp:926-931): stack and static overrun past 200 valid contacts, before the SortList is reached. `getMoversWithinRadius` walks up to 255 movers into a 200-slot list. Every caller's output array is already `MAX_MOVERS` (ablmc2_camera.cpp:769/788/807-808, warrior_target.cpp:152). Modern fix: size every buffer to `MAX_MOVERS` with a `static_assert` against the team cap: overflow becomes impossible, no clamp, no log, no change on any non-UB input. Output goes through `std::span`, so `brain_special_dispatch.cpp:1442` (`int wcl[8]`) can no longer be overrun (write `min(n, span.size())`, return the full count as today) | BEFORE (B2.4-pre) |
| H9-H11 | right | - | delete sortMovers etc.; H11 also means `calcOptimalRange` needs NO sort: it is an argmax plus the tie mask over 5 values (D8) | with B2.4 |
| H12 5002nd insert | real, **reachable** | medium | `MAX_GLOBALMAP_DOORS` is 10000 (move.h:54), so the global A* can exceed 5001 open nodes. Capacity = the graph size, reserved once per solver (move-map cells, `numDoors + NUM_DOOR_OFFSETS`, `GOALMAP_DIM^2`): never grows mid-solve, never drops; identical to today whenever today did not overflow. P2 records the max per solver | with B2.5 |
| H13-H17 | right | low | as written; H15 gets the objhash proof (section 6) | with |
| **H18 NEW** | real | **high** | `GlobalMap::propogateCost` calls `openList->change(door, ...)` (move_globalmap_path.cpp:201) with the door ID, not `openIndex` (the MoveMap twin, move_internal.h:184, is correct). It re-keys an arbitrary slot, can sift a STALE node from an earlier solve (of any solver: the buffer is shared) into the live heap, and writes past the 5002-slot buffer for a door >= 5002. Fix: `change(openIndex, ...)`, its own commit, ruled like the ladder (section 3); P2 counts hits | BEFORE B2.5 |
| **H19 NEW** | real | low | the two static scratch arrays (section 1c) go in B2.4-pre (sim-kernel: no static-duration scratch) | B2.4-pre |
| **H20 NEW** | real | high if missed | objective loops have side effects and exact short-circuit shapes: `CObjective::Status()` runs actions and `break`s on the first failure; `checkObjectiveSuccess` evaluates ALL objectives with `result |=`; `checkObjectiveStatus(n)` returns `OS_UNDETERMINED` for an out-of-range ABL index. A conversion to `any_of`/`all_of`/`find_if`/`||` changes the sim (D3) | with B2.3 |
| **H21 NEW** | real | low | the old descending sort over the padded array lets a sentinel slot into the prefix when a real score is NaN or -inf (its id = slot number = a bogus weapon index or handle). The prefix-only sort cannot; a listed behaviour change; P1 counts NaN/inf | with B2.4 |
| **H22 NEW** | real | high if missed | `code/objective.cpp`, `code/group_goals.cpp` and `mclib/move.cpp` are on the PCH skip list (pch_skip.cmake:119, 92, 69): their code CHANGES when `<cmath>`/`<cstdlib>` overloads become visible. B2.3/B2.4/B2.5 add `<vector>`/`<memory>`/`<algorithm>`/`<cmath>` to them or to headers they include. Rule D16 | before each such batch |

## 3. SortList
- **Strict weak ordering: yes**, with one trap. Key = (is_nan, score in the requested direction, slot). Slots are unique, so it is a strict total
  order and `std::sort` and `std::stable_sort` give the same output (keep `std::sort`: no temporary buffer in a per-frame path; a doctest proves
  equality with `stable_sort` on random inputs). Trap: a comparator written `a.score != b.score ? ... : slot` is wrong for two NaNs (NaN != NaN
  is true and both `>` tests are false): test `isnan` on both operands FIRST. HS7 includes two NaNs, +-0, +-inf and denormals. NaN-last is
  acceptable for threat, CV, distance and attack chance (none is NaN on stock data [unverified]: P1 counts it).
- **Does it match the old qsort on stock data?** Exactly where the filled prefix has no ties: then any correct sort gives the one order, and every
  real score lies strictly inside the sentinels. Ties are NOT rare at three of the four live sites: identical weapons in one location give equal
  attack chances (`sortWeapons`), identical unit types give equal CV and threat (`getContacts` CV sort, `getMoversWithinRadius`). Only distance ties
  and `calcOptimalRange` (order-free) are rare. Expect P1 > 0 and plan for one re-baseline at B2.4.
- **Reproducing MSVC's order is possible but ruled out**, not impossible: sort the same padded array with the same CRT qsort. That is what the
  owner rejected (implementation-defined; glibc differs). Answer to Q3: re-baseline.
- **The ladder cannot attribute a divergence.** "First SIMID divergence = first P1 divergence" is necessary, not sufficient: a reordered contact
  list changes state frames later or never. Replace steps 2-3 with a **patched-old equivalence**: build the pre-B2.4 tree with an uncommitted
  ~20-line patch that sorts ONLY the filled prefix with the new key (scratch worktree, the B1 G2 recipe); record its SIMID chain on mc2_03/10/16/24
  as a scratch baseline; the B2.4 exe must equal it on every frame of every window. Then the whole SIMID change is attributable to the rule and the
  container refactor is proven neutral; the orchestrator adopts that chain as the new baseline (the sim-kernel's "reviewed ruleset change").
  New exe != patched-old exe is a defect and a hard stop. Step 4 (12 pairs + the camera-park pair on the new exe) stays.
  The same equivalence applies to H18 if P2 shows hits.
- **P1 coverage**: the four baseline windows plus one long mc2_24 run past tick 4000 (sim-kernel recipe); per site: calls, max n, calls with ties
  in the prefix, calls whose qsort order differs from the new key, NaN/inf count. A site with 0 calls is UNEXERCISED, listed as such, never "P1 = 0".

## 4. PriorityQueue: keep the heap rules (decided)
Keep D9: same 1-based layout, `upHeap` moves above an EQUAL parent, `downHeap` takes the LEFT child on equal children and stops on an equal key,
linear `find` from index 0 returning the first match (a position index is a later perf change, proven by HP6), `change` as today.
Why not extend the ruling: the ruling targeted implementation-defined order (CRT qsort). The heap is our code: its tie order is already a pure,
platform-independent function of the operation sequence, so a (key, insertion sequence) order buys no determinism. Cost of keeping: HP6 and P2
only. Cost of extending: a 20-byte node, every equal-f expansion order changes, so every mission's paths and every SIMID/liveness baseline is
re-made, and B2.5 stops being a provable container swap. If path fairness ever needs a different tie rule it is a sim-kernel ruleset step, not B2.
Name: `IndexedMinHeap` suggests a position index it does not have; call it `mc2::PathOpenList`, or keep the name and say "index = heap position".

## 5. Dead code (Q8)
Bar: census rows = 0 for the symbol + `build_all_targets` rc 0 + a grep of inactive `#if` branches and `data/` (FIT keys, ABL scripts) + tag
`step4-b2-pre` + objhash IDENTICAL on every TU not edited (B2.1). Sufficient for `BitFlag/ByteFlag/dbitflag.h`, `VisibleBits/SeenBits`,
`MechWarrior::sortList` (allocated at warrior.cpp:537 and warrior_state.cpp:887, freed at :1056, never read), `MoverGroup::sortMovers` and the
undefined `MoverGroup::sortList`, `findByKey`/`getItem`. The `USE_OLD_LOS` branch of team_los.cpp:123 and the commented `VisibleBits` blocks of
terrain.cpp:951/991 are deleted in B2.1 (dead code naming a deleted type). `MoverGroup::handleTacticalOrder`'s MOVETO branch: section 1.

## 6. Batches, order, proof bar (Q7, Q10)
Order: B2.0 -> B2.1 -> B2.2a/b(/c) -> B2.3 -> B2.4-pre -> B2.4 -> B2.5-pre -> B2.5. Objectives BEFORE SortList (Q10): B2.3 must be SIMID-identical,
and the re-baseline at B2.4 must happen with no other unproven sim change in flight; B2.4's ruleset commit contains nothing but the rule switch.
Editor: `elist.h` stays until E-end, no shim (Q7). **E1 (editor objective model, 21 files) edits the same files as B1.6** (editor/Objective.cpp,
ObjectiveDlg.cpp, ObjectivesSummaryPanel.cpp, ...): E1 starts after B1.6 is merged and after-reviewed, or one agent does both. E2-E4 may run in
parallel with B2.4/B2.5 after `check_conflict`; E2 changes the undo stack's container only (`EList<EditorObject*>` copies are shallow either
way), ownership stays B7e. The two multiplayer files take the forced edit at E-end.
BASE for every code batch: the notes' section 5 list + census rows only fall + `pch_audit --check` + the CRLF check.
| batch | content | identity proof | save | captures | the extra proof (blind spot it closes) |
|---|---|---|---|---|---|
| B2.0 | tests HE/HS/HP, census, P1-P3 (+R5) | selftests; goldens committed before any class changes | - | - | probe tables in notes 8b |
| B2.1 | BitFlag deletion (+USE_OLD_LOS) | `objhash compare` IDENTICAL/LAYOUT-ONLY on every non-deleted TU of all targets (stronger than SIMID: proves the mclib.h include removal changed no overload) | - | - | objhash |
| B2.2a/b | pointer lists + getters; then deques | `check` x4 identical | campaign round trip (G2: `old_a`/`var_a` load + re-save, logistics text identical, 120 pilot records 0-2167) | mechbay set at frame 600, list order viewed | capture run with the soak salvage phase on (mission_update.cpp:740-900) so `getBestPilot`/`tradeMover`/`addMechToInventory` run on the new lists; ASan mechbay run |
| B2.2c (optional) | logistics owners (`unique_ptr`, D12) | as B2.2 | as B2.2 | - | `own_inventory` rows fall with the reason |
| B2.3 | objectives | `check` x4 + camera-park identical; P3 trace identical | mission save frame 2400 mc2_01/mc2_16 `--mask --across-builds`, objective text packet byte-identical | HUD objective list, tac-map markers | `BOOT_OBJ_COMPLETE` run to the result screen |
| B2.4-pre | span API, buffers to MAX_MOVERS, static scratch, dead consumers | `check` x4 identical (P1 shows max n <= 200 on stock) | - | - | doctests of the clamp + log and of the span writer under ASan |
| B2.4 | `score_sort.h`, SortList deleted | patched-old equivalence (section 3), then 12 mc2_24 pairs `--duration 60` + camera-park on the new exe | quickload run (a reload rebuilds the sensor lists) | one combat pair viewed | P1 per-site table |
| B2.5-pre | H18 fix | P2 hits = 0: `check` x4 identical; else patched-old equivalence + ruled re-baseline | - | - | P2 hit count |
| B2.5 | open list per solver, no global | `check` x4 identical vs the current baselines; P2 per-solve hashes identical in both `MC2_PATH_SOLVE_ISOLATED` positions | masked mission save mc2_24 frame 2400 | - | P2 (the save mask hides `stepList`, so P2 is the only path-CONTENT proof); goal-map solves counted and covered (liveness fixture or HP6); tier2 once; Tracy `Move.*` |
| E1-E4 | editor | `run_editor_smoke` 15/15 | E0 ledger `-mission`/`-smoke-save` `.fit` old vs new editor byte-identical (container-only) | - | dialogs compile-only, listed |
What the digest cannot see is as in notes section 6, plus: weapon ORDER (only its effect via hp/RNG counts), path content (counts only), contact
lists no brain consumed, objective status (P3), anything after the window. Saved containers: objectives (mission text packet), variants,
inventory, pilots (LogisticsData text packet), `groups[].infos`/`additionalPurchaseFiles` (campaign save). SortList and the heap are not saved;
their outputs are (targets, `MovePath::stepList`, masked).

## 7. Static A* open list (Q11, task item 12)
A value global with `releaseStorage()` is a static-duration owner under a new name: rejected. B2.5 makes the open list a member of the object that
runs the solve: `GlobalMap` and `MoveMap` (Mission-lifetime objects created at mission init; who owns THEM is B7/PathSystem), and a local value
heap in `MoverGroup::calcMoveGoals/calcJumpGoals` (player orders only, capacity `GOALMAP_DIM^2` = 3721). This needs no new Mission member, does
not contradict the sim-kernel's PathSystem (it moves the heap one step toward it), and deletes `::openList`, its 4 lazy inits and the mission.cpp
delete. It is behaviour-preserving ONLY after H18: every other read stays within `[0, numItems]`, so per-solver heaps are equivalent once H18 is
fixed. `SolveContext::ownPQ` becomes a value heap; with per-solver heaps the isolated gate no longer changes the open list, so P2's ON==OFF
result is the evidence for retiring `MC2_PATH_SOLVE_ISOLATED` (gate amnesty board, not B2).

## 8. DynamicArrayOf/StaticArrayOf/MemoryBlock (Q1)
Deferral is RIGHT. A storage swap behind `marray.hpp` zero-fills where `new T[n]` did not (MLR/gosFX outputs that read uninitialised slots would
change), changes `sizeof` of every holder, edits code the owner ruled out of ownership work (MLR, 9.3) and buys nothing if MLR retires. Correct
the reason: not every user is MLR. `mc2::particles::SpecLibrary::m_effects` (spec_library.h:45, the B8 owner of specs) and gosFX
(`tube.cpp:1435` `static Stuff::DynamicArrayOf<float> s_worldPos`, a static-duration owner) survive MLR: name both in B8's list (R12).

## 9. Sign/width and PCH
`Count()` is 32-bit unsigned, `size()` 64-bit: D5. Keep `PQNode`'s exact `int32_t key`/`uint32_t id` and `find(unsigned)`; `change(int, int)`
receives a `long fPrime` today: keep the narrowing explicit. `valueList[i] = getValue(i)` is a float->long truncation: gone with `valueList`.
`Scored::id` is `int32_t` (handles, weapon indices, -1). PCH: H22/D16; new headers are NOT added to `mclib/mclib.h` (that would put `<cmath>`
and `<algorithm>` into every TU that includes it).

## 10. KPI (honest)
LG03 -26 of 2,397 (1.1 %), plus the CObjective delete sites if D12 lands (counted, not promised); idiom total about -78 at E-end. B2's value is not
the ratchet: hand-written containers 5 -> 0, static sort singletons 5 -> 0, static scratch arrays 2 -> 0 (plus `::openList`), UB/corruption
sites 6 -> 0 (H7 x2, H8 x2, `wcl[8]`, H18), A* capacity tied to the graph, and a defined, portable sort order. Report those on the board.

## 11. Answers to the notes' questions
1. Defer (section 8), with the corrected reason and the two B8 names.
2. Delete the class and the statics for `score_sort.h` (free `sort_scored(std::span<Scored>, SortOrder)` + a fixed-capacity local buffer sized to
   the real bound: `MAX_MOVERS` for contacts and movers, the mover weapon maximum for weapons; not 4000 slots on the stack). `calcOptimalRange`
   uses no sort. (score, slot ascending) in BOTH directions is right (it is "stable"); NaN last is acceptable.
3. Re-baseline, via the patched-old equivalence (section 3); reproducing MSVC is ruled out by the owner, not by impossibility. Coverage: section 3.
4. Keep the heap rules (section 4).
5. `std::deque` with a structural append-only API now; the ID refactor in B6 (it does not touch save bytes: the save writes the pilot's file name).
6. Objectives: `std::vector<std::unique_ptr<T>>` in B2.3 with an explicit `clone()` for `CastAndCopy`. `variants`/`inventory`: a SEPARATE commit
   B2.2c after B2.2a/b are proven, never in the same diff as the container swap. `additionalPurchaseFiles` -> `std::vector<std::string>` only with
   `getAllPurchaseFiles` returning strings by value (a `c_str()` into a growable vector of strings dangles on relocation, SSO included).
7. Yes; yes for E2-E4, E1 after B1.6; yes.
8. Section 5.
9. Accept H5 and H7 as ruling-5 behaviour with a log line, listed per commit. H8 needs no clamp (buffers sized to the bound). H3 is not a change.
10. Objectives first (section 6).
11. Section 7: no value global; per-solver members after H18.

## 12. Required changes to the notes (concrete edits)
- R1 Replace the "FINDING THAT CHANGES THE SCOPE" paragraph with section 1 (logistics pilot list read by `tradeMover`, the three ABL builtins,
  the static scratch, the Fatal-returns fact) and add `MoverGroup::handleTacticalOrder` to the census.
- R2 H3: rewrite as "no absent case exists; `std::erase` on a known member". H7, H8: rewrite as in section 2. Add H18-H22.
- R3 Batches: add B2.4-pre (H7, H8, H19, `wcl[8]`, dead sort consumers; SIMID-identical) and B2.5-pre (H18); B2.4 becomes the ruleset-only
  commit; B2.2 splits a/b(/c); add `brain_special_dispatch.cpp`, `ablxstd_stack.cpp`, `abl.h` to files.json; fix 64 vs 66.
- R4 Section 5 ladder: replace steps 2-3 by the patched-old equivalence; pairs at `--duration 60`; P1 per-site fields as in section 3.
- R5 P2 adds: hits of `propogateCost`'s open branch (`door != openIndex`), `find() == 0` events, max index passed to `change`, solves per solver
  (goal map included), max open size per solver.
- R6 B2.5: per-solver open lists (section 7) and graph-sized capacity; drop "grows instead of corrupting" and the "named exception".
- R7 B2.1: objhash proof; `USE_OLD_LOS` and the commented terrain blocks deleted; `own_inventory`/ratchet rows rewritten with the reason.
- R8 Proof table: replace with section 6's.
- R9 New scripts (census, probes) go under `tests/analyze/` (lane 1), or `check_conflict` before `tools/` (lane 2).
- R10 Notes section 7 gets the per-commit unlisted-behaviour list (RA1 F3 practice): H5, H7, H21, the prefix-only sort.
- R11 Orchestrator: tell lane 2 and the Codex sim-kernel track before B2.4 that the baselines are re-made once (the design's step-1 freeze comes
  after B2.4, or B2.4 is its first reviewed ruleset change).
- R12 The B8 list (ownership plan section 6) names `SpecLibrary::m_effects` and gosFX `s_worldPos`.

## 13. Conversion rules for the lane brief (replace notes D1-D13)
- D1 Pointer and trivial elements: `std::vector`. By-value elements whose address escapes: `std::deque` behind an append-only API (no erase but
  `clear`, which states its precondition). Each list gets a one-line contract: order, who erases, address stability.
- D2 No class derives from a std container; EList-derived classes become composition with the narrow API the call sites use.
- D3 Range-for keeps the exact loop shape: every `break`, `continue` and non-short-circuit `|=` stays. No `any_of`/`all_of`/`find_if`/`||` over
  bodies with side effects (objective `Status`, `Execute`, `StatusChanged*`). Erase-while-iterating: `std::erase`/`std::erase_if`.
- D4 Any index or length from a save, a mod file or ABL goes through a checked accessor or a `std::span` with its real size: out of range = one log
  line + reject or clamp, never UB. ABL array parameters use `ABLi_popIntegerBuf()` (a span with the declared element count).
- D5 Sizes: `std::ssize` in comparisons, explicit casts at `long` targets, no warning suppression.
- D6 Snapshot getters return `std::vector<T*>` by value; nothing hands out `c_str()` or element pointers from a container that can grow.
- D7 No static-duration owner or scratch: sort buffers are locals sized to the domain bound (`MAX_MOVERS`, weapon maximum, 5); the open list is a
  member of the solving object.
- D8 `mc2::sort_scored(std::span<Scored>, SortOrder)`: key (is_nan, directed score, fill slot ascending); `isnan` tested on both operands first;
  -0 == +0; documented in the header and at each call site. `calcOptimalRange` is an argmax + tie mask, no sort.
- D9 The open list keeps the 1-based heap, the three sift rules and the linear first-match `find`; no sentinel, explicit bounds; capacity = graph
  size, reserved once; `findByKey`/`getItem` deleted; never `std::priority_queue`.
- D10 Dead code leaves with its container after the section 5 bar; tag first.
- D11 A header changes in the same commit as every includer the compiler reports; new headers are never added to `mclib/mclib.h`.
- D12 Sole-owner lists (objectives) become `std::vector<std::unique_ptr<T>>` with an explicit `clone()`; logistics owners only in B2.2c.
- D13 CRLF preserved (`git ls-files --eol`); comments WHAT+WHY; touched regions lose `file:NNN` refs.
- D14 Behaviour changes on malformed input are listed in each commit (ruling 5); a ruleset change (B2.4; B2.5-pre if hit) is its own commit with
  nothing else in it and is proven by the patched-old equivalence.
- D15 Destructor order that is observable is made explicit (HE12 pins head to tail).
- D16 Before a batch adds a standard header to a TU on `cmake/pch/pch_skip.cmake` (or to a header such a TU includes), an include-only commit is
  objhash-compared: IDENTICAL, or the overload-sensitive call is fixed explicitly first, in its own commit.

## 14. First batch to code, and owner questions
B2.0 first. Conditions: R1-R5 and R8-R10 applied to the notes; goldens (HS2 tie table, HP6 hashes) generated by the OLD classes and committed
before any class edit; the census sealed; P1 (section 3 fields) and P2 (R5 fields) run on the four baseline windows and one long mc2_24 run, results
in notes 8b. B2.1 may follow at once (objhash proof). B2.4-pre and B2.5-pre need the P1/P2 numbers.
Owner questions: none. Every point is decidable under the rulings (explicit total order, determinism required, ruling 5, no static owners, MLR out).
