#include "../../tests/unit/doctest.h"
#include "CommandHistory.h"
#include "TestInputs.h"
#include "FitReader.h"
#include "../../code/brain_missionfit_oporbd.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>

using namespace editor::document;
namespace {
MissionDocument fixture() {
    std::vector<std::byte> pak(156);
    const auto put = [&](std::size_t p, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) pak[p + i] = static_cast<std::byte>(value >> (i * 8));
    };
    put(0, 0xfeedface); put(4, 28); put(8, 28);
    for (int i = 1; i < 5; ++i) put(8 + i * 4, 0xe000009c);
    auto doc = MissionDocument::decode("FITini\r\n[MissionSettings]\r\nst MissionName=\"B4\"\r\nst Blurb2=\"\"\r\n"
        "uc scenarioTuneNum=0\r\n[Script]\r\nst ScenarioScript=\"unit\"\r\n[Parts]\r\nul NumParts=0\r\n"
        "[Objectives Version]\r\nul Version=2\r\n[Objectives]\r\nul NumObjectives=0\r\nFITend\r\n", std::move(pak), "B4", {});
    REQUIRE(doc); return std::move(*doc);
}
UnitPlacement unit() {
    UnitPlacement result;
    result.objectNumber = 1; result.csvFile = "test"; result.objectProfile = "test";
    result.warriorProfile = "pilot"; result.legacyBrain = "dredattack01";
    return result;
}
PatrolPath path() { return {42, "North patrol", true, 4, {{10, {12, -23, 4}}, {20, {-32, 45, 6}}}}; }
std::filesystem::path output(std::string name) {
    const auto* temp = std::getenv("TMP"); REQUIRE(temp);
    const auto dir = std::filesystem::path(temp) / "editor-b4-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}
std::vector<char> bytes(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary); REQUIRE(input);
    return {std::istreambuf_iterator<char>(input), {}};
}
}

TEST_CASE("B4/path multi-assignment point identities undo redo and reopen") {
    auto doc = fixture(); CommandHistory history(doc);
    auto first = history.insertUnit(unit()), second = history.insertUnit(unit()); REQUIRE(first); REQUIRE(second);
    const std::array ids{*first, *second};
    const auto original = doc.fitBytes(); const auto pak = std::vector(doc.pakBytes().begin(), doc.pakBytes().end());
    auto route = path();
    REQUIRE(history.setPatrolPath(ids, route));
    const auto authored = doc.fitBytes();
    const auto assignment = doc.brainAssignment(*first); REQUIRE(assignment);
    for (auto id : ids) { auto value = doc.patrolPath(id); REQUIRE(value); REQUIRE(*value); CHECK(**value == route); }
    REQUIRE(history.undo()); CHECK(doc.fitBytes() == original);
    REQUIRE(history.redo()); CHECK(doc.fitBytes() == authored);
    const auto revision = doc.revision(); REQUIRE(history.setPatrolPath(ids, route)); CHECK(doc.revision() == revision);
    std::ranges::reverse(route.points); route.loop = false; route.name = "Return";
    REQUIRE(history.setPatrolPath(ids, route));
    CHECK(*doc.brainAssignment(*first) == *assignment);
    auto reopened = MissionDocument::decode(doc.fitBytes(), pak, "reopen", {}); REQUIRE(reopened);
    for (auto id : ids) { auto value = reopened->patrolPath(id); REQUIRE(value); REQUIRE(*value); CHECK(**value == route); }
    REQUIRE(history.setPatrolPath(ids, std::nullopt));
    for (auto id : ids) { const auto value = doc.patrolPath(id); REQUIRE(value); CHECK_FALSE(value->has_value()); }
    REQUIRE(history.undo()); CHECK(**doc.patrolPath(*first) == route);
    CHECK(std::ranges::equal(doc.pakBytes(), pak));
}

TEST_CASE("B4/brain module cells definition and absence are reversible") {
    auto doc = fixture(); CommandHistory history(doc);
    const auto id = history.insertUnit(unit()); REQUIRE(id);
    const auto original = doc.fitBytes();
    auto brain = doc.brainAssignment(*id); REQUIRE(brain);
    const auto prior = *brain;
    brain->module = "project_patrol";
    brain->cells = std::vector<BrainCell>{{1, 0, 17}, {2, 1, 2.5}};
    brain->definition = "Brain {\r\n compatibilityMode = \"Enhanced\";\r\n EngageRadius = 300;\r\n"
        " PrimaryOPORD {\r\n type = Patrol;\r\n loop = true;\r\n Waypoint { index=0; x=12; y=23; }\r\n }\r\n}";
    REQUIRE(history.setBrainAssignment(*brain));
    auto read = doc.brainAssignment(*id); REQUIRE(read); CHECK(*read == *brain);
    // Keep the backing bytes alive for the production reader's borrowed spans.
    const auto wire = doc.fitBytes(); const FitReader runtimeReader(wire);
    CHECK(runtimeReader.read("Warrior1Cell0", "l", "Value") == Scalar{std::int64_t{17}});
    CHECK(runtimeReader.read("Warrior1Cell1", "f", "Value") == Scalar{2.5});
    MissionFitBrain parsed{}; REQUIRE(parseMissionFitBrains(wire.c_str(), &parsed, 1) == 1);
    CHECK(std::string(parsed.compatibilityMode) == "Enhanced"); CHECK(std::string(parsed.primary.type) == "Patrol");
    auto reopened = MissionDocument::decode(doc.fitBytes(), {doc.pakBytes().begin(), doc.pakBytes().end()}, "reopen", {}); REQUIRE(reopened);
    REQUIRE(reopened->brainAssignment(*id)); CHECK(*reopened->brainAssignment(*id) == *brain);
    REQUIRE(history.undo()); CHECK(doc.fitBytes() == original); CHECK(*doc.brainAssignment(*id) == prior);
    REQUIRE(history.redo());
    brain->module.reset(); brain->cells.reset(); brain->definition.reset();
    REQUIRE(history.setBrainAssignment(*brain)); CHECK(*doc.brainAssignment(*id) == *brain);
    REQUIRE(history.undo()); CHECK(doc.brainAssignment(*id)->definition.has_value());
}

TEST_CASE("B4/cell value edits preserve optional syntax and integer types") {
    auto doc = fixture(); CommandHistory initial(doc); const auto id = initial.insertUnit(unit()); REQUIRE(id);
    auto brain = doc.brainAssignment(*id); REQUIRE(brain);
    brain->cells = std::vector<BrainCell>{{0, 1, 17.5}}; REQUIRE(initial.setBrainAssignment(*brain));
    auto fit = doc.fitBytes();
    const auto point = fit.find("[Warrior1Cell0]"); REQUIRE(point != fit.npos);
    fit.insert(fit.find('\n', point) + 1, "// retained cell annotation\r\nst Extension = \"untouched\"\r\n");
    auto loaded = MissionDocument::decode(fit, {doc.pakBytes().begin(), doc.pakBytes().end()}, "extension", {}); REQUIRE(loaded);
    CommandHistory history(*loaded); brain = loaded->brainAssignment(*id); REQUIRE(brain);
    (*brain->cells)[0] = {0, 0, -19}; REQUIRE(history.setBrainAssignment(*brain));
    const auto wire = loaded->fitBytes(); const FitReader reader(wire);
    CHECK(reader.read("Warrior1Cell0", "l", "Value") == Scalar{std::int64_t{-19}});
    CHECK(wire.find("// retained cell annotation\r\nst Extension = \"untouched\"") != wire.npos);
    REQUIRE(history.undo()); CHECK(loaded->fitBytes() == fit);
    REQUIRE(history.redo()); CHECK(loaded->fitBytes() == wire);
}

TEST_CASE("B4/invalid edits retain document history and redo") {
    auto doc = fixture(); CommandHistory history(doc); auto id = history.insertUnit(unit()); REQUIRE(id);
    const std::array ids{*id}; REQUIRE(history.setPatrolPath(ids, path())); REQUIRE(history.undo());
    const auto before = doc.fitBytes(); const auto revision = doc.revision();
    auto route = path(); route.points[0].position[0] = std::numeric_limits<double>::infinity();
    CHECK_FALSE(history.setPatrolPath(ids, route));
    route = path(); route.points[1].id = route.points[0].id; CHECK_FALSE(history.setPatrolPath(ids, route));
    const std::array invalid{*id, EntityId{999}}; CHECK_FALSE(history.setPatrolPath(invalid, path()));
    auto brain = doc.brainAssignment(*id); REQUIRE(brain);
    brain->definition = "Brain { compatibilityMode=\"Enhanced\"; }\n[Injected]\nst Value=\"bad\"";
    CHECK_FALSE(history.setBrainAssignment(*brain));
    CHECK(doc.fitBytes() == before); CHECK(doc.revision() == revision); CHECK(history.canRedo());
}

TEST_CASE("B4/stock mc2_01 byte-identical save and authored round trip" * doctest::test_suite("stock-fixture")) {
    const auto& stock = editor::tests::inputs().stockDirectory;
    if (stock.empty()) { MESSAGE("--stock-dir required for stock mc2_01 proof"); return; }
    const auto fit = stock / "mc2_01.fit", pak = stock / "mc2_01.pak";
    auto doc = MissionDocument::read(fit, pak, {}, editor::tests::inputs().markerObjects);
    if (!doc) FAIL(doc.error().field << ": " << doc.error().reason);
    REQUIRE(doc);
    const auto target = output("mc2_01");
    REQUIRE(doc->save(target, {}));
    CHECK(bytes(target.string() + ".fit") == bytes(fit)); CHECK(bytes(target.string() + ".pak") == bytes(pak));
    const auto found = std::ranges::find_if(doc->entities(), [](const Entity& entity) { return entity.warrior.has_value(); });
    REQUIRE(found != doc->entities().end()); const auto id = found->id;
    const auto before = doc->fitBytes(); const auto assignment = doc->brainAssignment(id); REQUIRE(assignment);
    CommandHistory history(*doc); const std::array ids{id};
    REQUIRE(history.setPatrolPath(ids, path()));
    auto brain = *assignment; brain.module = "project_patrol"; REQUIRE(history.setBrainAssignment(brain));
    REQUIRE(doc->save(output("mc2_01_authored"), {}));
    auto reopened = MissionDocument::read(output("mc2_01_authored.fit"), output("mc2_01_authored.pak"), {}, doc->markerObjects()); REQUIRE(reopened);
    REQUIRE(reopened->patrolPath(id)); CHECK(**reopened->patrolPath(id) == path());
    REQUIRE(reopened->brainAssignment(id)); CHECK(*reopened->brainAssignment(id) == brain);
    REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(doc->fitBytes() == before);
    REQUIRE(doc->save(output("mc2_01_undo"), {}));
    CHECK(bytes(output("mc2_01_undo.fit")) == bytes(fit)); CHECK(bytes(output("mc2_01_undo.pak")) == bytes(pak));
}
