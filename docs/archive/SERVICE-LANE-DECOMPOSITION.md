# Static Terrain Object Service-Lane Decomposition

**Branch:** nifty-mendeleev  
**Date:** 2026-06-09  
**Goal:** Split GameObjectManager::update into service lanes (render-static vs gameplay-service) to enable selective optimization without breaking gameplay.

---

## Part 1: Object Classification by Service Requirements

### Terrain Object Subtypes

| Subtype | Class | Move Map | Vision | Alarm/Sensor | Power Dep | Anim/Falling | Classification |
|---------|-------|----------|--------|--------------|-----------|--------------|-----------------|
| **TERROBJ_TREE** | TerrainObject | ✗ No | ✗ No | ✗ No | ✓ Yes (lights) | ✓ **Fall** | **Mixed:** pure render + falling-animation |
| **TERROBJ_BRIDGE** | TerrainObject | ✓ **YES** (passable/destroyed state) | ✗ No | ✗ No | ✗ No | ✗ No | **Move-map contributor** |
| **TERROBJ_FOREST** | TerrainObject | ✓ **YES** (impassable state) | ✗ No | ✗ No | ✗ No | ✗ No | **Move-map contributor** |
| **TERROBJ_WALL_HEAVY** | TerrainObject | ✓ **YES** (impassable state) | ✗ No | ✗ No | ✗ No | ✗ No | **Move-map contributor** |
| **TERROBJ_WALL_MEDIUM** | TerrainObject | ✓ **YES** (impassable state) | ✗ No | ✗ No | ✗ No | ✗ No | **Move-map contributor** |
| **TERROBJ_WALL_LIGHT** | TerrainObject | ✓ **YES** (impassable state) | ✗ No | ✗ No | ✗ No | ✗ No | **Move-map contributor** |

**Key insight:** Bridges/forests/walls affect move map, but **only on destruction** (state change). Current code doesn't check destruction state per frame; it's one-time during creation (bldng.cpp:826-856). **Can be event-driven.**

### Building Types

| Category | Criteria | Move Map | Vision (Lookout) | Alarm/Sensor | Power Dep | Parent Dep | Classification |
|----------|----------|----------|------------------|--------------|-----------|-----------|-----------------|
| **Special Building** | `perimeterAlarmRange > 0 AND timer > 0` OR `lookoutTowerRange > 0` OR `sensorRange > 0` | ✗ No | ✓ **YES** (lookout marks vision) | ✓ **YES** (proximity timer) | ✓ Yes (lights-out) | ✓ Yes (parent capture) | **Gameplay-service** (every frame) |
| **Gate Control** | Building that controls a gate (parent link) | ✗ No | ✗ No | ✗ No | ✗ No | ✗ No (self-aware) | **Pure render-static** (if no alarm/lookout/sensor) |
| **Turret Control** | Building that controls a turret (parent link) | ✗ No | ✗ No | ✗ No | ✗ No | ✗ No (self-aware) | **Pure render-static** (if no alarm/lookout/sensor) |
| **Power Generator** | `isPowerSource() == true` | ✗ No | ✗ No | ✗ No | ✗ No (self-aware) | ✗ No | **Pure render-static** |
| **Ordinary Building** | No special flags | ✗ No | ✗ No | ✗ No | ✓ Yes (power check at 862) | ✓ Yes (team capture) | **Mixed:** mostly render-static, per-frame power/team check |

**Key insight:** Only buildings with `perimeterAlarmRange > 0 OR lookoutTowerRange > 0 OR sensorRange > 0` are in `specialBuildings[]` list and MUST update every frame. All other buildings can be render-static or event-gated.

### Gate Types

| Category | Move Map | Vision | Parent Dep | Classification |
|----------|----------|--------|-----------|-----------------|
| **Gate** | ✓ **YES** (area ownership/teamID every frame) | ✗ No | ✓ Yes (lock-close on parent destroy) | **Gameplay-service** (every frame, non-negotiable) |

**Code:** `gate.cpp:271-282` runs UNCONDITIONALLY every frame. NO gating possible.

### Turret Types

| Category | Vision | Parent Dep | Animated | Classification |
|----------|--------|-----------|----------|-----------------|
| **Turret (weapon)** | ✓ **YES** (markSeen every frame) | ✓ Yes (powerdown) | ✓ Yes (turret rotation) | **Gameplay-service** (every frame) |
| **Spotlight** | ✓ **YES** (markSeen every frame) | ✓ Yes (light creation) | ✓ Yes (turret rotation) | **Gameplay-service** (every frame) |

**Code:** `turret.cpp:659-666` marks seen unconditionally. Rotation happens every frame. NO gating possible.

---

## Part 2: Current Storage (Lists)

### GameObjectManager member arrays (objmgr.h:190-221)

```cpp
TerrainObjectPtr*       terrainObjects;     // ALL terrain objects (trees, bridges, forests, walls)
BuildingPtr*            buildings;          // ALL buildings
TurretPtr*              turrets;            // ALL turrets
GatePtr*                gates;              // ALL gates
WeaponBoltPtr*          weapons;            // projectiles (not relevant)
LightPtr*               lights;             // dynamic lights (not relevant)
CarnagePtr*             carnage;            // corpses/explosions (not relevant)
ArtilleryPtr*           artillery;          // incoming fire (not relevant)
```

### Specialized lists (created during mission load)

```cpp
BuildingPtr             specialBuildings[MAX_SPECIAL_BUILDINGS];   // Subset of buildings with:
                                                                   //   - perimeterAlarmRange > 0
                                                                   //   - lookoutTowerRange > 0
                                                                   //   - sensorRange > 0
BuildingPtr             gateControls[MAX_GATE_CONTROLS];          // Buildings that control gates
BuildingPtr             turretControls[MAX_TURRET_CONTROLS];      // Buildings that control turrets
BuildingPtr             powerGenerators[MAX_POWER_GENERATORS];    // Power source buildings
```

### Terrain object block list (terrain.h:166)

```cpp
ObjBlockInfo*           objBlockInfo;       // Per-block active flag (already gates terrain obj updates)
bool*                   objVertexActive;    // Per-vertex active flag
```

**Current gating:** Terrain objects are gated by `objBlockInfo[].active` and `objVertexActive[]`. Gates, turrets, special buildings are NOT gated.

---

## Part 3: Object Counts (Estimated)

Based on handoff notes and typical missions:

| Category | Count Range | Notes |
|----------|-------------|-------|
| Total terrain objects (all types) | 100–1000+ | Varies with map size; 1K map = ~687 blocks |
| — Trees | 80% | Sparse/dense forest dependent |
| — Bridges/Forests/Walls | 5% | Rare gameplay events (destruction) |
| Special buildings | **2–20** | Perimeter alarms, lookout towers, sensors (SMALL) |
| Other buildings | 10–100 | Decor, power generators, control buildings |
| Gates | **2–20** | Choke points (SMALL, critical) |
| Turrets | **2–50** | Defense (SMALL to medium, critical) |

**Key insight:** The expensive loop (terrain objects) is dominated by **non-critical trees**. Special buildings, gates, and turrets are **small lists but load-bearing**.

---

## Part 4: Service-Lane Decomposition

### Lane A: Move-Map Sync (MUST-RUN EVERY FRAME)

**Current implementation:** Inline in GameObjectManager::update()

**Members:**
- All **Gates** (move map area ownership + teamID)
- **Bridges/Forests/Walls** that are **destroyed** (change passability)

**Per-frame work:**
```cpp
// Gate-specific (gate.cpp:271-282, RUNS EVERY FRAME)
for (int i = 0; i < numSubAreas; i++) {
    GlobalMoveMap[0]->setAreaOwnerWID(subArea0[i], getWatchID());
    GlobalMoveMap[1]->setAreaOwnerWID(subArea1[i], getWatchID());
    GlobalMoveMap[0]->setAreaTeamID(subArea0[i], (destroyed ? -1 : teamId));
    // ... same for subArea1 ...
}
```

**Cannot optimize:** Move map must be fresh every frame for pathfinding correctness.

**Cost:** Gate count is small (2–20 objects).

---

### Lane B: Vision/LOS Emitter (MUST-RUN EVERY FRAME)

**Current implementation:** Inline in object::update()

**Members:**
- All **Turrets** (markSeen with LOSFactor)
- All **Special Buildings** with lookout tower (markSeen with lookoutTowerRange)

**Per-frame work:**
```cpp
// Turret-specific (turret.cpp:659-666, RUNS EVERY FRAME)
if (active && getTeam()) {
    getTeam()->markSeen(position, turretType->LOSFactor);
}

// Building lookout-specific (bldng.cpp:867-875, RUNS EVERY FRAME)
if (lookoutTowerRange > 0.0f && getTeam() && parentOK()) {
    getTeam()->markSeen(position, lookoutTowerRange);
}
```

**Cannot optimize:** LOS is positional; turrets can move. Must be fresh every frame for fog-of-war.

**Cost:** Turret count + lookout-tower buildings = small (5–70 objects combined).

---

### Lane C: Alarm/Sensor/Proximity (MUST-RUN EVERY FRAME)

**Current implementation:** Inline in building::update()

**Members:**
- All **Special Buildings** with perimeter alarm (proximity timer)
- All **Special Buildings** with sensor range (state sync)

**Per-frame work:**
```cpp
// Perimeter alarm (bldng.cpp:755-777, RUNS EVERY FRAME if turn != updatedTurn)
if (moverInProximity) {
    proximityTimer += frameLength;  // ACCUMULATES each frame
}
moverInProximity = false;
if (!GeneralAlarm && proximityTimer > 0.0f) {
    if (proximityTimer > perimeterAlarmTimer) {
        GeneralAlarm = true;
    }
}

// Sensor system (bldng.cpp:879-887, RUNS EVERY FRAME)
if (parent && sensorSystem) {
    if (parent->isDisabled() || parent->isDestroyed()) {
        sensorSystem->disable();
    }
}
```

**Cannot optimize:** Proximity timer MUST accumulate per frame. Skipping frames = delayed alarms.

**Cost:** Special buildings with alarm/sensor = small (2–20 objects).

---

### Lane D: Parent/Power Dependencies (EVENT-DRIVEN CANDIDATE)

**Current implementation:** Inline in building::update() and turret::update()

**Members:**
- All **Buildings** (power supply check, parent capture)
- All **Turrets** (parent disabled check, team capture)
- All **Special Buildings** (same)

**Per-frame work (currently):**
```cpp
// Power supply check (bldng.cpp:862-863, RUNS EVERY FRAME)
if (powerSupply && ObjectManager->getByWatchID(powerSupply)->isDestroyed()) {
    appearance->setLightsOut(true);
}

// Building capture (bldng.cpp:891-900, RUNS EVERY FRAME)
if (parent && !parent->isDisabled() && !parent->isDestroyed() &&
    parent->getTeamId() != getTeamId()) {
    setTeamId(parent->getTeam()->getId());
}

// Turret powerdown (turret.cpp:620-635, RUNS EVERY FRAME)
if (active && parent && parent->isDestroyed()) {
    active = false;
    appearance->startActivity(TURRET_POWER_DOWN_EFFECT);
}
```

**Can optimize?** These are reactive checks, but frequency matters:
- Power death is **rare** (one-time event) → EVENT-DRIVEN
- Parent capture is **rare** (state change) → EVENT-DRIVEN
- Lights-out appearance is **visual** → can sync on power-dead event

**Optimization:** Event hook on parent/power destruction → set affected building's `needsAppearanceSync` flag → sync on next render.

**Cost:** Medium risk — appearance updates might lag 1 frame.

---

### Lane E: Pure Render-Static (CAN SKIP UPDATE ENTIRELY)

**Members:**
- **Trees** (except falling ones)
- **Power generators** (no game logic)
- **Gate control buildings** (no alarm/lookout/sensor)
- **Turret control buildings** (no alarm/lookout/sensor)
- **Ordinary buildings** (no alarm/lookout/sensor/lookout)

**Per-frame work:** NONE (appearance cached, bounds gated by inView)

**Can optimize?** YES — completely skip update() call.

**Cost:** Huge (80% of terrain objects are non-critical trees).

---

## Part 5: Event-Driven Candidates

### E1: Bridge/Forest/Wall Destruction → Move Map Update

**Current:** Every terrain object runs update(), checks destruction state once at creation (bldng.cpp:447-479).

**Event-driven alternative:**
- OnDestroyed() event → sync move map immediately
- OnCreated() event → sync move map on first creation
- NO per-frame re-sync needed (state doesn't change mid-mission except destruction)

**Blocker:** Destruction state COULD be reverted by ABL (unclear if this happens). If ABL can un-destroy objects, need per-frame check.

**Recommendation:** Assume destruction is permanent → event-driven is safe.

---

### E2: Power Supply Destruction → Lights-Out Update

**Current:** Every building checks `powerSupply->isDestroyed()` every frame (bldng.cpp:862).

**Event-driven alternative:**
- OnDestroyed(power generator) event → mark all dependent buildings as `needsAppearanceSync`
- Sync appearance on next render (appearance->update())

**Blocker:** None. Appearance update is already per-frame (when inView), so 1-frame lag is acceptable.

**Recommendation:** Safe to event-gate.

---

### E3: Building Capture → Team Update

**Current:** Every building checks `parent->getTeamId()` every frame (bldng.cpp:891-900).

**Event-driven alternative:**
- OnTeamChanged(parent building) event → update child buildings' teamId immediately
- NO per-frame check needed

**Blocker:** None. Team capture is already an event-driven action.

**Recommendation:** Safe to event-gate.

---

### E4: Parent Disabled/Destroyed → Appearance/Logic Update

**Current:** Every turret/building checks `parent->isDisabled/Destroyed()` every frame.

**Event-driven alternative:**
- OnParentStateChanged(parent building) event → update turret/gate/building state
- Turrets: trigger powerdown effect immediately
- Gates: lock closed immediately
- Buildings: update appearance immediately

**Blocker:** Rapid parent state changes (disabled/re-enabled) would need frequent event firing. But typically rare.

**Recommendation:** Safe to event-gate, but need robust event system.

---

## Part 6: Recommended Minimal R2b Patch

### Option A: Early Skip for Pure Render-Static (SAFEST)

**Approach:** Add a skip-list for terrain objects that are pure render-static.

**Implementation:**
1. Classify terrain objects at mission load:
   - `pureRenderStatic[]` = trees (not falling) + power generators + control buildings (no alarm/lookout/sensor)
   - `gameplayService[]` = everything else (move-map, vision, alarm, animated)

2. Split terrain object update loop:
   ```cpp
   // Update gameplay-service terrain objects (small list)
   for (int i = 0; i < numGameplayService; i++) {
       gameplayService[i]->update();
   }
   
   // Skip pure render-static entirely
   // (they already have persistent render recipe + bounds cached)
   ```

3. Keep all other loops unchanged:
   - Special buildings (small, must-run)
   - Gates (small, must-run)
   - Turrets (small, must-run)

**Cost:**
- New lists: `pureRenderStatic[]`, `gameplayService[]`
- One-time classification at mission load (cheap)
- Skip huge tree/building lists (fast path)

**Risk:** Low
- No gameplay change to move map, LOS, alarms
- Appearance updates already gated by inView
- Falling trees still update (classified as not-pure-render-static)

**Benefit:**
- Skip 80%+ of terrain object updates
- No event system complexity
- Can verify with counters

---

### Option B: Full Service-Lane System (AMBITIOUS, FUTURE)

**Approach:** Refactor update loop into independent service lanes.

**Structure:**
```cpp
void GameObjectManager::update() {
    // Lane A: Move-map sync (gates only)
    for (int i = 0; i < numGates; i++) {
        gates[i]->updateMoveMapOwnership();  // FAST, called unconditionally
    }
    
    // Lane B: Vision/LOS emitters (turrets + lookout buildings)
    for (int i = 0; i < numVisionEmitters; i++) {
        visionEmitters[i]->markSeen();  // FAST, called unconditionally
    }
    
    // Lane C: Alarm/Sensor (special buildings)
    for (int i = 0; i < numSpecialBuildings; i++) {
        specialBuildings[i]->updateAlarms();  // FAST, accumulate timers
    }
    
    // Lane D: Animation/Appearance (only when inView, gated)
    for (int i = 0; i < numAnimated; i++) {
        if (animated[i]->isInView() || animated[i]->isFalling()) {
            animated[i]->updateAnimation();
        }
    }
    
    // Lane E: Event-driven (parent/power state)
    // (handled by event dispatcher, not in update loop)
}
```

**Cost:**
- New refactoring, new lists
- More complex initialization
- Event system build

**Risk:** High — major refactor, risk of breakage

**Benefit:** Clear separation, future optimization path

---

## Part 7: Recommended Path: Option A (R2b Minimal Patch)

### Implementation Steps

1. **At mission load (after building special building list):**
   ```cpp
   // Classify terrain objects
   for (int i = 0; i < numTerrainObjects; i++) {
       TerrainObject* obj = terrainObjects[i];
       TerrainObjectTypePtr type = (TerrainObjectTypePtr)obj->getObjectType();
       
       if (type->subType == TERROBJ_TREE && !obj->isFalling()) {
           // Pure render-static: tree, not falling
           pureRenderStatic[numPureRenderStatic++] = i;
       } else {
           // Gameplay-service: bridges, forests, walls, falling trees
           gameplayService[numGameplayService++] = i;
       }
   }
   ```

2. **In GameObjectManager::update():**
   ```cpp
   // Update gameplay-service terrain objects only
   for (int i = 0; i < numGameplayService; i++) {
       long objIndex = gameplayService[i];
       if (objList[objIndex] && objList[objIndex]->getExists()) {
           if (!objList[objIndex]->update()) {
               objList[objIndex]->setExists(false);
           }
       }
   }
   
   // Skip pure render-static loop entirely
   // (NO UPDATE CALL for pureRenderStatic objects)
   ```

3. **Add counters for verification:**
   ```cpp
   extern long gameplayServiceTerrainCount;
   extern long pureRenderStaticTerrainCount;
   extern long skippedTerrainUpdates;
   
   skippedTerrainUpdates += (numPureRenderStatic * framesPassed);
   ```

### Verification Strategy

**Before patch:**
- Run smoke (tier1) → baseline
- Profile terrain object update cost

**After patch:**
- Run smoke (tier1) → check no crashes
- Verify move map consistency (bridge destruction works)
- Verify appearance (trees, buildings render correctly)
- Check gate/turret behavior unchanged

**Counters to verify:**
- `totalTerrainObjects` = `gameplayService` + `pureRenderStatic`
- `pureRenderStatic` ~ 80% of total
- `gameplayService` ~ 20% (bridges, forests, walls, falling trees)
- No gameplay changes to move map, LOS, alarms

---

## Part 8: Fallback if Appearance Updates Lag

If skipping pure render-static causes visual glitches (appearance stale):

**Fallback:** Keep appearance update for pure render-static, skip only game logic.

```cpp
for (int i = 0; i < numPureRenderStatic; i++) {
    long objIndex = pureRenderStatic[i];
    TerrainObject* obj = objList[objIndex];
    if (obj && obj->getExists() && obj->appearance) {
        // Skip game logic (falling tree check, power check, etc.)
        // Just update appearance transform/bounds
        appearance->recalcBounds();
        if (appearance->isInView()) {
            appearance->update();
        }
    }
}
```

**Cost:** Smaller gain (still skip most of update()), but safer.

---

## References

- `code/objmgr.cpp:1729` — Terrain object update loop (current)
- `code/objmgr.cpp:1682` — Special buildings loop
- `code/objmgr.cpp:1707` — Gates loop
- `code/objmgr.cpp:1848` — Turrets loop
- `code/bldng.h:342` — `isSpecialBuilding()` definition
- `code/gate.cpp:271` — Gate move-map sync (unconditional every frame)
- `code/turret.cpp:659` — Turret LOS mark (unconditional every frame)
- `code/bldng.cpp:752` — Perimeter alarm (per-frame accumulation)
- `code/terrobj.cpp:445` — Terrain object update (minimal per-frame work for most types)
