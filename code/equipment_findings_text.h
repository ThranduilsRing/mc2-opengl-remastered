#ifndef EQUIPMENT_FINDINGS_TEXT_H
#define EQUIPMENT_FINDINGS_TEXT_H
// WEAPON-MODLINT-1 (equipment arc, slice 7).
//
// The EQV_* reason-code -> modder-readable message table, living NEXT TO the
// authoritative validator (equipment_validator.h) so there is exactly ONE place
// that turns an EquipmentReasonCode into a human line -- the direct mirror of the
// unit arc's unit_profile_findings_text.h. `mc2_modlint --equipment ...` prints
// these; a future engine validate-hook may reuse them when logging. Pure,
// header-only, no engine/GL/IO drag -> compiles into the offline `mc2_tests`
// target and into the standalone `mc2_modlint` target unchanged.
//
// TWO strings per code:
//   * a stable machine token (`EQV_...`) so CI / mod tooling can grep the code
//     independent of wording.
//   * a modder-readable headline in weapon-authoring vocabulary ("weapon has no
//     fire executor", "weapon requires a hardpoint that accepts its mount-class").
//
// The per-finding EquipmentFinding::message (the terse literal chosen at the emit
// site in equipment_validator.h) stays authoritative for the SPECIFIC instance;
// this table is the GENERIC per-code headline modlint prints, with the finding's
// own message appended as detail. Keeping the map here (not in the CLI .cpp) means
// a future EQV_ code is added in ONE diff: the enum in equipment_validator.h plus
// a row here, and both the validator tests and the CLI pick it up.
#include "equipment_validator.h"   // EquipmentReasonCode, EQV_*, EQV__COUNT

// Stable machine token for an equipment reason code (grep key; never reworded).
inline const char* equipmentReasonToken(EquipmentReasonCode code) {
    switch (code) {
        case EQV_WEAPON_NO_FIRE_EXECUTOR:
            return "EQV_WEAPON_NO_FIRE_EXECUTOR";
        case EQV_PRESENTATION_ASSET_MISSING:
            return "EQV_PRESENTATION_ASSET_MISSING";
        case EQV_WEAPON_NO_CATEGORY:
            return "EQV_WEAPON_NO_CATEGORY";
        case EQV_AMMO_CATEGORY_MISSING:
            return "EQV_AMMO_CATEGORY_MISSING";
        case EQV_WEAPON_RANGE_INVALID:
            return "EQV_WEAPON_RANGE_INVALID";
        case EQV_KIND_FACET_MISMATCH:
            return "EQV_KIND_FACET_MISMATCH";
        case EQV_WEAPON_HARDPOINT_REJECTED:
            return "EQV_WEAPON_HARDPOINT_REJECTED";
        case EQV__COUNT:
            break;   // not a real code; fall through to the unknown guard
    }
    return "EQV_UNKNOWN";
}

// Modder-readable headline for an equipment reason code. Generic form; the
// specific finding's own message carries the instance detail.
inline const char* equipmentReasonMessage(EquipmentReasonCode code) {
    switch (code) {
        case EQV_WEAPON_NO_FIRE_EXECUTOR:
            return "Weapon has no fire executor bound (cannot fire)";
        case EQV_PRESENTATION_ASSET_MISSING:
            return "Claimed weapon presentation event maps to no VFX/SFX/decal/light asset";
        case EQV_WEAPON_NO_CATEGORY:
            return "Weapon has no authored mount-class category";
        case EQV_AMMO_CATEGORY_MISSING:
            return "Ammo-using item names no ammo category (nothing can supply it)";
        case EQV_WEAPON_RANGE_INVALID:
            return "Weapon ranges/damage invalid (negative, or short>med>long ordering violated)";
        case EQV_KIND_FACET_MISMATCH:
            return "Equipment kind and WeaponFacet presence disagree";
        case EQV_WEAPON_HARDPOINT_REJECTED:
            return "Weapon requires a hardpoint that accepts its mount-class";
        case EQV__COUNT:
            break;
    }
    return "unknown finding";
}

#endif // EQUIPMENT_FINDINGS_TEXT_H
