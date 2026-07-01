// tests/unit/test_unit_profile_validator.cpp
// UNITPROFILE-VALIDATOR-1 (slice 6 of the UNIT-PROFILE arc).
//
// Pure-logic doctest coverage for unit_profile_validator.h: the load-time
// construction-rules validator. Every LIVE reason code must be reachable, and
// the stock-parity guarantee (walker+jump -> none, vehicle-no-jump -> none)
// must hold. Header-only, drags no engine/GL/IO. Mirrors test_unit_actions.cpp.
#include "doctest.h"
#include "unit_profile_validator.h"
#include "action_executor_registry.h"

// The engine-side singleton binds UnitAction::Jump -> mech jump-jet executor,
// compatible ONLY with LOCO_MECH_WALKER (action_executor_registry.h). Reproduce
// that binding with a local registry so the validator sees the same truth the
// runtime does (the .cpp holding theActionExecutorRegistry() is not linked here).
static ActionExecutorRegistry stockJumpRegistry() {
    ActionExecutorRegistry reg;
    reg.bind(UnitAction::Jump, kMechJumpExecutor, {LOCO_MECH_WALKER});
    return reg;
}

// -------------------------------------------------------------------------
// Stock parity: the two shapes a clean stock run produces.
// -------------------------------------------------------------------------

TEST_CASE("validator: stock jet mech (walker + CAP_JUMP) produces ZERO findings") {
    // A walker mech granted jump IS compatible with the bound executor -> coherent.
    const ActionExecutorRegistry reg = stockJumpRegistry();
    UnitProfileFacts facts;
    facts.grantsJump = true;
    facts.locomotion = LOCO_MECH_WALKER;
    CHECK(validateUnitProfile(facts, reg).empty());
}

TEST_CASE("validator: stock vehicle (no CAP_JUMP) produces ZERO findings") {
    // A vehicle that does not claim jump has nothing to prove, regardless of
    // its (non-walker) locomotion model -> coherent.
    const ActionExecutorRegistry reg = stockJumpRegistry();
    UnitProfileFacts facts;
    facts.grantsJump = false;
    facts.locomotion = LOCO_WHEELED;
    CHECK(validateUnitProfile(facts, reg).empty());

    facts.locomotion = LOCO_TRACKED;  CHECK(validateUnitProfile(facts, reg).empty());
    facts.locomotion = LOCO_HOVER;    CHECK(validateUnitProfile(facts, reg).empty());
    facts.locomotion = LOCO_INFANTRY; CHECK(validateUnitProfile(facts, reg).empty());
}

// -------------------------------------------------------------------------
// LIVE reason code: JUMP granted but no compatible executor.
// -------------------------------------------------------------------------

TEST_CASE("validator: vehicle granted CAP_JUMP -> JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR") {
    // The real audit case: a wheeled/tracked/hover unit authored with jump. The
    // jump executor is bound only for LOCO_MECH_WALKER, so no compatible executor
    // exists for this chassis -> exactly one finding with the live reason code.
    const ActionExecutorRegistry reg = stockJumpRegistry();
    for (LocomotionModel loco : {LOCO_WHEELED, LOCO_TRACKED, LOCO_HOVER, LOCO_INFANTRY}) {
        UnitProfileFacts facts;
        facts.grantsJump = true;
        facts.locomotion = loco;
        std::vector<UnitProfileFinding> f = validateUnitProfile(facts, reg);
        REQUIRE(f.size() == 1);
        CHECK(f[0].code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);
        CHECK(f[0].message != nullptr);
    }
}

TEST_CASE("validator: reflects the registry, not the class -- vehicle+MechWalker is coherent") {
    // If a vehicle's locomotion facet is DATA-overridden to MechWalker, the bound
    // executor IS compatible -> no finding. Proves the validator keys on the facet
    // + registry, never on C++ class (the whole point of the arc).
    const ActionExecutorRegistry reg = stockJumpRegistry();
    UnitProfileFacts facts;
    facts.grantsJump = true;
    facts.locomotion = LOCO_MECH_WALKER;   // e.g. .fit declared Locomotion=MechWalker
    CHECK(validateUnitProfile(facts, reg).empty());
}

TEST_CASE("validator: empty registry -> even a walker+jump trips the check") {
    // Defensive: if NOTHING is bound (executor registry not populated), a
    // jump-granting profile has no compatible executor at all -> the finding
    // fires. Confirms the check depends on the registry, not a hardcoded walker
    // assumption.
    ActionExecutorRegistry empty;   // no bindings
    UnitProfileFacts facts;
    facts.grantsJump = true;
    facts.locomotion = LOCO_MECH_WALKER;
    std::vector<UnitProfileFinding> f = validateUnitProfile(facts, empty);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);
}

// -------------------------------------------------------------------------
// Every LIVE reason code is reachable (guards against a refactor making one
// dead). Stubbed Phase-2 codes are intentionally NOT emitted this slice.
// -------------------------------------------------------------------------

TEST_CASE("validator: every LIVE reason code is reachable") {
    const ActionExecutorRegistry reg = stockJumpRegistry();

    // UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR
    UnitProfileFacts jumpVeh;
    jumpVeh.grantsJump = true;
    jumpVeh.locomotion = LOCO_WHEELED;
    std::vector<UnitProfileFinding> f = validateUnitProfile(jumpVeh, reg);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);

    // Phase-2 codes exist in the vocabulary but are NOT emitted this slice.
    // (Documented stubs; a Phase-2 slice adds facet inputs + checks + tests.)
    CHECK(UPV__COUNT == 4);   // vocabulary size lock: 1 live + 3 reserved stubs
}
