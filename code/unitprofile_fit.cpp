// code/unitprofile_fit.cpp  (engine ONLY — never in mc2_tests)
// UNIT-PROFILE-SEAM-1: optional, additive .fit [UnitProfile] data read.
// inifile.h drags file.h -> stdio/fcntl/heap (real file I/O), which the
// doctest target forbids, so this populator lives in its own engine-only TU.
#include "unitprofile.h"
#include "unit_definition.h"             // SINGLE-SOURCE-UNIT-DEFINITION-1: mergeUIProfile (single UI resolution)
#include "ui_profile_registry.h"         // uiProfileForChassis (additive-merge registry — the shared UI source)
#include "unit_profile_validator.h"      // UNITPROFILE-VALIDATOR-1: pure validator + facts
#include "action_executor_registry.h"    // theActionExecutorRegistry()
#include "inifile.h"                    // resolves via mclib/ on the engine include path
#include "objmgr.h"
#include "mover.h"
#include "objtype.h"
#include "cmponent.h"                    // WEAPON-SHADOW-COMPARE-1: MasterComponent::masterList
#include <cstdio>                        // printf (report-only finding log)
#include <cstdlib>                       // getenv (throwaway test hook)
#include <cstring>                        // strncpy (chassis stem copy)

void populateProfileFromFit(FitIniFile& fit, UnitProfile& p) {
    // Optional, additive (grant-only, never clears). Stock .fits have no
    // [UnitProfile] block -> no-op for all shipped data. A FAILED seekBlock
    // returns BLOCK_NOT_FOUND without mutating cursor state, so an absent
    // block is side-effect-free -> stock parity clean.
    if (fit.seekBlock("UnitProfile") == NO_ERR) {
        bool jump = false;
        if (fit.readIdBoolean("Jump", jump) == NO_ERR && jump)
            p.baselineCapabilities.set(CAP_JUMP, true);
        // LOCOMOTION-FACET-1 (slice 4): optional data override of the locomotion
        // model. Absent key -> p.locomotion keeps whatever the type-load default
        // set (walker for mechs, wheeled otherwise), so stock .fits are parity.
        char loco[64] = {0};
        if (fit.readIdString("Locomotion", loco, sizeof(loco)) == NO_ERR)
            p.locomotion = parseLocomotionModel(loco, p.locomotion);
        // DAMAGE-SCHEMA-FACET-1 (slice 5): optional data override of the damage
        // schema (hit-location model). Absent key -> p.damageSchema keeps whatever
        // the type-load default set (mech-8part for mechs, vehicle-facings
        // otherwise), so stock .fits are parity.
        char dmg[64] = {0};
        if (fit.readIdString("DamageSchema", dmg, sizeof(dmg)) == NO_ERR)
            p.damageSchema = parseDamageSchema(dmg, p.damageSchema);
    }
    // UI-PROFILE-1 (Phase-2 slice 6): optional data-owned UI presentation. Lives
    // in a SEPARATE optional [UIProfile] block (a UNIT may declare UI without
    // declaring capabilities, and vice-versa). Absent block or absent key -> the
    // corresponding UIProfile field stays empty/UNSET -> the accessor seam keeps
    // its legacy iconFileNames[]/IDS_ lookup, so stock .fits are byte-identical.
    // Read-only into the facet; NO atlas / blit-math touch here.
    //
    // Read the inline `.fit [UIProfile]` block into a LOCAL first, so slice-7 can
    // merge it with the shared registry through the ONE resolution point below.
    UIProfile inlineUI;
    bool haveInline = false;
    if (fit.seekBlock("UIProfile") == NO_ERR) {
        haveInline = true;
        char buf[UI_ASSET_NAME_CAP] = {0};
        if (fit.readIdString("Icon", buf, sizeof(buf)) == NO_ERR)
            inlineUI.icon = UIAssetName(buf);
        if (fit.readIdString("Portrait", buf, sizeof(buf)) == NO_ERR)
            inlineUI.portrait = UIAssetName(buf);
        if (fit.readIdString("Minimap", buf, sizeof(buf)) == NO_ERR)
            inlineUI.minimap = UIAssetName(buf);
        if (fit.readIdString("Silhouette", buf, sizeof(buf)) == NO_ERR)
            inlineUI.silhouette = UIAssetName(buf);
        long id = UI_STRING_ID_UNSET;
        if (fit.readIdLong("NameID", id) == NO_ERR)
            inlineUI.nameStringId = id;
        id = UI_STRING_ID_UNSET;
        if (fit.readIdLong("EncyclopediaID", id) == NO_ERR)
            inlineUI.encyclopediaStringId = id;
    }

    // SINGLE-SOURCE-UNIT-DEFINITION-1 (slice 7): unify the UI facet resolution.
    // Slice 6 populated the sim-side p.ui from the inline block ONLY, while the
    // logistics side (LogisticsChassis::init) resolved the additive-merge
    // REGISTRY (uiProfileForChassis) into its own override fields — two resolution
    // points that could diverge (a registry-declared UI was invisible to the sim
    // facet). Route BOTH through the ONE pure merge (mergeUIProfile: registry over
    // inline), so p.ui — read via UnitDefinition::ui() — now reflects the SAME
    // registry entry the mechlab sees. uiProfileForChassis() is itself gated
    // (returns null when MC2_UNIT_PROFILE_DATA is OFF, or the data file is absent),
    // so a stock run merges null-over-inline == inline, i.e. no behavior change.
    const UIProfile* regUI = uiProfileForChassis(fit.getFilename());
    if (haveInline || regUI)
        p.ui = mergeUIProfile(haveInline ? &inlineUI : nullptr, regUI);
}

// VEHICLE-JUMP-PROOF-1 (slice 7, Phase-1 closer) — THROWAWAY/proof-only hook.
// Repacking objects.pak in-session to author a vehicle .fit is infeasible, so
// this writes the SAME two facet fields a `.fit [UnitProfile]` block would
// (Jump=TRUE -> baselineCapabilities CAP_JUMP; Locomotion=MechWalker ->
// locomotion) directly onto a GroundVehicleType's profile at type-load. It is a
// pure DATA mutation of the facet layer — no C++ class poke, no executor virtual.
// Gate MC2_UNIT_PROFILE_VEHICLE_JUMP_PROOF is SEPARATE from MC2_UNIT_PROFILE_DATA
// so the proof is strictly opt-in and default-OFF stays stock-parity.
//
// Effect through the existing stack (nothing new added below the facets):
//   locomotion=LOCO_MECH_WALKER -> Mover::locomotionModel() reads MechWalker
//     -> theActionExecutorRegistry().isCompatible(Jump, MechWalker) == true
//   CAP_JUMP baseline -> Mover::onTypeBound copies baseline->current
//     -> hasCapability(CAP_JUMP) == true
//   => Mover::queryAction(Jump) == Available on a GroundVehicle (a NON-BattleMech).
//
// HONEST FRAMING: this proves the jump DECISION path is class-independent (data
// facets decide, not the C++ class). It does NOT mean the vehicle jumps: no
// jump-*movement* code runs for vehicles (deferred LOCOMOTION-DATA-1 executor
// work). The proof is "jump-Available reached on a non-mech purely via data."
void mc2_unitprofile_vehicle_jump_proof(UnitProfile& p) {
    static const bool on = mc2_unitprofile_parse_enabled(
        getenv("MC2_UNIT_PROFILE_VEHICLE_JUMP_PROOF"));
    if (!on)
        return;
    // Same facet fields the .fit path writes — a compatible locomotion model plus
    // the capability. Order matches populateProfileFromFit: capability then model.
    p.baselineCapabilities.set(CAP_JUMP, true);
    p.locomotion = LOCO_MECH_WALKER;
}

// UNITPROFILE-VALIDATOR-1 (slice 6): run the pure construction-rules validator
// over a type's populated facets and log each finding. GATED + REPORT-ONLY:
//   * gate OFF -> returns immediately, no output (behavioral parity).
//   * gate ON, coherent unit (stock walker+jump, stock vehicle no-jump) ->
//     validator returns no findings, so still no output.
//   * gate ON, incoherent authored profile (e.g. vehicle granted CAP_JUMP) ->
//     one "[UnitDefinition:VALIDATE]" line per finding. NO spawn rejection.
// The validator consults the SAME theActionExecutorRegistry() gameplay queries
// use, so its verdict can never diverge from the runtime executor resolution.
void validateAndReportUnitProfile(const UnitProfile& profile, const char* typeName) {
    if (!mc2UnitProfileDataEnabled())
        return;   // gate OFF: no validation, no output -> parity with legacy

    // UNIT-DEFINITION-VALIDATOR-1 (Phase-2 slice 8): route the jump-facet check
    // through the ONE consolidated pass instead of calling validateUnitProfile()
    // directly. At the type-load site only the profile facet is in hand (loadout
    // inventory / loaded shape belong to later lifecycle points and have their own
    // gathering sites that likewise call validateUnitDefinition); the other input
    // lists stay empty -> zero extra findings, identical output to the old call.
    UnitDefinitionValidationInputs in;
    in.profile.grantsJump = profile.baselineCapabilities.has(CAP_JUMP);
    in.profile.locomotion = profile.locomotion;

    const std::vector<UnitProfileFinding> findings =
        validateUnitDefinition(in, theActionExecutorRegistry());

    const char* label = (typeName && typeName[0]) ? typeName : "<unknown>";
    for (const UnitProfileFinding& f : findings) {
        // Report-only: describe the incoherence in modder terms; do not reject.
        printf("[UnitDefinition:VALIDATE] %s: %s (reason=%d)\n",
               label, f.message, static_cast<int>(f.code));
    }
}

// UNIT-PROFILE-SEAM-1: fill the state-dump witness array from live movers.
// Returns the number of rows written (capped at maxRows). Read-only.
extern "C" int mc2_unitprofile_collect_witness(UnitProfileWitnessRow* out, int maxRows) {
    if (!out || maxRows <= 0 || !ObjectManager)
        return 0;
    int n = 0;
    const long numMovers = ObjectManager->getNumMovers();
    for (long i = 0; i < numMovers && n < maxRows; ++i) {
        MoverPtr m = ObjectManager->getMover(i);
        if (!m)
            continue;
        ObjectTypePtr ot = m->getObjectType();
        const UnitProfile emptyProfile;
        const UnitProfile& prof = ot ? ot->profile() : emptyProfile;
        const bool baseline = prof.baselineCapabilities.has(CAP_JUMP);
        out[n].objectId        = m->getPartId();
        out[n].capJumpBaseline = baseline ? 1 : 0;
        out[n].capJumpCurrent  = m->hasCapability(CAP_JUMP) ? 1 : 0;
        out[n].canPerformJump  = m->canPerform(UnitAction::Jump) ? 1 : 0;
        // UNIT-DEFINITION-SHADOW-COMPARE-1: chassis stem + locomotion/damage facet.
        out[n].chassis[0] = '\0';
        const char* appear = ot ? ot->getAppearanceTypeName() : nullptr;
        if (appear) {
            strncpy(out[n].chassis, appear, sizeof(out[n].chassis) - 1);
            out[n].chassis[sizeof(out[n].chassis) - 1] = '\0';
        }
        out[n].locomotion   = static_cast<int>(prof.locomotion);
        out[n].damageSchema = static_cast<int>(prof.damageSchema);
        // LOADOUT-THREE-LAYER-1 (Phase 3): spawned variant identity (mech overrides
        // getVariantName/Id from its CSV variant block; base Mover -> "" / -1).
        out[n].variant[0] = '\0';
        const char* vname = m->getVariantName();
        if (vname) {
            strncpy(out[n].variant, vname, sizeof(out[n].variant) - 1);
            out[n].variant[sizeof(out[n].variant) - 1] = '\0';
        }
        out[n].variantId = m->getVariantId();
        ++n;
    }
    return n;
}

// WEAPON-SHADOW-COMPARE-1 (equipment arc, slice 5): fill the equipment witness
// from MasterComponent::masterList. READ-ONLY; no component-loading behavior
// change. ComponentFormString / WeaponRangeString are file-scope tables in
// cmponent.cpp with no header decl — reference them extern here (the collector
// is the only cross-TU consumer).
extern const char* ComponentFormString[];   // cmponent.cpp: form-index -> token
extern const char* WeaponRangeString[];      // cmponent.cpp: WEAPON_RANGE_* -> token

namespace {
// Mirror of the converter's TYPE_TO_KIND (equipment_extract.py): map an engine
// ComponentFormType to the converter's coarse EquipmentKind vocabulary. The
// converter keys on the CSV `Type` string (== ComponentFormString token), so the
// two classifications agree by construction — the shadow-compare's `kind` diff
// therefore catches a *converter* mis-map, not a spelling drift.
const char* kindTokenForForm(ComponentFormType form) {
    switch (form) {
        case COMPONENT_FORM_WEAPON:
        case COMPONENT_FORM_WEAPON_ENERGY:
        case COMPONENT_FORM_WEAPON_BALLISTIC:
        case COMPONENT_FORM_WEAPON_MISSILE: return "Weapon";
        case COMPONENT_FORM_AMMO:           return "Ammo";
        case COMPONENT_FORM_SENSOR:
        case COMPONENT_FORM_PROBE:          return "Sensor";
        case COMPONENT_FORM_ECM:
        case COMPONENT_FORM_JAMMER:         return "ECM";
        case COMPONENT_FORM_JUMPJET:        return "JumpJet";
        case COMPONENT_FORM_HEATSINK:       return "Heatsink";
        case COMPONENT_FORM_BULK:
        case COMPONENT_FORM_CASE:           return "Structure";
        case COMPONENT_FORM_COCKPIT:
        case COMPONENT_FORM_LIFESUPPORT:    return "Cockpit";
        case COMPONENT_FORM_ACTUATOR:
        case COMPONENT_FORM_GYROSCOPE:      return "Actuator";
        case COMPONENT_FORM_ENGINE:         return "Engine";
        default:                            return "Other";
    }
}

bool formIsWeapon(ComponentFormType form) {
    return form == COMPONENT_FORM_WEAPON
        || form == COMPONENT_FORM_WEAPON_ENERGY
        || form == COMPONENT_FORM_WEAPON_BALLISTIC
        || form == COMPONENT_FORM_WEAPON_MISSILE;
}

// WeaponTag category token mirroring the converter's _weapon_category(): the
// energy/ballistic/missile split comes from the specific weapon FORM (the CSV
// EnergyWeapon/BallisticWeapon/MissileWeapon tokens). A generic COMPONENT_FORM_
// WEAPON has no engine-side split -> "Any" (the converter's WT_ANY).
const char* weaponCategoryToken(ComponentFormType form) {
    switch (form) {
        case COMPONENT_FORM_WEAPON_ENERGY:    return "Energy";
        case COMPONENT_FORM_WEAPON_BALLISTIC: return "Ballistic";
        case COMPONENT_FORM_WEAPON_MISSILE:   return "Missile";
        case COMPONENT_FORM_WEAPON:           return "Any";
        default:                              return "";
    }
}

void copyToken(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}
} // namespace

extern "C" int mc2_component_collect_witness(ComponentWitnessRow* out, int maxRows) {
    if (!out || maxRows <= 0 || !MasterComponent::masterList)
        return 0;
    int n = 0;
    const long total = MasterComponent::numComponents;
    for (long i = 0; i < total && n < maxRows; ++i) {
        MasterComponent& mc = MasterComponent::masterList[i];
        // Skip the `undefined`/blank sentinel slots (masterID == -1), matching the
        // converter's dropped-row waivers so the two row sets align on live comps.
        if (mc.getMasterID() < 0)
            continue;
        const ComponentFormType form = mc.getForm();
        // A slot whose masterID is >= 0 but whose `form` never got assigned
        // (initEXCEL early-returns -1 on an unknown Type token WITHOUT resetting
        // masterID) carries a garbage form. `form` is the raw index into the
        // ComponentFormString table, so an out-of-range value would read OOB.
        // Skip such slots — they mirror the converter's dropped unconvertible
        // rows and are not live components.
        if (form < 0 || form >= NUM_COMPONENT_FORMS)
            continue;
        ComponentWitnessRow& r = out[n];
        r.masterId = mc.getMasterID();
        copyToken(r.form, sizeof(r.form),
                  ComponentFormString[static_cast<long>(form)]);
        copyToken(r.kind, sizeof(r.kind), kindTokenForForm(form));
        r.critSlots = static_cast<int>(mc.getSize());
        r.combatValue = mc.getCV();
        const bool weapon = formIsWeapon(form);
        r.isWeapon = weapon ? 1 : 0;
        if (weapon) {
            copyToken(r.category, sizeof(r.category), weaponCategoryToken(form));
            r.damage  = mc.getWeaponDamage();
            r.heat    = mc.getWeaponHeat();
            r.recycle = mc.getWeaponRecycleTime();
            r.ammoMasterId = static_cast<int>(mc.getWeaponAmmoMasterId());
            const long rng = mc.getWeaponRange();
            const char* rb = "";
            if (rng == WEAPON_RANGE_SHORT || rng == WEAPON_RANGE_MEDIUM
                || rng == WEAPON_RANGE_LONG)
                rb = WeaponRangeString[rng];
            copyToken(r.rangeBucket, sizeof(r.rangeBucket), rb);
        } else {
            r.category[0] = '\0';
            r.rangeBucket[0] = '\0';
            r.damage = r.heat = r.recycle = 0.0f;
            r.ammoMasterId = 0;
        }
        ++n;
    }
    return n;
}
