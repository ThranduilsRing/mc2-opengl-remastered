# UNIT-KIND CLASS-CHECK AUDIT (migration blueprint)

> **Status:** SEEDED (Task 0 sweep + jump-gate inventory + taxonomy decision).
> Full two-bucket catalog with replacement-intent columns lands in **Task 8**.
> Anchors below located by grepping the quoted code string in the canonical
> worktree `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/` at
> `HEAD fda48be5`. Line numbers are hints — re-grep before editing.

## Purpose

`UNIT-PROFILE-SEAM-1` introduces a data-owned unit-profile seam
(`ObjectType::UnitProfile` baseline → `Mover` runtime state →
`Mover::canPerform(UnitAction)`). This audit records every
`getObjectClass() == KIND` style class-check so each can be classified as a
*gameplay* decision (candidate for migration onto a profile facet) versus a
*non-gameplay* mechanical check (serialization / spawn / FX / debug / editor),
and assigned an owning facet + future slice.

## Capability taxonomy decision (Task 0, Step 3)

**Slice 1 enumerates `CAP_JUMP` ONLY.** Capabilities are *abilities*. Sensor
concepts (ext-sensor / optics / adv-sensor / active-probe) are a **separate
"sensor" facet** and are deliberately NOT enumerated as capability bits this
slice — adding them now would build a wrong taxonomy (junk-drawer risk) before
the sensor facet exists. Structural truths (locomotion, location schema, UI,
repair, AI role) are likewise separate facets, never capability bits.

Rationale: the `CapabilitySet` bitmask is hidden behind `has()/set()/clear()`
so a future swap to a string-key registry + JSON values is internal churn-free;
but the *enum membership* is the public taxonomy and must not accrete
speculative bits. Each concept's facet is decided by this audit before a bit is
added.

## Jump-permission gate inventory (Task 0, Step 2) — the proof facet

Both gates live in the **same `TACTICAL_ORDER_JUMPTO_POINT /
TACTICAL_ORDER_JUMPTO_OBJECT` case** in `Mover::handleTacticalOrder`
(`code/mover.cpp`):

| anchor | code | role | this-slice action |
|---|---|---|---|
| `mover.cpp:3256` | `bool canJump = (getObjectClass() == BATTLEMECH);` | seeds the per-order jump permission from unit class | **REWIRED in Task 6** (gated): `mc2UnitProfileDataEnabled() ? canPerform(UnitAction::Jump) : (getObjectClass() == BATTLEMECH)`. **Risk LOW / behavior-identical gate-ON** for stock — executor stays class-gated via the `BattleMech` override, so only the capability *fact* is decoupled here, NOT the executor. Executor coupling remains until `LOCOMOTION-DATA-1`. |
| `mover.cpp:3262` | `if (getObjectClass() == BATTLEMECH) {` | guards a passability map check (`worldToCell` / `getPassable`) before allowing the jump | **NOT rewired this slice** — catalogued only. Safe to leave: a non-mech order is already rejected because `canJump` (from `:3256`, now `canPerform(Jump)`) is false and the `if (!canJump)` at `:3268` rejects it before this block matters. Decoupling belongs to `LOCOMOTION-DATA-1` alongside the jump executor. |

`canJump()` itself: declared `virtual bool canJump (void)` in `code/mover.h:1652`
(base returns false) and overridden in `code/mech.h:656`
(`return(numJumpJets > 0);`). Task 6 makes both runtime-gated wrappers over
`canPerform(UnitAction::Jump)`.

`numJumpJets` finalize sites (Task 4 targets, recorded here for completeness):
`code/mech.cpp:1568`, `:2339`, `:3084` (the three `BattleMech::init` overloads,
all `numJumpJets = 5;` inside a fall-through `case COMPONENT_FORM_JUMPJET:` — do
NOT alter that control flow), reset `:1197`/`:2235` (`= 0`), serialized
save `:9282` (`data->numJumpJets = numJumpJets;`) / load `:9353`
(`numJumpJets = data->numJumpJets;`). `numJumpJets` is **instance state**
(member of `BattleMech`, `code/mech.h:292`, serialized in `MechData`
`code/mech.h:366`) — NOT on `BattleMechType` — so the baseline is written from
the instance loadout scan into the shared type profile (Task 4).

## `getObjectClass() == KIND` sweep — `code/mover.cpp` (full)

(Other TUs swept; mover.cpp is where the gameplay-relevant jump gate lives.
Full cross-TU catalog with axis + facet + slice columns lands in Task 8.)

| anchor | code | preliminary axis (Task 8 confirms) |
|---|---|---|
| `:517` | `(mover->getObjectClass() == BATTLEMECH) && ...->inJump` | jump-state FX/anim read |
| `:801` | `run && (mover->getObjectClass() == BATTLEMECH)` | locomotion/run gating |
| `:2435` | `if (getObjectClass() == BATTLEMECH)` | (Task 8) |
| `:2450` | `if (getObjectClass() == BATTLEMECH) {` | (Task 8) |
| `:2630` | `if (getObjectClass() == BATTLEMECH) {` | (Task 8) |
| `:2649` | `else if (getObjectClass() == GROUNDVEHICLE) {` | (Task 8) |
| `:3256` | `bool canJump = (getObjectClass() == BATTLEMECH);` | **JUMP GATE — rewired (see above)** |
| `:3262` | `if (getObjectClass() == BATTLEMECH) {` | **JUMP GATE — catalogued, not rewired** |
| `:3603` | `if (getObjectClass() == BATTLEMECH)` | (Task 8) |
| `:3631` | `if (getObjectClass() == BATTLEMECH)` | (Task 8) |
| `:3698` | `if (getObjectClass() == BATTLEMECH)` | (Task 8) |
| `:4831` | `(getObjectClass() == ELEMENTAL) && target && getGroup()` | elemental-specific |
| `:5050` | `if (getObjectClass() == ELEMENTAL) {` | elemental-specific |
| `:5125` | `if (getObjectClass() == ELEMENTAL) {` | elemental-specific |
| `:5232` | `if (getObjectClass() == ELEMENTAL) {` | elemental-specific |
| `:5360` | `if (getObjectClass() == BATTLEMECH)` | jump-state read (`inJump`) |
| `:5499` | `if (getObjectClass() == ELEMENTAL) {` | elemental-specific |
| `:6066` | `!refitBuddyWID && getObjectClass() == BATTLEMECH` | refit |
| `:6556` | `if (getObjectClass() == BATTLEMECH)` | damage-state |
| `:6726` | `(getObjectClass() == BATTLEMECH) /*&& ...*/` | damage-state |
| `:6885` | `(getObjectClass() == BATTLEMECH) /*&& ...*/` | damage-state |

(Other call-site families — `controlgui.cpp`, `forcegroupbar.cpp`,
`mechicon.cpp`, `missiongui.cpp`, `warrior.cpp` — read `canJump()` / vehicle
jump range and so flow through the rewired seam automatically once gate-ON;
they are not class-checks themselves and need no migration.)

## Vehicle jump baseline note (Task 4 Step 4)

A vehicle's capability baseline stays **default-empty** unless its `.fit` carries
an optional additive `[UnitProfile] Jump = TRUE` block (the slice-1
data-extensibility proof). No redundant `set(CAP_JUMP, false)` is added. Stock
`.fit`s have no `[UnitProfile]` block (grep-confirmed) → stock parity preserved.

---

## Task 8 — full two-bucket catalog (migration blueprint)

> **Anchor drift note (re-grepped at Task 8 HEAD):** the jump gates seeded
> above as `:3256`/`:3262` are at **`mover.cpp:3264`** (`bool canJump = (getObjectClass() == BATTLEMECH);`,
> rewired) and **`mover.cpp:3270`** (`if (getObjectClass() == BATTLEMECH) {`,
> the passability guard, NOT rewired) in the current tree. Re-grep the quoted
> strings; do not trust line numbers.

Entry schema: `file / line · pattern · current behavior · axis` /
`transitional (slice-1 intent query, if any) · future facet query` /
`owning facet · slice · risk`.

### Bucket 1 — migration-relevant GAMEPLAY decisions

| anchor | pattern | current behavior · axis | transitional / future query | owning facet · slice · risk |
|---|---|---|---|---|
| `mover.cpp:3264` | `bool canJump = (getObjectClass()==BATTLEMECH)` | seeds per-order jump permission · **locomotion (jump)** | **NOW:** gated `canPerform(UnitAction::Jump)`. **FUTURE:** unchanged — fact already on capability facet | capability(fact)+locomotion(executor) · **SLICE 1 (fact); LOCOMOTION-DATA-1 (executor)** · **LOW / behavior-identical gate-ON** — executor still class-gated via `BattleMech::hasJumpExecutor()=true`; only the fact is decoupled |
| `mover.cpp:3270` | `if (getObjectClass()==BATTLEMECH) {` (passability guard) | gates `worldToCell`/`getPassable` map check before jump · **locomotion (jump)** | **NOW:** none. **FUTURE:** `if (hasJumpExecutor())` or a locomotion-facet "needs-passable-landing" query | locomotion · **LOCOMOTION-DATA-1** · **LOW** — already dead for non-mechs: `canJump` (from :3264) is false → rejected at `:3276 if(!canJump)` before this block matters |
| `mover.cpp:801` | `run && (mover->getObjectClass()==BATTLEMECH)` | run/locomotion gate · **locomotion (gait)** | none this slice | locomotion · LOCOMOTION-DATA-1 · MED (movement behavior) |

### Bucket 2 — NON-gameplay (FX / damage / refit / spawn / class-specific subsystems)

| anchor | pattern | axis / nature | bucket rationale |
|---|---|---|---|
| `mover.cpp:517` | `(mover->getObjectClass()==BATTLEMECH) && ...->inJump` | FX/anim read of jump-state | reads instance `inJump`, not a capability; FX layer, no migration |
| `mover.cpp:2435` | `if (getObjectClass()==BATTLEMECH)` | mech-specific subsystem branch | class-specific data access (mech-only members) — not a capability gate |
| `mover.cpp:2450` | `if (getObjectClass()==BATTLEMECH) {` | mech-specific subsystem branch | as above |
| `mover.cpp:2630` | `if (getObjectClass()==BATTLEMECH) {` | mech-specific branch | as above |
| `mover.cpp:2649` | `else if (getObjectClass()==GROUNDVEHICLE) {` | vehicle-specific branch | class-specific data access — not a capability gate |
| `mover.cpp:3603/3631/3698` | `if (getObjectClass()==BATTLEMECH)` | mech-specific subsystem | class-specific behavior, no facet candidate this arc |
| `mover.cpp:4831/5050/5125/5232/5499` | `getObjectClass()==ELEMENTAL` | elemental-specific (group/special) | class-specific elemental behavior; would need an elemental facet, deferred |
| `mover.cpp:5360` | `if (getObjectClass()==BATTLEMECH)` | jump-state (`inJump`) read | instance-state read, not a capability gate |
| `mover.cpp:6066` | `!refitBuddyWID && getObjectClass()==BATTLEMECH` | refit eligibility | refit subsystem — possible future "refit" facet, low priority |
| `mover.cpp:6556/6726/6885` | `(getObjectClass()==BATTLEMECH)` | damage-state / armor | damage model is mech-structural, not an ability — separate facet if ever |

(Other TUs — `controlgui.cpp`, `forcegroupbar.cpp`, `mechicon.cpp`,
`missiongui.cpp`, `warrior.cpp` — consume `canJump()` / jump range and flow
through the rewired seam automatically once gate-ON; they are not class-checks
and need no migration.)

### ACTION-EXECUTOR-REGISTRY-1 (slice 3) — executor class-check consolidated

The jump *executor* (previously the `hasJumpExecutor()` class virtual —
`Mover`=false, `BattleMech`=true) is **no longer a per-class question**. It is
now resolved from the ONE engine-side `ActionExecutorRegistry` singleton
(`code/action_executor_registry.cpp`, accessor `theActionExecutorRegistry()`),
which binds `UnitAction::Jump` → mech jump-jet executor compatible with
`LOCO_MECH_WALKER` only. `Mover::queryAction(Jump)` feeds
`isCompatible(Jump, locomotionModel())` as the `hasExecutor` fact.

`hasJumpExecutor()` is **removed** (both the base and the `BattleMech` override).
All remaining executor class-knowledge is consolidated into ONE method:

| anchor | pattern | current behavior · axis | transitional / future query | owning facet · slice · risk |
|---|---|---|---|---|
| `mover.h` `Mover::locomotionModel()` | ~~`return LOCO_WHEELED;` (base)~~ | non-jump locomotion model id · **locomotion** | **DONE (LOCOMOTION-FACET-1):** de-virtualized; now a single DATA read `getObjectType()->profile().locomotion` (defined in `mover.cpp`). Class-derivation removed. | locomotion · **LOCOMOTION-FACET-1 (slice 4) — RETIRED** · LOW |
| `mech.h` `BattleMech::locomotionModel()` | ~~`return LOCO_MECH_WALKER;`~~ | mech-walker locomotion model id · **locomotion** | **DONE (LOCOMOTION-FACET-1):** override **REMOVED**. The mech-walker value is now a data facet set once at type-load (`BattleMechType::init`, `mech.cpp` `profile().locomotion = LOCO_MECH_WALKER;`) — the last class-touch, a type-keyed data default a future `UnitDefinition` load supplies directly. | locomotion · **LOCOMOTION-FACET-1 (slice 4) — RETIRED** · LOW |

**LOCOMOTION-FACET-1 (slice 4) — DONE.** The two `locomotionModel()` class
sites are reduced to ONE data read (`Mover::locomotionModel()` →
`UnitProfile::locomotion`, a `LocomotionModel` field defaulted to `LOCO_WHEELED`).
The mech-walker value moved to `BattleMechType::init` as a type-load facet
default (last class-touch, clearly the seam a future `UnitDefinition` removes),
optionally overridden by a `.fit [UnitProfile] Locomotion=` token
(`parseLocomotionModel`, `code/locomotion_facet.h`; pure logic covered by
`tests/unit/test_unit_actions.cpp`). Values unchanged from slice 3 → gate-ON
parity. The "capability-granted vehicle" audit case still resolves
`MissingExecutor` (its wheeled/tracked data model isn't jump-compatible), and a
vehicle whose `.fit` declares `Locomotion=MechWalker` would now be compatible —
proving the model is data-owned, not class-locked.

### Completeness check (Task 8, Step 3)

Every **gameplay-bucket** `getObjectClass()==KIND` is accounted for: the one
migration-relevant jump-permission gate (`mover.cpp:3264`) is **migrated this
slice** (fact decoupled; executor coupling assigned to `LOCOMOTION-DATA-1`); the
companion passability guard (`:3270`) and the run/gait gate (`:801`) are
**assigned to `LOCOMOTION-DATA-1`**. All remaining Bucket-2 sites are
non-gameplay (class-specific subsystem access, FX, damage, refit, elemental
specials) and carry no slice obligation this arc.

**DAMAGE-SCHEMA-FACET-1 (slice 5) — DONE (additive, no class-check retired).**
Added a second data facet, `UnitProfile::damageSchema` (a `DamageSchemaId`,
`code/damage_schema.h`), defaulted to `DMG_VEHICLE_FACINGS` and set to
`DMG_MECH_8PART` once at `BattleMechType::init` (type-load), optionally overridden
by a `.fit [UnitProfile] DamageSchema=` token (`parseDamageSchema`, pure). Exposed
read-only via `Mover::damageSchema()` (mover.cpp, mirrors `locomotionModel()`).
This slice retires NO existing class-check — the armor / hit-application math
(`injureBodyLocation`, `body[]`, `bodyLocation`, `criticalSpacesReq[]`) is
untouched. The facet is the DATA SOURCE that a future `resolveHit(HitRequest)`
will pick its location model from, and that the **Phase-2 HardpointId validator**
consults via the pure `schemaHasLocation(DamageSchemaId, DamageLocationId)` /
`schemaLocationCount()` helpers to reject "loadout references RightArm but the
unit's DamageSchema has no RightArm". Nobody reads the facet at runtime yet, so
gate-ON and gate-OFF are both parity (Δdestroys +0). Pure logic covered
RED→GREEN in `tests/unit/test_unit_actions.cpp` (295 doctests pass).

**DamageSchema contract Phase-2 slices MUST honor** — enum vocabulary
`DamageSchemaId { DMG_MECH_8PART, DMG_VEHICLE_FACINGS, DMG_SQUAD, DMG_SINGLE_ACTOR }`;
logical locations `DamageLocationId` (mech: `DLOC_MECH_{HEAD,CTORSO,LTORSO,RTORSO,
LARM,RARM,LLEG,RLEG}` in `MECH_BODY_LOCATION_*` order; vehicle: `DLOC_VEH_{FRONT,
LEFT,RIGHT,REAR,TURRET}` in `bodyLocationString[]` order; squad/single:
`DLOC_BODY`). `.fit` tokens are `Mech8Part / VehicleFacings / Squad / SingleActor`
(case-insensitive; unknown/absent → type-load default). HardpointId→damage-
location joins are validated against `schemaHasLocation()`; do NOT invent a
parallel location vocabulary.
