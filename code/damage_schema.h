#ifndef DAMAGE_SCHEMA_H
#define DAMAGE_SCHEMA_H
// DAMAGE-SCHEMA-FACET-1 (slice 5 of the UNIT-PROFILE arc).
//
// Data-owned DAMAGE SCHEMA: which hit-location model a unit uses. This is a pure
// DATA facet — it declares the *shape* of a unit's hit locations (mech 8 body
// parts / vehicle facings / squad members / single actor). It does NOT touch the
// armor / internal-structure / hit-application MATH (injureBodyLocation, body[],
// bodyLocation, criticalSpacesReq …); those stay exactly as they are this slice.
//
// Why it exists now: Phase 2's HardpointId join key needs to VALIDATE that a
// loadout's damage-location reference (e.g. "RightArm") actually exists in the
// unit's schema — "Damage location RightArm referenced but schema has no
// RightArm" is one of the modder-readable errors the roadmap promises. A future
// resolveHit(HitRequest) also routes through the schema id to pick the location
// model. So this slice introduces the id + a pure location-membership query the
// validator can consult; nobody reads the facet at runtime yet (pure additive).
//
// Mirrors LOCOMOTION-FACET-1 (slice 4) exactly: pure, header-only (plain enums /
// const char*), so it compiles into the offline `mc2_tests` target and drags no
// engine/GL/IO code. Set once at type-load (BattleMechType::init = DMG_MECH_8PART),
// vehicle/base default = DMG_VEHICLE_FACINGS, optional `.fit [UnitProfile]
// DamageSchema=` token override via parseDamageSchema().

// Which hit-location model a unit uses.
enum DamageSchemaId {
    DMG_MECH_8PART,        // Head/CTorso/L+RTorso/L+RArm/L+RLeg  (8 body parts)
    DMG_VEHICLE_FACINGS,   // Front/Left/Right/Rear/Turret        (facings)
    DMG_SQUAD,             // N discrete squad members            (per-member)
    DMG_SINGLE_ACTOR       // one indivisible hit box             (turret/gun-emplacement/etc.)
};

// Logical damage locations, spanning every schema. Phase 2's HardpointId join
// binds a loadout mount + model socket to one of these ids by NAME; a validator
// asks schemaHasLocation(schema, loc) to reject an impossible reference. The
// mech ids mirror the existing MECH_BODY_LOCATION_* order (mech.h); the vehicle
// facings mirror the GroundVehicle bodyLocationString[] order (gvehicl.cpp). This
// is a pure vocabulary — it deliberately does NOT include the numeric values of
// the engine defines, so this header stays engine/IO-free and offline-testable.
enum DamageLocationId {
    // --- DMG_MECH_8PART ---
    DLOC_MECH_HEAD,
    DLOC_MECH_CTORSO,
    DLOC_MECH_LTORSO,
    DLOC_MECH_RTORSO,
    DLOC_MECH_LARM,
    DLOC_MECH_RARM,
    DLOC_MECH_LLEG,
    DLOC_MECH_RLEG,
    // --- DMG_VEHICLE_FACINGS ---
    DLOC_VEH_FRONT,
    DLOC_VEH_LEFT,
    DLOC_VEH_RIGHT,
    DLOC_VEH_REAR,
    DLOC_VEH_TURRET,
    // --- DMG_SQUAD / DMG_SINGLE_ACTOR ---
    DLOC_BODY,             // the single/whole-actor location (squad member or single actor)
    NUM_DAMAGE_LOCATION_IDS
};

// Case-insensitive ASCII equality for a facet token vs a fixed key. (Same helper
// shape as locomotion_facet.h::locomotionTokenEquals — kept local so this header
// stands alone for the offline test target.)
inline bool damageSchemaTokenEquals(const char* a, const char* key) {
    if (!a || !key) return false;
    for (; *a && *key; ++a, ++key) {
        char ca = *a, ck = *key;
        if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
        if (ck >= 'A' && ck <= 'Z') ck = char(ck - 'A' + 'a');
        if (ca != ck) return false;
    }
    return *a == '\0' && *key == '\0';
}

// Map an optional `.fit [UnitProfile] DamageSchema=` token to a DamageSchemaId.
// Unknown / empty / null token -> `fallback` (the class-default the caller
// supplies), so stock .fits that omit the key preserve parity. Recognized tokens
// mirror the enum: Mech8Part, VehicleFacings, Squad, SingleActor.
inline DamageSchemaId parseDamageSchema(const char* token,
                                        DamageSchemaId fallback) {
    if (!token || token[0] == '\0') return fallback;
    if (damageSchemaTokenEquals(token, "Mech8Part"))      return DMG_MECH_8PART;
    if (damageSchemaTokenEquals(token, "VehicleFacings")) return DMG_VEHICLE_FACINGS;
    if (damageSchemaTokenEquals(token, "Squad"))          return DMG_SQUAD;
    if (damageSchemaTokenEquals(token, "SingleActor"))    return DMG_SINGLE_ACTOR;
    return fallback;
}

// How many discrete hit locations a schema exposes. DMG_SQUAD is variable at
// runtime (member count is per-unit data a later slice supplies); the pure query
// reports its structural minimum of 1 (the whole-squad body). Phase-2 validators
// use this as a coarse bound; schemaHasLocation() is the precise membership test.
inline int schemaLocationCount(DamageSchemaId s) {
    switch (s) {
        case DMG_MECH_8PART:      return 8;   // NUM_MECH_BODY_LOCATIONS - 1 (ANY excluded)
        case DMG_VEHICLE_FACINGS: return 5;   // NUM_GROUNDVEHICLE_LOCATIONS
        case DMG_SQUAD:           return 1;   // structural minimum; members are per-unit data
        case DMG_SINGLE_ACTOR:    return 1;
    }
    return 1;
}

// Does a given logical damage location belong to this schema? The Phase-2
// HardpointId validator calls this to reject "loadout references RightArm but the
// unit's DamageSchema has no RightArm" (a mech location on a vehicle-facings unit,
// etc.). Pure lookup, no engine state.
inline bool schemaHasLocation(DamageSchemaId s, DamageLocationId loc) {
    switch (s) {
        case DMG_MECH_8PART:
            return loc >= DLOC_MECH_HEAD && loc <= DLOC_MECH_RLEG;
        case DMG_VEHICLE_FACINGS:
            return loc >= DLOC_VEH_FRONT && loc <= DLOC_VEH_TURRET;
        case DMG_SQUAD:
        case DMG_SINGLE_ACTOR:
            return loc == DLOC_BODY;
    }
    return false;
}

#endif // DAMAGE_SCHEMA_H
