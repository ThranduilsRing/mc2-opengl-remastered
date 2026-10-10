<!-- doc-status: current | reviewed:2026-10-11 -->
# brain_dispatch_harness V2

**BRAIN-DISPATCH-HARNESS-V2** — extends the v1 offline harness to exercise `executeSpecialBody_Apply` + `commitBrainIntents` against a fake `TacOrderSink`, proving the A/B effect-identity contract for all 6 effect verbs offline.

## What is now proven offline

| Claim | How proven |
|---|---|
| gate-OFF: each effect verb calls `setGeneralTacOrder` directly, correct code | `RecordingMechWarrior::setGeneralTacOrder` records into `TacOrderSink`; asserted per-verb |
| gate-ON: same orders reach `setGeneralTacOrder` via `emitBrainIntent` → `commitBrainIntents` | `getBrainRuntime()` returns real `MechBrainRuntime`; `commitBrainIntents` drains intent buffer → same sink |
| A/B identity: gate-OFF order == gate-ON committed order for all 6 verbs | same `apply_expectation` assertions pass in both gate states |
| ATTACK guards: bad-WID / self / friendly → NO order | `ConfigurableGameObjectManager` drives the three guard scenarios |
| ATTACK valid enemy → ATTACK_OBJECT(targetWID) issued once | Enemy object added to fake table with different team pointer |
| NaN-coord MOVETO → no order issued | NaN fixture + `expected_count=0` assertion |
| Once-guard: single effect verb produces exactly 1 order per Apply+Commit run | `expected_count=1` on `once-guard-powerdown` fixture |

## What still needs smoke (real mc2.exe)

- Engine survival: `Mover::handleTacticalOrder` receiving the orders from real warrior
- `ObjectManager` live lookup (real WIDs, real team assignments)
- Per-mission `dispatchEffectApplied` / `ejectEffectApplied` flags in `warrior.cpp` (engine once-guards, not in dispatch.cpp)
- Chained-body effects (CALL-CHAIN-1B, not yet implemented)

## Stub surface (symbols + count)

**7 stubs total** (6 v1 + 1 new):

| Stub file | Symbols provided | Purpose |
|---|---|---|
| `stubs/objmgr_stub.cpp` | `ObjectManager` (global `GameObjectManagerPtr`) | V1: returns nullptr for all lookups |
| `stubs/tacorder_stub.cpp` | `TacticalOrder::operator new/delete` | Custom allocator stubs |
| `stubs/inifile_stub.cpp` | `FitIniFile::open`, `seekBlock`, `readIdString` | Legacy FIT parser stubs |
| `stubs/brain_tick_stub.cpp` | `getBrainTickIndex()` | Returns 0; used by `emitBrainIntent` stamp |
| `stubs/tac_order_sink.cpp` | `g_activeSink` (global `TacOrderSink*`) | **V2 new**: recording warrior writes here |
| `stubs/recording_warrior.h` | `RecordingMechWarrior` class | **V2 new**: records orders + returns real `MechBrainRuntime` |
| `stubs/configurable_objmgr.h` | `ConfigurableGameObjectManager`, `FakeGameObject` | **V2 new**: fake object table for ATTACK guard tests |

Header stubs (shadow engine headers, no `.cpp` required):
- `warrior.h` — `MechWarrior` base (virtual `setGeneralTacOrder`, `getTeam`, `getWatchID`, `getBrainRuntime`)
- `objmgr.h` — `GameObjectManager` base + `ObjectManager` extern
- `tacordr.h` — full `TacticalOrder` + enums
- `gameobj.h` — `GameObject` + `Team` + `GameObjectPtr`
- `inifile.h` — `FitIniFile` stub
- `dstd.h`, `platform_str.h`, `stuff_vector3d.h` — engine primitive stubs

## Running

### V1 parse/bodyHasX/TraceOnly (back-compat, 19/21 pass, 2 skip):

```powershell
$env:MC2_BRAIN_DISPATCH="1"; $env:MC2_BRAIN_DISPATCH_CALL="1"
.\build64-brain-harness\RelWithDebInfo\brain_dispatch_harness.exe `
    --manifest tests/fixtures/brain_runtime/manifest.json `
    --fixture-dir tests/fixtures/brain_runtime
```

### V2 apply A/B identity (both gate states, 19/21 pass each):

```powershell
# A/B runner (runs both gate states, asserts both pass):
powershell -NoProfile -File scripts/run_brain_apply_ab.ps1

# Manual gate-OFF:
Remove-Item Env:MC2_BRAIN_INTENT_QUEUE -ErrorAction SilentlyContinue
$env:MC2_BRAIN_DISPATCH_APPLY="1"
.\build64-brain-harness\RelWithDebInfo\brain_dispatch_harness.exe `
    --manifest ... --fixture-dir ... --apply-mode

# Manual gate-ON:
$env:MC2_BRAIN_INTENT_QUEUE="1"
.\build64-brain-harness\RelWithDebInfo\brain_dispatch_harness.exe `
    --manifest ... --fixture-dir ... --apply-mode
```

### Build (harness only, ~30s, no mc2.exe relink):

```powershell
cmake -S tools/brain_dispatch_harness -B build64-brain-harness `
    -G "Visual Studio 17 2022" -A x64 `
    -DREPO_ROOT="A:/Games/mc2-brain-dispatch-harness-v2"
cmake --build build64-brain-harness --config RelWithDebInfo
```

## Apply expectation schema (manifest.json)

```json
"apply_expectation": {
  "expected_order_code": "TACTICAL_ORDER_GUARD",   // or "NONE"
  "expected_count": 1,                             // 0 = guard fires, 1+ = order(s) issued
  "expected_target_wid": 77,                       // ATTACK_OBJECT only
  "expected_waypoint": [100.0, 200.0, 0.0],        // MOVETO_POINT only
  "attack_role": "enemy",                          // "enemy"|"self"|"friendly"|"absent"
  "attack_fake_obj_wid": 77                        // WID of fake obj to insert in table
}
```

Fixtures without `apply_expectation` skip apply assertions (v1 checks only).
