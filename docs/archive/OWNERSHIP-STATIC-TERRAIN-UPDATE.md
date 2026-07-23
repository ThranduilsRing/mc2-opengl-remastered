# Static Terrain Object Update Ownership Analysis

**Branch:** nifty-mendeleev  
**Date:** 2026-06-09  
**Goal:** Identify which per-frame outputs of GameObjectManager::update are actually load-bearing, and which can be cached/event-driven.

---

## Executive Summary

**Static terrain objects (buildings, gates, turrets, trees) CANNOT be removed from the per-frame update loop.** The move map, LOS system, and game logic have **hard per-frame dependencies** that are fundamental to gameplay correctness.

### Persistent/Cached Outputs (Can stay out of update loop)
- Static prop mesh recipe
- lightDataIndex
- GPU cull records
- World bounds (cached, recalc gated by inView)
- Active flag (gated by terrain LOD block/vertex active)

### LOAD-BEARING Per-Frame Outputs (MUST stay in update loop)
- **Gate area ownership/teamID** — updated every frame (move map correctness)
- **Turret LOS/reveal position** — marked every frame (fog of war)
- **Building perimeter alarm proximity** — checked every frame
- **Building lookout tower vision** — marked every frame
- **Appearance transform/animation frame** — updated when inView
- **Destruction-state-dependent move map** — gates/bridges/walls affect pathability

---

## Detailed Findings

### 1. Gates: Hard Per-Frame Dependency on Move Map

**Location:** `code/gate.cpp:271-282`

```cpp
// RUNS EVERY FRAME, NO GATING
for (long i = 0; i < numSubAreas0; i++) {
    GlobalMoveMap[0]->setAreaOwnerWID(subAreas0[i], getWatchID());
    GlobalMoveMap[1]->setAreaOwnerWID(subAreas1[i], getWatchID());
    if (status == OBJECT_STATUS_DESTROYED) {
        GlobalMoveMap[0]->setAreaTeamID(subAreas0[i], -1);
        GlobalMoveMap[1]->setAreaTeamID(subAreas1[i], -1);
    }
    else {
        GlobalMoveMap[0]->setAreaTeamID(subAreas0[i], teamId);
        GlobalMoveMap[1]->setAreaTeamID(subAreas1[i], teamId);
    }
}
```

**Comment from code:** `"We can call update multiple times now since a gate will be updated every frame regardless AND it could also be near where the camera is looking!"`

**Ownership:** Gates MUST sync the move map area ownership + teamID state every frame. A stale gate would cause:
- Movers to get stuck in owned areas
- Pathfinding to be incorrect
- Capture logic to break

**Why it can't be event-driven:**
- Gate destruction/capture changes teamID → move map must reflect
- Gate state changes (open/closed) don't affect move map, but ownership might
- No event hook captures "gate ownership changed" reliably across edge cases

**Cannot be optimized away.**

---

### 2. Turrets: Hard Per-Frame Dependency on LOS/Vision System

**Location:** `code/turret.cpp:659-666`

```cpp
// RUNS EVERY FRAME, NO GATING
if ((turn > 1) && active && getTeam())
{
    if (turretsEnabled[getTeamId()]) 
    {
        TurretTypePtr turretType = (TurretTypePtr)ObjectManager->getObjectType(typeHandle);
        getTeam()->markSeen(position, turretType->LOSFactor);
    }
}
```

**Ownership:** Turrets MUST mark their sight range as "seen" every frame for the fog-of-war system. A stale turret would cause:
- Fogged terrain to become visible when turret is destroyed
- Enemies to disappear when turret should see them
- LOS ring to be wrong

**Why it can't be event-driven:**
- Turrets move (if mounted) → LOS changes every frame
- Turrets can be toggled active/inactive → LOS visibility changes
- LOS fade/interpolation is real-time per-frame

**Also:** Target validation (lines 594-614), parent state checking (lines 620-649), and team capture logic (lines 639-649) all run every frame and must stay in sync.

**Cannot be optimized away.**

---

### 3. Buildings: Multiple Hard Per-Frame Hooks

#### 3.1 Perimeter Alarms (code/bldng.cpp:752-776)

```cpp
// RUNS EVERY FRAME if turn != updatedTurn (but runs repeatedly)
if (moverInProximity)
{
    proximityTimer += frameLength;
}
else
{
    proximityTimer = 0.0f;
}

moverInProximity = false;

if (!GeneralAlarm && proximityTimer > 0.0f)
{
    soundSystem->playDigitalSample(PING_SFX);
    if (proximityTimer > ((BuildingTypePtr)getObjectType())->perimeterAlarmTimer)
    {
        GeneralAlarm = true;
    }
}
```

**Ownership:** Proximity timer MUST accumulate every frame. If a building skips update, alarm triggers are delayed or missed.

#### 3.2 Lookout Tower Vision (code/bldng.cpp:867-875)

```cpp
// RUNS EVERY FRAME (not gated by turn check at line 749)
if ((((BuildingTypePtr)getObjectType())->lookoutTowerRange > 0.0f) && getTeam() && 
    (!parent || 
     (parent && 
      !ObjectManager->getByWatchID(parent)->isDisabled() && 
      !ObjectManager->getByWatchID(parent)->isDestroyed())))
{
    float lookoutRange = ((BuildingTypePtr)getObjectType())->lookoutTowerRange;
    getTeam()->markSeen(position, lookoutRange);
}
```

**Ownership:** Lookout towers MUST mark their vision cone every frame. LOS would become stale.

#### 3.3 Sensor System (code/bldng.cpp:879-887)

```cpp
// RUNS EVERY FRAME (not gated)
if (parent && sensorSystem)
{
    if (ObjectManager->getByWatchID(parent)->isDisabled() || 
        ObjectManager->getByWatchID(parent)->isDestroyed())
    {
        sensorSystem->disable();
        sensorSystem->broken = true;
    }
}
```

**Ownership:** Sensor state MUST sync every frame with parent status.

#### 3.4 Power Supply Check (code/bldng.cpp:862-863)

```cpp
// RUNS EVERY FRAME (not gated)
if (powerSupply && (ObjectManager->getByWatchID(powerSupply)->getStatus() == OBJECT_STATUS_DESTROYED))
    appearance->setLightsOut(true);
```

**Ownership:** Lights-out appearance MUST update if power dies.

**Cannot optimize away all of this.**

---

### 4. Terrain Objects (Trees): Minimal Per-Frame Work

**Location:** `code/terrobj.cpp:445-545`

```cpp
// JUSTCREATED block (lines 447-479): runs once
if (getFlag(OBJECT_FLAG_JUSTCREATED)) { ... }

// Power check (lines 483-484): RUNS EVERY FRAME if powered
if (powerSupply && (ObjectManager->getByWatchID(powerSupply)->getStatus() == OBJECT_STATUS_DESTROYED))
    appearance->setLightsOut(true);

// Appearance/animation (lines 486-542): RUNS EVERY FRAME when inView
if (appearance)
{
    if (getFlag(OBJECT_FLAG_FALLING)) { ... update fall animation ... }
    appearance->setObjectParameters(...);
    bool inView = appearance->recalcBounds();
    if (inView) {
        windowsVisible = turn;
        appearance->update();
        // ... effect updates ...
    }
}
```

**Ownership:** TerrainObjects MUST update fall animations per-frame. Power supply check must run.

**Can be optimized:** The `objVertexActive` gating already works here — only in-view objects update. Further optimization would be minimal.

---

## The Gating Mechanism: objVertexActive & objBlockInfo.active

**Current state:** `code/objmgr.cpp:1729-1762`

```cpp
for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++) 
{
    if (Terrain::objBlockInfo[terrainBlock].active || (turn < 3)) 
    {
        long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
        long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
        for (long terrainObj = 0; terrainObj < numObjs; terrainObj++,objIndex++) 
        {
            if (objList[objIndex] && 
                (Terrain::objVertexActive[objList[objIndex]->getVertexNum()] || (turn < 3)) && 
                objList[objIndex]->getExists())
            {
                if (!objList[objIndex]->update()) { ... }
            }
        }
    }
}
```

**Gating is already in place:** Only objects in **active blocks** with **active vertices** are updated. This works for TerrainObjects.

**BUT Gates, Turrets, Buildings are NOT gated by this:** They're in separate lists and updated unconditionally:

- **Special Buildings** (line 1682): `for (long spBuilding = 0; spBuilding < numSpecialBuildings; ...)`
- **Gates** (line 1707): `for (long nGates = 0; nGates < numGates; ...)`
- **Turrets** (line 1848): `for (long i = 0; i < numTurrets; ...)`

So the gating is intentionally bypassed for these object types because they have persistent gameplay requirements.

---

## What Would Break If Static Objects Were Removed from Update Loop

| Feature | Impact | Severity |
|---------|--------|----------|
| Gate move map sync | Movers stuck, pathfinding wrong | CRITICAL |
| Gate area ownership | Capture logic breaks | CRITICAL |
| Turret LOS marking | Fog of war stale, invisible enemies | CRITICAL |
| Turret target validation | Dead targets stay targeted | HIGH |
| Building perimeter alarm | Alarms don't trigger | HIGH |
| Lookout tower vision | Vision cone stale | HIGH |
| Sensor system state sync | Broken sensors don't update | MEDIUM |
| Power supply state | Lights stay on when powered off | MEDIUM |
| Tree fall animation | Falls pause mid-animation | MEDIUM |
| Building destruction move map | Destroyed bridges still block | HIGH |
| Appearance animation frame | Animation stutters or freezes | MEDIUM |

---

## Per-Frame Output vs Persistent/Event-Driven Alternative

| Output | Current | Persistent | Event-Driven | Blocker |
|--------|---------|-----------|--------------|---------|
| Gate area ownership | Every frame | ✗ Stale | ✓ On capture/destroy | Move map must be accurate for all pathfinding checks |
| Gate teamID | Every frame | ✗ Stale | ✓ On capture/team-change | Same as above |
| Turret LOS mark | Every frame | ✗ Stale | ✗ Turrets move, so position changes | Turrets can move if mounted; LOS position varies per frame |
| Turret target validation | Every frame | ✓ Lazy-validated on fire | ✗ Can diverge | Could event-gate on target death, but requires more bookkeeping |
| Building alarm timer | Every frame | ✗ Pauses | ✗ Need per-frame accumulation | Timer must accumulate every frame it's armed |
| Building lookout vision | Every frame | ✗ Stale | ✓ On lookout enabled/destroyed | Vision should be per-frame if lookout moves (parent building can move? unclear) |
| Building sensor state | Every frame | ✗ Stale | ✓ On parent destroyed/disabled | Could event-hook parent destruction |
| Power supply check | Every frame | ✗ Stale | ✓ On power building destroyed | Could event-hook power building death |
| Tree fall animation | Every frame | ✓ Resumes from last frame | ✗ Frame-dependent animation | Falls can resume after skip (but looks glitchy) |
| Bridge/gate move map impact | Every frame | ✗ Stale | ✓ On destruction | Destruction is infrequent; could event-gate |
| Appearance transform | Every frame | ✓ When inView (already gated) | ✗ Visual glitches if skipped | Gating already exists; could stay |

---

## Minimal R2 Patch Shape (IF Optimization Were Forced)

### What CAN be optimized without breaking gameplay:

1. **Lazy Turret target validation** — Don't validate targets every frame; validate on fire or every N frames (would add input lag or allow stale targets briefly)

2. **Lazy Building perimeter alarm** — Timer could use lower resolution (skip some frames) without critical impact (alarms would trigger ±1 frame)

3. **Lazy Appearance updates** — Already gated by inView; could add per-object "frame budget" to skip some animation updates (would cause jerky animation)

4. **Sensor state** — Could use an event hook on parent destruction instead of per-frame check (would need new event system)

### What CANNOT be optimized:

1. **Gate move map sync** — Pathfinding/mover logic depends on fresh state every frame
2. **Turret LOS marking** — Fog of war must be accurate every frame
3. **Bridge destroyed state** — Must reflect in move map immediately

---

## Recommended Path Forward

### Phase A: Confirm Load-Bearing Requirements (Current Investigation)
- ✓ Gate move map is per-frame critical
- ✓ Turret LOS is per-frame critical
- ✓ Building logic has per-frame requirements
- [ ] Measure actual cost of these updates (profile game/editor)
- [ ] Profile which buildings/turrets are actually expensive

### Phase B: Selective Gating (Not Full Sleep)
Rather than removing statics from the update loop, apply finer gating:

1. **Don't sleep gates/turrets** — they MUST update
2. **Gate buildings by visual distance** — only update buildings in camera frustum + margin
3. **Lazy perimeter alarm** — use coarse timer resolution
4. **Lazy target validation** — validate every N frames for off-screen turrets

### Phase C: Measure Impact
- Profile before/after with real missions
- Check for gameplay breakage (gate stuck, LOS wrong, alarms missing)
- Iterate on acceptable slop tolerance

---

## References

- `code/objmgr.cpp:1656` — GameObjectManager::update() entry
- `code/gate.cpp:271` — **Gate move map sync (LOAD-BEARING)**
- `code/turret.cpp:659` — **Turret LOS marking (LOAD-BEARING)**
- `code/bldng.cpp:752` — Building perimeter alarm
- `code/bldng.cpp:867` — Lookout tower vision
- `code/terrobj.cpp:445` — Tree update (minimal per-frame work)
- `mclib/terrain.h:89` — ObjBlockInfo structure
- `code/mission.cpp:497` — objVerticesActive cleared at mission start
