#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "CommandHistory.h"
#include "FitReader.h"
#include <algorithm>
#include <chrono>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <format>
#include <limits>
#include <tuple>
#include <sstream>

using namespace editor::document;
namespace {
std::vector<std::byte> packet()
{
    std::vector<std::byte> result(156);
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) result[offset + i] = static_cast<std::byte>(value >> (8 * i));
    };
    put(0, 0xfeedface); put(4, 28); put(8, 28);
    for (unsigned i = 1; i < 5; ++i) put(8 + i * 4, 0xe000009c);
    return result;
}
std::string fixture()
{
    return "FITini\r\n[MissionSettings]\r\nst MissionName = \"E6\"\r\nst Blurb2 = \"\"\r\nuc scenarioTuneNum = 0\r\n"
        "[Script]\r\nst ScenarioScript = \"unit\"\r\n[Parts]\r\nul NumParts = 0\r\n"
        "[Objectives Version]\r\nul Version = 2\r\n[Objectives]\r\nul NumObjectives = 1\r\n"
        "[Objective0]\r\nst Title = \"Authored title\" // retain comment\r\nul Priority = 1\r\nul ResourcePoints = 700\r\n"
        "f MarkerX = 001.250000\r\nf MarkerY = -4.5\r\nl ModelType = 14\r\n"
        "b PreviousObjectiveMustBeComplete = TRUE\r\nul ActivateFlagIndex = 42\r\n"
        "b IsActive = TRUE\r\nf ActivationTime = 10.500\r\nb IsResolved = FALSE\r\nl ResolvedStatus = 1\r\nb ChangedStatus = FALSE\r\n"
        "x VendorKey = unparsed_extension\r\nul NumConditions = 1\r\nul NumActions = 1\r\nul NumFailureConditions = 1\r\nul NumFailureActions = 1\r\n"
        "[Objective0Condition0]\r\nst ConditionSpeciesString = \"VendorPredicate\"\r\nf PositionX = 001.25\r\nx Raw = retain_this\r\n"
        "[Team0Objective0Action0]\r\nst ActionSpeciesString = \"SetBooleanFlag\"\r\nst FlagID = \"reward\"\r\nb Value = TRUE\r\n"
        "[Team0Objective0FailureCondition0]\r\nst FailureConditionSpeciesString = \"DestroyEnemyUnitGroup\"\r\nul VendorGroup = 73\r\n"
        "[Team0Objective0FailureAction0]\r\nul FailureActionSpecies = 200\r\nx Unknown = keep\r\n"
        "[Team0BooleanFlags]\r\nst BooleanString0 = \"existing\"\r\nb BooleanValue0 = TRUE\r\nFITend\r\n";
}
std::filesystem::path scratch()
{
    auto path = std::filesystem::path("scratch/e6a-objective-tests") / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(path); return path;
}
std::string bytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}
Scalar replacement(const ObjectiveFieldSpec& spec)
{
    if (spec.type == 0) return std::int64_t{17};
    if (spec.type == 1) return 0.12345678912345;
    if (spec.type == 2) return true;
    return std::string("Changed objective\nsecond line");
}
}

namespace {
std::string messageFixture(std::string wire = "\"old\"")
{
    auto fit = fixture();
    const auto begin = fit.find("st ActionSpeciesString");
    const auto end = fit.find("[Team0Objective0FailureCondition0]", begin);
    fit.replace(begin, end - begin, "st ActionSpeciesString = \"DisplayTextMessage\"\r\nst Message = " + wire + "\r\n");
    return fit;
}
}

TEST_CASE("objectives/message assembly limit includes retained whitespace")
{
    auto fit = messageFixture();
    fit.insert(fit.find("\"old\""), 100, ' ');
    std::vector<std::string> logs;
    auto doc = MissionDocument::decode(fit, packet(), "buffer", {}); REQUIRE(doc);
    CommandHistory history(*doc, 32 * 1024 * 1024, [&](auto line) { logs.push_back(line); });
    const auto id = doc->objectives()[0].entries[1][0].section;
    const auto result = history.setObjectivePayload(id, "Message", std::string(1998, 'x') + "\ny");
    REQUIRE_FALSE(result);
    CHECK(result.error().reason == "reader_string_buffer_limit");
    CHECK(logs.size() == 1);
    CHECK(doc->fitBytes() == fit);
    CHECK_FALSE(history.canUndo());
    REQUIRE(history.setObjectivePayload(id, "Message", std::string(1900, 'x') + "\ny"));
    REQUIRE(history.undo());
    const auto title = doc->find(doc->objectives()[0].section, "Title"); REQUIRE(title);
    auto padded = fit;
    padded.insert(title->syntax.begin, 100, ' ');
    auto scalarDoc = MissionDocument::decode(padded, packet(), "scalar-buffer", {}); REQUIRE(scalarDoc);
    CommandHistory scalarHistory(*scalarDoc, 32 * 1024 * 1024, [&](auto line) { logs.push_back(line); });
    logs.clear();
    const auto scalarResult = scalarHistory.setObjectiveScalar(scalarDoc->objectives()[0].section, "Title", std::string(1998, 'x') + "\ny");
    REQUIRE_FALSE(scalarResult); CHECK(scalarResult.error().reason == "reader_string_buffer_limit");
    CHECK(logs.size() == 1); CHECK(scalarDoc->fitBytes() == padded);
}

TEST_CASE("objectives/LF blank line collapse matches the RAM reader")
{
    const auto fit = messageFixture("\"a\n\nb\"");
    auto doc = MissionDocument::decode(fit, packet(), "lf", {}); REQUIRE(doc);
    CHECK(doc->objectives()[0].entries[1][0].payload[0] == Scalar{std::string("ab")});
    CHECK(doc->fitBytes() == fit);
}

TEST_CASE("objectives/quoted declaration edits refuse once without changing authored bytes")
{
    auto doc = MissionDocument::decode(messageFixture(), packet(), "declaration", {}); REQUIRE(doc);
    const auto before = doc->fitBytes();
    std::vector<std::string> logs;
    CommandHistory history(*doc, 32 * 1024 * 1024, [&](auto line) { logs.push_back(line); });
    const auto id = doc->objectives()[0].entries[1][0].section;
    const std::string text = "hello\nul Count = 1";
    const auto result = history.setObjectivePayload(id, "Message", text);
    REQUIRE_FALSE(result); CHECK(result.error().reason == "reader_scanner_collision");
    CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before);
}

TEST_CASE("objectives/imported closing quote keeps trailing bytes through edits")
{
    const auto fit = messageFixture("\"one\"two\"");
    auto doc = MissionDocument::decode(fit, packet(), "quote", {}); REQUIRE(doc);
    CHECK(doc->objectives()[0].entries[1][0].payload[0] == Scalar{std::string("one")});
    CHECK(doc->fitBytes() == fit);
    CommandHistory history(*doc);
    REQUIRE(history.setObjectivePayload(doc->objectives()[0].entries[1][0].section, "Message", std::string("new")));
    auto expected = fit;
    expected.replace(expected.find("\"one\""), 5, "\"new\"");
    CHECK(doc->fitBytes() == expected);
    REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
}

TEST_CASE("objectives/all authoring scalars have independent defaults and exact commands")
{
    const auto original = fixture();
    std::vector<std::string> logs;
    const auto log = [&](const std::string& line) { logs.push_back(line); };
    auto doc = MissionDocument::decode(original, packet(), "e6a", log); REQUIRE(doc); CHECK(logs.empty());
    const auto id = doc->objectives().front().section;
    CHECK(doc->objectives().front().scalars.size() == 26);
    const std::vector<Scalar> expected{
        std::string("Authored title"), std::string("Authored title"), std::int64_t{1}, std::int64_t{700}, true, false,
        false, 1.25, -4.5, false, false, std::string("42"), false, std::string{}, false, std::int64_t{}, false, std::int64_t{},
        std::string{}, std::int64_t{14}, std::int64_t{-1}, std::int64_t{-1}, std::int64_t{-1}, 1.0, std::int64_t{-1}, std::int64_t{}};
    REQUIRE(expected.size() == doc->objectives().front().scalars.size());
    for (std::size_t i = 0; i < expected.size(); ++i) CHECK(doc->objectives().front().scalars[i].value == expected[i]);
    for (const auto& list : doc->objectives().front().entries) REQUIRE(list.size() == 1);
    CHECK(std::holds_alternative<std::monostate>(doc->objectives().front().entries[0][0].species));
    CHECK(std::get<ActionSpecies>(doc->objectives().front().entries[1][0].species) == ActionSpecies::flag);
    CHECK(std::holds_alternative<std::monostate>(doc->objectives().front().entries[2][0].species));
    CHECK(std::holds_alternative<std::monostate>(doc->objectives().front().entries[3][0].species));
    std::array<NodeId, 4> entryIds;
    for (std::size_t i = 0; i < 4; ++i) entryIds[i] = doc->objectives()[0].entries[i][0].section;
    const auto output = scratch();
    CommandHistory history(*doc, 32 * 1024 * 1024, log);
    for (const auto& spec : objectiveSchema()) {
        CAPTURE(spec.key);
        auto value = replacement(spec);
        if (spec.type == 1) value = static_cast<double>(static_cast<float>(std::get<double>(value)));
        if (spec.type == 2) value = !std::get<bool>(doc->objectiveScalar(id, spec.key)->value);
        const auto source = doc->objectiveScalar(id, spec.key)->source;
        const auto oldSource = source ? std::optional{doc->field(*source)->name} : std::nullopt;
        REQUIRE(history.setObjectiveScalar(id, spec.key, value));
        REQUIRE(doc->fitBytes() != original);
        CHECK(doc->objectiveScalar(id, spec.key)->value == value);
        CHECK(doc->objective(id)->section == id);
        const auto edited = doc->fitBytes();
        CHECK(edited.substr(edited.find("[Objective0Condition0]")) == original.substr(original.find("[Objective0Condition0]")));
        for (std::size_t i = 0; i < 4; ++i) CHECK(doc->objective(id)->entries[i][0].section == entryIds[i]);
        // Only the edited value or absent declaration may change.
        if (source) {
            auto reverted = edited;
            const auto* changed = doc->field(*source); REQUIRE(changed);
            auto old = decodeFit(original); REQUIRE(old);
            const auto oldField = std::ranges::find(old->fields, *source, &Field::id); REQUIRE(oldField != old->fields.end());
            reverted.replace(changed->syntax.begin, changed->syntax.end - changed->syntax.begin,
                original.substr(oldField->syntax.begin, oldField->syntax.end - oldField->syntax.begin));
            CHECK(reverted == original);
        }
        REQUIRE(doc->save(output / "edited", log));
        auto parsed = decodeFit(bytes(output / "edited.fit")); REQUIRE(parsed);
        const auto actual = std::ranges::find(parsed->fields, oldSource.value_or(std::string(spec.key)), &Field::name);
        REQUIRE(actual != parsed->fields.end());
        CHECK(actual->value == Value{value});
        auto reopened = MissionDocument::read(output / "edited.fit", output / "edited.pak", log); REQUIRE(reopened);
        CHECK(reopened->objectiveScalar(reopened->objectives()[0].section, spec.key)->value == value);
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original); CHECK(doc->objective(id));
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == edited); CHECK(doc->objectiveScalar(id, spec.key)->value == value);
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
    }
    CHECK(logs.empty());
}

TEST_CASE("objectives/near-match species remain opaque through scalar editing and history")
{
    auto original = fixture();
    const auto offset = original.find("SetBooleanFlag"); REQUIRE(offset != original.npos);
    original.replace(offset, std::string_view("SetBooleanFlag").size(), "setBooleanFlag");
    std::vector<std::string> logs;
    auto doc = MissionDocument::decode(original, packet(), "opaque", [&](const auto& line) { logs.push_back(line); }); REQUIRE(doc);
    const auto id = doc->objectives().front().section;
    const auto entryId = doc->objective(id)->entries[1][0].section;
    CHECK(std::holds_alternative<std::monostate>(doc->objective(id)->entries[1][0].species));
    CommandHistory history(*doc);
    REQUIRE(history.setObjectiveScalar(id, "ResourcePoints", std::int64_t{17}));
    const auto edited = doc->fitBytes();
    const auto opaqueStart = original.find("[Objective0Condition0]");
    CHECK(edited.substr(edited.find("[Objective0Condition0]")) == original.substr(opaqueStart));
    CHECK(doc->objective(id)->entries[1][0].section == entryId);
    REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
    REQUIRE(history.redo()); CHECK(doc->fitBytes() == edited);
    CHECK(std::holds_alternative<std::monostate>(doc->objective(id)->entries[1][0].species));
    CHECK(logs.empty());
}

TEST_CASE("objectives/rejection is atomic with one diagnostic and retains executable redo")
{
    std::vector<std::string> logs;
    const auto log = [&](const std::string& line) { logs.push_back(line); };
    auto doc = MissionDocument::decode(fixture(), packet(), "reject", log); REQUIRE(doc);
    const auto id = doc->objectives()[0].section;
    CommandHistory history(*doc, 32 * 1024 * 1024, log);
    REQUIRE(history.setObjectiveScalar(id, "Title", std::string(2000, 'x')));
    CHECK(std::get<std::string>(doc->objectiveScalar(id, "Title")->value).size() == 2000);
    auto limit = MissionDocument::decode(doc->fitBytes(), packet(), "limit", log); REQUIRE(limit);
    REQUIRE(history.undo());
    const auto reject = [&](NodeId target, std::string_view key, Scalar value) {
        const auto before = doc->fitBytes(); const auto revision = doc->revision(); const auto memory = history.bytesUsed(); const auto dirty = doc->modified();
        logs.clear();
        CHECK_FALSE(history.setObjectiveScalar(target, key, std::move(value)));
        CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before); CHECK(doc->revision() == revision);
        CHECK(history.bytesUsed() == memory); CHECK(doc->modified() == dirty); CHECK_FALSE(history.canUndo()); CHECK(history.canRedo());
        REQUIRE(history.redo()); REQUIRE(history.undo()); CHECK(doc->fitBytes() == before);
    };
    for (const auto key : {"Title", "Description", "ActivateFlagID", "ResetStatusFlagID", "ObjectiveModel"}) {
        reject(id, key, std::string(2001, 'x'));
        reject(id, key, std::string("quoted\"value"));
        reject(id, key, std::string("bad\rline"));
        reject(id, key, std::string("embedded\0nul", 12));
    }
    reject(id, "Priority", std::int64_t{-2147483649LL}); reject(id, "ResourcePoints", std::int64_t{2147483648LL});
    reject(id, "MarkerX", std::numeric_limits<double>::infinity()); reject(id, "ModelScale", std::numeric_limits<double>::max());
    reject(id, "ModelType", std::int64_t{2147483648LL}); reject(id, "Title", false);
    reject(id, "IsActive", false); reject(id, "NumConditions", std::int64_t{}); reject(NodeId{99999}, "Title", std::string{});
    logs.clear(); const auto revision = doc->revision(); const auto memory = history.bytesUsed();
    REQUIRE(history.setObjectiveScalar(id, "Title", std::string("Authored title")));
    CHECK(doc->revision() == revision); CHECK(history.bytesUsed() == memory); CHECK(logs.empty()); CHECK(history.canRedo());
    CommandHistory tiny(*doc, 1, log); logs.clear();
    CHECK_FALSE(tiny.setObjectiveScalar(id, "Description", std::string("too little history")));
    CHECK(logs.size() == 1); CHECK(doc->fitBytes() == fixture()); CHECK_FALSE(tiny.canUndo());
    auto control = MissionDocument::decode(fixture(), packet(), "control", {}); REQUIRE(control);
    CommandHistory controlHistory(*control);
    REQUIRE(controlHistory.setObjectiveScalar(control->objectives()[0].section, "Description", std::string("new field")));
    REQUIRE(history.setObjectiveScalar(id, "Description", std::string("new field")));
    CHECK(doc->objectiveScalar(id, "Description")->source == control->objectiveScalar(control->objectives()[0].section, "Description")->source);
    REQUIRE(history.undo()); CHECK(doc->fitBytes() == fixture());
}

TEST_CASE("objectives/malformed known fields and lists reject once without replacing history")
{
    auto doc = MissionDocument::decode(fixture(), packet(), "valid", {}); REQUIRE(doc);
    std::vector<std::string> logs;
    const auto log = [&](const std::string& line) { logs.push_back(line); };
    CommandHistory history(*doc);
    REQUIRE(history.setObjectiveScalar(doc->objectives()[0].section, "Priority", std::int64_t{2})); REQUIRE(history.undo());
    const auto bad = [&](std::string input) {
        logs.clear(); const auto revision = doc->revision(); const auto memory = history.bytesUsed();
        CHECK_FALSE(history.replace(std::move(input), packet(), "bad", log));
        CHECK(logs.size() == 1); CHECK(doc->fitBytes() == fixture()); CHECK(doc->revision() == revision);
        CHECK(history.bytesUsed() == memory); CHECK(history.canRedo()); REQUIRE(history.redo()); REQUIRE(history.undo());
    };
    const auto replace = [&](std::string_view a, std::string_view b) { auto input = fixture(); input.replace(input.find(a), a.size(), b); bad(std::move(input)); };
    replace("b IsActive = TRUE", "f DisplayMarker = 1\r\nb IsActive = TRUE");
    replace("ul Version = 2", "l Version = 2");
    replace("ul NumObjectives = 1", "l NumObjectives = 1");
    for (const auto key : {"NumConditions", "NumActions", "NumFailureConditions", "NumFailureActions"}) {
        replace(std::string(key) + " = 1", std::string(key) + " = 2");
        if (std::string_view(key) == "NumConditions") {
            replace(std::string("ul ") + key + " = 1", std::string("f ") + key + " = 1.5");
            replace(std::string("ul ") + key + " = 1", std::string("l ") + key + " = 1");
        }
    }
}

TEST_CASE("objectives/scalar prefixes follow game ignore and unsafe-read behavior")
{
    std::vector<std::string> logs;
    const auto log = [&](const auto& line) { logs.push_back(line); };
    auto doc = MissionDocument::decode(fixture(), packet(), "prefix", log); REQUIRE(doc);
    CommandHistory history(*doc);
    REQUIRE(history.setObjectiveScalar(doc->objectives()[0].section, "Priority", std::int64_t{2})); REQUIRE(history.undo());
    const auto reject = [&](std::string input, std::string_view field) {
        CAPTURE(field);
        if (field != "DisplayMarker") {
            logs.clear(); auto imported = MissionDocument::decode(input, packet(), "ignored-prefix", log);
            REQUIRE(imported);
            CHECK(imported->fitBytes() == input); CHECK(logs.size() == 1);
            CHECK(logs[0].find("ignored by the game") != std::string::npos);
            return;
        }
        logs.clear(); const auto revision = doc->revision(), memory = history.bytesUsed();
        const auto failed = history.replace(std::move(input), packet(), "wrong-prefix", log);
        REQUIRE_FALSE(failed); CHECK(failed.error().field == "Objective0/" + std::string(field));
        CHECK(logs.size() == 1); CHECK(doc->fitBytes() == fixture()); CHECK_FALSE(doc->modified());
        CHECK(doc->revision() == revision); CHECK(history.bytesUsed() == memory);
        CHECK_FALSE(history.canUndo()); CHECK(history.canRedo()); REQUIRE(history.redo()); REQUIRE(history.undo());
    };
    for (const auto& spec : objectiveSchema()) {
        CAPTURE(spec.key);
        auto valid = fixture();
        const auto begin = valid.find("st Title"), end = valid.find("x VendorKey");
        valid.erase(begin, end - begin);
        const auto declaration = std::string(spec.prefix) + " " + std::string(spec.key) + " = " +
            (spec.type == 3 ? "\"text\"" : spec.type == 2 ? "TRUE" : spec.type == 1 ? "1.25" : "1") + "\r\n";
        valid.insert(begin, declaration + (spec.key == "ObjectiveModel" ? "l ModelType = 14\r\n" : ""));
        REQUIRE(MissionDocument::decode(valid, packet(), "valid-prefix", log));
        for (const auto wrong : {"l", "ul", "c", "uc", "s", "us", "x"}) {
            if (spec.prefix == wrong) continue;
            CAPTURE(wrong);
            auto input = valid; input.replace(begin, spec.prefix.size(), wrong);
            if (spec.type != 0 && wrong != std::string_view("x")) continue;
            reject(std::move(input), spec.key);
        }
    }
    for (const auto key : {"ActivateFlagIndex", "PreviousObjectiveMustBeComplete"}) {
        auto input = fixture(); const auto pos = input.find(key);
        input.replace(input.rfind('\n', pos) + 1, key == std::string_view("ActivateFlagIndex") ? 2 : 1, "l");
        reject(std::move(input), key);
    }
}

TEST_CASE("objectives/ignored lines author explicitly while integer overflow stays read-only")
{
    for (const auto replacement : {"l ResourcePoints = 500 // keep", "ul ResourcePoints = 2147483648 // keep", "ul ResourcePoints = 9223372036854775808 // keep"}) {
        auto original = fixture();
        original.replace(original.find("ul ResourcePoints = 700"), 23, replacement);
        std::vector<std::string> logs;
        const auto log = [&](const auto& line) { logs.push_back(line); };
        auto doc = MissionDocument::decode(original, packet(), "preserved", log); REQUIRE(doc);
        REQUIRE(logs.size() == 1);
        const bool ignored = replacement[0] == 'l';
        CHECK(logs.front().find(ignored ? "ignored by the game" : "overflows or wraps") != std::string::npos);
        const auto id = doc->objectives()[0].section;
        REQUIRE(doc->objectiveScalar(id, "ResourcePoints")->source);
        CHECK(doc->find(id, "ResourcePoints")->opaque);
        CommandHistory history(*doc, 32 * 1024 * 1024, log);
        const auto before = doc->fitBytes(); const auto revision = doc->revision(); logs.clear();
        if (ignored) CHECK(doc->objectiveScalar(id, "ResourcePoints")->value == Scalar{std::int64_t{0}});
        else {
            CHECK_FALSE(history.setObjectiveScalar(id, "ResourcePoints", std::int64_t{0}));
            CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before); CHECK(doc->revision() == revision);
        }
        CHECK_FALSE(history.canUndo()); CHECK_FALSE(doc->modified());
        logs.clear(); CHECK_FALSE(history.setText(*doc->objectiveScalar(id, "ResourcePoints")->source, "repair", "Forbidden edit"));
        CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before); CHECK(doc->revision() == revision);
        const auto output = scratch(); REQUIRE(doc->save(output / "untouched", {}));
        CHECK(bytes(output / "untouched.fit") == original);
        auto reopened = MissionDocument::read(output / "untouched.fit", output / "untouched.pak", {}); REQUIRE(reopened);
        CHECK(reopened->fitBytes() == original);
        REQUIRE(history.setObjectiveScalar(id, "Priority", std::int64_t{2}));
        CHECK(doc->fitBytes().find(replacement) != std::string::npos);
        const auto changed = doc->fitBytes();
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == changed);
        CHECK(doc->objectiveScalar(id, "ResourcePoints")->warning.size() != 0);
        if (ignored) {
            REQUIRE(history.undo()); logs.clear();
            REQUIRE(history.setObjectiveScalar(id, "ResourcePoints", std::int64_t{0}));
            CHECK(logs.empty()); CHECK(doc->fitBytes().find("ul ResourcePoints = 0 // keep") != std::string::npos);
            CHECK(doc->objectiveScalar(id, "ResourcePoints")->warning.empty());
            REQUIRE(doc->save(output / "authored", {}));
            auto authored = MissionDocument::read(output / "authored.fit", output / "authored.pak", {}); REQUIRE(authored);
            CHECK(authored->objectiveScalar(authored->objectives()[0].section, "ResourcePoints")->value == Scalar{std::int64_t{0}});
            REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
            REQUIRE(history.redo()); CHECK(doc->fitBytes() == authored->fitBytes());
        }
    }
}

TEST_CASE("objectives/consumed signed values and finite scales are editable without warnings")
{
    std::vector<std::string> logs;
    auto original = fixture();
    original.replace(original.find("ul ResourcePoints = 700"), 23, "ul ResourcePoints = -55000");
    auto doc = MissionDocument::decode(original, packet(), "signed", [&](const auto& line) { logs.push_back(line); }); REQUIRE(doc);
    CHECK(logs.empty()); CHECK(doc->fitBytes() == original);
    const auto id = doc->objectives()[0].section;
    CHECK(doc->objectiveScalar(id, "ResourcePoints")->value == Scalar{std::int64_t{-55000}});
    CommandHistory history(*doc);
    for (const auto& spec : objectiveSchema()) {
        if (spec.type != 0 && spec.type != 1) continue;
        for (const auto value : {spec.type == 0 ? Scalar{std::int64_t{-2147483648LL}} : Scalar{-1.0},
                               spec.type == 0 ? Scalar{std::int64_t{2147483647}} : Scalar{0.0}}) {
            CAPTURE(spec.key);
            REQUIRE(history.setObjectiveScalar(id, spec.key, value));
            CHECK(doc->objectiveScalar(id, spec.key)->value == value);
            CHECK(doc->objectiveScalar(id, spec.key)->warning.empty());
            const auto output = scratch(); REQUIRE(doc->save(output / "signed", {}));
            auto reopened = MissionDocument::read(output / "signed.fit", output / "signed.pak", {}); REQUIRE(reopened);
            CHECK(reopened->objectiveScalar(reopened->objectives()[0].section, spec.key)->value == value);
            REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
            REQUIRE(history.redo()); REQUIRE(history.undo());
        }
    }
    auto unsafe = original;
    unsafe.replace(unsafe.find("l ModelType = 14"), 16, "l ModelType = 256");
    unsafe.insert(unsafe.find("[Objective0Condition0]"), "st ObjectiveModel = \"model\"\r\n");
    logs.clear(); CHECK_FALSE(MissionDocument::decode(unsafe, packet(), "unsafe", [&](const auto& line) { logs.push_back(line); }));
    CHECK(logs.size() == 1); CHECK(logs[0].find("asserts_in_game_camera") != std::string::npos);
}

TEST_CASE("objectives/integer bit patterns reflect the game rather than dialog bounds")
{
    auto overflow = fixture();
    overflow.replace(overflow.find("ul ActivateFlagIndex = 42"), 25, "ul ActivateFlagIndex = 2147483648");
    auto legacy = MissionDocument::decode(overflow, packet(), "legacy-overflow", {}); REQUIRE(legacy);
    CHECK(legacy->objectiveScalar(legacy->objectives()[0].section, "ActivateFlagID")->value == Scalar{std::string("2147483647")});
    for (const auto& [wire, expected] : std::vector<std::pair<std::string, std::int64_t>>{
        {"l BaseColor = 0xffffffff", -1}, {"l BaseColor = 0x80000000", -2147483648LL}, {"l BaseColor = 0Xffffffff", 0}}) {
        auto original = fixture(); original.insert(original.find("[Objective0Condition0]"), wire + "\r\n");
        auto doc = MissionDocument::decode(original, packet(), "bits", {}); REQUIRE(doc);
        CHECK(doc->fitBytes() == original);
        CHECK(doc->objectiveScalar(doc->objectives()[0].section, "BaseColor")->value == Scalar{expected});
        CHECK(doc->objectiveScalar(doc->objectives()[0].section, "BaseColor")->warning.empty());
        CommandHistory history(*doc); REQUIRE(history.setObjectiveScalar(doc->objectives()[0].section, "BaseColor", std::int64_t{7}));
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
    }
}

TEST_CASE("objectives/imported multiline lexemes remain exact through no-op edit and undo")
{
    for (const auto text : {"first\nsecond\nthird", "first\r\nsecond\r\nthird"}) {
        auto original = fixture(); const auto at = original.find("Authored title");
        original.replace(at, std::string_view("Authored title").size(), text);
        std::vector<std::string> logs;
        auto doc = MissionDocument::decode(original, packet(), "multiline", [&](const auto& line) { logs.push_back(line); }); REQUIRE(doc);
        const auto id = doc->objectives()[0].section;
        CHECK(doc->objectiveScalar(id, "Title")->value == Scalar{std::string("firstsecond\nthird")});
        CHECK(doc->fitBytes() == original);
        CommandHistory history(*doc);
        REQUIRE(history.setObjectiveScalar(id, "Priority", std::int64_t{2})); REQUIRE(history.undo());
        REQUIRE(history.setObjectiveScalar(id, "Title", std::string("firstsecond\r\nthird")));
        CHECK(doc->fitBytes() == original); CHECK(history.canRedo());
        REQUIRE(history.redo()); REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
        CHECK(logs.empty());
    }
}

TEST_CASE("objectives/entry commands preserve identities and exact history in all four lists")
{
    for (std::size_t list = 0; list < 4; ++list) {
        auto doc = MissionDocument::decode(fixture(), packet(), "entries", {}); REQUIRE(doc);
        CommandHistory history(*doc);
        const auto owner = doc->objectives()[0].section;
        const auto preserved = doc->objective(owner)->entries[list][0].section;
        const auto original = doc->fitBytes();
        const auto add = [&] { return list % 2 == 0 ? history.addObjectiveEntry(owner, list) :
            history.addObjectiveAction(owner, list, {ActionSpecies::flag, {std::string("created"), true}}); };
        auto added = add(); REQUIRE(added);
        const auto first = *added;
        REQUIRE(doc->objective(owner)->entries[list][1].flag);
        CHECK(doc->objective(owner)->entries[list][1].flag->id == (list % 2 ? "created" : ""));
        CHECK(doc->objective(owner)->entries[list][1].flag->value);
        const auto addedBytes = doc->fitBytes();
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == addedBytes);
        CHECK(doc->objective(owner)->entries[list][1].section == first);
        auto second = add(); REQUIRE(second);
        const auto two = doc->fitBytes();
        REQUIRE(history.reorderObjectiveEntry(*second, 1));
        CHECK(doc->objective(owner)->entries[list][1].section == *second);
        CHECK(doc->objective(owner)->entries[list][2].section == first);
        const auto reordered = doc->fitBytes();
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == two);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == reordered);
        REQUIRE(history.deleteObjectiveEntry(preserved));
        CHECK(doc->objective(owner)->entries[list][0].section == *second);
        const auto deleted = doc->fitBytes();
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == reordered);
        CHECK(doc->objective(owner)->entries[list][0].section == preserved);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == deleted);
        REQUIRE(history.setObjectiveFlag(first, std::string("flag\nsecond"), 0, false));
        const auto& payload = *doc->objective(owner)->entries[list][1].flag;
        CHECK(payload.id == "flag\nsecond"); CHECK_FALSE(payload.value);
        REQUIRE(payload.idSource); CHECK(doc->field(*payload.idSource)->name == "FlagID");
        REQUIRE(payload.valueSource); CHECK(doc->field(*payload.valueSource)->name == "Value");
        const auto authored = doc->fitBytes();
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == deleted);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == authored);
        REQUIRE(history.setObjectiveFlag(first, std::string(2000, 'x'), 0, true));
        CHECK(doc->objective(owner)->entries[list][1].flag->id->size() == 2000);
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == authored);
    }
}

TEST_CASE("objectives/version two action names follow the reader and keep ignored aliases exact")
{
    auto fit = fixture();
    const auto ignored = std::string("[Objective0Action0]\r\nst ActionSpeciesString = \"SetBooleanFlag\"\r\nst FlagID = \"ignored-alias\"\r\n");
    fit.insert(fit.find("FITend"), ignored);
    auto doc = MissionDocument::decode(fit, packet(), "v2-names", {}); REQUIRE(doc);
    const auto owner = doc->objectives()[0].section;
    const auto entry = doc->objective(owner)->entries[1][0].section;
    CHECK(doc->section(entry)->name == "Team0Objective0Action0");
    CHECK(doc->objective(owner)->entries[1][0].flag->id == "reward");
    auto unreadable = fit;
    unreadable.replace(unreadable.find("st ActionSpeciesString"), 2, "st[1]");
    auto preserved = MissionDocument::decode(unreadable, packet(), "array-species", {}); REQUIRE(preserved);
    CHECK(preserved->objectives()[0].entries[1][0].policy.starts_with("unreadable species string"));
    CHECK(preserved->fitBytes() == unreadable);
    CommandHistory history(*doc);
    REQUIRE(history.setObjectiveFlag(entry, std::string("actual-reader-entry"), 0, false));
    CHECK(doc->fitBytes().find(ignored) != std::string::npos);
    REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
    const auto copy = history.copyObjective(owner); REQUIRE(copy);
    CHECK(doc->section(doc->objective(*copy)->entries[1][0].section)->name == "Team0Objective1Action0");
    const auto& copiedFlag = *doc->objective(*copy)->entries[1][0].flag;
    REQUIRE(copiedFlag.idSource); CHECK(doc->field(*copiedFlag.idSource)->name == "FlagID");
    CHECK(copiedFlag.idSource != doc->objective(owner)->entries[1][0].flag->idSource);
    REQUIRE(copiedFlag.valueSource); CHECK(doc->field(*copiedFlag.valueSource)->name == "Value");
    CHECK(doc->find("Objective1Action0", "FlagID")->value == Value{std::string("ignored-alias")});
    CHECK(doc->fitBytes().find(ignored) != std::string::npos);
    REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
}

TEST_CASE("objectives/flag precedence preserves unreadable aliases and signed reader conversions")
{
    for (const auto& [wire, expected, fallback, state] : std::vector<std::tuple<std::string, std::optional<std::string>, bool, bool>>{
        {"st FlagID = \"\"\r\nul FlagIndex = malformed\r\n", std::string{}, false, true},
        {"st FlagID = \"wins\"\r\nul FlagIndex = 9223372036854775808\r\nb Value = FALSE\r\n", "wins", false, false},
        {"l FlagID = 7\r\nul FlagIndex = -2147483648\r\n", "-2147483648", true, true},
        {"st FlagID = unquoted\r\nul FlagIndex = 2147483647\r\n", "2147483647", true, true},
        {"st FlagID = \"" + std::string(2001, 'x') + "\"\r\nul FlagIndex = -1\r\n", "-1", true, true},
        {"ul FlagIndex = 2147483648\r\n", "2147483647", true, true},
        {"ul FlagIndex = -2147483649\r\n", "-2147483648", true, true},
        {"ul FlagIndex = 0xffffffff\r\n", "-1", true, true},
        {"ul FlagIndex = malformed\r\nb Value = falsehood\r\n", "0", true, false},
        {"l FlagIndex = 7\r\nl Value = 0\r\n", std::nullopt, false, true}}) {
        CAPTURE(wire);
        auto fit = fixture();
        const auto begin = fit.find("st FlagID = \"reward\"");
        fit.replace(begin, fit.find("[Team0Objective0FailureCondition0]", begin) - begin, wire);
        auto doc = MissionDocument::decode(fit, packet(), "flags", {}); REQUIRE(doc);
        const auto owner = doc->objectives()[0].section;
        const auto& flag = *doc->objective(owner)->entries[1][0].flag;
        CHECK(flag.id == expected); CHECK(flag.indexFallback == fallback); CHECK(flag.value == state);
        CHECK(doc->fitBytes() == fit);
        CommandHistory history(*doc);
        const auto flagEntry = doc->objective(owner)->entries[1][0].section;
        if (!expected) {
            CHECK_FALSE(doc->objective(owner)->entries[1][0].policy.empty());
            CHECK_FALSE(history.setObjectiveFlag(flagEntry, std::string("awaken"), 0, true));
            CHECK_FALSE(history.reorderObjectiveEntry(flagEntry, 0));
            CHECK(doc->fitBytes() == fit); CHECK(doc->revision() == 0); CHECK_FALSE(history.canUndo());
        }
        if (fallback) {
            for (const auto boundary : {-2147483648LL, 2147483647LL}) {
                REQUIRE(history.setObjectiveFlag(flagEntry, std::nullopt, boundary, !state));
                CHECK(doc->objective(owner)->entries[1][0].flag->id == std::to_string(boundary));
                CHECK(doc->objective(owner)->entries[1][0].flag->value == !state);
                const auto changed = doc->fitBytes();
                REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
                REQUIRE(history.redo()); CHECK(doc->fitBytes() == changed);
                REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
            }
        }
        REQUIRE(history.setObjectiveScalar(owner, "Priority", std::int64_t{2}));
        CHECK(doc->fitBytes().find(wire) != std::string::npos);
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
    }
}

TEST_CASE("objectives/policy and collision rejection cannot consume history or future IDs")
{
    std::vector<std::string> logs;
    auto doc = MissionDocument::decode(fixture(), packet(), "reject-entry", {}); REQUIRE(doc);
    auto control = MissionDocument::decode(fixture(), packet(), "control", {}); REQUIRE(control);
    CommandHistory history(*doc, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
    CommandHistory controlHistory(*control);
    const auto owner = doc->objectives()[0].section;
    REQUIRE(history.addObjectiveEntry(owner, 0)); REQUIRE(history.undo());
    REQUIRE(controlHistory.addObjectiveEntry(owner, 0)); REQUIRE(controlHistory.undo());
    const auto rejected = [&](auto edit) {
        logs.clear(); const auto before = doc->fitBytes(); const auto revision = doc->revision();
        const std::vector<std::byte> beforePak(doc->pakBytes().begin(), doc->pakBytes().end());
        const auto memory = history.bytesUsed(); const auto dirty = doc->modified();
        CHECK_FALSE(edit()); CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before);
        CHECK(std::vector<std::byte>(doc->pakBytes().begin(), doc->pakBytes().end()) == beforePak);
        CHECK(doc->revision() == revision); CHECK(doc->modified() == dirty); CHECK(history.bytesUsed() == memory);
        CHECK_FALSE(history.canUndo()); CHECK(history.canRedo());
        REQUIRE(history.redo()); REQUIRE(history.undo()); CHECK(doc->fitBytes() == before);
    };
    for (const auto list : {0, 2, 3}) {
        const auto entry = doc->objective(owner)->entries[list][0].section;
        CHECK_FALSE(doc->objective(owner)->entries[list][0].policy.empty());
        if (list == 0) CHECK(doc->objective(owner)->entries[list][0].policy.find("DestroyAllEnemyUnits") != std::string_view::npos);
        if (list == 2) CHECK(doc->objective(owner)->entries[list][0].policy == "unimplemented group predicate; game skips entry");
        rejected([&] { return history.reorderObjectiveEntry(entry, 0); });
        rejected([&] { return history.setObjectiveFlag(entry, std::string("bad"), 0, false); });
    }
    const auto initialized = doc->teamFlags(0);
    REQUIRE(initialized.resolve("EXISTING")); CHECK(initialized.resolve("existing")->value);
    CHECK_FALSE(doc->teamFlags(1).resolve("existing"));
    CHECK_FALSE(history.setText(doc->find("Team0BooleanFlags", "BooleanString0")->id, "renamed", "flag"));
    CHECK(doc->fitBytes() == fixture());
    auto unknown = fixture();
    unknown.replace(unknown.find("ul FailureActionSpecies = 200"), std::string_view("ul FailureActionSpecies = 200").size(), "st FailureActionSpeciesString = \"VendorAction\"");
    auto quarantined = MissionDocument::decode(unknown, packet(), "unknown-action", {}); REQUIRE(quarantined);
    CHECK(quarantined->objectives()[0].entries[3][0].policy.find("PlayBIK") != std::string_view::npos);
    CommandHistory quarantineHistory(*quarantined);
    REQUIRE(quarantineHistory.setObjectiveFlag(quarantined->objectives()[0].entries[1][0].section, std::string("editable"), 0, false));
    REQUIRE(quarantineHistory.undo()); CHECK(quarantined->fitBytes() == unknown);
    auto supported = fixture();
    supported.replace(supported.find("VendorPredicate"), 15, "DestroyAllEnemyUnits");
    supported.replace(supported.find("DestroyEnemyUnitGroup"), 21, "DestroyAllEnemyUnits");
    auto controlPolicy = MissionDocument::decode(supported, packet(), "supported-policy", {}); REQUIRE(controlPolicy);
    CHECK(controlPolicy->objectives()[0].entries[0][0].policy.empty());
    CHECK(controlPolicy->objectives()[0].entries[2][0].policy.empty());
    const auto flag = doc->objective(owner)->entries[1][0].section;
    rejected([&] { CommandHistory tiny(*doc, 1, [&](const auto& line) { logs.push_back(line); });
        return tiny.addObjectiveAction(owner, 1, {ActionSpecies::flag, {std::string("valid"), true}}); });
    rejected([&] { return history.setObjectiveFlag(flag, std::string(2001, 'x'), 0, true); });
    rejected([&] { return history.setObjectiveFlag(flag, std::nullopt, -2147483649LL, true); });
    rejected([&] { return history.setObjectiveFlag(flag, std::nullopt, 7, true); });
    for (const auto list : {1U, 3U}) {
        for (const auto& spec : actionSchema(true)) {
            rejected([&] { return history.addObjectiveAction(owner, list, {spec.species, {}}); });
            if (spec.species != ActionSpecies::flag && spec.species != ActionSpecies::resourceText)
                for (const auto& text : {std::string{}, std::string(2001, 'x'), std::string("bad\"quote"),
                        std::string("bad\rline"), std::string("bad\0tail", 8)})
                    rejected([&] { return history.addObjectiveAction(owner, list, {spec.species, {text}}); });
        }
        for (const ActionDraft draft : {
                ActionDraft{ActionSpecies::flag, {std::string("bad\"quote"), true}},
                ActionDraft{ActionSpecies::flag, {std::string(2001, 'x'), true}},
                ActionDraft{ActionSpecies::flag, {std::string("id")}},
                ActionDraft{ActionSpecies::flag, {std::string("id"), std::int64_t{1}}},
                ActionDraft{ActionSpecies::flag, {std::int64_t{2147483648LL}, true}, true},
                ActionDraft{ActionSpecies::resourceText, {std::int64_t{-2147483649LL}}},
                ActionDraft{ActionSpecies::resourceText, {std::string("7")}},
                ActionDraft{static_cast<ActionSpecies>(6), {std::string("_RemoveStructure")}},
                ActionDraft{static_cast<ActionSpecies>(255), {std::string("unknown")}}})
            rejected([&] { return history.addObjectiveAction(owner, list, draft); });
        rejected([&] { return history.addObjectiveEntry(owner, list); });
        rejected([&] { CommandHistory tiny(*doc, 1, [&](const auto& line) { logs.push_back(line); });
            return tiny.addObjectiveAction(owner, list, {ActionSpecies::flag, {std::string("valid"), true}}); });
    }
    rejected([&] { return history.addObjectiveAction(owner, 0, {ActionSpecies::wav, {std::string("sound.wav")}}); });
    auto added = history.addObjectiveEntry(owner, 0), expected = controlHistory.addObjectiveEntry(owner, 0);
    REQUIRE(added); REQUIRE(expected); CHECK(*added == *expected);
    auto hidden = fixture(); hidden.insert(hidden.find("FITend"), "[Team0Objective0Action1]\r\nst ActionSpeciesString = \"VendorAction\"\r\n");
    auto collision = MissionDocument::decode(hidden, packet(), "tail", {}); REQUIRE(collision);
    logs.clear(); CommandHistory collisionHistory(*collision, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
    CHECK_FALSE(collisionHistory.addObjectiveAction(collision->objectives()[0].section, 1,
        {ActionSpecies::flag, {std::string("valid"), true}}));
    CHECK(logs.size() == 1); CHECK(collision->fitBytes() == hidden); CHECK(collision->revision() == 0); CHECK_FALSE(collisionHistory.canUndo());
    auto parentTail = fixture(); parentTail.insert(parentTail.find("FITend"), "[Objective1]\r\nul NumConditions = 0\r\n");
    auto parent = MissionDocument::decode(parentTail, packet(), "parent-tail", {}); REQUIRE(parent);
    CommandHistory parentHistory(*parent); CHECK_FALSE(parentHistory.addObjective(0));
    CHECK(parent->fitBytes() == parentTail); CHECK(parent->revision() == 0);
    for (std::size_t list = 0; list < 4; ++list) {
        auto numeric = fixture();
        constexpr std::array suffixes = {"Condition", "Action", "FailureCondition", "FailureAction"};
        const auto key = std::string(suffixes[list]) + "SpeciesString";
        const auto at = numeric.find("st " + key);
        if (at != numeric.npos) numeric.replace(at, numeric.find('\n', at) - at + 1,
            "ul " + std::string(suffixes[list]) + "Species = " + (list % 2 == 0 ? "22" : "4") +
            (list == 1 ? "\r\n" : "\r\nst FlagID = \"numeric\"\r\n"));
        else numeric.replace(numeric.find("ul FailureActionSpecies = 200"), std::string_view("ul FailureActionSpecies = 200").size(),
            "ul FailureActionSpecies = 4\r\nst FlagID = \"numeric\"");
        auto loaded = MissionDocument::decode(numeric, packet(), "numeric", {}); REQUIRE(loaded);
        CHECK(loaded->objectives()[0].entries[list][0].flag.has_value() == (list == 0));
    }
}

TEST_CASE("objectives/condition payload edits preserve reader values and reject atomic conversion changes")
{
    const auto& paths = editor::tests::inputs();
    const auto output = paths.byteGradeDirectory.empty() ? scratch() : paths.byteGradeDirectory;
    std::filesystem::create_directories(output);
    for (const auto list : {0U, 2U}) for (const auto& spec : conditionSchema()) {
        auto doc = MissionDocument::decode(fixture(), packet(), "payload", {}); REQUIRE(doc);
        std::vector<std::string> logs;
        CommandHistory history(*doc, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
        const auto owner = doc->objectives()[0].section;
        const auto original = doc->fitBytes();
        if (spec.reference) {
            CHECK_FALSE(history.addObjectiveCondition(owner, list, spec.species));
            CHECK(doc->fitBytes() == original); CHECK_FALSE(history.canUndo());
            continue;
        }
        const auto added = history.addObjectiveCondition(owner, list, spec.species); REQUIRE(added);
        const auto created = doc->fitBytes();
        CHECK(doc->objective(owner)->entries[list].back().species == decltype(ObjectiveEntry::species){spec.species});
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == created);
        for (const auto& field : spec.fields) {
            CAPTURE(spec.name, field.key, list);
            const auto stem = std::format("payload-{}-{}-{}", list, spec.name, field.key);
            REQUIRE(doc->save(output / (stem + "-before"), {}));
            const Scalar changed = field.type == 0 ? Scalar{std::int64_t{-17}} : Scalar{-1.25};
            REQUIRE(history.setObjectivePayload(*added, field.key, changed));
            CHECK(doc->find(*added, field.key)->value == Value{changed});
            REQUIRE(doc->save(output / (stem + "-after"), {}));
            auto reopened = MissionDocument::read(output / (stem + "-after.fit"), output / (stem + "-after.pak"), {}); REQUIRE(reopened);
            CHECK(reopened->fitBytes() == doc->fitBytes());
            const auto edited = doc->fitBytes();
            REQUIRE(history.undo()); CHECK(doc->fitBytes() == created);
            REQUIRE(history.redo()); CHECK(doc->fitBytes() == edited);
            REQUIRE(history.undo());
            const std::vector<Scalar> accepted = field.type == 0 ?
                std::vector<Scalar>{std::int64_t{-2147483648LL}, std::int64_t{2147483647}} :
                std::vector<Scalar>{-1.0, 0.0, -0.0, static_cast<double>(std::numeric_limits<float>::denorm_min()),
                    static_cast<double>(std::numeric_limits<float>::max())};
            for (const auto& value : accepted) {
                REQUIRE(history.setObjectivePayload(*added, field.key, value));
                CHECK(doc->find(*added, field.key)->value == Value{value});
                if (field.type == 1) CHECK(std::signbit(std::get<double>(doc->find(*added, field.key)->value[0])) == std::signbit(std::get<double>(value)));
                if (doc->fitBytes() != created) { REQUIRE(history.undo()); CHECK(doc->fitBytes() == created); }
            }
            const std::vector<Scalar> rejected = field.type == 0 ?
                std::vector<Scalar>{std::int64_t{-2147483649LL}, std::int64_t{2147483648LL}, 1.0} :
                std::vector<Scalar>{std::numeric_limits<double>::infinity(), std::int64_t{1}};
            for (const auto& value : rejected) {
                const auto revision = doc->revision(); logs.clear();
                CHECK_FALSE(history.setObjectivePayload(*added, field.key, value));
                CHECK(logs.size() == 1); CHECK(doc->revision() == revision); CHECK(doc->fitBytes() == created);
                CHECK(history.canRedo());
            }
            auto wrong = created;
            const auto* source = doc->find(*added, field.key);
            const auto start = wrong.rfind('\n', source->syntax.begin) + 1;
            wrong.replace(start, field.prefix.size(), field.type == 0 ? "l" : "ul");
            auto ignored = MissionDocument::decode(wrong, packet(), "ignored", {}); REQUIRE(ignored);
            const auto entry = ignored->objectives()[0].entries[list].back().section;
            CommandHistory ignoredHistory(*ignored, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
            logs.clear(); CHECK_FALSE(ignoredHistory.setObjectivePayload(entry, field.key, changed));
            CHECK(logs.size() == 1); CHECK(ignored->fitBytes() == wrong); CHECK_FALSE(ignoredHistory.canUndo());
            CommandHistory tiny(*doc, 1); CHECK_FALSE(tiny.setObjectivePayload(*added, field.key, changed));
            CHECK(doc->fitBytes() == created);
        }
        CHECK_FALSE(history.addObjectiveCondition(owner, 1, spec.species));
        CHECK_FALSE(history.addObjectiveCondition(owner, 3, spec.species));
        CHECK(doc->fitBytes() == created);
    }
}

TEST_CASE("objectives/action payloads preserve paths and reject invalid text without changing history")
{
    const auto& paths = editor::tests::inputs();
    const auto output = paths.byteGradeDirectory.empty() ? scratch() : paths.byteGradeDirectory;
    std::filesystem::create_directories(output);
    for (const auto list : {1U, 3U}) for (const auto& spec : actionSchema()) {
        const auto& field = spec.fields.front();
        auto fit = fixture();
        const auto block = std::string("Team0Objective0") + (list == 1 ? "Action0" : "FailureAction0");
        const auto begin = fit.find("[" + block + "]");
        const auto end = fit.find('[', begin + 1);
        fit.replace(begin, end - begin, "[" + block + "]\r\nst " + (list == 1 ? "Action" : "FailureAction") +
            "SpeciesString = \"" + std::string(spec.name) + "\"\r\n" + std::string(field.prefix) + " " +
            std::string(field.key) + (field.type == 3 ? " = \"../Original/Name.ext\"\r\n" : " = 7\r\n"));
        auto doc = MissionDocument::decode(fit, packet(), "actions", {}); REQUIRE(doc);
        const auto id = doc->objectives()[0].entries[list][0].section;
        std::vector<std::string> logs;
        CommandHistory history(*doc, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
        const auto stem = std::format("payload-{}-{}-{}", list, spec.name, field.key);
        REQUIRE(doc->save(output / (stem + "-before"), {}));
        const Scalar changed = field.type == 3 ? Scalar{std::string("../Exact/Name\n\nsecond line\n")} : Scalar{std::int64_t{-17}};
        REQUIRE(history.setObjectivePayload(id, field.key, changed));
        CHECK(doc->objectives()[0].entries[list][0].payload[0] == changed);
        REQUIRE(doc->save(output / (stem + "-after"), {}));
        auto reopened = MissionDocument::read(output / (stem + "-after.fit"), output / (stem + "-after.pak"), {}); REQUIRE(reopened);
        CHECK(reopened->objectives()[0].entries[list][0].payload[0] == changed);
        const auto edited = doc->fitBytes();
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == edited);
        REQUIRE(history.undo());
        const auto reject = [&](Scalar value) {
            const auto revision = doc->revision(); const auto dirty = doc->modified(); const auto cost = history.bytesUsed(); logs.clear();
            CHECK_FALSE(history.setObjectivePayload(id, field.key, std::move(value)));
            CHECK(logs.size() == 1); CHECK(doc->fitBytes() == fit); CHECK(doc->revision() == revision);
            CHECK(doc->modified() == dirty); CHECK(history.bytesUsed() == cost); CHECK_FALSE(history.canUndo()); CHECK(history.canRedo());
        };
        if (field.type == 3) {
            for (const auto size : {1999U, 2000U}) {
                auto text = std::string(size - 2, 'x') + "\r\ny";
                REQUIRE(history.setObjectivePayload(id, field.key, text));
                text.erase(text.find('\r'), 1);
                CHECK(doc->objectives()[0].entries[list][0].payload[0] == Scalar{text});
                REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
            }
            for (const auto& text : {std::string(2001, 'x'), std::string("a\"b"), std::string("a\rb"), std::string("a\0b", 3)}) reject(text);
            reject(std::int64_t{3});
        } else {
            for (const auto n : {-2147483648LL, 2147483647LL}) {
                REQUIRE(history.setObjectivePayload(id, field.key, std::int64_t{n})); REQUIRE(history.undo());
            }
            reject(std::int64_t{2147483648LL}); reject(std::int64_t{-2147483649LL}); reject(std::string("7"));
        }
        auto wrong = fit;
        const auto declaration = std::string(field.prefix) + " " + std::string(field.key);
        wrong.replace(wrong.find(declaration), field.prefix.size(), "l");
        auto ignored = MissionDocument::decode(wrong, packet(), "ignored-action", {}); REQUIRE(ignored);
        CommandHistory ignoredHistory(*ignored, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
        logs.clear(); CHECK_FALSE(ignoredHistory.setObjectivePayload(ignored->objectives()[0].entries[list][0].section, field.key, changed));
        CHECK(logs.size() == 1); CHECK(ignored->fitBytes() == wrong); CHECK_FALSE(ignoredHistory.canUndo());
    }
}

TEST_CASE("objectives/decimal payload authoring records the finite game float including underflow")
{
    auto doc = MissionDocument::decode(fixture(), packet(), "decimal", {}); REQUIRE(doc);
    CommandHistory history(*doc);
    const auto entry = history.addObjectiveCondition(doc->objectives()[0].section, 0, ConditionSpecies::elapsed); REQUIRE(entry);
    for (const double number : {0.1, 2.3, 1234.56, 1e-50, -1e-50, 0.100000001}) {
        REQUIRE(history.setObjectivePayload(*entry, "Time", number));
        CHECK(std::get<double>(doc->find(*entry, "Time")->value[0]) == static_cast<double>(static_cast<float>(number)));
        auto reopened = MissionDocument::decode(doc->fitBytes(), packet(), "decimal", {}); REQUIRE(reopened);
        CHECK(reopened->find(reopened->objectives()[0].entries[0].back().section, "Time")->value == doc->find(*entry, "Time")->value);
    }
}

TEST_CASE("objectives/required unreadable payloads quarantine only their entry byte exactly")
{
    for (const auto payload : {"", "l Time = 3\r\n", "f[1] Time = 3\r\n"}) {
        auto text = fixture();
        const auto begin = text.find("[Objective0Condition0]"), end = text.find("[Team0Objective0Action0]");
        text.replace(begin, end - begin, std::string("[Objective0Condition0]\r\nst ConditionSpeciesString = \"ElapsedMissionTime\"\r\n") + payload);
        auto doc = MissionDocument::decode(text, packet(), "unreadable", {}); REQUIRE(doc);
        const auto& entry = doc->objectives()[0].entries[0][0];
        CHECK(entry.policy.find("game skips entry") != entry.policy.npos);
        CommandHistory history(*doc); CHECK_FALSE(history.setObjectivePayload(entry.section, "Time", 2.3));
        CHECK(doc->fitBytes() == text);
    }
    auto text = fixture();
    const auto begin = text.find("[Team0Objective0Action0]"), end = text.find("[Team0Objective0FailureCondition0]");
    text.replace(begin, end - begin, "[Team0Objective0Action0]\r\nst ActionSpeciesString = \"PlayBIK\"\r\n");
    auto doc = MissionDocument::decode(text, packet(), "movie", {}); REQUIRE(doc);
    CHECK_FALSE(doc->objectives()[0].entries[1][0].policy.empty()); CHECK(doc->fitBytes() == text);
}

TEST_CASE("objectives/oversized hex rejects before legacy numeric shifts")
{
    for (const auto prefix : {"ul", "l"}) {
        auto text = fixture();
        text.insert(text.find("ul NumConditions"), std::string(prefix) + " ModelID = 0x100000000\r\n");
        std::vector<std::string> logs;
        auto doc = MissionDocument::decode(text, packet(), "hex", [&](auto line) { logs.push_back(line); });
        REQUIRE_FALSE(doc);
        CHECK(doc.error().reason == "hex_digit_limit");
        REQUIRE(logs.size() == 1);
        CHECK(logs.front().find("rejected=1") != logs.front().npos);
    }
}

TEST_CASE("objectives/scanner numeric hex rejects oversized and non-ASCII digits")
{
    for (const auto prefix : {"ul", "l"}) for (const auto wire : {std::string("0x100000000"), std::string("0x1") + char(0xff)}) {
        const auto text = "FITini\r\n[Objective0]\r\n" + std::string(prefix) + " ModelID = " + wire + "\r\nFITend\r\n";
        const FitReader reader(text);
        const auto value = reader.read("Objective0", prefix, "ModelID");
        REQUIRE_FALSE(value);
        CHECK(value.error() == (wire.back() == char(0xff) ? "malformed_hex" : "hex_digit_limit"));
    }
    const auto value = readerNumber("ul", "0xffffffff // retained");
    REQUIRE(value);
    CHECK(std::get<std::int64_t>(*value) == -1);
}

TEST_CASE("objectives/case mismatched ignored blocks stay ignored through sibling and owner renames")
{
    for (const std::string ignoredName : {"Objective0condition0", "Objective0Condition0 "}) {
    auto text = fixture();
    text.replace(text.find("Objective0Condition0]"), std::string("Objective0Condition0]").size(), ignoredName + "]");
    const auto ignored = text.substr(text.find("[" + ignoredName + "]"), text.find("[Team0Objective0Action0]") - text.find("[" + ignoredName + "]"));
    auto doc = MissionDocument::decode(text, packet(), "case", {}); REQUIRE(doc);
    CHECK(doc->objectives()[0].entries[0].empty());
    CommandHistory history(*doc);
    if (ignoredName.back() == ' ') {
        REQUIRE(history.addObjectiveEntry(doc->objectives()[0].section, 0)); REQUIRE(history.undo());
    } else CHECK_FALSE(history.addObjectiveEntry(doc->objectives()[0].section, 0));
    const auto copy = history.copyObjective(doc->objectives()[0].section); REQUIRE(copy);
    CHECK_FALSE(history.reorderObjective(*copy, 0));
    CHECK(doc->fitBytes().find(ignored) != std::string::npos);
    CHECK(doc->fitBytes().find("[Objective1Condition0]") == std::string::npos);
    }
}

TEST_CASE("objectives/stock corpus exports typed scalars independently of retained FIT bytes" * doctest::test_suite("stock-fixture"))
{
    const auto& paths = editor::tests::inputs();
    const auto output = paths.byteGradeDirectory.empty() ? scratch() : paths.byteGradeDirectory;
    std::filesystem::create_directories(output);
    std::size_t total{};
    for (unsigned i = 1; i <= 24; ++i) {
        const auto name = std::format("mc2_{:02}", i);
        auto doc = MissionDocument::read(paths.stockDirectory / (name + ".fit"), paths.stockDirectory / (name + ".pak"), {}, paths.markerObjects); REQUIRE(doc);
        CHECK(doc->fitBytes() == bytes(paths.stockDirectory / (name + ".fit")));
        std::ofstream typed(output / (name + ".objectives.fit"), std::ios::binary);
        for (const auto& objective : doc->objectives()) {
            typed << '[' << doc->section(objective.section)->name << "]\n";
            total++;
            for (const auto& scalar : objective.scalars) {
                REQUIRE(scalar.source);
                const auto& field = *doc->field(*scalar.source);
                typed << field.prefix << ' ' << scalar.key << " = ";
                std::visit([&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::string>) typed << '"' << value << '"';
                    else if constexpr (std::is_same_v<T, bool>) typed << (value ? "TRUE" : "FALSE");
                    else if constexpr (std::is_same_v<T, double>) typed << std::format("{:.17g}", value);
                    else typed << value;
                }, scalar.value);
                typed << '\n';
            }
        }
        REQUIRE(typed);
    }
    CHECK(total == 173);
}

TEST_CASE("objectives/CRUD preserves four lists and stable identity in both versions")
{
    for (const bool v3 : {false, true}) {
        auto original = fixture();
        if (v3) {
            original.replace(original.find("Version = 2"), 11, "Version = 3");
            for (auto at = original.find("[Objective"); at != original.npos; at = original.find("[Objective", at + 1)) {
                if (original.substr(at, 12) == "[Objectives ") continue;
                original.insert(at + 1, "Team0");
            }
            for (unsigned team = 1; team < 8; ++team) {
                auto block = std::format("[Team{}Objectives]\r\nul NumObjectives = {}\r\n", team, team == 1 ? 1 : 0);
                if (team == 1) block += "[Team1Objective0]\r\nst Title = \"Other team\" // unchanged\r\nul NumConditions = 0\r\nx VendorRef = retain_17\r\n";
                original.insert(original.find("FITend"), block);
            }
        }
        const auto dir = scratch();
        auto doc = MissionDocument::decode(original, packet(), "crud", {}); REQUIRE(doc);
        const auto initialCount = doc->objectives().size();
        const auto source = doc->objectives().front().section;
        const auto entry = doc->objective(source)->entries[0][0].section;
        const auto payload = [&](const MissionDocument& d, NodeId section) {
            const auto* s = d.section(section); REQUIRE(s);
            const auto fit = d.fitBytes();
            return fit.substr(s->nameSyntax.end, s->syntax.end - s->nameSyntax.end);
        };
        const auto sourcePayload = payload(*doc, entry);
        const auto rootPayload = payload(*doc, source);
        const auto otherTeam = v3 ? std::optional{doc->objectives()[1].section} : std::nullopt;
        const auto otherPayload = otherTeam ? payload(*doc, *otherTeam) : std::string{};
        std::vector<std::string> logs;
        CommandHistory history(*doc, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
        const auto check = [&](const std::string& before, const std::string& changed, std::size_t count) {
            REQUIRE(changed != before); CHECK(doc->objectives().size() == count);
            REQUIRE(doc->save(dir / "crud", {}));
            auto reopened = MissionDocument::read(dir / "crud.fit", dir / "crud.pak", {}); REQUIRE(reopened);
            CHECK(reopened->fitBytes() == changed); CHECK(reopened->objectives().size() == count);
            CHECK(std::ranges::equal(reopened->pakBytes(), packet()));
            if (otherTeam) CHECK(payload(*doc, *otherTeam) == otherPayload);
            REQUIRE(history.undo()); CHECK(doc->fitBytes() == before);
            REQUIRE(history.redo()); CHECK(doc->fitBytes() == changed);
        };
        auto added = history.addObjective(v3 ? 7 : 0); REQUIRE(added);
        const auto addedId = *added;
        check(original, doc->fitBytes(), initialCount + 1);
        const std::vector<Scalar> expected{
            std::string{}, std::string{}, std::int64_t{1}, std::int64_t{}, false, false, false, 0.0, 0.0,
            false, false, std::string{}, false, std::string{}, false, std::int64_t{}, false, std::int64_t{},
            std::string{}, std::int64_t{}, std::int64_t{-1}, std::int64_t{-1}, std::int64_t{-1}, 1.0, std::int64_t{-1}, std::int64_t{2}};
        REQUIRE(doc->objective(addedId));
        for (std::size_t i = 0; i < expected.size(); ++i) CHECK(doc->objective(addedId)->scalars[i].value == expected[i]);
        for (const auto& list : doc->objective(addedId)->entries) CHECK(list.empty());
        CHECK(payload(*doc, entry) == sourcePayload);
        CHECK(payload(*doc, source) == rootPayload);
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
        auto copied = history.copyObjective(source); REQUIRE(copied); CHECK(*copied != source);
        check(original, doc->fitBytes(), initialCount + 1);
        CHECK(payload(*doc, source) == rootPayload); CHECK(payload(*doc, *copied) == rootPayload);
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& a = doc->objective(source)->entries[i];
            const auto& b = doc->objective(*copied)->entries[i];
            REQUIRE(b.size() == 1); CHECK(a[0].section != b[0].section);
            CHECK(a[0].species == b[0].species); CHECK(payload(*doc, a[0].section) == payload(*doc, b[0].section));
        }
        const auto pair = doc->fitBytes();
        REQUIRE(history.reorderObjective(*copied, 0)); check(pair, doc->fitBytes(), initialCount + 1);
        CHECK(doc->objective(source)->exportedIndex == 1); CHECK(doc->objective(*copied)->exportedIndex == 0);
        CHECK(payload(*doc, entry) == sourcePayload);
        const auto reordered = doc->fitBytes();
        REQUIRE(history.deleteObjective(source)); check(reordered, doc->fitBytes(), initialCount);
        CHECK(!doc->objective(source)); REQUIRE(doc->objective(*copied)); CHECK(doc->objective(*copied)->exportedIndex == 0);
        REQUIRE(history.undo()); REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
        REQUIRE(history.deleteObjective(source)); check(original, doc->fitBytes(), initialCount - 1);
        const auto empty = doc->fitBytes();
        auto first = history.addObjective(0); REQUIRE(first); check(empty, doc->fitBytes(), initialCount);
        CHECK(doc->objective(*first)->exportedIndex == 0);
        CHECK(logs.empty());
    }
}

TEST_CASE("objectives/oversize copy and colliding orphan headers reject without truncation")
{
    for (const bool oversized : {false, true}) {
        auto original = fixture();
        if (oversized) {
            std::string opaque;
            for (unsigned i = 0; i < 9; ++i) opaque += "st Vendor" + std::to_string(i) + " = \"" + std::string(textByteLimit - 1, 'x') + "\"\r\n";
            original.insert(original.find("[Team0Objective0Action0]"), opaque);
        } else original.insert(original.find("FITend"), "[Objective1]\r\nx Orphan = retain\r\n");
        auto doc = MissionDocument::decode(original, packet(), "crud-limit", {}); REQUIRE(doc);
        std::vector<std::string> logs;
        CommandHistory history(*doc, 128 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
        const auto id = doc->objectives()[0].section;
        const auto title = doc->find("MissionSettings", "MissionName")->id;
        REQUIRE(history.setText(title, "Retained redo", "Mission name")); REQUIRE(history.undo());
        const auto revision = doc->revision(), used = history.bytesUsed();
        auto copied = history.copyObjective(id); REQUIRE_FALSE(copied);
        CHECK(copied.error().reason == (oversized ? "file_size_limit" : "invalid_or_duplicate_block"));
        CHECK(logs.size() == 1); CHECK(doc->fitBytes() == original); CHECK_FALSE(doc->modified());
        CHECK(doc->revision() == revision); CHECK(history.bytesUsed() == used); CHECK(history.canRedo());
        REQUIRE(history.redo()); CHECK(doc->field(title)->value == Value{std::string("Retained redo")});
    }
}

TEST_CASE("objectives/CRUD rejection and no-op retain revision dirty bytes and executable history")
{
    auto doc = MissionDocument::decode(fixture(), packet(), "crud-reject", {}); REQUIRE(doc);
    std::vector<std::string> logs;
    const auto log = [&](const auto& line) { logs.push_back(line); };
    CommandHistory history(*doc, 32 * 1024 * 1024, log);
    const auto id = doc->objectives()[0].section;
    REQUIRE(history.copyObjective(id)); REQUIRE(history.undo());
    const auto before = doc->fitBytes(); const auto revision = doc->revision(); const auto used = history.bytesUsed();
    auto control = *doc; CommandHistory controlHistory(control);
    const auto nextExpected = controlHistory.addObjective(0); REQUIRE(nextExpected);
    for (unsigned op = 0; op < 6; ++op) {
        if (op == 0) CHECK_FALSE(history.addObjective(1));
        if (op == 1) CHECK_FALSE(history.copyObjective(NodeId{999999}));
        if (op == 2) CHECK_FALSE(history.deleteObjective(NodeId{999999}));
        if (op == 3) CHECK_FALSE(history.reorderObjective(id, 1));
        if (op == 4) { CommandHistory tiny(*doc, 1, log); CHECK_FALSE(tiny.addObjective(0)); CHECK_FALSE(tiny.canUndo()); }
        if (op == 5) { CommandHistory tiny(*doc, 1, log); CHECK_FALSE(tiny.deleteObjective(id)); }
        CHECK(logs.size() == op + 1); CHECK(doc->fitBytes() == before); CHECK(doc->revision() == revision);
        CHECK_FALSE(doc->modified()); CHECK(history.bytesUsed() == used); CHECK(history.canRedo());
    }
    REQUIRE(history.reorderObjective(id, 0)); CHECK(logs.size() == 6); CHECK(doc->revision() == revision); CHECK(history.canRedo());
    REQUIRE(history.redo()); CHECK(doc->objectives().size() == 2);
    REQUIRE(history.undo());
    const auto nextActual = history.addObjective(0); REQUIRE(nextActual); CHECK(*nextActual == *nextExpected);
}

TEST_CASE("objectives/CRUD defensively rejects invalid structural metadata once")
{
    for (const auto key : {"Version", "NumObjectives"}) for (const Value value : {
        Value{}, Value{true}, Value{std::int64_t{9}}, Value{std::int64_t{2}, std::int64_t{3}}}) {
        auto doc = MissionDocument::decode(fixture(), packet(), "metadata", {}); REQUIRE(doc);
        auto* field = const_cast<Field*>(doc->find(key == std::string_view("Version") ? "Objectives Version" : "Objectives", key));
        REQUIRE(field); field->value = value;
        std::vector<std::string> logs;
        CommandHistory history(*doc, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
        const auto revision = doc->revision(); const auto dirty = doc->modified();
        CHECK_FALSE(history.addObjective(0)); CHECK(logs.size() == 1);
        CHECK(doc->revision() == revision); CHECK(doc->modified() == dirty);
        CHECK(field->value == value); CHECK_FALSE(history.canUndo()); CHECK_FALSE(history.canRedo());
    }
}

TEST_CASE("objectives/structural entity and objective commands share one identity history")
{
    auto doc = MissionDocument::decode(fixture(), packet(), "interleave", {}); REQUIRE(doc);
    const auto original = doc->fitBytes();
    const auto objective = doc->objectives()[0].section;
    CommandHistory history(*doc);
    UnitPlacement unit; unit.objectNumber = 123; unit.objectProfile = "unit"; unit.csvFile = "unit";
    unit.warriorProfile = "pilot"; unit.legacyBrain = "PBrain";
    auto entity = history.insertUnit(unit); REQUIRE(entity);
    auto copy = history.copyObjective(objective); REQUIRE(copy);
    REQUIRE(history.transform(*entity, {{12, 34, 0}, 90}));
    REQUIRE(history.reorderObjective(*copy, 0));
    REQUIRE(history.erase(*entity));
    const auto final = doc->fitBytes();
    for (unsigned i = 0; i < 5; ++i) REQUIRE(history.undo());
    CHECK(doc->fitBytes() == original); CHECK(doc->objective(objective));
    for (unsigned i = 0; i < 5; ++i) REQUIRE(history.redo());
    CHECK(doc->fitBytes() == final); CHECK(doc->objective(*copy)->exportedIndex == 0); CHECK_FALSE(doc->entity(*entity));
    REQUIRE(history.undo()); REQUIRE(doc->entity(*entity)); CHECK(doc->entity(*entity)->position[0] == 12);
}

TEST_CASE("objectives/CRUD numeric boundaries nested extensions and compact framing survive")
{
    auto original = fixture();
    original.insert(original.find("[Team0Objective0Action0]"), "Vendor { InternalTarget = 17; Child { Token = raw; } }\r\n");
    original.insert(original.find("[Team0BooleanFlags]"), "[Objective0Vendor]\r\nx UnknownReference = 12345 // unchanged\r\n");
    original.replace(original.rfind("\r\nFITend"), 2, "; ");
    auto doc = MissionDocument::decode(original, packet(), "crud-boundary", {}); REQUIRE(doc);
    CommandHistory history(*doc);
    const auto source = doc->objectives()[0].section;
    const auto entry = doc->objective(source)->entries[0][0].section;
    const auto* nested = doc->childSection(entry, "Vendor"); REQUIRE(nested);
    const auto nestedId = nested->id;
    auto copy = history.copyObjective(source); REQUIRE(copy);
    const auto* cloned = doc->childSection(doc->objective(*copy)->entries[0][0].section, "Vendor"); REQUIRE(cloned);
    CHECK(cloned->id != nestedId); CHECK(doc->childSection(entry, "Vendor")->id == nestedId);
    const auto output = doc->fitBytes();
    CHECK(output.find("\r\n[Objective1]") != output.npos);
    CHECK(doc->find("Objective1Vendor", "UnknownReference")->value == Value{std::string("12345")});
    REQUIRE(history.undo()); CHECK(doc->fitBytes() == original);
    for (unsigned i = 1; i < 12; ++i) {
        auto added = history.addObjective(0); REQUIRE(added);
        REQUIRE(history.setObjectiveScalar(*added, "Title", std::format("objective-{}", i)));
    }
    const auto one = doc->objectives()[1].section, ten = doc->objectives()[10].section, eleven = doc->objectives()[11].section;
    copy = history.copyObjective(one); REQUIRE(copy);
    CHECK(doc->objective(ten)->exportedIndex == 11); CHECK(doc->objective(eleven)->exportedIndex == 12);
    CHECK(doc->objectiveScalar(*copy, "Title")->value == Scalar{std::string("objective-1")});
    CHECK(doc->objectiveScalar(ten, "Title")->value == Scalar{std::string("objective-10")});
    REQUIRE(history.deleteObjective(one)); CHECK(doc->objective(ten)->exportedIndex == 10);
    REQUIRE(history.reorderObjective(ten, 1)); CHECK(doc->objective(ten)->exportedIndex == 1);
    REQUIRE(history.undo()); CHECK(doc->objective(ten)->exportedIndex == 10);
}

namespace {
struct ReferenceFixture {
    struct Target { bool unit; float x, y; int team; };
    std::vector<Target> targets;
    std::string species = "DestroySpecificEnemyUnit", x = "0", y = "0", wid;
    int alignment{};
    std::string fit() const {
        auto text = fixture();
        text.replace(text.find("ul NumParts = 0"), 15, "ul NumParts = " +
            std::to_string(std::ranges::count_if(targets, [](const auto& t) { return t.unit; })));
        const auto begin = text.find("[Objective0Condition0]"), end = text.find("[Team0Objective0Action0]");
        text.replace(begin, end - begin, "[Objective0Condition0]\r\nst ConditionSpeciesString = \"" + species +
            "\"\r\n" + wid + (x.empty() ? "" : "f PositionX = " + x + "\r\n") +
            (y.empty() ? "" : "f PositionY = " + y + "\r\n") +
            "ul Commander = 007\r\nul Group = 9\r\nul Mate = 3\r\nul CellX = 88\r\nul CellY = 99\r\nx VendorTarget = untouched\r\n");
        std::string parts;
        unsigned index{};
        for (const auto& target : targets) if (target.unit) {
            parts += std::format("[Part{}]\r\nul ObjectNumber = 1\r\nf PositionX = {:.9g}\r\nf PositionY = {:.9g}\r\nc TeamID = {}\r\n",
                ++index, target.x, target.y, target.team);
        }
        text.insert(text.find("FITend"), parts);
        if (alignment) {
            text.replace(text.find("ul Version = 2"), 14, "ul Version = 3");
            text.replace(text.find("[Objectives]"), 12, std::format("[Team{}Objectives]", alignment));
            for (std::size_t offset{}; (offset = text.find("[Objective0", offset)) != text.npos;) {
                const auto replacement = std::format("[Team{}Objective0", alignment);
                text.replace(offset, 11, replacement); offset += replacement.size();
            }
            for (std::size_t offset{}; (offset = text.find("[Team0Objective0", offset)) != text.npos;) {
                const auto replacement = std::format("[Team{}Objective0", alignment);
                text.replace(offset, 16, replacement); offset += replacement.size();
            }
            for (int team = 0; team < 8; ++team) if (team != alignment)
                text.insert(text.find("FITend"), std::format("[Team{}Objectives]\r\nul NumObjectives = 0\r\n", team));
        }
        return text;
    }
    std::vector<std::byte> pak() const {
        auto bytes = packet();
        std::vector<std::byte> placements;
        const auto put = [&](std::vector<std::byte>& data, std::size_t offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<std::byte>(value >> (8 * i));
        };
        const auto buildings = std::ranges::count_if(targets, [](const auto& t) { return !t.unit; });
        if (buildings) {
            placements.resize(4 + 40 * buildings);
            put(placements, 0, static_cast<std::uint32_t>(buildings));
            std::size_t offset = 4;
            for (const auto& target : targets) if (!target.unit) {
                put(placements, offset, 10);
                put(placements, offset + 4, std::bit_cast<std::uint32_t>(target.x));
                put(placements, offset + 8, std::bit_cast<std::uint32_t>(target.y));
                put(placements, offset + 24, static_cast<std::uint32_t>(target.team));
                offset += 40;
            }
        }
        put(bytes, 12, 156);
        for (std::size_t i = 2; i < 5; ++i) put(bytes, 8 + 4 * i, 0xe0000000U | static_cast<std::uint32_t>(156 + placements.size()));
        bytes.insert(bytes.end(), placements.begin(), placements.end());
        return bytes;
    }
};
struct ExpectedReference {
    ReferenceKind kind;
    ReferenceStatus status;
    std::optional<EntityId> target;
    bool accepts(const ObjectiveReference& actual) const {
        return actual.kind == kind && actual.status == status && actual.target == target;
    }
};
ExpectedReference targetOracle(const ReferenceFixture& fixture) {
    ExpectedReference result{ReferenceKind::none, ReferenceStatus::Unsupported, {}};
    int filter{};
    if (fixture.species == "DestroySpecificEnemyUnit" || fixture.species == "DeadOrFledSpecificEnemyUnit") result.kind = ReferenceKind::unit;
    else if (fixture.species == "CaptureSpecificUnit" || fixture.species == "CaptureOrDestroySpecificEnemyUnit") {
        result.kind = ReferenceKind::unit; filter = -1;
    } else if (fixture.species == "GuardSpecificUnit") { result.kind = ReferenceKind::unit; filter = 1; }
    else if (fixture.species == "DestroySpecificStructure" || fixture.species == "CaptureSpecificStructure" ||
        fixture.species == "CaptureOrDestroySpecificStructure" || fixture.species == "GuardSpecificStructure") result.kind = ReferenceKind::building;
    else return result;
    if (fixture.wid.starts_with(result.kind == ReferenceKind::unit ? "l MoverWID" : "l BuildingWID")) {
        result.status = ReferenceStatus::RuntimeOnlyWid; return result;
    }
    const float x = std::stof(fixture.x), y = std::stof(fixture.y);
    const auto close = [](float a, float b) {
        const float scale = std::max(std::fabs(a), std::fabs(b));
        return scale == 0 || static_cast<float>(std::fabs(a - b) / scale) < 0.00001;
    };
    std::vector<EntityId> matches;
    std::uint64_t id{};
    for (const bool unit : {false, true}) for (const auto& target : fixture.targets) if (target.unit == unit) {
        ++id;
        if (unit != (result.kind == ReferenceKind::unit) || (filter == -1 && target.team == fixture.alignment) ||
            (filter == 1 && target.team != fixture.alignment)) continue;
        if (close(x, target.x) && close(y, target.y)) matches.push_back(EntityId{id});
    }
    result.status = matches.empty() ? ReferenceStatus::Missing : matches.size() == 1 ? ReferenceStatus::Resolved : ReferenceStatus::Ambiguous;
    if (matches.size() == 1) result.target = matches[0];
    return result;
}
ExpectedReference outputOracle(const std::string& fit, std::span<const std::byte> pak,
    const std::string& entry, int alignment = 0)
{
    const auto value = [&](const std::string& section, const std::string& key) -> Scalar {
        const auto begin = fit.find("[" + section + "]"); REQUIRE(begin != fit.npos);
        const auto end = fit.find('[', begin + 1);
        std::istringstream lines(fit.substr(begin, end - begin));
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream tokens(line);
            std::string prefix, name, equals;
            tokens >> prefix >> name >> equals;
            if (name != key || equals != "=") continue;
            std::string wire; std::getline(tokens, wire);
            if (prefix == "st") {
                const auto quote = wire.find('"');
                return wire.substr(quote + 1, wire.find('"', quote + 1) - quote - 1);
            }
            if (prefix == "f") return static_cast<double>(static_cast<float>(std::strtod(wire.c_str(), nullptr)));
            const auto hex = wire.find("0x");
            if (hex == wire.npos) return static_cast<std::int64_t>(std::strtol(wire.c_str(), nullptr, 10));
            return static_cast<std::int64_t>(static_cast<std::int32_t>(std::strtoul(wire.c_str() + hex + 2, nullptr, 16)));
        }
        FAIL("oracle field missing: " << section << "/" << key); return {};
    };
    ReferenceFixture decoded; decoded.alignment = alignment;
    decoded.species = std::get<std::string>(value(entry, "ConditionSpeciesString"));
    decoded.x = std::format("{:.17g}", std::get<double>(value(entry, "PositionX")));
    decoded.y = std::format("{:.17g}", std::get<double>(value(entry, "PositionY")));
    auto archive = decodePak({pak.begin(), pak.end()}); REQUIRE(archive);
    const auto& placements = archive->packets[1].decoded;
    const auto word = [&](std::size_t at) {
        std::uint32_t number{};
        for (unsigned i = 0; i < 4; ++i) number |= std::to_integer<std::uint32_t>(placements[at + i]) << (8 * i);
        return number;
    };
    for (std::size_t offset = 4; offset + 40 <= placements.size(); offset += 40)
        decoded.targets.push_back({false, std::bit_cast<float>(word(offset + 4)), std::bit_cast<float>(word(offset + 8)),
            std::bit_cast<std::int32_t>(word(offset + 24))});
    const auto parts = std::get<std::int64_t>(value("Parts", "NumParts"));
    for (int i = 1; i <= parts; ++i) {
        const auto part = "Part" + std::to_string(i);
        decoded.targets.push_back({true, static_cast<float>(std::get<double>(value(part, "PositionX"))),
            static_cast<float>(std::get<double>(value(part, "PositionY"))),
            static_cast<int>(std::get<std::int64_t>(value(part, "TeamID")))});
    }
    return targetOracle(decoded);
}
const ObjectiveReference& reference(const MissionDocument& document, std::size_t index = 0) {
    return document.objectives()[index].entries[0][0].reference;
}
}

TEST_CASE("objectives/reference oracle rejects wrong identities statuses kinds and alignment")
{
    for (const auto species : {"DestroySpecificEnemyUnit", "DeadOrFledSpecificEnemyUnit", "CaptureOrDestroySpecificEnemyUnit",
        "CaptureSpecificUnit", "GuardSpecificUnit", "DestroySpecificStructure", "CaptureOrDestroySpecificStructure",
        "CaptureSpecificStructure", "GuardSpecificStructure", "VendorPredicate"}) {
        for (int shape = 0; shape < 6; ++shape) {
            ReferenceFixture input; input.species = species;
            input.alignment = shape == 4 ? 3 : 0;
            input.targets = {{true, 0, 0, shape == 1 ? 0 : 1}, {false, 40, 40, 0}};
            if (shape == 2) input.targets.push_back({true, 0, 0, 1});
            if (shape == 3) input.targets[0].x = 5;
            if (shape == 4) input.targets[1].x = input.targets[1].y = 0;
            if (shape == 5) input.targets.erase(input.targets.begin());
            std::vector<std::string> logs;
            auto document = MissionDocument::decode(input.fit(), input.pak(), "oracle", [&](auto line) { logs.push_back(line); });
            REQUIRE(document);
            const auto expected = targetOracle(input);
            CAPTURE(species, shape);
            CHECK(expected.accepts(reference(*document)));
            CHECK(logs.size() == (expected.status == ReferenceStatus::Missing || expected.status == ReferenceStatus::Ambiguous ? 1 : 0));
            CHECK(document->fitBytes() == input.fit()); CHECK(std::ranges::equal(document->pakBytes(), input.pak()));
            auto corrupted = reference(*document);
            corrupted.target = EntityId{9999}; CHECK_FALSE(expected.accepts(corrupted));
            corrupted = reference(*document);
            corrupted.status = expected.status == ReferenceStatus::Resolved ? ReferenceStatus::Missing : ReferenceStatus::Resolved;
            CHECK_FALSE(expected.accepts(corrupted));
        }
    }
}

TEST_CASE("objectives/strict float matching handles zero negative adjacent and tolerance boundary")
{
    for (const float value : {0.0f, -0.0f, -100.0f, 100.0f, std::nextafter(100.0f, 0.0f),
        std::nextafter(100.0f, 200.0f), 99.99901f, 99.999f, 99.99899f, -99.99901f, -99.99899f,
        std::numeric_limits<float>::denorm_min()}) {
        ReferenceFixture input; input.x = std::format("{:.9g}", value);
        input.targets = {{true, value < 0 ? -100.0f : value == 0 || value < 1 ? 0.0f : 100.0f, 0, 0}};
        auto document = MissionDocument::decode(input.fit(), input.pak(), "tolerance", {}); REQUIRE(document);
        CAPTURE(value); CHECK(targetOracle(input).accepts(reference(*document)));
    }
    ReferenceFixture input; input.targets = {{true, 0, 0, 0}};
    for (const auto [x, expected] : {std::pair{"99.99901", ReferenceStatus::Resolved}, {"99.99899", ReferenceStatus::Missing}}) {
        input.x = x; input.targets[0].x = 100;
        auto document = MissionDocument::decode(input.fit(), input.pak(), "boundary", {}); REQUIRE(document);
        CHECK(reference(*document).status == expected);
    }
}

TEST_CASE("objectives/readable WIDs bypass absent and malformed XY but unreadable WIDs use XY")
{
    for (const bool unit : {false, true}) for (const auto xy : {"", "broken", "0"}) for (const auto prefix : {"l", "ul", "st"}) {
        ReferenceFixture input; input.species = unit ? "DestroySpecificEnemyUnit" : "GuardSpecificStructure";
        input.targets = {{unit, 0, 0, 0}}; input.x = input.y = xy;
        input.wid = std::string(prefix) + (unit ? " MoverWID = " : " BuildingWID = ") + (prefix == std::string_view("st") ? "\"7\"" : "7") + "\r\n";
        auto document = MissionDocument::decode(input.fit(), input.pak(), "wid", {});
        if (prefix != std::string_view("l") && xy == std::string_view("")) { REQUIRE(document); CHECK_FALSE(document->objectives()[0].entries[0][0].policy.empty()); continue; }
        REQUIRE(document);
        if (xy == std::string_view("broken")) input.x = input.y = "0";
        CHECK(targetOracle(input).accepts(reference(*document)));
        if (xy == std::string_view("broken")) input.x = input.y = "broken";
        CHECK(document->fitBytes() == input.fit());
    }
}

TEST_CASE("objectives/array WIDs use XY while malformed signed WIDs read as zero")
{
    for (const bool unit : {false, true}) for (const auto declaration : {"l[1]", "l"}) for (const bool xy : {false, true}) {
        ReferenceFixture input; input.species = unit ? "DestroySpecificEnemyUnit" : "GuardSpecificStructure";
        input.targets = {{unit, 0, 0, 0}};
        input.wid = std::string(declaration) + (unit ? " MoverWID = " : " BuildingWID = ") +
            (declaration == std::string_view("l") ? "broken\r\n" : "7\r\n");
        if (!xy) input.x = input.y = "";
        auto doc = MissionDocument::decode(input.fit(), input.pak(), "wid-declaration", {});
        CAPTURE(unit, declaration, xy);
        if (declaration == std::string_view("l")) {
            REQUIRE(doc); CHECK(reference(*doc).status == ReferenceStatus::RuntimeOnlyWid);
            CHECK(doc->fitBytes() == input.fit()); continue;
        }
        if (!xy) { REQUIRE(doc); CHECK_FALSE(doc->objectives()[0].entries[0][0].policy.empty()); continue; }
        REQUIRE(doc); CHECK(reference(*doc).status == ReferenceStatus::Resolved);
        CHECK(reference(*doc).target == EntityId{1}); CHECK(doc->fitBytes() == input.fit());
    }
}

TEST_CASE("objectives/moves cannot collide with a recorded resolved target")
{
    for (const bool unit : {false, true}) for (const bool moveTarget : {false, true}) {
        ReferenceFixture input; input.species = unit ? "DestroySpecificEnemyUnit" : "GuardSpecificStructure";
        input.targets = {{unit, 10, 0, 0}, {unit, 20, 0, 0}}; input.x = "20";
        auto doc = MissionDocument::decode(input.fit(), input.pak(), "collision", {}); REQUIRE(doc);
        std::vector<std::string> logs;
        CommandHistory history(*doc, 32 * 1024 * 1024, [&](auto line) { logs.push_back(line); });
        const auto revision = doc->revision();
        CHECK_FALSE(history.transform(EntityId{moveTarget ? 2ULL : 1ULL}, {{moveTarget ? 10.0 : 20.0, 0, 0}, 0}));
        REQUIRE(logs.size() == 1); CHECK(logs[0].find("objective_reference") != std::string::npos);
        CHECK(doc->fitBytes() == input.fit()); CHECK(std::ranges::equal(doc->pakBytes(), input.pak()));
        CHECK(doc->revision() == revision); CHECK_FALSE(doc->modified()); CHECK_FALSE(history.canUndo());
        CHECK(history.bytesUsed() == 0); CHECK(reference(*doc).target == EntityId{2});
    }
}

TEST_CASE("objectives/team edits cannot exclude or add a resolved target candidate")
{
    for (const auto species : {"GuardSpecificUnit", "CaptureSpecificUnit", "CaptureOrDestroySpecificEnemyUnit"})
        for (const bool target : {false, true}) {
        ReferenceFixture input; input.species = species;
        const int eligible = input.species == "GuardSpecificUnit" ? 0 : 1;
        input.targets = {{true, 0, 0, eligible}, {true, 0, 0, 1 - eligible}};
        auto doc = MissionDocument::decode(input.fit(), input.pak(), "team-filter", {}); REQUIRE(doc);
        std::vector<std::string> logs;
        CommandHistory history(*doc, 32 * 1024 * 1024, [&](auto line) { logs.push_back(line); });
        const auto revision = doc->revision();
        CHECK_FALSE(history.setTeam(EntityId{target ? 1ULL : 2ULL}, target ? 1 - eligible : eligible));
        REQUIRE(logs.size() == 1); CHECK(logs[0].find("objective_reference") != std::string::npos);
        CHECK(doc->fitBytes() == input.fit()); CHECK(doc->revision() == revision); CHECK_FALSE(doc->modified());
        CHECK_FALSE(history.canUndo()); CHECK(history.bytesUsed() == 0); CHECK(reference(*doc).target == EntityId{1});
    }
}

TEST_CASE("objectives/transforms retain each unchanged referring coordinate lexeme")
{
    for (const bool unit : {false, true}) for (const int axis : {-1, 0, 1, 2}) {
        ReferenceFixture input; input.species = unit ? "DestroySpecificEnemyUnit" : "GuardSpecificStructure";
        input.x = "001.250000"; input.y = "-4.50000"; input.targets = {{unit, 1.25f, -4.5f, 0}};
        auto doc = MissionDocument::decode(input.fit(), input.pak(), "spelling", {}); REQUIRE(doc);
        CommandHistory history(*doc);
        Transform transform{{1.25, -4.5, 0}, axis == -1 ? 90.0 : 0.0};
        if (axis >= 0) transform.position[axis] += 1;
        if (axis == 0 || axis == 1) { CHECK_FALSE(history.transform(EntityId{1}, transform)); continue; }
        REQUIRE(history.transform(EntityId{1}, transform));
        for (const auto& source : reference(*doc).sources) {
            if (source.key == "PositionX" && axis != 0) CHECK(source.lexeme == input.x);
            if (source.key == "PositionY" && axis != 1) CHECK(source.lexeme == input.y);
        }
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == input.fit());
    }
}

TEST_CASE("objectives/structural edits preserve missing ambiguous and noncontiguous target identities")
{
    ReferenceFixture input; input.x = "10"; input.targets = {{true, 10, 0, 0}, {true, 30, 0, 0}};
    auto doc = MissionDocument::decode(input.fit(), input.pak(), "identity", {}); REQUIRE(doc);
    CommandHistory history(*doc);
    const auto target = reference(*doc).target; REQUIRE(target); CHECK(target->value == 1);
    BuildingPlacement building; building.transform.position = {100, 100, 0};
    auto inserted = history.insertBuilding(building); REQUIRE(inserted); CHECK(inserted->value == 3);
    CHECK(doc->entities()[0].id == *inserted); CHECK(reference(*doc).target == target);
    REQUIRE(history.erase(*inserted));
    inserted = history.insertBuilding(building); REQUIRE(inserted); CHECK(inserted->value == 4);
    CHECK(reference(*doc).target == target);
    REQUIRE(history.setTeam(*target, 1)); CHECK(reference(*doc).target == target);
    for (const bool ambiguous : {false, true}) {
        input.x = "50";
        input.targets = {{true, ambiguous ? 50.0f : 0.0f, 0, 0}, {true, 50, 0, 0}};
        if (!ambiguous) input.targets[1].x = 10;
        doc = MissionDocument::decode(input.fit(), input.pak(), "carry", {}); REQUIRE(doc);
        std::vector<std::string> logs;
        CommandHistory edits(*doc, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
        const auto expected = targetOracle(input); CHECK(expected.accepts(reference(*doc)));
        CHECK_FALSE(edits.transform(doc->entities()[0].id, {{ambiguous ? 70.0 : 50.0, 0, 0}, 0}));
        REQUIRE(logs.size() == 1); logs.clear();
        if (ambiguous) { CHECK_FALSE(edits.erase(doc->entities()[0].id)); REQUIRE(logs.size() == 1); logs.clear(); }
        UnitPlacement unit; unit.transform.position = {50, 0, 0};
        unit.objectProfile = unit.csvFile = unit.warriorProfile = unit.legacyBrain = "unit";
        CHECK_FALSE(edits.insertUnit(unit)); REQUIRE(logs.size() == 1);
        CHECK(expected.accepts(reference(*doc)));
        auto fresh = MissionDocument::decode(doc->fitBytes(), input.pak(), "fresh", {}); REQUIRE(fresh);
        CHECK(reference(*fresh).status == expected.status);
        CHECK_FALSE(edits.canUndo());
        const auto copy = edits.copyObjective(doc->objectives()[0].section); REQUIRE(copy);
        CHECK(expected.accepts(doc->objective(*copy)->entries[0][0].reference));
        REQUIRE(edits.reorderObjective(*copy, 0));
        CHECK(expected.accepts(doc->objective(*copy)->entries[0][0].reference));
    }
}

TEST_CASE("objectives/referenced deletes reject once and resolved moves undo exact spelling across CRUD")
{
    for (int kind = 0; kind < 3; ++kind) {
        const bool unit = kind == 0, marker = kind == 2;
        ReferenceFixture input; input.species = unit ? "GuardSpecificUnit" : "DestroySpecificStructure";
        input.x = "001.250000"; input.y = "-4.50000"; input.targets = {{unit, 1.25f, -4.5f, 0}};
        auto original = input.fit();
        const auto failure = original.find("[Team0Objective0FailureCondition0]"), failureEnd = original.find("[Team0Objective0FailureAction0]");
        original.replace(failure, failureEnd - failure, "[Team0Objective0FailureCondition0]\r\nst FailureConditionSpeciesString = \"" +
            input.species + "\"\r\nf PositionX = 001.250000\r\nf PositionY = -4.50000\r\nul Commander = 007\r\n");
        if (marker) original.insert(original.find("FITend"),
            "[NavMarkers]\r\nul NumNavMarkers = 1\r\n[NavMarker0]\r\nf xPos = 1.2500\r\nf yPos = -4.5000\r\nf radius = 20\r\n");
        const std::vector<std::uint32_t> markers = marker ? std::vector<std::uint32_t>{10} : std::vector<std::uint32_t>{};
        auto doc = MissionDocument::decode(original, input.pak(), "commands", {}, markers); REQUIRE(doc);
        auto control = *doc;
        std::vector<std::string> logs;
        CommandHistory history(*doc, 32 * 1024 * 1024, [&](auto line) { logs.push_back(line); });
        const auto target = *reference(*doc).target;
        const auto objective = doc->objectives()[0].section;
        const auto before = doc->fitBytes(); const auto revision = doc->revision();
        CHECK_FALSE(history.erase(target)); CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before);
        CHECK(doc->revision() == revision); CHECK_FALSE(doc->modified()); CHECK_FALSE(history.canUndo()); CHECK(history.bytesUsed() == 0);
        auto copied = history.copyObjective(objective); REQUIRE(copied); CHECK(reference(*doc, 1).target == target);
        REQUIRE(history.setObjectiveScalar(*copied, "AllPreviousPrimaryObjectivesMustBeComplete", true));
        REQUIRE(history.reorderObjective(*copied, 0));
        CHECK(doc->objective(*copied)->exportedIndex == 0);
        CHECK(doc->objectiveScalar(*copied, "PreviousPrimaryObjectiveMustBeComplete")->value == Scalar{true});
        CHECK(doc->objectiveScalar(*copied, "AllPreviousPrimaryObjectivesMustBeComplete")->value == Scalar{true});
        CHECK(doc->objectiveScalar(objective, "AllPreviousPrimaryObjectivesMustBeComplete")->value == Scalar{false});
        CHECK(doc->objective(*copied)->entries[0][0].reference.target == target);
        REQUIRE(history.undo()); REQUIRE(history.undo());
        CommandHistory controlHistory(control);
        const auto expectedCopy = controlHistory.copyObjective(objective); REQUIRE(expectedCopy); CHECK(*copied == *expectedCopy);
        CHECK_FALSE(history.transform(target, {{12, 34, 0}, 0}));
        REQUIRE(history.transform(target, {{1.25, -4.5, 1}, 0}));
        if (marker) CHECK(doc->find("NavMarker0", "xPos")->value == Value{1.25});
        for (const auto& item : doc->objectives()) {
            for (const auto list : {0, 2}) {
                const auto entry = item.entries[list][0].section;
                CHECK(doc->find(entry, "PositionX")->value == Value{1.25});
                CHECK(doc->find(entry, "PositionY")->value == Value{-4.5});
                CHECK(doc->find(entry, "Commander")->value == Value{std::int64_t{7}});
                CHECK(item.entries[list][0].reference.target == target);
            }
            CHECK(item.entries[0][0].reference.target == target);
            for (const auto& source : item.entries[0][0].reference.sources) {
                CHECK(doc->field(source.node));
                if (source.key == "Commander") CHECK(source.lexeme == "007");
            }
        }
        REQUIRE(history.setObjectiveScalar(objective, "Title", std::string("moved")));
        const auto moved = doc->fitBytes();
        const auto path = scratch() / "references"; REQUIRE(doc->save(path, {}));
        auto reopened = MissionDocument::read(path.string() + ".fit", path.string() + ".pak", {}, markers); REQUIRE(reopened);
        CHECK(reopened->fitBytes() == moved); CHECK(reference(*reopened).status == ReferenceStatus::Resolved);
        REQUIRE(history.undo()); REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(doc->fitBytes() == before);
        REQUIRE(history.redo()); REQUIRE(history.redo()); REQUIRE(history.redo()); CHECK(doc->fitBytes() == moved);
        REQUIRE(history.deleteObjective(*copied)); REQUIRE(history.deleteObjective(objective)); REQUIRE(history.erase(target));
        REQUIRE(history.undo()); REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(reference(*doc).target == target);
        CHECK(doc->fitBytes() == moved);
    }
}

TEST_CASE("objectives/Part compaction retains target identity Pilot and reference spelling and insertion refuses ambiguity")
{
    ReferenceFixture input; input.x = "030.0000"; input.targets = {{true, 10, 0, 0}, {true, 30, 0, 0}};
    auto fit = input.fit();
    fit.insert(fit.find("[Part2]"), "ul Pilot = 1\r\n");
    fit.insert(fit.find("FITend"), "ul Pilot = 2\r\n[Warriors]\r\nul NumWarriors = 2\r\n"
        "[Warrior1]\r\nst Profile = \"first\"\r\n[Warrior2]\r\nst Profile = \"target\"\r\n");
    auto doc = MissionDocument::decode(fit, input.pak(), "renumber", {}); REQUIRE(doc);
    std::vector<std::string> logs;
    CommandHistory history(*doc, 32 * 1024 * 1024, [&](auto line) { logs.push_back(line); });
    const auto target = *reference(*doc).target;
    const auto sources = reference(*doc).sources;
    const auto warrior = doc->entity(target)->warrior;
    REQUIRE(history.erase(EntityId{1}));
    CHECK(doc->entity(target)->exportedPart == 1); CHECK(doc->entity(target)->warrior == warrior);
    CHECK(reference(*doc).target == target);
    for (std::size_t i = 0; i < sources.size(); ++i) CHECK(reference(*doc).sources[i].lexeme == sources[i].lexeme);
    CHECK(outputOracle(doc->fitBytes(), doc->pakBytes(), "Objective0Condition0").target == EntityId{1});
    const auto compacted = doc->fitBytes(); const auto revision = doc->revision(); const auto cost = history.bytesUsed();
    UnitPlacement unit; unit.transform.position = {30, 0, 0};
    unit.objectProfile = unit.csvFile = unit.warriorProfile = unit.legacyBrain = "unit";
    CHECK_FALSE(history.insertUnit(unit)); REQUIRE(logs.size() == 1);
    CHECK(logs[0].find("objective_reference") != std::string::npos);
    CHECK(doc->fitBytes() == compacted); CHECK(doc->revision() == revision); CHECK(history.bytesUsed() == cost);
    unit.transform.position[0] = 90;
    const auto inserted = history.insertUnit(unit); REQUIRE(inserted); CHECK(inserted->value == 3);
    CHECK(reference(*doc).target == target); CHECK(doc->entity(target)->warrior == warrior);
    REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit);
    REQUIRE(history.redo()); CHECK(doc->fitBytes() == compacted);
}

TEST_CASE("objectives/resolve replaces carried status with a unique exported target and undo redo save reopen agree")
{
    for (const bool unit : {false, true}) for (const bool ambiguous : {false, true}) {
        ReferenceFixture input; input.species = unit ? "CaptureSpecificUnit" : "GuardSpecificStructure";
        input.x = "050.000"; input.y = "0.0000";
        input.targets = {{unit, ambiguous ? 50.0f : 10.0f, 0, 1}, {unit, ambiguous ? 50.0f : 30.0f, 0, 1}, {unit, 70, 0, 1}};
        auto doc = MissionDocument::decode(input.fit(), input.pak(), "resolve", {}); REQUIRE(doc);
        CommandHistory history(*doc);
        const auto entry = doc->objectives()[0].entries[0][0].section;
        const auto original = reference(*doc);
        if (ambiguous) CHECK_FALSE(history.transform(EntityId{1}, {{20, 0, 0}, 0}));
        else REQUIRE(history.transform(EntityId{1}, {{20, 0, 0}, 0}));
        CHECK(reference(*doc).status == original.status);
        const auto before = doc->fitBytes(); const auto old = reference(*doc).sources;
        const auto resolvedCommand = history.resolveObjectiveEntry(entry, EntityId{3});
        INFO("unit=" << unit << " ambiguous=" << ambiguous << " reason=" << (resolvedCommand ? "" : resolvedCommand.error().reason));
        REQUIRE(resolvedCommand);
        CHECK(reference(*doc).target == EntityId{3}); CHECK(reference(*doc).status == ReferenceStatus::Resolved);
        auto exported = outputOracle(doc->fitBytes(), doc->pakBytes(), "Objective0Condition0");
        CHECK(exported.status == ReferenceStatus::Resolved); CHECK(exported.target == EntityId{3});
        for (std::size_t i = 0; i < old.size(); ++i)
            if (old[i].key != "PositionX") CHECK(reference(*doc).sources[i].lexeme == old[i].lexeme);
        const auto resolved = doc->fitBytes();
        auto corrupted = resolved;
        const auto at = corrupted.find("f PositionX = 70", corrupted.find("[Objective0Condition0]")); REQUIRE(at != corrupted.npos);
        corrupted.replace(at, 16, "f PositionX = 50");
        CHECK(outputOracle(corrupted, doc->pakBytes(), "Objective0Condition0").target != exported.target);
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == before); CHECK(reference(*doc).status == original.status);
        REQUIRE(history.redo()); CHECK(doc->fitBytes() == resolved); CHECK(reference(*doc).target == EntityId{3});
        const auto path = scratch() / "resolved"; REQUIRE(doc->save(path, {}));
        auto reopened = MissionDocument::read(path.string() + ".fit", path.string() + ".pak", {}); REQUIRE(reopened);
        CHECK(outputOracle(reopened->fitBytes(), reopened->pakBytes(), "Objective0Condition0").accepts(reference(*reopened)));
        CHECK(reference(*reopened).target == EntityId{3});
    }
}

TEST_CASE("objectives/refused resolve logs once and preserves bytes revision dirty redo budget and future IDs")
{
    for (int shape = 0; shape < 9; ++shape) {
        ReferenceFixture input; input.x = "50"; input.targets = {{true, 10, 0, 1}, {true, 10, 0, 1}, {false, 20, 0, 0}};
        if (shape == 2) input.species = "GuardSpecificUnit";
        if (shape == 3) input.wid = "l MoverWID = 7\r\n";
        if (shape == 4) input.species = "VendorPredicate";
        if (shape == 5) input.targets[1].x = 30;
        if (shape == 8) { input.targets[1].x = 30; input.x = "10"; }
        auto doc = MissionDocument::decode(input.fit(), input.pak(), "reject-resolve", {}); REQUIRE(doc);
        auto twin = *doc;
        std::vector<std::string> logs;
        CommandHistory history(*doc, shape == 5 ? 1 : 32 * 1024 * 1024, [&](auto line) { logs.push_back(line); });
        CommandHistory control(twin);
        const auto objective = doc->objectives()[0].section, entry = doc->objectives()[0].entries[0][0].section;
        if (shape != 5) {
            REQUIRE(history.copyObjective(objective)); REQUIRE(history.undo());
            REQUIRE(control.copyObjective(objective)); REQUIRE(control.undo());
        }
        const auto fit = doc->fitBytes(); const std::vector<std::byte> pak(doc->pakBytes().begin(), doc->pakBytes().end());
        const auto revision = doc->revision(), cost = history.bytesUsed(); const auto dirty = doc->modified();
        const auto carried = reference(*doc);
        const auto undo = history.canUndo(), redo = history.canRedo();
        CHECK_FALSE(history.resolveObjectiveEntry(shape == 6 ? NodeId{99999} : entry,
            EntityId{shape == 7 ? 99999U : shape == 1 ? 1U : 2U}));
        REQUIRE(logs.size() == 1); CHECK(logs[0].find(shape == 5 ? "history_budget" : "rejected=1") != std::string::npos);
        CHECK(doc->fitBytes() == fit); CHECK(std::ranges::equal(doc->pakBytes(), pak)); CHECK(doc->revision() == revision);
        CHECK(doc->modified() == dirty); CHECK(history.bytesUsed() == cost); CHECK(history.canUndo() == undo); CHECK(history.canRedo() == redo);
        CHECK(reference(*doc).status == carried.status); CHECK(reference(*doc).target == carried.target);
        CHECK(reference(*doc).reason == carried.reason);
        if (redo) { REQUIRE(history.redo()); REQUIRE(history.undo()); }
        CommandHistory retry(*doc);
        const auto copy = retry.copyObjective(objective), expected = control.copyObjective(objective); REQUIRE(copy); REQUIRE(expected);
        CHECK(*copy == *expected);
        BuildingPlacement building; building.transform.position = {90, 0, 0};
        const auto inserted = retry.insertBuilding(building), controlInserted = control.insertBuilding(building);
        if (shape == 3) {
            REQUIRE_FALSE(inserted); REQUIRE_FALSE(controlInserted);
            CHECK(inserted.error().reason == "objective_reference");
            continue;
        }
        REQUIRE(inserted); REQUIRE(controlInserted); CHECK(*inserted == *controlInserted);
    }
}

TEST_CASE("objectives/reference storage contributes to history budget and rejection preserves future IDs")
{
    ReferenceFixture input; input.targets = {{true, 0, 0, 0}};
    auto doc = MissionDocument::decode(input.fit(), input.pak(), "budget", {}); REQUIRE(doc);
    auto twin = *doc;
    const auto snapshotReference = reference(twin);
    const auto nested = referenceStorage(snapshotReference); REQUIRE(nested > 0);
    auto& stripped = const_cast<ObjectiveReference&>(reference(twin));
    stripped.sources = {};
    stripped.sources.shrink_to_fit();
    CommandHistory measured(*doc), control(twin);
    BuildingPlacement building; building.transform.position = {20, 20, 0};
    REQUIRE(measured.insertBuilding(building)); REQUIRE(control.insertBuilding(building));
    CHECK(measured.bytesUsed() == control.bytesUsed() + nested);
    const auto budget = measured.bytesUsed() - 1;
    auto rejected = MissionDocument::decode(input.fit(), input.pak(), "budget", {}); REQUIRE(rejected);
    std::vector<std::string> logs;
    CommandHistory limited(*rejected, budget, [&](auto line) { logs.push_back(line); });
    const auto bytes = rejected->fitBytes();
    const auto revision = rejected->revision();
    CHECK_FALSE(limited.insertBuilding(building)); CHECK(logs.empty()); CHECK(rejected->fitBytes() == bytes);
    CHECK(rejected->revision() == revision); CHECK_FALSE(rejected->modified()); CHECK(limited.bytesUsed() == 0);
    CHECK_FALSE(limited.canUndo()); CHECK_FALSE(limited.canRedo());
    CommandHistory retry(*rejected); auto id = retry.insertBuilding(building); REQUIRE(id); CHECK(id->value == 2);
    CHECK(reference(*rejected).target == EntityId{1});
    auto unitDoc = MissionDocument::decode(input.fit(), input.pak(), "unit-budget", {}); REQUIRE(unitDoc);
    CommandHistory noRoom(*unitDoc, 1);
    UnitPlacement unit; unit.objectProfile = unit.csvFile = unit.warriorProfile = unit.legacyBrain = "unit";
    unit.transform.position = {20, 20, 0};
    CHECK_FALSE(noRoom.insertUnit(unit)); CHECK(unitDoc->fitBytes() == input.fit());
    CommandHistory unitRetry(*unitDoc); auto unitId = unitRetry.insertUnit(unit); REQUIRE(unitId); CHECK(unitId->value == 2);
}

TEST_CASE("objectives/reference allocation limit rejects the entire replacement without truncation")
{
    ReferenceFixture input; input.targets = {{true, 0, 0, 0}};
    auto doc = MissionDocument::decode(input.fit(), input.pak(), "limit", {}); REQUIRE(doc);
    const auto original = doc->fitBytes();
    auto large = original;
    const auto count = large.find("ul NumConditions = 1");
    large.replace(count, 20, "ul NumConditions = 1250");
    std::string entries;
    for (unsigned i = 1; i < 1250; ++i) entries += std::format(
        "[Objective0Condition{}]\r\nst ConditionSpeciesString = \"DestroySpecificEnemyUnit\"\r\n"
        "f PositionX = 0\r\nf PositionY = 0\r\nul MoverWID = 7\r\nul BuildingWID = 8\r\n"
        "st Commander = \"{}\"\r\nul Group = 9\r\nul Mate = 3\r\nul CellX = 88\r\nul CellY = 99\r\n", i, std::string(12800, 'a'));
    large.insert(large.find("FITend"), entries);
    REQUIRE(large.size() < fitByteLimit);
    CommandHistory history(*doc);
    auto result = history.replace(large, input.pak(), "limit", {});
    REQUIRE_FALSE(result); CHECK(result.error().reason == "reference_storage_limit");
    CHECK(doc->fitBytes() == original); CHECK(doc->revision() == 0); CHECK_FALSE(doc->modified()); CHECK(history.bytesUsed() == 0);
    CHECK(reference(*doc).target == EntityId{1});
}
