# Object Classification Summary & Service Lanes

## Quick Reference: What Can Be Optimized

### Lane A: Move-Map Sync (CRITICAL, ~2-20 objects)
**MUST update every frame — no optimization possible.**
- **Gates** (gate.cpp:271-282)
  - Updates GlobalMoveMap area ownership + teamID every frame
  - Cannot skip: pathfinding depends on it
- **Bridges/Forests/Walls** (terrobj.cpp, destroyable only)
  - Can be EVENT-DRIVEN (only update on destruction, not every frame)

---

### Lane B: Vision Emitters (CRITICAL, ~5-70 objects)
**MUST update every frame — no optimization possible.**
- **Turrets** (turret.cpp:659-666)
  - Call getTeam()->markSeen() every frame for LOS
  - Cannot skip: fog-of-war breaks
- **Special Buildings w/ Lookout** (bldng.cpp:867-875)
  - Call getTeam()->markSeen() every frame
  - Cannot skip: vision becomes stale

---

### Lane C: Alarm/Sensor/Proximity (CRITICAL, ~2-20 objects)
**MUST update every frame — timer must accumulate.**
- **Special Buildings w/ Perimeter Alarm** (bldng.cpp:752-776)
  - proximityTimer += frameLength accumulates each frame
  - Cannot skip: alarms trigger late or miss entirely
- **Special Buildings w/ Sensor** (bldng.cpp:879-887)
  - State sync with parent building
  - Cannot skip: broken sensors don't update

---

### Lane D: Parent/Power Dependencies (CAN EVENT-GATE)
**Currently per-frame, but safe to event-drive.**
- **All Buildings** (power supply check, team capture)
  - bldng.cpp:862-863 (power lights-out)
  - bldng.cpp:891-900 (team capture)
  - **CAN OPTIMIZE:** Event on power destruction / parent team change
- **All Turrets** (parent disabled/destroyed)
  - turret.cpp:620-649 (parent state check, team capture)
  - **CAN OPTIMIZE:** Event on parent state change

---

### Lane E: Pure Render-Static (CAN SKIP ENTIRELY, ~80% of terrain objects)
**No per-frame game logic required.**
- **Trees (non-falling)** (~80% of terrain objects)
  - Pure render + appearance (already gated by inView)
  - No move map contribution
  - No physics/animation
  - **CAN OPTIMIZE:** Skip update() entirely
- **Power Generators** (buildings)
  - No special behavior
  - **CAN OPTIMIZE:** Skip update() entirely
- **Gate/Turret Control Buildings** (if no alarm/lookout/sensor)
  - Pure decoration or management
  - **CAN OPTIMIZE:** Skip update() entirely

---

## Object Count Distribution (Estimated)

| Category | Count | % of Total | Criticality | Optimization |
|----------|-------|-----------|-------------|--------------|
| Trees (non-falling) | 100–800 | ~80% | None | **SKIP** ✓ |
| Bridges/Forests/Walls | 5–50 | ~2% | Move map | EVENT-GATE ✓ |
| Power Generators | 2–10 | ~1% | None | **SKIP** ✓ |
| Control Buildings (non-special) | 5–20 | ~2% | None | **SKIP** ✓ |
| **Special Buildings** (alarm/lookout/sensor) | 2–20 | ~1% | **CRITICAL** | Must run every frame |
| **Gates** | 2–20 | ~1% | **CRITICAL** | Must run every frame |
| **Turrets** | 2–50 | ~2% | **CRITICAL** | Must run every frame |

**Key insight:** 80%+ of updates can be eliminated with pure render-static skip.

---

## Service-Lane Table: Per-Frame vs Event-Driven

| Service | Current | Per-Frame Work | Can Event-Gate? | Blocker | Risk |
|---------|---------|---|---|---|---|
| **Gate move-map sync** | Every frame, unconditional | setAreaOwnerWID + setAreaTeamID × numSubAreas | ✗ NO | Pathfinding fresh-state requirement | NONE (LOCKED) |
| **Turret LOS mark** | Every frame, unconditional | getTeam()->markSeen(position) | ✗ NO | Turret can move, LOS position changes per frame | NONE (LOCKED) |
| **Building lookout** | Every frame, unconditional | getTeam()->markSeen(position, range) | ✗ NO | Vision stale if skipped | NONE (LOCKED) |
| **Perimeter alarm** | Every frame if turn!=updatedTurn | proximityTimer += frameLength | ✗ NO | Timer must accumulate per frame | NONE (LOCKED) |
| **Sensor state sync** | Every frame, unconditional | Check parent disabled/destroyed | ✓ YES (event on parent state) | Rare state change; 1-frame lag acceptable | LOW |
| **Power supply check** | Every frame, unconditional | Check power building destroyed | ✓ YES (event on power destroyed) | Rare event; appearance lag 1 frame | LOW |
| **Building team capture** | Every frame, unconditional | Check parent teamId changed | ✓ YES (event on parent capture) | Rare state change; immediate or 1-frame lag | LOW |
| **Turret parent powerdown** | Every frame, unconditional | Check parent disabled/destroyed | ✓ YES (event on parent state) | Rare event; powerdown effect lag 1 frame | LOW |
| **Bridge/forest/wall destruction** | Once at creation, not per-frame | Check passable/impassable state | ✓ YES (event on destruction) | Assumption: destruction is permanent | LOW |
| **Tree fall animation** | Every frame when falling | pitchAngle -= frameLength × fallRate | ✗ NO (frame-based anim) | Animation stutters if frames skipped | NONE (only if falling) |
| **Pure render-static** | Every frame, no logic | Appearance update (gated by inView) | ✓ YES (skip entirely or defer) | Appearance could lag if inView but not updated | LOW–MED |

---

## Minimal R2b Patch: Option A (RECOMMENDED)

### What Changes
1. **At mission load:** Classify terrain objects into:
   - `pureRenderStatic[]` = trees (non-falling) + power generators + non-special control buildings
   - `gameplayService[]` = bridges, forests, walls, falling trees

2. **In GameObjectManager::update():**
   - Update only `gameplayService[]` terrain objects (small list)
   - **Skip `pureRenderStatic[]` entirely** (large list)
   - Keep gates, turrets, special buildings unchanged (already small, already critical)

3. **Add counters:**
   ```cpp
   long gameplayServiceTerrainCount;
   long pureRenderStaticTerrainCount;
   ```

### What Doesn't Change
- **Gate move-map sync** — still runs every frame
- **Turret LOS mark** — still runs every frame
- **Building alarms/lookout/sensor** — still run every frame
- **Appearance updates** — already gated by inView

### Estimated Speedup
- **Skip 80% of terrain object updates** (trees, power gen, control buildings)
- Cost: Small classification loop at mission load (negligible)
- Verification: 2–3 counters to confirm split is correct

### Risk Profile
- **Gameplay risk:** NONE (move map, LOS, alarms unchanged)
- **Visual risk:** LOW (appearance already gated by inView)
- **Appearance lag risk:** LOW (1 frame is imperceptible)

### Verification Checklist
- [ ] Smoke tier1 runs without crash
- [ ] Move map consistency (bridge destruction still works)
- [ ] Gate behavior unchanged
- [ ] Turret behavior unchanged
- [ ] Perimeter alarm triggering unchanged
- [ ] Appearance rendering unchanged (trees, buildings visible)
- [ ] Counter: `totalTerrainObjects == gameplayService + pureRenderStatic`
- [ ] Counter: `pureRenderStatic >= 0.75 * totalTerrainObjects`

---

## Fallback: If Appearance Updates Lag (Option B)

If visual glitches occur (appearance doesn't update for pure render-static):

1. Keep appearance update call for pure render-static (skip game logic only)
2. Still skips:
   - Power supply check
   - Destruction state checks
   - Fall animation logic
3. Smaller gain, but safer

Cost: ~50% terrain object update skip (better than nothing).

---

## Future: Event-Driven Architecture (Option C)

If full service-lane system is desired later:

1. Build event dispatcher for parent/power state changes
2. Refactor building/turret update into:
   - `updateMoveMap()` — gates only
   - `updateVision()` — turrets + lookout
   - `updateAlarms()` — special buildings
   - `updateAppearance()` — appearance only (gated by inView)
   - Event handlers for parent/power state
3. Full separation of concerns

Cost: Major refactor. Use after Option A is shipping.

---

## Key Dependencies (Don't Break These)

| Dependency | Critical For | Lock Reason |
|------------|-------------|------------|
| Gate::updateMoveMapOwnership() every frame | Pathfinding, mover positioning, gate control | Move map must be fresh for all pathfinding checks |
| Turret::markSeen() every frame | Fog-of-war, LOS visibility | Turrets can move; position changes per frame |
| Building::markSeen() every frame (lookout) | Vision cone accuracy | Vision must be current for target acquisition |
| Building::proximityTimer every frame | Perimeter alarm trigger timing | Timer must accumulate continuously |
| Building::sensorSystem every frame (if sensor) | Sensor state accuracy | Rare change; not event-driven yet |
| Appearance::recalcBounds() + update() when inView | Visual correctness | Already gated; no per-frame requirement if not in view |

---

## References

- Full analysis: `SERVICE-LANE-DECOMPOSITION.md`
- Object ownership: `OWNERSHIP-STATIC-TERRAIN-UPDATE.md`
- GameObjectManager::update(): `code/objmgr.cpp:1656`
- Special building classification: `code/bldng.h:342`
