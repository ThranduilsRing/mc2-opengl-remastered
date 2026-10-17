<!-- doc-status: current | reviewed:2026-10-17 -->
# Lane 1 s11 + s12 cleanup review (505511f91..3606e6820), read-only

## Verdict
APPROVE. No must-fix. The Sensor teardown order, the terrtxm frees and the Artillery re-init hold up on read.
The remaining gaps are proof gaps (non-quickLoad free path never run, no reload capture, TerrainTextures not under ASan) plus two small hardening items.

## Must-fix
None.

## Should-fix
1. BUGS row number clash. Row 53 here is "sensor system" (7cfacf259). Lane 2 also used 53 on its branch.
   When the branches meet, one side renumbers. Renumber lane 1's rows to the next free numbers at merge time, not before (the commit messages and lane1-state quote 53/54).
   Add one line in lane1-state OPEN saying so, so the merge owner does not discover it by conflict.
2. `SensorManager` is now a namespace-scope `unique_ptr`, so its destructor runs at process exit.
   `~SensorSystemManager` -> `destroy()` calls `missionHeap->Free(sensorPool)` and reads `Team::numTeams`.
   If the game exits while a mission is live (or a load failed after `initTriggerAndDropZone` and the error path skipped `Mission::destroy`), that runs after `missionHeap` is deleted or nulled.
   Check the exit path calls `Mission::destroy`. Otherwise guard `destroy()` with `if (sensorPool && missionHeap)`, or explicitly `SensorManager.reset()` in shutdown.
   Not reproduced; the old raw global could never hit this.
3. Proof gap, terrtxm: `quickLoad` (pre-cooked `.lst`/`.txm`, the default in-game) leaves every `textureData`, `maskData` and `overlayData` entry null (`initMask` returns early; the tables are memset to 0).
   So the new mip-entry frees run for real only with `!quickLoad` (editor, no `.lst`, or `MC2_FORCE_TXM_REGEN=1`).
   The reload fixture's survivor counts do not exercise them on the default path.
   Run one proof-build load with `MC2_FORCE_TXM_REGEN=1` and check no heap error plus a normal terrain capture.
   By read, the frees are correct. `MC_MAX_MIP_LEVELS` is 1, the tables are memset full-length, and no entry aliases another.
4. Terrain visual proof was a fresh load. Yes, require one capture after a save reload (quickload of a save), not a fresh boot.
   A reload runs `destroy()` -> `update()` no-op -> `tileHeap->Free`, then a new `init()` in the same process, which is the one sequence the frees changed.
   A single mc2_03 post-reload montage, compared with the fresh-load one, is enough.
5. ASan: the doctest tier never instantiates `TerrainTextures`, so it says nothing about these frees. Do not cite "ASan doctest rc 0" as evidence for s12 terrtxm.
   gos heaps are also not ASan-visible, because `gos_Free` goes through the gos allocator.
   The real evidence is the alloc-count delta from the proof build. Say that in lane1-state.

## Nits
- `CMakeLists.txt` line ~146 comment still says only `gameos.cpp` and `mechcmd2_frame.cpp` see `MC2_ALLOC_STATS`. `artlry.cpp` now does too; update the comment.
- `TerrainTextures::destroy` nulls `textures`/`details` but leaves `numDetails`/`numTxms` non-zero. `getTilingFactor`/`getDetailFrameRate` after destroy would deref null (it was a dangling pointer before). Zero the counts.
- The `S10_BARRAGE` block in `mechcmd2_frame.cpp` is proof-build-only and long (2 print_out lines of about 300 chars). Acceptable, but it sits in the per-frame logic file; consider moving it into a helper in an `#ifdef` region.
- `Artillery::init(bool)` for `create=true` runs on an unconstructed object, hence the `!create` guard. The comment says so, which is good. `init(bool, ObjectTypePtr)` also calls `destroy()` (existing), so the pooled path now destroys twice; harmless because it is idempotent. The second one could go, but leave it.
- terrtxm.cpp +50 lines trips the file-size WARNING. The two lambdas could be a small `freeTileTables()` private member, which would also get a unit-testable seam.

## Checked-OK
- Sensor teardown order. `ObjectManager->destroy()` runs first, while the manager is live: `Artillery`, `Mover` and `Building` dtors and destroys call `removeTeamSensor`/`freeSensor` (bldng.cpp:942, mover.cpp:270, artlry.cpp:1134). Then weaponEffects/craterManager (no sensor use), then `reset()`, which runs `~SensorSystemManager`: it frees the pool through `missionHeap` (still alive) and loops `Team::numTeams` (still set) before `resetTeamsAndCommanders()` zeroes it. `missionHeap->destroy()` comes after. Order matches the contact.h comment and the code.
- Users after reset: `controlgui_render` null-checks. `gametacmap`, `team.cpp:161`, ABL queries and `mission_update:493` run only inside a mission, and `closeABL` runs after the reset. `MechWarrior::shutdown` precedes it; no remaining reader is reached after the reset.
- saveload_load: `make_unique` over an existing manager would destroy the old one cleanly. `Mission::destroy` runs before a load, and `loadTeamsAndObjects` creates after `Team::Load`, so `Team::numTeams` is valid when `~SensorSystemManager` later runs. `init(true)` has the same allocation sites as before.
- Header uses: `extern std::unique_ptr<...>` in contact.h; `<memory>` is reached via existing includes (the TU builds). All call sites use `->`; `Assert(SensorManager != nullptr)` compiles for unique_ptr.
- terrtxm frees: every freed block pairs with a `tileRAMHeap->Malloc` of init/initTexture/initMask/initOverlay/initDetail. `textures[]` and `details[]` pair with `tileHeap->Malloc` and are freed only in `destroy()` (they stay readable all mission via `isCement`/`getTilingFactor`/`setDetail`). No reader of the freed tables exists after `update()`: `types`, `overlays`, `transitions` and `localBaseName` were already nulled there before s12, so no new use-after-free. The only post-update `types[]` readers (`setTexture`, `peekTextureHandle`) run during load.
- Double free: `update()` is guarded by `tileRAMHeap` and nulls it; `destroy()` calls it again as a no-op; the `tileHeap` frees are guarded by non-null and the pointers are nulled; the destructor re-enters `destroy()` safely. Re-init builds fresh heaps. `types[i]` with a missing block or a duplicate `terrainId` have null tables and names. `textureFromMemoryRaw` copying its input makes the 16x16 scratch free correct.
- Artillery re-init: `getArtillery` calls `init(false)` only when `!getExists()`; destroy then frees hitEffect, contrails, bomber and appearance, and nulls them. Sensor index is untouched there (freed by update at round end). The `create=true` constructor path has nothing to free.
- ALLOC_STATS: every counter, the global definitions, the barrage block and the prints are inside `#ifdef MC2_ALLOC_STATS`. CMake adds the define only under the OFF-by-default option, and `artlry.cpp` is added only inside that `if`. Default build: zero code, zero cost. The `if (!create) destroy();` is unconditional but real behaviour, not instrumentation. `g_mc2Art*` are globals defined in artlry.cpp and `extern`ed in frame.cpp, both gated.
- Log-tag census: `.inc` in CPP_EXT plus the re-frozen baseline (+ALLOC_ART probe shapes). Consistent with the s10 ratchet note.
- WHY comments in contact.h, mission.cpp, terrtxm.cpp, artlry.cpp and effectlibrary.cpp state what and why; none restate the code.
