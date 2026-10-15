#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "CommandHistory.h"
#include "inifile.h"
#include "objective_text.h"
#include <chrono>
#include <fstream>
#include <tuple>

namespace {
std::string bytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}

void checkGameScalarReads()
{
    for (const auto key : {"MoverWID", "BuildingWID"}) for (const auto declaration : {"l", "ul", "st"}) {
        const auto fit = std::string("FITini\r\n[Reference]\r\n") + declaration + " " + key +
            " = 7\r\nf PositionX = malformed\r\n[End]\r\nul End = 0\r\nFITend\r\n";
        FitIniFile reader; REQUIRE(reader.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
        REQUIRE(reader.seekBlock("Reference") == NO_ERR);
        long wid{};
        CHECK((reader.readIdLong(key, wid) == NO_ERR) == (declaration == std::string_view("l")));
    }
    for (const auto& [wire, expected] : std::vector<std::pair<std::string, long>>{
        {"0xffffffff", -1}, {"0x80000000", -2147483647L - 1}, {"0Xffffffff", 0}}) {
        const auto fit = "FITini\r\n[Objective0]\r\nl BaseColor = " + wire + "\r\n[End]\r\nul End = 0\r\nFITend\r\n";
        FitIniFile reader; REQUIRE(reader.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
        REQUIRE(reader.seekBlock("Objective0") == NO_ERR);
        long value{}; REQUIRE(reader.readIdLong("BaseColor", value) == NO_ERR); CHECK(value == expected);
    }
    for (const auto declaration : {"l ResourcePoints = 500", "ul ResourcePoints = -55000", "ul ResourcePoints = 2147483648"}) {
        const auto fit = std::string("FITini\r\n[Objective0]\r\n") + declaration + "\r\n[End]\r\nul End = 0\r\nFITend\r\n";
        FitIniFile reader; REQUIRE(reader.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
        REQUIRE(reader.seekBlock("Objective0") == NO_ERR);
        unsigned long value{};
        const auto result = reader.readIdULong("ResourcePoints", value);
        if (declaration[0] == 'l') CHECK(result != NO_ERR);
        else { CHECK(result == NO_ERR); CHECK(static_cast<std::int32_t>(value) == (std::string_view(declaration).find("-55000") != std::string_view::npos ? -55000 : 2147483647)); }
    }
}
std::string memoryText(const std::string& file, const char* key)
{
    FitIniFile reader;
    REQUIRE(reader.open(file.data(), static_cast<int>(file.size())) == NO_ERR);
    REQUIRE(reader.seekBlock("Team0Objective1") == NO_ERR);
    auto value = objective_text::read(reader, key); REQUIRE(value);
    return *value;
}
std::string diskText(const std::filesystem::path& path, const char* key)
{
    FitIniFile reader;
    REQUIRE(reader.open(path.string().c_str()) == NO_ERR);
    REQUIRE(reader.seekBlock("Team0Objective1") == NO_ERR);
    auto value = objective_text::read(reader, key); REQUIRE(value);
    return *value;
}

void checkNumericObjectiveRead(std::string_view wire, bool species, long expected)
{
    using namespace editor::document;
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    auto base = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(base);
    CommandHistory setup(*base);
    const auto entry = setup.addObjectiveEntry(base->objectives()[1].section, 0); REQUIRE(entry);
    const auto block = base->section(*entry)->name;
    auto fit = base->fitBytes();
    const auto* id = base->find(*entry, "FlagID"); REQUIRE(id);
    const auto idStart = fit.rfind('\n', id->syntax.begin) + 1;
    const auto idEnd = fit.find('\n', id->syntax.end);
    fit.replace(idStart, idEnd - idStart, "ul FlagIndex = " + std::string(species ? "7" : wire) + "\r");
    const auto* source = base->find(*entry, "ConditionSpeciesString"); REQUIRE(source);
    const auto start = fit.rfind('\n', source->syntax.begin) + 1;
    const auto end = fit.find('\n', source->syntax.end);
    const auto key = species ? "ConditionSpecies" : "FlagIndex";
    if (species) fit.replace(start, end - start, std::string("ul ") + key + " = " + std::string(wire) + "\r");
    FitIniFile reader; REQUIRE(reader.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
    REQUIRE(reader.seekBlock(block.c_str()) == NO_ERR);
    unsigned long actual{}; REQUIRE(reader.readIdULong(key, actual) == NO_ERR);
    CHECK(static_cast<std::int32_t>(actual) == expected);
    auto doc = MissionDocument::decode(fit, {base->pakBytes().begin(), base->pakBytes().end()}, "numeric-reader", {}, base->markerObjects()); REQUIRE(doc);
    const auto& decoded = doc->objectives()[1].entries[0].back();
    if (species) CHECK(std::get<ConditionSpecies>(decoded.species) == static_cast<ConditionSpecies>(actual));
    else {
        REQUIRE(decoded.flag);
        CHECK(decoded.flag->id == std::optional{std::to_string(static_cast<std::int32_t>(actual))});
        CommandHistory history(*doc);
        REQUIRE(history.setObjectiveFlag(decoded.section, std::nullopt, 9, true));
        const auto changed = doc->fitBytes();
        FitIniFile edited; REQUIRE(edited.open(changed.data(), static_cast<int>(changed.size())) == NO_ERR);
        REQUIRE(edited.seekBlock(block.c_str()) == NO_ERR);
        REQUIRE(edited.readIdULong("FlagIndex", actual) == NO_ERR); CHECK(actual == 9);
        CHECK(doc->objectives()[1].entries[0].back().flag->id == std::optional<std::string>{"9"});
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == changed);
    }
}
}

TEST_CASE("reader/hex in a FlagIndex comment supplies the ID and is removed on edit")
{
    checkNumericObjectiveRead("7 // 0xff", false, 255);
}
TEST_CASE("reader/a sign after the FlagIndex hex prefix yields zero")
{
    checkNumericObjectiveRead("0x-1", false, 0);
}
TEST_CASE("reader/uppercase numeric species reads DestroyAllEnemyUnits")
{
    checkNumericObjectiveRead("0X16", true, 0);
}

TEST_CASE("reader/owned multiline edits match the real disk and RAM objective readers")
{
    using namespace editor::document;
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    const auto output = std::filesystem::path("scratch/e6a-reader-tests") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(output);
    auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
    CommandHistory history(*doc);
    const auto id = doc->objectives()[1].section;
    for (const auto* key : {"Title", "Description", "ActivateFlagID", "ResetStatusFlagID", "ObjectiveModel"}) {
        for (const auto& [authored, logical, runtime] : std::vector<std::array<std::string, 3>>{
            {"one\ntwo", "one\ntwo", "one\r\ntwo"},
            {"one\r\ntwo", "one\ntwo", "one\r\ntwo"},
            {"\nfirst\n\nlast\n", "\nfirst\n\nlast\n", "\r\nfirst\r\n\r\nlast\r\n"},
            {"literal \\n stays", "literal \\n stays", "literal \\n stays"},
            {std::string(1998, 'x') + "\nz", std::string(1998, 'x') + "\nz", std::string(1998, 'x') + "\r\nz"}}) {
            CAPTURE(key);
            CAPTURE(authored.size());
            const auto original = doc->fitBytes();
            REQUIRE(history.setObjectiveScalar(id, key, authored));
            CHECK(std::get<std::string>(doc->objectiveScalar(id, key)->value) == logical);
            REQUIRE(doc->save(output / "edited", {}));
            CHECK(memoryText(doc->fitBytes(), key) == runtime);
            CHECK(diskText(output / "edited.fit", key) == runtime);
            auto reopened = MissionDocument::read(output / "edited.fit", output / "edited.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(reopened);
            CHECK(reopened->objectiveScalar(reopened->objectives()[1].section, key)->value == Scalar{logical});
            const auto edited = doc->fitBytes();
            REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
            REQUIRE(history.redo()); CHECK(doc->fitBytes() == edited);
            REQUIRE(history.undo());
        }
    }
    for (const auto* key : {"Title", "Description"}) {
        const auto path = widgets / (std::string("objective-") + key + "-edited.fit");
        CHECK(memoryText(bytes(path), key) == "E6a objective\r\n\r\nsecond line\r\n");
        CHECK(diskText(path, key) == "E6a objective\r\n\r\nsecond line\r\n");
    }
}

TEST_CASE("reader/objective prefixes and imported multiline semantics match FitIniFile")
{
    checkGameScalarReads();
    using namespace editor::document;
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    const auto original = bytes(widgets / "fixture.fit");
    auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
    for (const auto& spec : objectiveSchema()) {
        CAPTURE(spec.key);
        const auto path = widgets / ("objective-" + std::string(spec.key == "ModelID Version" ? "ModelID_Version" : spec.key) + "-edited.fit");
        FitIniFile reader; REQUIRE(reader.open(path.string().c_str()) == NO_ERR);
        REQUIRE(reader.seekBlock("Team0Objective1") == NO_ERR);
        const auto key = std::string(spec.key);
        if (spec.prefix == "ul") { unsigned long value{}; REQUIRE(reader.readIdULong(key.c_str(), value) == NO_ERR); CHECK(value == (key == "Priority" ? 2UL : 17UL)); }
        else if (spec.prefix == "l") { long value{}; REQUIRE(reader.readIdLong(key.c_str(), value) == NO_ERR); CHECK(value == (key == "ModelType" ? 13 : 17)); }
        else if (spec.prefix == "f") { float value{}; REQUIRE(reader.readIdFloat(key.c_str(), value) == NO_ERR); CHECK(value == 123.25f); }
        else if (spec.prefix == "b") {
            bool value{}; REQUIRE(reader.readIdBoolean(key.c_str(), value) == NO_ERR);
            CHECK(value == !std::get<bool>(doc->objectiveScalar(doc->objectives()[1].section, spec.key)->value));
        } else { REQUIRE(objective_text::read(reader, key.c_str())); }
    }
    for (const auto key : {"ResourcePoints", "MarkerX", "ModelScale"}) for (const auto wire : {"7 // 0xff", "0X16", "0x-1", "0.1"}) {
        CAPTURE(key, wire);
        const auto* field = doc->find(doc->objectives()[1].section, key); REQUIRE(field);
        auto variant = original;
        variant.replace(field->syntax.begin, field->syntax.end - field->syntax.begin, wire);
        FitIniFile game; REQUIRE(game.open(variant.data(), static_cast<int>(variant.size())) == NO_ERR);
        REQUIRE(game.seekBlock("Team0Objective1") == NO_ERR);
        Scalar expected;
        if (field->prefix == "ul") {
            unsigned long value{}; REQUIRE(game.readIdULong(key, value) == NO_ERR);
            expected = static_cast<std::int64_t>(static_cast<std::int32_t>(value));
        } else {
            float value{}; REQUIRE(game.readIdFloat(key, value) == NO_ERR); expected = static_cast<double>(value);
        }
        auto imported = MissionDocument::decode(variant, {doc->pakBytes().begin(), doc->pakBytes().end()}, "scalar-reader", {}, doc->markerObjects()); REQUIRE(imported);
        CHECK(imported->objectiveScalar(imported->objectives()[1].section, key)->value == expected);
        CHECK(imported->fitBytes() == variant);
    }
    auto wrong = original;
    const auto position = wrong.find("ul Priority"); REQUIRE(position != wrong.npos);
    wrong.replace(position, 2, "l");
    FitIniFile reader; REQUIRE(reader.open(wrong.data(), static_cast<int>(wrong.size())) == NO_ERR);
    REQUIRE(reader.seekBlock("Team0Objective0") == NO_ERR);
    unsigned long ignored = 123; CHECK(reader.readIdULong("Priority", ignored) != NO_ERR); CHECK(ignored == 0);
    auto multiline = original;
    const auto start = multiline.find("st Title", multiline.find("[Team0Objective1]")); REQUIRE(start != multiline.npos);
    const auto quote = multiline.find('"', start), end = multiline.find('"', quote + 1); REQUIRE(end != multiline.npos);
    multiline.replace(quote + 1, end - quote - 1, "first\r\nsecond\r\nthird");
    CHECK(memoryText(multiline, "Title") == "firstsecond\r\nthird");
    std::ifstream pak(widgets / "fixture.pak", std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(pak), {}};
    const auto span = std::as_bytes(std::span(raw));
    auto imported = MissionDocument::decode(multiline, {span.begin(), span.end()}, "multiline", {}, editor::tests::inputs().markerObjects); REQUIRE(imported);
    CHECK(imported->fitBytes() == multiline);
    CHECK(imported->objectiveScalar(imported->objectives()[1].section, "Title")->value == Scalar{std::string("firstsecond\nthird")});
}

TEST_CASE("reader/exported four-list boolean payloads obey ID precedence and defaults on disk and RAM")
{
    using namespace editor::document;
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
    CommandHistory history(*doc);
    const auto owner = doc->objectives()[1].section;
    const auto output = std::filesystem::path("scratch/e6c-reader-tests") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(output);
    const auto check = [](FitIniFile& reader, const std::string& block, std::optional<std::string> id, bool expected) {
        REQUIRE(reader.seekBlock(block.c_str()) == NO_ERR);
        CHECK(objective_text::readFlagID(reader, "FlagID", "FlagIndex") == id);
        bool value = true, temporary{};
        if (reader.readIdBoolean("Value", temporary) == NO_ERR) value = temporary;
        CHECK(value == expected);
    };
    for (std::size_t list = 0; list < 4; ++list) {
        const auto entry = history.addObjectiveEntry(owner, list); REQUIRE(entry);
        REQUIRE(history.setObjectiveFlag(*entry, std::string("flag\nsecond"), 0, false));
        const auto block = doc->section(*entry)->name;
        REQUIRE(doc->save(output / "exported", {}));
        const auto fit = bytes(output / "exported.fit");
        FitIniFile ram; REQUIRE(ram.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
        FitIniFile disk; REQUIRE(disk.open((output / "exported.fit").string().c_str()) == NO_ERR);
        check(ram, block, "flag\r\nsecond", false); check(disk, block, "flag\r\nsecond", false);
        for (const auto& [payload, expected, state] : std::vector<std::tuple<std::string, std::optional<std::string>, bool>>{
            {"st FlagID = \"\"\r\nul FlagIndex = malformed\r\n", std::string{}, true},
            {"st FlagID = \"wins\"\r\nul FlagIndex = 9223372036854775808\r\nb Value = FALSE\r\n", "wins", false},
            {"l FlagID = 7\r\nul FlagIndex = -2147483648\r\n", "-2147483648", true},
            {"st FlagID = \"" + std::string(2001, 'x') + "\"\r\nul FlagIndex = 2147483647\r\n", "2147483647", true},
            {"ul FlagIndex = -1\r\n", "-1", true},
            {"ul FlagIndex = 0xffffffff\r\n", "-1", true},
            {"ul FlagIndex = 2147483648\r\n", "2147483647", true},
            {"ul FlagIndex = malformed\r\nb Value = falsehood\r\n", "0", false},
            {"l FlagIndex = 7\r\nl Value = 0\r\n", std::nullopt, true}}) {
            auto variant = fit;
            const auto start = variant.find("st FlagID", variant.find("[" + block + "]")); REQUIRE(start != variant.npos);
            const auto end = variant.find('\n', variant.find("b Value", start)); REQUIRE(end != variant.npos);
            variant.replace(start, end + 1 - start, payload);
            std::ofstream(output / "variant.fit", std::ios::binary) << variant;
            FitIniFile memory; REQUIRE(memory.open(variant.data(), static_cast<int>(variant.size())) == NO_ERR);
            FitIniFile file; REQUIRE(file.open((output / "variant.fit").string().c_str()) == NO_ERR);
            check(memory, block, expected, state); check(file, block, expected, state);
            std::vector<std::byte> pak(doc->pakBytes().begin(), doc->pakBytes().end());
            auto imported = MissionDocument::decode(variant, std::move(pak), "reader-flag", {}, doc->markerObjects()); REQUIRE(imported);
            const auto& flag = *imported->objectives()[1].entries[list].back().flag;
            CHECK(flag.id == expected); CHECK(flag.value == state); CHECK(imported->fitBytes() == variant);
        }
        auto mutation = fit; const auto at = mutation.find("flag\r\nsecond"); REQUIRE(at != mutation.npos);
        mutation.replace(at, 4, "wrong");
        FitIniFile wrong; REQUIRE(wrong.open(mutation.data(), static_cast<int>(mutation.size())) == NO_ERR);
        REQUIRE(wrong.seekBlock(block.c_str()) == NO_ERR);
        CHECK(objective_text::readFlagID(wrong, "FlagID", "FlagIndex") != std::optional<std::string>{"flag\r\nsecond"});
        REQUIRE(history.undo()); REQUIRE(history.undo());
    }
}
