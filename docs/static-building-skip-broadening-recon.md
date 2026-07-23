# Static-Building Skip Broadening Recon

**Branch:** claude/terrain-gen-pcg (nifty-mendeleev worktree)
**Date:** 2026-06-10
**Goal:** Determine whether the R2b static-natural skip can be safely broadened from
"trees + Pine-named buildings" to ALL genuinely pure-render-static terrain objects.
**Status:** READ-ONLY RECON — no code changes.

---

## Baseline measurement (dense production map, frame 29700)

```
eligible=4132  natural=1081  skipped=1055  nonNatural=2011  updated=3077
GOM.TerrainObjects.ObjUpdate (x3077) = 931us  (63% of loop budget)
```

The skip fires for 1055 of 1081 `natural` candidates (trees + passing Pine-buildings).
The 2011 `nonNatural` objects are updated every frame and account for the hotspot.

---

## Q1 — What lands in `nonNatural=2011`?

The classification block (`objmgr.cpp:2319-2360`) counts `nonNaturalUpdated++` in
exactly two places:

1. **Line 2354:** `objectClass == BUILDING && isPineAppearance` but `dynamic_cast`
   to `Building*` or `BuildingTypePtr` returns null. Effectively zero in a healthy
   mission; covered as an error path.

2. **Line 2359:** The `else` branch — everything that is neither `TREE`,
   `TERRAINOBJECT subType==TERROBJ_TREE`, nor `BUILDING && isPineAppearance`.

So the 2011 `nonNatural` objects are:

| Object class / subtype | Code path | Estimated share (dense urban) | Notes |
|---|---|---|---|
| `BUILDING` with non-Pine appearance name | `objectClass==BUILDING`, not `isPineAppearance` → else → nonNatural | **~70-80%** of nonNatural | The dominant bucket. Ordinary city/industrial buildings, power generators, mechbays, turret/gate controls, etc. |
| `TERRAINOBJECT subType==TERROBJ_BRIDGE` | `objectClass==TERRAINOBJECT`, subType != TERROBJ_TREE → else → nonNatural | ~5-10% | Few per map |
| `TERRAINOBJECT subType==TERROBJ_FOREST` | same | ~5-10% | Small clusters |
| `TERRAINOBJECT subType==TERROBJ_WALL_{HEAVY,MEDIUM,LIGHT}` | same | ~5-10% | Perimeter walls |
| `TERRAINOBJECT subType==TERROBJ_NONE` | same | ~1-2% | Edge case |

The dominant sink is **non-Pine buildings** — the entire non-forest terrain map
decoration that was never covered by the `isPineAppearance` guard.

Gates and turrets are NOT in this list: they are pre-updated in the two dedicated
loops before the terrain-block walk (objmgr.cpp:2237-2260) and never reach the
`nonNatural` counter.

---

## Q2 — What does Building::update() actually do per frame?

Full trace of `Building::update()` (bldng.cpp:708-944), in order of execution:

### One-time work (JUSTCREATED flag, first call only)
- Extent-radius calculation, parent-WID lookup.
- Cost: fires once, safe to skip after turn>3.

### Per-frame work inside `if (turn != updatedTurn)` (line 750)

1. **Perimeter alarm accumulation** (bldng.cpp:752-777):
   `if (moverInProximity) proximityTimer += frameLength; moverInProximity = false;`
   Also checks `proximityTimer > perimeterAlarmTimer` → sets `GeneralAlarm`.
   **Load-bearing every frame** for perimeter alarm buildings.
   BUT: this branch is only reachable if `perimeterAlarmRange > 0 && perimeterAlarmTimer > 0`.
   Those buildings are ALREADY in `specialBuildings[]` and are pre-updated in the
   special-buildings loop before the terrain-block walk. They never reach the
   terrain-block classification.

2. **Appearance animations** (bldng.cpp:781-819):
   `updateAnimations(); appearance->setObjectParameters(...); bool inView = appearance->recalcBounds();`
   Then the `s_bldgStaticSkip` gate: if `IsStaticNow() && !FALLING` → `appearance->touch()`
   (cheap GPU resubmit), else `appearance->update()` (full hierarchy transform).
   **This is the expensive path for non-static buildings.**

3. **Activity effect** (bldng.cpp:829-836):
   `appearance->startActivity(activityEffectId, true)` when inView and not destroyed.
   Idempotent (starts only if not already running). Near-zero cost once running.

4. **Bridge destruction overlay** (bldng.cpp:840-884):
   Gated on `baseTileId != 177 && status == OBJECT_STATUS_DESTROYED`.
   Sets `baseTileId=177` on first fire (comment: "ONLY do this ONCE. MASSIVE frame rate
   hit if we do all the time."). Calls `setOverlay()`, `MarkDecalDirty()`,
   `openSubAreas()/closeSubAreas()`, `clearPathExistsTable()`.
   **One-time on first destroyed frame. After that the gate fires false every frame
   (baseTileId==177) — near-zero cost per frame.**
   NOTE: This is in `Building::update()` not `TerrainObject::update()`. The `whatAmI()==607`
   check suggests this is for building-typed bridges. See Q4.

5. **Power supply check** (bldng.cpp:887-890, OUTSIDE the `turn!=updatedTurn` guard):
   `if (powerSupply && powerObject->isDestroyed()) appearance->setLightsOut(true);`
   Runs **unconditionally every frame** (not gated on `updatedTurn`). For ordinary
   buildings with no `powerSupply` WID (the common case), this is a single null check.
   For buildings wired to a power source, this is an ObjectManager WID lookup every
   frame.

6. **Lookout tower LOS mark** (bldng.cpp:892-901):
   `if (lookoutTowerRange > 0.0f && getTeam()) getTeam()->markSeen(...)`.
   Buildings with `lookoutTowerRange > 0` are in `specialBuildings[]`, pre-updated.
   For ordinary buildings `lookoutTowerRange == 0` → branch not taken.

7. **Sensor system check** (bldng.cpp:903-912):
   Gated on `parent && sensorSystem`. Sensor buildings are in `specialBuildings[]`.
   For ordinary buildings `sensorSystem == null` → branch not taken.

8. **Parent capture** (bldng.cpp:915-927):
   `if (parent && !parent->isDisabled() && !parent->isDestroyed() && parent->getTeamId() != getTeamId()) setTeamId(...)`.
   Runs every frame for buildings with a parent WID. For buildings WITHOUT a parent
   (majority of standalone decorative buildings), this is a single null/zero check.

9. **Parent disabled/destroyed sleep** (bldng.cpp:929-940):
   `if (parent && (isDisabled || isDestroyed || !getAwake())) setAwake(false)`.
   Same parent-WID guard as above.

### Summary: per-frame costs for an ORDINARY standalone building

| Work item | Cost if no parent, no powerSupply, no special flags |
|---|---|
| `turn != updatedTurn` guard | ~1 comparison |
| `updateAnimations()` | Cheap if `bdAnimationState == -1` (no active gesture) |
| `recalcBounds()` | Full bounds recalc — NOT free |
| `IsStaticNow() → touch()` | GPU resubmit (cheap) if already registered |
| `appearance->update()` | Full hierarchy transform if not static |
| `appearance->startActivity()` | Idempotent null check |
| Destruction overlay gate | `baseTileId == 177` check → false → nothing |
| Power supply check | null check → nothing |
| Lookout/sensor/parent/capture | zero-cost null/0 checks |

**The real cost is `recalcBounds()` + `appearance->update()` for non-static buildings,
or `touch()` for already-registered static ones.** For buildings where `IsStaticNow()`
returns true, the per-frame cost is almost entirely `touch()` (GPU light resubmit
+ `bldgShape->Touch()` — a pointer write and light-env-gen comparison).

---

## Q3 — Is "99% of buildings fully static" true?

### What IsStaticNow() means

`BldgAppearance::IsStaticNow()` (bdactor.cpp:3283-3288):
```cpp
return staticReg.registered
    && staticReg.shape == bldgShape
    && !needsFullBakeNextFrame
    && isStaticEligible();
```

`isStaticEligible()` (bdactor.cpp:3045-3076) disqualifies:
- `appearType->spinMe` — rotating radar dishes, etc.
- Buildings with active animation (`bdAnimationState != -1` and gesture has animation data).
- Buildings with `drawFlash`, `destructFX`, `activity`, `activity1` active.

**"99% static" assessment:**

The claim is **approximately true but not 99% — more like 85-95%** depending on map.
The conditions that prevent a building from being static are:
- `spinMe` — radar dishes. Present on ~2-5 buildings per map. MUST update.
- Active gesture animation — only fires when the animation data is non-empty AND the
  building is in a non-idle state. For most decorative buildings `bdAnimationState == -1`.
- `activity`/`activity1` GOSFX — fires when `activityEffectId != 0xffffffff` and
  building is inView and not destroyed. After `startActivity()` fires, the effect is
  running and the flag is set. While effect is running `activity != null` → not static.
  This is a **transient** ineligibility that clears once the effect finishes.
- `drawFlash` — brief flash on capture/hit. Short-lived.
- `destructFX` — playing destruction effect. Ends quickly.

**The fraction safely skippable:** All ordinary buildings not in the above disqualifier
set, i.e., those where `IsStaticNow()` returns true. On a steady-state mid-mission
dense map with no ongoing captures/destructions: likely **85-92%** of all non-special
buildings pass `IsStaticNow()`.

**What "99% static" misses:** The `powerSupply` check (bldng.cpp:887-890) runs outside
the `updatedTurn` guard — it fires every frame even after the building is registered
static. It calls `ObjectManager->getByWatchID(powerSupply)`. For buildings with a
`powerSupply` WID, this is a per-frame WID lookup. This is NOT gated by IsStaticNow.
However the work after the lookup (`setLightsOut`) only fires once when the power
source is destroyed (idempotent after that — appearance is already lights-out).

**Verdict:** The skip is safe for any building where `IsStaticNow()` returns true,
subject to:
1. `powerSupply` WID lookup must still run per-frame, OR be event-driven first
   (see Q3 risk below).
2. `parent` capture check must still run per-frame, OR be event-driven first.
3. The `recalcBounds()` + `touch()` inside `Building::update()` currently does useful
   work (marks `windowsVisible`, sets inView for the GPU cull record). Skipping
   `update()` entirely would suppress `emitGpuCullRecord()` — see skip bypass analysis
   below.

**Critical: what does the R2b `continue` SKIP bypass?**

Looking at `objmgr.cpp:2506-2531`, the `continue` (skip) bypasses:
- `ZoneScopedN("GOM.TerrainObjects.ObjUpdate")` → `objList[objIndex]->update()`
- `terrainObjectsUpdated++`
- `emitGpuCullRecord(...)` → the GPU cull record emission.

The GPU cull record emission being skipped is **intentionally safe** for the existing
tree skip: trees are registered in the GPU static prop registry and rendered via the
static prop path which does NOT depend on the GOM cull record for rendering. But for
buildings with a `powerSupply` check, if the building is skipped, `appearance->setLightsOut()`
never fires on the frame the power source is destroyed. This is the **primary safety risk**.

---

## Q4 — Move-map contributors: per-frame or event-driven?

### TerrainObject::update() for BRIDGE/FOREST/WALL

`terrobj.cpp:745-779` — the `JUSTCREATED` block:
```cpp
if (getFlag(OBJECT_FLAG_JUSTCREATED)) {
    case TERROBJ_BRIDGE: if (!GameMap->getPassable(...)) setStatus(DESTROYED);
    case TERROBJ_FOREST/WALL: if (GameMap->getPassable(...)) setStatus(DESTROYED);
}
```
This is a **one-time init check only** (JUSTCREATED flag cleared after first call).

After that, for a non-falling BRIDGE/FOREST/WALL with no powerSupply:
- Lines 782-787: powerSupply null check → nothing (bridges/walls have no powerSupply).
- Lines 790-924: appearance path — `recalcBounds()`, `IsStaticNow() → touch()` or
  `update()`.

**There is NO per-frame passability-map write for TERROBJ_BRIDGE/FOREST/WALL.**
The passability state is set once via `openSubAreas()/closeSubAreas()` in the
`Building::update()` destruction path (bldng.cpp:874-883), triggered once when
`status == OBJECT_STATUS_DESTROYED` and `baseTileId != 177`.

**Wait** — bridges/forests/walls are `TERRAINOBJECT` objectClass, NOT `BUILDING`.
The `Building::update()` bridge path (bldng.cpp:840-884) is for building-typed bridges
(`whatAmI() == 607`). For `TERRAINOBJECT`-typed bridges, the destruction handling is
in `terrobj.cpp`. Let's trace that:

`terrobj.cpp` does NOT have its own `openSubAreas()/closeSubAreas()` call. The move-map
contribution for TERROBJ types comes from the initial `OBJECT_FLAG_TILECHANGED` path
and the JUSTCREATED subtype check that sets destroyed status, which then cascades
through the standard damage/destruction path (`setStatus(DESTROYED)` →
`GameObject::handleCollision` → passability updates via game logic).

**docs/archive/SERVICE-LANE-DECOMPOSITION.md says** (line 22): "bridges/forests/walls affect
move map, but only on destruction (state change). Current code doesn't check
destruction state per frame; it's one-time during creation (bldng.cpp:826-856).
Can be event-driven."

**Confirmed:** There is NO per-frame move-map write in `TerrainObject::update()` for
bridge/forest/wall once the JUSTCREATED flag is cleared. The move-map contribution
is a one-time event. After that, their per-frame work is identical to trees:
`recalcBounds()` + `touch()`/`update()`. They are safe to skip per-frame once
registered static.

**However:** These object types may not be using `BldgAppearance` — they use
`TreeAppearance` or a generic appearance. The `IsStaticNow()` check path for them
goes through `TreeAppearance::IsStaticNow()` (bdactor.cpp:5287). Confirmed in the
grep. So the `IsStaticNow()` gate applies to them just as it does to trees.

---

## Q5 — Broadened skip plan: `MC2_SKIP_STATIC_BUILDINGS`

### Exact broadened candidate predicate

Proposed replacement for the classification block in `objmgr.cpp:2319-2361`:

```cpp
// ---- NEW broadened predicate ----
bool isStaticBuildingCandidate = false;
bool isBridgeWallForestCandidate = false;

if (objType) {
    long objectClass = objType->getObjectClass();

    if (objectClass == TREE) {
        isTreeCandidate = true;                         // unchanged

    } else if (objectClass == TERRAINOBJECT) {
        TerrainObjectTypePtr terrainType = (TerrainObjectTypePtr)objType;
        if (terrainType->subType == TERROBJ_TREE)
            isTreeCandidate = true;                     // unchanged
        else if (terrainType->subType == TERROBJ_BRIDGE ||
                 terrainType->subType == TERROBJ_FOREST ||
                 terrainType->subType == TERROBJ_WALL_HEAVY ||
                 terrainType->subType == TERROBJ_WALL_MEDIUM ||
                 terrainType->subType == TERROBJ_WALL_LIGHT)
            isBridgeWallForestCandidate = true;         // NEW

    } else if (objectClass == BUILDING) {
        BuildingPtr building = dynamic_cast<Building*>(obj);
        BuildingTypePtr bldgType = dynamic_cast<BuildingTypePtr>(objType);
        if (building && bldgType) {
            // Exclusion set: must-update buildings
            const bool mustUpdate =
                building->isSpecialBuilding()                         ||  // already pre-updated
                (bldgType->perimeterAlarmRange > 0.0f &&
                 bldgType->perimeterAlarmTimer > 0.0f)                ||  // alarm timer
                (bldgType->lookoutTowerRange > 0.0f)                  ||  // LOS emitter
                (bldgType->sensorRange > 0.0f)                        ||  // sensor system
                bldgType->powerSource                                  ||  // power generator
                obj->getFlag(OBJECT_FLAG_CONTROLBUILDING)              ||  // control building
                obj->getFlag(OBJECT_FLAG_MECHBAY);                        // mechbay

            if (!mustUpdate)
                isStaticBuildingCandidate = true;       // NEW: all non-special buildings
        } else {
            nonNaturalUpdated++;                        // null-cast error path, unchanged
        }
    } else {
        nonNaturalUpdated++;                            // non-building, non-terrain, non-tree
    }
}
```

### Skip decision for each candidate type

```cpp
// TREE: unchanged (MC2_SKIP_STATIC_TREES)
if (isTreeCandidate) {
    staticNaturalCandidates++;
    if (skipStaticNatural && turn >= 3 && !isFalling && !isJustCreated) {
        skippedStaticNatural++;
        continue;
    }
}

// BRIDGE/WALL/FOREST: gated on MC2_SKIP_STATIC_BUILDINGS + IsStaticNow()
// Move-map contribution is one-time; safe to skip per-frame once registered.
else if (isBridgeWallForestCandidate) {
    staticBuildingCandidates++;
    const bool staticNow = appearance ? appearance->IsStaticNow() : false;
    if (skipStaticBuildings && turn >= 3 && staticNow && !isFalling && !isJustCreated) {
        skippedStaticBuildings++;
        continue;
    }
}

// BUILDING: gated on MC2_SKIP_STATIC_BUILDINGS + IsStaticNow()
else if (isStaticBuildingCandidate) {
    staticBuildingCandidates++;
    const bool staticNow = appearance ? appearance->IsStaticNow() : false;
    if (skipStaticBuildings && turn >= 3 && staticNow && !isFalling && !isJustCreated) {
        skippedStaticBuildings++;
        continue;
    }
}
```

### Must-NOT-skip set (explicit)

The following must always run through the full `update()` path:

| Object | Reason | Where handled |
|---|---|---|
| Gates | Move-map area ownership per frame (gate.cpp:273-284) | Pre-loop, not in terrain-block walk |
| Turrets | LOS markSeen per frame (turret.cpp:664-671) | Pre-loop |
| Spotlights | LOS markSeen (via turret path) | Pre-loop |
| Special buildings (alarm, lookout, sensor) | Timer accumulation, LOS, sensor state | Pre-loop via `specialBuildings[]` |
| `spinMe` buildings | `isStaticEligible()` returns false → `IsStaticNow()` returns false → not skipped | Gate via IsStaticNow |
| Power-source buildings (generators) | `bldgType->powerSource == true` → excluded by mustUpdate | Explicit mustUpdate flag |
| Control buildings | `OBJECT_FLAG_CONTROLBUILDING` | Explicit mustUpdate flag |
| Mechbay buildings | `OBJECT_FLAG_MECHBAY` | Explicit mustUpdate flag |
| `OBJECT_FLAG_FALLING` buildings | Fall animation requires update | `!isFalling` guard |
| `OBJECT_FLAG_JUSTCREATED` | First-frame init | `!isJustCreated` guard |
| Buildings with active animation/FX | `isStaticEligible()` → false → `IsStaticNow()` → false | Gate via IsStaticNow |

### Per-frame work that must be PRESERVED or moved to an event

**Risk 1: powerSupply WID check (bldng.cpp:887-890)**

This runs OUTSIDE `turn != updatedTurn` and OUTSIDE any `IsStaticNow()` gate.
If we skip `Building::update()` entirely via `continue`, this check never fires on
the frame the power source is destroyed. The building will remain lit when it should
be dark.

**Required action before skipping is safe:** The `powerSupply` check must be
either:
- (A) Run independently BEFORE the skip gate (i.e., pull it out of `update()` into
  the loop body for skipped buildings), OR
- (B) Made event-driven: when a power-source building is destroyed, iterate its
  dependents and call `appearance->setLightsOut(true)` immediately.

Option B is cleaner but requires a dependent-building list on the power source.
Option A is the safe minimal change: add to the skip body:
```cpp
// Even for skipped buildings, propagate power-out appearance.
if (auto* bldg = dynamic_cast<Building*>(obj)) {
    if (bldg->powerSupply) {
        auto* ps = ObjectManager->getByWatchID(bldg->powerSupply);
        if (ps && ps->getStatus() == OBJECT_STATUS_DESTROYED)
            obj->getAppearance()->setLightsOut(true);
    }
}
```

**Risk 2: parent capture check (bldng.cpp:915-927)**

Buildings with a parent (child buildings of a base complex) inherit their parent's
team when the parent is captured. If skipped, a child building will lag one frame in
the visual highlight flash (capture sound + team color). The capture ITSELF is
correct (parent's `setTeamId` fires immediately via its own update), but the
propagation to the child is delayed until the child is no longer skipped.

This is only a concern when `IsStaticNow()` returns true AND parent's team just
changed. The moment the parent is captured, `setTargeted(true)` fires on the parent
(which is in the `specialBuildings[]` list or the main object update), and the
building will likely exit the static state within a few frames due to `drawFlash`.

**Assessment:** The parent-capture risk is LOW for buildings that are currently
`IsStaticNow() == true`. If the parent just changed team, the child building likely
has `drawFlash` active (set by the capture flash in `setTeamId`), which makes
`isStaticEligible()` → false → `IsStaticNow()` → false → not skipped.

**Risk 3: emitGpuCullRecord bypass**

The `continue` skips `emitGpuCullRecord()` for the skipped object. For buildings
registered in the GPU static prop registry, this means the GPU cull consumer record
for that building is not emitted. This is the same situation as for trees — the
render path for static buildings uses the GPU static prop path, not the GOM cull
record, so this is safe. However: if any non-static render path reads the GOM cull
record for a building that is being skipped, that path will see a stale or absent
record.

**Required validation:** Confirm that buildings registered as GPU static props do NOT
also need the GOM cull record for any secondary purpose (e.g., picking, shadow
casting). The existing Pine-building skip (1055 skipped today) gives partial evidence
this is safe for Pine buildings. The broader skip extends the same logic to all
`IsStaticNow() == true` buildings.

### New diagnostic counters

Add to the `[STATIC_NATURAL_SKIP]` log line:

```
staticBuildingCandidates=%ld  skippedStaticBuildings=%ld
  miss_building_notStaticNow=%ld  miss_bwf_notStaticNow=%ld
  miss_building_mustUpdate=%ld  miss_building_falling=%ld  miss_building_justCreated=%ld
```

New counters in the local-variable block (objmgr.cpp:2166-2206):
```cpp
long staticBuildingCandidates = 0;
long skippedStaticBuildings = 0;
long missBuildingNotStaticNow = 0;
long missBWFNotStaticNow = 0;
long missBuildingMustUpdate = 0;
```

New env-var gate:
```cpp
const bool skipStaticBuildings = mc2SkipStaticBuildingsEnabled();  // MC2_SKIP_STATIC_BUILDINGS=1
```

Add to tier1_env_vars.md:
```
MC2_SKIP_STATIC_BUILDINGS=1  Enable per-frame skip for non-special buildings + bridge/wall/forest
                              that pass IsStaticNow(). Separate from MC2_SKIP_STATIC_TREES.
                              Default OFF. A/B measured independently.
```

### Validation plan

**A/B measurement (dense production map):**

1. Baseline: `MC2_SKIP_STATIC_TREES=1 MC2_SKIP_STATIC_BUILDINGS=0` → record
   `updated=3077`, `nonNatural=2011`, `ObjUpdate=931us`.

2. Treatment: `MC2_SKIP_STATIC_TREES=1 MC2_SKIP_STATIC_BUILDINGS=1` → expect:
   - `nonNatural` → near zero (most become `skippedStaticBuildings`)
   - `updated` drops from ~3077 to ~500-1000 (only must-update buildings + miss cases)
   - `ObjUpdate` budget drops proportionally (target: <200us)

3. Δdestroys check: run tier1 smoke, compare `MC2_DESTROY` counts. Must be +0.

**Gameplay correctness tests (manual):**

| Test | Expected | Verifies |
|---|---|---|
| Capture a base building | Flash effect + team color change on child buildings within 1-2 frames | Parent-capture propagation |
| Destroy a bridge | Pathfinding reroutes; bridge shows destroyed appearance | Move-map event, one-time update |
| Destroy a power generator | Attached buildings go dark (lights-out appearance) | powerSupply check preserved |
| Power generator destroyed, then camera pans to lit area | Buildings in newly-active blocks are dark | IsStaticNow() correctly false after lights-out |
| Building with activity effect (activityEffectId != 0xffffffff) | GOSFX still runs when inView | activity != null → not static → not skipped |
| Spinning radar dish | Still rotates | spinMe → not static → not skipped |

**Tier1 smoke:**
```
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
```
5/5 pass required before considering the gate default-on.

---

## Drift assessment vs docs/archive/SERVICE-LANE-DECOMPOSITION.md

The decomposition doc was written before the current code state. Key drifts:

1. **The doc classifies "Gate Control" and "Turret Control" buildings as pure-render-static.**
   The current code has `OBJECT_FLAG_CONTROLBUILDING` explicitly excluded from the
   Pine skip. This is conservative and correct: control buildings may have parent-
   capture logic that needs to fire. Keep them in the `mustUpdate` exclusion.

2. **The doc classifies "Power Generator" as pure-render-static** (no game logic).
   The current code has `bldgType->powerSource` explicitly excluded from the Pine
   skip. Power source buildings do NOT depend on powerSupply (they ARE the supply),
   but they are the SOURCE — when they are destroyed, their dependents must be updated.
   The power source building itself has no per-frame appearance work beyond IsStaticNow.
   **Risk:** If we skip the power-source building, the `update()` path that handles
   the power-source's own destruction (status->DESTROYED) may not fire correctly.
   Keep power sources in `mustUpdate` for safety until event-driven power propagation
   is implemented.

3. **The doc says bridge/wall/forest move-map is event-driven.** Confirmed by code
   review above: the JUSTCREATED gate and the `baseTileId==177` one-shot gate confirm
   this is a one-time event. Per-frame update after that is pure appearance work.

4. **docs/archive/MINIMAL-R2B-PATCH.md was written for TerrainObjects only** (trees, bridges,
   forests, walls). It does NOT cover `BUILDING` class objects. The plan above extends
   the skip to `BUILDING` class, which is the dominant `nonNatural` sink.

---

## Summary (12 lines)

**Is "99% static" true?** Approximately — better stated as 85-92% of non-special buildings
pass `IsStaticNow()` at steady state mid-mission. The exact fraction is unknown without
a live measurement; add `staticBuildingCandidates` vs `skippedStaticBuildings` counters.

**What per-frame work blocks blind skipping?** Two items outside `IsStaticNow()` control:
(1) `powerSupply` WID check (bldng.cpp:887-890, outside `updatedTurn` guard) — must be
preserved inline in the skip path or made event-driven; (2) `parent` capture check
(bldng.cpp:915-927) — LOW risk because captures trigger `drawFlash` which clears
`isStaticEligible()` transiently, exiting the skip naturally.

**Exact broadened predicate:** `objectClass == BUILDING && !mustUpdate && IsStaticNow()
&& !isFalling && !isJustCreated && turn >= 3`, where `mustUpdate` =
`isSpecialBuilding || perimeterAlarm || lookoutTower || sensorRange > 0 || powerSource
|| CONTROLBUILDING || MECHBAY`. Same `IsStaticNow()` predicate applied to TERROBJ
bridge/wall/forest.

**What must move to an event before skipping is fully safe?** The `powerSupply` check.
Either: (A) run it inline in the skip body (3-line add), or (B) hook into
`powerGenerator::onDestroyed()` to push `setLightsOut(true)` to all dependents.
Option A is the minimal safe approach for a default-off gate.

**Biggest risk:** Power-out visual — a building wired to a power source that is
destroyed will remain lit until it exits the skip (either `IsStaticNow()` goes false
due to some other trigger, or the powerSupply inline check is added). Add the
inline check; this is not optional.

**The one gameplay test that proves safety:** Destroy a power generator → all wired
buildings must go dark within 1 frame. If they stay lit, the powerSupply check is
missing from the skip path. This single test is the minimum correctness gate.
