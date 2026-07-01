#ifndef UNITPROFILE_H
#define UNITPROFILE_H
// UNIT-PROFILE-SEAM-1: static baseline facets (ObjectType) + current runtime
// facets (Mover). Slice 1 facet = capability.
#include "capability.h"
#include "locomotion_facet.h"          // LocomotionModel enum + parseLocomotionModel
#include "damage_schema.h"             // DamageSchemaId enum + parseDamageSchema

bool mc2_unitprofile_parse_enabled(const char* envValue);   // pure, testable
bool mc2UnitProfileDataEnabled();                            // cached getenv

class UnitProfile {                  // static, on ObjectType (flyweight)
  public:
    CapabilitySet baselineCapabilities;
    // LOCOMOTION-FACET-1 (slice 4): data-owned movement mode. Replaces the
    // transitional Mover/BattleMech locomotionModel() class-derivation. Default
    // is LOCO_WHEELED (a non-jump model), matching slice 3's base-Mover value;
    // BattleMechType::init sets LOCO_MECH_WALKER at type-load, and an optional
    // `.fit [UnitProfile] Locomotion=` token overrides either. Mover::
    // locomotionModel() now reads THIS field instead of asking "is BattleMech?".
    LocomotionModel locomotion = LOCO_WHEELED;
    // DAMAGE-SCHEMA-FACET-1 (slice 5): data-owned hit-location model (which
    // damage schema a unit uses). Default is DMG_VEHICLE_FACINGS (a non-mech
    // baseline, matching the GroundVehicle facing model); BattleMechType::init
    // sets DMG_MECH_8PART at type-load, and an optional `.fit [UnitProfile]
    // DamageSchema=` token overrides either. PURE DATA facet: nobody reads it at
    // runtime yet — it is the source a future resolveHit(HitRequest) picks its
    // location model from, and the source the Phase-2 HardpointId validator
    // consults (schemaHasLocation) to reject impossible loadout↔location refs.
    // This slice does NOT change any armor / hit-application math.
    DamageSchemaId damageSchema = DMG_VEHICLE_FACINGS;
};
struct UnitRuntimeState {            // mutable, on Mover
    CapabilitySet currentCapabilities;
};

class FitIniFile;                      // fwd; keep heavy I/O header out of unitprofile.h
void populateProfileFromFit(FitIniFile& fit, UnitProfile& profile);

// VEHICLE-JUMP-PROOF-1 (slice 7, Phase-1 closer) — THROWAWAY/proof-only hook.
// Gated behind MC2_UNIT_PROFILE_VEHICLE_JUMP_PROOF (default OFF, SEPARATE from
// MC2_UNIT_PROFILE_DATA). When ON, a GroundVehicleType's profile is given
// locomotion=LOCO_MECH_WALKER + CAP_JUMP via the SAME facet fields the .fit path
// writes, so a non-BattleMech reports canPerform(Jump)=Available through the full
// data->facet->registry->queryAction stack. This proves the jump DECISION is
// data-driven and class-independent. It does NOT make the vehicle functionally
// jump (no jump-movement executor runs for vehicles — that is the deferred
// LOCOMOTION-DATA-1 work). Remove once the arc has a real data-unit proof.
void mc2_unitprofile_vehicle_jump_proof(UnitProfile& profile);

// UNITPROFILE-VALIDATOR-1 (slice 6): run the pure construction-rules validator
// (unit_profile_validator.h) over a type's now-populated facets and REPORT each
// finding to the console. REPORT-ONLY — never rejects/refuses a spawn — and
// GATED behind mc2UnitProfileDataEnabled(): a no-op when the gate is OFF, so
// gate-OFF and a clean stock gate-ON run both log nothing (stock units are
// coherent). `typeName` is a human/modder label (the .fit filename) prefixed
// onto each logged line. Call at type-load AFTER facets + populateProfileFromFit.
void validateAndReportUnitProfile(const UnitProfile& profile, const char* typeName);

// UNIT-PROFILE-SEAM-1: state-dump witness bridge. debug_state_dump.cpp keeps no
// game headers, so the mover walk lives engine-side (unitprofile_fit.cpp) and is
// exposed as a flat POD array, mirroring gos_getMechTextureNameByNodeIdx.
struct UnitProfileWitnessRow {
    long objectId;
    int  capJumpBaseline;   // 0/1
    int  capJumpCurrent;    // 0/1
    int  canPerformJump;    // 0/1
};
extern "C" int mc2_unitprofile_collect_witness(UnitProfileWitnessRow* out, int maxRows);
#endif // UNITPROFILE_H
