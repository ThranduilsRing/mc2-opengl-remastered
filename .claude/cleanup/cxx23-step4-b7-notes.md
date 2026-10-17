<!-- doc-status: current | reviewed:2026-10-17 -->
# C++23 step 4, batch B7 (actor instance ownership): batch notes (lane 1, 2026-10-17 fourteenth session; NOTES AND CENSUS ONLY, no code; before-review questions in `cxx23-step4-b7-before-review.md`)

WHAT: scope, sealed census, conversion rules (G-series, in the style of the B5 F-series), batch order and proof for B7, the batch the B4/B5 notes left everything "instance-shaped" to: the
appearance an actor owns, the pooled object types and managers, the raw TG node pointers that are copied by value. B7 here is the OWNERSHIP half of the plan's B7; the save-schema half
(`cxx23-step4-save-schema-design.md` B7.0-B7.7) keeps its own numbering and is not touched. The batches below are named B7.1-B7.5 in this file only (open question 3 in section 8 asks for a rename).
WHY: after B5 the shared assets (shapes, appearance TYPES, textures) have owners, but the per-actor appearance INSTANCE is still `new` in one function and `delete` in another, the
replacement paths in `init` overwrite the pointer without freeing the old one, and `GameObject::appearance` is a bare pointer read at 587 sites. A leak there was invisible while the
heap bulk-released it; B5.5 removed the heap (H2 in the B5 notes), so every one of these is now a real process leak or a latent double free.

## 1. Scope and exclusions
In (the items named across the B4/B5 notes):
1. Actor-owned `appearance` instances: the eight `delete appearance` sites (`bldng`, `gate`, `gvehicl`, `mech`, `mech_init`, `mover`, `terrobj`, `turret`) and the `new <X>Appearance` that pair with them.
2. The init replacement paths that do not free the old appearance: `bldng.cpp` ~1263, `gate.cpp` ~702, `turret.cpp` ~2137, `terrobj.cpp` ~1171/~1199, `gvehicl.cpp` ~695.
3. Non-actor owners of an appearance instance: `Artillery::appearance` (and the three strike `new BldgAppearance` in `artlry.cpp`), `MechWarrior` waypoint markers (`warrior_state.cpp`), `MissionInterfaceManager::vTol[]` (`missiongui*.cpp`; its raw deletes are the s10 cxx17 LG03 +2 to retire), `GameCamera` sky/compass, `SimpleCamera::pObject`.
4. The ended-artillery-round lifetime (G8).
5. `ObjectType::operator new/delete` (routes to `objectTypeCache`, guarded by `inUse()`), and with it `GameObject::operator new/delete` and the 37 `objectCache->Malloc/Free` pool-array rows, because `objectCache` can only be deleted when its last user goes.
6. `GameObjectManager::objTypeManager` as a raw owning pointer (two deletes and one new in `objmgr.cpp`; tests assign borrowed objects).
7. `TG_ShapeRec::node` and the `typeNodeStore` entries: raw `new`/`delete` (B5 LG03 +3), `TG_ShapeRec` copied by value in `Detach` and the compaction loop.
8. Per-instance `nodeUsed` / `nodeRecycle` / `paintSchemata` leftovers: ALREADY DONE in B5.5a (vectors; paintSchemata deleted). B7 only re-verifies (zero raw rows today: the census has no kind for them because none is left) and guards by H3.
Out (each with its owner):
- Appearance TYPES, TG types and textures: B5 (done). The `numUsers` unload stays.
- Save schema, archive, S3 batches: `cxx23-step4-save-schema-design.md`. B7 changes no saved byte.
- `SensorManager` / `SensorSystem*` `missionHeap` classes (never destroyed; s13 added the ABL ledger and documented the null-heap guards as exit containment): they own no appearance. A mission-teardown item, a finding, not scheduled here.
- ABL blocks (B8); the `MoveMap`/`GlobalMap` class operators and path pools (plan B7, path system); `ArtilleryChunk`/`WeaponFireChunk` and the other `*Chunk::operator new` (multiplayer packets).
- Editor (`editor/`): never edited by B7; a header change is proved by `tools/build_all_targets.py`, a forced edit is named in the commit.

## 2. Inventory: who owns what today
| thing | creator | releaser | note |
|---|---|---|---|
| `GameObject::appearance` (`AppearancePtr`, `gameobj.h:327`) | `new <X>Appearance` in the actor's `init(create, type)` / constructor path | the actor's `destroy()` (`delete appearance; appearance = nullptr`); `Mech` retires its RenderWorld handle first (`mech.cpp` ~507-519) | 587 `appearance->` reads in 38 files, about 43 non-arrow uses (casts, arguments) |
| replacement paths | `Building::init`, `Gate::init`, `Turret::init`, `TerrainObject::init` x2, `GroundVehicle::init` assign over the member | none | never re-init'ed today (the constructor nulls the member): no leak today, one re-init away from one |
| `Artillery::appearance` | three strike variants in `artlry.cpp` | `destroy()`; `init()` calls `destroy()` first (s10 fix) | pooled rounds are re-init'ed per strike |
| `MechWarrior` waypoint marker | `getWayPointMarker` | `clearTacOrderQueue` (s10 fix) | returns a raw `BldgAppearance*` the queue slot keeps |
| `vTol[]` | `missiongui_input.cpp` (2), `missiongui_support.cpp` (2) | `missiongui.cpp:411`, `missiongui_style.cpp` 37 and 174 | raw `delete`s: the cxx17 LG03 +2 |
| `ObjectType` | `ObjectTypeManager::load` | `typeStore` (`unique_ptr`) via `destroy()`; retired types via `retiredTypes` | `operator delete` frees only when `!inUse()` |
| `GameObject` (every actor) | `ObjectManager` pools at `setNumObjects` | per-type pool teardown; recycled, never freed mid-mission | `operator new` zero-fills: the save copies whole member arrays |
| `objTypeManager` | `new ObjectTypeManager` (`objmgr.cpp:178`) | `delete` at 176 and 1051 | tests set it to a stack object or a test-owned pointer |
| `TG_ShapeRec::node` | `TG_TypeShape::CreateFrom` (`new TG_Shape()` x2 in `tgl_types.cpp`) | `msl_load.cpp:409` `delete listOfShapes[i].node` | `shapeRecStore` is a `std::vector<TG_ShapeRec>`, filled by copy |
| `typeNodeStore` | `new TG_TypeShape` / `new TG_TypeNode` (`msl_load.cpp` x5) | `delete listOfTypeShapes[i]` (x2) | `listOfTypeShapes` is a raw view of the vector |

## 3. The census (B7.0)
Command (sealed, re-runnable): `py -3 tests/analyze/b7_actor_census.py` (summary), `--check` (exit 1 when the tree differs from the list), `--write` (a batch commit rewrites it by intent).
List: `.claude/cleanup/cxx23-step4-b7-callsites.txt`: sub-batch, file:line, kind, source text. Text based, comments and strings skipped, scanning tracked `code/`, `mclib/`, `gui/` and `tests/unit/` (the last only for `objTypeManager`).
**Count at the seal (tip 643b44bfd plus the B7.0 commit): 96 sites in 27 files.**
| sub-batch | sites | files | kinds |
|---|---|---|---|
| B7.1 actor appearance | 16 | 9 | `app_delete` 8, `app_new` 8 (`light.cpp` is under `USE_LIGHT_APPEARANCE`, dead) |
| B7.2 non-actor owners | 19 | 8 | `app_delete` 4, `app_new` 15 (artillery, camera, vTol, warrior marker, missiongui_support) |
| B7.3 shape nodes | 10 | 2 | `shape_node` 10 |
| B7.4 type manager | 10 | 3 | `objtypemgr` 10 (5 in `objmgr.cpp`, 5 in two unit tests) |
| B7.5 objectCache | 41 | 7 | `objcache_heap` 37, `objcache_op` 2, `objtype_op` 2 |
Not counted, by design: the 587 `appearance->` readers (unchanged under G1, which keeps the raw view); `msl.cpp:76` (`.node = CreateFrom()`, goes with B7.3); the `GameCamera` destroy path (a virtual `destroy`, not a `delete` the regex sees); `SimpleCamera` `delete pObject` (not named `appearance`; its sites are in the B7.2 file list by hand).

## 4. Conversion rules (G-series; paste into the lane brief)
- G1 **Owner model for actors: an owner member beside the legacy view.** `GameObject` gets `std::unique_ptr<Appearance> appearanceOwner` and keeps `AppearancePtr appearance` as the non-owning view the 587 readers use. One helper `GameObject::adoptAppearance(Appearance*)` sets both (and frees the previous owner, which closes the five replacement paths for free); `releaseAppearance()` resets both. The raw view STAYS: it is the F7 pattern (users hold non-owning pointers valid for the object's life), and rewriting 587 reads gives no ownership gain. A `static_assert(sizeof(std::unique_ptr<Appearance>) == sizeof(Appearance*))` pins the default deleter.
- G2 **Lifecycle stays `destroy()`-driven, not destructor-driven.** Pool objects are re-init'ed and never destructed mid-mission, and `Mech::destroy` has an order contract (retire the RenderWorld handle BEFORE the appearance goes). `destroy()` keeps the same steps in the same order and ends in `releaseAppearance()`. The `unique_ptr` destructor is a backstop for objects that reach `delete` without `destroy()`; it must never run `Appearance::destroy` twice (a doctest on a fake appearance with a destroy counter).
- G3 **Teardown order is written beside the member (E2/F8).** Actor appearances are released before `AppearanceTypeList::destroy()` (the appearance reads its type); a doctest pins it.
- G4 **Zero-fill stays.** `GameObject::operator new` memsets the object because saves copy whole member arrays and heap garbage would make the save a function of history. The operator pair is kept (re-routed from `objectCache` to the CRT only in B7.5), and is NOT replaced by `make_unique<T>()`, which does not zero padding or members a constructor skips.
- G5 **Non-actor owners get a plain owner member and no view.** `Artillery` is a `GameObject` (G1 applies); the others are plain members: `std::array<std::unique_ptr<BldgAppearance>, N> vTol`, the waypoint marker as `std::unique_ptr<BldgAppearance>` in the queue slot (the slot moves, never copies), `GameCamera::theSky/compass` and `SimpleCamera::pObject` as `unique_ptr`. The `delete vTol[i]` sites become `.reset()`: this retires the s10 cxx17 LG03 +2.
- G6 **ObjectType.** `ObjectTypeManager` already owns types by `unique_ptr` (`typeStore`, `retiredTypes`). The `inUse()` guard in `ObjectType::operator delete` keeps a type that a live object still uses from being freed: it becomes an explicit rule (a type is deleted only by `destroy()`, retired types included), written beside `retiredTypes`, and the operator pair is deleted last (B7.5) with `objectTypeCache`.
- G7 **objTypeManager keeps a raw VIEW and gains an owner.** `GameObjectManager::objTypeManager` stays `ObjectTypeManager*` (tests assign borrowed objects); a new `std::unique_ptr<ObjectTypeManager> ownedTypeManager` is what `init` fills and `destroy` resets, and the view is set from it. The tests keep assigning the view and never touch the owner: no test edits.
- G8 **Ended artillery round.** A round that has finished its strike keeps its appearance until `init` (re-use) or `destroy`. B7 changes ONLY who frees it. Whether an ended round should release eagerly (render-visible: shadow, trail) is not decided here: default no behaviour change, the question is in the before-review. The `S10_STRIKE=1` reload-delta must be run with a round PROVEN created (the s10 fixture did not confirm one): the proof adds a log-line count.
- G9 **TG node: move before flip.** `TG_ShapeRec` is copy-filled (`assign(n, TG_ShapeRec{})`) and copied in `Detach` and the compaction loop (`msl_shape.cpp` ~438-481). B7.3a replaces those copies with `resize(n)` and explicit moves of the node pointer, behaviour identical. B7.3b changes `node` to `std::unique_ptr<TG_Shape>` and `typeNodeStore` to `std::vector<std::unique_ptr<TG_TypeNode>>` (virtual destructor on `TG_TypeNode` verified first); `listOfTypeShapes` stays a raw view refreshed wherever the vector is resized (H3).
- G10 **No saved byte changes.** `Mech3DAppearance::copyTo/copyFrom` (`apData`; only the first 10 BYTES of each table, flagged in B5), `appearanceTypeID` text and `objTypeNum` are the saved appearance/type state. B7.1-B7.4 leave them. `save_layout_guard.h` pins saved packet structs, not `GameObject`, so growing `GameObject` by 8 bytes is allowed, but `sizeof` of every pooled class is printed before and after in the commit.
- G11 **Include-only first (F10)** for `<memory>` in `gameobj.h`, objhash-compared. **Forced edits (F11)**: `tools/build_all_targets.py` after every header change; editor and tool edits are listed, never scope.
- G12 **Behaviour that changes goes in the commit and the notes (F12)**: every appearance freed where it was leaked is a named line. No new `STOP`/assert as a check; a malformed state logs one line and continues.

## 5. Batches (cap 25 EDITED files per commit) and proof
| batch | content | files | proof per commit |
|---|---|---|---|
| B7.0 | this file, the sealed census, before-review | 4 | none (no code) |
| B7.1a | include-only: `<memory>` in `gameobj.h`; doctest of adopt/release on a fake appearance; no callers | 2-3 | build, doctest, objhash IDENTICAL for the TUs that include `gameobj.h` (except the new test), `build_all_targets --root-only -j2` |
| B7.1b | `appearanceOwner`, `adoptAppearance`, `releaseAppearance`; the eight `delete appearance` sites call `releaseAppearance` (order unchanged); the `new` sites call `adoptAppearance` | ~11 | build, doctest + ASan doctest, `sim_determinism check --sim-speed 4` mc2_03/10/16/24 (60 s), saveload `check --mask --first-save-only` x5, reload-delta `S10_TRACK=1` (appearance survivors stay 0), tier1 5/5 |
| B7.1c | the five replacement paths free the old appearance through `adoptAppearance` (no behaviour change today: documented); the dead `light.cpp` path follows | ~7 | as B7.1b minus saveload |
| B7.2 | non-actor owners (G5): artillery with the G8 decision, warrior marker, `vTol[]`, camera sky/compass, `pObject` | ~9 | as B7.1b plus the `S10_STRIKE=1` reload-delta with a round proven created, GL pair mc2_01 day / mc2_10 night (VTOL and strike are visible) |
| B7.3a | `TG_ShapeRec` copies become moves / `resize` | 3 | build, doctest, sim pair, objhash where the TU shape allows |
| B7.3b | `node` and `typeNodeStore` to `unique_ptr`; cxx17 LG03 -3 | 4 | as B7.1b plus reload-delta (shape survivors 0) |
| B7.4 | `ownedTypeManager` (G7) | 2 | build, doctest (both unit tests unedited), sim pair, saveload x5 (object type ids are saved) |
| B7.5 | `ObjectType` / `GameObject` operators and the 37 pool-array rows leave `objectCache`/`objectTypeCache`; heaps deleted | ~7 | `build_all_targets`, sim x4, saveload x5, tier1, reload-delta, ASan doctest; BLOCKED until B7.1-B7.4 land (a pooled actor must own its appearance before its heap goes) |
Each batch's last commit: tier1, ASan doctest, the campaign round trip once. Order is fixed: 1, 2, 3, 4, 5. The render proof (GL pair viewed) is the same assignment as B5; if withdrawn, B7.2 stops at the build. Every batch is under 25 edited files; B7.1b is the largest.

## 6. Hazards (each batch checks its own)
- H1 **Save format.** Object type ids (`objTypeNum`), `appearanceTypeID` and Mech3D `apData` are saved; B7.1-B7.4 may not change their bytes. Gate: saveload `check --mask --first-save-only` x5 per touching batch.
- H2 **Determinism.** Frees are not sim-visible, but freeing the old appearance on re-init (B7.1c) and any eager release (G8) change `Appearance` side effects (RenderWorld handles, static-prop registration). The sim pair is the check; any SIMID divergence stops the batch.
- H3 **Views.** `appearance` (raw view), `listOfTypeShapes`, `listOfShapes` point into owners; sized once, refreshed at the single resize point.
- H4 **Double destroy and destroy order.** The `unique_ptr` backstop must not run on an appearance already destroyed by `destroy()` (G2), and `Mech`'s handle retirement precedes it.
- H5 **Zero-fill** (G4) must survive: a poisoned-heap doctest allocates a derived object and reads all bytes.
- H6 **Teardown order** (G3): appearances before the type list, objects before `ObjectTypeManager::destroy`; the process-exit path (null `missionHeap` guards, s13) is unaffected.
- H7 **Editor** builds the same classes: `build_all_targets` plus `scripts/run_editor_smoke.py` when `gameobj.h`, `objmgr.h` or `msl.h` change.
- H8 **cxx17 ratchet**: LG03 only down (B7.2 -2, B7.3b -3, the rest by intent); baselines rewritten in the same commit with the reason.

## 7. KPI expectation
Sealed census 96 sites. Expected: `app_delete` 12 -> 0 (owner destructors are not counted), `objtypemgr` 10 -> 4 (the test assignments remain, by G7), `shape_node` 10 -> 0, `objcache_*`/`objtype_op` 41 -> 0 at B7.5. `own_inventory` appearance counts down; cxx17 LG03 -5 at least.

## 8. Open (orchestrator)
1. G8: do ended artillery rounds release their appearance eagerly? (default: no, ownership only).
2. Render-proof assignment for B7.2 (as B5).
3. The save-schema design already uses B7.0-B7.7: rename these batches (for example B7o.1-5) before the first code commit?
4. `SensorManager` teardown (never destroyed in `Mission::destroy`): its own item or left to the mission-teardown arc.
