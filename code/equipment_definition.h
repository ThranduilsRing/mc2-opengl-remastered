#ifndef EQUIPMENT_DEFINITION_H
#define EQUIPMENT_DEFINITION_H
// EQUIPMENT-DEFINITION-SCHEMA-1 (equipment arc, slice 1).
//
// PURE SCHEMA. `compbas.csv` is MC2's COMPONENT/EQUIPMENT table -- not "weapons
// forever". So the TOP data type is `EquipmentDefinition`, a FACET AGGREGATOR
// covering the whole compbas component family (weapons, ammo, sensors, ECM,
// jump-jets, heatsinks, structure, cockpit, actuators, engines, ...). A weapon is
// simply `EquipmentDefinition with a WeaponFacet` -- we do NOT ship a bare
// WeaponDefinition top type and then rediscover the same 255-cap for every other
// component family.
//
// This header wires NO engine behavior. Nothing engine-side reads it yet (no gate
// needed). It is header-only, GL/IO-free: it reuses qualified_id.h (std::string
// only), the FNV-hashed WeaponId + WeaponFacet content from weapon_definition.h,
// WeaponTag + hardpointAcceptsWeapon() from hardpoint.h, and DamageLocationId from
// damage_schema.h. So it compiles into the offline `mc2_tests` target and drags no
// engine/GL/IO code -- exactly like hardpoint.h / weapon_definition.h.
//
// FUTURE CONSUMERS (documented, NOT built here):
//  * WEAPON-REGISTRY-RUNTIME-FACADE (later slice) resolves a string id -> the
//    EquipmentDefinition; legacy 8-bit masterID still works via legacyMasterId.
//  * LEGACY-COMPBAS-CONVERTER (later slice) reads compbas.csv rows and EMITS
//    EquipmentDefinitions, stamping each row's ordinal into `legacyMasterId` as
//    the SHADOW-PATH bridge -- NOT as identity.
// During that shadow path, `legacyMasterId` is the compatibility bridge to the old
// 8-bit `inventory[].masterID`; the canonical `modId:name` STRING is the real
// identity (the 255-cap is designed out because identity is a name, not an ordinal
// into a fixed array).

#include "qualified_id.h"       // mc2id::QualifiedId, parseQualifiedId, isValidQualifiedId
#include "weapon_definition.h"  // WeaponFacet (+ WeaponDefinition alias), weaponFitsHardpoint
#include "hardpoint.h"          // WeaponTag, HardpointProfile, hardpointAcceptsWeapon
#include "damage_schema.h"      // DamageLocationId

#include <string>

// -------------------------------------------------------------------------
// EquipmentKind -- which compbas component family this equipment is. Covers the
// families a compbas row can belong to. `Other` is the catch-all for rows that do
// not (yet) map to a modeled kind.
// -------------------------------------------------------------------------
enum EquipmentKind {
    EQ_WEAPON = 0,   // an entry carrying a WeaponFacet
    EQ_AMMO,         // ammunition (linked by named ammo category, not offset math)
    EQ_SENSOR,       // sensor / active probe
    EQ_ECM,          // electronic countermeasures
    EQ_JUMPJET,      // jump jet
    EQ_HEATSINK,     // heat sink
    EQ_STRUCTURE,    // internal structure / armor kit
    EQ_COCKPIT,      // cockpit / life support
    EQ_ACTUATOR,     // actuator
    EQ_ENGINE,       // fusion / ICE engine
    EQ_OTHER,        // catch-all (unmapped compbas family)
    NUM_EQUIPMENT_KINDS   // keep last; must stay <= 32 (used as a tag bit range too)
};

// -------------------------------------------------------------------------
// EquipmentTag -- small OR-able tag set for cross-cutting equipment properties
// (independent of EquipmentKind). Bit positions so an EquipmentTagMask fits a
// uint32. Deliberately coarse now; a later slice widens it as needs appear.
// -------------------------------------------------------------------------
enum EquipmentTag {
    ETAG_OMNI          = 0,   // omni-pod / hot-swappable
    ETAG_CLAN          = 1,   // clan-tech
    ETAG_EXPERIMENTAL  = 2,   // experimental / advanced
    ETAG_DESTROYABLE   = 3,   // can be crit-destroyed independently
    ETAG_EXPLOSIVE     = 4,   // explosive on crit (ammo, gauss)
    NUM_EQUIPMENT_TAGS        // keep last; must stay <= 32
};

typedef unsigned int EquipmentTagMask;

inline EquipmentTagMask equipmentTagBit(EquipmentTag t) {
    return (EquipmentTagMask)1u << (unsigned)t;
}

// -------------------------------------------------------------------------
// Optional non-weapon facet STUBS. Each is a minimal struct + a presence flag on
// EquipmentDefinition. Full fields are LATER slices -- these exist now only so the
// aggregator shape is complete and presence can be tested. Keep them tiny: a stub
// carries just enough to prove "this facet is present".
// -------------------------------------------------------------------------

// Ammo as a top-level equipment kind (distinct from a weapon's WeaponAmmoFacet,
// which describes the weapon's consumption). This is the AMMO ITEM's own named
// category + rounds-per-ton. Empty category = unset.
struct AmmoFacet {
    WeaponStringId ammoCategory;   // named category id this ammo item satisfies
    int            roundsPerTon;   // capacity metadata (0 = unset)
    AmmoFacet() : roundsPerTon(0) {}
};

struct SensorFacet {
    float sensorRange;             // detection range (0 = unset)
    SensorFacet() : sensorRange(0.f) {}
};

struct ECMFacet {
    float ecmRadius;               // ECM bubble radius (0 = unset)
    ECMFacet() : ecmRadius(0.f) {}
};

struct JumpJetFacet {
    float thrust;                  // per-jet thrust contribution (0 = unset)
    JumpJetFacet() : thrust(0.f) {}
};

// -------------------------------------------------------------------------
// PresentationProfile slot -- look/sound, kept DISTINCT from behavior facets
// (behavior != presentation). For a weapon this overlaps the WeaponFacet's own
// WeaponPresentationProfile; at the equipment level this is the generic slot any
// equipment kind can use (e.g. a jump-jet's exhaust VFX). Minimal now.
// -------------------------------------------------------------------------
struct EquipmentPresentationProfile {
    WeaponStringId activateVfxId;  // generic activate/use VFX cue
    WeaponStringId activateSfxId;  // generic activate/use SFX cue
    EquipmentPresentationProfile() {}
};

// -------------------------------------------------------------------------
// Plain metadata common to all equipment (heat/cost/tech/era). No engine types.
// -------------------------------------------------------------------------
struct EquipmentMetadata {
    float heat;      // heat generated (or dissipated, sign per kind); 0 = none
    float cost;      // C-Bill / value metric
    int   techBase;  // 0 = inner-sphere, 1 = clan, ... (author convention)
    int   era;       // introduction era (author convention; 0 = unset)
    EquipmentMetadata() : heat(0.f), cost(0.f), techBase(0), era(0) {}
};

// -------------------------------------------------------------------------
// EquipmentDefinition -- THE top type. A facet aggregator, NOT a blob.
// -------------------------------------------------------------------------
// Identity is the canonical `modId:name` STRING id (stored as a QualifiedId; the
// FNV-hash used by WeaponId is an impl detail, not the identity contract). Two
// equipment defs are the same iff their canonical qualified ids are equal.
//
// `legacyMasterId` is a COMPATIBILITY ALIAS to the old compbas row ordinal (the
// 8-bit masterID). It is NEVER the identity: two definitions with the SAME
// legacyMasterId but DIFFERENT modId:name are DISTINCT equipment. Default -1 =
// "no legacy alias" (a natively-authored mod component with no compbas row).
struct EquipmentDefinition {
    // --- Identity (the real identity) ---
    mc2id::QualifiedId  id;             // canonical modId:name (identity)

    // --- Classification ---
    EquipmentKind       kind;           // which compbas family
    EquipmentTagMask    tags;           // OR of equipmentTagBit(...)

    // --- Compatibility bridge (NOT identity) ---
    int                 legacyMasterId; // old compbas ordinal / 8-bit masterID alias; -1 = none

    // --- Optional facets (presence flag + payload) ---
    bool         hasWeapon;    WeaponFacet   weapon;
    bool         hasAmmo;      AmmoFacet     ammo;
    bool         hasSensor;    SensorFacet   sensor;
    bool         hasECM;       ECMFacet      ecm;
    bool         hasJumpJet;   JumpJetFacet  jumpJet;

    // --- Presentation + metadata ---
    EquipmentPresentationProfile presentation;
    EquipmentMetadata            meta;

    EquipmentDefinition()
        : kind(EQ_OTHER), tags(0u), legacyMasterId(-1),
          hasWeapon(false), hasAmmo(false), hasSensor(false),
          hasECM(false), hasJumpJet(false) {}

    // An equipment def is valid iff its canonical id parsed to a well-formed
    // qualified id. (legacyMasterId has NO bearing on validity or identity.)
    bool valid() const { return id.valid; }

    // Canonical string id (empty when !valid). THE identity string.
    std::string idStr() const { return id.str(); }
};

// -------------------------------------------------------------------------
// Construction helpers (author ergonomics).
// -------------------------------------------------------------------------

// Build an EquipmentDefinition from a raw id string (`modId:name`, bare name ->
// core:). Kind defaults to Other; caller adds facets. Invalid id -> !valid().
inline EquipmentDefinition makeEquipment(const char* rawId, EquipmentKind kind) {
    EquipmentDefinition e;
    e.id = mc2id::parseQualifiedId(rawId ? std::string(rawId) : std::string());
    e.kind = kind;
    return e;
}

// Attach a WeaponFacet, flagging the def as a weapon (sets kind + presence flag).
inline void setWeaponFacet(EquipmentDefinition& e, const WeaponFacet& w) {
    e.weapon = w;
    e.hasWeapon = true;
    e.kind = EQ_WEAPON;
}

// -------------------------------------------------------------------------
// Identity guarantee -- legacyMasterId is an ALIAS, never the identity.
// -------------------------------------------------------------------------
// Two equipment definitions are the same equipment IFF their canonical qualified
// ids are equal. legacyMasterId is deliberately EXCLUDED: mod load order (which
// determines compbas ordinals during the shadow path) must NEVER change equipment
// identity. This is the pure predicate a registry / validator leans on.
inline bool sameEquipmentIdentity(const EquipmentDefinition& a,
                                  const EquipmentDefinition& b) {
    return a.id.valid && b.id.valid && a.id == b.id;
}

// -------------------------------------------------------------------------
// Facet presence + accessors (pure helpers).
// -------------------------------------------------------------------------
inline bool hasFacet(const EquipmentDefinition& e, EquipmentKind kind) {
    switch (kind) {
        case EQ_WEAPON:  return e.hasWeapon;
        case EQ_AMMO:    return e.hasAmmo;
        case EQ_SENSOR:  return e.hasSensor;
        case EQ_ECM:     return e.hasECM;
        case EQ_JUMPJET: return e.hasJumpJet;
        default:         return false;
    }
}

inline bool isWeapon(const EquipmentDefinition& e) { return e.hasWeapon; }

// Accessor for the composed WeaponFacet (only meaningful when isWeapon()).
inline const WeaponFacet& weaponFacet(const EquipmentDefinition& e) { return e.weapon; }
inline       WeaponFacet& weaponFacet(EquipmentDefinition& e)       { return e.weapon; }

// Tag helpers.
inline void addEquipmentTag(EquipmentDefinition& e, EquipmentTag t) {
    e.tags |= equipmentTagBit(t);
}
inline bool equipmentHasTag(const EquipmentDefinition& e, EquipmentTag t) {
    return (e.tags & equipmentTagBit(t)) != 0u;
}

// -------------------------------------------------------------------------
// The unit-spine join, THROUGH the WeaponFacet. A weapon-equipment fits a
// hardpoint iff it carries a WeaponFacet whose category the hardpoint accepts.
// Non-weapon equipment never "fits" a weapon hardpoint (returns false). Delegates
// to weaponFitsHardpoint() (weapon_definition.h) -> hardpointAcceptsWeapon().
// -------------------------------------------------------------------------
inline bool equipmentFitsHardpoint(const EquipmentDefinition& e,
                                   const HardpointProfile& hp) {
    if (!e.hasWeapon) return false;
    return weaponFitsHardpoint(e.weapon, hp);
}

#endif // EQUIPMENT_DEFINITION_H
