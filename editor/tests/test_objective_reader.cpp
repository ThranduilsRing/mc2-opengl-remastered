#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "CommandHistory.h"
#include "inifile.h"
#include "objective_text.h"
#include "objective_flags.h"
#include <chrono>
#include <fstream>
#include <tuple>
#include <cmath>
#include <limits>
#include <bit>

extern bool g_mc2TestStopFatal;
namespace { std::string bytes(const std::filesystem::path& path); }

TEST_CASE("reader/team flag gaps duplicates oversized IDs and default values match the runtime table")
{
    using namespace editor::document;
    const auto& fixtures = editor::tests::inputs().byteGradeDirectory;
    auto base = MissionDocument::read(fixtures / "fixture.fit", fixtures / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(base);
    auto fit = base->fitBytes();
    if (const auto table = fit.find("[Team0BooleanFlags]"); table != fit.npos) {
        const auto end = fit.find('[', table + 1);
        fit.erase(table, (end == fit.npos ? fit.find("FITend", table) : end) - table);
    }
    const auto huge = std::string(2001, 'x');
    fit.insert(fit.find("FITend"),
        "[Team0BooleanFlags]\r\nst BooleanString0 = \"Alpha\"\r\nb BooleanValue0 = TRUE\r\n"
        "st BooleanString1 = \"aLPHA\"\r\nb BooleanValue1 = FALSE\r\n"
        "st BooleanString2 = \"" + huge + "\"\r\nb BooleanValue2 = TRUE\r\n"
        "st BooleanString3 = \"missing-value\"\r\n"
        "st BooleanString4 = \"wrong-value-type\"\r\nl BooleanValue4 = 1\r\n"
        "st BooleanString5 = \"noncanonical\"\r\nb BooleanValue5 = yes\r\n"
        "st BooleanString6 = \"\r\nline\r\nflag\"\r\nb BooleanValue6 = TRUE\r\n"
        "b BooleanValue7 = TRUE\r\n"
        "st BooleanString8 = \"hidden\"\r\nb BooleanValue8 = TRUE\r\n"
        "[Team1BooleanFlags]\r\nst BooleanString0 = \"ALPHA\"\r\nb BooleanValue0 = FALSE\r\n");
    const auto output = std::filesystem::path("scratch/e6c-team-flags") / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(output);
    const auto check = [&](const std::string& source, const char* key, bool expected) {
        auto doc = MissionDocument::decode(source, {base->pakBytes().begin(), base->pakBytes().end()}, "flags", {}, base->markerObjects()); REQUIRE(doc);
        REQUIRE(doc->save(output / "flags", {}));
        CHECK(bytes(output / "flags.fit") == source);
        CHECK(bytes(output / "flags.pak") == bytes(fixtures / "fixture.pak"));
        for (const bool disk : {false, true}) {
            FitIniFile reader;
            REQUIRE((disk ? reader.open((output / "flags.fit").string().c_str()) : reader.open(source.data(), static_cast<int>(source.size()))) == NO_ERR);
            CBooleanArray game; game.load(0, &reader);
            CHECK(game.getElementValue(key) == expected);
            const auto flags = doc->teamFlags(0);
            const auto* resolved = flags.resolve(key);
            CHECK((resolved && resolved->value) == expected);
            for (const auto& [id, value] : std::vector<std::pair<std::string, bool>>{
                {"aLpHa", source.substr(source.find("BooleanValue0 = "), 20) == "BooleanValue0 = TRUE"},
                {"wrong-value-type", false}, {"noncanonical", true}, {"line\r\nflag", true}, {"absent", false}, {huge, false}}) {
                CAPTURE(id);
                CHECK(game.getElementValue(id) == value);
                const auto* row = flags.resolve(id); CHECK((row && row->value) == value);
            }
            game.setElementValue("AlPhA", false); CHECK_FALSE(game.getElementValue("alpha"));
            CBooleanArray other; other.load(1, &reader); CHECK_FALSE(other.getElementValue("alpha"));
            CBooleanArray missing; missing.load(2, &reader); CHECK_FALSE(missing.getElementValue("alpha"));
        }
        return doc->teamFlags(0);
    };
    const auto flags = check(fit, "hidden", false);
    REQUIRE(flags.rows.size() == 9); CHECK(flags.stop == 7);
    CHECK(flags.rows[1].duplicateOf == std::optional<std::size_t>{0});
    CHECK(flags.rows[2].status == "oversized ID skipped");
    CHECK(flags.rows[7].status == "absent/unreadable ID ends loading");
    CHECK(flags.rows[8].status == "ignored tail after first absent ID");
    CHECK_FALSE(flags.resolve("missing-value")->value);
    auto changed = fit;
    changed.insert(changed.find("b BooleanValue7"), "st BooleanString7 = \"gap-filled\"\r\n");
    CHECK(check(changed, "hidden", true).stop == 9);
    changed = fit; changed.replace(changed.find(huge), huge.size(), "short");
    CHECK(check(changed, "short", true).rows[2].status == "initialized");
    changed = fit; changed.replace(changed.find("BooleanValue0 = TRUE"), 20, "BooleanValue0 = FALSE");
    check(changed, "alpha", false);
    changed = fit; changed.insert(changed.find("st BooleanString4"), "b BooleanValue3 = TRUE\r\n");
    check(changed, "missing-value", true);
    changed = fit; changed.replace(changed.find("st BooleanString2"), 2, "st[1]");
    auto array = MissionDocument::decode(changed, {base->pakBytes().begin(), base->pakBytes().end()}, "array-flags", {}, base->markerObjects()); REQUIRE(array);
    CHECK(array->teamFlags(0).stop == 2); CHECK_FALSE(array->teamFlags(0).resolve("noncanonical"));
    FitIniFile reader; REQUIRE(reader.open(changed.data(), static_cast<int>(changed.size())) == NO_ERR);
    CBooleanArray game; game.load(0, &reader); CHECK_FALSE(game.getElementValue("noncanonical"));
}

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

TEST_CASE("reader/condition fields match decimal saturation hex wrap rounding and underflow on disk and RAM")
{
    using namespace editor::document;
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    const auto output = std::filesystem::path("scratch/e6c-payload-reader") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(output);
    for (const auto list : {0U, 2U}) for (const auto& spec : conditionSchema()) {
        auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
        CommandHistory history(*doc);
        auto entry = history.addObjectiveCondition(doc->objectives()[1].section, list, spec.species);
        std::size_t targetIndex{};
        if (spec.reference) {
            CHECK_FALSE(entry);
            const auto before = doc->fitBytes();
            for (const auto& entity : doc->entities()) {
                if (entity.exportedPart.has_value() == spec.name.ends_with("Structure")) continue;
                const auto index = entity.exportedPart ? *entity.exportedPart - 1 : *entity.packetRecord;
                entry = history.addObjectiveCondition(doc->objectives()[1].section, list, spec.species, entity.id);
                if (entry) { targetIndex = index; break; }
                CHECK(doc->fitBytes() == before);
            }
        }
        REQUIRE(entry);
        const auto block = doc->section(*entry)->name;
        REQUIRE(doc->save(output / "created", {}));
        const auto fit = bytes(output / "created.fit");
        FitIniFile game; REQUIRE(game.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
        const auto owner = doc->section(doc->objectives()[1].section)->name;
        REQUIRE(game.seekBlock(owner.c_str()) == NO_ERR);
        unsigned long count{};
        REQUIRE(game.readIdULong(list == 0 ? "NumConditions" : "NumFailureConditions", count) == NO_ERR);
        REQUIRE(count > 0);
        CHECK(block == owner + (list == 0 ? "Condition" : "FailureCondition") + std::to_string(count - 1));
        REQUIRE(game.seekBlock(block.c_str()) == NO_ERR);
        const auto name = objective_text::read(game, list == 0 ? "ConditionSpeciesString" : "FailureConditionSpeciesString");
        REQUIRE(name); CHECK(*name == spec.name);
        if (name->find("Specific") != name->npos) {
            float x{}, y{};
            REQUIRE(game.readIdFloat("PositionX", x) == NO_ERR); REQUIRE(game.readIdFloat("PositionY", y) == NO_ERR);
            const bool building = name->ends_with("Structure");
            std::vector<std::tuple<float, float, int>> candidates;
            if (building) {
                const auto raw = bytes(output / "created.pak"); const auto span = std::as_bytes(std::span(raw));
                const auto archive = decodePak({span.begin(), span.end()}); REQUIRE(archive);
                const auto& data = archive->packets[1].decoded;
                const auto word = [&](std::size_t offset) {
                    std::uint32_t value{};
                    for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(data[offset + i]) << (8 * i);
                    return value;
                };
                for (std::size_t offset = 4; offset + 40 <= data.size(); offset += 40)
                    candidates.emplace_back(std::bit_cast<float>(word(offset + 4)), std::bit_cast<float>(word(offset + 8)), static_cast<int>(word(offset + 24)));
            } else {
                REQUIRE(game.seekBlock("Parts") == NO_ERR); unsigned long count{}; REQUIRE(game.readIdULong("NumParts", count) == NO_ERR);
                for (unsigned long part = 1; part <= count; ++part) {
                    REQUIRE(game.seekBlock(("Part" + std::to_string(part)).c_str()) == NO_ERR);
                    float px{}, py{}; char team{};
                    REQUIRE(game.readIdFloat("PositionX", px) == NO_ERR); REQUIRE(game.readIdFloat("PositionY", py) == NO_ERR);
                    REQUIRE(game.readIdChar("TeamID", team) == NO_ERR); candidates.emplace_back(px, py, team);
                }
            }
            const auto matches = [&](float cx, float cy) {
                std::size_t count{};
                for (const auto& [px, py, team] : candidates) {
                    if (!building && ((*name == "GuardSpecificUnit" && team != 0) ||
                        (name->starts_with("Capture") && team == 0))) continue;
                    const auto close = [](float a, float b) { const auto scale = std::max(std::abs(a), std::abs(b)); return scale == 0 || std::abs(a - b) / scale < 0.00001; };
                    count += close(px, cx) && close(py, cy);
                }
                return count;
            };
            REQUIRE(targetIndex < candidates.size());
            CHECK(x == std::get<0>(candidates[targetIndex])); CHECK(y == std::get<1>(candidates[targetIndex]));
            CHECK(matches(x, y) == 1); CHECK(matches(-7654321, -8765432) == 0);
        } else if (name->find("NumberOf") != name->npos) {
            unsigned long count{}; REQUIRE(game.readIdULong("Num", count) == NO_ERR); CHECK(count == 0);
        } else if (*name == "ElapsedMissionTime") {
            float time{}; REQUIRE(game.readIdFloat("Time", time) == NO_ERR); CHECK(time == 0);
            for (const double number : {0.1, 2.3, 1234.56, 1e-50}) {
                REQUIRE(history.setObjectivePayload(*entry, "Time", number));
                REQUIRE(doc->save(output / "decimal", {}));
                FitIniFile decimal; REQUIRE(decimal.open((output / "decimal.fit").string().c_str()) == NO_ERR);
                REQUIRE(decimal.seekBlock(block.c_str()) == NO_ERR);
                REQUIRE(decimal.readIdFloat("Time", time) == NO_ERR); CHECK(time == static_cast<float>(number));
            }
        } else if (name->starts_with("Move")) {
            for (const auto key : {"TargetCenterX", "TargetCenterY", "TargetRadius"}) { float number{}; REQUIRE(game.readIdFloat(key, number) == NO_ERR); CHECK(number == 0); }
        } else CHECK(name->ends_with("AllEnemyUnits"));
    }
    for (const auto list : {0U, 2U}) for (const auto& spec : conditionSchema()) {
        if (spec.reference) continue;
        auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
        CommandHistory history(*doc);
        const auto entry = history.addObjectiveCondition(doc->objectives()[1].section, list, spec.species); REQUIRE(entry);
        const auto block = doc->section(*entry)->name;
        for (const auto& field : spec.fields) {
            const std::vector<std::pair<std::string, Scalar>> cases = field.type == 0 ?
                std::vector<std::pair<std::string, Scalar>>{
                    {"-2147483648", std::int64_t{-2147483648LL}}, {"2147483647", std::int64_t{2147483647}},
                    {"2147483648", std::int64_t{2147483647}}, {"-2147483649", std::int64_t{-2147483648LL}},
                    {"9223372036854775808", std::int64_t{2147483647}},
                    {"0xffffffff", std::int64_t{-1}}, {"0x80000000", std::int64_t{-2147483648LL}},
                    {"0Xffffffff", std::int64_t{0}}} :
                std::vector<std::pair<std::string, Scalar>>{
                    {"-1.25", -1.25}, {"0", 0.0}, {"-0", -0.0},
                    {"0.1", 0.100000001490116119384765625}, {"16777217", 16777216.0},
                    {"1.401298464324817e-45", static_cast<double>(std::numeric_limits<float>::denorm_min())},
                    {"1e-50", 0.0}, {"-1e-50", -0.0}};
            for (const auto& [wire, expected] : cases) {
                CAPTURE(spec.name, field.key, list, wire);
                auto fit = doc->fitBytes();
                const auto* source = doc->find(*entry, field.key); REQUIRE(source);
                fit.replace(source->syntax.begin, source->syntax.end - source->syntax.begin, wire);
                std::ofstream(output / "variant.fit", std::ios::binary) << fit;
                auto decoded = MissionDocument::decode(fit, {doc->pakBytes().begin(), doc->pakBytes().end()}, "payload-reader", {}, doc->markerObjects()); REQUIRE(decoded);
                const auto id = decoded->objectives()[1].entries[list].back().section;
                CHECK(decoded->find(id, field.key)->value == Value{expected});
                FitIniFile ram; REQUIRE(ram.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
                FitIniFile disk; REQUIRE(disk.open((output / "variant.fit").string().c_str()) == NO_ERR);
                for (auto* reader : {&ram, &disk}) {
                    REQUIRE(reader->seekBlock(block.c_str()) == NO_ERR);
                    if (field.type == 0) {
                        unsigned long actual{}; REQUIRE(reader->readIdULong(field.key.data(), actual) == NO_ERR);
                        CHECK(static_cast<std::int32_t>(actual) == std::get<std::int64_t>(expected));
                    } else {
                        float actual{}; REQUIRE(reader->readIdFloat(field.key.data(), actual) == NO_ERR);
                        CHECK(static_cast<double>(actual) == std::get<double>(expected));
                        CHECK(std::signbit(actual) == std::signbit(std::get<double>(expected)));
                    }
                }
                auto wrongPrefix = fit;
                const auto declaration = wrongPrefix.rfind('\n', source->syntax.begin) + 1;
                wrongPrefix.replace(declaration, field.prefix.size(), field.type == 0 ? "l" : "ul");
                FitIniFile ignored; REQUIRE(ignored.open(wrongPrefix.data(), static_cast<int>(wrongPrefix.size())) == NO_ERR);
                REQUIRE(ignored.seekBlock(block.c_str()) == NO_ERR);
                if (field.type == 0) { unsigned long value{}; CHECK(ignored.readIdULong(field.key.data(), value) != NO_ERR); }
                else { float value{}; CHECK(ignored.readIdFloat(field.key.data(), value) != NO_ERR); }
                CHECK(decoded->fitBytes() == fit);
                std::vector<std::string> logs;
                CommandHistory edited(*decoded, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
                const Scalar next = field.type == 0 ? Scalar{std::int64_t{-17}} : Scalar{-1.25};
                if (decoded->find(id, field.key)->warning.starts_with("outside signed")) {
                    const auto revision = decoded->revision();
                    CHECK_FALSE(edited.setObjectivePayload(id, field.key, next));
                    CHECK(logs.size() == 1); CHECK(decoded->revision() == revision);
                    CHECK(decoded->fitBytes() == fit); CHECK_FALSE(edited.canUndo());
                    REQUIRE(decoded->save(output / "overflow", {}));
                    CHECK(bytes(output / "overflow.fit") == fit);
                    continue;
                }
                REQUIRE(edited.setObjectivePayload(id, field.key, next));
                REQUIRE(decoded->save(output / "edited", {}));
                FitIniFile exported; REQUIRE(exported.open((output / "edited.fit").string().c_str()) == NO_ERR);
                REQUIRE(exported.seekBlock(block.c_str()) == NO_ERR);
                if (field.type == 0) {
                    unsigned long actual{}; REQUIRE(exported.readIdULong(field.key.data(), actual) == NO_ERR);
                    CHECK(static_cast<std::int32_t>(actual) == -17);
                } else {
                    float actual{}; REQUIRE(exported.readIdFloat(field.key.data(), actual) == NO_ERR); CHECK(actual == -1.25f);
                }
            }
        }
    }
}
TEST_CASE("reader/a sign after the FlagIndex hex prefix yields zero")
{
    checkNumericObjectiveRead("0x-1", false, 0);
}
TEST_CASE("reader/uppercase numeric species reads DestroyAllEnemyUnits")
{
    checkNumericObjectiveRead("0X16", true, 0);
}

TEST_CASE("reader/action text limits and imported quotes have explicit disk and RAM expectations")
{
    using namespace editor::document;
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    const auto output = std::filesystem::path("scratch/e6c-action-reader") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(output);
    for (const auto list : {1U, 3U}) for (const auto& spec : actionSchema()) {
        const auto& field = spec.fields.front();
        const auto block = std::string("Team0Objective1") + (list == 1 ? "Action0" : "FailureAction0");
        const auto key = std::string(field.key);
        const auto path = widgets / (std::format("widget-payload-{}-{}-edited.fit", list, key));
        const auto fit = bytes(path); REQUIRE_FALSE(fit.empty());
        for (const bool disk : {false, true}) {
            FitIniFile reader;
            REQUIRE((disk ? reader.open(path.string().c_str()) : reader.open(fit.data(), static_cast<int>(fit.size()))) == NO_ERR);
            const auto owner = std::string("Team0Objective1");
            REQUIRE(reader.seekBlock(owner.c_str()) == NO_ERR);
            unsigned long count{};
            REQUIRE(reader.readIdULong(list == 1 ? "NumActions" : "NumFailureActions", count) == NO_ERR);
            REQUIRE(count == 5);
            const auto index = static_cast<std::size_t>(&spec - actionSchema().data());
            REQUIRE(reader.seekBlock((owner + (list == 1 ? "Action" : "FailureAction") + std::to_string(index)).c_str()) == NO_ERR);
            if (field.type == 0) {
                unsigned long value{}; REQUIRE(reader.readIdULong(key.c_str(), value) == NO_ERR);
                CHECK(static_cast<std::int32_t>(value) == -17);
            } else CHECK(objective_text::read(reader, key.c_str()) == std::optional<std::string>{"../Exact/Name\r\n\r\nsecond line\r\n"});
        }
        if (field.type != 3) continue;
        auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
        const auto index = static_cast<std::size_t>(&spec - actionSchema().data());
        const auto id = doc->objectives()[1].entries[list][index].section;
        const auto targetBlock = doc->section(id)->name;
        CommandHistory history(*doc);
        for (const auto& [authored, expected] : std::vector<std::pair<std::string, std::string>>{
                {std::string(1999, 'x'), std::string(1999, 'x')}, {std::string(2000, 'x'), std::string(2000, 'x')},
                {std::string(1998, 'x') + "\ny", std::string(1998, 'x') + "\r\ny"},
                {std::string(1998, 'x') + "\r\ny", std::string(1998, 'x') + "\r\ny"},
                {"hello\n ul Count = 1", "hello\r\n ul Count = 1"}}) {
            REQUIRE(history.setObjectivePayload(id, key, authored));
            REQUIRE(doc->save(output / "authored", {}));
            const auto saved = bytes(output / "authored.fit");
            for (const bool disk : {false, true}) {
                FitIniFile reader;
                REQUIRE((disk ? reader.open((output / "authored.fit").string().c_str()) : reader.open(saved.data(), static_cast<int>(saved.size()))) == NO_ERR);
                REQUIRE(reader.seekBlock(targetBlock.c_str()) == NO_ERR);
                CHECK(objective_text::read(reader, key.c_str()) == std::optional{expected});
            }
            REQUIRE(history.undo());
        }
        for (const auto& [wire, expected] : std::vector<std::pair<std::string, std::optional<std::string>>>{
                {"\"" + std::string(2001, 'x') + "\"", std::nullopt},
                {"\"one\"two\"", std::string("one")},
                {"\"one\r\ntwo\"", std::string("onetwo")},
                {"\"one\ntwo\"", std::string("onetwo")},
                {"\"\r\nhello\r\nul Count = 1\"", std::string("hello\r\nul Count = 1")}}) {
            const auto raw = "FITini\r\n[Text]\r\nst " + key + " = " + wire + "\r\n[End]\r\nul End = 0\r\nFITend\r\n";
            const auto rawPath = output / "imported.fit";
            { std::ofstream out(rawPath, std::ios::binary); out << raw; }
            for (const bool disk : {false, true}) {
                FitIniFile reader;
                REQUIRE((disk ? reader.open(rawPath.string().c_str()) : reader.open(raw.data(), static_cast<int>(raw.size()))) == NO_ERR);
                REQUIRE(reader.seekBlock("Text") == NO_ERR);
                CHECK(objective_text::read(reader, key.c_str()) == expected);
            }
            if (expected) {
                auto imported = doc->fitBytes();
                const auto* source = doc->find(id, key); REQUIRE(source);
                imported.replace(source->syntax.begin, source->syntax.end - source->syntax.begin, wire);
                auto decoded = MissionDocument::decode(imported, {doc->pakBytes().begin(), doc->pakBytes().end()}, "imported", {}, editor::tests::inputs().markerObjects);
                REQUIRE(decoded);
                CHECK(decoded->objectives()[1].entries[list][index].payload[0] == Scalar{objective_text::toFileBreaks(*expected)});
                CHECK(decoded->fitBytes() == imported);
            }
        }
    }
    const auto lf = std::string("FITini\n[Text]\nst Message = \"a\n\nb\"\n[End]\nul End = 0\nFITend\n");
    FitIniFile reader; REQUIRE(reader.open(lf.data(), static_cast<int>(lf.size())) == NO_ERR);
    REQUIRE(reader.seekBlock("Text") == NO_ERR);
    CHECK(objective_text::read(reader, "Message") == std::optional<std::string>{"ab"});
}

TEST_CASE("reader/embedded boolean declarations cannot override created or edited action values")
{
    using namespace editor::document;
    const std::string fit = "FITini\r\n[Action]\r\nst FlagID = \"\r\nname\r\nb Value = FALSE\"\r\nb Value = TRUE\r\n[End]\r\nul End = 0\r\nFITend\r\n";
    FitIniFile reader; REQUIRE(reader.open(fit.data(), static_cast<int>(fit.size())) == NO_ERR);
    REQUIRE(reader.seekBlock("Action") == NO_ERR);
    bool value = true; REQUIRE(reader.readIdBoolean("Value", value) == NO_ERR); CHECK_FALSE(value);
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
    std::vector<std::string> logs;
    CommandHistory history(*doc, 32 * 1024 * 1024, [&](const std::string& line) { logs.push_back(line); });
    const auto owner = history.addObjective(0); REQUIRE(owner);
    const auto entry = history.addObjectiveAction(*owner, 1, {ActionSpecies::flag, {std::string("seed"), true}}); REQUIRE(entry);
    const auto before = doc->fitBytes();
    logs.clear();
    CHECK_FALSE(history.addObjectiveAction(*owner, 1, {ActionSpecies::flag, {std::string("name\nb Value = FALSE"), true}}));
    CHECK(logs.size() == 1); logs.clear();
    CHECK_FALSE(history.setObjectiveFlag(*entry, std::string("name\nb Value = FALSE"), 0, true));
    CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before);
}

TEST_CASE("reader/embedded block headers refuse creation and payload edits before FIT opening fails")
{
    using namespace editor::document;
    const std::string fit = "FITini\r\n[Action]\r\nst Message = \"\r\nhello\r\n[unterminated\"\r\n[End]\r\nul End = 0\r\nFITend\r\n";
    FitIniFile reader;
    g_mc2TestStopFatal = false;
    const auto opened = reader.open(fit.data(), static_cast<int>(fit.size()));
    g_mc2TestStopFatal = true;
    CHECK(opened == SYNTAX_ERROR);
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
    std::vector<std::string> logs;
    CommandHistory history(*doc, 32 * 1024 * 1024, [&](const std::string& line) { logs.push_back(line); });
    const auto owner = history.addObjective(0); REQUIRE(owner);
    const auto entry = history.addObjectiveAction(*owner, 1, {ActionSpecies::text, {std::string("seed")}}); REQUIRE(entry);
    const auto before = doc->fitBytes();
    for (const auto* text : {"hello\n[unterminated", "hello\n[Injected]", "hello\nst ActionSpeciesString = bogus"}) {
        logs.clear(); CHECK_FALSE(history.addObjectiveAction(*owner, 1, {ActionSpecies::text, {std::string(text)}}));
        CHECK(logs.size() == 1); logs.clear();
        CHECK_FALSE(history.setObjectivePayload(*entry, "Message", std::string(text)));
        CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before);
    }
}

TEST_CASE("reader/created actions require all six literal payloads in both lists without changing unrelated bytes")
{
    using namespace editor::document;
    struct Expected { ActionSpecies species; const char* name; const char* key; Scalar value; const char* wire; const char* runtime; };
    const Expected cases[] = {
        {ActionSpecies::wav, "PlayWAV", "WAVFileName", std::string("../Audio/cue.wav"), "st WAVFileName = \"../Audio/cue.wav\"\r\n", "../Audio/cue.wav"},
        {ActionSpecies::text, "DisplayTextMessage", "Message", std::string("first\n\nlast\n"), "st Message = \"\r\nfirst\r\n\r\nlast\r\n\"\r\n", "first\r\n\r\nlast\r\n"},
        {ActionSpecies::technology, "MakeNewTechnologyAvailable", "PurchaseFileName", std::string("../Tech/new.fit"), "st PurchaseFileName = \"../Tech/new.fit\"\r\n", "../Tech/new.fit"},
        {ActionSpecies::bik, "PlayBIK", "AVIFileName", std::string("../Video/intro.bik"), "st AVIFileName = \"../Video/intro.bik\"\r\n", "../Video/intro.bik"},
        {ActionSpecies::resourceText, "DisplayResourceTextMessage", "ResourceStringID", std::int64_t{-17}, "ul ResourceStringID = -17\r\n", nullptr},
        {ActionSpecies::flag, "SetBooleanFlag", "FlagID", std::string("created\nflag"), "st FlagID = \"\r\ncreated\r\nflag\"\r\nb Value = FALSE\r\n", "created\r\nflag"}};
    const auto& widgets = editor::tests::inputs().byteGradeDirectory;
    const auto output = std::filesystem::path("scratch/e6c-created-reader") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(output);
    for (const auto list : {1U, 3U}) for (const auto& row : cases) for (const bool index : {false, true}) {
        if (index && row.species != ActionSpecies::flag) continue;
        CAPTURE(list); INFO(row.name); CAPTURE(index);
        auto doc = MissionDocument::read(widgets / "fixture.fit", widgets / "fixture.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(doc);
        CommandHistory history(*doc);
        const auto owner = history.addObjective(0); REQUIRE(owner);
        const auto original = doc->fitBytes();
        const auto ownerName = doc->section(*owner)->name;
        ActionDraft draft{row.species, {index ? Scalar{std::int64_t{-2147483648LL}} : row.value}, index};
        if (row.species == ActionSpecies::flag) draft.payload.emplace_back(false);
        const auto added = history.addObjectiveAction(*owner, list, draft);
        INFO((added ? "created" : added.error().reason));
        REQUIRE(added);
        const auto block = doc->section(*added)->name;
        const auto suffix = list == 1 ? "Action" : "FailureAction";
        CHECK(block == ownerName + suffix + "0");
        auto expected = original;
        const auto* count = doc->find(*owner, list == 1 ? "NumActions" : "NumFailureActions"); REQUIRE(count);
        expected.replace(count->syntax.begin, 1, "1");
        expected.insert(expected.find("FITend"), "[" + block + "]\r\nst " + suffix + "SpeciesString = \"" + row.name + "\"\r\n" +
            (index ? "ul FlagIndex = -2147483648\r\nb Value = FALSE\r\n" : row.wire));
        CHECK(doc->fitBytes() == expected);
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == expected);
        CHECK(doc->objective(*owner)->entries[list].back().section == *added);
        const auto path = output / (std::to_string(list) + row.name + (index ? "-index" : "-id"));
        REQUIRE(doc->save(path, {}));
        CHECK(bytes(path.string() + ".fit") == expected);
        auto reopened = MissionDocument::read(path.string() + ".fit", path.string() + ".pak", {}, editor::tests::inputs().markerObjects); REQUIRE(reopened);
        CHECK(reopened->fitBytes() == expected);
        CHECK(std::vector<std::byte>(reopened->pakBytes().begin(), reopened->pakBytes().end()) ==
            std::vector<std::byte>(doc->pakBytes().begin(), doc->pakBytes().end()));
        for (const bool disk : {false, true}) {
            FitIniFile reader;
            REQUIRE((disk ? reader.open((path.string() + ".fit").c_str()) : reader.open(expected.data(), static_cast<int>(expected.size()))) == NO_ERR);
            REQUIRE(reader.seekBlock(block.c_str()) == NO_ERR);
            CHECK(objective_text::read(reader, (std::string(suffix) + "SpeciesString").c_str()) == std::optional<std::string>{row.name});
            if (index || row.species == ActionSpecies::resourceText) {
                unsigned long value{}; REQUIRE(reader.readIdULong(index ? "FlagIndex" : row.key, value) == NO_ERR);
                CHECK(static_cast<std::int32_t>(value) == (index ? -2147483648LL : -17));
            } else CHECK(objective_text::read(reader, row.key) == std::optional<std::string>{row.runtime});
            if (row.species == ActionSpecies::flag) {
                bool value = true; REQUIRE(reader.readIdBoolean("Value", value) == NO_ERR); CHECK_FALSE(value);
                if (index) CHECK_FALSE(objective_text::read(reader, "FlagID"));
            }
        }
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
        const auto entry = list % 2 == 0 ? history.addObjectiveEntry(owner, list) :
            history.addObjectiveAction(owner, list, {ActionSpecies::flag, {std::string("seed"), true}});
        REQUIRE(entry);
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
