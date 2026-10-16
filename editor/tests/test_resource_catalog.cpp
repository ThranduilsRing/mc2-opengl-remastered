#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../tests/unit/doctest.h"
#include "ResourceStrings.h"
#include "utilities.h"
#include "gameos_resource.hpp"
#include "MC2Strings.h"
#include "utils/mc2log.h"

#include <chrono>
#include <fstream>
#include <cstdio>

using namespace editor::document;

namespace {
// The fixture owns the mod enumeration borrowed by the real FIT provider.
const FileResolver* modFiles{};
void write(const std::filesystem::path& path, std::string_view value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << value;
}
}

void EnumerateModFitFiles(const char* prefix, void (*callback)(const char*, void*), void* context)
{
    if (!modFiles) return;
    for (const auto& key : modFiles->keys(prefix)) {
        auto file = modFiles->read(key);
        if (file && file->layer.starts_with("mod:") && key.ends_with(".fit"))
            callback(file->source.string().c_str(), context);
    }
}

TEST_CASE("resource catalog matches real provider precedence and keeps full text and missing IDs")
{
    const auto previous = std::filesystem::current_path();
    const auto root = previous / "scratch/resource-catalog" / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string longText = "<missing-string:legacy:70000>" + std::string(65536, 'M') + "\n\n";
    write(root / "system.cfg", "FITini\n[FastFiles]\nl NumFastFiles = 0\nFITend\n");
    write(root / "data/defs/text/en_us/a.fit",
        "String {\n key = \"base\"\n text = \"Base\"\n legacyId = 70001\n source = \"mc2res\"\n}\n"
        "String {\n key = \"direct\"\n text = \"Direct\"\n legacyId = 70002\n source = \" mc2res \"\n}\n"
        "String {\n key = \"duplicate\"\n text = \"First key\"\n legacyId = 70003\n}\n"
        "String {\n key = \"empty\"\n text = \"\"\n value = \"Wrong fallback\"\n legacyId = 70004\n}\n");
    write(root / "mods/active/data/defs/text/en_us/a.fit",
        "String {\n key = \"duplicate\"\n text = \"Wrong duplicate\"\n legacyId = 70003\n source = \"mc2res\"\n}\n"
        "String {\n key = \"mod\"\n text = \"Mod direct\"\n legacyId = 70002\n}\n"
        "LegacyTextAlias {\n legacyId = 70001\n textKey = \"mod\"\n}\n");
    write(root / "mods/active/data/defs/text/en_us/nested/z.fit",
        "String {\n key = \"long\"\n text = \"" + longText + "\"\n legacyId = 70000\n}\n"
        "LegacyTextAlias {\n legacyId = 70001\n textKey = \"long\"\n}\n"
        "LegacyTextAlias {\n legacyId = 70002\n textKey = \"absent\"\n}\n");
    auto resolver = FileResolver::mount({root, {}, "active"}); REQUIRE(resolver);
    auto catalog = ResourceStrings::load(*resolver);
    modFiles = &*resolver;
    struct Restore {
        std::filesystem::path path;
        ~Restore() { modFiles = nullptr; std::filesystem::current_path(path); }
    } restore{previous};
    std::filesystem::current_path(root);
    const auto handle = gos_OpenResourceDLL("mc2res.dll", nullptr, 0); REQUIRE(handle);
    struct Close { HSTRRES handle; ~Close() { gos_CloseResourceDLL(handle); } } close{handle};
    for (const auto id : {70000, 70001, 70002, 70003, 70004, 999999}) {
        CAPTURE(id);
        const auto entry = catalog.resolve(id);
        const auto length = gos_ResourceStringLength(handle, id);
        CHECK(entry.id == id);
        CHECK(entry.found == length.has_value());
        if (length) {
            CHECK(entry.text == gos_GetResourceString(handle, id));
            CHECK(entry.text == loadResourceString(id, handle));
        }
    }
    CHECK(catalog.resolve(70001).text == longText);
    CHECK(catalog.resolve(70002).text == "Mod direct");
    CHECK(catalog.resolve(70003).text == "First key");
    CHECK_FALSE(catalog.resolve(70004).found);
    CHECK(catalog.resolve(999999).id == 999999);
    CHECK(catalog.search("70000").size() == 2);
    CHECK(catalog.search("MOD DIRECT").size() == 1);
    CHECK(catalog.search("nested/z.fit").size() == 2);
    CHECK(catalog.resolve(70001).aliasSource->layer == "mod:active");
    CHECK(catalog.resolve(70003).source.layer == "base-loose");
    CHECK(std::string(gos_GetResourceString(handle, 999999)).starts_with("<missing-string:"));
    auto* log = std::tmpfile(); REQUIRE(log);
    mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, log);
    char output[64] = "stale";
    CHECK(cLoadString(999999, output, sizeof output, handle) == 0);
    CHECK(output[0] == '\0');
    std::rewind(log);
    char diagnostic[256]{};
    const auto count = std::fread(diagnostic, 1, sizeof diagnostic, log);
    CHECK(std::string(diagnostic, count) == "[RESOURCE_STRING v1] event=missing_id id=999999 source_length=0 capacity=64 copied=0\n");
    mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
    std::fclose(log);
}
