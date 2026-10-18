// Shared modder-finding struct + ONE line format
// (code/modder_finding.h) + the token-recognition probes that let loaders report
// a silent fallback instead of swallowing it (locomotion_facet.h /
// damage_schema.h). Pure, offline.
#include "doctest.h"

#include "modder_finding.h"
#include "locomotion_facet.h"
#include "damage_schema.h"

#include <string>

TEST_SUITE("ModderFinding") {

TEST_CASE("full fill: every context field present, in stable order") {
    ModderFinding f;
    f.reason   = "EQV_WEAPON_RANGE_INVALID";
    f.message  = "weapon range buckets incoherent";
    f.source   = "sample_bad_mod";
    f.file     = "data/defs/equipment/sample/hot_medium_laser/equipmentdef.fit";
    f.id       = "sample:hot_medium_laser";
    f.field    = "RangeBucket";
    f.expected = "Short|Medium|Long";
    f.actual   = "Sideways";
    f.fallback = "legacy weapon stats";
    CHECK(formatModderFinding(f) ==
        "[EQV_WEAPON_RANGE_INVALID] weapon range buckets incoherent "
        "(source=sample_bad_mod "
        "file=data/defs/equipment/sample/hot_medium_laser/equipmentdef.fit "
        "id=sample:hot_medium_laser field=RangeBucket expected=Short|Medium|Long "
        "actual=Sideways fallback=legacy weapon stats)");
}

TEST_CASE("empty fields are omitted; no dangling parens or brackets") {
    ModderFinding f;
    f.message  = "unknown Locomotion token in generated def";
    f.file     = "data/defs/units/core/hunchback/chassisdef.fit";
    f.field    = "Locomotion";
    f.fallback = "legacy locomotion 'MechWalker'";
    CHECK(formatModderFinding(f) ==
        "unknown Locomotion token in generated def "
        "(file=data/defs/units/core/hunchback/chassisdef.fit "
        "field=Locomotion fallback=legacy locomotion 'MechWalker')");
}

TEST_CASE("message-only finding: no context suffix at all") {
    ModderFinding f;
    f.message = "nothing else known";
    CHECK(formatModderFinding(f) == "nothing else known");
    CHECK(modderFindingContext(f).empty());
}

TEST_CASE("reason token alone still brackets; context alone still parenthesizes") {
    ModderFinding f;
    f.reason  = "UPV_HARDPOINT_SOCKET_MISSING";
    f.message = "declared socket not found";
    CHECK(formatModderFinding(f) ==
        "[UPV_HARDPOINT_SOCKET_MISSING] declared socket not found");
    f.reason.clear();
    f.source = "core";
    CHECK(formatModderFinding(f) == "declared socket not found (source=core)");
}

// --- silent-fallback probes ------------------------------------------------

TEST_CASE("locomotionTokenKnown: known tokens recognized, case-insensitive") {
    CHECK(locomotionTokenKnown("MechWalker"));
    CHECK(locomotionTokenKnown("mechwalker"));
    CHECK(locomotionTokenKnown("TRACKED"));
    CHECK(locomotionTokenKnown("Wheeled"));
    CHECK(locomotionTokenKnown("Hover"));
    CHECK(locomotionTokenKnown("Infantry"));
}

TEST_CASE("locomotionTokenKnown: unknown/empty/null -> false (would fall back)") {
    CHECK_FALSE(locomotionTokenKnown("Walker"));      // near-miss typo
    CHECK_FALSE(locomotionTokenKnown("MechWalkerX"));
    CHECK_FALSE(locomotionTokenKnown(""));
    CHECK_FALSE(locomotionTokenKnown(nullptr));
}

TEST_CASE("damageSchemaTokenKnown: known tokens recognized, unknown rejected") {
    CHECK(damageSchemaTokenKnown("Mech8Part"));
    CHECK(damageSchemaTokenKnown("vehiclefacings"));
    CHECK(damageSchemaTokenKnown("Squad"));
    CHECK(damageSchemaTokenKnown("SingleActor"));
    CHECK_FALSE(damageSchemaTokenKnown("Mech8Parts"));
    CHECK_FALSE(damageSchemaTokenKnown(""));
    CHECK_FALSE(damageSchemaTokenKnown(nullptr));
}

TEST_CASE("vocabulary strings: every advertised token is actually accepted") {
    // Split "A|B|C" and require tokenKnown for each — locks the expected= text
    // shown to modders to the parser's real vocabulary.
    auto eachToken = [](const char* vocab, auto&& check) {
        std::string v(vocab), tok;
        for (size_t i = 0; i <= v.size(); ++i) {
            if (i == v.size() || v[i] == '|') { check(tok); tok.clear(); }
            else tok += v[i];
        }
    };
    eachToken(locomotionTokenVocabulary(),
              [](const std::string& t) { CHECK(locomotionTokenKnown(t.c_str())); });
    eachToken(damageSchemaTokenVocabulary(),
              [](const std::string& t) { CHECK(damageSchemaTokenKnown(t.c_str())); });
}

TEST_CASE("struct fill from a parse failure: unknown token -> complete context") {
    // Mirrors the engine loader's emit site (unitprofile_fit.cpp): a generated
    // def carries Locomotion=Sideways; the finding must carry file/id/field/
    // expected/actual/fallback so the modder needs NO debugger.
    const char* badToken = "Sideways";
    REQUIRE_FALSE(locomotionTokenKnown(badToken));
    const char* legacy = locomotionModelName(LOCO_MECH_WALKER);  // the kept legacy value
    ModderFinding f;
    f.message  = "unknown Locomotion token in generated def";
    f.source   = "generated";
    f.file     = "data/defs/units/core/atlas/chassisdef.fit";
    f.id       = "core:atlas";
    f.field    = "Locomotion";
    f.expected = locomotionTokenVocabulary();
    f.actual   = badToken;
    f.fallback = std::string("legacy locomotion '") + legacy + "'";
    const std::string line = formatModderFinding(f);
    CHECK(line.find("file=data/defs/units/core/atlas/chassisdef.fit") != std::string::npos);
    CHECK(line.find("id=core:atlas") != std::string::npos);
    CHECK(line.find("field=Locomotion") != std::string::npos);
    CHECK(line.find("expected=MechWalker|Tracked|Wheeled|Hover|Infantry") != std::string::npos);
    CHECK(line.find("actual=Sideways") != std::string::npos);
    CHECK(line.find("fallback=legacy locomotion '") != std::string::npos);
}

} // TEST_SUITE
