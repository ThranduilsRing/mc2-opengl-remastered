# MISSION-INTERFACE-PERF-1 — GameLogic.Mission.Interface attribution + fix

Date: 2026-07-01. Lane: `A:/Games/mc2-ifaceperf` (`claude/ifaceperf-1`), deploy `A:/Games/mc2-opengl/mc2-win64-v0.4`.
Trigger: Tracy capture shows `GameLogic.Mission.Interface` at ~300+µs of a 2.7ms frame.

## What "Interface" covers

`code/mission.cpp:786` → `MissionInterfaceManager::update()` (`code/missiongui.cpp`): the whole
in-mission UI frame step — cursor world-unproject (`Camera::inverseProject`), team LOS at cursor,
`ControlGui::update` (pause window, command-button hover/hit-test, roster scan, mover button state,
tacmap tab, info window, vehicle tab, force-group bar), world target pick (`updateTarget`),
Attila axes + debug status bar sprintf, per-mover passability poll, input-style dispatch
(AOE/MC1 + `moveCameraAround` camera scroll), target draw-bars, rollovers. VTOL and waypoint
updates are SEPARATE zones (`Mission.VTol`, `Mission.Waypoints`).

## Instrumentation (commit 92d0f8da)

`MC2_IFACE_COST_SPLIT=1` (default OFF, registered + smoke-allowlisted) — extends the pre-existing
`MC2_MIF_SPLIT` chrono split with a rollovers bucket, ControlGui sub-phase fold-in
(`g_ifaceCgFrameNs[8]` defined in controlgui.cpp, folded by `mfFrameEnd`), and one-line
`[IFACE_PERF v1]` emission every 900 frames + partial flush at `MissionInterfaceManager::destroy()`.
Tracy: coarse `MIF.InverseProject` zone added on the cache-miss walk (cache-miss frames only).

## Measured breakdown — mc2_24, 30s smoke, minimized-window (~100fps)

Steady-state window (frames=900, camera in motion, walks≈890/900):

| phase | avg µs | share |
|---|---|---|
| **TOTAL** | **971** | 100% |
| **invProj** (`Camera::inverseProject`) | **812** | **84%** |
| updateTarget (object pick) | 68 | 7% |
| controlGui total | 20 | 2% (vehicleTab 12.4, fgBar 3.9, rosterScan 1.8) |
| LOS / drawBars / rollovers / rest | <2 | — |

Load transient: single 1.12s invProj spike (first walk, cold). Second window/mission_end
confirm invProj 868–892µs at 81–95%.

## Root cause

`[PICK_CAP]` trace (`MC2_PICK_CAP_TRACE=1`): **`numTiles=0` on every walk.** LOD-chunk terrain is
default-ON (cutover 2026-06-09) and `MapData::makeLists` only runs when it is OFF
(`mclib/terrain.cpp:2050`), so `Terrain::numberQuads == 0` in every production build. Consequences:

1. Both quad-walk stages of `Camera::inverseProject` (`mclib/camera.cpp` lowcam + legacy cap-100
   paths) iterate ZERO tiles → **dead code in production**, incl. the LOW-CAMERA-PICK-RAY-1 fix.
2. `closestTile` is always NULL → the "not in any tile, must be off map" fallback
   (`mclib/camera.cpp:1453`) is the **production ground picker**. It brute-force projects
   **ALL `realVerticesMapSide`² map vertices** (`getTerrainElevation` +
   `projectForSelectionPicking` each) on every cache-miss frame. The original code (still
   commented out above it) walked map EDGES only; it was widened to the full map at some point —
   which is also why ground picking still "works" (nearest-projected-vertex ≈ cursor ground hit at
   vertex resolution).
3. The per-frame delta-cache (missiongui.cpp) only helps when BOTH cursor pixel and camera matrix
   are unchanged — any camera pan/drift = 100% miss rate (walks 812/812 observed).

## Fix (this slice, gated default OFF)

`MC2_PICK_FALLBACK_COARSE=1` — coarse-to-fine argmin over the same screen-distance field:
stride-8 coarse pass (+ last row/col), then full-res ±9 refine around the coarse winner.
Same nearest-projected-vertex answer in practice (basin spans many vertices at playable zooms);
with `MC2_PICK_CAP_TRACE=1` every 32nd walk re-runs the brute force and logs
`[PICK_FALLBACK] parity=ok|DIFF` (keeps the brute answer on divergence, trace mode only).

Before/after (mc2_24, 30s smoke, steady 900-frame windows, per-walk normalized):

| | invProj avg | walks/frames | Interface TOTAL avg |
|---|---|---|---|
| OFF, instrumentation build (legacy brute force) | **812 µs** | 890/900 | 971 µs |
| OFF, fix build same binary (refactored legacy path, regression check PASS) | 973 µs | 900/900 | 1352 µs |
| ON (+cap-trace parity brute every 32nd walk) | **235 µs** (~207 net of parity overhead) | 900/900 | 550 µs |
| ON, window 2 (camera part-stationary, 482 cache hits) | 99.5 µs | 418/900 | 381 µs |

≈ **4× on the walk; Interface drops from ~970µs to ~380–550µs**; remaining top consumers are
updateTarget (~100µs) and the (now ~210µs/walk) coarse fallback. Speedup is below the ~24×
sample-count ratio because the coarse pass strides scattered memory (`getTerrainElevation`
block lookups) while the brute force streams it — per-sample cost ~350ns vs ~62ns.

Parity (12 sampled walks): 10 `ok`, 2 `DIFF` — both DIFFs are exact zero-distance ties
(`coarse_dsq=0.0 brute_dsq=0.0`) at the headless smoke's degenerate cursor (0,0), where all
behind-camera vertices project to exactly (0,0) via the `w<=1e-4` guard and the argmin is
tie-broken by iteration order. Both answers are equally "nearest"; the same corner-cursor
mass-tie exists in the legacy brute force. Real in-viewport cursors produce distinct distances.

## Follow-on proposals (NOT this slice)

- **PICKER-TRUTH-1**: the quad-walk stages + their env gates (`MC2_LOWCAM_PICK`,
  `MC2_PICK_CAP_TRACE` stage counters) are dead under LOD-chunk terrain. Either repoint
  `inverseProject` at a heightfield walk that is exact (the Phase-7B raycast picker exists but was
  reverted: wrong eye origin + collapsed X response in `worldToClipGL` inverse), or delete the
  dead stages and make the (now-cheap) fallback the honest primary with cell-snapping restored.
  Note the fallback returns `1` ("bogus value") yet callers use the point regardless — return
  contract is a lie today.
- **updateTarget (68µs)**: second consumer; object pick under cursor. Worth a split only after
  invProj lands.
- The 1.12s first-walk spike happens during load (cold caches, full map) — masked by load screen,
  not worth a slice.
