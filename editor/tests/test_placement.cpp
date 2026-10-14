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
    return MissionDocument::read(stem.string() + ".fit", stem.string() + ".pak", [](const auto& s) { std::cout << s << '\n'; });
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
    auto reopened = MissionDocument::read(output.string() + ".fit", output.string() + ".pak", {}); REQUIRE(reopened);
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
        auto reopened = MissionDocument::read(output.string() + ".fit", output.string() + ".pak", {}); REQUIRE(reopened);
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

TEST_CASE("placement/marker transform and delete reject atomically before and after reopen") {
    const auto& paths = editor::tests::inputs();
    auto resolver = FileResolver::mount({paths.placementBase}); REQUIRE(resolver);
    auto policy = PlacementPolicy::load(*resolver); REQUIRE(policy);
    const auto catalog = ObjectCatalog::load(*resolver, {});
    auto doc = baseline(); REQUIRE(doc);
    auto plan = policy->make(*doc, entry(catalog, 290, "Nav1"), {0, 0, {{-4288, 4288, 200}, 4.5}}); REQUIRE(plan);
    CommandHistory placement(*doc);
    const auto id = plan->insert(placement); REQUIRE(id);
    const auto output = paths.placementOutput / "marker-rejected";
    REQUIRE(doc->save(output, {}));
    auto reopened = MissionDocument::read(output.string() + ".fit", output.string() + ".pak", {}); REQUIRE(reopened);
    for (auto* current : {&*doc, &*reopened}) {
        EntityId marker;
        for (const auto& e : current->entities())
            if (e.objectNumber == 290 && e.position[0] == -4288 && e.position[1] == 4288) marker = e.id;
        REQUIRE(marker.value != 0); CHECK(current->hasMarkerReference(marker));
        const auto fit = current->fitBytes();
        const auto pak = std::vector<std::byte>(current->pakBytes().begin(), current->pakBytes().end());
        const auto revision = current->revision();
        const auto modified = current->modified();
        std::vector<std::string> logs;
        CommandHistory history(*current, 32 * 1024 * 1024, [&](const auto& line) { logs.push_back(line); });
        // Rejections must retain an existing redo branch as well as document bytes.
        BuildingPlacement ordinary; ordinary.objectNumber = 263; ordinary.transform = {{100, 200, 200}, 0};
        REQUIRE(history.insertBuilding(ordinary)); REQUIRE(history.undo());
        const auto rejectedRevision = current->revision(), budget = history.bytesUsed();
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto transform = Transform{current->entity(marker)->position, current->entity(marker)->rotation};
            if (attempt == 0) transform.position[0] += 100;
            if (attempt == 2) transform.rotation = std::numeric_limits<double>::infinity();
            const auto result = attempt == 3 ? history.erase(marker) : history.transform(marker, transform);
            REQUIRE_FALSE(result); CHECK(result.error().reason == "marker_fit_sync_required");
            REQUIRE(logs.size() == static_cast<std::size_t>(attempt + 1));
            CHECK(logs.back() == std::string("[EDITOR_ENTITY_REJECT v1] action=") +
                (attempt == 3 ? "delete" : "transform") + " reason=marker_fit_sync_required");
            CHECK(current->fitBytes() == fit); CHECK(std::ranges::equal(current->pakBytes(), pak));
            CHECK(current->revision() == rejectedRevision); CHECK(current->modified() == modified);
            CHECK(history.bytesUsed() == budget); CHECK_FALSE(history.canUndo()); CHECK(history.canRedo());
        }
        CHECK(current->revision() > revision);
        REQUIRE(history.redo()); REQUIRE(history.undo());
        CHECK(current->fitBytes() == fit); CHECK(std::ranges::equal(current->pakBytes(), pak));
    }
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
    auto reopened = MissionDocument::read(paths.placementOutput / "mod-placement.fit", paths.placementOutput / "mod-placement.pak", {}); REQUIRE(reopened);
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
    auto reopened = MissionDocument::read(paths.placementOutput / "authored-unit.fit", paths.placementOutput / "authored-unit.pak", {}); REQUIRE(reopened);
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
        auto saved = MissionDocument::read(output.string() + ".fit", output.string() + ".pak", {}); REQUIRE(saved);
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
