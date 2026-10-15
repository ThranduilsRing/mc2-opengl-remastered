#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "CommandHistory.h"
#include "inifile.h"
#include "objective_text.h"
#include <chrono>
#include <fstream>

namespace {
std::string bytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}

void checkGameScalarReads()
{
    for (const auto declaration : {"l ResourcePoints = 500", "ul ResourcePoints = -55000"}) {
        const auto fit = std::string("FITini\r\n[Objective0]\r\n") + declaration + "\r\n[End]\r\nul End = 0\r\nFITend\r\n";
        FitIniFile reader; REQUIRE(reader.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
        REQUIRE(reader.seekBlock("Objective0") == NO_ERR);
        unsigned long value{};
        const auto result = reader.readIdULong("ResourcePoints", value);
        if (declaration[0] == 'l') CHECK(result != NO_ERR);
        else { CHECK(result == NO_ERR); CHECK(static_cast<std::int32_t>(value) == -55000); }
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
        else if (spec.prefix == "l") { long value{}; REQUIRE(reader.readIdLong(key.c_str(), value) == NO_ERR); CHECK(value == 17); }
        else if (spec.prefix == "f") { float value{}; REQUIRE(reader.readIdFloat(key.c_str(), value) == NO_ERR); CHECK(value == 123.25f); }
        else if (spec.prefix == "b") {
            bool value{}; REQUIRE(reader.readIdBoolean(key.c_str(), value) == NO_ERR);
            CHECK(value == !std::get<bool>(doc->objectiveScalar(doc->objectives()[1].section, spec.key)->value));
        } else { REQUIRE(objective_text::read(reader, key.c_str())); }
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
