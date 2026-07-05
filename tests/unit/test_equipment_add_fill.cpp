// EQUIP-ADD-FILL-1 (equipment arc, ADD-ladder S2) -- offline tests for the PURE
// ADD logic: the MC2_EQUIPMENT_ADD gate parse, the namespace-aware def path,
// the fill decision matrix (explicit-ordinal-only / first-wins / never-clobber-
// populated / 255-sentinel), the kind+category -> ComponentFormType mapping,
// the AllowedLocations -> 8-crit-column mapping, the dual-consumer CSV line
// builder (column-order lock against BOTH parsers' shared layout, verified
// against stock compbas row 156), and the EQV_ADD_* validator mirrors.
// No engine, no MasterComponent -- the impure fill pass lives in
// equipment_registry_engine.cpp (not built here).

#include "doctest.h"
#include "equipment_registry.h"
#include "equipment_validator.h"
#include "equipment_findings_text.h"

#include <string>
#include <vector>

namespace {
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> cols;
    std::string cur;
    for (char c : line) {
        if (c == ',') { cols.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    cols.push_back(cur);
    return cols;
}
bool hasCode(const std::vector<EquipmentFinding>& fs, EquipmentReasonCode c) {
    for (const auto& f : fs) if (f.code == c) return true;
    return false;
}
} // namespace

TEST_CASE("MC2_EQUIPMENT_ADD gate parse: unset/0 off, else on") {
    CHECK(!mc2_equipment_add_parse_enabled(nullptr));
    CHECK(!mc2_equipment_add_parse_enabled(""));
    CHECK(!mc2_equipment_add_parse_enabled("0"));
    CHECK(mc2_equipment_add_parse_enabled("1"));
    CHECK(mc2_equipment_add_parse_enabled("true"));
    CHECK(mc2_equipment_add_parse_enabled("01"));   // same rule as MC2_UNIT_PROFILE_DATA
}

TEST_CASE("equipmentDefPathForQualifiedId is namespace-aware") {
    CHECK(equipmentDefPathForQualifiedId("sample:hot_medium_laser") ==
          "data/defs/equipment/sample/hot_medium_laser/equipmentdef.fit");
    CHECK(equipmentDefPathForQualifiedId("core:laser") ==
          "data/defs/equipment/core/laser/equipmentdef.fit");
    // defensive: no namespace -> core
    CHECK(equipmentDefPathForQualifiedId("laser") ==
          "data/defs/equipment/core/laser/equipmentdef.fit");
}

TEST_CASE("equipmentAddFillDecide: explicit-only, free-only, first-wins, no-255") {
    const long N = 255; // stock numComponents
    // happy path: explicit ordinal, in range, free, unclaimed
    CHECK(equipmentAddFillDecide(true, 40, N, false, false, false) == EQADD_FILL);
    // no explicit LegacyMasterId -> never auto-assign
    CHECK(equipmentAddFillDecide(false, -1, N, false, false, false) == EQADD_SKIP_NO_ORDINAL);
    // range: negative / >= numComponents / the 0xff sentinel
    CHECK(equipmentAddFillDecide(true, -1, N, false, false, false) == EQADD_SKIP_ORDINAL_RANGE);
    CHECK(equipmentAddFillDecide(true, 255, N, false, false, false) == EQADD_SKIP_ORDINAL_RANGE);
    CHECK(equipmentAddFillDecide(true, 300, N, false, false, false) == EQADD_SKIP_ORDINAL_RANGE);
    // even if numComponents were ever >255, 255 stays the sentinel
    CHECK(equipmentAddFillDecide(true, 255, 300, false, false, false) == EQADD_SKIP_ORDINAL_RANGE);
    // populated stock row: conflict finding, unless it is the row's OWN core def
    CHECK(equipmentAddFillDecide(true, 156, N, true, false, false) == EQADD_SKIP_POPULATED);
    CHECK(equipmentAddFillDecide(true, 156, N, true, true, false) == EQADD_SKIP_SELF_OVERRIDE);
    // duplicate claim: first-wins
    CHECK(equipmentAddFillDecide(true, 40, N, false, false, true) == EQADD_SKIP_DUPLICATE);
}

TEST_CASE("equipmentAddKindToFormOrdinal maps kind+category to legacy form") {
    CHECK(equipmentAddKindToFormOrdinal("Weapon", "Energy") == CFORM_WEAPON_ENERGY);
    CHECK(equipmentAddKindToFormOrdinal("Weapon", "Ballistic") == CFORM_WEAPON_BALLISTIC);
    CHECK(equipmentAddKindToFormOrdinal("Weapon", "Missile") == CFORM_WEAPON_MISSILE);
    CHECK(equipmentAddKindToFormOrdinal("Weapon", "") == CFORM_WEAPON_ENERGY);
    CHECK(equipmentAddKindToFormOrdinal("weapon", "energy") == CFORM_WEAPON_ENERGY); // case-insensitive
    CHECK(equipmentAddKindToFormOrdinal("Ammo", nullptr) == CFORM_AMMO);
    CHECK(equipmentAddKindToFormOrdinal("Sensor", nullptr) == CFORM_SENSOR);
    CHECK(equipmentAddKindToFormOrdinal("ECM", nullptr) == CFORM_ECM);
    CHECK(equipmentAddKindToFormOrdinal("JumpJet", nullptr) == CFORM_JUMPJET);
    CHECK(equipmentAddKindToFormOrdinal("Heatsink", nullptr) == CFORM_HEATSINK);
    CHECK(equipmentAddKindToFormOrdinal("bogus", nullptr) == CFORM_SIMPLE);
}

TEST_CASE("equipmentAddAllowedLocationsToCrit maps tokens to compbas column order") {
    bool c[8];
    // explicit list
    equipmentAddAllowedLocationsToCrit("CenterTorso,LeftArm", c);
    CHECK(!c[0]); CHECK(c[1]); CHECK(!c[2]); CHECK(!c[3]);
    CHECK(c[4]); CHECK(!c[5]); CHECK(!c[6]); CHECK(!c[7]);
    // spaces + case tolerated
    equipmentAddAllowedLocationsToCrit(" head , leftleg ", c);
    CHECK(c[0]); CHECK(c[6]);
    CHECK(!c[1]);
    // empty -> permissive torso+arms default (head/legs no)
    equipmentAddAllowedLocationsToCrit("", c);
    CHECK(!c[0]); CHECK(c[1]); CHECK(c[2]); CHECK(c[3]);
    CHECK(c[4]); CHECK(c[5]); CHECK(!c[6]); CHECK(!c[7]);
}

TEST_CASE("buildEquipmentAddCsvLine: dual-consumer column layout (row-156 shape)") {
    EquipmentAddRowFields f;
    f.ordinal = 40; f.formOrdinal = CFORM_WEAPON_ENERGY;
    f.displayName = "Hot, Medium Laser";   // comma must be stripped
    f.critSlots = 1; f.recycle = 4.2f; f.heat = 4.5f; f.tonnage = 2.0f;
    f.damage = 3.2f; f.combatValue = 120.0f; f.cost = 2100;
    f.rangeOrdinal = 0;   // short
    // default crit[] = torso+arms
    f.ammoMasterId = 0; f.specialEffect = 12;
    f.iconFile = "MCL_MC_Laser.tga";       // picture defaults to icon stem
    f.stringId = 32156; f.helpStringId = 34040; f.iconX = 1; f.iconY = 1;

    std::vector<std::string> cols = splitCsv(buildEquipmentAddCsvLine(f));
    REQUIRE(cols.size() == 29);            // stock compbas row width
    CHECK(cols[0] == "40");                // id (both parsers)
    CHECK(cols[1] == "EnergyWeapon");      // form (initEXCEL) / Type (logistics)
    CHECK(cols[2] == "Hot Medium Laser");  // name column, comma stripped
    CHECK(cols[3] == "1");                 // size / (ignored crit col)
    CHECK(cols[4] == "4.20");              // recycle
    CHECK(cols[5] == "4.50");              // heat
    CHECK(cols[6] == "2.00");              // tonnage / weight
    CHECK(cols[7] == "3.20");              // damage
    CHECK(cols[8] == "120.00");            // CV / (ignored BR)
    CHECK(cols[9] == "2100");              // resourcePoints / cost
    CHECK(cols[10] == "short");            // range bucket
    // crit columns 11..18 in Head,CT,LT,RT,LA,RA,LL,RL order (default)
    CHECK(cols[11] == "No");  CHECK(cols[12] == "Yes"); CHECK(cols[16] == "Yes");
    CHECK(cols[17] == "No");  CHECK(cols[18] == "No");
    CHECK(cols[19] == "0");                // ammoType token (no ammo)
    CHECK(cols[20] == "0");                // flags
    CHECK(cols[21] == "12");               // specialEffect
    CHECK(cols[22] == "0");                // ammoMasterId == logistics Ammo column
    CHECK(cols[23] == "MCL_MC_Laser.tga"); // icon (MANDATORY for logistics)
    CHECK(cols[24] == "MCL_MC_Laser");     // picture (defaulted to icon stem)
    CHECK(cols[25] == "32156");            // stringID
    CHECK(cols[26] == "34040");            // helpStringID
    CHECK(cols[27] == "1"); CHECK(cols[28] == "1");   // iconX/iconY

    // ammo-linked variant: ammoType token flips to '1', Ammo column carries the ordinal
    f.ammoMasterId = 199;
    cols = splitCsv(buildEquipmentAddCsvLine(f));
    CHECK(cols[19] == "1");
    CHECK(cols[22] == "199");
}

TEST_CASE("validator EQV_ADD_* mirrors the engine fill rules") {
    std::vector<long> populated = { 145, 146, 156, 199 };

    // clean ADD def on a free ordinal -> no ADD findings
    EquipmentValidationInputs ok;
    ok.addFill = true;
    ok.def.legacyMasterId = 40;
    ok.populatedLegacyOrdinals = populated;
    {
        auto fs = validateEquipmentDefinition(ok);
        CHECK(!hasCode(fs, EQV_ADD_ORDINAL_UNAVAILABLE));
        CHECK(!hasCode(fs, EQV_ADD_FIELDS_INCOMPLETE));
        CHECK(!hasCode(fs, EQV_ADD_AMMO_UNRESOLVED));
    }

    // missing ordinal / out of range / populated -> EQV_ADD_ORDINAL_UNAVAILABLE
    {
        EquipmentValidationInputs in = ok; in.def.legacyMasterId = -1;
        CHECK(hasCode(validateEquipmentDefinition(in), EQV_ADD_ORDINAL_UNAVAILABLE));
        in.def.legacyMasterId = 255;
        CHECK(hasCode(validateEquipmentDefinition(in), EQV_ADD_ORDINAL_UNAVAILABLE));
        in.def.legacyMasterId = 156;   // populated stock row
        CHECK(hasCode(validateEquipmentDefinition(in), EQV_ADD_ORDINAL_UNAVAILABLE));
        // no populated context -> only the range half runs
        in.populatedLegacyOrdinals.clear();
        CHECK(!hasCode(validateEquipmentDefinition(in), EQV_ADD_ORDINAL_UNAVAILABLE));
    }

    // missing DisplayName / Icon -> EQV_ADD_FIELDS_INCOMPLETE
    {
        EquipmentValidationInputs in = ok; in.hasIcon = false;
        CHECK(hasCode(validateEquipmentDefinition(in), EQV_ADD_FIELDS_INCOMPLETE));
        in.hasIcon = true; in.hasDisplayName = false;
        CHECK(hasCode(validateEquipmentDefinition(in), EQV_ADD_FIELDS_INCOMPLETE));
    }

    // dangling ammo link -> EQV_ADD_AMMO_UNRESOLVED (context-gated)
    {
        EquipmentValidationInputs in = ok; in.ammoMasterId = 42;
        CHECK(hasCode(validateEquipmentDefinition(in), EQV_ADD_AMMO_UNRESOLVED));
        in.ammoMasterId = 199;   // populated ammo row
        CHECK(!hasCode(validateEquipmentDefinition(in), EQV_ADD_AMMO_UNRESOLVED));
        in.ammoMasterId = 42; in.populatedLegacyOrdinals.clear();
        CHECK(!hasCode(validateEquipmentDefinition(in), EQV_ADD_AMMO_UNRESOLVED));
    }

    // add_fill + Enabled on a FREE ordinal must NOT trip EQV_ENABLE_UNRESOLVED
    // (the fill populates the row before the enable wire runs)
    {
        EquipmentValidationInputs in = ok; in.enableDeclared = true;
        CHECK(!hasCode(validateEquipmentDefinition(in), EQV_ENABLE_UNRESOLVED));
        // ...but a NON-add enable on a free ordinal still trips it
        in.addFill = false;
        CHECK(hasCode(validateEquipmentDefinition(in), EQV_ENABLE_UNRESOLVED));
    }

    // stable tokens for the new codes
    CHECK(std::string(equipmentReasonToken(EQV_ADD_ORDINAL_UNAVAILABLE)) == "EQV_ADD_ORDINAL_UNAVAILABLE");
    CHECK(std::string(equipmentReasonToken(EQV_ADD_FIELDS_INCOMPLETE)) == "EQV_ADD_FIELDS_INCOMPLETE");
    CHECK(std::string(equipmentReasonToken(EQV_ADD_AMMO_UNRESOLVED)) == "EQV_ADD_AMMO_UNRESOLVED");
}
