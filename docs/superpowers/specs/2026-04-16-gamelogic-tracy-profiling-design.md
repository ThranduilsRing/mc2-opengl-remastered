# GameLogic Tracy Profiling — Design Spec
**Date:** 2026-04-16
**Branch:** claude/nifty-mendeleev
**Goal:** Replace legacy ProfileTime/MCTime* system with Tracy ZoneScopedN across all game logic files, and add full per-frame + activation-path coverage to expose the source of on-screen appearance spikes.

---

## Motivation

Two problems to solve:

1. The existing `ProfileTime(x, y)` / MCTime* accumulator system is opaque — it writes to globals that are never visualized in real time, and is not the user's code. Replacing it with Tracy zones makes the same data visible in the live flame chart.

2. There are observed 200ms+ spikes in GameLogic when units/objects appear on screen. Suspected cause: AI brain init, pathfinding goal recalculation, and/or sensor contact registration firing on unit activation. These paths have no current profiling coverage and must be instrumented to confirm.

---

## Scope

### Files in scope

All 12 files that currently contain ProfileTime/MCTime* markers, plus their key per-frame functions:

| File | Primary responsibility |
|------|----------------------|
| `code/mechcmd2.cpp` | Top-level DoGameLogic dispatcher, multiplayer update |
| `code/mission.cpp` | Mission frame update, all subsystem dispatch |
| `code/mission2.cpp` | Secondary mission logic |
| `code/gameobj.cpp` | LOS update (MCTimeLOSUpdate sites) |
| `code/movemgr.cpp` | Movement manager update |
| `code/mover.cpp` | Per-mover tick |
| `code/objmgr.cpp` | Object manager update loop |
| `code/team.cpp` | Team/commander AI update |
| `code/warrior.cpp` | Warrior/unit AI per-tick |
| `mclib/mech3d.cpp` | Mech 3D state/animation update |
| `mclib/move.cpp` | Pathfinding move step |
| `mclib/msl.cpp` | Missile/projectile update |

**Not in scope:** Rendering path, shader code, txmmgr.cpp, gameos_graphics.cpp, quad.cpp (covered by separate render profiling already present, and restricted by next-session audit).

---

## Design

### Part 1 — Replace ProfileTime with Tracy

Every call site of the form:
```cpp
ProfileTime(MCTimeX, someCall());
```
becomes:
```cpp
{ ZoneScopedN("Subsystem.Name"); someCall(); }
```

The `ProfileTime` macro definitions in `mechcmd2.cpp` and `mission.cpp` are removed.

All `__int64 MCTime*` global variable definitions and their `extern` forward declarations are removed from every file. These globals serve no purpose once Tracy zones are in place.

`#include "gos_profiler.h"` is added to each file that doesn't already have it.

### Part 2 — Full per-frame sweep

For each file in scope, add `ZoneScopedN` at the entry of every function that runs per-frame and does not already have a zone. This includes:

- `DoGameLogic()` — top-level wrapper, currently unzoned
- `mission->update()` — main mission tick
- `ObjectManager->update()` per-unit loops (mechs, vehicles, turrets, terrain objects)
- `PathManager->update()` and goal/path calc sub-steps
- `missionBrain->execute()` — AI script execution
- Sensor/contact manager updates
- LOS calculation entry points (currently only MCTimeLOSUpdate cycle-counting, no flame chart visibility)

### Part 3 — Activation-path coverage (spike investigation)

In addition to steady-state per-frame zones, instrument the initialization/activation paths that fire when a unit first becomes visible or active:

- Unit/object `init()` or `activate()` entry points in `objmgr.cpp`, `warrior.cpp`, `mover.cpp`
- AI brain first-run / goal initialization
- Pathfinding first-request for a newly activated unit
- Sensor contact registration on first detection

These zones will confirm whether spikes come from activation cost, from pathfinding first-request, or from some other cause.

---

## Zone Naming Convention

Format: `"Subsystem.Component.Action"`

Examples:
- `"GameLogic"` — DoGameLogic top-level
- `"GameLogic.Mission.Update"` — mission->update()
- `"GameLogic.PathManager.Update"` — PathManager per-frame
- `"GameLogic.PathManager.CalcPath"` — individual path calc
- `"GameLogic.AI.BrainExecute"` — missionBrain->execute()
- `"GameLogic.LOS.Update"` — LOS update
- `"GameLogic.Units.Mechs"` — mech object loop
- `"GameLogic.Units.Vehicles"` — vehicle object loop
- `"GameLogic.Units.Activate"` — unit activation path (spike target)
- `"GameLogic.Sensors.Update"` — sensor/contact manager
- `"GameLogic.Collisions"` — collision update
- `"GameLogic.Multiplayer"` — MPlayer->update()

---

## What is NOT changed

- No logic changes. Pure instrumentation only.
- No rendering files (gameos_graphics.cpp, txmmgr.cpp, quad.cpp, shaders).
- No changes to Tracy infrastructure itself — `gos_profiler.h` already provides `ZoneScopedN` and `TRACY_ENABLE` is always on.
- No changes to existing render-side Tracy zones (Shadow.*, Terrain.*, Grass.Draw, etc.).

---

## Commit strategy

One commit per file or small group of related files. Do not bundle all 12 files into a single commit — if a zone name needs adjustment after live testing, smaller commits are easier to bisect.

Suggested grouping:
1. `mechcmd2.cpp` + `mission.cpp` — top-level dispatch + ProfileTime macro removal
2. `mission2.cpp` + `objmgr.cpp` — object manager
3. `warrior.cpp` + `mover.cpp` + `movemgr.cpp` — unit/movement
4. `gameobj.cpp` — LOS
5. `team.cpp` — AI/team
6. `mclib/mech3d.cpp` + `mclib/move.cpp` + `mclib/msl.cpp` — mclib

---

## Success criteria

- Tracy flame chart shows `GameLogic` as a named top-level zone with all sub-systems visible beneath it.
- The 200ms+ spike, when it occurs, now shows which specific zone is responsible.
- No MCTime* globals remain. The ProfileTime macro is gone.
- No regressions in gameplay behavior (instrumentation-only change).
