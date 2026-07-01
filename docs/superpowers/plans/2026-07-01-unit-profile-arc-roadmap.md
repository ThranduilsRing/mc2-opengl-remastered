# UNIT-PROFILE arc — roadmap (post-seam)

**Context:** UNIT-PROFILE-SEAM-1 (capability facet + `canPerform` + `.fit [UnitProfile]` data read + witness dump) is verified working on branch `claude/unit-profile-seam`. This roadmap continues toward the north-star: **data-defined unit facets + registered action executors + schema validation, with existing C++ classes as adapters.** Strangler pattern — NOT an ECS/scripting rewrite.

## North-star shape
```
UnitDefinition            = the modder-facing package (references the facets below; NOT one blob)
  ├─ UnitProfile/Combat   = rules/capabilities (what it is ALLOWED to be)   [static, type-load]
  ├─ LocomotionProfile    = movement mode (mech/tracked/wheeled/hover/infantry/jump)
  ├─ DamageSchema         = hit locations / armor / internal structure
  ├─ LoadoutProfile       = equipment + logical hardpoints (what is mounted, where)
  ├─ PresentationProfile  = model/skeleton/animgraph/VFX/audio/sockets (how it looks/sounds/animates)
  ├─ UIProfile            = icon/portrait/minimap/silhouette (how it appears in menus/HUD)
  └─ ValidationProfile    = construction rules; proves the facets agree before a mission starts
UnitRuntimeState          = what THIS instance currently is (mutable, per-mission)
Action / Executor         = how an allowed behavior actually RUNS (registry-bound)
```
**Rule of separation:** gameplay asks *what the unit can do*; presentation says *how it looks/animates while doing it*; loadout says *what is mounted where*; validators prove the three agree at load. Do **not** make `UnitProfile` carry everything — it *references* the facets. Existing `BattleMech`/`GroundVehicle` classes become executors/adapters; `getObjectClass()==KIND` branches disappear one by one (tracked in `.claude/unit-kind-classcheck-audit.md`).

**Connective tissue (shared across phases):** a **logical hardpoint** (e.g. `RightArm_Energy_1` {location=RightArm, tags=[Energy,Medium], socket=hp_ra_energy_1}) is the join key. Loadout mounts weapons on logical hardpoints; DamageSchema maps hardpoint→body location; PresentationProfile maps hardpoint→model socket/muzzle. **Never** bind a weapon directly to a model bone — a new model only needs to provide the expected sockets. Animation is **action-driven**: executors request semantic events (JumpStart/Loop/Land, FireWeapon, Death, HitReaction, Startup…); a PresentationAdapter asks the AnimationGraph, never `if BattleMech then playMechJumpAnim()`.

## Key correction over the seam
`canPerform(Jump) -> bool` becomes `queryAction(UnitAction) -> ActionAvailability`:
```
Available | MissingExecutor | MissingLocomotionFacet | MissingAnimation
| MissingResource | DisabledByDamage | InvalidForUnitSchema
```
So "profile grants Jump but only BattleMech owns the executor" is *reported*, not silently confusing. The validator yells at load: "UnitProfile grants Jump, but no compatible JumpExecutor is bound."

## Enabling piece FIRST (Phase 0)
**UNITPROFILE-OFFLINE-HARNESS-1** — extend `tests/unit/` (doctest, standalone `build64-tests`, GL/IO-free) to cover the pure logic of every slice below BEFORE engine integration, so iteration is seconds not a 10-min build→deploy→smoke loop.
- Cover: `CapabilitySet` (done), `ActionAvailability` reasoning, `ActionExecutorRegistry` compatibility resolution, `UnitProfile::validate` construction rules, facet composition.
- Rule: keep all new *logic* in leaf TUs with no engine/GL/IO drag so they compile into `mc2_tests`. Engine glue (init sites, dump) stays thin and is verified by smoke.
- Deliverable: red/green doctest cases that a slice's logic must pass before it touches `mech.cpp`/`mover.cpp`.

## Slice sequence (each: offline doctest first → engine glue → gate-OFF parity + gate-ON dump proof)
1. **UNITPROFILE-TYPELOAD-SOURCE-1** — populate `UnitProfile` ONCE at the type/data-load layer, retiring the scattered per-instance-init populates (this session proved that scattering is fragile — the mech populate was dead code after a `return`). Single source of truth.
2. **UNIT-ACTION-QUERY-RESULT-1** — replace `bool canPerform` with `queryAction(UnitAction) -> ActionAvailability`; `canJump()` etc become thin adapters over it. Gate-preserving.
3. **ACTION-EXECUTOR-REGISTRY-1** — registry binding `UnitAction -> Executor` + compatibility rules; jump/move/attack executors registered; queries resolve executor instead of asking "is BattleMech?".
4. **LOCOMOTION-FACET-1** — data-owned movement mode (mech-walker / tracked / wheeled / hover / infantry / jump-capable); locomotion facet gates jump executor compatibility.
5. **DAMAGE-SCHEMA-FACET-1** — mech body parts / vehicle facings / squad members / single actor as a data facet; `resolveHit(HitRequest)` routes through it.
6. **UNITPROFILE-VALIDATOR-1** — load-time rejection of impossible profiles (grants-jump-without-executor, squad-schema-with-mech-UI, etc). The tabletop construction-rules layer, report-then-enforce.
7. **VEHICLE-JUMP-PROOF-1** — data-granted jump on a non-mech using a *compatible executor* (gated/test-mission only). The architecture-no-longer-class-locked proof. Not vehicles-jump-by-default.

Cross-cutting, fold in as reached: **SAVELOAD-VERSIONING** (runtime state serializes by schema, not class), **EDITOR/MOD SURFACE** (expose facets in `.fit`/editor).

## Phase 2 — Presentation + Loadout facet family (after Phase 1)
Ties model/animation/icons/loadout into the SAME facet system (separate facets under `UnitDefinition`, not jammed into `UnitProfile`). Depends on Phase 1's facet framework + validator. Loadout gets its own rework, split into three layers: **ChassisProfile** (what can be mounted) → **VariantLoadout** (default/factory equipment) → **UnitInstanceLoadout** (player's customized/persistent state). Out-of-mission `RosterUnit/HangarUnit` and in-mission `MissionUnit` consume the same validated definition.

Slice order:
1. **UNIT-DEFINITION-SCHEMA-1** — top-level `UnitDefinition` referencing profile/loadout/presentation/ui/damage/locomotion facets by id. No behavior change; establishes the umbrella + loader.
2. **PRESENTATION-PROFILE-1** — model/skeleton/animgraph/scale/wreck/selection-radius + `[PresentationSockets]` loaded and validated; no behavior change (legacy render path still authoritative, reads via the facet).
3. **HARDPOINT-SOCKET-VALIDATOR-1** — every loadout logical hardpoint MUST resolve to a DamageSchema location AND a PresentationProfile socket; validator emits actionable errors (missing socket / missing location / weapon-tag-vs-hardpoint mismatch).
4. **UNIT-VARIANT-LOADOUT-1** — split ChassisProfile / VariantLoadout / UnitInstanceLoadout; equipment expressed as `Weapon@LogicalHardpoint`, never `Weapon@modelBone`.
5. **ACTION-ANIMATION-BINDINGS-1** — executors request semantic animation tags (JumpStart/Loop/Land, FireWeapon, Death, HitReaction, Startup, Shutdown, Idle, Move/Stop/Turn); PresentationAdapter resolves via AnimationGraph; validator flags missing tags or uses explicit fallback. Kills `if BattleMech then playMechJumpAnim()`.
6. **UNIT-EDITOR-SURFACE-1** — editor lists/loads/validates `UnitDefinition`s (in + out of mission).
7. **NON-MECH-PROOF-UNIT-1** — stand up a test unit purely from data facets (model + loadout + actions + presentation), proving the model/loadout/action path is not class-locked. The Phase-2 analogue of VEHICLE-JUMP-PROOF-1.

Modder north-star workflow (the acceptance test for Phase 2):
```
mods/MyUnit/  unit.fit  presentation.fit  hardpoints.fit  loadouts/HBK-4G.fit
              models/hunchback.glb  icons/hunchback.png  anim/mech_biped.animgraph
→ run unit validator → unit appears in editor/mission
```
Validator must yell in modder terms, e.g.: `Missing socket: RightTorso_Ballistic_1` · `Weapon AC20 requires Ballistic hardpoint` · `Jump granted but no JumpExecutor-compatible locomotion` · `AnimationGraph missing JumpLand` · `Damage location RightArm referenced but schema has no RightArm`. That is the line between "mod-friendly" and "modders grep C++ crashes."

## Discipline (every slice)
- Canonical worktree `.claude/worktrees/unit-profile-seam`, branch `claude/unit-profile-seam`. Grep-anchored edits (line numbers drift). Full relink on layout change.
- Offline doctest RED→GREEN before engine glue. Gate default-OFF; gate-OFF must be behavioral parity (tier1). Gate-ON dump proof per slice.
- Honest promise: today = data-toggled capabilities on existing types; near-term = class-check removal + mod-visible flags; real future = new archetypes with data-defined locomotion/damage/actions/presentation.
