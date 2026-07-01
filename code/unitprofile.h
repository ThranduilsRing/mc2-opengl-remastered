#ifndef UNITPROFILE_H
#define UNITPROFILE_H
// UNIT-PROFILE-SEAM-1: static baseline facets (ObjectType) + current runtime
// facets (Mover). Slice 1 facet = capability.
#include "capability.h"
#include "locomotion_facet.h"          // LocomotionModel enum + parseLocomotionModel
#include "damage_schema.h"             // DamageSchemaId enum + parseDamageSchema
#include "ui_profile.h"                // UIProfile facet + accessor-override helpers

bool mc2_unitprofile_parse_enabled(const char* envValue);   // pure, testable
bool mc2UnitProfileDataEnabled();                            // cached getenv

// EQUIPMENT-REGISTRY-FACADE-1 (equipment arc, engine slice E2). Gated build +
// verification dump of the parallel equipment registry over masterList. Called
// once after MasterComponent::loadMasterList. Gate-OFF: no-op (byte-identical).
// ADDITIVE: reads masterList only; touches no firing/loadout/inventory/save path.
void buildAndDumpEquipmentRegistryIfEnabled();

// FIRE-EXECUTOR-REGISTRY-1 (equipment arc, engine slice E7). Gated proof that
// the weapon fireExecutor lookup resolves end-to-end: for each weapon-form
// MasterComponent it derives the converter's default fireExecutor id and
// resolves it against theFireExecutorRegistry(), reporting bound/unbound counts
// + samples. Called after buildAndDumpEquipmentRegistryIfEnabled(). Gate-OFF:
// no-op (byte-identical). ★ SCHEMA/DISPATCH ONLY -- resolves names, fires
// nothing; the legacy weaponFire path stays authoritative.
void dumpFireExecutorProofIfEnabled();

// LOADOUT-EQUIPMENT-ID-ALIAS-1 (equipment arc, engine slice E5). Accessor for
// the process-global equipment registry populated by the E2 facade above. The
// loadout parser queries it (gate-ON) to resolve an optional `core:<slug>`
// equipment reference to its legacy masterID ordinal (see equipment_registry.h
// resolveLoadoutEquipmentRef). Empty until buildEquipmentRegistryFromMasterList
// runs under the gate.
class EquipmentRegistry;  // fwd; heavy header kept out of unitprofile.h
const EquipmentRegistry& getEquipmentRegistry();

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
    // UI-PROFILE-1 (Phase-2 slice 6): data-owned UI presentation (icon/portrait/
    // minimap/silhouette by NAME + optional name/encyclopedia string-id override).
    // Default is an all-empty UIProfile (every field unmapped) -> every accessor
    // seam keeps its legacy iconFileNames[]/atlas/IDS_ lookup. Populated ONLY from
    // an optional `.fit [UIProfile]` block; stock .fits declare none, so stock UI
    // is byte-identical. PURE DATA facet: it overrides ONLY which asset/id an
    // accessor selects, never the icon-atlas blit math (mechicon.cpp CANARY).
    UIProfile ui;
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
    // UNIT-DEFINITION-SHADOW-COMPARE-1 (Phase 3): additive per-mover facet fields
    // so the offline shadow_compare.py can (a) match a dump row to a converter
    // unit by chassis stem and (b) diff the engine-derived locomotion/damage
    // schema against the converter-generated facet. `chassis` is the ObjectType
    // appearance name (tgl model stem == the converter's --mech/--vehicle stem);
    // the two enum ints carry the LocomotionModel / DamageSchemaId values (their
    // canonical token names come from locomotionModelName()/damageSchemaName()).
    char chassis[64];       // ObjectType appearance stem; "" if unavailable
    int  locomotion;        // LocomotionModel enum value
    int  damageSchema;      // DamageSchemaId enum value
    // LOADOUT-THREE-LAYER-1 (Phase 3): spawned VARIANT identity so shadow_compare
    // can join by (chassis, variant) instead of chassis stem. The converter now
    // emits one mechdef per variant; joining on chassis alone mismatches whenever
    // the spawned variant's loadout differs from the FIRST CSV variant (root of
    // the Atlas-jump finding). variant == "" (base Mover / non-mech) -> the diff
    // falls back to a chassis-level join, preserving the pre-Phase-3 behavior.
    char variant[64];       // spawned variant name (Mover::getVariantName()); "" if none
    long variantId;         // Mover::getVariantId(); -1 if none
};
extern "C" int mc2_unitprofile_collect_witness(UnitProfileWitnessRow* out, int maxRows);

// WEAPON-SHADOW-COMPARE-1 (equipment arc, slice 5): the equipment safety-net.
// A per-loaded-MasterComponent witness so the offline weapon_shadow_compare.py
// can diff the compbas converter's generated EquipmentDefinitions against the
// ENGINE's authoritative parse of compbas.csv (MasterComponent::masterList).
// This is the equipment mirror of UnitProfileWitnessRow. The walk lives
// engine-side (unitprofile_fit.cpp) because debug_state_dump.cpp keeps no game
// headers; exposed as a flat POD array. READ-ONLY; component loading behavior
// is untouched. Field spellings/enum-token names match the converter's
// to_report_dict() so the Python join can string-diff directly:
//   masterId   = the compbas row ordinal == converter legacy_master_id (JOIN KEY)
//   form       = ComponentFormString[form] token (Engine token; the converter
//                maps CSV Type -> EquipmentKind, so kind is derived, not equal)
//   kind       = the converter's EquipmentKind token derived from `form`, so the
//                two agree by construction on the engine side (kind is checked
//                against the converter's coarser family classification)
//   category   = weapon WeaponTag token (Energy/Ballistic/Missile) or "" (non-weapon)
//   damage/heat/recycle = weapon combat stats (0 for non-weapons)
//   rangeBucket = "short"/"medium"/"long"/"" (WeaponRangeString[stats.weapon.range])
//   combatValue = MasterComponent CV (the converter's BR column -> combat_value)
//   ammoMasterId = stats.weapon.ammoMasterId (the converter's ammo_master_id)
struct ComponentWitnessRow {
    long masterId;          // compbas row ordinal (JOIN KEY == converter legacy_master_id)
    char form[24];          // ComponentFormString[form] token
    char kind[16];          // derived EquipmentKind token (converter vocabulary)
    char category[16];      // weapon WeaponTag token or "" (non-weapon)
    char rangeBucket[8];    // "short"/"medium"/"long"/"" (weapons only)
    int  isWeapon;          // 0/1
    float damage;           // stats.weapon.damage (0 for non-weapons)
    float heat;             // stats.weapon.heat
    float recycle;          // stats.weapon.recycleTime
    float combatValue;      // MasterComponent CV
    int  ammoMasterId;      // stats.weapon.ammoMasterId
    int  critSlots;         // InventorySize (size) — the converter's crit_slots
};
extern "C" int mc2_component_collect_witness(ComponentWitnessRow* out, int maxRows);

// UNIT-PREVIEW-DUMP-1 (Phase 3): a headless, per-loaded-unit-TYPE presentation
// preview. Gathers the RESOLVED facts a modder needs to verify a unit WITHOUT
// launching an interactive mission: the model/appearance name, the declared
// presentation sockets (with per-socket "present on the loaded model?" flag),
// which ActionAnimTags resolve to a gesture via the pure fallback resolver
// (given the loaded skeleton's actual per-gesture clip presence), the resolved
// UI icon, and the aggregate validateUnitDefinition() finding count for the
// type. READ-ONLY from already-loaded type data — reuses the same collector
// plumbing as UnitProfileWitnessRow and loads NO new assets. One row per unique
// ObjectType (de-duplicated across movers). Present in the dump ONLY under
// MC2_UNIT_PROFILE_DATA (schema byte-for-byte unchanged when the gate is OFF).
enum {
    UNIT_PREVIEW_MAX_SOCKETS   = 32,   // mirrors MAX_PRESENTATION_SOCKETS
    UNIT_PREVIEW_SOCKET_CAP    = 48,   // mirrors PRESENTATION_SOCKET_NAME_CAP
    UNIT_PREVIEW_ANIM_TAG_COUNT = 11,  // count of ActionAnimTag enumerators
};
struct UnitPreviewSocket {
    char name[UNIT_PREVIEW_SOCKET_CAP];
    int  present;   // 0/1: a model node of this exact name exists on the LOD0 shape
};
struct UnitPreviewWitnessRow {
    long objectId;                                  // representative mover's partId
    char name[64];                                  // mover display name ("" if none)
    char chassis[64];                               // ObjectType appearance stem
    char variant[64];                               // spawned variant name ("" if none)
    int  isMech;                                    // 0/1: BATTLEMECH object class
    int  socketCount;                               // declared presentation sockets
    UnitPreviewSocket sockets[UNIT_PREVIEW_MAX_SOCKETS];
    int  missingSocketCount;                        // declared sockets absent on model
    // Per-ActionAnimTag availability, indexed by the ActionAnimTag enum value
    // (0..UNIT_PREVIEW_ANIM_TAG_COUNT-1). 1 = the tag's fallback chain resolves to
    // a gesture the loaded skeleton actually provides; 0 = it dead-ends (missing).
    // For a non-mech (no skeleton) every entry is 0.
    int  animTagAvailable[UNIT_PREVIEW_ANIM_TAG_COUNT];
    char iconResolved[128];                         // resolved UI icon asset ("" if none)
    int  findingCount;                              // validateUnitDefinition finding count
    int  firstFindingCode;                          // first finding's reason code (-1 if none)
    char firstFindingMsg[128];                      // first finding's message ("" if none)
};
extern "C" int mc2_unitpreview_collect_witness(UnitPreviewWitnessRow* out, int maxRows);
// Canonical ActionAnimTag token for enum index i (0..UNIT_PREVIEW_ANIM_TAG_COUNT-1),
// or "" if out of range. Used by the dump writer (engine-free TU) to label columns.
extern "C" const char* mc2_unitpreview_anim_tag_name(int i);
#endif // UNITPROFILE_H
