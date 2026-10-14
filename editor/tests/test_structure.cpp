#include "../../tests/unit/doctest.h"
#include "CommandHistory.h"
#include "../../code/brain_missionfit_oporbd.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <zlib.h>

using namespace editor::document;
namespace {
void put(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>(value >> (8 * i));
}
std::vector<std::byte> compressed(const std::vector<std::byte>& bytes)
{
    std::vector<std::byte> result(4 + compressBound(static_cast<uLong>(bytes.size())));
    uLongf size = static_cast<uLongf>(result.size() - 4);
    REQUIRE(compress2(reinterpret_cast<Bytef*>(result.data() + 4), &size,
        reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uLong>(bytes.size()), Z_BEST_COMPRESSION) == Z_OK);
    put(result, 0, static_cast<std::uint32_t>(bytes.size()));
    result.resize(4 + size);
    return result;
}
std::vector<std::byte> pak(bool checksum = false, bool compressPlacements = false)
{
    std::vector<std::byte> terrain(128), placements(84);
    put(placements, 0, 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto p = 4 + i * 40;
        put(placements, p, static_cast<std::uint32_t>(10 + i));
        put(placements, p + 20, static_cast<std::uint32_t>(20 + i));
        put(placements, p + 28, 0xffffffffU);
        put(placements, p + 32, static_cast<std::uint32_t>(30 + i));
        put(placements, p + 36, static_cast<std::uint32_t>(40 + i));
    }
    std::vector<std::byte> bytes(28);
    put(bytes, 0, 0xfeedfaceU); put(bytes, 4, 28);
    const std::array<std::vector<std::byte>, 5> packets{compressed(terrain),
        compressPlacements ? compressed(placements) : placements, compressed(std::vector<std::byte>(64, std::byte{0x42})), {}, {}};
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto storage = i == 0 || i == 2 || (i == 1 && compressPlacements) ? 4U : i > 2 ? 7U : 0U;
        put(bytes, 8 + i * 4, static_cast<std::uint32_t>(bytes.size()) | (storage << 29));
        bytes.insert(bytes.end(), packets[i].begin(), packets[i].end());
    }
    if (checksum) {
        std::uint32_t sum{};
        for (std::size_t i = 4; i < bytes.size(); ++i) sum += std::to_integer<unsigned>(bytes[i]);
        put(bytes, 0, sum);
    }
    return bytes;
}
std::string fit()
{
    return "FITini\r\n[MissionSettings]\r\nst MissionName = \"Structural fixture\" // retained\r\n"
        "st Blurb2 = \"\"\r\nuc scenarioTuneNum = 0\r\n[Script]\r\nst ScenarioScript = \"unit\"\r\n"
        "[Objectives Version]\r\nul Version = 2\r\n[Objectives]\r\nul NumObjectives = 0\r\n"
        "[Parts]\r\nul NumParts = 2\r\n"
        "[Part1]\r\nul ObjectNumber = 1\r\nf PositionX = 0.000\r\nf PositionY = 0.000\r\nul Pilot = 2\r\n"
        "[Part2]\r\nul ObjectNumber = 2\r\nf PositionX = 0.000\r\nf PositionY = 0.000\r\nul Pilot = 1\r\n"
        "[Warriors]\r\nul NumWarriors = 2\r\n"
        "[Warrior1]\r\nst Profile = \"pilot1\"\r\nst Brain = \"legacy1\"\r\n"
        "Brain {\r\n compatibilityMode = \"Legacy\";\r\n}\r\n"
        "[Warrior2]\r\nst Profile = \"pilot2\"\r\nst Brain = \"legacy2\" // module\r\nul NumCells = 1\r\n"
        "Brain {\r\n // hybrid comment retained exactly\r\n compatibilityMode = \"Hybrid\";\r\n"
        " UnknownSwitch = strange;\r\n PrimaryOPORD { type = Patrol; compatibilityMode = \"Enhanced\"; }\r\n}\r\n"
        "[Warrior2Cell0]\r\nul Cell = 0\r\nul MemType = 1\r\nf Value = 17.500\r\n"
        "[Extension]\r\nBrain { compatibilityMode = \"Enhanced\"; }\r\nFITend\r\n";
}
MissionDocument document(bool checksum = false, bool compressPlacements = false)
{
    auto loaded = MissionDocument::decode(fit(), pak(checksum, compressPlacements), "structural-fixture", {});
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
}
std::vector<std::byte> bytes(const MissionDocument& document)
{
    return {document.pakBytes().begin(), document.pakBytes().end()};
}
std::vector<std::byte> fileBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const auto size = input.tellg(); REQUIRE(size >= 0);
    std::vector<std::byte> output(static_cast<std::size_t>(size));
    input.seekg(0); input.read(reinterpret_cast<char*>(output.data()), size); REQUIRE(input);
    return output;
}
std::filesystem::path scratch()
{
    auto path = std::filesystem::path("scratch/e3a-document-tests") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(path);
    return path;
}
void unchangedPackets(const PacketArchive& original, const std::vector<std::byte>& output)
{
    const auto changed = decodePak(output);
    REQUIRE(changed.has_value());
    REQUIRE(changed->packets.size() == original.packets.size());
    for (std::size_t i = 0; i < original.packets.size(); ++i) {
        if (i == 1) continue;
        const auto a = original.packets[i].encoded, b = changed->packets[i].encoded;
        CHECK(original.packets[i].storage == changed->packets[i].storage);
        CHECK(std::ranges::equal(std::span(original.originalBytes).subspan(a.begin, a.end - a.begin),
            std::span(output).subspan(b.begin, b.end - b.begin)));
    }
}
UnitPlacement newUnit()
{
    UnitPlacement unit;
    unit.objectNumber = 123; unit.objectProfile = "PM1"; unit.csvFile = "unit.csv";
    unit.warriorProfile = "pilot3"; unit.legacyBrain = "dredattack01";
    unit.transform = {{12.5, -9.25, 6}, 90}; unit.team = 2;
    return unit;
}
}

TEST_CASE("document/explicit Pilot and scoped nested Brain identity")
{
    auto doc = document();
    REQUIRE(doc.entities().size() == 4);
    const auto& unit = doc.entities()[2];
    REQUIRE(unit.warrior.has_value()); REQUIRE(unit.brain.has_value());
    CHECK(doc.section(*unit.warrior)->name == "Warrior2");
    CHECK(doc.section(*unit.brain)->parent == unit.warrior);
    CHECK(std::get<std::string>(doc.find(*unit.warrior, "Brain")->value[0]) == "legacy2");
    CHECK(std::get<std::string>(doc.find(*unit.brain, "compatibilityMode")->value[0]) == "Hybrid");
    CHECK(doc.find("Warrior2", "compatibilityMode") == nullptr);
    CHECK(doc.find(*unit.warrior, "NumCells") != nullptr);
    CHECK(doc.find(*unit.brain, "NumCells") == nullptr);
    CHECK(doc.fitBytes() == fit()); CHECK(bytes(doc) == pak());
}

TEST_CASE("document/panel commands change saved output and restore bytes through undo redo")
{
    auto input = fit(); input.insert(input.find("FITend"), "[Player0]\r\nul DefaultTeam = 0 // alignment\r\n");
    for (const auto operation : {"unit-team", "building-team", "player-team", "brain-mode", "legacy-module"}) {
        CAPTURE(operation);
        auto loaded = MissionDocument::decode(input, pak(true, true), "panel-fixture", {}); REQUIRE(loaded);
        auto& doc = *loaded; CommandHistory history(doc);
        const auto unit = doc.entities()[2].id, building = doc.entities()[0].id;
        const auto warrior = doc.entity(unit)->warrior, brain = doc.entity(unit)->brain;
        const auto cells = doc.find("Warrior2Cell0", "Value")->id;
        const auto originalPak = bytes(doc);
        if (std::string_view(operation) == "unit-team") REQUIRE(history.setTeam(unit, 3));
        if (std::string_view(operation) == "building-team") REQUIRE(history.setTeam(building, -1));
        if (std::string_view(operation) == "player-team") {
            NodeId player;
            for (const auto& section : doc.syntax().sections) if (section.name == "Player0") player = section.id;
            REQUIRE(history.setPlayerTeam(player, 3));
        }
        if (std::string_view(operation) == "brain-mode") REQUIRE(history.setBrainMode(unit, "Enhanced"));
        if (std::string_view(operation) == "legacy-module") REQUIRE(history.setLegacyBrain(unit, "project_brain"));
        const auto changedFit = doc.fitBytes(); const auto changedPak = bytes(doc);
        CHECK((changedFit != input || changedPak != originalPak));
        CHECK(doc.entity(unit)->warrior == warrior); CHECK(doc.entity(unit)->brain == brain);
        CHECK(doc.find("Warrior2Cell0", "Value")->id == cells);
        CHECK(changedFit.find("// hybrid comment retained exactly\r\n") != std::string::npos);
        CHECK(changedFit.find("UnknownSwitch = strange;") != std::string::npos);
        CHECK(changedFit.find("PrimaryOPORD { type = Patrol; compatibilityMode = \"Enhanced\"; }") != std::string::npos);
        CHECK(changedFit.find("f Value = 17.500\r\n") != std::string::npos);
        const auto dir = scratch(); REQUIRE(doc.save(dir / "panel-edited", {}));
        auto reopen = MissionDocument::read(dir / "panel-edited.fit", dir / "panel-edited.pak", {}); REQUIRE(reopen);
        CHECK(reopen->fitBytes() == changedFit); CHECK(bytes(*reopen) == changedPak);
        if (std::string_view(operation) == "unit-team") CHECK(reopen->entities()[2].team == 3);
        if (std::string_view(operation) == "building-team") CHECK(reopen->entities()[0].team == -1);
        if (std::string_view(operation) == "player-team") CHECK(reopen->find("Player0", "DefaultTeam")->value == Value{std::int64_t{3}});
        REQUIRE(history.undo()); REQUIRE(doc.save(dir / "panel-undone", {}));
        CHECK(fileBytes(dir / "panel-undone.fit") == std::vector<std::byte>(reinterpret_cast<const std::byte*>(input.data()), reinterpret_cast<const std::byte*>(input.data() + input.size())));
        CHECK(fileBytes(dir / "panel-undone.pak") == originalPak);
        REQUIRE(history.redo()); CHECK(doc.fitBytes() == changedFit); CHECK(bytes(doc) == changedPak);
        REQUIRE(history.undo()); CHECK(doc.fitBytes() == input);
    }
}

TEST_CASE("document/three brain modes handle absent block and scoped field insertion losslessly")
{
    for (int absent = 0; absent < 3; ++absent) {
        auto input = fit();
        const auto p = input.find(" compatibilityMode = \"Hybrid\";");
        if (absent == 1) input.erase(p, std::string(" compatibilityMode = \"Hybrid\";\r\n").size());
        if (absent == 2) {
            const auto begin = input.find("Brain {", input.find("[Warrior2]"));
            input.erase(begin, input.find("[Warrior2Cell0]") - begin);
        }
        auto loaded = MissionDocument::decode(input, pak(), "brain-fixture", {}); REQUIRE(loaded);
        CommandHistory history(*loaded); const auto id = loaded->entities()[2].id;
        for (const auto* mode : {"Enhanced", "Hybrid", "Legacy"}) {
            REQUIRE(history.setBrainMode(id, mode));
            if (absent == 2 && std::string_view(mode) == "Legacy") { CHECK(loaded->fitBytes() == input); continue; }
            const auto* entity = loaded->entity(id); REQUIRE(entity->brain);
            CHECK(loaded->find(*entity->brain, "compatibilityMode")->value == Value{std::string(mode)});
            CHECK(loaded->find(*entity->warrior, "Brain")->value == Value{std::string("legacy2")});
            CHECK(loaded->fitBytes().find("f Value = 17.500") != std::string::npos);
            const auto changed = loaded->fitBytes(); const auto dir = scratch(); REQUIRE(loaded->save(dir / "mode", {}));
            auto reopened = MissionDocument::read(dir / "mode.fit", dir / "mode.pak", {}); REQUIRE(reopened);
            CHECK(reopened->find(*reopened->entities()[2].brain, "compatibilityMode")->value == Value{std::string(mode)});
            const auto hybridOriginal = reopened->fitBytes(); REQUIRE(reopened->save(dir / "mode-roundtrip", {}));
            auto second = MissionDocument::read(dir / "mode-roundtrip.fit", dir / "mode-roundtrip.pak", {}); REQUIRE(second);
            CHECK(second->fitBytes() == hybridOriginal); CHECK(bytes(*second) == pak());
            CHECK(fileBytes(dir / "mode-roundtrip.fit") == fileBytes(dir / "mode.fit"));
            CHECK(fileBytes(dir / "mode-roundtrip.pak") == fileBytes(dir / "mode.pak"));
            if (changed == input) continue;
            REQUIRE(history.undo()); CHECK(loaded->fitBytes() == input);
            REQUIRE(history.redo()); CHECK(loaded->fitBytes() == changed);
            REQUIRE(history.undo()); CHECK(loaded->fitBytes() == input);
        }
    }
}

TEST_CASE("document/saved brain switches are consumed by the game parser")
{
    for (const auto* nl : {"\n", "\r\n"}) {
        for (const auto* mode : {"Legacy", "Hybrid", "Enhanced"}) {
            CAPTURE(nl, mode);
            auto input = fit();
            const auto begin = input.find("Brain {", input.find("[Warrior2]"));
            const auto end = input.find("[Warrior2Cell0]");
            const std::string before = "Brain { UnknownSwitch = strange; ";
            const std::string after = " PrimaryOPORD { type = Patrol; } }" + std::string(nl);
            input.replace(begin, end - begin, before + "compatibilityMode = \"Legacy\";" + after);
            if (std::string_view(nl) == "\n") std::erase(input, '\r');
            auto loaded = MissionDocument::decode(input, pak(), "inline-brain", {}); REQUIRE(loaded);
            CommandHistory history(*loaded);
            REQUIRE(history.setBrainMode(loaded->entities()[2].id, mode));
            const auto expected = before + nl + "compatibilityMode = \"" + mode + "\";" + nl + after;
            CHECK(loaded->fitBytes().find(expected) != std::string::npos);
            const auto dir = scratch(); REQUIRE(loaded->save(dir / "game-mode", {}));
            const auto saved = fileBytes(dir / "game-mode.fit");
            const std::string text(reinterpret_cast<const char*>(saved.data()), saved.size());
            std::array<MissionFitBrain, 4> brains;
            const auto count = parseMissionFitBrains(text.c_str(), brains.data(), static_cast<int>(brains.size()));
            const auto brain = std::find_if(brains.begin(), brains.begin() + count,
                [](const auto& record) { return record.warriorIndex == 2; });
            REQUIRE(brain != brains.begin() + count);
            CHECK(std::string(brain->compatibilityMode) == mode);
            REQUIRE(history.undo()); CHECK(loaded->fitBytes() == input);
        }
    }
}

TEST_CASE("document/panel readers and invalid commands reject once without mutation")
{
    for (const auto* mode : {"Alien", "123", ""}) {
        auto input = fit(); input.replace(input.find("\"Hybrid\""), 8, std::string("\"") + mode + "\"");
        std::vector<std::string> logs;
        CHECK_FALSE(MissionDocument::decode(input, pak(), "bad-mode", [&](const auto& line) { logs.push_back(line); }));
        CHECK(logs.size() == 1);
    }
    for (const auto* value : {"ul DefaultTeam = 8", "st DefaultTeam = \"bad\"", "l DefaultTeam = -1"}) {
        auto input = fit(); input.insert(input.find("FITend"), std::string("[Player0]\r\n") + value + "\r\n");
        std::vector<std::string> logs;
        CHECK_FALSE(MissionDocument::decode(input, pak(), "bad-player", [&](const auto& line) { logs.push_back(line); }));
        CHECK(logs.size() == 1);
    }
    auto doc = document(); CommandHistory history(doc); const auto id = doc.entities()[2].id;
    REQUIRE(history.setTeam(id, 2)); REQUIRE(history.undo());
    const auto revision = doc.revision(), cost = history.bytesUsed();
    CHECK_FALSE(history.setTeam(id, 8)); CHECK_FALSE(history.setTeam(EntityId{999}, 0));
    CHECK_FALSE(history.setBrainMode(id, "Alien")); CHECK_FALSE(history.setLegacyBrain(id, "bad\"reference"));
    CHECK_FALSE(history.setBrainMode(doc.entities()[0].id, "Hybrid"));
    CHECK_FALSE(history.setPlayerTeam(*doc.entity(id)->warrior, 1));
    CHECK(doc.revision() == revision); CHECK(history.bytesUsed() == cost); CHECK(history.canRedo());
    CHECK(doc.fitBytes() == fit()); CHECK(bytes(doc) == pak());
    auto unquoted = fit(); unquoted.replace(unquoted.find("\"Hybrid\""), 8, "Legacy");
    std::vector<std::string> logs;
    CHECK_FALSE(MissionDocument::decode(unquoted, pak(), "unquoted-mode", [&](const auto& line) { logs.push_back(line); }));
    CHECK(logs.size() == 1);
    auto extension = fit(); extension.insert(extension.find("FITend"),
        "[WarriorExtension]\r\nl Brain = 42\r\nBrain { compatibilityMode = custom; }\r\n[Player0]\r\n");
    auto loaded = MissionDocument::decode(extension, pak(), "extension", {}); REQUIRE(loaded);
    CommandHistory absent(*loaded); NodeId player;
    for (const auto& section : loaded->syntax().sections) if (section.name == "Player0") player = section.id;
    REQUIRE(absent.setPlayerTeam(player, 3)); CHECK(loaded->find(player, "DefaultTeam")->value == Value{std::int64_t{3}});
    REQUIRE(absent.undo()); CHECK(loaded->fitBytes() == extension); REQUIRE(absent.redo());
}

TEST_CASE("document/unit structural commands preserve coincident identities and undo bytes")
{
    auto doc = document(); CommandHistory history(doc);
    const auto one = doc.entities()[2].id, two = doc.entities()[3].id;
    const auto oneSection = doc.entity(one)->section, twoBrain = doc.entity(two)->brain;
    const auto title = doc.find("MissionSettings", "MissionName")->id;
    const auto mode = doc.find(*doc.entity(one)->brain, "compatibilityMode")->id;
    const Transform moved{{5.125, -8.25, 3.5}, 135};
    REQUIRE(history.transform(one, moved));
    CHECK(doc.entity(one)->position == moved.position); CHECK(doc.entity(two)->position == std::array<double, 3>{});
    CHECK(doc.entity(one)->section == oneSection); CHECK(doc.entity(two)->brain == twoBrain);
    CHECK(doc.find(*doc.entity(one)->brain, "compatibilityMode")->id == mode);
    CHECK(doc.fitBytes().find("f PositionZ = 3.5\r\n") != std::string::npos);
    CHECK(doc.fitBytes().find("// hybrid comment retained exactly\r\n") != std::string::npos);
    CHECK(doc.fitBytes().find("f Value = 17.500\r\n") != std::string::npos);
    CHECK(bytes(doc) == pak());
    const auto changed = doc.fitBytes();
    const auto dir = scratch();
    REQUIRE(doc.save(dir / "moved", {})); CHECK_FALSE(doc.modified());
    const auto reopened = MissionDocument::read(dir / "moved.fit", dir / "moved.pak", {});
    REQUIRE(reopened); CHECK(reopened->fitBytes() == changed);
    CHECK(reopened->entities()[2].position == moved.position);
    REQUIRE(history.undo()); CHECK(doc.fitBytes() == fit()); CHECK(doc.modified());
    REQUIRE(doc.save(dir / "undone", {})); CHECK_FALSE(doc.modified());
    REQUIRE(history.redo()); CHECK(doc.fitBytes() == changed); CHECK(doc.modified());
    REQUIRE(history.undo());
    REQUIRE(history.setText(title, "Interleaved", "Title"));
    REQUIRE(history.transform(two, {{1, 2, 0}, 45}));
    REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(doc.fitBytes() == fit());
    REQUIRE(history.redo()); REQUIRE(history.redo());
    CHECK(doc.entity(one)->position == std::array<double, 3>{});
    CHECK(doc.entity(two)->position == std::array<double, 3>{1, 2, 0});
}

TEST_CASE("document/unit insertion deletion and export compaction retain other IDs")
{
    auto doc = document(); CommandHistory history(doc);
    const auto one = doc.entities()[2].id, two = doc.entities()[3].id;
    const auto section = doc.entity(two)->section, brain = doc.entity(two)->brain;
    REQUIRE(history.erase(one));
    CHECK(doc.entity(one) == nullptr); REQUIRE(doc.entity(two));
    CHECK(doc.entity(two)->exportedPart == 1); CHECK(doc.entity(two)->section == section);
    CHECK(doc.entity(two)->brain == brain); CHECK(doc.section(*doc.entity(two)->warrior)->name == "Warrior1");
    CHECK(doc.find("Part2", "Pilot") == nullptr);
    const auto deleted = doc.fitBytes();
    REQUIRE(history.undo()); CHECK(doc.fitBytes() == fit()); CHECK(bytes(doc) == pak());
    CHECK(doc.entity(one)->id == one); CHECK(doc.entity(two)->exportedPart == 2);
    REQUIRE(history.redo()); CHECK(doc.fitBytes() == deleted);
    const auto inserted = history.insertUnit(newUnit()); REQUIRE(inserted);
    REQUIRE(doc.entity(*inserted)); CHECK(doc.entity(*inserted)->exportedPart == 2);
    CHECK(doc.entity(*inserted)->objectNumber == 123); CHECK(doc.entity(*inserted)->team == 2);
    CHECK(doc.section(*doc.entity(*inserted)->warrior)->name == "Warrior3");
    CHECK(doc.entity(two)->section == section); CHECK(doc.entity(two)->brain == brain);
    const auto placed = doc.fitBytes(); CHECK(placed != deleted); CHECK(bytes(doc) == pak());
    const auto dir = scratch(); REQUIRE(doc.save(dir / "placed", {}));
    const auto reopened = MissionDocument::read(dir / "placed.fit", dir / "placed.pak", {});
    REQUIRE(reopened); CHECK(reopened->entities().back().position == newUnit().transform.position);
    REQUIRE(history.undo()); CHECK(doc.fitBytes() == deleted); CHECK(doc.entity(*inserted) == nullptr);
    REQUIRE(history.redo()); CHECK(doc.fitBytes() == placed); CHECK(doc.entity(*inserted)->id == *inserted);
    REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(doc.fitBytes() == fit());
    REQUIRE(doc.save(dir / "original", {}));
    auto original = MissionDocument::read(dir / "original.fit", dir / "original.pak", {});
    REQUIRE(original); CHECK(original->fitBytes() == fit()); CHECK(bytes(*original) == pak());
    const auto branch = history.insertUnit(newUnit()); REQUIRE(branch); CHECK(*branch != *inserted); CHECK_FALSE(history.canRedo());
}

TEST_CASE("document/inserted Part supplies every required runtime field")
{
    auto doc = document(); CommandHistory history(doc);
    const auto placed = history.insertUnit(newUnit()); REQUIRE(placed);
    const auto section = *doc.entity(*placed)->section;
    for (const auto& [key, prefix] : {
        std::pair{"ObjectNumber", "ul"}, {"ControlType", "ul"}, {"ControlDataType", "ul"},
        {"ObjectProfile", "st"}, {"Pilot", "ul"}, {"PositionX", "f"}, {"PositionY", "f"},
        {"Rotation", "f"}, {"TeamID", "c"}, {"CommanderID", "c"}, {"Active", "l"},
        {"Exists", "l"}, {"MyIcon", "c"}, {"SquadNum", "ul"}, {"IndicesOfAlternatives", "l"}}) {
        CAPTURE(key);
        const auto* field = doc.find(section, key); REQUIRE(field);
        CHECK(field->prefix == prefix);
        CHECK(field->value.size() == (std::string_view(key) == "IndicesOfAlternatives" ? 15 : 1));
    }
    CHECK(doc.find(section, "MyIcon")->value == Value{std::int64_t{0}});
    const auto dir = scratch(); REQUIRE(doc.save(dir / "payload", {}));
    auto reopened = MissionDocument::read(dir / "payload.fit", dir / "payload.pak", {}); REQUIRE(reopened);
    CHECK(reopened->fitBytes() == doc.fitBytes());
}

TEST_CASE("document/group members retain unit identity after delete and export")
{
    auto doc = document(); CommandHistory seed(doc); REQUIRE(seed.insertUnit(newUnit()));
    auto input = doc.fitBytes();
    input.insert(input.find("FITend"),
        "[Commander0Group:0]\r\nl[12] Mates = 1,2,3,0,-1,1,3,0,0,0,0,0, // slots\r\n"
        "[cOMMANDER1gROUP:0]\r\nl[12] Mates = 2,2,0,0,0,0,0,0,0,0,0,0,\r\n"
        "[CommanderExtensionGroup:0]\r\nl[2] Mates = 2,3,\r\n");
    for (std::size_t deleted = 0; deleted < 3; ++deleted) {
        CAPTURE(deleted);
        auto loaded = MissionDocument::decode(input, pak(), "groups", {}); REQUIRE(loaded);
        CommandHistory history(*loaded);
        const auto originalIds = loaded->entities();
        const auto removed = originalIds[deleted + 2].id;
        const auto matesId = loaded->find("Commander0Group:0", "Mates")->id;
        const auto originalMates = loaded->field(matesId)->value;
        auto expected = loaded->field(matesId)->value;
        for (auto& mate : expected) {
            auto& index = std::get<std::int64_t>(mate);
            if (index == static_cast<std::int64_t>(deleted + 1)) index = 0;
            else if (index > static_cast<std::int64_t>(deleted + 1)) --index;
        }
        REQUIRE(history.erase(removed)); CHECK(loaded->field(matesId)->value == expected);
        const auto dir = scratch(); REQUIRE(loaded->save(dir / "groups", {}));
        auto exported = MissionDocument::read(dir / "groups.fit", dir / "groups.pak", {}); REQUIRE(exported);
        CHECK(exported->find("Commander0Group:0", "Mates")->value == expected);
        const auto& remaining = exported->find("cOMMANDER1gROUP:0", "Mates")->value;
        CHECK(remaining[0] == Scalar{std::int64_t{deleted == 0 ? 1 : deleted == 1 ? 0 : 2}});
        CHECK(remaining[1] == remaining[0]); CHECK(remaining.size() == 12);
        CHECK(exported->find("CommanderExtensionGroup:0", "Mates")->value == Value{std::int64_t{2}, std::int64_t{3}});
        for (std::size_t slot = 0; slot < expected.size(); ++slot) {
            const auto index = std::get<std::int64_t>(expected[slot]);
            if (index <= 0) continue;
            const auto found = std::ranges::find_if(loaded->entities(), [=](const Entity& e) { return e.exportedPart == index; });
            REQUIRE(found != loaded->entities().end());
            const auto originalIndex = static_cast<std::size_t>(std::get<std::int64_t>(originalMates[slot]));
            CHECK(found->id == originalIds[originalIndex + 1].id);
            CHECK(exported->entities()[static_cast<std::size_t>(index) + 1].objectNumber == originalIds[originalIndex + 1].objectNumber);
            CHECK(found->id != removed);
        }
        const auto changed = loaded->fitBytes();
        REQUIRE(history.undo()); CHECK(loaded->fitBytes() == input); CHECK(bytes(*loaded) == pak());
        REQUIRE(loaded->save(dir / "undone", {}));
        CHECK(fileBytes(dir / "undone.fit") == std::vector<std::byte>(reinterpret_cast<const std::byte*>(input.data()), reinterpret_cast<const std::byte*>(input.data() + input.size())));
        CHECK(fileBytes(dir / "undone.pak") == pak());
        REQUIRE(history.redo()); CHECK(loaded->fitBytes() == changed);
    }
}

TEST_CASE("document/malformed commander groups reject deletion atomically")
{
    for (const auto mates : {"l[12] Mates = 3,0,0,0,0,0,0,0,0,0,0,0,", "l[2] Mates = 1,2,", "st Mates = \"bad\"", "l Other = 1"}) {
        auto input = fit(); input.insert(input.find("FITend"), std::string("[Commander0Group:0]\r\n") + mates + "\r\n");
        auto loaded = MissionDocument::decode(input, pak(), "bad-group", {}); REQUIRE(loaded);
        CommandHistory history(*loaded);
        REQUIRE(history.transform(loaded->entities()[2].id, {{1, 2, 3}, 4})); REQUIRE(history.undo());
        const auto revision = loaded->revision(), used = history.bytesUsed();
        const auto rejected = history.erase(loaded->entities()[2].id); REQUIRE_FALSE(rejected);
        CHECK(rejected.error().reason == "malformed_group_mates");
        CHECK(loaded->fitBytes() == input); CHECK(bytes(*loaded) == pak());
        CHECK(loaded->revision() == revision); CHECK(history.bytesUsed() == used); CHECK(history.canRedo());
    }
}

TEST_CASE("document/building packet encoding transforms insert delete undo and redo")
{
    for (bool checksum : {false, true}) for (bool compressPlacements : {false, true}) {
        CAPTURE(checksum, compressPlacements);
        auto doc = document(checksum, compressPlacements); CommandHistory history(doc);
        const auto original = bytes(doc);
        const auto originalFit = doc.fitBytes();
        const auto one = doc.entities()[0].id, two = doc.entities()[1].id, unit = doc.entities()[2].id;
        const Transform moved{{1.25, 5.5, -4}, 180};
        REQUIRE(history.transform(one, moved));
        CHECK(doc.entity(one)->position == moved.position); CHECK(doc.entity(two)->position == std::array<double, 3>{});
        CHECK(doc.fitBytes() == originalFit); CHECK(bytes(doc) != original); unchangedPackets(doc.archive(), bytes(doc));
        auto decoded = decodePak(bytes(doc)); REQUIRE(decoded);
        const auto& old = doc.archive().packets[1].decoded;
        const auto& changed = decoded->packets[1].decoded;
        CHECK(std::ranges::equal(std::span(old).subspan(24), std::span(changed).subspan(24)));
        const auto transformed = bytes(doc);
        REQUIRE(history.undo()); CHECK(bytes(doc) == original); CHECK_FALSE(doc.modified());
        REQUIRE(history.redo()); CHECK(bytes(doc) == transformed);
        REQUIRE(history.erase(one)); CHECK(doc.entity(one) == nullptr); CHECK(doc.entity(two)->packetRecord == 0);
        CHECK(doc.entity(unit)->exportedPart == 1);
        const auto deleted = bytes(doc);
        REQUIRE(history.undo()); CHECK(bytes(doc) == transformed); CHECK(doc.entity(two)->packetRecord == 1);
        REQUIRE(history.redo()); CHECK(bytes(doc) == deleted);
        BuildingPlacement building; building.objectNumber = 999; building.transform = {{8.5, 9, 10}, 270};
        building.team = 3; building.damage = 1; building.parentCell = 0x12345678; building.forest = 7; building.scale = 12;
        const auto inserted = history.insertBuilding(building); REQUIRE(inserted);
        CHECK(doc.entity(two)->packetRecord == 0); CHECK(doc.entity(*inserted)->packetRecord == 1);
        CHECK(doc.entity(*inserted)->position == building.transform.position); CHECK(doc.entity(*inserted)->team == 3);
        CHECK(doc.entity(unit)->exportedPart == 1); unchangedPackets(doc.archive(), bytes(doc));
        const auto placed = bytes(doc);
        const auto dir = scratch(); REQUIRE(doc.save(dir / "placed", {}));
        const auto reopened = MissionDocument::read(dir / "placed.fit", dir / "placed.pak", {});
        REQUIRE(reopened); CHECK(bytes(*reopened) == placed); CHECK(reopened->entities()[1].objectNumber == 999);
        CHECK(reopened->entities()[1].position == building.transform.position);
        REQUIRE(history.undo()); CHECK(bytes(doc) == deleted); REQUIRE(history.redo()); CHECK(bytes(doc) == placed);
        REQUIRE(history.undo()); REQUIRE(history.undo()); REQUIRE(history.undo());
        CHECK(bytes(doc) == original); CHECK(doc.fitBytes() == originalFit);
        REQUIRE(doc.save(dir / "original", {}));
        auto restored = MissionDocument::read(dir / "original.fit", dir / "original.pak", {});
        REQUIRE(restored); CHECK(bytes(*restored) == original); CHECK(restored->fitBytes() == originalFit);
        REQUIRE(history.erase(one)); REQUIRE(history.erase(two));
        CHECK(doc.entities().size() == 2); REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(bytes(doc) == original);
    }
}

TEST_CASE("document/rejected structural commands are atomic and bounded")
{
    auto doc = document(); CommandHistory history(doc);
    const auto id = doc.entities()[0].id, unitId = doc.entities()[2].id;
    REQUIRE(history.transform(id, {{1, 2, 3}, 45})); REQUIRE(history.undo());
    const auto revision = doc.revision(), used = history.bytesUsed();
    CHECK_FALSE(history.transform(id, {{std::numeric_limits<double>::quiet_NaN(), 0, 0}, 0}));
    CHECK_FALSE(history.transform(id, {{0, 0, 0}, 1e100}));
    CHECK_FALSE(history.transform(EntityId{99999}, {})); CHECK_FALSE(history.erase(EntityId{99999}));
    auto invalid = newUnit(); invalid.legacyBrain = "bad\"injection"; CHECK_FALSE(history.insertUnit(invalid));
    CHECK(doc.revision() == revision); CHECK(history.bytesUsed() == used); CHECK(history.canRedo());
    CHECK(doc.fitBytes() == fit()); CHECK(bytes(doc) == pak());
    REQUIRE(history.transform(unitId, {})); CHECK(doc.revision() == revision); CHECK(history.canRedo());
    CommandHistory tiny(doc, 1);
    CHECK_FALSE(tiny.erase(id)); CHECK_FALSE(tiny.insertUnit(newUnit())); CHECK_FALSE(tiny.insertBuilding({}));
    CHECK(bytes(doc) == pak()); CHECK(doc.fitBytes() == fit()); CHECK_FALSE(tiny.canUndo());
    CommandHistory bounded(doc, 100000);
    for (int i = 1; i <= 12; ++i) REQUIRE(bounded.transform(id, {{static_cast<double>(i), 0, 0}, 0}));
    CHECK(bounded.evicted() > 0); CHECK(bounded.bytesUsed() <= 100000); REQUIRE(bounded.undo()); REQUIRE(bounded.redo());
}

TEST_CASE("document/structural readers reject malformed references and nested blocks once")
{
    for (const auto replacement : {"ul Pilot = 77", "st Pilot = \"bad\"", "l Pilot = -1"}) {
        auto input = fit(); const auto p = input.find("ul Pilot = 2"); input.replace(p, 12, replacement);
        std::vector<std::string> logs;
        auto rejected = MissionDocument::decode(input, pak(), "bad", [&](const auto& line) { logs.push_back(line); });
        CHECK_FALSE(rejected); REQUIRE(logs.size() == 1); CHECK(logs[0].find("Part1/Pilot") != std::string::npos);
    }
    auto input = fit(); input.insert(input.find("[Warrior2Cell0]"), "Brain { compatibilityMode = \"Legacy\"; }\r\n");
    std::vector<std::string> logs;
    CHECK_FALSE(MissionDocument::decode(input, pak(), "bad", [&](const auto& line) { logs.push_back(line); }));
    REQUIRE(logs.size() == 1); CHECK(logs[0].find("duplicate_nested_block") != std::string::npos);
    auto archive = decodePak(pak()); REQUIRE(archive);
    const auto offset = archive->packets[1].encoded.begin;
    for (const auto& [relative, value] : {std::pair<std::size_t, std::uint32_t>{0, 3}, {8, 0x7fc00000U}, {28, 8}}) {
        auto invalid = pak(); put(invalid, offset + relative, value);
        logs.clear();
        CHECK_FALSE(MissionDocument::decode(fit(), invalid, "bad", [&](const auto& line) { logs.push_back(line); }));
        REQUIRE(logs.size() == 1); CHECK(logs[0].find("Packet1") != std::string::npos);
    }
}

TEST_CASE("document/unit creation rejects assignments beyond runtime warrior capacity")
{
    auto input = fit(); input.replace(input.find("NumWarriors = 2"), 15, "NumWarriors = 119");
    auto loaded = MissionDocument::decode(input, pak(), "capacity", {}); REQUIRE(loaded);
    CommandHistory history(*loaded); const auto result = history.insertUnit(newUnit());
    REQUIRE_FALSE(result); CHECK(result.error().reason == "runtime_warrior_limit");
    CHECK(loaded->fitBytes() == input); CHECK_FALSE(history.canUndo());
}

TEST_CASE("document/repeated nested extension blocks retain distinct node IDs")
{
    auto input = fit();
    input.insert(input.find("PrimaryOPORD { type"), "Extension { x = 1; } Extension { x = 2; }\r\n");
    auto loaded = MissionDocument::decode(input, pak(), "repeated", {}); REQUIRE(loaded);
    const auto brain = loaded->entities()[2].brain;
    std::vector<NodeId> nodes;
    for (const auto& section : loaded->syntax().sections) if (section.parent == brain && section.name == "Extension") nodes.push_back(section.id);
    REQUIRE(nodes.size() == 2); CHECK(nodes[0] != nodes[1]);
    CommandHistory history(*loaded); REQUIRE(history.transform(loaded->entities()[2].id, {{1, 2, 3}, 4}));
    CHECK(loaded->section(nodes[0])->id == nodes[0]); CHECK(loaded->section(nodes[1])->id == nodes[1]);
    CHECK(std::get<std::string>(loaded->find(nodes[0], "x")->value[0]) == "1");
    CHECK(std::get<std::string>(loaded->find(nodes[1], "x")->value[0]) == "2");
    REQUIRE(history.undo()); CHECK(loaded->fitBytes() == input);
}

TEST_CASE("document/extension names cannot alias identity paths or Part references")
{
    auto input = fit();
    input.insert(input.find("FITend"), "[Extension#0/Brain]\r\nst Marker = \"root\"\r\n"
        "[@deleted]\r\nst Marker = \"tombstone\"\r\n"
        "[PartExtensions]\r\nl[2] IndicesOfAlternatives = 2,-1,\r\n");
    auto loaded = MissionDocument::decode(input, pak(), "extension-names", {}); REQUIRE(loaded);
    const auto marker = loaded->find("Extension#0/Brain", "Marker")->id;
    const auto tombstone = loaded->find("@deleted", "Marker")->id;
    const auto reference = loaded->find("PartExtensions", "IndicesOfAlternatives")->id;
    CommandHistory history(*loaded); const auto id = loaded->entities()[2].id;
    REQUIRE(history.transform(id, {{1, 2, 3}, 4})); REQUIRE(history.erase(id));
    CHECK(loaded->find("Extension#0/Brain", "Marker")->id == marker);
    CHECK(loaded->find("@deleted", "Marker")->id == tombstone);
    CHECK(loaded->find("PartExtensions", "IndicesOfAlternatives")->id == reference);
    CHECK(std::get<std::int64_t>(loaded->field(reference)->value[0]) == 2);
    CHECK(std::get<std::string>(loaded->field(marker)->value[0]) == "root");
    CHECK(std::get<std::string>(loaded->field(tombstone)->value[0]) == "tombstone");
    std::vector<NodeId> ids;
    for (const auto& section : loaded->syntax().sections) ids.push_back(section.id);
    for (const auto& field : loaded->syntax().fields) ids.push_back(field.id);
    std::ranges::sort(ids); CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    REQUIRE(history.undo()); REQUIRE(history.undo()); CHECK(loaded->fitBytes() == input);
}

TEST_CASE("document/Part index references compact without changing Warrior references")
{
    auto doc = document(); CommandHistory history(doc);
    auto placed = history.insertUnit(newUnit()); REQUIRE(placed);
    auto input = doc.fitBytes();
    input.insert(input.find("[Part2]"), "l[2] IndicesOfAlternatives = 3,-1,\r\nul AlternativeStartIndex = 3\r\n");
    auto loaded = MissionDocument::decode(input, pak(), "alternatives", {}); REQUIRE(loaded);
    CommandHistory references(*loaded);
    const auto owner = loaded->entities()[2].id, middle = loaded->entities()[3].id, alternative = loaded->entities()[4].id;
    REQUIRE(references.erase(middle));
    CHECK(loaded->entity(owner)->exportedPart == 1); CHECK(loaded->entity(alternative)->exportedPart == 2);
    CHECK(std::get<std::int64_t>(loaded->find("Part1", "IndicesOfAlternatives")->value[0]) == 2);
    CHECK(std::get<std::int64_t>(loaded->find("Part1", "AlternativeStartIndex")->value[0]) == 2);
    CHECK(loaded->section(*loaded->entity(alternative)->warrior)->name == "Warrior3");
    const auto retained = loaded->fitBytes();
    const auto revision = loaded->revision();
    CHECK_FALSE(references.erase(alternative)); CHECK(loaded->fitBytes() == retained); CHECK(loaded->revision() == revision);
    REQUIRE(references.undo()); CHECK(loaded->fitBytes() == input);
}

TEST_CASE("document/empty placements and absent Warriors support reversible creation")
{
    auto input = fit();
    input.erase(input.find("[Part1]"), input.find("[Extension]") - input.find("[Part1]"));
    input.replace(input.find("NumParts = 2"), 12, "NumParts = 0");
    auto archive = decodePak(pak()); REQUIRE(archive);
    auto empty = encodePlacements(*archive, {}); REQUIRE(empty);
    auto loaded = MissionDocument::decode(input, *empty, "empty", {}); REQUIRE(loaded);
    CommandHistory history(*loaded);
    const auto unit = history.insertUnit(newUnit()); REQUIRE(unit);
    const auto unitNode = loaded->entity(*unit)->section;
    const auto building = history.insertBuilding({}); REQUIRE(building);
    CHECK(loaded->entities()[0].id == *building); CHECK(loaded->entities()[1].id == *unit);
    CHECK(loaded->entity(*unit)->section == unitNode);
    const auto title = loaded->find("MissionSettings", "MissionName")->id;
    REQUIRE(history.setText(title, "created", "Name")); REQUIRE(history.undo());
    REQUIRE(history.erase(*unit)); CHECK(loaded->find("Parts", "NumParts")->value[0] == Scalar{std::int64_t{0}});
    REQUIRE(history.undo()); REQUIRE(history.undo()); REQUIRE(history.undo());
    CHECK(loaded->fitBytes() == input); CHECK(bytes(*loaded) == *empty);
    REQUIRE(history.redo()); REQUIRE(history.redo()); CHECK(loaded->entity(*unit)->section == unitNode);
}

TEST_CASE("document/case insensitive FIT Part lookup preserves stable handles")
{
    auto input = fit(); input.replace(input.find("[Part1]"), 7, "[pART1]");
    auto loaded = MissionDocument::decode(input, pak(), "case", {}); REQUIRE(loaded);
    const auto id = loaded->entities()[2].id; REQUIRE(loaded->entity(id)->section);
    CommandHistory history(*loaded); REQUIRE(history.transform(id, {{1, 2, 3}, 45}));
    CHECK(loaded->entity(id)->position == std::array<double, 3>{1, 2, 3});
    REQUIRE(history.undo()); CHECK(loaded->fitBytes() == input);
}

TEST_CASE("document/stock corpus requires exactly 24 unchanged pairs and a negative control" * doctest::skip(std::getenv("MC2_EDITOR_STOCK_DIR") == nullptr))
{
    const auto* directory = std::getenv("MC2_EDITOR_STOCK_DIR");
    REQUIRE(directory);
    const auto* gradingDirectory = std::getenv("MC2_EDITOR_BYTE_GRADE_DIR");
    const auto output = gradingDirectory ? std::filesystem::path(gradingDirectory) : scratch();
    std::filesystem::create_directories(output);
    std::size_t count{};
    for (int i = 1; i <= 24; ++i) {
        const auto name = std::string("mc2_") + (i < 10 ? "0" : "") + std::to_string(i);
        const auto stem = std::filesystem::path(directory) / name;
        auto doc = MissionDocument::read(stem.string() + ".fit", stem.string() + ".pak", {});
        REQUIRE(doc);
        std::ifstream input(stem.string() + ".fit", std::ios::binary);
        const std::string original{std::istreambuf_iterator<char>(input), {}};
        REQUIRE(doc->fitBytes() == original); REQUIRE(bytes(*doc) == doc->archive().originalBytes);
        const auto originalPak = fileBytes(stem.string() + ".pak");
        const auto originalFit = fileBytes(stem.string() + ".fit");
        REQUIRE(doc->save(output / name, {}));
        CHECK(fileBytes(output / (name + ".fit")) == originalFit);
        CHECK(fileBytes(output / (name + ".pak")) == originalPak);
        auto corruption = fileBytes(output / (name + ".fit")); corruption.back() ^= std::byte{1};
        const auto corruptPath = output / (name + ".corrupted.fit");
        { std::ofstream corrupt(corruptPath, std::ios::binary); corrupt.write(reinterpret_cast<const char*>(corruption.data()), corruption.size()); REQUIRE(corrupt); }
        const auto title = doc->find("MissionSettings", "MissionName"); REQUIRE(title);
        CommandHistory history(*doc);
        REQUIRE(history.setText(title->id, "E3 negative control", "Name")); CHECK(doc->fitBytes() != original);
        REQUIRE(history.undo()); CHECK(doc->fitBytes() == original); CHECK(bytes(*doc) == doc->archive().originalBytes);
        std::vector<EntityId> originalIds;
        for (const auto& entity : doc->entities()) originalIds.push_back(entity.id);
        for (const bool building : {false, true}) {
            const auto found = std::ranges::find_if(doc->entities(), [=](const Entity& e) { return e.packetRecord.has_value() == building; });
            REQUIRE(found != doc->entities().end());
            const auto id = found->id;
            Transform moved{found->position, found->rotation}; moved.position[0] += 16;
            REQUIRE(history.transform(id, moved));
            CHECK(std::bit_cast<std::uint32_t>(static_cast<float>(doc->entity(id)->position[0])) ==
                std::bit_cast<std::uint32_t>(static_cast<float>(moved.position[0])));
            CHECK((doc->fitBytes() != original) == !building);
            CHECK((bytes(*doc) != originalPak) == building);
            for (std::size_t n = 0; n < originalIds.size(); ++n) CHECK(doc->entities()[n].id == originalIds[n]);
            const auto changedStem = output / (name + (building ? "-building" : "-unit"));
            REQUIRE(doc->save(changedStem, {}));
            auto reopened = MissionDocument::read(changedStem.string() + ".fit", changedStem.string() + ".pak", {});
            REQUIRE(reopened); CHECK(reopened->fitBytes() == doc->fitBytes()); CHECK(bytes(*reopened) == bytes(*doc));
            REQUIRE(history.undo()); CHECK(doc->fitBytes() == original); CHECK(bytes(*doc) == originalPak);
        }
        ++count;
    }
    CHECK(count == 24);
}
