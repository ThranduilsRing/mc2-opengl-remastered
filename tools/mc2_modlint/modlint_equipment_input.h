#ifndef MC2_MODLINT_EQUIPMENT_INPUT_H
#define MC2_MODLINT_EQUIPMENT_INPUT_H
// WEAPON-MODLINT-1 (equipment arc, slice 7). Facts-JSON -> EquipmentValidationInputs.
//
// The modder feeds modlint a small JSON describing an equipment component's
// already-extracted facts (the equipment converter did the impure compbas.csv
// extraction; modlint does the pure coherence check by calling the SAME
// validateEquipmentDefinition the engine will use). This header owns ONLY the
// parse (JSON text -> the plain POD EquipmentValidationInputs). It drags no
// engine/GL and no file IO (the caller reads the bytes); it reuses the tiny
// hand-rolled JSON reader from modlint_input.h (the UNIT modlint), so the two
// linters share one parser. Doctest-covered in isolation.
//
// EQUIPMENT FACTS-JSON schema (all fields optional; defaults = coherent stock weapon):
// {
//   "equip_id": "core:ac20",              // label only (printed, not validated)
//   "kind": "Weapon",                      // Weapon|Ammo|Sensor|ECM|JumpJet|...
//   "has_weapon": true,                    // WeaponFacet present (defaults from kind)
//   "fire_executor": "projectile_ballistic", // "" -> EQV_WEAPON_NO_FIRE_EXECUTOR
//   "category": "Ballistic",               // Energy|Ballistic|Missile|AntiPersonnel|AMS
//   "category_authored": true,             // false -> EQV_WEAPON_NO_CATEGORY
//   "damage": 20.0,
//   "range_short": 90.0, "range_med": 180.0, "range_long": 270.0,
//   "ammo_per_shot": 1,                    // >0 with empty ammo_category -> EQV_AMMO_CATEGORY_MISSING
//   "ammo_category": "core:ammo_ac20",
//   "has_ammo": false, "ammo_item_category": "",   // AMMO-kind item's own category
//   "claimed_events": ["OnFire","OnImpact"],       // declared-but-empty -> EQV_PRESENTATION_ASSET_MISSING
//   "presented_events": ["OnFire"],                // which claimed events DO map an asset
//   "hardpoints": [ {"accepts":["Energy"]} ]       // hardpoint context (optional)
// }
//
// It also accepts the equipment converter's --json roster report (a top-level
// "components" array of to_report_dict() entries). See parseEquipmentRoster().
#include <string>
#include <vector>

#include "modlint_input.h"            // MlJson + parseModlintJson + weaponTagFromToken
#include "equipment_validator.h"      // EquipmentValidationInputs and the validator
#include "equipment_definition.h"     // EquipmentDefinition, EquipmentKind, setWeaponFacet
#include "weapon_definition.h"        // WeaponFacet, WeaponEvent, makeWeaponId/Executor

// --- token -> enum helpers (spellings match the converter + engine schema) ----

inline EquipmentKind equipmentKindFromToken(const std::string& t) {
    if (t == "Weapon")    return EQ_WEAPON;
    if (t == "Ammo")      return EQ_AMMO;
    if (t == "Sensor")    return EQ_SENSOR;
    if (t == "ECM")       return EQ_ECM;
    if (t == "JumpJet")   return EQ_JUMPJET;
    if (t == "Heatsink")  return EQ_HEATSINK;
    if (t == "Structure") return EQ_STRUCTURE;
    if (t == "Cockpit")   return EQ_COCKPIT;
    if (t == "Actuator")  return EQ_ACTUATOR;
    if (t == "Engine")    return EQ_ENGINE;
    return EQ_OTHER;
}

// Converter emits WeaponTag as "Energy"/"Ballistic"/"Missile"/"Any"; map "Any"
// (a converter-neutral tag with no single hardpoint class) onto WTAG_ENERGY, the
// schema default. All others go through weaponTagFromToken (modlint_input.h).
inline WeaponTag weaponCategoryFromToken(const std::string& t) {
    if (t == "Any" || t.empty()) return WTAG_ENERGY;
    return weaponTagFromToken(t);
}

inline WeaponEvent weaponEventFromToken(const std::string& t, bool* known = nullptr) {
    if (known) *known = true;
    if (t == "OnFire")            return WeaponEvent::OnFire;
    if (t == "OnProjectileSpawn") return WeaponEvent::OnProjectileSpawn;
    if (t == "OnMuzzleFlash")     return WeaponEvent::OnMuzzleFlash;
    if (t == "OnBeamStart")       return WeaponEvent::OnBeamStart;
    if (t == "OnBeamEnd")         return WeaponEvent::OnBeamEnd;
    if (t == "OnImpact")          return WeaponEvent::OnImpact;
    if (t == "OnMiss")            return WeaponEvent::OnMiss;
    if (t == "OnJam")             return WeaponEvent::OnJam;
    if (t == "OnAmmoDepleted")    return WeaponEvent::OnAmmoDepleted;
    if (t == "OnOverheat")        return WeaponEvent::OnOverheat;
    if (known) *known = false;
    return WeaponEvent::OnFire;
}

// --- facts JSON -> EquipmentValidationInputs -------------------------------
// Builds the plain POD inputs the authoritative validateEquipmentDefinition
// consumes. Missing fields default to the coherent stock state (validator returns
// nothing). The (event, asset) presentation model: `claimed_events` are the events
// the def CLAIMS to present; any claimed event NOT listed in `presented_events`
// resolves to an empty asset cell -> EQV_PRESENTATION_ASSET_MISSING (a declared-
// but-empty cue). This lets the small facts-JSON exercise the presentation check
// without serializing the whole event x asset-kind grid.
inline EquipmentValidationInputs
equipmentFactsFromJson(const MlJson& root, std::string* label = nullptr) {
    EquipmentValidationInputs in;
    EquipmentDefinition& e = in.def;

    std::string idStr = "core:component";
    if (const MlJson* id = root.find("equip_id")) idStr = id->asStr("core:component");
    e = makeEquipment(idStr.c_str(), EQ_OTHER);
    if (label) *label = idStr;

    // Kind (defaults to Weapon-implied when has_weapon set; else Other).
    EquipmentKind kind = EQ_OTHER;
    bool kindGiven = false;
    if (const MlJson* k = root.find("kind")) { kind = equipmentKindFromToken(k->asStr("Other")); kindGiven = true; }

    // Weapon facet presence: explicit has_weapon, else implied by kind==Weapon.
    bool hasWeapon = kindGiven ? (kind == EQ_WEAPON) : false;
    if (const MlJson* hw = root.find("has_weapon")) hasWeapon = hw->asBool(hasWeapon);
    // If any weapon-only field is present without an explicit kind, imply a weapon.
    if (!kindGiven && (root.find("fire_executor") || root.find("category") ||
                       root.find("damage") || root.find("range_short"))) {
        hasWeapon = true;
        if (!root.find("has_weapon")) { /* leave hasWeapon true */ }
    }

    if (hasWeapon) {
        WeaponFacet w;
        w.weaponId = makeWeaponId(idStr.c_str());
        w.category = weaponCategoryFromToken(
            root.find("category") ? root.find("category")->asStr("Energy") : std::string("Energy"));
        // Fire executor: empty string -> unset -> EQV_WEAPON_NO_FIRE_EXECUTOR.
        std::string fx = root.find("fire_executor")
                             ? root.find("fire_executor")->asStr("beam_direct")
                             : std::string("beam_direct");
        if (!fx.empty()) w.fireExecutor = makeFireExecutorId(fx.c_str());

        if (const MlJson* d = root.find("damage"))      w.combat.damage     = (float)d->n;
        if (const MlJson* r = root.find("range_short")) w.combat.rangeShort = (float)r->n;
        if (const MlJson* r = root.find("range_med"))   w.combat.rangeMed   = (float)r->n;
        if (const MlJson* r = root.find("range_long"))  w.combat.rangeLong  = (float)r->n;

        if (const MlJson* a = root.find("ammo_per_shot")) w.ammo.ammoPerShot = (int)a->n;
        if (const MlJson* a = root.find("ammo_category"))
            w.ammo.ammoCategory = WeaponStringId(a->asStr().c_str());

        // Map every presented event to a token asset so its cell is non-empty;
        // claimed-but-not-presented events stay empty -> presentation finding.
        if (const MlJson* pe = root.find("presented_events"))
            for (const MlJson& ev : pe->arr) {
                bool known = true;
                WeaponEvent we = weaponEventFromToken(ev.asStr(), &known);
                if (known)
                    w.presentation.setEventAsset(we, WeaponAssetKind::Vfx, "core:present");
            }

        setWeaponFacet(e, w);   // sets hasWeapon + kind = EQ_WEAPON
    }

    // Explicit kind wins for the kind/facet-mismatch check (e.g. kind=Ammo but a
    // weapon facet authored, or kind=Weapon with no facet).
    if (kindGiven) e.kind = kind;

    // Category-authored assertion (schema can't distinguish default from authored).
    if (const MlJson* ca = root.find("category_authored")) in.categoryAuthored = ca->asBool(true);

    // AMMO-kind item's own named category.
    if (const MlJson* ha = root.find("has_ammo")) e.hasAmmo = ha->asBool(false);
    if (const MlJson* ac = root.find("ammo_item_category")) {
        e.ammo.ammoCategory = WeaponStringId(ac->asStr().c_str());
        if (!root.find("has_ammo")) e.hasAmmo = true;
    }

    // Claimed presentation events (declared-but-empty check).
    if (const MlJson* ce = root.find("claimed_events"))
        for (const MlJson& ev : ce->arr) {
            bool known = true;
            WeaponEvent we = weaponEventFromToken(ev.asStr(), &known);
            if (known) in.claimedEvents.push_back(ClaimedWeaponEvent(we));
        }

    // Optional hardpoint context.
    if (const MlJson* hps = root.find("hardpoints"))
        for (const MlJson& hp : hps->arr) {
            HardpointProfile prof;
            if (const MlJson* acc = hp.find("accepts"))
                for (const MlJson& a : acc->arr)
                    addWeaponTag(prof, weaponCategoryFromToken(a.asStr("Energy")));
            in.hardpointContext.push_back(prof);
        }

    return in;
}

// One roster component reduced to the facts modlint checks from the equipment
// converter's --json report (a "components" array of to_report_dict() entries).
struct RosterEquipmentFacts {
    std::string                label;
    EquipmentValidationInputs  inputs;
};

// Build EquipmentValidationInputs from ONE converter to_report_dict() entry.
// The converter emits: equip_id, name, kind, tags, legacy_master_id, and (for
// weapons) a "weapon" object {category, fire_executor, damage, heat, recycle,
// range_bucket, combat_value, ammo_category, ammo_master_id, crit_slots}. The
// stock converter emits ONE range bucket per weapon (not three thresholds), so
// range-ordering is trivially coherent; the range check never trips on stock.
inline EquipmentValidationInputs
equipmentFromConverterEntry(const MlJson& c, std::string* label = nullptr) {
    EquipmentValidationInputs in;
    EquipmentDefinition& e = in.def;

    std::string idStr = c.find("equip_id") ? c.find("equip_id")->asStr("core:component")
                                           : std::string("core:component");
    e = makeEquipment(idStr.c_str(), EQ_OTHER);
    if (label) *label = idStr;

    EquipmentKind kind = c.find("kind") ? equipmentKindFromToken(c.find("kind")->asStr("Other"))
                                        : EQ_OTHER;

    if (const MlJson* wj = c.find("weapon")) {
        WeaponFacet w;
        w.weaponId = makeWeaponId(idStr.c_str());
        w.category = weaponCategoryFromToken(
            wj->find("category") ? wj->find("category")->asStr("Energy") : std::string("Energy"));
        std::string fx = wj->find("fire_executor") ? wj->find("fire_executor")->asStr() : std::string();
        if (!fx.empty()) w.fireExecutor = makeFireExecutorId(fx.c_str());
        if (const MlJson* d = wj->find("damage")) w.combat.damage = (float)d->n;
        // ammo_master_id > 0 in stock means the weapon consumes ammo; treat as 1/shot.
        int ammoMaster = wj->find("ammo_master_id") ? (int)wj->find("ammo_master_id")->n : 0;
        std::string ammoCat = wj->find("ammo_category") ? wj->find("ammo_category")->asStr() : std::string();
        w.ammo.ammoPerShot = (ammoMaster > 0) ? 1 : 0;
        w.ammo.ammoCategory = WeaponStringId(ammoCat.c_str());
        setWeaponFacet(e, w);
    }
    e.kind = kind;   // explicit converter kind (drives kind/facet mismatch check)

    return in;
}

// Parse the equipment converter's --json report into per-component facts. Returns
// empty when there is no "components" array.
inline std::vector<RosterEquipmentFacts> parseEquipmentRoster(const MlJson& root) {
    std::vector<RosterEquipmentFacts> out;
    const MlJson* comps = root.find("components");
    if (!comps || comps->type != MlJson::Arr) return out;
    for (const MlJson& c : comps->arr) {
        RosterEquipmentFacts rf;
        rf.inputs = equipmentFromConverterEntry(c, &rf.label);
        out.push_back(rf);
    }
    return out;
}

#endif // MC2_MODLINT_EQUIPMENT_INPUT_H
