// tests/unit/test_modlint.cpp
// UNIT-MODLINT-CLI-1 (Phase-3 slice 3 of the UNIT-PROFILE arc).
//
// Offline doctest coverage for the mc2_modlint pieces: the reason-code -> message
// table (unit_profile_findings_text.h), the facts-JSON parser (modlint_input.h),
// and the finding formatter (modlint_report.h). Proves modlint reuses the SAME
// validateUnitDefinition the engine uses (single source of truth): a coherent
// facts input -> zero findings; a deliberately-broken one -> the specific expected
// modder-term codes. Header-only, drags no engine/GL/IO.
#include "doctest.h"

#include "unit_profile_validator.h"
#include "unit_profile_findings_text.h"
#include "../../tools/mc2_modlint/modlint_input.h"
#include "../../tools/mc2_modlint/modlint_report.h"
#include "../../tools/mc2_modlint/modlint_registry.h"

#include <string>
#include <vector>

// -------------------------------------------------------------------------
// Reason-code -> message table: every LIVE code has a distinct token + message.
// -------------------------------------------------------------------------

TEST_CASE("modlint: every reason code maps to a distinct token and a message") {
    for (int c = 0; c < UPV__COUNT; ++c) {
        UnitProfileReasonCode code = (UnitProfileReasonCode)c;
        const char* tok = unitProfileReasonToken(code);
        const char* msg = unitProfileReasonMessage(code);
        CHECK(tok != nullptr);
        CHECK(msg != nullptr);
        CHECK(std::string(tok) != "UPV_UNKNOWN");   // no code falls through
        CHECK(std::string(msg) != "unknown finding");
        CHECK(std::string(tok).rfind("UPV_", 0) == 0);
    }
    // Tokens are unique across the whole vocabulary (grep keys must not collide).
    for (int a = 0; a < UPV__COUNT; ++a)
        for (int b = a + 1; b < UPV__COUNT; ++b)
            CHECK(std::string(unitProfileReasonToken((UnitProfileReasonCode)a))
                  != unitProfileReasonToken((UnitProfileReasonCode)b));
}

// -------------------------------------------------------------------------
// Facts-JSON parser: coherent unit -> zero findings via the SAME validator.
// -------------------------------------------------------------------------

static std::vector<UnitProfileFinding> lint(const std::string& json, std::string* label = nullptr) {
    bool ok = true;
    MlJson root = parseModlintJson(json, &ok);
    REQUIRE(ok);
    const ActionExecutorRegistry reg = modlintExecutorRegistry();
    UnitDefinitionValidationInputs in = factsFromJson(root, label);
    return validateUnitDefinition(in, reg);
}

TEST_CASE("modlint: coherent stock jet mech (walker + jump) -> ZERO findings") {
    std::string label;
    auto f = lint(R"({"unit_id":"core:atlas","jump":true,"locomotion":"MechWalker"})", &label);
    CHECK(f.empty());
    CHECK(label == "core:atlas");
}

TEST_CASE("modlint: coherent stock vehicle (no jump) -> ZERO findings") {
    auto f = lint(R"({"unit_id":"core:armoredcar","jump":false,"locomotion":"Wheeled"})");
    CHECK(f.empty());
}

TEST_CASE("modlint: empty JSON object -> ZERO findings (defaults are coherent)") {
    auto f = lint("{}");
    CHECK(f.empty());
}

// -------------------------------------------------------------------------
// Broken units: each facet trips its specific modder-term code.
// -------------------------------------------------------------------------

TEST_CASE("modlint: vehicle granted jump -> JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR") {
    auto f = lint(R"({"unit_id":"myMod:jeep","jump":true,"locomotion":"Wheeled"})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);
}

TEST_CASE("modlint: AC20 (Ballistic) on an Energy-only hardpoint -> WEAPON_CLASS_REJECTED") {
    auto f = lint(R"({
        "unit_id":"myMod:hbk",
        "loadout_joins":[
            {"weapon_tag":"Ballistic","accepts":["Energy"],
             "damage_location":"RightTorso","unit_schema":"Mech8Part","socket_present":true}
        ]})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_HARDPOINT_WEAPON_CLASS_REJECTED);
}

TEST_CASE("modlint: mech-arm hardpoint on a vehicle schema -> DAMAGE_LOCATION_MISSING") {
    auto f = lint(R"({
        "loadout_joins":[
            {"weapon_tag":"Energy","accepts":["Energy"],
             "damage_location":"RightArm","unit_schema":"VehicleFacings","socket_present":true}
        ]})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_HARDPOINT_DAMAGE_LOCATION_MISSING);
}

TEST_CASE("modlint: declared socket absent on model -> SOCKET_MISSING") {
    auto f = lint(R"({"declared_sockets":[{"present":false}]})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_HARDPOINT_SOCKET_MISSING);
}

TEST_CASE("modlint: animation fallback dead-end -> ANIMATION_FALLBACK_MISSING") {
    auto f = lint(R"({"anim_fallbacks":[{"resolved":false}]})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_ANIMATION_FALLBACK_MISSING);
}

TEST_CASE("modlint: declared UI asset unresolved -> UI_ASSET_MISSING") {
    auto f = lint(R"({"ui_assets":[{"resolves":false}]})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_UI_ASSET_MISSING);
}

TEST_CASE("modlint: a deliberately-broken unit trips EVERY live code in facet order") {
    auto f = lint(R"({
        "unit_id":"myMod:frankenmech",
        "jump":true,"locomotion":"Wheeled",
        "loadout_joins":[
            {"weapon_tag":"Ballistic","accepts":["Energy"],
             "damage_location":"RightArm","unit_schema":"VehicleFacings","socket_present":false}
        ],
        "declared_sockets":[{"present":false}],
        "anim_fallbacks":[{"resolved":false}],
        "ui_assets":[{"resolves":false}]
    })");
    bool seen[UPV__COUNT] = {false};
    for (const UnitProfileFinding& x : f) seen[x.code] = true;
    CHECK(seen[UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR]);
    CHECK(seen[UPV_HARDPOINT_WEAPON_CLASS_REJECTED]);
    CHECK(seen[UPV_HARDPOINT_DAMAGE_LOCATION_MISSING]);
    CHECK(seen[UPV_HARDPOINT_SOCKET_MISSING]);
    CHECK(seen[UPV_ANIMATION_FALLBACK_MISSING]);
    CHECK(seen[UPV_UI_ASSET_MISSING]);
    // Stable facet order: jump first, UI last.
    CHECK(f.front().code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);
    CHECK(f.back().code  == UPV_UI_ASSET_MISSING);
}

// -------------------------------------------------------------------------
// Formatter: the actual modder-term LINES modlint prints.
// -------------------------------------------------------------------------

TEST_CASE("modlint: formatted line carries the token and a modder-readable message") {
    auto f = lint(R"({"jump":true,"locomotion":"Wheeled"})");
    REQUIRE(f.size() == 1);
    std::string line = formatFinding(f[0]);
    CHECK(line.find("UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR") != std::string::npos);
    CHECK(line.find("Jump granted but no compatible jump executor") != std::string::npos);
}

TEST_CASE("modlint: clean unit formats an OK block; broken formats a finding count") {
    CHECK(formatUnitFindings("core:atlas", {}).find("OK (0 findings)") != std::string::npos);
    auto f = lint(R"({"jump":true,"locomotion":"Wheeled"})");
    std::string block = formatUnitFindings("myMod:jeep", f);
    CHECK(block.find("myMod:jeep") != std::string::npos);
    CHECK(block.find("1 finding(s)") != std::string::npos);
}

// -------------------------------------------------------------------------
// Roster path: a converter summary report lints jump-facet per unit.
// -------------------------------------------------------------------------

TEST_CASE("modlint: roster report lints jump facet per unit") {
    std::string report = R"({
        "tool":"LEGACY-UNIT-CONVERTER-1",
        "units":[
            {"unit_id":"core:atlas","jump":true,"locomotion":"MechWalker"},
            {"unit_id":"myMod:jeep","jump":true,"locomotion":"Wheeled"}
        ]})";
    bool ok = true;
    MlJson root = parseModlintJson(report, &ok);
    REQUIRE(ok);
    std::vector<RosterUnitFacts> units = parseRosterUnits(root);
    REQUIRE(units.size() == 2);
    const ActionExecutorRegistry reg = modlintExecutorRegistry();
    // Atlas (walker+jump) coherent; jeep (wheeled+jump) trips the jump code.
    CHECK(validateUnitDefinition(units[0].inputs, reg).empty());
    auto jf = validateUnitDefinition(units[1].inputs, reg);
    REQUIRE(jf.size() == 1);
    CHECK(jf[0].code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);
}
