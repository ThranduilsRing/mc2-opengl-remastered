#ifndef EQUIPMENT_VALIDATOR_H
#define EQUIPMENT_VALIDATOR_H
// WEAPON-VALIDATOR-1 (equipment arc, slice 6).
//
// Pure CONSTRUCTION-RULES validator for EquipmentDefinition / WeaponFacet. Mirrors
// the unit arc's unit_profile_validator.h exactly: it only *describes* incoherence
// in an equipment definition and yields modder-readable findings keyed by stable
// reason codes -- it NEVER rejects a component (report-only). The future engine
// validate hook logs findings; the future WEAPON-MODLINT-1 CLI (next slice) wraps
// THIS function as a command-line linter over compbas-emitted defs.
//
// Pure, header-only (plain enums / bools / the pure schema structs), so it compiles
// into the offline `mc2_tests` target and drags NO engine/GL/IO code -- exactly
// like equipment_definition.h / weapon_definition.h / hardpoint.h. No gate: nothing
// engine-side reads it yet, and it changes no engine behavior.
//
// It reuses the schema vocabularies already established by the equipment arc:
//   * equipment_definition.h  -- EquipmentDefinition, EquipmentKind, facet slots
//   * weapon_definition.h     -- WeaponFacet, WeaponEvent/AssetKind grid, fireExecutor,
//                                WeaponCombatStats, WeaponAmmoFacet, weaponFitsHardpoint
//   * hardpoint.h             -- HardpointProfile, weaponFitsHardpoint / hardpointAcceptsWeapon
//
// REASON-CODE DISCIPLINE (same as UnitProfileReasonCode): a NEW, independent
// `EQV_` vocabulary (Equipment-Validator). Order is NOT significance; each code is
// a distinct coherence rule. Future equipment-arc slices ADD codes here and grow
// the vocabulary -- they must NOT renumber existing ones, because logs / mod
// tooling key on the value. A doctest locks EQV__COUNT.

#include <vector>
#include "equipment_definition.h"   // EquipmentDefinition, EquipmentKind, facet slots
#include "weapon_definition.h"      // WeaponFacet, WeaponEvent/AssetKind, combat/ammo
#include "hardpoint.h"              // HardpointProfile, weaponFitsHardpoint

// -------------------------------------------------------------------------
// REASON-CODE VOCABULARY. `EQV_` = Equipment-Validator. Stable, modder-facing.
// APPEND-ONLY (never renumber): logs / mod tooling key on the value.
// -------------------------------------------------------------------------
enum EquipmentReasonCode {
    // --- LIVE (WEAPON-VALIDATOR-1, equipment arc slice 6) ---
    // A weapon-kind equipment carries a WeaponFacet with NO fireExecutor bound
    // (fireExecutor.id == 0). A weapon needs an executor to fire; without one it
    // is inert. WARN, not reject (a converter default may fill it later).
    EQV_WEAPON_NO_FIRE_EXECUTOR = 0,

    // A weapon-kind equipment carries a WeaponFacet with no category set in a way
    // the hardpoint spine can use. The stock converter always emits a WeaponTag;
    // a def whose category never appears in ANY declared hardpoint accept-set is
    // caught by the hardpoint-context check below. This code fires for the weapon
    // that DECLARES it uses a presentation event but leaves that (event,kind) cell
    // empty -- a declared-but-empty presentation asset (mirrors UPV_UI_ASSET_MISSING
    // and the unit anim/UI declared-but-empty pattern).
    EQV_PRESENTATION_ASSET_MISSING,

    // A weapon-kind equipment whose category tag is not a modeled weapon class /
    // the equipment is a weapon but no meaningful WeaponTag was authored. In this
    // schema WeaponTag is always one of the modeled values (defaults WTAG_ENERGY),
    // so "no category" is expressed by the caller asserting the def carries a
    // weapon facet with an out-of-range / unauthored category -- reported here so
    // the vocabulary has the intended slot for the unit-spine join.
    EQV_WEAPON_NO_CATEGORY,

    // An ammo-consuming weapon (WeaponAmmoFacet::ammoPerShot > 0) whose
    // ammoCategory string is EMPTY -- it fires ammo but names no ammo category, so
    // nothing can supply it. (Or an AMMO-kind equipment whose AmmoFacet names no
    // category.)
    EQV_AMMO_CATEGORY_MISSING,

    // A weapon whose range buckets are incoherent: negative range/damage, or the
    // short/med/long ordering is violated (short > med, or med > long). Ranges must
    // be non-negative and monotonically non-decreasing; damage non-negative.
    EQV_WEAPON_RANGE_INVALID,

    // EquipmentKind vs facet mismatch: kind == EQ_WEAPON but no WeaponFacet present,
    // or a WeaponFacet IS present but kind != EQ_WEAPON. Optional cheap coherence.
    EQV_KIND_FACET_MISMATCH,

    // The weapon<->hardpoint legality check, reachable when a hardpoint CONTEXT is
    // supplied: the weapon's category is NOT accepted by the provided hardpoint
    // (weaponFitsHardpoint == false). REPORT-ONLY -- author-time enforcement is
    // deferred/blocked per the mech-bay constraint; here it is a finding only.
    EQV_WEAPON_HARDPOINT_REJECTED,

    EQV__COUNT
};

// A single coherence finding. `code` = stable machine key; `message` = a short
// modder-facing literal (the future glue/CLI prefixes it with the equipment id).
// Flat POD, no ownership -- mirrors UnitProfileFinding.
struct EquipmentFinding {
    EquipmentReasonCode code;
    const char*         message;   // static string literal; no ownership
};

// -------------------------------------------------------------------------
// Inputs. The def is the pure schema aggregate. The optional hardpoint context
// (a list of hardpoints the weapon is being mounted against) enables the
// weapon<->hardpoint legality check; empty = no hardpoint context, that check is
// skipped. The `usedEvents` list lets the caller state which WeaponEvents the def
// CLAIMS to use (e.g. from its fire executor's declared event set), so the
// presentation-asset check reports a declared-but-empty event cell rather than
// walking every possible event (most are legitimately unmapped).
// -------------------------------------------------------------------------

// One WeaponEvent the def CLAIMS to present, plus which asset kind is expected.
// The check reports EQV_PRESENTATION_ASSET_MISSING when the def maps NO asset for
// this event at all (eventHasAnyAsset == false) -- a declared-but-empty cue.
struct ClaimedWeaponEvent {
    WeaponEvent event;
    ClaimedWeaponEvent() : event(WeaponEvent::OnFire) {}
    explicit ClaimedWeaponEvent(WeaponEvent e) : event(e) {}
};

struct EquipmentValidationInputs {
    EquipmentDefinition             def;
    // Hardpoints the weapon is being mounted against (optional context). Empty ->
    // the weapon<->hardpoint legality check is skipped (no context to judge).
    std::vector<HardpointProfile>   hardpointContext;
    // WeaponEvents the def CLAIMS to present (optional). Empty -> the presentation
    // check is skipped (an unmapped event is not a defect unless it is claimed).
    std::vector<ClaimedWeaponEvent> claimedEvents;
    // Caller assertion that the weapon's category tag was actually AUTHORED (the
    // schema can't distinguish an authored WTAG_ENERGY from the default). Default
    // true (schema always has a value); set false to exercise EQV_WEAPON_NO_CATEGORY.
    bool                            categoryAuthored;

    EquipmentValidationInputs() : categoryAuthored(true) {}
};

// -------------------------------------------------------------------------
// THE validation entry. Pure; returns aggregated findings in a stable check order.
// A coherent stock-shaped weapon (category authored, sane ranges, an executor,
// ammo where needed, no claimed-but-empty events, category accepted by any supplied
// hardpoint) yields ZERO findings. Report-only -- never rejects.
// -------------------------------------------------------------------------
inline std::vector<EquipmentFinding>
validateEquipmentDefinition(const EquipmentValidationInputs& in) {
    std::vector<EquipmentFinding> findings;
    const EquipmentDefinition& e = in.def;

    // --- Kind vs facet coherence (optional, cheap). ---
    const bool kindSaysWeapon = (e.kind == EQ_WEAPON);
    if (kindSaysWeapon != e.hasWeapon) {
        findings.push_back(EquipmentFinding{
            EQV_KIND_FACET_MISMATCH,
            "equipment kind and WeaponFacet presence disagree"});
    }

    // Weapon-facet checks only apply when a weapon facet is actually present.
    if (e.hasWeapon) {
        const WeaponFacet& w = e.weapon;

        // --- 1. No fire executor bound. ---
        if (!w.fireExecutor.valid()) {
            findings.push_back(EquipmentFinding{
                EQV_WEAPON_NO_FIRE_EXECUTOR,
                "weapon has no fire executor bound (cannot fire)"});
        }

        // --- 2. No authored category (unit-spine join half missing). ---
        if (!in.categoryAuthored) {
            findings.push_back(EquipmentFinding{
                EQV_WEAPON_NO_CATEGORY,
                "weapon has no authored mount-class category"});
        }

        // --- 3. Ammo-using weapon names no ammo category. ---
        if (w.ammo.ammoPerShot > 0 && w.ammo.ammoCategory.empty()) {
            findings.push_back(EquipmentFinding{
                EQV_AMMO_CATEGORY_MISSING,
                "ammo-consuming weapon names no ammo category"});
        }

        // --- 4. Range / damage coherence. ---
        {
            const WeaponCombatStats& c = w.combat;
            const bool negative = c.damage < 0.f || c.rangeShort < 0.f ||
                                  c.rangeMed < 0.f || c.rangeLong < 0.f;
            const bool disordered = c.rangeShort > c.rangeMed ||
                                    c.rangeMed   > c.rangeLong;
            if (negative || disordered) {
                findings.push_back(EquipmentFinding{
                    EQV_WEAPON_RANGE_INVALID,
                    "weapon ranges/damage invalid (negative or short>med>long)"});
            }
        }

        // --- 5. Claimed-but-empty presentation events. ---
        for (const ClaimedWeaponEvent& ce : in.claimedEvents) {
            if (!eventHasAnyAsset(w.presentation, ce.event)) {
                findings.push_back(EquipmentFinding{
                    EQV_PRESENTATION_ASSET_MISSING,
                    "claimed weapon event maps to no presentation asset"});
            }
        }

        // --- 6. Weapon<->hardpoint legality (only with a hardpoint context). ---
        for (const HardpointProfile& hp : in.hardpointContext) {
            if (!weaponFitsHardpoint(w, hp)) {
                findings.push_back(EquipmentFinding{
                    EQV_WEAPON_HARDPOINT_REJECTED,
                    "weapon category not accepted by a supplied hardpoint"});
            }
        }
    }

    // --- AMMO-kind item with no named category (parallel to the weapon case). ---
    if (e.hasAmmo && e.ammo.ammoCategory.empty()) {
        findings.push_back(EquipmentFinding{
            EQV_AMMO_CATEGORY_MISSING,
            "ammo item names no ammo category"});
    }

    return findings;
}

#endif // EQUIPMENT_VALIDATOR_H
