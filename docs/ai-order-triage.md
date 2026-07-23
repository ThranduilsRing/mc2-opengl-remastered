# AI order triage playbook (META-FIX design F)

Field guide for "I ordered a unit to do X and nothing happened" reports —
issues #17 (reluctant fire), #10 (scripted-move stalls), #9 (called shots
feel useless). Every row ends the same way: **escalate = run
`py -3 scripts/check-order-liveness.py --exe <your-lane>/mc2.exe`** — the
3-leg machine oracle (attack / move_stall / effectiveness).

All instrumentation below is default-OFF, byte-identical unset, and needs
`MC2_DIAG_TAGS=CONFIG,BUILD,DEVICE,COMBAT` for the JSONL events
(`MC2_DEBUG_STATE_DUMP=1` + `MC2_DIAGNOSTIC_TRACE_FILE=...`).

Static guard: `scripts/check-order-gate-coverage.py` (in check-contracts.sh)
keeps every silent fireWeapon early-return traced or declared. Behavioral
fixture: `MC2_FIXTURE_ORDER_LIVENESS` (see `docs/tier1_env_vars.md`).

---

## 1. "Decision said fire, nothing came out" — MC2_FIRE_DECISION_TRACE=1

Two COMBAT event families (`code/warrior.cpp` + `code/mech.cpp`, ISSUE-17
commit `e1eeaa05`):

### `nofire` (combatDecisionTree; one event per 0.25 s combat update while a
combat order is active and no weapon fired)

```json
{"ev":"nofire","wid":33,"tgt_wid":392,"ord":16,"result":0,
 "rd":5,"am":0,"rn":3,"lk":1,"ch":0,"hot":0,"los":1,"dist":117}
```

| Field | Meaning | Dominant-tally reading |
|---|---|---|
| `wid` | refusing unit's watch-ID | — |
| `tgt_wid` | current target watch-ID (0 = point target) | — |
| `ord` | active `curTacOrder.code` | 0 here = no combat order actually installed — see the COMMANDER/PLAYER trap below |
| `result` | `weaponsStatusResult` (weapons the tree considered fireable) | 0 with nonzero tallies = every weapon gated |
| `rd` | weapons `WEAPON_STATUS_NOT_READY` (recycle timer) | dominant `rd` = normal cadence, NOT a bug — weapons cycling |
| `am` | `OUT_OF_AMMO` | dominant `am` = ammo exhausted; expected late-mission |
| `rn` | `OUT_OF_RANGE` | dominant `rn` + `dist` >> weapon range = closing distance; persistent while STATIONARY = range computation or optimal-range regression |
| `lk` | `NOT_LOCKED` (LOS **and** fire-arc conflated) | split with `los`: `lk` high + `los:1` = fire-ARC (torso twist/facing) problem; `lk` high + `los:0` = genuine LOS blockage |
| `ch` | `NO_CHANCE` (hit odds < 1%) | dominant `ch` = to-hit math class (#9 family) |
| `hot` | `TOO_HOT` (heat shutdown threshold) | dominant `hot` = heat management, check overheat |
| `los` | direct lineOfSight probe (1/0/-1 no target) | disambiguates `lk` |
| `dist` | meters to target/point | compare against weapon optimal ranges |

Historical baseline (Probe F, mc2_24 30 s): 66 nofire events, `rd` dominant
66, `rn` 54, `lk` 1, `los:1` on all — recycle/range cadence, healthy.

### `firegate` (fireWeapon early-returns; the decision tree said FIRE but the
weapon silently skipped)

```json
{"ev":"firegate","wid":33,"widx":2,"site":"node_recycle"}
```

| `site` | Gate | Reading |
|---|---|---|
| `status` | shutdown / shutting-down / disabled / destroyed | ordered a dead or powered-down unit; check the order queue upstream |
| `body` | parked / fallen body state | mech is down — knockdown recovery stall class |
| `ready` | `isWeaponReady` re-check failed | recycle race between tree and delivery; benign unless dominant |
| `terrain` | firing position off the game map | unit walked outside `IsGameSelectTerrainPosition` — pathing class |
| `dead_target` | target destroyed between decision and delivery | benign race |
| `no_target` | neither target nor point at delivery | order plumbing dropped the target — real bug class |
| `range` | single-player range re-check failed | tree/delivery range disagreement (moving target) |
| `range_mp` | **MPlayer-server** range re-check failed | MP-only fire dead-end — was the 10th, UNTRACED gate until META-FIX-BATCH-2; if an MP "mechs won't shoot" report lands, look here FIRST |
| `shots` | weapon has 0 shots | ammo accounting split from `am` |
| `node_recycle` | per-hotspot ripple-fire recycle | normal ripple cadence (Probe F: all 16 firegate events were this) — benign unless a weapon NEVER exits it |

**Declared-uncovered:** `GroundVehicle::fireWeapon` has ZERO firegate
instrumentation (KNOWN_UNCOVERED in check-order-gate-coverage.py) — vehicle
fire dead-ends are invisible to this trace until an instrumentation slice
lands. If the report is vehicle-specific, escalate straight to the oracle.

**Escalate** = `check-order-liveness.py --only attack`.

## 2. "Scripted/ordered unit stands still forever" — MC2_BRAIN_FREEZE_TRACE=1

One stderr line per warrior per brain tick (`code/warrior.cpp` runBrain tail,
ISSUE-10 commit `ecda2201`; high volume — short runs):

```
[BRAIN_FREEZE_TRACE] wid=30 brainState=0 curTacOrder.code=0 movePath.numSteps=0
 moveState=0 hasBrain=1 orderState=0 mainGoal=0 goalWID=0 genOrder=0
 pos=(7445,-4032) lastMoveErr=0
```

| Field | Classification |
|---|---|
| `orderState` | 0=GENERAL 1=PLAYER 2=ALARM. **Pinned at 2** = escape-tile alarm loop (general orders never consumed while the alarm slot is held) — frozen-signature (a) when also static + zero hits |
| `mainGoal`/`goalWID` | goal-plan divert target (turret/gate/wall capture-divert that never completes) |
| `genOrder` | `tacOrder[GENERAL].code`: 0 after previously nonzero = brain stopped emitting (ABL stall) — frozen-signature (c), WARN until the mc2_16 baseline promotes it (legitimate patrols idle forever after completing) |
| `movePath.numSteps` | >0 with `pos` static over 30 s = path-but-not-moving (reservation/collision) — frozen-signature (b) |
| `pos` | position delta over ticks = machine-checkable movement |
| `lastMoveErr` | `LastMoveCalcErr` nonzero = classified no-path (LR path fail / area-graph misread) — a REFUSAL, not a silent stall |
| `curTacOrder.code`==0 + `numSteps`==0 after load | standing-order not re-emitted after save/load (AIFREEZE class) |

**Escalate** = `check-order-liveness.py --only move_stall` (mc2_16 oracle;
empty trace stream exits 2 — never a silent pass).

## 3. "Called shots feel useless" — the #9 leg

`MC2_CALLED_SHOT_FIX=1` (gameplay gate, default OFF) floors aimed modifiers
at Head 0.30 / Arm 0.45 / Leg 0.50; `MC2_AIMED_FIRE_FIXTURE=1` drives
aimed-HEAD orders headlessly; COMBAT `hit` events carry `"loc"` (0 = head).
A silently-dead fix gate (wiring regression) drops the fixture head-share
from ~15-27% back to the ~5% clamp world.

**Escalate** = `check-order-liveness.py --only effectiveness` (head share
>= 15% at n>=30 is the red line; n<30 degrades to WARN).

## 4. The COMMANDER-vs-PLAYER origin trap (burned the first fixture run)

`orderAttackObject` / `setGeneralTacOrder` only INSTALL the tac order (and
with it the aim location) for `ORDER_ORIGIN_COMMANDER`; with
`ORDER_ORIGIN_PLAYER` only `lastTarget` is set and fire is UNAIMED
(`code/warrior.cpp` ~5566-5574). Any headless driver, ABL brain, or test
fixture that issues PLAYER-origin orders and then wonders why `ord:0` shows
in `nofire` events or aim never applies has hit this trap. Player orders in
the real UI take a different path (tacOrderQueue). Fixtures must use
COMMANDER origin — both MC2_AIMED_FIRE_FIXTURE and MC2_FIXTURE_ORDER_LIVENESS
do.

## 5. Machine oracles (zero-launch to full)

| Tier | Command | Catches |
|---|---|---|
| commit-time | `bash scripts/check-contracts.sh` (`order_gate_coverage` row) | new untraced fireWeapon gate, deleted sentinel, deleted nofire emitter, stale KNOWN_UNCOVERED |
| opt-in smoke | any `run_smoke.py` run with `MC2_FIXTURE_ORDER_LIVENESS=1` in the env | missing/FAIL ORDER_LIVENESS_SUMMARY hard-fails the run |
| nightly | `py -3 scripts/check-order-liveness.py --exe <lane>/mc2.exe` (beside check-combat-soak.py) | fire dead-ends, move stalls, frozen signatures, #9 wiring |
