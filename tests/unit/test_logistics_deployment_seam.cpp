#include "doctest.h"
#include "logisticsdata.h"
#include "logisticsmech.h"
#include "logisticspilot.h"
#include "logistics_deployment.h"
#include "inifile.h"
#include "packet.h"
#include "paths.h"
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
struct DeploymentFiles {
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path scratch = cwd / "mvh_scratch_deployment";
    std::string objects = objectPath, missions = missionPath, art = artPath;
    LogisticsData* previous = LogisticsData::instance;
    DeploymentFiles()
    {
        std::filesystem::create_directories(scratch / "data/campaign");
        std::filesystem::create_directories(scratch / "data/objects");
        std::filesystem::create_directories(scratch / "data/art");
        std::filesystem::current_path(scratch);
        std::strcpy(objectPath, "data/objects/");
        std::strcpy(missionPath, "");
        std::strcpy(artPath, "data/art/");
        LogisticsData::instance = nullptr;
        std::ofstream("data/objects/compbas.csv") << "header\r\n";
        std::ofstream("data/objects/pilots.csv") << "\r\n";
        std::ofstream("data/art/buildings.csv") << "\r\n";
        PacketFile pak;
        REQUIRE(pak.create("data/objects/object2.pak") == NO_ERR);
        pak.reserve(1);
        pak.close();
        std::ofstream("data/campaign/campaign.fit") <<
            "FITini\r\n[Campaign]\r\nst CampaignName = \"deployment fixture\"\r\n"
            "l GroupCount = 1\r\nl CBills = 100\r\n[Group0]\r\nl MissionCount = 3\r\n"
            "st Video = \"\"\r\n[Group0Mission0]\r\nst FileName = \"mission\"\r\n"
            "st PurchaseFile = \"purchase\"\r\n[Group0Mission1]\r\nst FileName = \"light\"\r\n"
            "st PurchaseFile = \"purchase\"\r\n[Group0Mission2]\r\nst FileName = \"modcap\"\r\n"
            "st PurchaseFile = \"purchase\"\r\nFITend\r\n";
        for (const auto* name : {"mission", "light", "modcap"}) {
            std::ofstream file(std::string(name) + ".fit");
            file << "FITini\r\n[MissionSettings]\r\nst MissionName = \"fixture\"\r\n"
                "st Blurb2 = \"fixture\"\r\nf DropWeightLimit = "
                << (std::string(name) == "light" ? 240 : 480) << "\r\n";
            if (std::string(name) == "modcap") file << "l MaxDeployedMechs = 12\r\n";
            file << "FITend\r\n";
        }
        std::ofstream("purchase.fit") <<
            "FITini\r\n[Components]\r\nb AllComponents = false\r\n[Mechs]\r\n[Pilots]\r\nFITend\r\n";
    }
    ~DeploymentFiles()
    {
        LogisticsData::instance = previous;
        std::strcpy(objectPath, objects.c_str());
        std::strcpy(missionPath, missions.c_str());
        std::strcpy(artPath, art.c_str());
        std::filesystem::current_path(cwd);
        std::filesystem::remove_all(scratch);
    }
};
class DeploymentChassis : public LogisticsChassis {
public:
    DeploymentChassis() { maxWeight = 20; ID = 1; fileName = "fixture.csv"; }
};
}

TEST_CASE("DeploymentProduction/insertion enumeration trimming and inventory preservation")
{
    DeploymentFiles files;
    LogisticsVariant variant(new DeploymentChassis, true);
    variant.setName("Repeated fixture variant");
    LogisticsPilot pilot;
    LogisticsData logistics;
    logistics.init();
    REQUIRE(logistics.getDeploymentCapacity() == 24);
    for (int slot = 1; slot <= 25; ++slot) {
        if (slot % 2)
            logistics.addMechToInventory(&variant, slot, nullptr);
        else
            logistics.addMechToInventory(&variant, nullptr, slot);
        CHECK(logistics.getInventory().size() == static_cast<size_t>(slot));
        CHECK(logistics.getForceGroup().size() == static_cast<size_t>(std::min(slot, 24)));
    }
    auto inventory = logistics.getInventory();
    REQUIRE(inventory.size() == 25);
    CHECK(inventory.back()->getForceGroup() == 0);
    CHECK(logistics.getCurrentDropWeight() == 480);
    CHECK_FALSE(logistics.canAddMechToForceGroup(inventory.back()));
    CHECK_FALSE(logistics.addMechToForceGroup(inventory.back(), 25));
    CHECK_FALSE(logistics.addMechToForceGroup(inventory.back(), 12));
    REQUIRE(logistics.removeMechFromForceGroup(13));
    CHECK(logistics.canAddMechToForceGroup(inventory.back()));
    REQUIRE(logistics.addMechToForceGroup(inventory.back(), 13));
    CHECK(logistics.getForceGroup()[12] == inventory.back());
    // A lower weight limit removes the highest real slots, releasing their pilots.
    inventory[23]->setPilot(&pilot);
    REQUIRE(logistics.setCurrentMission("light"));
    CHECK(logistics.getForceGroup().size() == 12);
    CHECK(logistics.getInventory().size() == 25);
    CHECK(inventory[23]->getPilot() == nullptr);
    CHECK_FALSE(pilot.isUsed());
    // Sparse high slots, rather than just force size, must respect a lower mod cap.
    REQUIRE(logistics.setCurrentMission("mission"));
    REQUIRE(logistics.removeMechFromForceGroup(12));
    REQUIRE(logistics.addMechToForceGroup(inventory[23], 24));
    REQUIRE(logistics.setCurrentMission("modcap"));
    CHECK(logistics.getDeploymentCapacity() == 12);
    CHECK(inventory[23]->getForceGroup() == 0);
    CHECK(logistics.getInventory().size() == 25);
    // Incompatible records must not clear the live inventory or its pilots.
    inventory[0]->setPilot(&pilot);
    std::string invalid = "FITini\r\n[Version]\r\nl VersionNumber = 10005\r\n"
        "[General]\r\nl InventoryCount = 1\r\n[Inventory0]\r\n"
        "st Variant = \"Repeated fixture variant\"\r\nl DeploymentSlot = 25\r\nFITend\r\n";
    FitIniFile bad;
    REQUIRE(bad.open(invalid.data(), static_cast<int>(invalid.size())) == NO_ERR);
    CHECK_FALSE(logistics.load(bad));
    CHECK(logistics.getInventory() == inventory);
    CHECK(inventory[0]->getPilot() == &pilot);
    CHECK(pilot.isUsed());
    bad.close();
    // Duplicate pilots are also rejected at insertion, without dropping the mech.
    inventory[23]->setPilot(&pilot);
    CHECK_FALSE(logistics.addMechToForceGroup(inventory[23], 12));
    inventory[23]->setPilot(nullptr);
    inventory[0]->setPilot(&pilot);
    CHECK(logistics.getInventory().size() == 25);
    // A compatibility failure after staging the new campaign must also roll back.
    std::string missingChassis = "FITini\r\n[Version]\r\nl VersionNumber = 10005\r\n"
        "[General]\r\nl VariantCount = 1\r\nl PilotCount = 0\r\nl InventoryCount = 1\r\n"
        "st CampaignFile = \"data/campaign/campaign.fit\"\r\nl Group = 0\r\n"
        "l CompletedMissions = 0\r\n[Variant0]\r\nst Chassis = \"removed-mod.csv\"\r\n"
        "st VariantName = \"Removed mod variant\"\r\n[Inventory0]\r\n"
        "st Variant = \"Removed mod variant\"\r\nl DeploymentSlot = 1\r\nFITend\r\n";
    FitIniFile incompatible;
    REQUIRE(incompatible.open(missingChassis.data(), static_cast<int>(missingChassis.size())) == NO_ERR);
    CHECK_FALSE(logistics.load(incompatible));
    CHECK(logistics.getInventory() == inventory);
    CHECK(inventory[0]->getPilot() == &pilot);
    incompatible.close();
}
