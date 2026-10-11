# Save/load fidelity gap — investigation 2026-10-11 (read-only; HEAD b609d6107)

Code read with `git show HEAD:`; no source edits, no builds. Runs: `tools/saveload_roundtrip.py run` (mc2_16, the only mission it supports:
MC2_FIXTURE_ORDER_LIVENESS) x3 + fixture runs through `scripts/run_smoke.py` (host lease, background), against the deployed
`mc2-win64-v0.4` (lane 1's exes: e706ac64d-dirty, then b609d6107-dirty; dirty = lane 1 `DT` trace lines only). Scratch analysis
scripts parse `[SIMID v1]` logs and the `testgame.ims` PacketFile (header 0xFEEDFACE, seek table, zlib packets).

## Q1 — did the legacy format ever store walk/run state and path step?
- **Path step: yes, since the first commit, and it IS restored.** `SaveableMoveOrders` holds `MovePath path[2]` BY VALUE
  (code/warrior.h:498; 305ad8746:code/warrior.h:495) incl. `curStep`/`numStepsWhenNotPaused`/`stepList` (mclib/move.h:971-981),
  plus `moveState`, `moveStateGoal`, `run` (warrior.h:500-506), copied by `copy()`/`copyTo()` (warrior.h:508-586), embedded in
  `MechWarriorData::moveOrders` (warrior.h:771). Measured: at the first post-load frame pstep/psteps match for all 51 movers.
- **Walk/run for MECHS: yes, since 305ad8746, restored.** `MechData::apData` (code/mech.h:337) = `MechAppearanceData`
  (mclib/mech3d.h:269-319: currentGestureId, currentStateGoal, transitionState, frames, jump state) via
  `Mech3DAppearance::copyTo/copyFrom` (mech3d.cpp:272-360). Walking mechs (gesture 4) keep it across the load.
- **Walk/run for GROUND VEHICLES: never.** `GroundVehicleData` (code/gvehicl.h:180-224) has no appearance block in any commit;
  `git log -S gvAnimationState -- code/gvehicl.h` = 0 commits; `GVAppearance` has no copyTo/copyFrom; the fresh appearance starts at
  `gvAnimationState = -1` (mclib/gvactor.cpp:735). The 4 movers lane 1 saw lose "walk/run" (706/709/713/715, gesture 1/2 -> 0) are
  vehicles (gvehicl_control.cpp:273-277: 0 stand, 1 walk, 2 run, 3 repair).
- `Mover::load/init`: `GameObjectManager::Load` news each object, `init(true,type)` (creates the appearance, mech_init.cpp:163),
  then `XXX::Load(&data)` (objmgr_io.cpp:981-1035); pilots via `MechWarrior::load` -> `init(MechWarriorData)` (warrior_state.cpp:845);
  path manager rebuilt empty (saveload_load.cpp:155); ABL env restores eternals + per-instance state/prevState/initCalled/statics
  (mclib/ablenv.cpp:1452-1474, 575-599).
- Verdict: **pre-existing legacy format gap, not a regression of this program.** "Path step lost" is downstream (Q3 #2-#3).

## Q2 — which commit dropped/broke it?
None. Every `*Data` save-struct body was diffed at every commit touching its header (gameobj, mover, warrior, mech, mech3d,
gvehicl, turret, bldng, terrobj, weaponbolt, gate, artlry, carnage, team, comndr, objmgr, move): the only layout changes are the
2016 64-bit ports d8690b800, dcb1c0388, 9fd3d88e5 (long -> int32; these already broke retail-save compatibility) and cf66ea53a
(2026-07-02, copy of the already-present `curWayDir`, additive). Save/Load bodies vs 305ad8746 differ only by memcpy -> ArrayCopy,
long -> int32_t, dropped dynamic_casts, `refreshDerivedCapabilities()` in BattleMech::Load (mech.cpp:981) and a gated restore trace.

## Q3 — what else is lossy (round trip, mc2_16, 250 compared frames)
Measured first divergence per SIMID field: gesture +0 (4 vehicles, 1 frame only), pos/rot/pstep +70 (mover 707 replans: 3/6 vs 0/10),
target +98, mstate +126, status +228; 59/250 frames keep an identical digest. Position, rotation, hp, target, path step all restored.
| # | State lost | Evidence | Gameplay-visible |
|---|---|---|---|
| 1 | Vehicle animation state (gvAnimationState, currentFrame, isReversed/canTransition, nodeRecycle) | Q1; re-derived next frame from velocityMag (gvehicl_control.cpp:286-300) | visual, 1 frame pop + anim restart |
| 2 | Process RNG state (`s_lcg_state`, GameOS/gameos/gameos.cpp:290; CRT rand in retail) | not saved; reseeded only in Mission::init (mission_init.cpp:79), one `[DET_RNG]` line per run; same save loaded at render 1000 vs 1250 diverges at +60 | YES: AI choices (warrior_move.cpp:1079,1248), hit rolls |
| 3 | Pending path requests | `MovePathManager` rebuilt empty (saveload_load.cpp:155); `movePathRequest` (warrior.h:928) not in MechWarriorData; 707 re-plans 1.03 s early post-load (DT CALC t=35.70 vs 36.73, src 10) | YES: move timing |
| 4 | `speedThrottle` | field IS in SaveableMoveOrders (warrior.h:479) but copy()/copyTo() never assign it (since 305ad8746) | likely: walk throttle [unverified] |
| 5 | Sensor contacts / contactInfo | Load makes a new SensorSystem (mech.cpp:971, gvehicl_update.cpp:913); not in MoverData | YES: re-acquire delay (target 707->-1 at +98) [cause unverified] |
| 6 | Mech ripple-fire nodes | `memcpy(..., nodeUsed/nodeRecycle, 10)` copies 10 BYTES of long[10]/float[10] (mech3d.cpp:305-306, 351-352) | minor: fire stagger |
| 7 | Mech appearance `status` | deliberately not restored (mech3d.cpp:340) | by design |
| 8 | Program-added runtime fields | lastJumpFailFrame/lastFailedJumpGoalCell* (warrior.h:936-938), Mover::runtime (re-derived), GameObject framesSinceActive/lastUpdateRet, BattleMech playedHeloDestruct | small |
| 9 | Brain runtime (brainTaskQueue, brainRuntime, brainAlarmCallback), radio playback | not in MechWarriorData; rebuilt from ABL instance | AI warm-up [unverified] |
| 10 | Watch-handle generations | reset to 0 on load by design (objmgr_io.cpp:639-644) | no |
ABL: no stack/locals persist across frames, so eternals + statics + FSM state is the full persistent set [FSM timers unverified].
Not checked: weapon recycle/heat (InventoryItem is saved), team/alignment (TeamData saved), turrets/buildings (SIMID is movers only).

## Q4 — is the round trip run-to-run stable?
**Yes, with the frames pinned.** Three full runs (lane 1 09:41 at 45711412c clean; mine at e706ac64d-dirty x2): pre-load chains
identical over 1251 frames, post-load chains identical over 807-810 frames, identical divergence report (+0, 4 movers, 59/250).
Caveats: (a) the post-load chain is NOT a function of the save file alone: loading the same-content save at render 1000 instead
of 1250 matches for 60 frames then diverges at t=35.366 (process state carried across the load, #2/#3), so the baseline is only
valid for the tool's fixed SAVE_AT=1000/LOAD_AT=1250 and pinned install data; (b) 2 of 6 fixture runs on this busy host never
reached the load (timeout at load 75 s; 971 frames at 35 fps) and report "no load found" — treat as infra failure, not a verdict.
=> B1-B6 CAN use the lossy chain as a baseline (`--baseline` storing the post-load chain digest, frames pinned).

## Q5 — re-save byte identity and the stable packet range
Layout of an mc2_16 save (10564 packets): 0 version stamp, 1 campaign, 2 mission FIT (camera, terrain, parts, objectives,
ScenarioTime), 3..5700 MOVE data, 5701 ABL env, 5702 StaticMechWarriorData (40 B), 5703-5822 MechWarriorData (19864 B), 5823-5826 team,
5827.. objList slots (MechData 3992, GroundVehicleData 3424, TerrainObjectData 720, BuildingData 756, GateData 760, TurretData 820,
empty bolt/carnage/artillery 580/396/432), 10557 watch list, 10558-10560 commanders, 10561 logistics FIT, 10562 mission no, 10563 END.
Same frame (ScenarioTime 33.333168), two runs, two exes (e706ac64d vs b609d6107): **packets 0..5700 byte-identical; from the ABL
packet on every live packet differs** at FIXED per-class offsets (TerrainObject 42 of 720 B, Building 19/756, Gate 8/760, Turret
24/820, GV 1712/3424, Mech 2236/3992, Warrior 10021/19864, ABL 2797/52781), while both runs' SIMID chains are identical. Cause:
uninitialised bytes serialised raw — every `XData data;` lives on the stack and CopyTo writes only named fields (e.g. mech.cpp:824,
mover_misc.cpp:291, warrior_state.cpp:815); warrior offset 24 = tail of `name[80]` holding a 64-bit heap-address-like value; the ABL
packet carries leftover strings ("data/missions/orde.." vs "getrelativepositio.."); objects come from non-zeroed pool Malloc.
**Stable range = everything before the ABL packet** (lane 1's "0..1905" is this boundary on its fixture [unverified mission]).
Load-then-re-save in the same frame was not measured (QS=QL=1000 run starved before frame 1000; the retry waited >20 min on the host lease held by a tier2 mc2_15 run and was stopped) [unverified].

## Q6 — proposal for B3/B7 (format work)
- **Version today**: one string stamp `"00.06.01.0614"` (code/version.h:16, unchanged since 305ad8746) in packet 0, strcmp'd
  (saveload.cpp:486-489) -> reject with IDS_QUICKSAVE_VERSION_WRONG. No per-struct version; packet-size dispatch (objmgr_io.cpp:761-1035) is the
  only layout check. Bumping the stamp rejects every older save, so it must stay fixed within a release line.
- **Minimal extension (B7)**: one optional packet AFTER "END" (saveload.cpp:305; the loader stops at the mission number,
  saveload_load.cpp:600, so old exes ignore it; `reserve(20000)` leaves room). Payload `{"MC2X", u32 extVersion=1, records...}`
  keyed by objList slot (= handle, stable per review finding 1): vehicle {gvAnimationState i32, currentFrame f32, flags u8};
  pilot {speedThrottle i8, pending path {present, selectionIndex, moveParams, initPath, faceObject, source} + PathManager queue order};
  mech {nodeUsed[10] i32, nodeRecycle[10] f32}; global {RNG state u32}. Missing/unknown version -> `[SAVE_EXT v1] absent` log line and
  today's behaviour (ruling: old content may degrade but loads). ~40 B x <=300 movers ~ 12 KB raw. ~200 lines (new
  `code/saveload_ext.cpp` + one save call + one load phase helper + GVAppearance setters that do not reset the frame + a re-enqueue
  in original queue order). Risk: format low (additive, both directions compatible within the line); behaviour medium (re-enqueue order,
  appearance setter side effects); RNG restore is exact only under the LCG — retail CRT `rand` state needs an owner ruling (make the LCG
  unconditional = gameplay RNG change). Sensor contacts (#5) = extVersion 2.
- **B3 layout guard (no format change)**: `code/save_layout_guard.h` included by objmgr_io.cpp, Win64-MSVC only (structs use `long`):
  static_assert sizeof MechWarriorData 19864, MechData 3992, GroundVehicleData 3424, TerrainObjectData 720, BuildingData 756, GateData
  760, TurretData 820, StaticMechWarriorData 40, + WeaponBolt/Carnage/Artillery 580/396/432 [class mapping inferred from slot order]
  and GameObjectData/MoverData [sizes unverified]; pairwise-distinct static_assert over the whole dispatch set; offsetof pins for
  MechData::apData, MoverData::inventory, MechWarriorData::moveOrders, SaveableMoveOrders::path. Plus `XData data{};` (value-init)
  in every Save/save and the ABL StackItem writes: format-identical, makes re-save byte identity a whole-file gate and stops heap/stack
  bytes leaking into save files. Size ~60 lines; risk low (a wrong pinned size fails the build, not the game).

## What would have made this faster
- A `tools/save_inspect.py` (packet map + per-class byte diff) and a recorded fixture mission/frame for lane 1's "0..1905" claim.
- A fixture that saves in the SAME frame right after a load (QS==QL ordering is undefined today) and a `saveload_roundtrip --load-at`.
- SIMID mover lines that carry the object class (mech/vehicle) and an RNG call counter; the host was contended (35 fps, 1 timeout).
