#ifndef UNIT_PROFILE_FINDINGS_TEXT_H
#define UNIT_PROFILE_FINDINGS_TEXT_H
// UNIT-MODLINT-CLI-1 (Phase-3 slice 3 of the UNIT-PROFILE arc).
//
// The reason-code -> modder-readable message table, living NEXT TO the
// authoritative validator (unit_profile_validator.h) so there is exactly ONE
// place that turns a UPV_* code into a human line. `mc2_modlint` prints these;
// the engine glue may reuse them when logging. Pure, header-only, no engine/GL/
// IO drag -> compiles into the offline `mc2_tests` target and into the tiny
// standalone `mc2_modlint` target unchanged.
//
// TWO strings per code:
//   * a stable machine token (`UPV_...`) so CI / mod tooling can grep the code
//     independent of wording.
//   * a modder-readable message: what is wrong, in the vocabulary the roadmap's
//     acceptance test demands ("Weapon requires Ballistic hardpoint",
//     "declared socket not found", "Jump granted but no compatible executor").
//
// The per-finding `UnitProfileFinding::message` (a terse static literal chosen at
// the emit site) stays authoritative for the SPECIFIC instance; this table is the
// GENERIC per-code line modlint prints as the headline, with the finding's own
// message appended as detail. Keeping the map here (not in the CLI .cpp) means a
// future reason code is added in ONE diff: the enum in unit_profile_validator.h
// plus a row here, and both the validator tests and the CLI pick it up.
#include "unit_profile_validator.h"   // UnitProfileReasonCode, UPV_*, UPV__COUNT

// Stable machine token for a reason code (grep key; never reworded).
inline const char* unitProfileReasonToken(UnitProfileReasonCode code) {
    switch (code) {
        case UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR:
            return "UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR";
        case UPV_HARDPOINT_DAMAGE_LOCATION_MISSING:
            return "UPV_HARDPOINT_DAMAGE_LOCATION_MISSING";
        case UPV_HARDPOINT_SOCKET_MISSING:
            return "UPV_HARDPOINT_SOCKET_MISSING";
        case UPV_ANIMATION_FALLBACK_MISSING:
            return "UPV_ANIMATION_FALLBACK_MISSING";
        case UPV_HARDPOINT_WEAPON_CLASS_REJECTED:
            return "UPV_HARDPOINT_WEAPON_CLASS_REJECTED";
        case UPV_UI_ASSET_MISSING:
            return "UPV_UI_ASSET_MISSING";
        case UPV__COUNT:
            break;   // not a real code; fall through to the unknown guard
    }
    return "UPV_UNKNOWN";
}

// Modder-readable headline for a reason code. Mirrors the roadmap acceptance
// lines ("Weapon AC20 requires Ballistic hardpoint" etc.) in generic form; the
// specific finding's own message carries the instance detail (socket name, etc.).
inline const char* unitProfileReasonMessage(UnitProfileReasonCode code) {
    switch (code) {
        case UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR:
            return "Jump granted but no compatible jump executor for this locomotion model";
        case UPV_HARDPOINT_DAMAGE_LOCATION_MISSING:
            return "Hardpoint damage location not present in the unit's damage schema";
        case UPV_HARDPOINT_SOCKET_MISSING:
            return "Declared socket not found on the model";
        case UPV_ANIMATION_FALLBACK_MISSING:
            return "Action animation tag has no available gesture in its fallback chain";
        case UPV_HARDPOINT_WEAPON_CLASS_REJECTED:
            return "Weapon requires a hardpoint that accepts its mount-class";
        case UPV_UI_ASSET_MISSING:
            return "Declared UI asset not found / does not resolve";
        case UPV__COUNT:
            break;
    }
    return "unknown finding";
}

#endif // UNIT_PROFILE_FINDINGS_TEXT_H
