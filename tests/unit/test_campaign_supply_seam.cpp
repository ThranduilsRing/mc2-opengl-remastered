// tests/unit/test_campaign_supply_seam.cpp
// The campaign weapon-supply and vehicle-salvage decisions, compiled verbatim from code/logisticsdata_modify.cpp and
// code/logisticsdata_save.cpp by the mc2_height_slice step in CMakeLists.txt (regenerated on source change). The doubles supply
// only the logistics inventory: no engine objects, no live play.
#include "doctest.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "economy_rules.h"
#include "inifile.h"

namespace {

struct LogisticsComponent {
    int id = 0;
    bool weapon = true, available = true;
    bool isWeapon() const { return weapon; }
    int getID() const { return id; }
    bool isAvailable() const { return available; }
};
struct LogisticsVariant {
    struct Chassis {
        bool vehicle = false;
        long fitID = -1;
        bool isVehicle() const { return vehicle; }
    } chassis;
    std::vector<LogisticsComponent*> parts;
    const Chassis* getChassis() const { return &chassis; }
    long getComponentCount() const { return static_cast<long>(parts.size()); }
    int getComponents(long& count, LogisticsComponent** array) const {
        count = static_cast<long>(parts.size());
        for (long i = 0; i < count; ++i) array[i] = parts[i];
        return 0;
    }
};
struct Team { static const Team home; };
const Team Team::home;
constexpr int MOVETYPE_AIR = 3;
struct Vehicle {
    bool enemy = true, air = false, disabled = false, destroyed = false;
    long handle = 0;
    bool isEnemy(const Team&) const { return enemy; }
    int getMoveType() const { return air ? MOVETYPE_AIR : 1; }
    bool isDisabled() const { return disabled; }
    bool isDestroyed() const { return destroyed; }
    long getTypeHandle() const { return handle; }
};
struct ObjectManagerType {
    std::vector<Vehicle> vehicles;
    int numVehicles = 0;
    Vehicle* getVehicle(long i) { return &vehicles[static_cast<std::size_t>(i)]; }
};
ObjectManagerType* ObjectManager = nullptr;
struct MissionInfo { mc2::rules::WeaponSupply weaponSupply; };
struct LogisticsData {
    static LogisticsData* instance;
    MissionInfo* missionInfo = nullptr;
    LogisticsVariant* oldVariant = nullptr;
    std::unique_ptr<LogisticsVariant> modifiedVariant;
    std::vector<LogisticsComponent> components;
    std::vector<std::unique_ptr<LogisticsVariant>> variants;
    std::vector<long> salvaged;   // fitID of every variant handed to addMechToInventory
    mc2::rules::GameRules campaignRules;
    const mc2::rules::GameRules& rules() const { return campaignRules; }
    std::optional<std::reference_wrapper<LogisticsComponent>> getComponent(int id) {
        for (auto& component : components) if (component.id == id) return component;
        return std::nullopt;
    }
    void addMechToInventory(LogisticsVariant* variant, void*, int) { salvaged.push_back(variant->getChassis()->fitID); }
    bool canAddWeapon(LogisticsComponent* component);
    bool hasRecoveredWeapon(LogisticsComponent* component);
    bool settleWeaponSupply(LogisticsVariant* target, bool commit);
    void completeMission() {
#include "vehicle_salvage.inc"
    }
};
LogisticsData* LogisticsData::instance = nullptr;

#include "weapon_supply.inc"

Vehicle vehicle(bool enemy, bool destroyed, bool disabled, bool air, long handle) {
    Vehicle v;
    v.enemy = enemy; v.destroyed = destroyed; v.disabled = disabled; v.air = air; v.handle = handle;
    return v;
}
}

TEST_CASE("campaign supply: stripping a salvaged mech returns its weapons to the spares and the cap still holds") {
    LogisticsData data; LogisticsData::instance = &data;
    MissionInfo mission; data.missionInfo = &mission;
    data.campaignRules.weaponSupply = 1;   // Limited (2)
    const LogisticsComponent laser{11}, ppc{12}, gauss{13, true, false};   // the gauss is recovered tech only
    data.components = {laser, ppc, gauss};
    LogisticsVariant salvaged;   // as it came out of the mission
    salvaged.parts = {&data.components[0], &data.components[2]};
    LogisticsVariant roster;    // another mech of the roster, before its edits
    roster.parts = {&data.components[0]};

    // Roster mech: a recovered weapon needs a spare; the mission's purchases cap the available ones.
    data.oldVariant = &roster;
    data.modifiedVariant = std::make_unique<LogisticsVariant>(roster);
    CHECK_FALSE(data.canAddWeapon(&data.components[2]));
    CHECK(data.canAddWeapon(&data.components[1]));
    mission.weaponSupply.bought = {{12, 2}};
    CHECK_FALSE(data.canAddWeapon(&data.components[1]));   // two bought this mission already

    // Lab, salvaged mech: the gauss comes off into the spares and is offered again (the component list).
    data.oldVariant = &salvaged;
    data.modifiedVariant = std::make_unique<LogisticsVariant>(salvaged);
    data.modifiedVariant->parts.pop_back();
    CHECK(data.settleWeaponSupply(data.modifiedVariant.get(), true));
    CHECK(mission.weaponSupply.spare.at(13) == 1);
    CHECK(data.hasRecoveredWeapon(&data.components[2]));
    const std::string path = "campaign_supply_seam.fit";   // the campaign save's [WeaponSupply] block keeps it
    {
        FitIniFile out;
        REQUIRE(out.create(path.c_str()) == NO_ERR);
        mission.weaponSupply.save(out);
        out.close();
    }
    MissionInfo restored;
    {
        FitIniFile in;
        REQUIRE(in.open(path.c_str()) == NO_ERR);
        restored.weaponSupply.load(in);
        in.close();
    }
    CHECK(restored.weaponSupply.spare.at(13) == 1);
    CHECK(restored.weaponSupply.bought.at(12) == 2);
    std::remove(path.c_str());

    // The one spare fits the roster mech once; the purchase cap is untouched by it.
    data.oldVariant = &roster;
    data.modifiedVariant = std::make_unique<LogisticsVariant>(roster);
    CHECK(data.canAddWeapon(&data.components[2]));
    data.modifiedVariant->parts.push_back(&data.components[2]);
    CHECK_FALSE(data.canAddWeapon(&data.components[2]));   // one spare only
    CHECK(data.settleWeaponSupply(data.modifiedVariant.get(), true));
    CHECK(mission.weaponSupply.spare.at(13) == 0);
    CHECK(mission.weaponSupply.bought.at(12) == 2);
    CHECK_FALSE(data.hasRecoveredWeapon(&data.components[2]));

    // MC2 Default (unlimited): nothing is refused and nothing is tracked as recovered.
    data.campaignRules.weaponSupply = 0;
    CHECK(data.canAddWeapon(&data.components[2]));
    CHECK_FALSE(data.hasRecoveredWeapon(&data.components[2]));
    CHECK(data.settleWeaponSupply(nullptr, true));
    LogisticsData::instance = nullptr;
}

TEST_CASE("vehicle salvage: a completed mission hands enemy wrecks to the bay only under both toggles") {
    LogisticsData data; LogisticsData::instance = &data;
    MissionInfo mission; data.missionInfo = &mission;
    ObjectManagerType manager; ObjectManager = &manager;
    manager.vehicles = {vehicle(true, true, false, false, 21), vehicle(true, false, true, false, 22), vehicle(false, true, false, false, 21),
                        vehicle(true, true, false, true, 23), vehicle(true, false, false, false, 24), vehicle(true, true, false, false, 99),
                        vehicle(true, true, false, false, 21)};
    manager.numVehicles = 7;
    const auto chassis = [](bool isVehicle, long fitID) {
        auto variant = std::make_unique<LogisticsVariant>();
        variant->chassis.vehicle = isVehicle;
        variant->chassis.fitID = fitID;
        return variant;
    };
    for (const auto& [isVehicle, fitID] : {std::pair(true, 24L), std::pair(false, 99L), std::pair(true, 22L), std::pair(true, 21L)})
        data.variants.push_back(chassis(isVehicle, fitID));
    data.campaignRules.mods = "@salvage-all";
    data.completeMission();
    CHECK(data.salvaged.empty());   // Salvage Everything alone never takes vehicles
    data.campaignRules.mods = "@salvage-vehicles";
    data.completeMission();
    CHECK(data.salvaged.empty());
    data.campaignRules.mods = "@salvage-all,@salvage-vehicles";
    mission.weaponSupply.bought = {{11, 2}};
    data.completeMission();
    // Each enemy wreck or capture once; own, flying, intact and chassis-less (a mech shares handle 99) vehicles are left.
    CHECK(data.salvaged == std::vector<long>({21, 22, 21}));
    CHECK(mission.weaponSupply.bought.empty());   // a completed mission replenishes the purchase allowance
    LogisticsData::instance = nullptr;
    ObjectManager = nullptr;
}
