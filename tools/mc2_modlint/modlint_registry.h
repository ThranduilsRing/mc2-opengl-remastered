#ifndef MC2_MODLINT_REGISTRY_H
#define MC2_MODLINT_REGISTRY_H
// UNIT-MODLINT-CLI-1 (Phase-3 slice 3). The executor registry modlint validates
// against — the SAME binding the engine's theActionExecutorRegistry() installs
// (action_executor_registry.h): UnitAction::Jump -> kMechJumpExecutor, compatible
// ONLY with LOCO_MECH_WALKER. Reproduced here because the engine .cpp that owns
// the singleton is NOT linked into this GL/IO-free tool; keeping the binding in a
// shared header means the CLI and its doctest lint against the same truth the
// runtime resolves, so modlint and runtime can never diverge on the jump facet.
#include "action_executor_registry.h"

inline ActionExecutorRegistry modlintExecutorRegistry() {
    ActionExecutorRegistry reg;
    reg.bind(UnitAction::Jump, kMechJumpExecutor, {LOCO_MECH_WALKER});
    return reg;
}

#endif // MC2_MODLINT_REGISTRY_H
