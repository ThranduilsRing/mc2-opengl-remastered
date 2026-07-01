#ifndef LOCOMOTION_FACET_H
#define LOCOMOTION_FACET_H
// LOCOMOTION-FACET-1 (slice 4 of the UNIT-PROFILE arc).
//
// Data-owned locomotion model. Slice 3 left ONE transitional class-derivation:
// `Mover::locomotionModel()` returned LOCO_WHEELED for the base class and
// BattleMech overrode it to LOCO_MECH_WALKER. That was the last executor-shaped
// class-check. This slice moves the VALUE onto a data facet carried on the
// UnitProfile (type-load), so `locomotionModel()` reads the facet instead of the
// C++ class. WHERE the value comes from changes (data facet); the values do not
// (mechs still resolve MechWalker, vehicles a non-jump model), so gate-ON
// behavior is identical to slice 3's class mapping.
//
// Pure, header-only (plain enums / const char*), so it compiles into the offline
// `mc2_tests` target and drags no engine/GL/IO code. Reuses the LocomotionModel
// enum defined in action_executor_registry.h (LOCO_MECH_WALKER, LOCO_TRACKED,
// LOCO_WHEELED, LOCO_HOVER, LOCO_INFANTRY) so the registry's compatibility sets
// and the facet speak the same id language.
#include "action_executor_registry.h"   // LocomotionModel enum

// Case-insensitive ASCII equality for a facet token vs a fixed key.
inline bool locomotionTokenEquals(const char* a, const char* key) {
    if (!a || !key) return false;
    for (; *a && *key; ++a, ++key) {
        char ca = *a, ck = *key;
        if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
        if (ck >= 'A' && ck <= 'Z') ck = char(ck - 'A' + 'a');
        if (ca != ck) return false;
    }
    return *a == '\0' && *key == '\0';
}

// Map an optional `.fit [UnitProfile] Locomotion=` token to a LocomotionModel.
// Unknown / empty / null token -> `fallback` (the class-default the caller
// supplies), so stock .fits that omit the key preserve parity. Recognized
// tokens mirror the enum: MechWalker, Tracked, Wheeled, Hover, Infantry.
inline LocomotionModel parseLocomotionModel(const char* token,
                                            LocomotionModel fallback) {
    if (!token || token[0] == '\0') return fallback;
    if (locomotionTokenEquals(token, "MechWalker")) return LOCO_MECH_WALKER;
    if (locomotionTokenEquals(token, "Tracked"))    return LOCO_TRACKED;
    if (locomotionTokenEquals(token, "Wheeled"))    return LOCO_WHEELED;
    if (locomotionTokenEquals(token, "Hover"))      return LOCO_HOVER;
    if (locomotionTokenEquals(token, "Infantry"))   return LOCO_INFANTRY;
    return fallback;
}

// UNIT-DEFINITION-SHADOW-COMPARE-1: canonical token NAME for a LocomotionModel.
// Inverse of parseLocomotionModel — the spelling MUST match the token that path
// accepts AND the converter's LOCO_* vocabulary (tools/unit_converter/model.py),
// because shadow_compare.py diffs the engine dump's `locomotion` string against
// the converter's. A round-trip identity holds: parseLocomotionModel(
// locomotionModelName(m), other) == m for every enumerator. Pure, offline.
inline const char* locomotionModelName(LocomotionModel m) {
    switch (m) {
        case LOCO_MECH_WALKER: return "MechWalker";
        case LOCO_TRACKED:     return "Tracked";
        case LOCO_WHEELED:     return "Wheeled";
        case LOCO_HOVER:       return "Hover";
        case LOCO_INFANTRY:    return "Infantry";
        case LOCO__COUNT:      break;
    }
    return "Unknown";
}

#endif // LOCOMOTION_FACET_H
