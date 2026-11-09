#include "doctest.h"
#include "logistics_deployment.h"
#include "inifile.h"
#include <cstdio>
#include <string>
#include <vector>

namespace ld = logistics_deployment;

TEST_CASE("Deployment/capacity and tonnage boundaries")
{
    CHECK(ld::capacity() == 24);
    for (int slot : {12, 13, 24})
        CHECK(ld::validSlot(slot, ld::capacity()));
    CHECK_FALSE(ld::validSlot(25, ld::capacity()));
    CHECK_FALSE(ld::validSlot(0, ld::capacity()));
    CHECK_FALSE(ld::validSlot(-1, ld::capacity()));
    int legacyPilots = 0;
    for (int i = 1; i <= 25; ++i)
        CHECK(ld::legacyPilotSlot(legacyPilots) == (i <= 12 ? i : 0));
    CHECK(ld::canAdd(23, 24, 20, 460, 480));
    CHECK_FALSE(ld::canAdd(24, 24, 20, 460, 480));
    CHECK_FALSE(ld::canAdd(12, 24, 20, 461, 480));
    CHECK(ld::canAdd(12, 24, 20, 460, 480));
    CHECK_FALSE(ld::canAdd(12, 12, 20, 0, 480));
    for (int players = 1; players <= 8; ++players)
        CHECK(ld::capacity(24, players) == std::min(12, 36 / players));
}

namespace {
struct Scratch {
    const char* path = "deployment_roundtrip.fit";
    ~Scratch() { std::remove(path); }
};
void writeInventory(FitIniFile& file, const std::vector<ld::MechRecord>& records)
{
    for (size_t i = 0; i < records.size(); ++i)
        ld::writeMech(file, static_cast<long>(i), records[i]);
}
}

TEST_CASE("Deployment/campaign record round trip preserves repeated variants and storage")
{
    Scratch scratch;
    std::vector<ld::MechRecord> records;
    // Inventory order differs from deployment order, with storage beyond capacity.
    for (int i = 24; i >= 1; --i)
        records.push_back({"commando.csv", "Commando 1", "pilot" + std::to_string(i), true, i});
    for (int i = 0; i < 7; ++i)
        records.push_back({"commando.csv", "Commando 1", std::nullopt, true, 0});
    {
        FitIniFile file;
        REQUIRE(file.create(scratch.path) == NO_ERR);
        writeInventory(file, records);
        file.close();
    }
    FitIniFile file;
    REQUIRE(file.open(scratch.path) == NO_ERR);
    REQUIRE(ld::compatible(file, ld::recordVersion, static_cast<long>(records.size())));
    for (size_t i = 0; i < records.size(); ++i) {
        REQUIRE(file.seekBlock(("Inventory" + std::to_string(i)).c_str()) == NO_ERR);
        const auto record = ld::readMech(file, ld::recordVersion);
        REQUIRE(record.has_value());
        CHECK(record->slot == records[i].slot);
        CHECK(record->pilot == records[i].pilot);
        CHECK(record->variant == records[i].variant);
        CHECK(record->chassis == records[i].chassis);
        CHECK(record->designer == records[i].designer);
    }
    file.close();
}

TEST_CASE("Deployment/reject duplicate slots pilots and malformed new records")
{
    Scratch scratch;
    std::vector<ld::MechRecord> records = {
        {"commando.csv", "Commando 1", "pilot1", true, 13},
        {"commando.csv", "Commando 1", "pilot2", true, 24}};
    SUBCASE("duplicate slots") { records[1].slot = 13; }
    SUBCASE("duplicate pilots case insensitive") { records[1].pilot = "PILOT1"; }
    SUBCASE("slot 25") { records[1].slot = 25; }
    SUBCASE("negative slot") { records[1].slot = -1; }
    {
        FitIniFile file;
        REQUIRE(file.create(scratch.path) == NO_ERR);
        writeInventory(file, records);
        file.close();
    }
    FitIniFile file;
    REQUIRE(file.open(scratch.path) == NO_ERR);
    CHECK_FALSE(ld::compatible(file, ld::recordVersion, 2));
    CHECK_FALSE(ld::compatible(file, 10006, 2));
    file.close();
}

TEST_CASE("Deployment/FIT optional lower mod cap")
{
    std::string text = "FITini\r\n[Campaign]\r\nl MaxDeployedMechs = 12\r\n"
        "[MissionSettings]\r\nl MaxDeployedMechs = 8\r\n[Absent]\r\n"
        "[TooHigh]\r\nl MaxDeployedMechs = 40\r\nFITend\r\n";
    FitIniFile file;
    REQUIRE(file.open(text.data(), static_cast<int>(text.size())) == NO_ERR);
    REQUIRE(file.seekBlock("Campaign") == NO_ERR);
    const int campaign = ld::readCap(file);
    CHECK(campaign == 12);
    REQUIRE(file.seekBlock("MissionSettings") == NO_ERR);
    CHECK(ld::readCap(file, campaign) == 8);
    REQUIRE(file.seekBlock("Absent") == NO_ERR);
    CHECK(ld::readCap(file) == 24);
    CHECK(ld::readCap(file, campaign) == 12);
    REQUIRE(file.seekBlock("TooHigh") == NO_ERR);
    CHECK(ld::readCap(file) == 24);
    file.close();
}

TEST_CASE("Deployment/10004 fixture retains pilot order and twelve slot reconstruction")
{
    FitIniFile file;
    const auto path = std::string(MC2_TEST_FIXTURE_DIR) + "/campaign_10004.fit";
    REQUIRE(file.open(path.c_str()) == NO_ERR);
    REQUIRE(file.seekBlock("Version") == NO_ERR);
    long version = 0;
    REQUIRE(file.readIdLong("VersionNumber", version) == NO_ERR);
    REQUIRE(version == ld::legacyVersion);
    REQUIRE(file.seekBlock("General") == NO_ERR);
    long count = 0;
    REQUIRE(file.readIdLong("InventoryCount", count) == NO_ERR);
    REQUIRE(count == 76);
    CHECK(ld::compatible(file, version, count));
    int pilots = 0;
    for (long i = 0; i < count; ++i) {
        REQUIRE(file.seekBlock(("Inventory" + std::to_string(i)).c_str()) == NO_ERR);
        const auto record = ld::readMech(file, version);
        REQUIRE(record.has_value());
        CHECK(record->slot == 0);
        if (record->pilot && !record->pilot->empty()) {
            const int slot = ld::legacyPilotSlot(pilots);
            CHECK(slot == (pilots <= 12 ? pilots : 0));
        }
    }
    CHECK(pilots > 0);
    file.close();
}
