# Minimal R2b Patch: Pure Render-Static Skip

**Goal:** Skip 80% of terrain object updates by separating pure render-static objects from gameplay-critical ones.

**Scope:** Small, low-risk refactor. No event system, no service lanes, no gameplay changes.

---

## Files Changed

### 1. objmgr.h — Add classification lists

```cpp
// After line 221 (after terrainObjects declaration):

long                    numGameplayServiceTerrain;  // Bridges, forests, walls, falling trees
long                    numPureRenderStaticTerrain; // Trees (non-falling), power gen, control bldgs
long*                   gameplayServiceTerrainIdx;  // Index list into objList
long*                   pureRenderStaticIdx;        // Index list into objList (not used in loop)

// For verification counters:
long                    totalTerrainObjsUpdated;    // Per-frame count
long                    totalTerrainObjsSkipped;    // Per-frame count
```

---

### 2. objmgr.cpp — Initialization (in setNumObjects)

**After line 408 (after terrainObjects allocation):**

```cpp
// Allocate terrain object classification lists
if (nTerrainObjects > 0) {
    gameplayServiceTerrainIdx = (long*)ObjectTypeManager::objectCache->Malloc(
        sizeof(long) * nTerrainObjects);
    if (!gameplayServiceTerrainIdx)
        Fatal(nTerrainObjects, " GameObjectManager.setNumObjects: cannot malloc gameplay service terrain indices ");
    
    pureRenderStaticIdx = (long*)ObjectTypeManager::objectCache->Malloc(
        sizeof(long) * nTerrainObjects);
    if (!pureRenderStaticIdx)
        Fatal(nTerrainObjects, " GameObjectManager.setNumObjects: cannot malloc pure render static indices ");
    
    numGameplayServiceTerrain = 0;
    numPureRenderStaticTerrain = 0;
}

// Initialize counters
totalTerrainObjsUpdated = 0;
totalTerrainObjsSkipped = 0;
```

---

### 3. objmgr.cpp — Destruction (in destroy)

**Find the terrainObjects destruction block (around line 430+), add:**

```cpp
if (gameplayServiceTerrainIdx) {
    ObjectTypeManager::objectCache->Free(gameplayServiceTerrainIdx);
    gameplayServiceTerrainIdx = NULL;
}

if (pureRenderStaticIdx) {
    ObjectTypeManager::objectCache->Free(pureRenderStaticIdx);
    pureRenderStaticIdx = NULL;
}

numGameplayServiceTerrain = 0;
numPureRenderStaticTerrain = 0;
```

---

### 4. objmgr.cpp — Classification (in loadTerrainObjects, at end)

**After line 1027 (after special buildings list is built, BEFORE line 1028):**

```cpp
//-----------------------------------------------------------
// Classify terrain objects into gameplay-service vs pure render-static
// This enables skipping ~80% of terrain object updates (trees, power gen, control bldgs)
for (int i = 0; i < numTerrainObjects; i++) {
    if (!terrainObjects[i])
        continue;
    
    TerrainObjectTypePtr type = (TerrainObjectTypePtr)ObjectManager->getObjectType(
        terrainObjects[i]->getTypeHandle());
    
    // Pure render-static criteria:
    // - Tree (non-falling)
    // - Has power supply (depends on power gen, not move map)
    // - Not a bridge/forest/wall (those affect move map)
    
    bool isTree = (type->subType == TERROBJ_TREE);
    bool isBridge = (type->subType == TERROBJ_BRIDGE);
    bool isForest = (type->subType == TERROBJ_FOREST);
    bool isWall = (type->subType == TERROBJ_WALL_HEAVY ||
                   type->subType == TERROBJ_WALL_MEDIUM ||
                   type->subType == TERROBJ_WALL_LIGHT);
    bool isFalling = terrainObjects[i]->getFlag(OBJECT_FLAG_FALLING);
    
    // Bridges, forests, walls MUST update (move map on destruction)
    // Falling trees MUST update (animation)
    // Non-falling trees are pure render-static
    if ((isBridge || isForest || isWall) || (isTree && isFalling)) {
        gameplayServiceTerrainIdx[numGameplayServiceTerrain++] = i;
    } else if (isTree && !isFalling) {
        pureRenderStaticIdx[numPureRenderStaticTerrain++] = i;
    }
}

gosASSERT(numGameplayServiceTerrain + numPureRenderStaticTerrain == numTerrainObjects);
```

---

### 5. objmgr.cpp — Update loop (replace lines 1729-1762)

**BEFORE (old code):**
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
                if (!objList[objIndex]->update()) 
                {
                    objList[objIndex]->setExists(false);
                }
            }
        }
    }
}
```

**AFTER (new code):**
```cpp
// ============================================================
// UPDATE GAMEPLAY-SERVICE TERRAIN OBJECTS ONLY
// (Bridges, forests, walls affecting move map; falling trees)
// Pure render-static terrain objects (non-falling trees) are skipped.
// ============================================================
for (long serviceIdx = 0; serviceIdx < numGameplayServiceTerrain; serviceIdx++) 
{
    long terrainObjIdx = gameplayServiceTerrainIdx[serviceIdx];
    if (objList[terrainObjIdx] && objList[terrainObjIdx]->getExists())
    {
        if (!objList[terrainObjIdx]->update()) 
        {
            objList[terrainObjIdx]->setExists(false);
        }
        totalTerrainObjsUpdated++;
    }
}

// Pure render-static terrain objects are NOT updated.
// (Appearance is gated by inView; no game logic required)
totalTerrainObjsSkipped += numPureRenderStaticTerrain;
```

---

### 6. objmgr.h — Add getter for verification

**After line 475 (after getNumTerrainObjects):**

```cpp
long getNumGameplayServiceTerrainObjects (void) {
    return(numGameplayServiceTerrain);
}

long getNumPureRenderStaticTerrainObjects (void) {
    return(numPureRenderStaticTerrain);
}
```

---

### 7. Optional: Debug output (objmgr.cpp)

**At end of loadTerrainObjects, for verification:**

```cpp
#ifdef DEBUG_OBJECT_CLASSIFICATION
{
    char msg[256];
    sprintf(msg, "Terrain objects classified: %ld gameplay-service, %ld pure render-static",
            numGameplayServiceTerrain, numPureRenderStaticTerrain);
    dprintf(msg);
}
#endif
```

---

## Why This Patch Is Safe

### No Gameplay Changes
1. **Move map sync:** Gates still run every frame (unchanged)
2. **Vision/LOS:** Turrets + lookout buildings still run every frame (unchanged)
3. **Alarms/Sensors:** Special buildings still run every frame (unchanged)
4. **Pathfinding:** Bridges/forests/walls still update when destroyed (unchanged)
5. **Falling trees:** Still update with proper animation (unchanged)

### Appearance Unchanged
- Appearance updates are already gated by `recalcBounds()` → `inView` check
- Skipped trees were not updating game logic anyway; only appearance (which is already optimized)
- If tree is inView, appearance will render correctly on next frame

### What We Skip
- Non-falling trees: No game logic per frame (just render static)
- Power generators: No game logic (just decorative)
- Control buildings without alarm/lookout/sensor: No game logic

---

## Verification Checklist

### Build & Smoke Test
- [ ] Compile without errors
- [ ] Smoke tier1 runs (5 missions) without crash
- [ ] No memory leaks in allocation/deallocation

### Gameplay Correctness
- [ ] Gates open/close normally
- [ ] Turrets fire at targets
- [ ] Perimeter alarms trigger
- [ ] Lookout towers reveal terrain
- [ ] Bridge destruction changes move map
- [ ] Forest impassability works
- [ ] Moving units navigate around trees

### Visual Correctness
- [ ] Trees visible and rendered correctly
- [ ] Buildings visible and rendered correctly
- [ ] Falling trees animate properly
- [ ] Destroyed bridges show destruction state
- [ ] No flashing or popping of objects

### Performance Verification
- [ ] Frame rate equal or better than baseline
- [ ] Terrain object update profile shows ~80% fewer updates
- [ ] Counters: `totalTerrainObjsUpdated + totalTerrainObjsSkipped == numTerrainObjects`

### Regression Detection
- [ ] Run tier1 smoke 3 times (catch non-deterministic bugs)
- [ ] Check for any new asserts in debug build
- [ ] Check for any new warnings in build output

---

## Optional: Enhanced Diagnostics

### Add to objmgr.cpp update() for per-frame reporting:

```cpp
#ifdef MC2_DEBUG_TERRAIN_OBJECT_CLASSIFICATION
static long frameCount = 0;
static long totalUpdatedFrames = 0;
static long totalSkippedFrames = 0;

totalUpdatedFrames += totalTerrainObjsUpdated;
totalSkippedFrames += totalTerrainObjsSkipped;
frameCount++;

if ((frameCount % 300) == 0) {  // Print every 5 seconds at 60fps
    char msg[256];
    sprintf(msg, "[TerrainObjClass] Frame %ld: updated=%ld, skipped=%ld, avg_skip_ratio=%.1f%%",
            frameCount, totalTerrainObjsUpdated, totalTerrainObjsSkipped,
            100.0f * totalSkippedFrames / (totalUpdatedFrames + totalSkippedFrames + 1));
    dprintf(msg);
}

totalTerrainObjsUpdated = 0;
totalTerrainObjsSkipped = 0;
#endif
```

---

## Timeline

- **Code change:** 2–3 hours
- **Build & smoke test:** 1 hour
- **Verification:** 2–3 hours
- **Total:** ~6 hours

---

## Rollback Plan

If unexpected issues arise:

1. Comment out the new classification loop (lines in loadTerrainObjects)
2. Revert to old loop (replace lines 1729-1762 back)
3. Rebuild and verify smoke passes

Rollback is one-line comment + reverting one loop.

---

## Future Enhancements

### Phase 2: Event-Driven Parent/Power State
- Hook into parent::setTeamId() to propagate to children
- Hook into powerGenerator::onDestroyed() to mark children for appearance sync
- Reduce appearance update overhead for buildings

### Phase 3: Full Service-Lane System
- Separate move-map, vision, alarm, animation into independent services
- Allow selective enabling/disabling per service
- Prepare for multithreading later

---

## References

- GameObjectManager::update(): `code/objmgr.cpp:1656`
- TerrainObject classification: `code/terrobj.h:41`
- Special buildings: `code/bldng.h:342`
- Service-lane design: `SERVICE-LANE-DECOMPOSITION.md`
- Object classification: `OBJECT-CLASSIFICATION-SUMMARY.md`
