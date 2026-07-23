#ifndef ACTION_EXECUTOR_REGISTRY_H
#define ACTION_EXECUTOR_REGISTRY_H
// UNITPROFILE-OFFLINE-HARNESS-1 / slice 3 (ACTION-EXECUTOR-REGISTRY-1).
//
// Pure, header-only contract. The seam that replaces "is this a BattleMech?"
// with "is a compatible executor bound for this action?". Plain ints/enums
// only — no engine types — so it compiles into the offline `mc2_tests` target
// and drags nothing.
//
// A binding maps a UnitAction key to:
//   * an executor id  (opaque int; slice 3 assigns real ids engine-side)
//   * the set of locomotion-model ids the executor can run on
//
// isBound(action)                -> is anything registered for this action?
// executorFor(action)            -> the bound executor id, or kNoExecutor.
// isCompatible(action, locoModel)-> can that locomotion model run the binding?
//
// This is the minimal-but-real map-based seam slice 3 fills in; the engine
// registers mech/vehicle jump/move/attack executors into an instance of this
// at type-load and gameplay queries it instead of branching on class.
#include <map>
#include <set>
#include "capability.h"   // UnitAction

// Sentinel for "no executor bound".
static const int kNoExecutor = -1;

// Locomotion-model ids. Data-owned movement mode (slice 4, LOCOMOTION-FACET-1).
// Plain ints here; the real facet may promote these to a richer type, but the
// registry only ever compares ids for set membership.
enum LocomotionModel {
    LOCO_MECH_WALKER = 0,
    LOCO_TRACKED     = 1,
    LOCO_WHEELED     = 2,
    LOCO_HOVER       = 3,
    LOCO_INFANTRY    = 4,
    LOCO__COUNT
};

class ActionExecutorRegistry {
    struct Binding {
        int                executorId = kNoExecutor;
        std::set<int>      compatibleLocomotion;
    };
    std::map<UnitAction, Binding> bindings_;

  public:
    // Register (or overwrite) the executor bound to `action`. `compatible` is
    // the set of locomotion-model ids that may run it.
    void bind(UnitAction action, int executorId,
              const std::set<int>& compatibleLocomotion) {
        Binding b;
        b.executorId           = executorId;
        b.compatibleLocomotion = compatibleLocomotion;
        bindings_[action]      = b;   // map::operator[] overwrites on rebind
    }

    bool isBound(UnitAction action) const {
        return bindings_.find(action) != bindings_.end();
    }

    int executorFor(UnitAction action) const {
        auto it = bindings_.find(action);
        return it == bindings_.end() ? kNoExecutor : it->second.executorId;
    }

    // True iff an executor is bound AND `locomotionModelId` is in its
    // compatible set. Unbound or incompatible -> false (never throws).
    bool isCompatible(UnitAction action, int locomotionModelId) const {
        auto it = bindings_.find(action);
        if (it == bindings_.end()) return false;
        const std::set<int>& s = it->second.compatibleLocomotion;
        return s.find(locomotionModelId) != s.end();
    }
};

// ACTION-EXECUTOR-REGISTRY-1 (slice 3): the ONE engine-side registry instance.
// Function-local static, populated once on first access (no GL/IO). Defined in
// action_executor_registry.cpp — a DECLARATION only here, so the pure offline
// `mc2_tests` target that includes this header drags nothing (it never links
// the .cpp; the symbol is only referenced from engine TUs like mover.cpp).
//
// Binding for slice 3: UnitAction::Jump -> the mech jump-jet executor, compatible
// ONLY with LOCO_MECH_WALKER. Gameplay resolves "can this unit run Jump?" via
// theActionExecutorRegistry().isCompatible(Jump, mover->locomotionModel())
// instead of asking "is this a BattleMech?".
ActionExecutorRegistry& theActionExecutorRegistry();

// Executor ids are opaque ints (kNoExecutor == -1 is the sentinel). Slice 3
// assigns the first real one; keep them here so engine + any future registrant
// share the numbering.
static const int kMechJumpExecutor = 1;

#endif // ACTION_EXECUTOR_REGISTRY_H
