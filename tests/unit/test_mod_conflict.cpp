// tests/unit/test_mod_conflict.cpp
// MOD-LOAD-ORDER-CONFLICT-REPORT-1: pure conflict classifier (mclib/mod_conflict.h).
//
// The SAME classifier runs engine-side (mclib/file.cpp InitModSearchPaths) and
// offline (mc2_modlint --mod). Coverage: def-key/qualified-id helpers, reserved
// core: namespace detection, duplicate id/file findings, stock-override
// visibility (incl. suppression under a reserved-core line), formatter cap +
// summary. Header-only, no engine/GL/IO.
#include "doctest.h"

#include "mod_conflict.h"

#include <string>
#include <vector>

// -------------------------------------------------------------------------
// def-key helpers
// -------------------------------------------------------------------------
TEST_CASE("mod_conflict: def-key detection and qualified-id derivation") {
    CHECK(modKeyIsDef("data/defs/units/sample/atlas/laserboat/unit.fit"));
    CHECK(modKeyIsDef("data/defs/equipment/sample/hot_medium_laser/equipmentdef.fit"));
    CHECK(!modKeyIsDef("data/objects/compbas.csv"));
    CHECK(!modKeyIsDef("data/defs/ui/ui_profiles.fit"));

    CHECK(modDefQualifiedId("data/defs/equipment/sample/hot_medium_laser/equipmentdef.fit")
          == "sample:hot_medium_laser");
    CHECK(modDefQualifiedId("data/defs/units/sample/atlas/laserboat/unit.fit")
          == "sample:atlas_laserboat");
    CHECK(modDefQualifiedId("data/defs/equipment/core/laser/equipmentdef.fit")
          == "core:laser");
    CHECK(modDefQualifiedId("data/objects/compbas.csv").empty());
    CHECK(modDefQualifiedId("data/defs/units/onlyns").empty());   // malformed: no id path
}

TEST_CASE("mod_conflict: reserved core: namespace detection") {
    CHECK(modKeyUsesReservedCoreNamespace("data/defs/units/core/atlas/chassisdef.fit"));
    CHECK(modKeyUsesReservedCoreNamespace("data/defs/equipment/core/laser/equipmentdef.fit"));
    CHECK(!modKeyUsesReservedCoreNamespace("data/defs/equipment/sample/laser/equipmentdef.fit"));
    CHECK(!modKeyUsesReservedCoreNamespace("data/defs/equipment/corex/laser/equipmentdef.fit"));
    CHECK(!modKeyUsesReservedCoreNamespace("data/objects/core/whatever.fit"));
}

// -------------------------------------------------------------------------
// classifier
// -------------------------------------------------------------------------
static bool hasCode(const std::vector<ModConflictFinding>& f, ModConflictCode c) {
    for (const auto& x : f) if (x.code == c) return true;
    return false;
}

TEST_CASE("mod_conflict: clean entries -> zero findings") {
    std::vector<ModIndexedEntry> e = {
        { "data/defs/equipment/sample/laser/equipmentdef.fit", "moda", {}, false },
        { "data/art/foo.tga", "moda", {}, false },
    };
    CHECK(classifyModConflicts(e).empty());
    CHECK(classifyModConflicts({}).empty());
}

TEST_CASE("mod_conflict: duplicate def id across two mods -> DUPLICATE_DEF_ID, winner named") {
    std::vector<ModIndexedEntry> e = {
        { "data/defs/equipment/sample/laser/equipmentdef.fit", "moda", { "modb" }, false },
    };
    auto f = classifyModConflicts(e);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == MODCONFLICT_DUPLICATE_DEF_ID);
    CHECK(f[0].line.find("sample:laser") != std::string::npos);
    CHECK(f[0].line.find("'moda'") != std::string::npos);
    CHECK(f[0].line.find("'modb'") != std::string::npos);
    CHECK(f[0].line.find("wins") != std::string::npos);
}

TEST_CASE("mod_conflict: duplicate NON-def file -> DUPLICATE_FILE with all losers") {
    std::vector<ModIndexedEntry> e = {
        { "data/objects/compbas.csv", "moda", { "modb", "modc" }, false },
    };
    auto f = classifyModConflicts(e);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == MODCONFLICT_DUPLICATE_FILE);
    CHECK(f[0].line.find("'modb', 'modc'") != std::string::npos);
}

TEST_CASE("mod_conflict: core: namespace def -> RESERVED_CORE_NAMESPACE; stock-override line suppressed") {
    std::vector<ModIndexedEntry> e = {
        { "data/defs/equipment/core/laser/equipmentdef.fit", "moda", {}, true },
    };
    auto f = classifyModConflicts(e);
    REQUIRE(f.size() == 1);   // ONE line per finding — no double report
    CHECK(f[0].code == MODCONFLICT_RESERVED_CORE_NAMESPACE);
    CHECK(f[0].line.find("core:laser") != std::string::npos);
    CHECK(f[0].line.find("'moda'") != std::string::npos);
}

TEST_CASE("mod_conflict: non-core def shadowing a stock file -> STOCK_OVERRIDE") {
    std::vector<ModIndexedEntry> e = {
        { "data/defs/units/sample/atlas/laserboat/unit.fit", "moda", {}, true },
    };
    auto f = classifyModConflicts(e);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == MODCONFLICT_STOCK_OVERRIDE);
    CHECK(f[0].line.find("overrides stock") != std::string::npos);
    CHECK(f[0].line.find("sample:atlas_laserboat") != std::string::npos);
}

TEST_CASE("mod_conflict: one entry can yield duplicate AND reserved findings") {
    std::vector<ModIndexedEntry> e = {
        { "data/defs/equipment/core/ppc/equipmentdef.fit", "moda", { "modb" }, true },
    };
    auto f = classifyModConflicts(e);
    CHECK(f.size() == 2);
    CHECK(hasCode(f, MODCONFLICT_DUPLICATE_DEF_ID));
    CHECK(hasCode(f, MODCONFLICT_RESERVED_CORE_NAMESPACE));
    CHECK(!hasCode(f, MODCONFLICT_STOCK_OVERRIDE));   // folded into the reserved line
}

// -------------------------------------------------------------------------
// formatter: cap + summary line
// -------------------------------------------------------------------------
TEST_CASE("mod_conflict: formatter emits token lines and one summary line") {
    std::vector<ModIndexedEntry> e = {
        { "data/defs/equipment/core/ppc/equipmentdef.fit", "moda", {}, false },
        { "data/defs/units/sample/atlas/boat/unit.fit", "moda", { "modb" }, true },
    };
    auto lines = formatModConflictReport(classifyModConflicts(e));
    REQUIRE(lines.size() == 4);   // 3 findings + summary
    CHECK(lines[0].rfind("[MODCONFLICT_", 0) == 0);
    CHECK(lines.back().find("conflict report: 3 finding(s)") != std::string::npos);
    CHECK(lines.back().find("1 duplicate") != std::string::npos);
    CHECK(lines.back().find("1 reserved-core") != std::string::npos);
    CHECK(lines.back().find("1 stock-override") != std::string::npos);
}

TEST_CASE("mod_conflict: formatter caps finding lines and reports suppression") {
    std::vector<ModConflictFinding> many;
    for (int i = 0; i < 30; ++i)
        many.push_back({ MODCONFLICT_DUPLICATE_FILE, "file " + std::to_string(i) });
    auto lines = formatModConflictReport(many, 20);
    REQUIRE(lines.size() == 22);   // 20 shown + "+10 more" + summary
    CHECK(lines[20] == "(+10 more finding(s) suppressed)");
    CHECK(lines[21].find("30 finding(s)") != std::string::npos);
}

TEST_CASE("mod_conflict: zero findings -> summary line only") {
    auto lines = formatModConflictReport({});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("conflict report: 0 finding(s)") != std::string::npos);
}
