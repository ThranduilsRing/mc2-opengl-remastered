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
