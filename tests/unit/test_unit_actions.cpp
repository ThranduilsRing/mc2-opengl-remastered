// tests/unit/test_unit_actions.cpp
// UNITPROFILE-OFFLINE-HARNESS-1 (Phase 0 of the UNIT-PROFILE arc).
//
// Pure-logic doctest coverage for the two shared contracts that future
// slices implement against:
//   * unit_action.h            -- resolveAction() precedence (slice 2)
//   * action_executor_registry.h -- bind / compatibility resolution (slice 3)
//
// These headers are header-only and drag NO engine/GL/IO code, so they
// compile into the `mc2_tests` target unchanged. Mirror the style of
// test_capability.cpp: small, exhaustive, one behavior per case.
#include "doctest.h"
#include "unit_action.h"
#include "action_executor_registry.h"

// ---------------------------------------------------------------------------
// resolveAction() precedence
//
// Precedence (highest-priority reason wins), documented in unit_action.h:
//   InvalidForUnitSchema
//     > MissingLocomotionFacet
//     > MissingExecutor
//     > DisabledByDamage
//     > MissingAnimation
//     > MissingResource
//     > Available
// ---------------------------------------------------------------------------

// A fully-satisfied query resolves Available. This is the "everything present"
// baseline every other case perturbs exactly one field from.
static ActionQueryInputs allGood() {
    ActionQueryInputs q;
    q.validForSchema     = true;   // schema allows this action at all
    q.hasCapability      = true;   // profile grants the capability
    q.locomotionSupports = true;   // locomotion facet is compatible
    q.hasExecutor        = true;   // a bound, compatible executor exists
    q.damageDisabled     = false;  // not knocked out by damage
    q.hasAnimation       = true;   // presentation adapter can play it
    q.hasResource        = true;   // enough jump-jet fuel / heat headroom / etc
    return q;
}

TEST_CASE("resolveAction: all satisfied -> Available") {
    CHECK(resolveAction(allGood()) == ActionAvailability::Available);
}

TEST_CASE("resolveAction: schema-invalid dominates everything") {
    ActionQueryInputs q = allGood();
    q.validForSchema = false;
    // Even if we ALSO break every lower reason, schema wins.
    q.hasCapability = false;
    q.locomotionSupports = false;
    q.hasExecutor = false;
    q.damageDisabled = true;
    q.hasAnimation = false;
    q.hasResource = false;
    CHECK(resolveAction(q) == ActionAvailability::InvalidForUnitSchema);
}

TEST_CASE("resolveAction: missing capability reports InvalidForUnitSchema") {
    // "profile does not grant this ability at all" is a schema-level fact:
    // the unit is not the sort of thing that can ever do this.
    ActionQueryInputs q = allGood();
    q.hasCapability = false;
    CHECK(resolveAction(q) == ActionAvailability::InvalidForUnitSchema);
}

TEST_CASE("resolveAction: missing locomotion facet outranks missing executor") {
    ActionQueryInputs q = allGood();
    q.locomotionSupports = false;
    q.hasExecutor        = false;  // also broken; locomotion must still win
    CHECK(resolveAction(q) == ActionAvailability::MissingLocomotionFacet);
}

TEST_CASE("resolveAction: missing executor (the BattleMech seam) ") {
    // The exact seam from the roadmap: profile grants Jump, locomotion is fine,
    // but no compatible executor is bound (only BattleMech owned it before).
    ActionQueryInputs q = allGood();
    q.hasExecutor    = false;
    q.damageDisabled = true;   // lower reason also broken; executor must win
    q.hasAnimation   = false;
    q.hasResource    = false;
    CHECK(resolveAction(q) == ActionAvailability::MissingExecutor);
}

TEST_CASE("resolveAction: disabled-by-damage outranks animation/resource") {
    ActionQueryInputs q = allGood();
    q.damageDisabled = true;
    q.hasAnimation   = false;
    q.hasResource    = false;
    CHECK(resolveAction(q) == ActionAvailability::DisabledByDamage);
}

TEST_CASE("resolveAction: missing animation outranks missing resource") {
    ActionQueryInputs q = allGood();
    q.hasAnimation = false;
    q.hasResource  = false;
    CHECK(resolveAction(q) == ActionAvailability::MissingAnimation);
}

TEST_CASE("resolveAction: missing resource is the lowest non-Available reason") {
    ActionQueryInputs q = allGood();
    q.hasResource = false;
    CHECK(resolveAction(q) == ActionAvailability::MissingResource);
}

TEST_CASE("resolveAction: BattleMech legacy logic is expressible") {
    // Old world: canJump == (capability && executor). Nothing else modeled.
    // Reproduce it by leaving all the facets future slices add satisfied.
    auto legacyCanJump = [](bool cap, bool exec) {
        ActionQueryInputs q;
        q.validForSchema     = cap;   // no-cap == not-that-kind-of-thing
        q.hasCapability      = cap;
        q.locomotionSupports = true;
        q.hasExecutor        = exec;
        q.damageDisabled     = false;
        q.hasAnimation       = true;
        q.hasResource        = true;
        return resolveAction(q) == ActionAvailability::Available;
    };
    CHECK(legacyCanJump(true,  true));
    CHECK_FALSE(legacyCanJump(true,  false));
    CHECK_FALSE(legacyCanJump(false, true));
    CHECK_FALSE(legacyCanJump(false, false));
}

TEST_CASE("resolveAction: every reason is reachable") {
    // Guards against a precedence refactor silently making a reason dead.
    ActionQueryInputs q;

    q = allGood(); CHECK(resolveAction(q) == ActionAvailability::Available);
    q = allGood(); q.validForSchema = false;
        CHECK(resolveAction(q) == ActionAvailability::InvalidForUnitSchema);
    q = allGood(); q.locomotionSupports = false;
        CHECK(resolveAction(q) == ActionAvailability::MissingLocomotionFacet);
    q = allGood(); q.hasExecutor = false;
        CHECK(resolveAction(q) == ActionAvailability::MissingExecutor);
    q = allGood(); q.damageDisabled = true;
        CHECK(resolveAction(q) == ActionAvailability::DisabledByDamage);
    q = allGood(); q.hasAnimation = false;
        CHECK(resolveAction(q) == ActionAvailability::MissingAnimation);
    q = allGood(); q.hasResource = false;
        CHECK(resolveAction(q) == ActionAvailability::MissingResource);
}

// ---------------------------------------------------------------------------
// ActionExecutorRegistry: bind + compatibility
//
// A registry maps a UnitAction key to (executorId, set-of-compatible
// locomotion-model-ids). isBound() answers "is anything registered for this
// action?"; isCompatible() answers "is the given locomotion model allowed to
// run the bound executor?". Plain ints/enums only -- no engine types.
// ---------------------------------------------------------------------------

TEST_CASE("registry: empty registry binds nothing") {
    ActionExecutorRegistry reg;
    CHECK_FALSE(reg.isBound(UnitAction::Jump));
    CHECK(reg.executorFor(UnitAction::Jump) == kNoExecutor);
    // Nothing bound -> nothing is compatible.
    CHECK_FALSE(reg.isCompatible(UnitAction::Jump, /*locomotionModelId=*/0));
}

TEST_CASE("registry: bind then isBound") {
    ActionExecutorRegistry reg;
    const int kMechJumpExecutor = 1;
    reg.bind(UnitAction::Jump, kMechJumpExecutor, {LOCO_MECH_WALKER});
    CHECK(reg.isBound(UnitAction::Jump));
    CHECK(reg.executorFor(UnitAction::Jump) == kMechJumpExecutor);
}

TEST_CASE("registry: compatibility is per-locomotion-model") {
    ActionExecutorRegistry reg;
    const int kJumpJetExecutor = 7;
    // Jump executor works for mech-walkers and hover, but not tracked/wheeled.
    reg.bind(UnitAction::Jump, kJumpJetExecutor, {LOCO_MECH_WALKER, LOCO_HOVER});
    CHECK(reg.isCompatible(UnitAction::Jump, LOCO_MECH_WALKER));
    CHECK(reg.isCompatible(UnitAction::Jump, LOCO_HOVER));
    CHECK_FALSE(reg.isCompatible(UnitAction::Jump, LOCO_TRACKED));
    CHECK_FALSE(reg.isCompatible(UnitAction::Jump, LOCO_WHEELED));
}

TEST_CASE("registry: rebind replaces the previous binding") {
    ActionExecutorRegistry reg;
    reg.bind(UnitAction::Jump, 1, {LOCO_MECH_WALKER});
    reg.bind(UnitAction::Jump, 2, {LOCO_HOVER});   // overwrite
    CHECK(reg.executorFor(UnitAction::Jump) == 2);
    CHECK_FALSE(reg.isCompatible(UnitAction::Jump, LOCO_MECH_WALKER));
    CHECK(reg.isCompatible(UnitAction::Jump, LOCO_HOVER));
}

TEST_CASE("registry: isCompatible on unbound action is false, not a crash") {
    ActionExecutorRegistry reg;
    reg.bind(UnitAction::Jump, 1, {LOCO_MECH_WALKER});
    // Query a locomotion model that was never registered as compatible.
    CHECK_FALSE(reg.isCompatible(UnitAction::Jump, LOCO_INFANTRY));
}

TEST_CASE("registry: feeds resolveAction's executor fields (integration)") {
    // Demonstrates the slice-3 -> slice-2 seam: the registry answers the two
    // executor-shaped facts that resolveAction consumes.
    ActionExecutorRegistry reg;
    reg.bind(UnitAction::Jump, 1, {LOCO_MECH_WALKER});

    auto canJumpFor = [&](int locomotionModelId) {
        ActionQueryInputs q = ActionQueryInputs{};
        q.validForSchema     = true;
        q.hasCapability      = true;
        q.locomotionSupports = reg.isCompatible(UnitAction::Jump, locomotionModelId);
        q.hasExecutor        = reg.isBound(UnitAction::Jump);
        q.damageDisabled     = false;
        q.hasAnimation       = true;
        q.hasResource        = true;
        return resolveAction(q);
    };
    CHECK(canJumpFor(LOCO_MECH_WALKER) == ActionAvailability::Available);
    // Tracked vehicle: executor is bound, but locomotion is incompatible ->
    // the registry reports it as a missing-locomotion-facet, not a crash.
    CHECK(canJumpFor(LOCO_TRACKED) == ActionAvailability::MissingLocomotionFacet);
}
