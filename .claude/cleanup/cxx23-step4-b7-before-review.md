<!-- doc-status: current | reviewed:2026-10-17 -->
# B7 (actor instance ownership): before-review questions (adversarial reviewer; read `cxx23-step4-b7-notes.md` first)

Read-only. Find what is wrong with the plan before any code is written. Sealed census: `py -3 tests/analyze/b7_actor_census.py --check` (96 sites / 27 files).

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
11. **Batching.** Is B7.5 really blocked on B7.1-B7.4, or can the 37 pool-array rows (pointer arrays, no ownership) go first? Is each batch under 25 edited files once the non-arrow `appearance` uses are counted?
12. **Name clash.** The save-schema design already has B7.0-B7.7. Rename these batches now?
