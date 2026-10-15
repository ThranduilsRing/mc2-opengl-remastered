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
            original.insert(original.find("[Objective0Action0]"), opaque);
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
    original.insert(original.find("[Objective0Action0]"), "Vendor { InternalTarget = 17; Child { Token = raw; } }\r\n");
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
