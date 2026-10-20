// Offline tests for the PURE
// ADD logic: the MC2_EQUIPMENT_ADD gate parse, the namespace-aware def path,
// the fill decision matrix (explicit-ordinal-only / first-wins / never-clobber-
// populated / 255-sentinel), the kind+category -> ComponentFormType mapping,
// the AllowedLocations -> 8-crit-column mapping, the dual-consumer CSV line
// builder (column-order lock against BOTH parsers' shared layout, verified
// against stock compbas row 156), and the EQV_ADD_* validator mirrors.
// No engine, no MasterComponent -- the impure fill pass lives in
// equipment_registry_engine.cpp (not built here).

#include "doctest.h"
#include <utility>
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

TEST_CASE("mc2ComponentRowPopulated: the string-table-name divergence class") {
    // A truly populated row: masterID >= 0 and a VALID form.
    CHECK(mc2ComponentRowPopulated(156, std::to_underlying(CFORM_WEAPON_ENERGY)));
    CHECK(mc2ComponentRowPopulated(0, std::to_underlying(CFORM_SIMPLE)));
    CHECK(mc2ComponentRowPopulated(254, std::to_underlying(CFORM_BULK)));   // last valid form ordinal (19)

    // The sentinel state loadMasterList now guarantees for undefined rows.
    CHECK(!mc2ComponentRowPopulated(-1, std::to_underlying(CFORM_WEAPON_ENERGY)));

// The observed live-witness divergence: initEXCEL bails on "undefined"
    // WITHOUT resetting masterID, leaving masterID==ordinal + a string-table
    // name + a GARBAGE form. A name-based test called this row populated (all
    // ADD defs refused EQADD_SKIP_POPULATED); the form-validity test does not.
    CHECK(!mc2ComponentRowPopulated(40, 999));               // garbage form
    CHECK(!mc2ComponentRowPopulated(40, -1));                // garbage form
    CHECK(!mc2ComponentRowPopulated(40, kComponentFormOrdinalCount)); // one-past-end

    // Conversely, a populated row whose string-table entry is MISSING (empty
    // name) must still read as populated -- a name test would mark it free
    // (clobberable by an ADD def). The helper takes no name at all.
    CHECK(mc2ComponentRowPopulated(150, std::to_underlying(CFORM_AMMO)));

    CHECK(kComponentFormOrdinalCount == 20);   // == NUM_COMPONENT_FORMS lock
}

TEST_CASE("equipmentAddKindToFormOrdinal maps kind+category to legacy form") {
    CHECK(equipmentAddKindToFormOrdinal("Weapon", "Energy") == std::to_underlying(CFORM_WEAPON_ENERGY));
    CHECK(equipmentAddKindToFormOrdinal("Weapon", "Ballistic") == std::to_underlying(CFORM_WEAPON_BALLISTIC));
    CHECK(equipmentAddKindToFormOrdinal("Weapon", "Missile") == std::to_underlying(CFORM_WEAPON_MISSILE));
    CHECK(equipmentAddKindToFormOrdinal("Weapon", "") == std::to_underlying(CFORM_WEAPON_ENERGY));
    CHECK(equipmentAddKindToFormOrdinal("weapon", "energy") == std::to_underlying(CFORM_WEAPON_ENERGY)); // case-insensitive
    CHECK(equipmentAddKindToFormOrdinal("Ammo", nullptr) == std::to_underlying(CFORM_AMMO));
    CHECK(equipmentAddKindToFormOrdinal("Sensor", nullptr) == std::to_underlying(CFORM_SENSOR));
    CHECK(equipmentAddKindToFormOrdinal("ECM", nullptr) == std::to_underlying(CFORM_ECM));
    CHECK(equipmentAddKindToFormOrdinal("JumpJet", nullptr) == std::to_underlying(CFORM_JUMPJET));
    CHECK(equipmentAddKindToFormOrdinal("Heatsink", nullptr) == std::to_underlying(CFORM_HEATSINK));
    CHECK(equipmentAddKindToFormOrdinal("bogus", nullptr) == std::to_underlying(CFORM_SIMPLE));
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
    f.ordinal = 40; f.formOrdinal = std::to_underlying(CFORM_WEAPON_ENERGY);
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

// ---------------------------------------------------------------------------
// String-table id decode + ADD-row UI text resolver.
// ---------------------------------------------------------------------------
TEST_CASE("equip-add-ui-name: equipmentUiStringDecode block mapping") {
    long ord = -99;
    // the four component blocks, base + top-of-range (ordinal 254)
    CHECK(equipmentUiStringDecode(32000, &ord) == EQUISTR_NAME);   CHECK(ord == 0);
    CHECK(equipmentUiStringDecode(32254, &ord) == EQUISTR_NAME);   CHECK(ord == 254);
    CHECK(equipmentUiStringDecode(33040, &ord) == EQUISTR_ABBREV); CHECK(ord == 40);
    CHECK(equipmentUiStringDecode(34040, &ord) == EQUISTR_DESC);   CHECK(ord == 40);
    CHECK(equipmentUiStringDecode(46040, &ord) == EQUISTR_DESC);   CHECK(ord == 40);

    // 255 = 0xff sentinel -> NOT a component string (block width is 0..254)
    CHECK(equipmentUiStringDecode(32255, &ord) == EQUISTR_NONE);   CHECK(ord == -1);
    CHECK(equipmentUiStringDecode(46255, &ord) == EQUISTR_NONE);
    // outside every block
    CHECK(equipmentUiStringDecode(0, &ord) == EQUISTR_NONE);
    CHECK(equipmentUiStringDecode(31999, &ord) == EQUISTR_NONE);
    CHECK(equipmentUiStringDecode(35000, &ord) == EQUISTR_NONE);
    CHECK(equipmentUiStringDecode(45999, &ord) == EQUISTR_NONE);
    CHECK(equipmentUiStringDecode(47000, &ord) == EQUISTR_NONE);
    // null ordOut is allowed
    CHECK(equipmentUiStringDecode(33001, nullptr) == EQUISTR_ABBREV);
}

TEST_CASE("equip-add-ui-name: pure resolver over fill rows") {
    std::vector<EquipmentAddFillRow> rows;
    EquipmentAddFillRow r;
    r.ordinal = 40;
    r.qualifiedId = "sample:hot_medium_laser";
    r.displayName = "Hot Medium Laser";
    r.description = "Runs hot.";
    rows.push_back(r);
    EquipmentAddFillRow r2;               // no description -> name fallback
    r2.ordinal = 44;
    r2.displayName = "Plain Thing";
    rows.push_back(r2);

    // name + abbrev blocks -> DisplayName
    CHECK(std::string(equipmentAddUiTextForStringIdPure(rows, 32040)) == "Hot Medium Laser");
    CHECK(std::string(equipmentAddUiTextForStringIdPure(rows, 33040)) == "Hot Medium Laser");
    // both description blocks -> Description
    CHECK(std::string(equipmentAddUiTextForStringIdPure(rows, 34040)) == "Runs hot.");
    CHECK(std::string(equipmentAddUiTextForStringIdPure(rows, 46040)) == "Runs hot.");
    // description absent -> DisplayName fallback (never the missing marker)
    CHECK(std::string(equipmentAddUiTextForStringIdPure(rows, 46044)) == "Plain Thing");

    // NOT an ADD ordinal -> nullptr (caller keeps legacy cLoadString path)
    CHECK(equipmentAddUiTextForStringIdPure(rows, 32156) == nullptr);
    CHECK(equipmentAddUiTextForStringIdPure(rows, 46156) == nullptr);
    // not a component-block id at all -> nullptr
    CHECK(equipmentAddUiTextForStringIdPure(rows, 12345) == nullptr);

    // gates OFF <=> rows empty -> ALWAYS nullptr (byte-identical stock path)
    std::vector<EquipmentAddFillRow> empty;
    CHECK(equipmentAddUiTextForStringIdPure(empty, 32040) == nullptr);
    CHECK(equipmentAddUiTextForStringIdPure(empty, 46040) == nullptr);

    // find helper directly
    CHECK(equipmentAddFindRow(rows, 40) == &rows[0]);
    CHECK(equipmentAddFindRow(rows, 41) == nullptr);
}
