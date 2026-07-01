// tests/unit/test_equipment_modlint.cpp
// WEAPON-MODLINT-1 (equipment arc, slice 7).
//
// Offline doctest coverage for the mc2_modlint equipment pieces: the EQV_ reason-
// code -> message table (equipment_findings_text.h), the equipment facts-JSON
// parser (modlint_equipment_input.h), the converter-roster parser, and the finding
// formatter (modlint_equipment_report.h). Proves the CLI reuses the SAME
// authoritative validateEquipmentDefinition (single source of truth): coherent
// facts -> zero findings; deliberately-broken facts -> the specific expected EQV
// codes. Header-only, drags no engine/GL/IO.
#include "doctest.h"

#include "equipment_validator.h"
#include "equipment_findings_text.h"
#include "../../tools/mc2_modlint/modlint_equipment_input.h"
#include "../../tools/mc2_modlint/modlint_equipment_report.h"

#include <string>
#include <vector>

// -------------------------------------------------------------------------
// Reason-code -> message table: every LIVE EQV_ code has a distinct token + message.
// -------------------------------------------------------------------------

TEST_CASE("equipment modlint: every EQV code maps to a distinct token and a message") {
    for (int c = 0; c < EQV__COUNT; ++c) {
        EquipmentReasonCode code = (EquipmentReasonCode)c;
        const char* tok = equipmentReasonToken(code);
        const char* msg = equipmentReasonMessage(code);
        CHECK(tok != nullptr);
        CHECK(msg != nullptr);
        CHECK(std::string(tok) != "EQV_UNKNOWN");         // no code falls through
        CHECK(std::string(msg) != "unknown finding");
        CHECK(std::string(tok).rfind("EQV_", 0) == 0);
    }
    for (int a = 0; a < EQV__COUNT; ++a)
        for (int b = a + 1; b < EQV__COUNT; ++b)
            CHECK(std::string(equipmentReasonToken((EquipmentReasonCode)a))
                  != equipmentReasonToken((EquipmentReasonCode)b));
}

// -------------------------------------------------------------------------
// Facts-JSON parser: coherent weapon -> zero findings via the SAME validator.
// -------------------------------------------------------------------------

static std::vector<EquipmentFinding> lint(const std::string& json, std::string* label = nullptr) {
    bool ok = true;
    MlJson root = parseModlintJson(json, &ok);
    REQUIRE(ok);
    EquipmentValidationInputs in = equipmentFactsFromJson(root, label);
    return validateEquipmentDefinition(in);
}

TEST_CASE("equipment modlint: coherent stock weapon (executor, ranges, no ammo) -> ZERO findings") {
    std::string label;
    auto f = lint(R"({
        "equip_id":"core:mediumlaser","kind":"Weapon",
        "fire_executor":"beam_direct","category":"Energy",
        "damage":25,"range_short":30,"range_med":120,"range_long":250
    })", &label);
    CHECK(f.empty());
    CHECK(label == "core:mediumlaser");
}

TEST_CASE("equipment modlint: coherent ammo weapon (names ammo category) -> ZERO findings") {
    auto f = lint(R"({
        "equip_id":"core:ac20","kind":"Weapon","fire_executor":"projectile_ballistic",
        "category":"Ballistic","ammo_per_shot":1,"ammo_category":"core:ammo_ac20",
        "range_short":90,"range_med":180,"range_long":270
    })");
    CHECK(f.empty());
}

TEST_CASE("equipment modlint: empty JSON object -> ZERO findings (defaults coherent)") {
    auto f = lint("{}");
    CHECK(f.empty());
}

// -------------------------------------------------------------------------
// Broken facts: each defect trips its specific EQV code.
// -------------------------------------------------------------------------

TEST_CASE("equipment modlint: weapon with no fire executor -> EQV_WEAPON_NO_FIRE_EXECUTOR") {
    auto f = lint(R"({"equip_id":"myMod:ppc","kind":"Weapon","fire_executor":""})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_NO_FIRE_EXECUTOR);
}

TEST_CASE("equipment modlint: weapon with unauthored category -> EQV_WEAPON_NO_CATEGORY") {
    auto f = lint(R"({"equip_id":"myMod:blank","kind":"Weapon","category_authored":false})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_NO_CATEGORY);
}

TEST_CASE("equipment modlint: ammo weapon names no category -> EQV_AMMO_CATEGORY_MISSING") {
    auto f = lint(R"({"equip_id":"myMod:ac","kind":"Weapon","ammo_per_shot":2,"ammo_category":""})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_AMMO_CATEGORY_MISSING);
}

TEST_CASE("equipment modlint: disordered ranges -> EQV_WEAPON_RANGE_INVALID") {
    auto f = lint(R"({"equip_id":"myMod:bad","kind":"Weapon",
                      "range_short":300,"range_med":100,"range_long":250})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_RANGE_INVALID);
}

TEST_CASE("equipment modlint: kind=Ammo but weapon facet authored -> EQV_KIND_FACET_MISMATCH") {
    auto f = lint(R"({"equip_id":"myMod:mix","kind":"Ammo","has_weapon":true,
                      "fire_executor":"beam_direct"})");
    // kind=Ammo + hasWeapon=true -> mismatch (weapon-facet checks also run).
    bool seen = false;
    for (auto& x : f) if (x.code == EQV_KIND_FACET_MISMATCH) seen = true;
    CHECK(seen);
}

TEST_CASE("equipment modlint: claimed event with no asset -> EQV_PRESENTATION_ASSET_MISSING") {
    auto f = lint(R"({"equip_id":"myMod:laser","kind":"Weapon","fire_executor":"beam_direct",
                      "claimed_events":["OnFire","OnImpact"],"presented_events":["OnFire"]})");
    // OnImpact claimed but not presented -> one presentation finding.
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_PRESENTATION_ASSET_MISSING);
}

TEST_CASE("equipment modlint: Ballistic weapon on Energy-only hardpoint -> EQV_WEAPON_HARDPOINT_REJECTED") {
    auto f = lint(R"({"equip_id":"myMod:ac20","kind":"Weapon","fire_executor":"projectile_ballistic",
                      "category":"Ballistic","hardpoints":[{"accepts":["Energy"]}]})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_HARDPOINT_REJECTED);
}

// -------------------------------------------------------------------------
// Formatter: the actual modder-term LINES modlint prints.
// -------------------------------------------------------------------------

TEST_CASE("equipment modlint: formatted line carries the token and a modder-readable message") {
    auto f = lint(R"({"equip_id":"myMod:ppc","kind":"Weapon","fire_executor":""})");
    REQUIRE(f.size() == 1);
    std::string line = formatEquipmentFinding(f[0]);
    CHECK(line.find("EQV_WEAPON_NO_FIRE_EXECUTOR") != std::string::npos);
    CHECK(line.find("Weapon has no fire executor bound") != std::string::npos);
}

TEST_CASE("equipment modlint: clean component formats an OK block; broken a finding count") {
    CHECK(formatEquipmentFindings("core:mlas", {}).find("OK (0 findings)") != std::string::npos);
    auto f = lint(R"({"equip_id":"myMod:ppc","kind":"Weapon","fire_executor":""})");
    std::string block = formatEquipmentFindings("myMod:ppc", f);
    CHECK(block.find("myMod:ppc") != std::string::npos);
    CHECK(block.find("1 finding(s)") != std::string::npos);
}

// -------------------------------------------------------------------------
// Converter roster path: an equipment converter --json report lints each component.
// -------------------------------------------------------------------------

TEST_CASE("equipment modlint: converter roster lints each component; stock-shaped -> clean") {
    std::string report = R"({
        "tool":"LEGACY-COMPBAS-CONVERTER-1",
        "components":[
            {"equip_id":"core:mediumlaser","name":"Medium Laser","kind":"Weapon",
             "weapon":{"category":"Energy","fire_executor":"beam_direct","damage":25,
                       "ammo_category":"","ammo_master_id":0}},
            {"equip_id":"core:ac20","name":"AC/20","kind":"Weapon",
             "weapon":{"category":"Ballistic","fire_executor":"projectile_ballistic","damage":40,
                       "ammo_category":"core:ammo_ac20","ammo_master_id":85}},
            {"equip_id":"core:heatsink","name":"Heat Sink","kind":"Heatsink"}
        ]})";
    bool ok = true;
    MlJson root = parseModlintJson(report, &ok);
    REQUIRE(ok);
    std::vector<RosterEquipmentFacts> comps = parseEquipmentRoster(root);
    REQUIRE(comps.size() == 3);
    int total = 0;
    for (auto& c : comps) total += (int)validateEquipmentDefinition(c.inputs).size();
    CHECK(total == 0);   // all three stock-shaped -> clean
}

TEST_CASE("equipment modlint: converter roster surfaces a broken component (no executor)") {
    std::string report = R"({
        "components":[
            {"equip_id":"core:goodlaser","kind":"Weapon",
             "weapon":{"category":"Energy","fire_executor":"beam_direct","ammo_master_id":0}},
            {"equip_id":"myMod:brokengun","kind":"Weapon",
             "weapon":{"category":"Ballistic","fire_executor":"","ammo_master_id":0}}
        ]})";
    bool ok = true;
    MlJson root = parseModlintJson(report, &ok);
    REQUIRE(ok);
    auto comps = parseEquipmentRoster(root);
    REQUIRE(comps.size() == 2);
    CHECK(validateEquipmentDefinition(comps[0].inputs).empty());
    auto bad = validateEquipmentDefinition(comps[1].inputs);
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].code == EQV_WEAPON_NO_FIRE_EXECUTOR);
}
