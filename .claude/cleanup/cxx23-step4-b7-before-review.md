<!-- doc-status: current | reviewed:2026-10-17 -->
# B7 (actor instance ownership): before-review questions (adversarial reviewer; read `cxx23-step4-b7-notes.md` first)

Read-only. Find what is wrong with the plan before any code is written. Sealed census: `py -3 tests/analyze/b7_actor_census.py --check` (157 sites / 33 files after rev 2; the questions below are the round 1 set and are answered in "Round 2: resolutions").

1. **G1 raw view next to the owner.** Is there any path where the view and the owner diverge (a subclass assigning `appearance` directly, casts that copy the pointer, `getAppearance()` callers that stash it past `destroy()`)? Grep `appearance =` and `getAppearance` for writers the census regex misses.
2. **G2 destroy order.** `Mech::destroy` retires the RenderWorld handle before `delete appearance`. Do the other seven owners have a hidden order (effects, static-prop late-spawn registration) that `releaseAppearance()` could reorder? Does any `Appearance` destructor or `destroy()` touch its owner after the owner nulled it?
3. **Backstop destructor.** Which objects reach `delete` without `destroy()` (pool teardown, `ObjectManager` destroy, the editor's object manager)? Would the new destructor free an appearance whose TYPE was already destroyed (`AppearanceTypeList::destroy()` earlier in `Mission::destroy`)?
4. **G4 zero-fill.** Does anything rely on `GameObject::operator new` zero-fill beyond the save member arrays (flags read before `init`)? Does an extra `unique_ptr` member change the memset/constructor interplay for classes that `memcpy` themselves?
5. **Save bytes.** Does any save writer copy a `GameObject`-derived struct whole (so 8 more bytes change a packet) instead of field by field? Is any `sizeof` pinned by `save_layout_guard.h` a `GameObject` subclass?
6. **G7 type manager.** Is owner-plus-view smaller than a `unique_ptr` with a test-side `release()`? Does any production code besides `objmgr.cpp` assign `objTypeManager`?
7. **G6 `inUse()` guard.** What does `ObjectType::inUse()` mean, and does anything depend on `operator delete` NOT freeing an in-use type (the s7 forced-reload retirement)? Is the explicit rule in the notes equivalent?
8. **G9 TG node.** Can `TG_ShapeRec` become move-only without breaking code that copies a whole `std::vector<TG_ShapeRec>` (`Detach`, compaction, `CreateFrom`, the GPU batcher)? Does `TG_TypeNode` have a virtual destructor so a `unique_ptr<TG_TypeNode>` deletes a `TG_TypeShape` correctly?
9. **G8 artillery.** Is "ended rounds keep their appearance until re-init" true? Where does a round stop rendering? Could eager release ever be required (pool of N rounds, long mission)?
10. **Census blind spots.** Which owners of an appearance or TG node does `b7_actor_census.py` not see (factories returning raw pointers, `std::vector<Appearance*>`, `mclib/` mover subclasses, the editor)? Is any `new <X>Appearance` outside `code/`, `mclib/`, `gui/` reachable in the game exe?
11. **Batching.** Is the old B7.5 really blocked on B7.1-B7.4, or can the 37 pool-array rows (pointer arrays, no ownership) go first? Is each batch under 25 edited files once the non-arrow `appearance` uses are counted?
12. **Name clash.** The save-schema design already has B7.0-B7.7. Rename these batches now?

## Round 1 verdict (adversarial reviewer, 2026-10-17): NO-GO as written
4 blockers (G5 queue markers, TG polymorphic deletion, raw-view invariant, pool-array lifetime) and SHOULD items. Full text kept in the session scratchpad; the 12 answers are mapped below.

## Round 2: resolutions
Batch names are now `B7o.*` (notes section 5). Section numbers refer to `cxx23-step4-b7-notes.md` rev 2.

| round 1 finding | resolution | where |
|---|---|---|
| BLOCKER G5 queue ownership (copy duplicates ownership, staging null deletes the live marker, restore overwrites) | wire record `QueuedTacOrder` unchanged; owners in a parallel `queueMarkers` array of `unique_ptr`; enqueue/shift/clear/save/restore/destroy flows specified, restore resets before rebuild; census kinds `marker_*` (23 rows); batch B7o.4 with a save/load round-trip doctest | G5, section 5 B7o.4, H9 |
| BLOCKER TG polymorphic deletion (no virtual `~TG_TypeNode`, explicit `destroy()` vs derived cleanup) | prerequisite batch B7o.1: virtual destructor, `destroy()` reconciled and idempotent, derived-destructor-count doctest plus objhash/sim identity | G9(a), B7o.1 |
| BLOCKER raw-view invariant (`GameObject::set` copy, null writers, `Mover::set` by value) | full writer list; batch B7o.2 seals all through `adoptAppearance`/`releaseAppearance` BEFORE ownership (B7o.3); census extended with `app_write`; proof is raw writers == 0 (now 23 outside the API) | G1, section 3, B7o.2 |
| BLOCKER pool arrays (no owner/release; teardown only nulls the array pointers) | split the 37 rows by kind (2 actor, 6 sub-area, 2 text, 27 pointer arrays); owner and release named for each; `destroy()` gains the explicit release; batches B7o.8a/B7o.8b/B7o.9 | G13, section 3, section 5 |
| G1 (appearance writers, deleted copy ctor) | as the raw-view blocker; `set` signature change or deletion; open item 3 | G1, section 8 |
| G2 release order | release at the SAME position per class (Mech handle in destroy and replacement; Mover before contacts; TerrainObject buffers first; artillery effects first); order doctest | G2 |
| Backstop | pure backstop, teardown order cited, repeated-teardown doctest | G3 |
| G4 rationale (SHOULD) | zero-fill also serves conditional init; `make_unique` still calls the class operator new | G4 |
| Save bytes | `*Data` records separate, guards pin them; queue record handled by G5 | G10 |
| G7 | accepted (two members); `init()` sync made explicit | G7 |
| G6 inUse (SHOULD) | forced-reload retirement and zero-user removal stay; only the allocator changes (B7o.9) | G6 |
| G9 transfer/reset, view array, parents (SHOULD) | `transferFrom` with the same resets, separate raw view vector, parent rebuild in Detach and compaction, doctests | G9(b)(c)(d), B7o.6a/6b |
| G8 | retention confirmed | G8 |
| Census blind spots | new kinds `app_write`, `app_write_other`, `app_copy_api`, extended `app_delete` (bomber, sky, compass, pObject), `marker_*`, split `objcache_*`; editor-only allocations documented as outside the game exe | section 3, section 1 |
| Batching (SHOULD: buffer migration first, 37 rows not all arrays, merge old 1b/1c, 25-file cap) | arrays split by kind; old B7.1b+1c merged into B7o.3; every batch re-checked (3 to 17 files) | section 5 |
| Scope corrections (SHOULD): sky global, bomber included, waypoint storage static, SensorManager teardown exists | each corrected in the text | section 1, G5, section 2 |
| Test plan (SHOULD) | destructor and type-release counts, queue round trip, detach parent validity, repeated teardown are the per-batch proofs | section 5 proof column |
| Naming | `B7o.*` everywhere | header, section 5 |

Round 2 questions for the next reviewer: (1) is `Mover::set`/`GameObject::set` truly dead (editor, tools included)? (2) does the `TG_TypeNode::destroy()` idempotency claim hold for every derived class (`TG_TypeShape` only?) and for assimp-imported shapes? (3) does `GameObjectManager::destroy` have any path that reads a pointer array after the owner vector is cleared (the load path allocates the same arrays again)? (4) is anything else byte-copying a struct that carries a raw owner (grep `MemCpy`/`memcpy` of `QueuedTacOrder`-like records)?

## Round 2 verdict (adversarial reviewer, 2026-10-17): NO-GO as written
13 items: 3 NEW BLOCKERS (full-capacity queue overflow, `warrior_draw.cpp` null dereference, TG views on three allocation paths), 3 NOT RESOLVED (G5 queue ownership, raw-view census exemption, buffer release by `clear()`), Q1-Q4 answered, B7o.1 ordering confirmed.

## Round 3: resolutions
Section numbers refer to `cxx23-step4-b7-notes.md` rev 3.

| round 2 item | resolution | where |
|---|---|---|
| NEW BLOCKER 9: full-queue overflow (`queueMarkers[i+1]` at capacity; existing record shift reads slot n) | both shifts run over `i + 1 < oldCount`, then the vacated tail (record zeroed, marker null) is reset; the EXISTING record shift at `warrior_queue.cpp:79` has the same one-past read today and is fixed in the same commit as a named behaviour line; full-capacity dequeue test | G5 flow (2), test (a) |
| NEW BLOCKER 10: `warrior_draw.cpp` reads the permanently null record markers | every reader migrates to `queueMarkers[i].get()`: `warrior_draw.cpp:54` and `:243-246`, plus the writers `warrior_queue.cpp` 33/36/39/74/141 and `warrior_state.cpp` 613/738/743/746/749; `warrior_draw.cpp` added to B7o.4 (8 files); null guard where no marker is created; new census kind `marker_read` with target 0 | G5 readers, B7o.4 |
| 11 queue rebinding | `enableTacOrderQueue()` releases the previous binding's markers (`resetQueueMarkers()`, the one wholesale-clear helper, also used by clear, restore, `init`/`destroy`) and zeroes the new slice; test list: full-capacity dequeue, dequeue-to-empty, looping, re-enable, restore over live markers, save bytes, clear after restore, order/identity after shift | G5 (7) and test list |
| 3 / 1 raw-view census exemption (whole file) | exemption narrowed to the BODIES of `GameObject::adoptAppearance` / `releaseAppearance` (new kind `app_write_api`, brace-matched in `b7_actor_census.py`, self-checked on a synthetic file); `gameobj.cpp` `init` and `set` are raw writers again. Census re-run: 157 sites in 33 files, `--check` PASS, RAW WRITERS = 25 (rev 2 said 23), 0 `app_write_api` rows until B7o.2 | G1, section 3, KPI |
| 4 buffer release (`vector::clear()` keeps storage) | empty-vector swap (or `reset()`) in `destroy()`, every corresponding view nulled; lookup arrays kept alive through ALL actor deletion loops, released after the last; load / `setNumObjects` recreate zeroed storage and refresh views; destroy-then-load and destroy-twice doctests | G13(d), B7o.8b |
| NEW BLOCKER 12: TG views on three allocation paths | `msl_load.cpp` 83 (binary), 226 (imported), 758 (ASE helpers): one `bindTypeNodes(count)` helper synchronises owner and view on all three; abort paths go through the same release; doctest covers all three plus an aborted load | G9(c), B7o.6b |
| 5 Q1 `set` APIs | `GameObject::set` and `Mover::set` deleted TOGETHER in B7o.2, with the `test_part_archive.cpp:39` stub; `mover.h` added to the batch accounting (B7o.2 = 19 files, still under the cap); open item 3 closed | G1, B7o.2 |
| 6 Q2 idempotency | confirmed: `TG_TypeShape` is the only derived class, assimp uses it, `destroy()` swaps stores and nulls handles | G9(a) |
| 7 Q3 / 8 Q4 | Q3 folded into the buffer-release order above; Q4: no other byte-copied owner record, manager `memmove`s copy views only (recorded, no change) | G13(d), G10 |
| 2 / 13 B7o.1 | IMPLEMENTED in the sixteenth session: `virtual ~TG_TypeNode() = default`, explicit `destroy()` before `delete` removed from `msl_load.cpp`, doctest `test_tg_type_node_dtor.cpp`; sim baseline captured BEFORE the change; proof table in `lane1-state.md` SESSION 2026-10-17 sixteenth | B7o.1 row |
