# ABL Native Arg Guard — Coverage Recon 1

**Date:** 2026-06-23  
**Branch:** claude/nifty-mendeleev  
**Scope:** `code/ablmc2.cpp` — all `ABLi_pop*Ptr` call sites

---

## 1. Methodology

1. `Grep ABLi_addFunction` → binding count  
2. `Grep ABLi_popRealPtr|ABLi_popIntegerPtr|ABLi_popBooleanPtr|ABLi_popAnything` → all out-pointer sites  
3. Read each site (offset+limit) to determine whether the returned pointer is null-checked before first dereference  
4. Confirmed shipped guard shape at lines 4410-4420  
5. Classified each site by crash shape

**Binding count:** 291 `ABLi_addFunction` calls (the full binding table for all script-callable native functions).  
**Pop-pointer sites found:** 63 total (45 `ABLi_popRealPtr`, 15 `ABLi_popIntegerPtr`, 3+ `ABLi_popAnything` ignoring dummy discards).

**Shipped guard (the reference shape):**

```cpp
// ablmc2.cpp:4417 — execGetRelativePositionToObject
if (s_ablArgGuard && !relPos) {
    abl_arg_guard_log("execGetRelativePositionToObject", "relPos");
    return;
}
```

Gate: `MC2_ABL_ARG_GUARD=1`. Same pattern should be applied to every HIGH-RISK site below.

---

## 2. ABLi_pop*Ptr null return contract

From the existing comments at ablmc2.cpp:8199-8234:

> `ABLi_popRealPtr` returns NULL when `tos->address == NULL`. This occurs when the calling ABL script passes a wrong-type or missing argument. The write to the returned pointer faults without a guard.

The same applies to `ABLi_popIntegerPtr`. `ABLi_popAnything` takes a user-supplied pointer so the risk is different (caller controls the address).

---

## 3. Risk Classification Table

**Key:**
- `arg-mask`: which popped pointer is the risk (first, second, etc.)
- Crash mode: what physically happens on null

| # | Function | File:line | Arg-mask | Risk class | Crash mode | RISK | DISPOSITION |
|---|---|---|---|---|---|---|---|
| 1 | `execGetContactRange` | ablmc2.cpp:788-791 | `range` (1st), `angle` (2nd) | out-pointer write | `*range = -1.0` → write to 0x0 | HIGH | GUARD-NOW |
| 2 | `execGetObjectPosition` | ablmc2.cpp:955-957 | `coordList` (2nd) | coordinate/vector write | `coordList[0]=0` unconditionally before obj check | HIGH | GUARD-NOW |
| 3 | `execGetFireRanges` | ablmc2.cpp:1111-1116 | `ranges` (1st, only arg) | vector write | `ranges[0..3]` written unconditionally | HIGH | GUARD-NOW |
| 4 | `execOrderMoveToPoint` | ablmc2.cpp:1638-1650 | `coordList` (1st) | coordinate/vector write | `coordList[0..2]` read into Vector3D unconditionally | HIGH | GUARD-NOW |
| 5 | `execGetCurTacOrder` | ablmc2.cpp:1381-1399 | `time` (2nd), `paramList` (3rd) | out-pointer write + array write | `paramList[17]`, `paramList[18]` written unconditionally | HIGH | GUARD-NOW |
| 6 | `execGetLastTacOrder` | ablmc2.cpp:1431-1445 | `time` (2nd), `paramList` (3rd) | out-pointer write + array write | `getParamData(time, paramList)` passes null into param handler | HIGH | GUARD-NOW |
| 7 | `execInArea` | ablmc2.cpp:2957-2964 | `areaCenter` (2nd) | coordinate/vector write | `center.x = areaCenter[0]` unconditionally | HIGH | GUARD-NOW |
| 8 | `execDistanceToPosition` | ablmc2.cpp:2145-2154 | `coordList` (2nd) | coordinate read | `coordList[0]` read in `isnan()` check — null ptr deref | HIGH | GUARD-NOW |
| 9 | `execConvertCoords` | ablmc2.cpp:4905-4924 | `worldPos` (2nd), `cellPos` (3rd) | coordinate/vector read+write | both dereffed based on `convertType` branch | HIGH | GUARD-NOW |
| 10 | `execCoreMoveTo` | ablmc2.cpp:4932-4948 | `location` (1st) | coordinate read | `isnan(location[0])` — null ptr deref | HIGH | GUARD-NOW |
| 11 | `execSetCameraPosition` | ablmc2.cpp:5385-5391 | `camPos` (1st) | coordinate read | `camPos[0..2]` read unconditionally | HIGH | GUARD-NOW |
| 12 | `execSetCameraGoalPosition` | ablmc2.cpp:5413-5419 | `camPos` (1st) | coordinate read | `camPos[0..2]` read unconditionally | HIGH | GUARD-NOW |
| 13 | `execSetCameraRotation` | ablmc2.cpp:5498-5504 | `camRot` (1st) | coordinate read | `camRot[0..2]` read unconditionally | HIGH | GUARD-NOW |
| 14 | `execSetCameraGoalRotation` | ablmc2.cpp:5527-5533 | `camRot` (1st) | coordinate read | `camRot[0..2]` read unconditionally | HIGH | GUARD-NOW |
| 15 | `execSetRevealed` | ablmc2.cpp:3712-3715 | `coordList` (3rd) | coordinate read | `coordList[0..1]` read unconditionally | HIGH | GUARD-NOW |
| 16 | `execCreateInfantry` | ablmc2.cpp:3060-3075 | `worldPos` (3rd) | coordinate read | `worldPos[0..1]` read unconditionally | HIGH | GUARD-NOW |
| 17 | `execRequestHelp` | ablmc2.cpp:6034-6048 | `friendlyCenter` (2nd), `enemyCenter` (4th) | coordinate read + commander null | `friendlyCenter[0..1]` + `CurWarrior->getCommander()->getId()` without null check on `getCommander()` | HIGH | GUARD-NOW |
| 18 | `execRequestTarget` | ablmc2.cpp:6070-6082 | `center` (1st) | coordinate read + commander null | `center[0..1]` + same `getCommander()->getId()` pattern without guard | HIGH | GUARD-NOW |
| 19 | `execGetWeapons` | ablmc2.cpp:7277-7286 | `weaponList` (1st) | array write | `weaponList[numWpns++]` in loop, no null check | HIGH | GUARD-NOW |
| 20 | `execGetWeaponsStatus` | ablmc2.cpp:7313-7318 | `weaponList` (1st) | array write | passed to `getWeaponsStatus(weaponList)` — no null check | HIGH | GUARD-NOW |
| 21 | `execSetMoveArea` | ablmc2.cpp:7327-7335 | `center` (1st) | coordinate read | `center[0..1]` read unconditionally inside `isMover()` branch | HIGH | GUARD-NOW |
| 22 | `execGetMapInfo` | ablmc2.cpp:7388-7392 | `mapInfo` (1st) | array write | `mapInfo[0]`, `mapInfo[1]` written unconditionally | HIGH | GUARD-NOW |
| 23 | `execIsOffMap` | ablmc2.cpp:7398-7403 | `worldPos` (1st) | coordinate read | `worldPos[0..1]` read unconditionally | HIGH | GUARD-NOW |
| 24 | `execGetContacts` | ablmc2.cpp:596-606 | `contactList` (1st) | array write | `getContacts(contactList, ...)` then `contactList[i]` loop | MEDIUM | GUARD-LATER |
| 25 | `execGetWeaponsReady` | ablmc2.cpp:866-871 | `weaponList` (1st) | array write | passed to `getWeaponsReady(weaponList, listSize)` | MEDIUM | GUARD-LATER |
| 26 | `execGetWeaponsLocked` | ablmc2.cpp:880-885 | `weaponList` (1st) | array write | passed to `getWeaponsLocked(weaponList, listSize)` | MEDIUM | GUARD-LATER |
| 27 | `execGetWeaponsInRange` | ablmc2.cpp:894-900 | `weaponList` (1st) | array write | passed to `getWeaponsInRange(weaponList, listSize, ...)` | MEDIUM | GUARD-LATER |
| 28 | `execGetObjectFireRanges` | ablmc2.cpp:938-947 | `rangeList` (2nd) | coordinate write | `rangeList[0..2]` written both branches (no null check) | HIGH | GUARD-NOW |
| 29 | `execGetAttackers` | ablmc2.cpp:1139-1144 | `attackers` (1st) | array write | passed to `getAttackers((unsigned int*)attackers, seconds)` | MEDIUM | GUARD-LATER |
| 30 | `execGetObjects` | ablmc2.cpp:1471-1481 | `objList` (2nd) | array write | `objList[numObjects++]` in loop, no null check | MEDIUM | GUARD-LATER |
| 31 | `execGetSalvage` | ablmc2.cpp:3752-3757 | `items` (3rd), `quantities` (4th) | array write | `items[i]=-1` / `quantities[i]=-1` loop, no null check | HIGH | GUARD-NOW |
| 32 | `execGetMateList` | ablmc2.cpp:1332 | `mateList` (2nd) | array write | needs code read to confirm | MEDIUM | GUARD-LATER |
| 33 | `execGetCameraPosition` | ablmc2.cpp:5358-5366 | `camPos` (1st) | coordinate write | `camPos[0..2]` written unconditionally | HIGH | GUARD-NOW |
| 34 | `execGetCameraGoalPosition` | ablmc2.cpp:5443-5452 | `camPos` (1st) | coordinate write | `camPos[0..2]` written unconditionally | HIGH | GUARD-NOW |
| 35 | `execGetCameraRotation` | ablmc2.cpp:5471-5479 | `camRot` (1st) | coordinate write | `camRot[0..2]` written unconditionally | HIGH | GUARD-NOW |
| 36 | `execGetCameraGoalRotation` | ablmc2.cpp:5557-5566 | `camRot` (1st) | coordinate write | `camRot[0..2]` written unconditionally | HIGH | GUARD-NOW |
| 37 | `execGetCameraVel` group | ablmc2.cpp:5678,5705,5734,5765 | `camVel` (1st) | coordinate write | 4 functions, same pattern | HIGH | GUARD-NOW |
| 38 | `execGetRelativePositionToPoint` | ablmc2.cpp:4365-4378 | `pos` (1st), `relPos` (2nd) | coordinate read+write | `pos[0..2]` read + `relPos[0..2]` written; only `relPos` guarded via adjacent shipped guard pattern | HIGH | GUARD-NOW (pos unguarded) |
| 39 | `execGetTargetRelativePosition` | ablmc2.cpp:6568-6572 | `rangeOut`, `angleOut` | coordinate write | already null-checked inline (`if (rangeOut)`) | LOW | DO-NOT-TOUCH |
| 40 | `execGetRelativePositionToTarget` | ablmc2.cpp:6603-6612 | `outPoint` | coordinate write | already null-checked inline (`if (!CurObject || !outPoint)`) | LOW | DO-NOT-TOUCH |
| 41 | `execNewDistanceToPosition` | ablmc2.cpp:6647-6652 | `coordList` | coordinate read | already null-checked (`if (!coordList) return;`) | LOW | DO-NOT-TOUCH |
| 42 | `ABLi_popAnything` (dummy discards) | ablmc2.cpp:7086,7087,7094 etc. | dummy | discard | caller passes &dummy — safe | LOW | DO-NOT-TOUCH |

---

## 4. TOP 10 NEXT GUARD TARGETS (Ranked by Crash Probability in Live Missions)

Ranking criteria: (a) unconditional write/read before any null check, (b) function called frequently by AI brains (movement/position/sensor functions called every AI update), (c) known crash stack matches (Exodus `execGetRelativePositionToObject` blueprint).

| Rank | Function | Line | Why First |
|---|---|---|---|
| 1 | `execGetObjectPosition` | ~954 | Called every AI frame to get target/mover position. `coordList[0]=0.0` write before null check. Universal crash path. |
| 2 | `execInArea` | ~2957 | Area-check used in almost every brain state machine. `areaCenter[0]` dereference before null check. |
| 3 | `execOrderMoveToPoint` | ~1638 | Core movement order. `coordList[0..2]` read unconditionally. Every patrol brain. |
| 4 | `execGetContactRange` | ~788 | Called on every sensor contact. Two out-ptr writes before null check. Sensor-heavy missions. |
| 5 | `execGetFireRanges` | ~1111 | Called during weapon range queries. `ranges[0..3]` unconditional write. Combat brains. |
| 6 | `execGetRelativePositionToPoint` | ~4365 | Sibling of the shipped guard. `pos` (first pop) is unguarded while `relPos` (second) is safe. |
| 7 | `execGetObjectFireRanges` | ~938 | Per-target range lookup. `rangeList[0..2]` written in both branches with no null check. |
| 8 | `execGetSalvage` | ~3752 | Two out-ptrs (`items`, `quantities`) looped with `items[i]=-1` — crashes if either null. |
| 9 | `execConvertCoords` | ~4905 | Both `worldPos` and `cellPos` dereferred in branched paths with no null checks. |
| 10 | `execRequestHelp` | ~6034 | Two coordinate ptr dereferences PLUS `CurWarrior->getCommander()->getId()` — double crash shape: null ptr + null commander. |

---

## 5. Commander/Team Null-Deref Cluster (Separate Risk Class)

These functions call `getCommander()` or `getTeam()` without null check on the result:

- `execRequestHelp` (ablmc2.cpp:6048): `CurWarrior->getCommander()->getId()` — `getCommander()` can return null in non-player teams or stub-chassis brains.
- `execRequestTarget` (ablmc2.cpp:6082): same `getCommander()->getId()` pattern.

These are not argument-guard issues but object-lookup null dereferences. They can be fixed with a simple:
```cpp
if (CurWarrior->getCommander())
    commanderID = CurWarrior->getCommander()->getId();
```

---

## 6. Already-Safe / DO-NOT-TOUCH List

| Function | Line | Why Safe |
|---|---|---|
| `execGetRelativePositionToObject` | 4410 | SHIPPED guard — relPos guarded |
| `execGetTargetRelativePosition` | 6568 | Inline null check on `rangeOut`/`angleOut` |
| `execGetRelativePositionToTarget` | 6603 | Inline null check on `!CurObject || !outPoint` |
| `execNewDistanceToPosition` | 6647 | Explicit `if (!coordList) return;` |
| All `ABLi_popAnything(&dummy)` sites | multiple | Address of local — always valid |

---

## 7. Implementation Notes

**Guard pattern to apply** (copy of shipped form):

```cpp
if (s_ablArgGuard && !ptrVar) {
    abl_arg_guard_log("execFunctionName", "ptrVarName");
    // push a safe default if the function is expected to push a return value
    ABLi_pushInteger(0);  // or ABLi_pushReal / ABLi_pushBoolean as appropriate
    return;
}
```

For functions that write multiple out-ptrs (e.g., `execGetCurTacOrder` with `time` + `paramList`), guard all of them before any use:

```cpp
if (s_ablArgGuard && (!time || !paramList)) {
    abl_arg_guard_log("execGetCurTacOrder", "time/paramList");
    ABLi_pushInteger(0);
    return;
}
```

**Gate:** `MC2_ABL_ARG_GUARD=1` — already declared at ablmc2.cpp:114. No new header changes needed.

**Logging:** `abl_arg_guard_log` already defined at ablmc2.cpp:123. No new infrastructure needed.

---

## 8. Scope Note

This recon covers out-pointer crash shapes only. Script-supplied array index used without bounds check (e.g., `mover->inventory[weaponIndex]` where `weaponIndex` comes from ABL script) is a separate class not inventoried here. That class is lower priority since the inventory size is fixed and scripts are pre-validated against it.
