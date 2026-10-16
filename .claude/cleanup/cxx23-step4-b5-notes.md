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
