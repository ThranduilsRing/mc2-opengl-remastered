// code/action_executor_registry.cpp
// ACTION-EXECUTOR-REGISTRY-1 (slice 3 of the post-seam UNIT-PROFILE arc).
//
// Home of the ONE engine-side ActionExecutorRegistry instance. The registry
// itself is pure/header-only and doctest-tested (tests/unit/test_unit_actions.cpp);
// this TU is the thin engine glue that populates it once at first access.
//
// It replaces the class-virtual `hasJumpExecutor()` seam: gameplay used to ask
// "is this a BattleMech?" to decide whether the jump executor exists. Now it
// asks the registry whether a compatible executor is bound for the unit's
// locomotion model (see Mover::locomotionModel / Mover::queryAction).
//
// No GL, no IO, no engine headers — just the pure registry contract. Populated
// lazily via a function-local static so init order is irrelevant.
#include "action_executor_registry.h"

ActionExecutorRegistry& theActionExecutorRegistry() {
    static ActionExecutorRegistry reg = [] {
        ActionExecutorRegistry r;
        // Slice 3: the only executor that exists is the legacy mech jump-jet
        // path, compatible ONLY with the mech-walker locomotion model. A
        // vehicle's locomotion model is not in this set -> isCompatible() is
        // false -> queryAction(Jump) reports MissingExecutor for it (even if a
        // future data grant gave it CAP_JUMP). LOCOMOTION-FACET-1 (slice 4)
        // will let data broaden the compatible set without touching class code.
        r.bind(UnitAction::Jump, kMechJumpExecutor, {LOCO_MECH_WALKER});
        return r;
    }();
    return reg;
}
