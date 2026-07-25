# VISIBILITY-SEMANTICS-1 — STATUS (written retroactively by VIS-SEMANTICS-FINISH-2)

`3ab33aba` shipped `mclib/vis_semantics.{h,cpp}` and referenced this file in six
places. **It was never written.** This document supplies the measurement table
the module's comments claim as their evidence base, produced from a real
instrumented run, not from reading code.

Measured on: clean build `ceb5bb06440d` (`MC2_BUILD_GIT_DIRTY 0`), deployed to
`A:/Games/mc2-opengl/mc2-win64-abl-validate` with `--require-clean`, md5
`d4704bff640165ab25aa74bdff59504a`, 804/804 manifest rows verified.

---

## 0. THE TRAP THAT MUST BE STATED FIRST

`scripts/smoke_lib/runner.py:117` does `env.setdefault("MC2_FORCE_ALL_VISIBLE","1")`
for **every** harness run, and `mc2_vis::emitsLight / meshDrawn` plus
`mc2_cone_glow::cone_geom_hidden_by_alpha` and the stale-emitter turn guard all
short-circuit under it.

**Every default smoke or visual capture is structurally incapable of showing
this bug class.** Every run in this document set `MC2_FORCE_ALL_VISIBLE=0`
explicitly. Any future lane touching sensor / contact / fog-of-war / emission
behaviour must do the same or its evidence is void.

---

## 1. THE MEASUREMENT TABLE

Instrument: `MC2_VIS_SEMANTICS_TRACE=1`, `MC2_VIS_SEMANTICS_TRACE_EVERY=30`,
`MC2_FORCE_ALL_VISIBLE=0`, mc2_10 (night, urban), 15 s live play, 3036 samples.
The trace prints the *raw inputs* (`sensorLevel`, `meshAlpha`, `status`,
`relation`, `legacyVisible`), so all three candidate predicates are derived
offline from one run — no cross-build comparison.

Predicates compared:

| name | definition | where it lives |
|---|---|---|
| **PARKED** | `powered` (emit regardless) | what every deployed binary does today — `mover_model_shown()` returns `true` because `89f0ed4f` parked `MC2_LIGHT_MOVER_VIS_GATE` OFF |
| **ARMED-LEGACY** | `powered && legacyVisible && (sensorLevel==0 \|\| sensorLevel>=5)` | the pre-park predicate; also what `MC2_VIS_SEMANTICS=0` selects today |
| **NEW** | `powered && (friendly \|\| meshAlpha!=0)` | `mc2_vis::emitsLight()`, `MC2_VIS_SEMANTICS` default ON |

### mc2_10, 15 s live play — emitters by unit category

| class | category | distinct actors | samples | emits NEW | emits ARMED-LEGACY | emits PARKED (shipped) | meshDrawn NEW |
|---|---|---:|---:|---:|---:|---:|---:|
| mech | player_own (rel 0) | 1 | 69 | **69** | 69 | 69 | 69 |
| mech | allied (rel 1) | 2 | 138 | **138** | 138 | 138 | 138 |
| mech | enemy, full contact | 1 | 25 | 25 | 25 | 25 | 25 |
| mech | **enemy hidden (alpha 0, sl 0)** | **14** | 832 | **0** | **832** | **832** | 0 |
| mech | destroyed/disabled/shutdown | 2 | 109 | 0 | 0 | 0 | 43 |
| vehicle | allied (rel 1) | 13 | 897 | **897** | 897 | 897 | 897 |
| vehicle | enemy, full contact | 6 | 408 | 408 | 408 | 408 | 408 |
| vehicle | **enemy hidden (alpha 0, sl 0)** | **14** | 558 | **0** | **558** | **558** | 0 |

Reading of the table:

* **28 distinct hidden enemy units** (14 mechs + 14 vehicles) emit light under
  both the shipped-parked predicate and the armed-legacy one, and under neither
  is the mesh drawn. That is the position leak, quantified.
* **Zero** player-owned, allied or full-contact samples lose emission under NEW.
  All 1537 friendly samples emit, in all three predicates.
* Wrecks emit nothing under all three (`powered` fails first) but 43 of 109
  destroyed samples are still *drawn* — the intended split between job (a) and
  job (c).
* `enemy_blip_only` (sensorLevel 1..4) had no samples in this window; the
  contact fade drives alpha to 0 while sensorLevel is still 0, so the hidden
  bucket is where blip-only units land in practice. This is why an
  alpha-based mesh predicate is correct and a sensorLevel-based one is not.

### What each field actually reports — per unit class

| field | player-owned | allied | enemy full contact | enemy blip-only | unspawned / off-map | destroyed |
|---|---|---|---|---|---|---|
| `Appearance::visible` | 1 | 1 | 1 | 1 | 1 | 1 |
| `sensorLevel` | 5 (mechs) / 0 (vehicles — `code/gvehicl.cpp` never passes 5) | 5 / 0 | 0 | 0 in practice (1..4 nominal) | 0 | unchanged |
| `meshAlpha` | 255 | 255 | 255 | **0** | **0** | 255 |
| `homeTeamRelationship` | **0** | **1** | 2 | 2 | 2 | unchanged |
| `status` | NORMAL(0) | NORMAL(0) | NORMAL(0) | NORMAL(0) | NORMAL(0) | DESTROYED(2)/DISABLED(1)/SHUTDOWN(5) |

**`visible` carries zero information** — it is 1 in every column. Confirmed
against source: `code/mech.cpp:6638` sets it unconditionally every frame and
every contact branch calls `setVisibility(true,true)`.

**`sensorLevel` is unreliable in two independent ways.** It is written only
inside the `isOnGui` arm of `BattleMech::update()`, so it can hold a stale value
indefinitely; and `code/gvehicl.cpp` never passes 5 at all, so any
`sensorLevel > 4` predicate is dead for the entire vehicle class. Two live sites
had exactly that gate (`mclib/mech3d.cpp:4091`, `:5552` — the searchlight
pointLight), which is the mechanism behind the "no mech lights at all" report.

**`homeTeamRelationship` is the reliable friend/foe signal.** It is recomputed
unconditionally every frame — `code/mech.cpp:6619-6626` and
`code/gvehicl.cpp:3710-3725` — with 0 = own commander, 1 = same team different
commander (allied), 2 = everything else. `mc2_vis::friendly()` reads it, which
is why the new predicate cannot blind a player unit even when sensorLevel is
stale.

**`meshAlpha` is the real hiding mechanism.** It is the only field that changes
between "enemy you can see" and "enemy you cannot".

---

## 2. THE FOUR JOBS, AS SHIPPED

| job | predicate | friendly rule | foreign rule |
|---|---|---|---|
| (a) MESH DRAWN | `mc2_vis::meshDrawn` + `cone_geom_hidden_by_alpha` + the QueueConeGlow turn guard | always true | `meshAlpha != 0` and emitter leaf transformed this frame |
| (b) KNOWN TO PLAYER | `mc2_vis::knownToPlayer` | always true | blip (sl 1..4) counts as contact, else follows (a) |
| (c) EMITS LIGHT | `mc2_vis::emitsLight` | **always true** if powered | follows (a) exactly |
| (d) SELF LIT | `mc2_vis::selfLit` | always true | follows (a) |

Buildings and static lamps are **exempt by construction**, not by rule:
`mclib/bdactor.cpp:3522` passes `ownerIsMover=false` (so neither job-(a) guard
can fire) and building shapes are pinned at `alphaValue 0xff`. Verified visually
— see the 204907 pair in `VIS-SEMANTICS-FINISH-2-STATUS.md`, where the airbase
compound, tower lamps and runway light strings are unchanged between arms.

---

## 3. WHICH MISSION LOST MECH LIGHTS

**mc2_10, and only mc2_10 — because it is the only tier1 mission that has
light-carrying movers at all.**

`mclib/mech3d.cpp:5543` and `mclib/gvactor.cpp:3088` emit the trace for every
mover holding `pointLight || !spotlightLights_.empty()`, on the per-frame
freshness path that runs *outside* the frustum cull and *before* any `isNight`
test. Across five 15 s tier1 runs with the trace armed:

| mission | `[VIS-SEM v1]` lines | light-carrying movers |
|---|---:|---|
| mc2_01 | 0 | none |
| mc2_03 | 0 | none |
| **mc2_10** | **3036** | 28 hidden enemies + 16 friendlies + 7 full-contact |
| mc2_17 | 0 | none |
| mc2_24 | 0 | none |

The mover-light regression was therefore never mission-specific in the sense the
report implied — it was structurally unreachable in four of the five tier1
missions, and the smoke run that showed it can only have been the mc2_10 leg.
The mechanism was the `sensorLevel > 4` gate on the searchlight pointLight, not
the `mover_model_shown()` fairness predicate: in this run every friendly mover
carried `sensorLevel = 5` or `relation ∈ {0,1}`, so the fairness predicate never
suppressed a friendly in either arm.

---

## 4. WHAT THIS DOCUMENT DOES NOT ESTABLISH

* No blip-only (`sensorLevel` 1..4) sample was observed in the 15 s window; the
  blip-only column above is inferred from the contact-branch source, not
  measured. A longer soak that catches a unit mid-fade would close it.
* Vehicle `sensorLevel` semantics are read from `code/gvehicl.cpp`, not
  measured, because every vehicle sample in the run reported 0.
