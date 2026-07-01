# UNIT-PROFILE arc — roadmap (post-seam)

**Context:** UNIT-PROFILE-SEAM-1 (capability facet + `canPerform` + `.fit [UnitProfile]` data read + witness dump) is verified working on branch `claude/unit-profile-seam`. This roadmap continues toward the north-star: **data-defined unit facets + registered action executors + schema validation, with existing C++ classes as adapters.** Strangler pattern — NOT an ECS/scripting rewrite.

## North-star shape
```
UnitType / UnitProfile   = what this thing is ALLOWED to be   (static, type-load)
UnitRuntimeState         = what this instance currently IS     (mutable, instance)
Action / Executor        = how an allowed behavior actually RUNS (registry-bound)
Presentation adapter     = how it animates/renders/sounds
```
Concrete units become data compositions of facets (locomotion, damage-schema, action/capability, weapon/mount, sensor, footprint/collision, AI-role, presentation, construction-rules). Existing `BattleMech`/`GroundVehicle` classes become executors/adapters; gameplay asks profile/runtime/action queries; `getObjectClass()==KIND` branches disappear one by one (tracked in `.claude/unit-kind-classcheck-audit.md`).

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

## Discipline (every slice)
- Canonical worktree `.claude/worktrees/unit-profile-seam`, branch `claude/unit-profile-seam`. Grep-anchored edits (line numbers drift). Full relink on layout change.
- Offline doctest RED→GREEN before engine glue. Gate default-OFF; gate-OFF must be behavioral parity (tier1). Gate-ON dump proof per slice.
- Honest promise: today = data-toggled capabilities on existing types; near-term = class-check removal + mod-visible flags; real future = new archetypes with data-defined locomotion/damage/actions/presentation.
