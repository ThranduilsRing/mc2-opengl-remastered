// Synthetic legacy-mod shapes: no installed content is opened or modified.
#include "doctest.h"
#include "model_override_registry.h"
#include "loose_asset_priority.h"
#include "fst_hash.h"
#include "save_archive.h"
#include "../../tools/mc2_modlint/modlint_input.h"
#include "../../tools/mc2_modlint/modlint_manifest.h"
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
struct ModFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("mc2-mod-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ModFixture() { std::filesystem::create_directories(root); }
    ~ModFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::string write(const char* name, std::string_view bytes) {
        const auto path = root / name;
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
        return path.string();
    }
};
constexpr auto model = R"({"overrides":[{"type":"model","class":"staticProp","replaces":"staticProp:depot","source":"source/depot.glb","renderOnly":true,"fallback":"stock"}]})";
}

TEST_SUITE("ModRegressions") {
TEST_CASE("mod mount order agrees with first-wins VFS including duplicate env deps") {
    // Dependencies are highest first; manifests merge lowest first, active last.
    const auto order = modelOverrideMountOrder("exodus", " shared;wrong,shared;exodus ", {"shared"});
    CHECK(order == std::vector<std::string>{"mods/kit", "mods/shared", "mods/exodus"});
}

TEST_CASE("model merge keeps unrelated entries and provider-relative sources") {
    ModFixture fixture;
    const auto path = fixture.write("models.json", model);
    ModelOverrideRegistry registry;
    REQUIRE(registry.loadFromFile(path, "mods/kit/data/model_overrides") == 1);
    // Keep a distinct kit record while replacing depot from a campaign layer.
    auto other = std::string(model);
    other.replace(other.find(":depot"), 6, ":tower");
    REQUIRE(registry.mergeFromFile(fixture.write("other.json", other), "mods/kit/data/model_overrides") == 1);
    REQUIRE(registry.mergeFromFile(path, "mods/wrong/data/model_overrides") == 1);
    REQUIRE(registry.count() == 2);
    const auto* depot = registry.resolve("STATICPROP", "Depot");
    REQUIRE(depot != nullptr);
    CHECK(depot->manifestDir == "mods/exodus/data/model_overrides");
    CHECK(depot->sourceRelPath == "source/depot.glb");
    CHECK(registry.resolve("staticProp", "tower")->manifestDir == "mods/kit/data/model_overrides");
}

TEST_CASE("manifest schema drift is diagnosed before launching a mod") {
    bool ok = false;
    const auto json = parseModlintJson(R"({"schema":"mc2-mod/1","id":"magicexpansion","name":"Magic","version":"1.0","type":"campaign","description":"synthetic"})", &ok);
    REQUIRE(ok);
    const auto findings = validateModManifest(manifestFactsFromJson(json));
    REQUIRE(findings.size() == 1);
    CHECK(findings.front().code == MODMAN_SCHEMA_UNKNOWN);
}

TEST_CASE("retail FST keys canonicalize mixed separators before hashing") {
    // CVE-G, REAP3 and MCO archives use this spelling; never rewrite their disk bytes.
    const std::string retail = "Data\\TGL/128\\WRONG.TGA";
    const auto key = fst_normalize_loose_key(retail);
    CHECK(key == "data/tgl/128/atlas.tga");
    CHECK(elfHash(key.c_str()) == elfHash("data/tgl/128/atlas.tga"));
    CHECK(retail == "Data\\TGL/128\\ATLAS.TGA");
}

TEST_CASE("missing loose asset falls back but same-size mod edits keep priority") {
    ModFixture fixture;
    const std::vector<unsigned char> stock{'s','t','o','c','k'};
    const auto path = fixture.write("atlas.tgl", "stock");
    CHECK_FALSE(looseAssetDiffers(path, stock));
    fixture.write("atlas.tgl", "stock");
    CHECK(looseAssetDiffers(path, stock));
    std::filesystem::remove(path);
    CHECK_FALSE(looseAssetDiffers(path, stock));
}

TEST_CASE("invalid override merge preserves stock fallback and earlier valid records") {
    ModFixture fixture;
    ModelOverrideRegistry registry;
    REQUIRE(registry.loadFromFile(fixture.write("good.json", model), "kit") == 1);
    auto invalid = std::string(model);
    invalid.replace(invalid.find("\"stock\""), 7, "\"stock\"");
    CHECK(registry.mergeFromFile(fixture.write("invalid.json", invalid), "campaign") == 0);
    REQUIRE(registry.count() == 1);
    CHECK(registry.resolve("staticProp", "depot")->manifestDir == "kit");
    CHECK(registry.resolve("staticProp", "missing") == nullptr);
}

TEST_CASE("mod save data round trips and mismatched data preserves live document") {
    using namespace mc2::save;
    Document original;
    original.header.releaseLine.bytes = "mod-unit";
    original.header.originBuildSha.bytes = std::string(40, 'a');
    original.header.dataFingerprint[0] = 42;
    for (std::uint32_t tag = 1; tag <= 15; ++tag)
        original.sections.push_back({tag, tag != 12, Bytes{7}});
    Bytes bytes;
    REQUIRE(encode(original, bytes));
    Compatibility compatible{"mod-unit", original.header.dataFingerprint, original.header.simRulesFingerprint};
    Document restored;
    REQUIRE(decode(bytes, compatible, restored));
    CHECK(restored.sections == original.sections);
    const auto before = restored;
    compatible.dataFingerprint[0] = 42; // deliberately unchanged for red proof
    std::vector<std::string> logs;
    CHECK_FALSE(decode(bytes, compatible, restored, [&](std::string_view line) { logs.emplace_back(line); }));
    CHECK(restored == before);
    CHECK(logs == std::vector<std::string>{"[SAVE v1] reject reason=fingerprint"});
}
}
