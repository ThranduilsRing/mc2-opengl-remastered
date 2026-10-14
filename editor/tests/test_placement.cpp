#include "../../tests/unit/doctest.h"
#include "PlacementPolicy.h"
#include "TestInputs.h"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <limits>
#include <chrono>
#include <zlib.h>
#include <nlohmann/json.hpp>

using namespace editor::document;
namespace {
auto baseline() {
    const auto stem = editor::tests::inputs().placementGoldens / "e5_reference_before";
    return MissionDocument::read(stem.string() + ".fit", stem.string() + ".pak", [](const auto& s) { std::cout << s << '\n'; }, editor::tests::inputs().markerObjects);
}
const CatalogEntry& entry(const ObjectCatalog& catalog, unsigned id, std::string_view name) {
    const auto found = std::ranges::find_if(catalog.entries(), [&](const auto& e) { return e.id == id && e.stem == name; });
    REQUIRE(found != catalog.entries().end()); return *found;
}
void cycle(MissionDocument& doc, const PlacementPlan& plan, const std::filesystem::path& output) {
    CommandHistory history(doc);
    const auto before = doc.fitBytes();
    const auto pak = std::vector<std::byte>(doc.pakBytes().begin(), doc.pakBytes().end());
    const auto id = plan.insert(history); REQUIRE(id);
    const auto inserted = doc.fitBytes();
    const auto changedPak = std::vector<std::byte>(doc.pakBytes().begin(), doc.pakBytes().end());
    REQUIRE(history.undo()); CHECK(doc.fitBytes() == before); CHECK(std::ranges::equal(doc.pakBytes(), pak));
    REQUIRE(history.redo()); CHECK(doc.fitBytes() == inserted); CHECK(std::ranges::equal(doc.pakBytes(), changedPak));
    REQUIRE(doc.save(output, {}));
    REQUIRE(history.undo()); CHECK(doc.entity(*id) == nullptr);
    auto reopened = MissionDocument::read(output.string() + ".fit", output.string() + ".pak", {}, editor::tests::inputs().markerObjects); REQUIRE(reopened);
    CHECK(reopened->fitBytes() == inserted); CHECK(std::ranges::equal(reopened->pakBytes(), changedPak));
}
}

TEST_CASE("placement/owned pilot resolver handles encoded tables missing profiles and malformed input") {
    const auto root = std::filesystem::path("scratch/e5b1-pilot-tests") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto write = [&](const std::string& key, const std::string& data) {
        const auto path = root / key; std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary); stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    };
    write("system.cfg", "FITini\n[FastFiles]\nl NumFastFiles = 0\nFITend\n");
    write("data/objects/pilots.csv", "missing\ngood\n\n");
    write("data/missions/warriors/good.fit", "FITini\n[General]\nl DescIndex = 42001 42003\nFITend\n");
    write("data/missions/warriors/bad.fit", "FITini\n[General]\nl DescIndex = 42001\nFITend\n");
    const std::string plain = "bad\n";
    std::string encoded(compressBound(static_cast<uLong>(plain.size())), '\0');
    uLongf size = static_cast<uLongf>(encoded.size());
    REQUIRE(compress2(reinterpret_cast<Bytef*>(encoded.data()), &size, reinterpret_cast<const Bytef*>(plain.data()),
        static_cast<uLong>(plain.size()), 6) == Z_OK);
    encoded.resize(size); write("data/objects/badpilots.csv", encoded);
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    auto policy = PlacementPolicy::load(*resolver); REQUIRE(policy);
    for (const auto& text : {std::string(256, 'x') + "\n", std::string("bad\"name\n"),
        std::string("good\n\nignored\n"), encoded + "garbage", std::string("missing\n")}) {
        write("data/objects/pilots.csv", text);
        std::vector<std::string> logs;
        auto rejected = PlacementPolicy::load(*resolver, [&](const auto& line) { logs.push_back(line); });
        CHECK_FALSE(rejected); CHECK(logs.size() == 1);
    }
    write("data/objects/pilots.csv", "good\n");
    write("data/missions/warriors/good.fit", "FITini\n[General]\nl DescIndex = invalid\nFITend\n");
    std::vector<std::string> logs;
    CHECK_FALSE(PlacementPolicy::load(*resolver, [&](const auto& line) { logs.push_back(line); })); CHECK(logs.size() == 1);
}

TEST_SUITE("placement-live") {
TEST_CASE("placement/all nine catalog families save undo redo and reopen") {
    const auto& paths = editor::tests::inputs();
    auto resolver = FileResolver::mount({paths.placementBase}); REQUIRE(resolver);
    auto policy = PlacementPolicy::load(*resolver, [](const auto& line) { std::cout << line << '\n'; }); REQUIRE(policy);
    const auto catalog = ObjectCatalog::load(*resolver, {});
    nlohmann::json reference;
    std::ifstream(paths.placementGoldens / "placements.json") >> reference;
    std::filesystem::create_directories(paths.placementOutput);
    for (const auto& [id, name] : std::vector<std::pair<unsigned, std::string>>{
        {53, "Anubis"}, {117, "APC"}, {263, "Barrels"}, {525, "AutoCannonCamo1"}, {252, "Birch1"},
        {157, "D_HeavyGate"}, {607, "bridge"}, {68, "Spotlight"}, {290, "Nav1"}}) {
        CAPTURE(name);
        auto doc = baseline(); REQUIRE(doc);
        const auto& object = entry(catalog, id, name);
        const auto& recorded = reference["placements"][std::string(familyName(object.family))];
        // The animated marker preview saved 4.5 degrees; replay that authored transform explicitly.
        const double rotation = recorded["building"].is_null() ? 0 : recorded["building"]["rotation"].get<double>();
        auto plan = policy->make(*doc, object, {0, 0, {{-4288, 4288, 200}, rotation}}); REQUIRE(plan);
        cycle(*doc, *plan, paths.placementOutput / ("e5_reference_" + std::string(familyName(object.family))));
        std::cout << "[EDITOR_LEGACY_PLACEMENT v1] family=" << familyName(object.family) << " fit_id=" << id <<
            " stem=" << name << " x=-4288 y=4288 z=200 input_team=0 team=" << (object.alignable ? 0 : -1) <<
            " painted=1 undo=1 redo=1 saved=1 removed=1 reopened=1\n";
    }
}

TEST_CASE("placement/player identity and remapped DefaultTeam retain separate defaults") {
    const auto& paths = editor::tests::inputs();
    auto resolver = FileResolver::mount({paths.placementBase}); REQUIRE(resolver);
    auto policy = PlacementPolicy::load(*resolver); REQUIRE(policy);
    const auto catalog = ObjectCatalog::load(*resolver, {});
    for (const auto player : {0, 1, 2}) for (const auto id : {53u, 117u}) {
        CAPTURE(player); CAPTURE(id);
        auto doc = baseline(); REQUIRE(doc); CommandHistory history(*doc);
        const auto* section = doc->find("Player" + std::to_string(player), "DefaultTeam"); REQUIRE(section);
        for (const auto& s : doc->syntax().sections) if (s.name == "Player" + std::to_string(player))
            REQUIRE(history.setPlayerTeam(s.id, player == 0 ? 3 : 0));
        const auto& object = entry(catalog, id, id == 53 ? "Anubis" : "APC");
        auto plan = policy->make(*doc, object, {player, player == 0 ? 3 : 0, {{-100, 100, 200}, 0}}); REQUIRE(plan);
        const auto& unit = std::get<UnitPlacement>(plan->value);
        CHECK(unit.playerPart == (player == 0)); CHECK(unit.commander == player); CHECK(unit.team == (player == 0 ? 3 : 0));
        CHECK(unit.objectProfile == (id == 53 ? (player == 0 ? "PM207300" : "PM101100") : (player == 0 ? "pv20600" : "pv20500")));
        CHECK(unit.legacyBrain == (player == 0 ? "PBrain" : "DredAttack01"));
        CHECK(unit.cells.size() == (player == 0 ? 0 : 3));
        CHECK(plan->sources.back().key == "data/missions/warriors/" + identifier(unit.warriorProfile) + ".fit");
        const auto output = paths.placementOutput / ("player" + std::to_string(player) + "-" + std::to_string(id));
        cycle(*doc, *plan, output);
        auto reopened = MissionDocument::read(output.string() + ".fit", output.string() + ".pak", {}, editor::tests::inputs().markerObjects); REQUIRE(reopened);
        const auto* cells = reopened->find("Warrior30", "NumCells"); REQUIRE(cells);
        CHECK(std::get<std::int64_t>(cells->value[0]) == (player == 0 ? 0 : 3));
        if (player != 0) {
            CHECK(std::get<std::int64_t>(reopened->find("Warrior30Cell0", "Cell")->value[0]) == 2);
            CHECK(std::get<double>(reopened->find("Warrior30Cell0", "Value")->value[0]) == 150);
            CHECK(std::get<std::int64_t>(reopened->find("Warrior30Cell1", "Cell")->value[0]) == 9);
            CHECK(std::get<std::int64_t>(reopened->find("Warrior30Cell2", "Cell")->value[0]) == 28);
        }
    }
}

TEST_CASE("placement/marker transform delete and renumber have exact atomic history") {
    const auto& paths = editor::tests::inputs();
    auto doc = baseline(); REQUIRE(doc);
    std::vector<std::string> synchronizationLogs;
    CommandHistory history(*doc, 32 * 1024 * 1024, [&](const auto& line) { synchronizationLogs.push_back(line); });
    const auto baseCount = std::get<std::int64_t>(doc->find("NavMarkers", "NumNavMarkers")->value[0]);
    BuildingPlacement value; value.objectNumber = 290; value.markerRadius = 256;
    value.transform = {{-4288, 4288, 200}, 4.5};
    const auto one = history.insertBuilding(value); REQUIRE(one);
    value.transform.position[0] = -3000; value.markerRadius = 512;
    const auto two = history.insertBuilding(value); REQUIRE(two);
    const auto secondBlock = doc->markerReference(*two); REQUIRE(secondBlock); REQUIRE(*secondBlock);
    const auto secondRadius = doc->find(**secondBlock, "radius")->id;
    const auto fit = doc->fitBytes();
    const auto pak = std::vector<std::byte>(doc->pakBytes().begin(), doc->pakBytes().end());
    const auto revision = doc->revision(), budget = history.bytesUsed();
    CHECK_FALSE(history.transform(*one, value.transform));
    auto invalid = value.transform; invalid.rotation = std::numeric_limits<double>::infinity();
    CHECK_FALSE(history.transform(*one, invalid));
    CHECK(synchronizationLogs.size() == 2);
    CHECK(doc->fitBytes() == fit); CHECK(std::ranges::equal(doc->pakBytes(), pak));
    CHECK(doc->revision() == revision); CHECK(history.bytesUsed() == budget);
    const Transform moved{{-4000, 4000, 250}, 25};
    REQUIRE(history.transform(*one, moved));
    const auto block = "NavMarker" + std::to_string(baseCount);
    CHECK(doc->find(block, "xPos")->value == Value{double{-4000}});
    CHECK(doc->find(block, "yPos")->value == Value{double{4000}});
    CHECK(doc->find(block, "radius")->value == Value{double{256}});
    REQUIRE(doc->save(paths.placementOutput / "marker-moved", {}));
    const auto movedFit = doc->fitBytes();
    const auto movedPak = std::vector<std::byte>(doc->pakBytes().begin(), doc->pakBytes().end());
    const auto secondRecord = *doc->entity(*two)->packetRecord;
    REQUIRE(history.undo()); CHECK(doc->fitBytes() == fit); CHECK(std::ranges::equal(doc->pakBytes(), pak));
    REQUIRE(history.redo()); CHECK(doc->fitBytes() == movedFit); CHECK(std::ranges::equal(doc->pakBytes(), movedPak));
    REQUIRE(history.erase(*one));
    CHECK(doc->find("NavMarkers", "NumNavMarkers")->value == Value{baseCount + 1});
    CHECK(doc->find(block, "radius")->id == secondRadius);
    const auto remainingBlock = doc->markerReference(*two); REQUIRE(remainingBlock);
    CHECK(*remainingBlock == *secondBlock);
    REQUIRE(doc->save(paths.placementOutput / "marker-deleted", {}));
    const auto deletedFit = doc->fitBytes();
    const auto deletedPak = std::vector<std::byte>(doc->pakBytes().begin(), doc->pakBytes().end());
    REQUIRE(history.undo()); CHECK(doc->fitBytes() == movedFit); CHECK(std::ranges::equal(doc->pakBytes(), movedPak));
    REQUIRE(history.redo()); CHECK(doc->fitBytes() == deletedFit); CHECK(std::ranges::equal(doc->pakBytes(), deletedPak));
    auto reopened = MissionDocument::read(paths.placementOutput / "marker-deleted.fit", paths.placementOutput / "marker-deleted.pak", {}, editor::tests::inputs().markerObjects);
    REQUIRE(reopened);
    CommandHistory reopenedHistory(*reopened);
    std::vector<EntityId> markers;
    for (const auto& e : reopened->entities()) if (reopened->hasMarkerReference(e.id)) markers.push_back(e.id);
    REQUIRE(!markers.empty());
    for (const auto id : markers) REQUIRE(reopenedHistory.erase(id));
    CHECK(reopened->find("NavMarkers", "NumNavMarkers")->value == Value{std::int64_t{0}});
    REQUIRE(reopened->save(paths.placementOutput / "marker-empty", {}));
    REQUIRE(reopenedHistory.undo()); REQUIRE(reopenedHistory.redo());
    // A packet-only move must fail the same reader boundary as a malformed external mission.
    auto corrupted = movedFit;
    const auto field = corrupted.find("f xPos = -4000"); REQUIRE(field != corrupted.npos);
    corrupted.replace(field, std::string("f xPos = -4000").size(), "f xPos = -2000");
    std::vector<std::string> logs;
    CHECK_FALSE(MissionDocument::decode(corrupted, movedPak, "marker-desync", [&](const auto& s) { logs.push_back(s); }, doc->markerObjects()));
    REQUIRE(logs.size() == 1); CHECK(logs[0].find("marker_packet_desync") != std::string::npos);
    auto guardedDocument = baseline(); REQUIRE(guardedDocument);
    std::vector<std::string> rejected;
    CommandHistory guarded(*guardedDocument, 32 * 1024 * 1024, [&](const auto& s) { rejected.push_back(s); });
    value.markerRadius = 256;
    const auto marker = guarded.insertBuilding(value); REQUIRE(marker);
    // Ordinary buildings at the same XY cannot acquire a marker's FIT radius.
    BuildingPlacement coincident; coincident.objectNumber = 263; coincident.transform = value.transform;
    const auto building = guarded.insertBuilding(coincident); REQUIRE(building);
    CHECK_FALSE(guardedDocument->hasMarkerReference(*building));
    REQUIRE(guardedDocument->markerReference(*marker));
    REQUIRE(guarded.transform(*marker, moved));
    REQUIRE(guarded.transform(*marker, coincident.transform));
    REQUIRE(guarded.transform(*marker, moved));
    BuildingPlacement extra; extra.objectNumber = 263;
    const auto reserved = guarded.insertBuilding(extra); REQUIRE(reserved); REQUIRE(guarded.undo());
    const auto before = guardedDocument->fitBytes();
    const auto beforePak = std::vector<std::byte>(guardedDocument->pakBytes().begin(), guardedDocument->pakBytes().end());
    const auto guardedRevision = guardedDocument->revision(), guardedBudget = guarded.bytesUsed();
    const auto modified = guardedDocument->modified();
    value.markerRadius.reset();
    for (const auto position : {moved.position, value.transform.position}) {
        value.transform.position = position;
        rejected.clear();
        CHECK_FALSE(guarded.insertBuilding(value));
        REQUIRE(rejected.size() == 1); CHECK(rejected[0].find("marker_count_mismatch") != std::string::npos);
        CHECK(guardedDocument->fitBytes() == before); CHECK(std::ranges::equal(guardedDocument->pakBytes(), beforePak));
        CHECK(guardedDocument->revision() == guardedRevision); CHECK(guardedDocument->modified() == modified);
        CHECK(guarded.bytesUsed() == guardedBudget); CHECK(guarded.canRedo());
    }
    // Two catalog markers at the same XY are ambiguous even when both FIT counts agree.
    auto movedArchive = decodePak(movedPak); REQUIRE(movedArchive);
    auto placements = movedArchive->packets[1].decoded;
    const auto record = 4 + secondRecord * 40;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(moved.position[axis]));
        for (unsigned byte = 0; byte < 4; ++byte) placements[record + 4 + axis * 4 + byte] = static_cast<std::byte>(bits >> (8 * byte));
    }
    auto ambiguousPak = encodePlacements(*movedArchive, placements); REQUIRE(ambiguousPak);
    rejected.clear();
    CHECK_FALSE(guarded.replace(movedFit, *ambiguousPak, "marker-ambiguity", [&](const auto& s) { rejected.push_back(s); }));
    REQUIRE(rejected.size() == 1); CHECK(rejected[0].find("ambiguous_marker_reference") != std::string::npos);
    CHECK(guardedDocument->fitBytes() == before); CHECK(std::ranges::equal(guardedDocument->pakBytes(), beforePak));
    CHECK(guardedDocument->revision() == guardedRevision); CHECK(guardedDocument->modified() == modified);
    CHECK(guarded.bytesUsed() == guardedBudget); CHECK(guarded.canRedo());
    const auto next = guarded.insertBuilding(extra); REQUIRE(next);
    CHECK(next->value == reserved->value + 1);
}

TEST_CASE("placement/mod override is inserted and keeps resolved provenance") {
    const auto& paths = editor::tests::inputs();
    auto resolver = FileResolver::mount({paths.placementBase, paths.placementMods, "MC2X-CVE-G"}); REQUIRE(resolver);
    auto policy = PlacementPolicy::load(*resolver); REQUIRE(policy);
    const auto catalog = ObjectCatalog::load(*resolver, {});
    const auto found = std::ranges::find_if(catalog.entries(), [](const auto& e) { return e.id == 2; }); REQUIRE(found != catalog.entries().end());
    CHECK(found->appearance == "Starslayer"); CHECK(found->definition.layer == "mod:MC2X-CVE-G");
    auto doc = baseline(); REQUIRE(doc);
    auto plan = policy->make(*doc, *found, {0, 0, {{-100, 100, 200}, 0}}); REQUIRE(plan);
    cycle(*doc, *plan, paths.placementOutput / "mod-placement");
    auto reopened = MissionDocument::read(paths.placementOutput / "mod-placement.fit", paths.placementOutput / "mod-placement.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(reopened);
    CHECK(reopened->entities().back().objectNumber == 2);
    nlohmann::json report{{"id", found->id}, {"stem", found->stem}, {"appearance", found->appearance}, {"sources", nlohmann::json::array()}};
    for (const auto& source : plan->sources) {
        report["sources"].push_back({{"key", source.key}, {"layer", source.layer}, {"source", source.source.generic_string()}});
        auto resolved = resolver->read(source.key); REQUIRE(resolved);
        const auto path = paths.placementOutput / "provenance" / source.key;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(resolved->bytes.data()), static_cast<std::streamsize>(resolved->bytes.size()));
    }
    std::ofstream(paths.placementOutput / "mod-placement.json") << report.dump(2) << '\n';
}

TEST_CASE("placement/authored unit values and forest instance encoding survive saving") {
    const auto& paths = editor::tests::inputs();
    auto resolver = FileResolver::mount({paths.placementBase}); REQUIRE(resolver);
    auto policy = PlacementPolicy::load(*resolver); REQUIRE(policy);
    const auto catalog = ObjectCatalog::load(*resolver, {});
    auto doc = baseline(); REQUIRE(doc);
    auto plan = policy->make(*doc, entry(catalog, 53, "Anubis"), {0, 0, {{100, 100, 200}, 15}, 3}); REQUIRE(plan);
    auto& unit = std::get<UnitPlacement>(plan->value);
    unit.damage = 2; unit.baseColor = 0x123456; unit.highlightColor1 = 0x654321; unit.highlightColor2 = 0xffffff;
    unit.selfRepair = false;
    cycle(*doc, *plan, paths.placementOutput / "authored-unit");
    auto reopened = MissionDocument::read(paths.placementOutput / "authored-unit.fit", paths.placementOutput / "authored-unit.pak", {}, editor::tests::inputs().markerObjects); REQUIRE(reopened);
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::int64_t>>{
        {"VariantNumber", 3}, {"BaseColor", 0x123456}, {"HighlightColor1", 0x654321}, {"HighlightColor2", 0xffffff}, {"SelfRepairBehavior", 0}})
        CHECK(std::get<std::int64_t>(reopened->find("Part30", key)->value[0]) == value);
    CHECK(std::get<double>(reopened->find("Part30", "Damage")->value[0]) == 2);
    for (const auto instance : {-1, 7}) {
        auto buildingDoc = baseline(); REQUIRE(buildingDoc);
        CommandHistory mapping(*buildingDoc);
        for (const auto& s : buildingDoc->syntax().sections) if (s.name == "Player0") REQUIRE(mapping.setPlayerTeam(s.id, 3));
        auto buildingPlan = policy->make(*buildingDoc, entry(catalog, 525, "AutoCannonCamo1"),
            {0, 3, {{100, 100, 200}, 0}, 0, instance, 12}); REQUIRE(buildingPlan);
        CHECK(std::get<BuildingPlacement>(buildingPlan->value).team == 0);
        const auto output = paths.placementOutput / ("forest" + std::to_string(instance));
        cycle(*buildingDoc, *buildingPlan, output);
        auto saved = MissionDocument::read(output.string() + ".fit", output.string() + ".pak", {}, editor::tests::inputs().markerObjects); REQUIRE(saved);
        const auto& bytes = saved->archive().packets[1].decoded;
        const auto word = [&](std::size_t p) {
            std::uint32_t value{}; for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[p + i]) << (8 * i); return value;
        };
        CHECK(word(bytes.size() - 8) == static_cast<std::uint32_t>(instance + 1));
        CHECK(word(bytes.size() - 4) == (instance == -1 ? 0u : 12u));
    }
}

TEST_CASE("placement/invalid inputs reject atomically without truncation") {
    auto doc = baseline(); REQUIRE(doc);
    const auto& paths = editor::tests::inputs();
    auto resolver = FileResolver::mount({paths.placementBase}); REQUIRE(resolver);
    auto policy = PlacementPolicy::load(*resolver); REQUIRE(policy);
    const auto catalog = ObjectCatalog::load(*resolver, {});
    const auto& object = entry(catalog, 53, "Anubis");
    const auto before = doc->fitBytes(); const auto revision = doc->revision();
    for (int i = 0; i < 6; ++i) {
        auto broken = object; PlacementChoice choice;
        if (i == 0) broken.stem = std::string(256, 'x');
        if (i == 1) broken.stem = "bad\"name";
        if (i == 2) choice.player = -1;
        if (i == 3) choice.team = 3;
        if (i == 4) choice.variant = 16;
        if (i == 5) choice.transform.rotation = std::numeric_limits<double>::infinity();
        std::vector<std::string> logs;
        CHECK_FALSE(policy->make(*doc, broken, choice, [&](const auto& s) { logs.push_back(s); }));
        CHECK(logs.size() == 1); CHECK(doc->fitBytes() == before); CHECK(doc->revision() == revision);
    }
    auto plan = policy->make(*doc, object, {}); REQUIRE(plan); auto unit = std::get<UnitPlacement>(plan->value);
    std::vector<std::string> rejected;
    CommandHistory history(*doc, 32 * 1024 * 1024, [&](const auto& line) { rejected.push_back(line); });
    unit.warriorProfile = std::string(256, 'x'); CHECK_FALSE(history.insertUnit(unit));
    unit.warriorProfile = "pilot"; unit.cells = {{2, 1, 150}, {2, 0, 0}}; CHECK_FALSE(history.insertUnit(unit));
    CHECK(rejected.size() == 2);
    CHECK(doc->fitBytes() == before); CHECK(doc->revision() == revision); CHECK_FALSE(history.canUndo());
}
}
