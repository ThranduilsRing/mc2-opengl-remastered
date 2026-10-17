# Lane 1 s10 cleanup review (B1/B2 closure, S1-S3, MC2_ALLOC_STATS proof build)

Range 885ca796f..505511f91. Read-only; no builds, no launches.

## Verdict
Changes requested. B2 is closed. B1 is closed for the destroy path, camera, waypoint markers and vTol, but NOT for the pooled artillery path the commit claims to cover: the new `appearance = nullptr` in `Artillery::init(bool)` runs before the new `destroy()` in `init(bool, ObjectTypePtr)`, so the previous round's appearance is still orphaned on every pooled reuse. The missing runtime evidence for artillery is what let this through, and it should block.

## Must-fix
1. Pooled artillery still leaks. Pool reuse is `GameObjectManager::createArtillery` -> `getArtillery()` -> `artillery[i]->init(false)` (objmgr.cpp:736) -> `init(false, objectType)` (objmgr_misc.cpp:480-487). `Artillery::init(bool)` (artlry.cpp:655) now sets `appearance = nullptr` unconditionally, so by the time `init(false, type)` calls `destroy()` the pointer is already gone and nothing is deleted. Nothing else frees a finished round's appearance (`update` deletes only effects and bomber, artlry.cpp:902-921). Result: one BldgAppearance plus TG shape leaked per strike that reuses a pool slot, the exact B1 symptom. The same `init(false)` also nulls hitEffect, contrails and bomber without deleting (older; normally already freed by update, not if a round is reused with a live effect).
   Fix: free before the reset on the reuse path. `init(bool)` is also the constructor path (create == true, members uninitialised), so key on create: `if (!create) destroy();` at the top of `init(bool)` (or call `destroy()` in `getArtillery` before `init(false)`), and keep `appearance = nullptr` for the constructor. The `destroy()` at the top of `init(bool, type)` then only serves fresh objects (createInstance, load) where it is a no-op; comment which case it serves.
2. Get one runtime proof of the pooled path; the state notes say a pooled round never appeared in the fixture world. With the ALLOC_STATS build, make sure `IfaceCallStrike` really reaches `createArtillery` (non-null, numArtillery > 0 for the mission), and show live blocks flat across two strikes in the same slot. Alternatively a doctest running `Artillery::init` twice with a counting appearance. Without it "B1 closed" is unsupported.

## Should-fix
1. `~EffectLibrary` now calls `SpecLibrary::Shutdown()`. Double shutdown is safe (idempotent; `TerminateClass` deletes Instance, then Shutdown finds a null instance). Remaining risk: any `gosFX::Effect` still alive when the texture-manager flush deletes the library keeps `m_specification` pointers into freed specs (effect.cpp:459, 561-690). Before, the specs leaked and stayed valid. Reload x8 and tier1 pass, so ordering is evidently fine; add a WHY line in the destructor that effects must be dead before the library is deleted, and confirm objects are destroyed before the txmmgr.cpp:272/455 flush.
2. `~GameCamera` -> `destroy()` -> `Camera::destroy()`, then `~Camera` -> `Camera::destroy()` (static dispatch). Safe only because the base version nulls what it frees; say "idempotent" in the comment on the virtual.
3. Record in lane1-state that the pooled path was not exercised at runtime until must-fix 2 is done.

## Nits
- The `init(bool, type)` comment "A re-init must not orphan ..." is only true after must-fix 1.
- `lockIconPage` comment: add that a null return means the lock is already released, so callers must not unlock.
- `delete vTol[i]` on `BldgAppearance*`: relies on the virtual Appearance destructor, as the other 11 sites do.
- `S10_*` knobs are plain getenv, outside the gate census; acceptable in a compile-time-only proof build, and the comment says so.
- Confirm ALLOC_SURVIVORS / ALLOC_SITE / ALLOC_SKIPPED (emitted from the .inc) are covered by the census; only ALLOC_RELOAD and ALLOC_TICK appear in this diff's baseline hunk.

## Checked-OK
- B2: `gos_UnLockTexture` guards handle range and null. All five mechicon writers (setDrawBack, MechIcon::init, doDraw, VehicleIcon::init, update) use `lockIconPage`; failure returns or `continue`s with the lock already released. txmmgr_texture.cpp: both memcpy sites null-guarded, the first bounded by surface bytes (the second trusts txmSize, acceptable). S1 preview read bounded. S2 comment now correct.
- MechWarrior::destroy -> `clearTacOrderQueue()`: single call, queue emptied so no double free later.
- vTol zero-initialised, deleted and nulled in `MissionInterfaceManager::destroy`.
- Virtual `Camera::destroy`: no double-free; GameCamera has `override`.
- ALLOC_STATS: option default OFF; every hook is under `#ifdef MC2_ALLOC_STATS` (include, gos_Malloc, gos_Free, frame fixture). OFF build is the original `return malloc(bytes)` / `free(ptr)`: zero cost. Tag names are upper-case, versionless like neighbours, in the baseline.
- Reload delta not ~0 (+1.8 MB; ABL B8, SensorManager B7, TerrainTextures B4): does NOT block. Owners named, outside this fix's classes, appearance/TG/type survivors are 0. Keep as OPEN.
- VK campaign ImGui crash: does NOT block; no file of this range on the stack, routed to lane 2 (only with MC2_VK_SHARED_GBUFFER=1).
- Artillery without runtime evidence: BLOCKS, because static review found the bug in must-fix 1.
