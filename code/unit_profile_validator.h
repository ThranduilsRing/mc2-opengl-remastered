#ifndef UNIT_PROFILE_VALIDATOR_H
#define UNIT_PROFILE_VALIDATOR_H
// UNITPROFILE-VALIDATOR-1 (slice 6 of the UNIT-PROFILE arc).
//
// Load-time CONSTRUCTION-RULES validator: proves a unit's facets COHERE and
// yields actionable, modder-readable findings. This is the tabletop
// "can you legally build this unit?" layer, expressed as pure logic.
//
// REPORT-FIRST this slice: the validator only *describes* incoherence — it
// never rejects a spawn. The engine glue logs each finding (gated behind
// MC2_UNIT_PROFILE_DATA) so gate-OFF and gate-ON stay behavioral parity;
// enforcement (refusing an impossible profile) is a deliberate future toggle.
//
// Pure, header-only (plain enums / bools), so it compiles into the offline
// `mc2_tests` target and drags NO engine/GL/IO code. It reuses the facet
// vocabularies already established by earlier slices:
//   * capability.h                 -- CAP_JUMP (the granted-capability bit)
//   * action_executor_registry.h   -- ActionExecutorRegistry, UnitAction, LocomotionModel
//   * locomotion_facet.h / damage_schema.h -- facet id enums (pulled transitively)
//
// The join it proves: "the profile GRANTS an ability, and the unit's other
// facets can actually deliver it." The one live check is the real audit case:
// CAP_JUMP granted but no compatible jump executor exists for this locomotion
// model -> the "capability-granted vehicle reports MissingExecutor" failure,
// caught at LOAD instead of silently at query time.
#include <vector>
#include "capability.h"                  // CAP_JUMP, UnitAction
#include "action_executor_registry.h"    // ActionExecutorRegistry, LocomotionModel

// -------------------------------------------------------------------------
// REASON-CODE VOCABULARY.
//
// Stable, modder-facing identifiers. Order is NOT significance; each code is a
// distinct coherence rule. Future slices (esp. Phase-2 UNIT-DEFINITION-VALIDATOR-1)
// ADD codes here and grow the vocabulary — they must NOT renumber existing ones,
// because logs / mod tooling key on the name. `UPV_` = Unit-Profile-Validator.
//
// LIVE this slice:
//   UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR
//
// STUBBED (Phase-2 reason codes reserved but NOT checked yet — documented so the
// vocabulary is stable and the check sites are obvious; see roadmap Phase 2):
//   UPV_HARDPOINT_DAMAGE_LOCATION_MISSING  -- loadout names a damage location the
//        DamageSchema has no such location for ("references RightArm but schema has none").
//   UPV_HARDPOINT_SOCKET_MISSING           -- a hardpoint maps to a model socket the
//        presentation profile does not expose.
//   UPV_ANIMATION_FALLBACK_MISSING         -- an action is available but the animation
//        binding profile has no clip/gesture fallback for it (e.g. no JumpLand).
enum UnitProfileReasonCode {
    // --- LIVE ---
    UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR = 0,

    // --- STUBBED (Phase 2; not emitted this slice) ---
    UPV_HARDPOINT_DAMAGE_LOCATION_MISSING,
    UPV_HARDPOINT_SOCKET_MISSING,
    UPV_ANIMATION_FALLBACK_MISSING,

    UPV__COUNT
};

// A single coherence finding. `code` is the stable machine key; `message` is a
// short human-readable, modder-facing summary (the engine glue prefixes it with
// the unit's type name when logging). Kept a flat POD for trivial test use.
struct UnitProfileFinding {
    UnitProfileReasonCode code;
    const char*           message;   // static string literal; no ownership
};

// Pure facet inputs the validator reasons over. Booleans / facet-enums ONLY —
// no engine types — so this compiles into `mc2_tests`. Future slices add fields
// (hardpoint table, socket set, animation bindings) for the stubbed checks; each
// stays at a non-blocking default until its slice lands, exactly like
// ActionQueryInputs in unit_action.h.
struct UnitProfileFacts {
    bool            grantsJump    = false;            // CAP_JUMP granted by the profile
    LocomotionModel locomotion    = LOCO_WHEELED;     // the unit's locomotion facet
};

// LIVE CHECK (CAP_JUMP <-> executor coherence):
//   grantsJump && !registry.isCompatible(Jump, locomotion)
//     -> UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR
//
// A walker mech granted CAP_JUMP is compatible (the jump executor lists
// LOCO_MECH_WALKER) -> ZERO findings. A vehicle NOT granted CAP_JUMP has nothing
// to prove -> ZERO findings. A vehicle (or any non-walker locomotion) that IS
// granted CAP_JUMP has an incoherent profile: it claims jump but no bound
// executor can run it on this chassis -> exactly this finding. That is the stock
// parity guarantee: stock mechs (walker + jump) and stock vehicles (no jump)
// both produce nothing; only an impossible authored profile trips the check.
//
// `registry` is the same ActionExecutorRegistry gameplay queries at runtime, so
// the validator's verdict and the runtime query can never diverge.
inline std::vector<UnitProfileFinding>
validateUnitProfile(const UnitProfileFacts& facts,
                    const ActionExecutorRegistry& registry) {
    std::vector<UnitProfileFinding> findings;

    // --- LIVE: JUMP granted but no compatible executor for this locomotion. ---
    if (facts.grantsJump &&
        !registry.isCompatible(UnitAction::Jump, facts.locomotion)) {
        findings.push_back(UnitProfileFinding{
            UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR,
            "JUMP granted but no compatible jump executor for this locomotion model"});
    }

    // --- STUBBED (Phase 2): hardpoint->damage-location, missing socket, missing
    // animation fallback. Not implemented this slice; the reason codes exist so
    // the vocabulary is stable and this is the single site future slices extend.
    // Do NOT add Phase-2 checks here without their facet inputs on UnitProfileFacts.

    return findings;
}

#endif // UNIT_PROFILE_VALIDATOR_H
