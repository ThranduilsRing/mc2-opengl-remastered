# G7: sim tick N+1 overlapped with render N, recon and verdict (2026-10-27)

Base 562e575b7. Source: gamelogic-rewrite-design-2026-10-21 Phase 6 (G7), profile-0f5 report.

**Verdict: overlap is unsafe at this base. It needs two prior packets (P1, P2 below). This lane ships only the profiler fix.**

## Frame today (main thread, serial)
1. `DoGameLogic` -> `Mission::update` (mission_update.cpp:1290). Per tick `updatePass` runs: tick clock, front end (UI, camera `eye->update`, `land->update`, weather), path manager, `updateTerrainGeometry` (`land->geometry()`, which sets `objBlockInfo.active` / `objVertexActive`), then the tick proper: `updateObjectManager` (`snapshotMoverPoses`, `ObjectManager->update`), sensors, collisions, mission ABL, `extractRenderSnapshots`, trace hooks.
2. `updatePresentationOnly` (:355): appearances, GPU cull records, VTol, displacement damp, blending mover poses between the tick-start snapshot and the LIVE mover state.
3. `ExtractRenderSnapshot` (render_snapshot.cpp:89): static props and mech packets only.
4. `draw_screen` -> `UpdateRenderers` -> `Mission::render` (mission.cpp:740): `eye->render()` walks every visible GameObject's appearance (TG shapes, `BattleMech::render` mech_update.cpp:658), gosFX effects, craters, lights; then `missionInterface->render()` reads mover status for the UI. GPU submit follows.

The only snapshot render consumes is the static-prop / mech-packet extract. Everything else render N reads is live GameObject and appearance state.

## What the tick writes that render N reads (the hazards)
- **Appearance state, inside the tick.** `BattleMech::update` reads `appearance->simulationAnimation()` / `simPose` (mech_update.cpp:18-19), writes `setGestureGoal`, `setCombatMode`, `setSensorLevel`, `setAlphaValue`, `setBarColor`, `setObjStatus`, and calls `appearance->update()` (:629). `GroundVehicle::update` writes `setObjectParameters` / `setMoverParameters` and calls `appearance->update()` (gvehicl_update.cpp:128-135, :182, :623). `TerrainObject::update` calls `recalcBounds()` and `appearance->update()` (terrobj.cpp:732-927, :1274, :1329). Render N walks the same appearances and their TG shapes: a write/read race on every visible unit.
- **Object and effect lifetime.** The tick creates weapon bolts, gosFX effects (weaponbolt.cpp:764-841, carnage.cpp:978, artlry.cpp:883), craters and lights, and removes movers (`mission->removeMover`, objmgr_update.cpp after the mover walk). Render N iterates those lists and the light rings.
- **UI state.** `missionInterface->render()` reads mover damage, selection and status the tick writes.

## What the tick reads that the frame or render writes (ordering, determinism)
- **Camera-gated activity.** The tick's terrain-object walk skips blocks and vertices that are not camera-active (objmgr_update.cpp:562-568). Those flags come from `land->geometry()` in the frame part (terrain_geometry.cpp:225-234). Render N reads the same geometry output. Running tick N+1 during render N therefore means running it on frame N's camera set, not N+1's: the SIMID chain changes whenever the camera moves (the harness camera is mostly static, so a short pair could pass and still be wrong).
- **Render-written values.** The tick reads `appearance->canBeSeen()` / `getScreenPos()` (mech_update.cpp:709-715), node positions (`getNodeNamePosition("cockpit")`, :394, :457) and `getVelocityMagnitude()` (:226, :356, :414; locomotion is animation-driven). Some of these are written by `recalcBounds` / render in the same frame.
- The shared appearance RNG (sim crater rotation vs presentation; SIMID prints rng=sim/app/wx) would interleave between threads: nondeterministic.

Not a hazard: the allocator (`operator new` -> `gos_Malloc` -> CRT `malloc`, thread-safe; GameObject uses the CRT heap since B7o.9).

## Prior packets that unblock G7
- **P1, appearance writes leave the tick.** Every `appearance->*` setter and `update()` call in `BattleMech::update`, `GroundVehicle::update` and `TerrainObject::update` moves into the presentation phase, fed by tick output (the design's "move presentation out of the tick", about 2-2.5 ms of the tick per the 2026-10-21 partition). The tick's reads of animation state (velocity magnitude, node positions, sim pose) come from a sim-owned pose that presentation copies from, not the reverse. Then render reads only presentation-written state, and presentation stays serial.
- **P2, camera-independent sim (ruling D2).** Terrain-object activity comes from a camera-free set (activity classes: Sleeping / Ambient / Active), and the tick stops reading `canBeSeen` / `getScreenPos` / `windowsVisible`. Without it the overlapped tick sees a different camera set than the serial tick.
- **Also needed for the overlap itself (in the G7 packet):** deferred spawn and removal queues for bolts, effects, craters, lights and mover removal, committed at the join; a per-tick UI snapshot for `missionInterface->render`; a split RNG (D3) so presentation never draws from the sim stream.

## The handoff once P1 and P2 exist
Frame N: frame part (camera, `land->geometry`) -> join tick N (if one was in flight) -> commit the tick's spawn/removal queue -> `extractRenderSnapshots` + presentation N (serial, main) -> kick tick N+1 on a worker with a frozen activity set -> `ExtractRenderSnapshot` + render N on main -> next frame joins. Saves, loads, pause, MP and the in-mission save fixture join first and run serial. The kill-switch (shipping ON) forces the serial order; SIMID must match serial on mc2_01/03/10/16/24 and saveload_roundtrip must pass.

## Expected gain (profile-0f5, profiler-free)
0.5-1.2 ms/frame average, about 3 ms on tick frames (mc2_24 GameLogic 3.9 vs 0.7). P1 alone should also shrink the tick, since most of its time is presentation.
