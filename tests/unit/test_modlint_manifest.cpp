// tests/unit/test_modlint_manifest.cpp
// MOD-PACKAGE-MANIFEST-1: mod.json manifest validation (modlint_manifest.h).
//
// Pure coverage for the MODMAN_* reason-code table, the manifest facts parser,
// the field rules (id token / dotted version / type enum / deps), and the
// formatter. Header-only, drags no engine/GL/IO.
#include "doctest.h"

#include "../../tools/mc2_modlint/modlint_input.h"     // parseModlintJson
#include "../../tools/mc2_modlint/modlint_manifest.h"

#include <string>
#include <vector>

static std::vector<ModManifestFinding> lintManifestJson(const std::string& json) {
    bool ok = true;
    MlJson root = parseModlintJson(json, &ok);
    REQUIRE(ok);
    return validateModManifest(manifestFactsFromJson(root));
}

static bool hasCode(const std::vector<ModManifestFinding>& f, ModManifestReasonCode c) {
    for (const auto& x : f) if (x.code == c) return true;
    return false;
}

// -------------------------------------------------------------------------
// Reason-code table: every live code has a distinct token + message.
// -------------------------------------------------------------------------
TEST_CASE("manifest: every MODMAN code maps to a distinct token and message") {
    for (int c = 0; c < MODMAN__COUNT; ++c) {
        ModManifestReasonCode code = (ModManifestReasonCode)c;
        CHECK(std::string(modManifestReasonToken(code)) != "MODMAN_UNKNOWN");
        CHECK(std::string(modManifestReasonMessage(code)) != "unknown finding");
        CHECK(std::string(modManifestReasonToken(code)).rfind("MODMAN_", 0) == 0);
    }
    for (int a = 0; a < MODMAN__COUNT; ++a)
        for (int b = a + 1; b < MODMAN__COUNT; ++b)
            CHECK(std::string(modManifestReasonToken((ModManifestReasonCode)a))
                  != modManifestReasonToken((ModManifestReasonCode)b));
}

// -------------------------------------------------------------------------
// Well-formed manifest (the shape the sample mods ship) -> zero findings.
// -------------------------------------------------------------------------
TEST_CASE("manifest: full well-formed mod.json -> zero findings") {
    auto f = lintManifestJson(R"({
        "schema": "mc2-mod/1",
        "id": "sample_equipment",
        "name": "Sample Equipment",
        "version": "1.0.0",
        "author": "someone",
        "type": "assets",
        "dependencies": [],
        "description": "adds a laser"
    })");
    CHECK(f.empty());
}

TEST_CASE("manifest: author is OPTIONAL — absence is never a finding") {
    auto f = lintManifestJson(R"({
        "schema": "mc2-mod/1", "id": "m", "name": "M", "version": "1.0",
        "type": "campaign", "dependencies": ["base_layer"], "description": "d"
    })");
    CHECK(f.empty());
}

// -------------------------------------------------------------------------
// Missing / malformed fields -> the specific expected codes.
// -------------------------------------------------------------------------
TEST_CASE("manifest: empty object -> all required-field findings, once each") {
    auto f = lintManifestJson("{}");
    CHECK(hasCode(f, MODMAN_SCHEMA_MISSING));
    CHECK(hasCode(f, MODMAN_ID_MISSING));
    CHECK(hasCode(f, MODMAN_NAME_MISSING));
    CHECK(hasCode(f, MODMAN_VERSION_MISSING));
    CHECK(hasCode(f, MODMAN_TYPE_MISSING));
    CHECK(hasCode(f, MODMAN_DESCRIPTION_MISSING));
    CHECK(f.size() == 6);
}

TEST_CASE("manifest: non-object top level -> single MODMAN_NOT_OBJECT") {
    auto f = lintManifestJson(R"([1,2,3])");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == MODMAN_NOT_OBJECT);
}

TEST_CASE("manifest: wrong schema string -> MODMAN_SCHEMA_UNKNOWN with detail") {
    auto f = lintManifestJson(R"({"schema":"mc2-mod/9","id":"m","name":"M",
        "version":"1.0","type":"assets","description":"d"})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == MODMAN_SCHEMA_UNKNOWN);
    CHECK(f[0].detail == "mc2-mod/9");
}

TEST_CASE("manifest: id token rule") {
    CHECK(modIdTokenValid("sample_equipment"));
    CHECK(modIdTokenValid("mod-1.2_x"));
    CHECK(modIdTokenValid("9lives"));
    CHECK(!modIdTokenValid(""));
    CHECK(!modIdTokenValid("Sample"));      // uppercase
    CHECK(!modIdTokenValid("_leading"));    // bad head char
    CHECK(!modIdTokenValid("has space"));
    CHECK(!modIdTokenValid("core:thing"));  // ':' not allowed in mod ids

    auto f = lintManifestJson(R"({"schema":"mc2-mod/1","id":"Bad Id","name":"M",
        "version":"1.0","type":"assets","description":"d"})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == MODMAN_ID_INVALID);
    CHECK(f[0].detail == "Bad Id");
}

TEST_CASE("manifest: version rule = dotted digits") {
    CHECK(modVersionValid("1"));
    CHECK(modVersionValid("1.0"));
    CHECK(modVersionValid("0.1.0"));
    CHECK(!modVersionValid(""));
    CHECK(!modVersionValid("1."));
    CHECK(!modVersionValid(".1"));
    CHECK(!modVersionValid("1..0"));
    CHECK(!modVersionValid("v1.0"));
    CHECK(!modVersionValid("1.0-beta"));

    auto f = lintManifestJson(R"({"schema":"mc2-mod/1","id":"m","name":"M",
        "version":"v1.0","type":"assets","description":"d"})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == MODMAN_VERSION_INVALID);
}

TEST_CASE("manifest: type must be assets|campaign|dependency") {
    for (const char* t : { "assets", "campaign", "dependency" }) {
        std::string j = std::string(R"({"schema":"mc2-mod/1","id":"m","name":"M",
            "version":"1.0","type":")") + t + R"(","description":"d"})";
        CHECK(lintManifestJson(j).empty());
    }
    auto f = lintManifestJson(R"({"schema":"mc2-mod/1","id":"m","name":"M",
        "version":"1.0","type":"addon","description":"d"})");
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == MODMAN_TYPE_UNKNOWN);
    CHECK(f[0].detail == "addon");
}

TEST_CASE("manifest: dependencies — invalid token, self-dep, duplicate") {
    auto f = lintManifestJson(R"({"schema":"mc2-mod/1","id":"m","name":"M",
        "version":"1.0","type":"assets","description":"d",
        "dependencies":["ok_dep","Bad Dep","m","ok_dep"]})");
    CHECK(hasCode(f, MODMAN_DEP_INVALID));
    CHECK(hasCode(f, MODMAN_DEP_SELF));
    CHECK(hasCode(f, MODMAN_DEP_DUPLICATE));
    CHECK(f.size() == 3);
}

// -------------------------------------------------------------------------
// Formatter: modder-first shape (label + source + token lines).
// -------------------------------------------------------------------------
TEST_CASE("manifest: formatter emits label, source, and token lines") {
    std::vector<ModManifestFinding> f = { { MODMAN_VERSION_INVALID, "v1" } };
    std::string s = formatManifestFindings("mymod", f, "mods/mymod/mod.json");
    CHECK(s.find("manifest mymod [mods/mymod/mod.json]") != std::string::npos);
    CHECK(s.find("[MODMAN_VERSION_INVALID]") != std::string::npos);
    CHECK(s.find("'v1'") != std::string::npos);

    std::string clean = formatManifestFindings("mymod", {}, "");
    CHECK(clean == "manifest mymod: OK (0 findings)\n");
}

TEST_CASE("manifest: sample mods' manifest shape parses with author field") {
    bool ok = true;
    MlJson root = parseModlintJson(R"({
        "schema":"mc2-mod/1","id":"core_unlocks","name":"Core Unlocks",
        "version":"0.1.0","author":"MC2 remaster examples","type":"assets",
        "dependencies":[],"description":"x"})", &ok);
    REQUIRE(ok);
    ModManifestFacts facts = manifestFactsFromJson(root);
    CHECK(facts.author == "MC2 remaster examples");
    CHECK(validateModManifest(facts).empty());
}
