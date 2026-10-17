<!-- doc-status: current | reviewed:2026-10-16 -->
# C++23 step 4, batch B5 (shared assets: textures, TG shapes, appearance types, object types, class-routed heaps): batch notes (lane 1, 2026-10-16; before-review `cxx23-step4-b5-before-review.md`)

WHAT: the scope, sealed census, conversion rules, batch order and proof of B5, the batch after B4 in the ownership plan (`cxx23-step4-ownership-plan.md` section 6: "B5 shared
assets (TG, appearance, ClassData, ObjectType caches) with textures and the class-routed appearance/tgl heaps"; the week plan names it as T7's alternative "one `TextureRef`
behind the two release APIs"). B5 gives every shared asset one owner and one release path, and deletes `tglHeap` and `appearanceHeap` with the classes that route to them.
WHY: these are the objects the plan calls P3 (factories freed by a registry elsewhere) and the texture node references that every UI widget and actor holds as a bare `DWORD`.
Two release APIs with different meanings and 26 call sites (section 5) are where a texture leaks or is freed under a second user; the shapes live in heaps that a mission-end
`destroy()` empties in bulk, which hides every leak (the unequal Malloc / Free pairs of section 2).

## 1. Inventory: who owns what today
| asset | creator | releaser | refcount | heap |
|---|---|---|---|---|
| texture node (`MC_TextureNode`, index `DWORD`) | `loadTexture` (cache hit: `numUsers++`), `textureFromMemory*` (`numUsers = 1`) | `removeTexture(gosHandle)` (finds the node by its GL name, `numUsers--`, destroys at 0), `removeTextureNode(node)` (destroys unconditionally) | `numUsers` | node data in `textureCacheHeap` (B4 class D) |
| TG shape types (`TG_TypeMultiShape`, `TG_TypeShape`, vertex / triangle / animation arrays) | `msl_load.cpp`, `tgl_parse.cpp`, `assimp_importer.cpp` | the owning appearance type, then `tglHeap->destroy()` at mission end | none | `TG_Shape::tglHeap` (created in `code/mission.cpp` 128 MiB and `code/mission2.cpp` 48 MiB) |
| `AppearanceType` family (Mech3D, GV, Bldg, Tree, Generic, VFX) | `AppearanceTypeList::getAppearance` (`apprtype.cpp`) | `AppearanceTypeList::removeAppearance` at `numUsers == 0`, and `~AppearanceTypeList` | `numUsers` (kept, ownership plan DECISION) | `AppearanceTypeList::appearanceHeap` |
| `ObjectType` (ClassData) | `ObjectTypeManager::load` (`code/objtype.cpp`) | `ObjectTypeManager::~` (table walk) | none | `objectCache` (B7) |
Hosts that create and delete the caches: the game (`code/mission.cpp`, `mission2.cpp`) and the editor (`EditorObjectMgr*.cpp`, `Editor.cpp`); the editor rows are the E-batches' and are never edited here.

## 2. The census (B5.0)
Command (sealed, re-runnable): `py -3 tests/analyze/b5_asset_census.py` (summary), `--check` (exit 1 when the tree differs from the list), `--write` (a sub-batch commit rewrites
the list by intent). The list is `.claude/cleanup/cxx23-step4-b5-callsites.txt`: one line per site, sub-batch, file:line, kind, source text. Text based, comments and strings
skipped, scanning tracked `code/`, `mclib/`, `gui/`; the editor, tools and GameOS are not scanned (the editor is the E-batches', tools are rebuilt by `tools/build_all_targets.py`).
**Count at the seal (tip e4ebb4e95 plus the B5.0 docs commit): 400 sites in 70 files.**
| sub-batch | rows | kinds |
|---|---|---|
| B5.1 texture | 125 | `tex_release` 26, `tex_acquire` 99 |
| B5.2 TG shapes | 93 | `heap_malloc` 43, `heap_free` 26, `class_op` 10, `asset_new` 7, `asset_delete` 7 |
| B5.3 appearance types | 143 | `asset_delete` 72, `asset_new` 36, `heap_malloc` 21, `heap_free` 10, `class_op` 4 |
| B5.4 object types | 8 | `asset_delete` 4, `asset_new` 2, `class_op` 2 |
| B5.5 heap owners | 31 | `asset_delete` 31 (the Sensor shapes, `appearanceTypeList`, `objTypeManager`, the heap objects themselves) |
Readings of the unequal pairs: `tglHeap` 43 / 26 and `appearanceHeap` 21 / 10 are released in bulk (`tglHeap->destroy()`, `delete appearanceHeap`); the surplus mallocs are buffers that
no code frees one by one, so a vector conversion adds a destructor path that the heap used to make unnecessary (hazard H2). The 99 `tex_acquire` against 26 `tex_release`: most
acquirers (actors, UI) never release; the node stays referenced until the manager is destroyed (hazard H1). The `asset_delete` filter is by expression name (type, shape, appearance,
texture, heap); an actor's own `delete appearance` instance (B7), other subsystems' heaps and UI icon memory are excluded in the script (`OTHER_BATCH_DELETE`).

## 3. Conversion rules (paste into the lane brief; F-series, in the style of the B3 E-series)
- F1 One release path for a texture node. A holder keeps the node reference in a move-only `mc2::TextureRef` (`mclib/texture_ref.h`); its destructor calls `MC_TextureManager::releaseNode`, which decrements `numUsers` and frees the node at zero. `removeTexture(gosHandle)` and `removeTextureNode(node)` are deleted when their last caller is converted.
- F2 No release is ADDED where today there is none (H1). A holder that never released keeps leaking its reference exactly as before (`TextureRef::release()` hands the node out without a decrement, the reason is written at the call); adding the missing releases is a separate, visually proved decision.
- F3 `get_gosTextureHandle` is never called to find a node to release (it caches a texture into video memory first). The converted site passes the node.
- F4 The refcount stays: `numUsers` is the cache-sharing contract of `loadTexture` (ownership plan DECISION). `TextureRef` copies are deleted; sharing is `loadTexture` again.
- F5 A TG shape or appearance buffer that is hand-sized from a count becomes a `std::vector` member (or `std::string` for a name copy) of the class that frees it today; the pointer that other code reads stays a view of the vector (`.data()`), sized once. Counts read from a file are range-checked with a log line before they size anything (ruling 5, B4 invariant 2).
- F6 Class `operator new` / `operator delete` of an asset class are deleted in the LAST batch of their heap (B5.5), after the class owner is `unique_ptr` / value. Until then they stay and keep routing to the heap.
- F7 A registry (`AppearanceTypeList`, `ObjectTypeManager`) owns its types with `std::unique_ptr`; users hold non-owning raw pointers valid for the mission (E3). The `numUsers` unload (`apprtype.cpp` removeAppearance) is kept: the registry erases the slot's `unique_ptr` at zero.
- F8 Teardown order is written beside the declaration (E2): types before the heap they came from; the Sensor shapes (`GVAppearanceType::SensorTriangleShape`, ...) have ONE deleter after (today `mission.cpp`, `simplecamera.cpp` and `apprtype.cpp` each delete them).
- F9 No static-duration owners (E10): the statics that remain are views of a non-static owner, or the batch leaves them and names them.
- F10 Include-only commit first (D16) wherever a batch adds a standard header to a TU or header that did not have it, objhash-compared (IDENTICAL); a new header with no user ("no uses yet", as B4.2a) is its own include-only commit with its doctest.
- F11 `tools/build_all_targets.py` is the proof of every header change; editor and tool files that need a compile edit are forced edits listed in the commit (B3 E12).
- F12 Behaviour that changes goes in the commit message and the notes (section 7b practice): every reader that rejects malformed content logs one line; no `STOP` or assert stands in for a check.

## 4. Batches (cap 25 EDITED files per commit; one asset family per commit) and proof
| batch | content | proof per commit | owner of the proof |
|---|---|---|---|
| B5.0 | notes, sealed census, before-review | none (no code) | lane 1 |
| B5.1a | include-only: `mclib/texture_ref.h` (`BasicTextureRef<Releaser>`, `TextureRef`), `MC_TextureManager::releaseNode`, doctest; no callers | build, doctest; objhash of the TUs that include `txmmgr.h` stays IDENTICAL except `txmmgr.cpp` | lane 1 |
| B5.1b | the 12 `removeTexture(gosHandle)` sites (UI, paint-scheme reset, `gosimage`) call `releaseNode(node)`; `removeTexture` deleted | sim: `sim_determinism check` x4, saveload x5; render: same-exe GL pairs on mc2_01 day and mc2_10 night, viewed, plus the repeat pair | lane 1 runs and views |
| B5.1c | the 14 `removeTextureNode` sites (`terrtxm*.cpp`, `cellip.cpp`) hold `TextureRef` members; `removeTextureNode` deleted | as B5.1b | lane 1 |
| B5.2 | TG shapes: the tglHeap buffers of `msl*`, `tgl*`, `assimp_importer` become vectors / `unique_ptr<T[]>` owned by their shape; `asset_new/delete` of shapes | as B5.1b plus a mission-reload delta (ALLOC_STATS proof build) per H2 | lane 1 |
| B5.3 | appearance types: `nodeData`, node names, paint schemata, weapon-node arrays; `AppearanceTypeList` owns types with `unique_ptr`; the Sensor shapes get one deleter | as B5.2 | lane 1 |
| B5.4 | `ObjectTypeManager` table to `unique_ptr` | sim x4, saveload x5 (object type ids are saved) | lane 1 |
| B5.5 | delete `tglHeap`, `appearanceHeap` and the asset classes' `operator new/delete` | build_all_targets, sim x4, tier1 | BLOCKED on Sol for the owner sites in `code/mission.cpp` (lines 847-908, 1142-1191) and `mission2.cpp` is not on the Sol list |
Each batch's last commit: tier1, ASan doctest and the campaign round trip once on its final exe (proof-by-domain checkpoint). Lane 1 is a sim lane; the render proof is the same-exe GL pair viewed, because the orchestrator assigned it for B4 and the texture, shape and appearance batches change what the renderer holds. If that assignment is withdrawn, B5.1b onward stop at the build.

## 5. Findings of the recon (each is a rule or a hazard above)
1. **Two release APIs, two meanings.** `removeTexture(gosHandle)` decrements; `removeTextureNode(node)` destroys unconditionally. A node shared by two `loadTexture` calls and released by node loses its texture under the second user. The terrain nodes are `textureFromMemory` nodes (one user), which is why it has not shown.
2. **Eleven sites call `get_gosTextureHandle(node)` only to obtain the key of `removeTexture`.** The call caches a texture into video memory (and may evict another) before the release; the node itself is what the release needs (F3).
3. **`mclib/mlr/gosimage.cpp:78` passes a NODE index to `removeTexture(gosHandle)`.** The function compares it with GL names, so it either finds nothing (a leaked reference) or decrements another node whose GL name equals this node index (node indices are below 4096, GL names are small integers). Fixed by B5.1b through the node API; file a BUGS row when the pair proves a visible effect, else a lane-review row.
4. **99 acquirers, 26 releases.** Actors and widgets that never release keep their reference until `MC_TextureManager::destroy`. F2 keeps that.
5. **The heaps are sized constants, bulk-released.** `tglHeap` 128 MiB (`mission.cpp`) or 48 MiB (`mission2.cpp`), `appearanceHeap` per `AppearanceTypeList::init`. A vector conversion has no cap; a leak that the heap's `destroy()` hid becomes a leak in the process (H2).

## 6. Hazards (each batch checks its own)
- H1 releases added or removed change texture residency and cache hits: render-visible, so captures.
- H2 bulk release hid per-buffer leaks: a batch that converts a tglHeap / appearanceHeap buffer states which object frees it and runs the reload-delta (mission load, unload, load again) before and after.
- H3 pointers into appearance / shape arrays held by instances (weapon node tables read per frame): the vector is sized once and never grows after the first view (B4 invariant 3).
- H4 `AppearanceType::numUsers` round trips (`apprtype.cpp` 300-508): the registry change keeps the exact count transitions; a doctest on a fake type pins them.
- H5 the editor holds its own `systemHeap` / `tglHeap` (`Editor.cpp`): a class whose `operator new` leaves the heap must still construct in the editor; `tools/build_all_targets.py` plus `scripts/run_editor_smoke.py` for editor-reachable changes.
- H6 PCH overload visibility (F10): `txmmgr.h` is included widely; `tgl.h` and `msl.h` by `pch_skip` TUs.

## 7. KPI expectation
Sealed census 400 sites; each batch states its own before / after. Expected: B5.1 tex_release 26 -> 0 (`releaseNode` and `TextureRef` destructors are not counted), B5.2 heap rows 69 -> 0 except the class operators, B5.3 heap rows 31 -> 0, B5.5 class_op 16 -> 0. `cxx17_ratchet` TOTAL only down by intent; `own_inventory` File-family and asset `new` counts down.

## 8. Open (orchestrator)
1. The render-proof assignment for B5.1b onward (section 4): stands as for B4, or lane 6 / lane 2 runs it.
2. B5.5 needs the `code/mission.cpp` owner sites: Sol's claim.
3. Whether the editor E-batches take the editor's `removeTexture*` / asset-heap rows (none today use the texture release APIs: `git grep` over `editor/` finds no call).

## 9. Results (lane 1, 2026-10-16 fifth session)
| batch | sha | content | census |
|---|---|---|---|
| review fixes | 7e8d96bea | BUGS row 47, node-0 guards, `releaseNode` double-release log line, census input errors (exit 2), `releaseThrough` null-manager doctest | |
| B5.1c | ef02d3a14, 3b87c4835 | colour map node members are `TextureRef` (include-only first, objhash 0 differing bytes); `removeTextureNode` deleted; the tile arena struct and cellip's file-static keep an index and call `releaseNode` (rule F9: no uninitialised or static-duration owner) | 388 -> 374 |
| B5.2a | 2f4bd7996 | the five per-frame TG pools own `unique_ptr<T[]>` made uninitialised; ASE parser scratch to vectors | 374 -> 360 |
| B5.2b | a73817ce7 | `TG_Animation` tracks and `TG_AnimateShape` list are vectors; `.anm` loader checks counts and track sizes (`[ANIM_TRACK_BAD]`) | 360 -> 351 |
Include-only commits: `<memory>`, `<vector>` in tgl.h, mc2log.h in msl_anim.cpp (objhash: executable bytes identical; the msl_anim.cpp object differs only in four std::numpunct comdat instantiations that the logging header pulls in). Open in B5.2: the msl_load / tgl_types / msl_shape type and texture arrays and the class `operator new/delete` rows (B5.5). B5.3 (appearance types, registry teardown order: the one place the review asks for an Opus read) not started.
Baselines: the four saveload baselines other than mc2_10 carried the same WeaponBolt handle bytes and were re-recorded on the final exe (BUGS row 45 cause); the cxx17 baseline is rewritten with one own rise, LG05 +2 in `msl_anim.cpp` (the two `return -1` refusals of `TG_AnimateShape::LoadBinaryCopy`, a function that already returned -1 for a bad version).
Proof on the final exe (a73817ce7): ninja + doctest rc 0, sim pair mc2_03 / mc2_24 PASS, saveload mc2_10 PASS (then mc2_01/03/16/24 re-recorded and confirmed), tier1 5/5, campaign round trip (same lossy diff as before), ASan doctest rc 0, VK capture pair mc2_01 day and mc2_10 night: `verify.py --golden current` PASS (11 regions, 2 poses) on both runs, frames viewed (`A:/Games/mc2-capture-evidence/l1s5`).

## 10. Teardown order of the appearance types (B5.3 pre-note, lane 1, 2026-10-16 sixth session)
1. Creation: `AppearanceTypeList::getAppearance` news a type (class `operator new` routes to `appearanceHeap`), links it into the `head`/`last` list and counts users; the type's `init(file)` loads TG shapes from `tglHeap` and takes texture node references.
2. Release while the mission runs: `removeAppearance` unlinks the type at `numUsers == 0` and `delete`s it, so the virtual destructor chain (each subclass `~X` calls its own `destroy()`, then `~AppearanceType` frees the name) is exercised today.
3. Mission end (`code/mission.cpp` 1142, Sol's file; `simplecamera.cpp` for the viewer): `~appearanceTypeList` -> `AppearanceTypeList::destroy()` calls the virtual `destroy()` of every remaining type and then DROPS `appearanceHeap`; no destructor runs, the type objects die with the heap. Then the Sensor shapes are deleted, then `tglHeap` is destroyed (`mission.cpp` 1150-1190), then the texture manager at application exit (`mechcmd2_frame.cpp` 240).
4. Order any conversion must keep: types before `appearanceHeap` (they free their names through it) and before `tglHeap` (their shapes live there); the texture manager outlives all of them (`TextureRef` release is null-safe in any case, review s5 S5: `mcTextureManager` is nulled only after `delete`, so a `TextureRef` member released inside `~MC_TextureManager` would see a half-destroyed manager; none is today).
5. Hazard checked: making the list `delete` each remaining type (one destroy via the destructor, the removeAppearance path) is the only change B5.3 needs there; calling `destroy()` first and then `delete` would run each subclass `destroy()` twice. `pointerCanBeDeleted` (`simplecamera.cpp`) tests an APPEARANCE INSTANCE's address against `appearanceHeap`, so it stays valid while `Appearance::operator new` routes there (deleted with the heap in B5.5, not B5.3).
Verdict: no ordering hazard; B5.3 proceeds with the list `delete`-ing each type once.

## 11. Results (lane 1, 2026-10-16 sixth session)
| batch | content | census |
|---|---|---|
| s5 review | `TG_AnimateShape::destroy` resets `count` (S1); `[ANIM_TRACK_BAD]` logs every rejection (S2); null-after-delete note in section 10 item 4 (S5) | |
| B5.2c-1 | `TG_TypeShape` vertex / triangle / texture lists are vector stores with views; `InitFromImportedMesh` takes vectors by move; shape files reject oversized counts (`[SHAPE_COUNT_BAD]`) | 351 -> 333 |
| B5.2c-2 | `TG_TypeMultiShape` node-pointer array and texture list, `TG_MultiShape` shape records are vector stores; multi-shape count checks | 333 -> 320 |
| B5.3a-c | `NodeData` owns its id text, the three type classes own their node tables; `AppearanceType::name` is a `std::string`; `AppearanceTypeList::destroy` deletes each type once (section 10: no ordering hazard) | 320 -> 303 |
| B5.4a | `ObjectTypeManager` table is a vector; `ObjectType::appearName` is a `std::string` | 303 |
Left in B5.2/B5.3: `TG_Shape::listOfShadowVertices` (raw instance memory), the per-instance `nodeUsed` / `nodeRecycle` / `paintSchemata` tables (instances, B7), the node objects the multi-shape pointer array points at, and every class `operator new/delete` (B5.5). Include-only commits this session (objhash: executable bytes identical; std comdat instantiations from the logging header differ in `tgl_types` and `msl_shape`): mc2log.h in tgl_internal.h / msl_internal.h, `<string>` in apprtype.h, `<memory>` and `<string>` in objectappearance.h, `<vector>` in objtype.h.

## 12. B5.5 prep (no code; lane 1, 2026-10-16 seventh session; census 300)
What is left of the two class-routed asset heaps, from the sealed list (16 class operator rows, 16 `heap_malloc`, 11 `heap_free`):
| heap | remaining users | what removes them |
|---|---|---|
| `tglHeap` | `operator new/delete` of `TG_TypeNode` (and so `TG_TypeShape`), `TG_TypeMultiShape`, `TG_AnimateShape`, `TG_MultiShape`, `TG_Shape` (10 rows); the instance memory of `TG_Shape` (`CreateFrom` x2, `msl_load.cpp` raw `Free(node)`) | give each class a plain `new` (delete the operators) and let the shape own its nodes with `unique_ptr`; the raw instance `Malloc` / `Free` pair becomes `new TG_Shape` / `delete` |
| `appearanceHeap` | `operator new/delete` of `Appearance` and `AppearanceType` (4 rows); the per-instance `nodeUsed`, `nodeRecycle`, `paintSchemata` tables of the actor classes (`bdactor.cpp`, `gvactor.cpp`, `mech3d.cpp`, `mech3d_init.cpp`, 9 rows: B7, they belong to the instances) | the instance tables go with the actors in B7; then the two operator pairs |
| `objectCache` (B7) | `ObjectType::operator new/delete` and every game object | not B5 |
Owner sites that need Sol's `code/mission.cpp` (claimed): creating, sizing (128 MiB game / 48 MiB `mission2.cpp`) and destroying `tglHeap` (lines 847-908 and 1150-1191), `delete appearanceTypeList`, and the Sensor shape deletes; `code/simplecamera.cpp` deletes the Sensor shapes a third time and calls `pointerCanBeDeleted` (the provenance test against `appearanceHeap`, which disappears with the heap: its four callers become an owner check). `code/mission2.cpp` is not on the Sol list. `AppearanceTypeList::init(heapSize)` and `pointerOnHeap` go in the same batch, and the editor (`Editor.cpp`) creates its own `tglHeap` and pools: an E-batch row.
Order: (1) instance tables to the actors (B7) or leave the `appearanceHeap` until then; (2) the TG operators and node ownership; (3) the heap objects and their sizing in `mission.cpp` / `mission2.cpp` after the claim lifts; (4) `pointerCanBeDeleted`. Proof: `build_all_targets`, sim pair, tier1, mission reload delta (H2), VK pair.

## 13. Results (lane 1, 2026-10-16 eighth session; census 300 -> 290)
| batch | content | census |
|---|---|---|
| B5.5a | the weapon-node tables `nodeUsed` / `nodeRecycle` of `BldgAppearance`, `GVAppearance`, `Mech3DAppearance` are `std::vector` members (were `appearanceHeap` mallocs; they went with the instances after all, since a vector member needs no view pointer); `Mech3DAppearance::copyTo/copyFrom` copy `min(10, table bytes)`; `LoadPaintSchemata`, `paintSchemata`, `numPaintSchemata`, `PaintSchemata` deleted (never called or read); `TG_*::CreateFrom` use `new TG_Shape()` and `TG_MultiShape::destroy` uses `delete node` (the class operators still route to `tglHeap`) | 300 -> 290 |
What is left is exactly the heap objects, `Appearance` / `AppearanceType` / TG class operators (14 rows: 4 appearance, 10 TG) and the 31 owner deletes; none can go before the `mission.cpp` / `mission2.cpp` / `simplecamera.cpp` owner sites (claimed) move. The Sensor shape deleters (`simplecamera.cpp`, `apprtype.cpp`) stay: F8 wants ONE deleter and the second one is `mission.cpp`.
Behaviour notes: (1) `Mech3DAppearance::copyFrom` restores only the first 10 BYTES of each table (a save format limit, not a restore of 10 entries); left as is, flagged. (2) In the game the mech tables used to leak into the heap on `destroy()` (freed only `InEditor`); the vector members now free with the instance. (3) cxx17 LG03 +3 (`new TG_Shape()` x2, `delete node`): the three raw new/delete go when the shape owns its nodes with `unique_ptr` (with the operators).

## 14. Results (lane 1, 2026-10-16/17 ninth session; census list 290 -> 238)
| batch | content | census |
|---|---|---|
| B5.5b | the five TG classes (`TG_TypeNode`, `TG_TypeMultiShape`, `TG_AnimateShape`, `TG_MultiShape`, `TG_Shape`) lose `operator new/delete`; `TG_Shape::tglHeap`, the heap sizing in `mission.cpp` / `mission2.cpp` and `Mission::tglHeapOwner` are deleted (the five per-frame pools alone mark "TGL started"); forced edits in the editor, `data_tools/aseconv` and the two tool stubs | (see total) |
| B5.5c | `Appearance` / `AppearanceType` lose their operators; `AppearanceTypeList::appearanceHeap`, `init(heapSize)`, `pointerCanBeDeleted` (callers were commented out) and `UserHeap::pointerOnHeap` (always true) are deleted; the registry owns `std::vector<std::unique_ptr<AppearanceType>>` in load order (F7), `removeAppearance` erases at `numUsers == 0`, `destroy()` releases each type once in load order; `AppearanceTypeList::destroySensorShapes()` is the one deleter of the three Sensor shapes (F8) | 290 -> 238 for B5.5b and B5.5c together |
Left, and why (each is a narrower reading of the task, listed as findings): (1) `ObjectType::operator new/delete` (`objectCache`, B7) stays; (2) `GameObjectManager::objTypeManager` stays a raw pointer: `tests/unit/test_part_archive.cpp` and `test_combat_retirement_seam.cpp` assign a stack object or a test-owned pointer to it, so owning it with `unique_ptr` breaks them; the two deletes in `objmgr.cpp` remain; (3) the seven `delete appearance` instance deletes belong to the actors (B7); (4) `TG_ShapeRec::node` and `typeNodeStore` still hold raw `new` / `delete` (LG03 +3 of the eighth session): `TG_ShapeRec` is copied by value in `Detach` and the shape-compaction loop, and `listOfTypeShapes` is a raw view of the vector, so `unique_ptr` needs those two to move first; (5) the other `asset_delete` rows counted under "heap owners" are the B4 pools (`terrain.cpp`, `terrtxm.cpp`, `txmmgr.cpp`, `mlrtexturepool.cpp`), not B5 objects.
Behaviour notes: a TG shape or appearance type that only the heap's bulk `destroy()` used to free is now freed (or leaked) by its owner; the types free through `AppearanceTypeList::destroy()` and the instances through the actors, so a leak that the heap hid is now a real leak (hazard H2, not measured with an ALLOC_STATS build this session). The `Appearance` operator used to return a null pointer when the heap was not ready; that class of crash is gone. The editor's forest placement lost its "appearance heap exhausted" stop (the heap does not exist).
