#include "../../tests/unit/doctest.h"
#include "MissionDocument.h"
#include "ObjectCatalog.h"
#include "mission_codec.h"
#include "inifile.h"
#include "packet.h"
#include "move.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

TEST_CASE("mission-codec/native stock and Python round-trip")
{
    const auto* root = std::getenv("MC2_CODEC_GOLDEN");
    if (!root) { MESSAGE("Set MC2_CODEC_GOLDEN to run the stock mission comparison"); return; }
    using nlohmann::json;
    using namespace editor::document;
    const auto* install = std::getenv("MC2_CODEC_INSTALL"); REQUIRE(install);
    const auto resolver = FileResolver::mount({install}); REQUIRE(resolver);
    const auto markers = ObjectCatalog::load(*resolver, {}).navMarkerObjects();
    for (const auto* name : {"mc2_01", "mc2_03", "mc2_17", "mc2_24"}) {
        CAPTURE(std::string(name));
        const auto stem = std::filesystem::path(root) / name;
        std::ifstream expectedFile(stem.string() + ".json"); REQUIRE(expectedFile.good());
        const auto expected = json::parse(expectedFile);
        for (const auto* suffix : {"", "_roundtrip"}) {
            CAPTURE(suffix);
            PacketFile pak;
            REQUIRE(pak.open((stem.string() + suffix + ".pak").c_str()) == NO_ERR);
            const auto packet = [&](int index) {
                REQUIRE(pak.seekPacket(index) == NO_ERR);
                std::vector<std::byte> raw(pak.getPacketSize());
                REQUIRE(pak.readPacket(index, reinterpret_cast<unsigned char*>(raw.data())) == raw.size());
                return raw;
            };
            const auto raw = packet(1);
            const auto count = mc2::mission::objectCount(raw); REQUIRE(count);
            REQUIRE(*count == expected["objects"].size());
            for (std::size_t i = 0; i < *count; ++i) {
                const auto record = mc2::mission::objectRecord(raw, i);
                const auto& wanted = expected["objects"][i];
                const json shared = {record.type, record.position[0], record.position[1], record.position[2], record.rotation,
                    record.damage, record.team, static_cast<std::int32_t>(record.parentId), record.padding[0], record.padding[1]};
                CHECK(shared == wanted);
                const auto parent = mc2::mission::parentCell(record.parentId);
                CHECK((parent ? json(*parent) : json(nullptr)) == expected["parents"][i]);
            }
            for (int dimension = 0; dimension < 3; ++dimension) {
                const auto value = packet(4 + dimension);
                REQUIRE(value.size() == 4);
                std::int32_t number; std::memcpy(&number, value.data(), 4);
                CHECK(number == expected["move"]["dimensions"][dimension]);
            }
            const auto cells = packet(7);
            REQUIRE(cells.size() == expected["move"]["passable"].size() * sizeof(MapCell));
            std::vector<bool> passable, forest;
            std::vector<unsigned> heights;
            for (std::size_t i = 0; i < cells.size() / sizeof(MapCell); ++i) {
                MapCell cell; std::memcpy(&cell, cells.data() + i * sizeof(cell), sizeof(cell));
                passable.push_back(cell.getPassable()); heights.push_back(cell.getLocalHeight()); forest.push_back(cell.getForest());
            }
            CHECK(passable == expected["move"]["passable"].get<std::vector<bool>>());
            CHECK(heights == expected["move"]["local_height"].get<std::vector<unsigned>>());
            CHECK(forest == expected["move"]["forest"].get<std::vector<bool>>());
        }
        std::ifstream source(stem.string() + ".fit", std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(source), {}};
        auto syntax = decodeFit(text); REQUIRE(syntax);
        FitIniFile fit; REQUIRE(fit.open(text.data(), static_cast<int>(text.size())) == NO_ERR);
        for (const auto& section : syntax->sections) {
            if (section.parent) continue;
            REQUIRE(fit.seekBlock(section.name.c_str()) == NO_ERR);
            for (const auto id : section.fields) {
                const auto& field = *std::ranges::find(syntax->fields, id, &Field::id);
                if (field.array) continue;
                const auto& wanted = expected["fit"].at(identifier(section.name)).at(identifier(field.name));
                const auto& key = field.name;
                if (field.prefix == "st") { char value[4096]; REQUIRE(fit.readIdString(key, value, sizeof(value)) == NO_ERR); CHECK(json(value) == wanted); }
                else if (field.prefix == "f") { float value; REQUIRE(fit.readIdFloat(key, value) == NO_ERR); CHECK(value == wanted.get<float>()); }
                else if (field.prefix == "b") { bool value; REQUIRE(fit.readIdBoolean(key, value) == NO_ERR); CHECK(json(value) == wanted); }
                else if (field.prefix == "l") { long value; REQUIRE(fit.readIdLong(key, value) == NO_ERR); CHECK(json(value) == wanted); }
                else if (field.prefix == "ul") { DWORD value; REQUIRE(fit.readIdULong(key, value) == NO_ERR); CHECK(json(value) == wanted); }
                else if (field.prefix == "c") { char value; REQUIRE(fit.readIdChar(key, value) == NO_ERR); CHECK(json(static_cast<int>(value)) == wanted); }
                else if (field.prefix == "uc") { unsigned char value; REQUIRE(fit.readIdUChar(key, value) == NO_ERR); CHECK(json(value) == wanted); }
                else if (field.prefix == "s") { short value; REQUIRE(fit.readIdShort(key, value) == NO_ERR); CHECK(json(value) == wanted); }
                else if (field.prefix == "us") { unsigned short value; REQUIRE(fit.readIdUShort(key, value) == NO_ERR); CHECK(json(value) == wanted); }
                else continue;
            }
        }
        // MissionDocument keeps the packet bytes but shares their native interpretation.
        auto doc = MissionDocument::read(stem.string() + ".fit", stem.string() + ".pak", {}, markers); REQUIRE(doc);
        std::size_t seen = 0;
        for (const auto& entity : doc->entities()) if (entity.packetRecord) {
            const auto& wanted = expected["objects"][*entity.packetRecord];
            CHECK(entity.objectNumber == wanted[0]);
            for (int axis = 0; axis < 3; ++axis) CHECK(entity.position[axis] == wanted[axis+1].get<double>());
            CHECK(entity.rotation == wanted[4].get<double>()); CHECK(entity.team == wanted[6]);
            const auto parent = doc->parentCell(entity.id);
            CHECK((parent ? json(*parent) : json(nullptr)) == expected["parents"][*entity.packetRecord]);
            ++seen;
        }
        CHECK(seen == expected["objects"].size());
    }
}
