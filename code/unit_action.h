#ifndef UNIT_ACTION_H
#define UNIT_ACTION_H
// UNITPROFILE-OFFLINE-HARNESS-1 / slice 2 (UNIT-ACTION-QUERY-RESULT-1).
//
// Pure, header-only contract. Drags NO engine/GL/IO code so it compiles into
// the offline `mc2_tests` target and future engine TUs alike.
//
// The seam this replaces: the old world answered `bool canPerform(Jump)` as
// `capability && executor` and silently confused every other failure mode.
// resolveAction() instead returns a *reason* so a load-time validator (slice 6)
// can yell "UnitProfile grants Jump, but no compatible JumpExecutor is bound",
// and gameplay/UI can distinguish "can't (out of fuel)" from "never could".
//
// UnitAction itself lives in capability.h (the public gameplay-intent enum);
// this header only adds the *result* type and the pure resolution function.
#include "capability.h"   // UnitAction

// Why an action is (un)available. Ordering of the enum is NOT the precedence
// order — precedence is encoded explicitly in resolveAction() below.
enum class ActionAvailability {
    Available,
    MissingExecutor,        // no bound executor can run this action
    MissingLocomotionFacet, // locomotion mode incompatible with the executor
    MissingAnimation,       // presentation adapter can't play it
    MissingResource,        // transient: fuel / heat / ammo / cooldown
    DisabledByDamage,       // capable in principle, knocked out right now
    InvalidForUnitSchema,   // this unit is not the sort of thing that can do it
};

// Pure facts a query needs. Booleans/enums ONLY — no engine types. Future
// slices fill these fields from their facets (locomotion facet -> locomotionSupports,
// executor registry -> hasExecutor, damage schema -> damageDisabled, etc.).
// Kept as a flat POD so it is trivially constructible in tests and callers.
struct ActionQueryInputs {
    bool validForSchema     = false; // schema permits this action category at all
    bool hasCapability      = false; // profile grants the capability bit
    bool locomotionSupports = false; // locomotion facet is compatible
    bool hasExecutor        = false; // a bound, compatible executor exists
    bool damageDisabled     = false; // currently knocked out by damage
    bool hasAnimation       = true;  // presentation adapter can play it
    bool hasResource        = true;  // enough transient resource (fuel/heat/ammo)
};

// PRECEDENCE (highest-priority reason reported first):
//
//   1. InvalidForUnitSchema   — permanent "wrong kind of thing" (schema says no,
//                               or the profile grants no such capability). Report
//                               this before anything transient so the validator
//                               and UI never mistake a category error for a
//                               fuel shortage.
//   2. MissingLocomotionFacet — the locomotion mode can't host the executor
//                               (a tracked vehicle can't run the jump-jet path).
//                               Outranks MissingExecutor: "even if you bound one,
//                               this chassis couldn't use it."
//   3. MissingExecutor        — the seam. Capability + locomotion OK, but no
//                               executor is bound (only BattleMech owned it).
//   4. DisabledByDamage       — capable in principle, out of action right now.
//   5. MissingAnimation       — bound + able, but no presentation to play.
//   6. MissingResource        — the most transient: out of fuel/heat/ammo.
//   7. Available.
//
// Rationale for the split: permanent/structural reasons (1–3) are what the
// construction-rules validator cares about; transient reasons (4–6) are what
// per-frame gameplay/UI cares about. Reporting the highest structural reason
// first means the validator's message is always the most actionable one.
inline ActionAvailability resolveAction(const ActionQueryInputs& q) {
    if (!q.validForSchema || !q.hasCapability)
        return ActionAvailability::InvalidForUnitSchema;
    if (!q.locomotionSupports)
        return ActionAvailability::MissingLocomotionFacet;
    if (!q.hasExecutor)
        return ActionAvailability::MissingExecutor;
    if (q.damageDisabled)
        return ActionAvailability::DisabledByDamage;
    if (!q.hasAnimation)
        return ActionAvailability::MissingAnimation;
    if (!q.hasResource)
        return ActionAvailability::MissingResource;
    return ActionAvailability::Available;
}

// UNIT-ACTION-QUERY-RESULT-1 (slice 2): the engine-side inputs mapping for the
// Jump action, extracted as a PURE free function so it is unit-testable without
// dragging Mover/engine. Wires only the facts that exist today; every future-
// slice facet (locomotion / animation / resource / damage) stays at its
// non-blocking default so it never spuriously denies before its slice lands.
//
//   hasCapability = false            -> InvalidForUnitSchema (not that kind of thing)
//   hasCapability && !hasExecutor    -> MissingExecutor       (the BattleMech seam)
//   hasCapability && hasExecutor     -> Available
//
// Mover::queryAction(Jump) builds the same inputs; keep the two in lockstep.
inline ActionQueryInputs jumpQueryInputs(bool hasCapability, bool hasExecutor) {
    ActionQueryInputs q;
    q.validForSchema     = hasCapability; // no capability == wrong kind of thing
    q.hasCapability      = hasCapability;
    q.hasExecutor        = hasExecutor;
    q.locomotionSupports = true;  // LOCOMOTION-FACET-1 (slice 4)
    q.damageDisabled     = false; // no cheap knocked-out signal at this seam yet
    q.hasAnimation       = true;  // presentation adapter facet (future slice)
    q.hasResource        = true;  // fuel/heat/ammo facet (future slice)
    return q;
}

#endif // UNIT_ACTION_H
