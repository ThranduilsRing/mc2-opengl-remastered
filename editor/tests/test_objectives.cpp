#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "CommandHistory.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <format>
#include <limits>

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
        "f MarkerX = 001.250000\r\nf MarkerY = -4.5\r\n"
        "b PreviousObjectiveMustBeComplete = TRUE\r\nul ActivateFlagIndex = 42\r\n"
        "b IsActive = TRUE\r\nf ActivationTime = 10.500\r\nb IsResolved = FALSE\r\nl ResolvedStatus = 1\r\nb ChangedStatus = FALSE\r\n"
        "x VendorKey = unparsed_extension\r\nul NumConditions = 1\r\nul NumActions = 1\r\nul NumFailureConditions = 1\r\nul NumFailureActions = 1\r\n"
        "[Objective0Condition0]\r\nst ConditionSpeciesString = \"VendorPredicate\"\r\nf PositionX = 001.25\r\nx Raw = retain_this\r\n"
        "[Objective0Action0]\r\nst ActionSpeciesString = \"SetBooleanFlag\"\r\nst FlagID = \"reward\"\r\nb Value = TRUE\r\n"
        "[Objective0FailureCondition0]\r\nst FailureConditionSpeciesString = \"DestroyEnemyUnitGroup\"\r\nul VendorGroup = 73\r\n"
        "[Objective0FailureAction0]\r\nul FailureActionSpecies = 200\r\nx Unknown = keep\r\n"
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
        std::string{}, std::int64_t{}, std::int64_t{-1}, std::int64_t{-1}, std::int64_t{-1}, 1.0, std::int64_t{-1}, std::int64_t{}};
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
        // Only the requested value or newly inserted declaration may differ from authored bytes.
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
        const auto wireValue = spec.type == 3 ? Scalar{std::string("\nChanged objective\nsecond line")} : value;
        CHECK(actual->value == Value{wireValue});
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
    reject(id, "Priority", std::int64_t{-1}); reject(id, "ResourcePoints", std::int64_t{2147483648LL});
    reject(id, "MarkerX", std::numeric_limits<double>::infinity()); reject(id, "ModelScale", 0.0);
    reject(id, "ModelType", std::int64_t{256}); reject(id, "Title", false);
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
    replace("ul Priority = 1", "st Priority = \"bad\"");
    replace("ul ResourcePoints = 700", "l ResourcePoints = -1");
    replace("st Title = \"Authored title\"", "st Title = \"" + std::string(2001, 'x') + "\"");
    replace("ul ActivateFlagIndex = 42", "f ActivateFlagIndex = 1.5");
    replace("b PreviousObjectiveMustBeComplete = TRUE", "l PreviousObjectiveMustBeComplete = 1");
    replace("ul Version = 2", "l Version = 2");
    replace("ul NumObjectives = 1", "l NumObjectives = 1");
    for (const auto key : {"NumConditions", "NumActions", "NumFailureConditions", "NumFailureActions"}) {
        replace(std::string(key) + " = 1", std::string(key) + " = 2");
        replace(std::string("ul ") + key + " = 1", std::string("f ") + key + " = 1.5");
        replace(std::string("ul ") + key + " = 1", std::string("l ") + key + " = 1");
    }
    replace("st ActionSpeciesString = \"SetBooleanFlag\"", "f ActionSpeciesString = 1.5");
    replace("st FlagID = \"reward\"\r\n", "");
    replace("st FlagID = \"reward\"", "x FlagID = reward");
    replace("st FlagID = \"reward\"", "st FlagID = \"" + std::string(2001, 'x') + "\"");
    replace("b Value = TRUE", "l Value = 1");
    replace("ul FailureActionSpecies = 200", "l FailureActionSpecies = 200");
    replace("st FlagID = \"reward\"", "l FlagIndex = 7\r\nst FlagID = \"reward\"");
    replace("st ConditionSpeciesString = \"VendorPredicate\"\r\n", "");
}

TEST_CASE("objectives/every scalar prefix rejects once without touching document or history")
{
    std::vector<std::string> logs;
    const auto log = [&](const auto& line) { logs.push_back(line); };
    auto doc = MissionDocument::decode(fixture(), packet(), "prefix", log); REQUIRE(doc);
    CommandHistory history(*doc);
    REQUIRE(history.setObjectiveScalar(doc->objectives()[0].section, "Priority", std::int64_t{2})); REQUIRE(history.undo());
    const auto reject = [&](std::string input, std::string_view field) {
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
        valid.insert(begin, declaration);
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
