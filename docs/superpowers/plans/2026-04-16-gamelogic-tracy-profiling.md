# GameLogic Tracy Profiling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the legacy ProfileTime/MCTime* accumulator system with Tracy ZoneScopedN across all 12 game logic files, and add full per-frame + activation-path coverage to expose the source of on-screen appearance spikes.

**Architecture:** Pure instrumentation pass — no logic changes. Each `ProfileTime(MCTimeX, call())` becomes `{ ZoneScopedN("Name"); call(); }`. Scattered manual `MCTimeX += (GetCycles() - timeStart)` blocks inside functions are replaced by a single `ZoneScopedN` at the function entry. MCTime* globals and their `extern` chains are deleted entirely.

**Tech Stack:** Tracy profiler (already compiled in via `TRACY_ENABLE`), `gos_profiler.h` (provides `ZoneScopedN`). Build: `cmake --build build64 --config RelWithDebInfo`. Deploy: `cp -f` exe to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe`.

---

## File Map

| File | What changes |
|------|-------------|
| `code/mechcmd2.cpp` | Remove `#ifdef LAB_ONLY` ProfileTime+MCTimeMultiplayerUpdate block; add `gos_profiler.h`; add `DoGameLogic` + multiplayer zones |
| `code/mission.cpp` | Remove `#ifdef LAB_ONLY` extern block (~50 lines); replace all `ProfileTime(...)` call sites with ZoneScopedN; add zone to mission update entry |
| `code/mission2.cpp` | Remove any remaining MCTime* externs/usages |
| `code/gameobj.cpp` | Remove `MCTimeLOSUpdate` extern + accumulation sites; add `ZoneScopedN("GameLogic.LOS.Update")` at LOS function entry |
| `code/movemgr.cpp` | Remove MCTimePath*/CalcGoal* externs + zero-resets; add PathManager::update zone |
| `code/mover.cpp` | Remove MCTimeCalcGoal* externs + accumulation; add zones for goal calc functions |
| `code/objmgr.cpp` | Remove MCTime* externs + accumulators; add per-type update zones + activation zone |
| `code/team.cpp` | Add team/commander AI zones |
| `code/warrior.cpp` | Remove MCTimeRunBrainUpdate/Path* externs + accumulators; add brain run + path zones |
| `mclib/mech3d.cpp` | Remove MCTimeAnimationCalc definition + accumulation; add geometry update zone |
| `mclib/move.cpp` | Remove MCTimeCalcPath*/MCTimeLOSCalc definitions + accumulation; add path calc zones |
| `mclib/msl.cpp` | Add missile update zone |

---

## Task 1: Remove ProfileTime macro + add gos_profiler.h to mechcmd2.cpp

**Files:**
- Modify: `code/mechcmd2.cpp:252-257`

- [ ] **Step 1: Remove the LAB_ONLY ProfileTime block**

In `code/mechcmd2.cpp`, find and delete lines 252–257:
```cpp
#ifdef LAB_ONLY
long currentLineElement = 0;
LineElement *debugLines[10000];

#define ProfileTime(x,y)	x=GetCycles();y;x=GetCycles()-x;
extern __int64 MCTimeMultiplayerUpdate;
#else
#define ProfileTime(x,y)	y;
#endif
```
Replace with only:
```cpp
#define ProfileTime(x,y)	y;
```
(Keep the no-op macro temporarily — it will be removed when the call site is replaced in Task 2.)

- [ ] **Step 2: Add gos_profiler.h include**

Near the top of `code/mechcmd2.cpp` with the other `#include` lines, add:
```cpp
#include "gos_profiler.h"
```

- [ ] **Step 3: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/mechcmd2.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "refactor: remove ProfileTime macro and MCTimeMultiplayerUpdate from mechcmd2.cpp"
```

---

## Task 2: Replace DoGameLogic + multiplayer ProfileTime in mechcmd2.cpp

**Files:**
- Modify: `code/mechcmd2.cpp` (DoGameLogic entry ~line 2027, MPlayer->update() call ~line 2036)

- [ ] **Step 1: Add DoGameLogic top-level zone**

At the top of `DoGameLogic()` (after the opening brace and the `if (!SnifferMode)` check is fine, but the zone should be before any work happens):
```cpp
void __stdcall DoGameLogic()
{
    ZoneScopedN("GameLogic");
    if (!SnifferMode)
    {
```

- [ ] **Step 2: Replace multiplayer ProfileTime call**

Find:
```cpp
ProfileTime(MCTimeMultiplayerUpdate,MPlayer->update());
```
Replace with:
```cpp
{ ZoneScopedN("GameLogic.Multiplayer"); MPlayer->update(); }
```

- [ ] **Step 3: Remove the now-unused `#define ProfileTime(x,y) y;` line from mechcmd2.cpp**

The no-op macro kept in Task 1 is no longer needed in mechcmd2.cpp since the only call site is now replaced. Remove it.

- [ ] **Step 4: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/mechcmd2.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "feat: add Tracy zones to DoGameLogic and multiplayer update"
```

---

## Task 3: Remove MCTime* extern block from mission.cpp

**Files:**
- Modify: `code/mission.cpp:229-295`

- [ ] **Step 1: Remove the LAB_ONLY extern block**

In `code/mission.cpp`, delete the entire block from line 229 to the `#else`/`#endif`:
```cpp
#ifdef LAB_ONLY
#define ProfileTime(x,y)	x=GetCycles();y;x=GetCycles()-x;
extern __int64 MCTimeTerrainUpdate;
extern __int64 MCTimeCameraUpdate;
// ... (all ~50 extern __int64 lines)
extern __int64 MCTimeMiscLoad;
extern __int64 MCTimeGUILoad;
extern __int64 x1;
extern __int64 x;
#else
#define ProfileTime(x,y)	y;
#endif
```
Replace the whole block with only:
```cpp
#include "gos_profiler.h"
```

- [ ] **Step 2: Compile to check for missing-extern errors**
```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo --target mc2 2>&1 | grep -E "error|warning.*MCTime" | head -30
```
Expected: errors for each MCTime* variable that is still defined/used elsewhere. These point to the files to clean in Tasks 4–9.

- [ ] **Step 3: Commit the extern block removal**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/mission.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "refactor: remove MCTime* extern block and ProfileTime macro from mission.cpp"
```

---

## Task 4: Replace ProfileTime call sites in mission.cpp with ZoneScopedN

**Files:**
- Modify: `code/mission.cpp` (lines ~526–584)

- [ ] **Step 1: Replace all ProfileTime call sites**

Find and replace each `ProfileTime(MCTimeX, call())` with `{ ZoneScopedN("Name"); call(); }`:

```cpp
// BEFORE → AFTER

ProfileTime(MCTimeInterfaceUpdate,missionInterface->update());
→ { ZoneScopedN("GameLogic.Mission.Interface"); missionInterface->update(); }

ProfileTime(MCTimeCameraUpdate,eye->update());
→ { ZoneScopedN("GameLogic.Mission.Camera"); eye->update(); }

ProfileTime(MCTimeTerrainUpdate,land->update());
→ { ZoneScopedN("GameLogic.Mission.Terrain"); land->update(); }

ProfileTime(MCTimeWeatherUpdate,weather->update());
→ { ZoneScopedN("GameLogic.Mission.Weather"); weather->update(); }

ProfileTime(MCTimePathManagerUpdate,PathManager->update());
→ { ZoneScopedN("GameLogic.PathManager"); PathManager->update(); }

ProfileTime(MCTimeTerrainGeometry,land->geometry());
→ { ZoneScopedN("GameLogic.Mission.TerrainGeometry"); land->geometry(); }

ProfileTime(MCTimeCraterUpdate,craterManager->update());
→ { ZoneScopedN("GameLogic.Mission.Craters"); craterManager->update(); }

ProfileTime(MCTimeTXMManagerUpdate,mcTextureManager->update());
→ { ZoneScopedN("GameLogic.Mission.TextureManager"); mcTextureManager->update(); }

ProfileTime(MCTimeSensorUpdate, SensorManager->update());
→ { ZoneScopedN("GameLogic.Mission.Sensors"); SensorManager->update(); }

ProfileTime(MCTimeCollisionUpdate,ObjectManager->updateCollisions());
→ { ZoneScopedN("GameLogic.Mission.Collisions"); ObjectManager->updateCollisions(); }

ProfileTime(MCTimeMissionScript,missionBrain->execute());
→ { ZoneScopedN("GameLogic.AI.BrainExecute"); missionBrain->execute(); }
```

- [ ] **Step 2: Add zone to mission update function entry**

Find the function that contains these calls (likely `Mission::update()` or equivalent). At the top of that function body, add:
```cpp
ZoneScopedN("GameLogic.Mission.Update");
```

- [ ] **Step 3: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/mission.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "feat: replace ProfileTime call sites in mission.cpp with Tracy zones"
```

---

## Task 5: Clean gameobj.cpp — LOS zones

**Files:**
- Modify: `code/gameobj.cpp` (~lines 1843–1985)

- [ ] **Step 1: Find the LOS update function**

The file has `extern __int64 MCTimeLOSUpdate` at line 1843 and ~8 accumulation sites in the same function. Find the function signature containing line 1843 — it will be something like `void GameObject::updateLOS(...)` or similar.

- [ ] **Step 2: Replace the manual accumulation pattern**

Remove the `extern __int64 MCTimeLOSUpdate` declaration and all `MCTimeLOSUpdate += (GetCycles() - timeStart)` lines within the function.

Add a single zone at the function entry:
```cpp
void GameObject::updateLOS(/* params */)
{
    ZoneScopedN("GameLogic.LOS.Update");
    // ... rest of function unchanged
```

If the function has multiple distinct inner blocks that were timed separately, add inner zones:
```cpp
{
    ZoneScopedN("GameLogic.LOS.SectorCalc");
    // that inner block
}
```

- [ ] **Step 3: Add gos_profiler.h if not already included**

Check whether `#include "gos_profiler.h"` is present in gameobj.cpp. If not, add it with the other includes.

- [ ] **Step 4: Compile check**
```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo --target mc2 2>&1 | grep "error" | head -20
```

- [ ] **Step 5: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/gameobj.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "feat: replace MCTimeLOSUpdate with Tracy zone in gameobj.cpp"
```

---

## Task 6: Clean mclib/move.cpp — path calc zones

**Files:**
- Modify: `mclib/move.cpp` (~lines 327–334 definitions, ~lines 4528, 4747, 4820 accumulation sites, ~lines 871–1260 LOS calc sites)

- [ ] **Step 1: Remove MCTime* definitions**

Find and delete these variable definitions near line 327:
```cpp
__int64 MCTimeCalcPath1Update = 0;
__int64 MCTimeCalcPath2Update = 0;
__int64 MCTimeCalcPath3Update = 0;
__int64 MCTimeCalcPath4Update = 0;
__int64 MCTimeCalcPath5Update = 0;
```
Also find and delete `__int64 MCTimeLOSCalc` definition and any `extern __int64 MCTimeLOSCalc` declarations.

- [ ] **Step 2: Replace accumulation sites with function-entry zones**

For each function containing `MCTimeCalcPath1Update += ...` (around line 4528), add `ZoneScopedN("GameLogic.PathManager.CalcPath1")` at its entry and remove the accumulation line.

Repeat for CalcPath2 (~4747), CalcPath3 (~4820).

For functions containing `MCTimeLOSCalc += ...` (~lines 871–1260), add `ZoneScopedN("GameLogic.LOS.Calc")` at the entry of each distinct function and remove the accumulation lines.

- [ ] **Step 3: Add gos_profiler.h if not already included**

- [ ] **Step 4: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add mclib/move.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "feat: replace MCTimeCalcPath* and MCTimeLOSCalc with Tracy zones in move.cpp"
```

---

## Task 7: Clean movemgr.cpp — PathManager::update zone

**Files:**
- Modify: `code/movemgr.cpp` (~lines 199–222)

- [ ] **Step 1: Remove MCTime* externs and zero-resets**

At the top of `MovePathManager::update()` (~line 211), delete:
- The `extern __int64 MCTimePath*` and `extern __int64 MCTimeCalcGoal*` declarations (lines 199–209)
- The manual `MCTimePath1Update = 0; MCTimePath2Update = 0;` zero-reset lines at the top of the function body (lines 214–222)

- [ ] **Step 2: Add zone at function entry**

```cpp
void MovePathManager::update(void)
{
    ZoneScopedN("GameLogic.PathManager.Update");
    // ... rest of function (zero-resets removed)
```

- [ ] **Step 3: Add gos_profiler.h if not already included**

- [ ] **Step 4: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/movemgr.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "feat: replace MCTimePath* resets with Tracy zone in movemgr.cpp"
```

---

## Task 8: Clean warrior.cpp + mover.cpp — unit AI and goal calc zones

**Files:**
- Modify: `code/warrior.cpp` (~lines 2119, 2748–2878+)
- Modify: `code/mover.cpp` (~lines 4374–4960)

- [ ] **Step 1: mover.cpp — MoverControl::update zone**

Find `void MoverControl::update(MoverPtr mover)` (~line 1736). Add at entry:
```cpp
void MoverControl::update(MoverPtr mover)
{
    ZoneScopedN("GameLogic.Mover.Update");
```

- [ ] **Step 2: warrior.cpp — brain run zone**

Find the function containing `MCTimeRunBrainUpdate += (GetCycles() - startTime)` (~line 2180). Remove the extern declaration (~line 2119) and the accumulation line. Add at function entry:
```cpp
ZoneScopedN("GameLogic.AI.BrainRun");
```

- [ ] **Step 2: warrior.cpp — path accumulation zones**

Find functions containing `MCTimePath1Update += ...` (~2878), `MCTimePath2Update += ...` (~2996), `MCTimePath3Update += ...` (~3086), `MCTimePath4Update += ...` (~3201), `MCTimePath5Update += ...` (~3224). Remove the externs (~2748–2752) and accumulation lines. Add `ZoneScopedN("GameLogic.Warrior.Path1")` etc. at each function entry.

- [ ] **Step 3: mover.cpp — goal calc zones**

Find functions containing `MCTimeCalcGoal1Update += ...` (~4735), `MCTimeCalcGoal2Update += ...` (~4535), `MCTimeCalcGoal3Update += ...` (~4778), `MCTimeCalcGoal4Update += ...` (~4857), `MCTimeCalcGoal5Update += ...` (~4862), `MCTimeCalcGoal6Update += ...` (~4960). Remove externs (~4374–4379) and accumulation lines. Add:
```cpp
ZoneScopedN("GameLogic.Mover.CalcGoal1");  // etc.
```

- [ ] **Step 4: Add gos_profiler.h to both files if not already included**

- [ ] **Step 5: Compile check**
```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo --target mc2 2>&1 | grep "error" | head -20
```

- [ ] **Step 6: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/warrior.cpp code/mover.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "feat: replace MCTimeRunBrain/Path/CalcGoal accumulators with Tracy zones in warrior.cpp + mover.cpp"
```

---

## Task 9: Clean objmgr.cpp — object manager update + activation zones

**Files:**
- Modify: `code/objmgr.cpp` (~lines 1633–1695+)

This file contains the per-type update loops (mechs, vehicles, turrets, terrain objects) and is the primary spike target for on-screen appearance events.

- [ ] **Step 1: Remove MCTime* externs and accumulators**

Remove all `extern __int64 MCTime*` declarations (~lines 1633–1647) and the accumulation/zero-reset sites (~lines 1665, 1684–1695, etc.).

- [ ] **Step 2: Add per-type update zones**

Find the loops that update each object type and add zones. Example pattern:
```cpp
// Before the mechs loop:
{
    ZoneScopedN("GameLogic.Units.Mechs");
    for (/* mech loop */) { ... }
}
// Before the vehicles loop:
{
    ZoneScopedN("GameLogic.Units.Vehicles");
    for (/* vehicle loop */) { ... }
}
// Before the turrets loop:
{
    ZoneScopedN("GameLogic.Units.Turrets");
    for (/* turret loop */) { ... }
}
// Before the terrain objects loop:
{
    ZoneScopedN("GameLogic.Units.TerrainObjects");
    for (/* terrain obj loop */) { ... }
}
```

- [ ] **Step 3: Add activation-path zone (spike target)**

Find where units are activated/first-updated — typically where `isActive()` transitions or an `activate()` / `init()` call happens within the update loop. Add:
```cpp
ZoneScopedN("GameLogic.Units.Activate");
```
around that block. This is the zone that will light up during on-screen appearance spikes.

- [ ] **Step 4: Add gos_profiler.h if not already included**

- [ ] **Step 5: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/objmgr.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "feat: add Tracy per-type update and activation zones to objmgr.cpp"
```

---

## Task 10: Clean remaining files — team.cpp, mission2.cpp, mech3d.cpp, msl.cpp

**Files:**
- Modify: `code/team.cpp`
- Modify: `code/mission2.cpp`
- Modify: `mclib/mech3d.cpp` (~line 2870 MCTimeAnimationCalc definition, ~line 3657 accumulation)
- Modify: `mclib/msl.cpp`

- [ ] **Step 1: mech3d.cpp — animation calc zone**

Find `__int64 MCTimeAnimationCalc = 0` (~line 2870) — delete it.  
Find `MCTimeAnimationCalc += x` (~line 3657) inside `Mech3DAppearance::updateGeometry()` (~line 2975) — delete the accumulation. Add at function entry:
```cpp
void Mech3DAppearance::updateGeometry(void)
{
    ZoneScopedN("GameLogic.Mech3D.UpdateGeometry");
```

- [ ] **Step 2: team.cpp — commander AI zone**

Find the main per-frame team update function (search for `void.*Team.*update` or `void.*Commander.*update`). Add at entry:
```cpp
ZoneScopedN("GameLogic.AI.Team");
```
Add `#include "gos_profiler.h"` if not present.

- [ ] **Step 3: mission2.cpp — remove any remaining MCTime* usage**

Scan for any remaining `MCTime*` references and remove them (these should be externs referring to variables being deleted). Add `#include "gos_profiler.h"` if there are any new zones to add.

- [ ] **Step 4: msl.cpp — missile update zone**

Find the main missile `update()` function. Add:
```cpp
ZoneScopedN("GameLogic.Projectile.Update");
```
Add `#include "gos_profiler.h"` if not present.

- [ ] **Step 5: Commit**
```bash
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" add code/team.cpp code/mission2.cpp mclib/mech3d.cpp mclib/msl.cpp
git -C "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" commit -m "feat: add Tracy zones to team, mission2, mech3d, msl; remove remaining MCTimeAnimationCalc"
```

---

## Task 11: Full build, deploy, and Tracy verification

- [ ] **Step 1: Full clean build**
```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | tail -20
```
Expected: 0 errors. Warnings about MCTime* "undefined" mean a definition was removed but a usage wasn't. Fix those before proceeding.

- [ ] **Step 2: Deploy exe**
```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```
Expected: `diff` prints nothing (files identical).

- [ ] **Step 3: Verify Tracy in-game**

Launch Tracy GUI and connect. Load a mission. Verify:
- `GameLogic` zone appears as a top-level CPU zone each frame
- Sub-zones visible: `GameLogic.Mission.Update`, `GameLogic.PathManager`, `GameLogic.AI.BrainExecute`, `GameLogic.Mission.Collisions`, `GameLogic.Mission.Sensors`, `GameLogic.Units.Mechs`, etc.
- Pan camera to reveal new units — the `GameLogic.Units.Activate` zone should widen on the spike frame

- [ ] **Step 4: Confirm no MCTime* globals remain**
```bash
grep -rn "MCTime\|ProfileTime" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/" --include="*.cpp" --include="*.h"
```
Expected: zero results (or only comments/strings, not live code).
