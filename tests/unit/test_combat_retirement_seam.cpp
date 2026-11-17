#include "doctest.h"
#include "mission.h"
#include "weaponbolt.h"
#include "carnage.h"
#include "collsn.h"
#include "weaponfx.h"
#include "heap.h"
#include "team.h"
#include "comndr.h"
#include "mission_load_frame.h"
#include "combat_clock_archive.h"
#include "watch_list_save_data.h"
#include "object_slot_codec.h"
#include "logisticsdialog.h"
#include "mech.h"
#include "contact.h"
#include "warrior.h"
#include <bit>
#include <array>
#include <cstdlib>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <process.h>

extern "C" void mc2FreezeMoversSet(int, long);
extern bool renderObjects;
extern char TeamRelations[MAX_TEAMS][MAX_TEAMS];
extern char versionStamp[];

TEST_CASE("heat production/real mech Save and Load preserve heat on reused objects and clear legacy state") {
    using namespace mc2::save;
    struct Mech : BattleMech {
        BattleMechType type;
        Mech() { type.setAppearanceTypeName("heat-save-test-missing"); }
        ObjectTypePtr getObjectType() override { return &type; }
    };
    const bool oldInitialize = GameObject::initialize;
    GameObject::initialize = true;
    auto oldSensors = std::move(SensorManager);
    SensorManager = std::make_unique<SensorSystemManager>();
    SensorManager->init(false);
    auto* oldComponents = MasterComponent::masterList;
    MasterComponent component;
    component.stats.sensor.range = 0;
    MasterComponent::masterList = &component;
    auto* oldPilot = MechWarrior::warriorList[0];
    MechWarrior pilot;
    MechWarrior::warriorList[0] = &pilot;
    const auto path = (std::filesystem::temp_directory_path() / "heat-mech-save.ims").string();
    {
        Mech mech;
        MechRecord seed;
        seed.value.teamId = -1;
        seed.value.screenPos = Stuff::Vector4D(0, 0, 0, 0);
        seed.load(mech);
        auto releaseSensor = [&] { SensorManager->freeSensor(mech.sensorSystem); mech.sensorSystem = nullptr; };
        releaseSensor();
        mech.heatState.heatMilli = 12345;
        mech.heatState.alphaMask.set(64); mech.heatState.alphaMask.set(71);
        mech.heatState.alphaExpireTick = 777;
        PacketFile file;
        REQUIRE(file.create(path.c_str()) == NO_ERR);
        file.reserve(1); mech.Save(&file, 0); file.close();
        REQUIRE(file.open(path.c_str()) == NO_ERR);
        file.seekPacket(0);
        Bytes bytes(file.getPacketSize());
        REQUIRE(file.readPacket(0, bytes.data()) == bytes.size());
        file.close();
        MechRecord saved;
        REQUIRE(decodeTaggedRecord(bytes, saved, mechTag));
        REQUIRE(saved.version == 2);
        for (int repeat = 0; repeat < 2; ++repeat) {
            mech.heatState.heatMilli = 99999; mech.heatState.alphaMask.set();
            saved.load(mech);
            CHECK(mech.heatState.heatMilli == 12345);
            CHECK(mech.heatState.alphaMask == saved.heat.alphaMask);
            CHECK(mech.heatState.alphaExpireTick == 777);
            releaseSensor();
        }
        saved.version = 1;
        REQUIRE(decodeTaggedRecord(encodeTaggedRecord(saved, mechTag), saved, mechTag));
        mech.heatState.heatMilli = 123; mech.heatState.alphaMask.set();
        saved.load(mech);
        CHECK(mech.heatState.heatMilli == 0); CHECK(mech.heatState.alphaMask.none());
        CHECK(mech.heatState.alphaExpireTick == 0);
        releaseSensor();
        MechData native = saved.value; // native legacy ABI, without an extension
        mech.heatState.heatMilli = 123; mech.heatState.alphaMask.set(); mech.heatState.alphaExpireTick = 9;
        mech.Load(&native);
        CHECK(mech.heatState.heatMilli == 0); CHECK(mech.heatState.alphaMask.none());
        CHECK(mech.heatState.alphaExpireTick == 0);
        releaseSensor();
    }
    std::filesystem::remove(path);
    MechWarrior::warriorList[0] = oldPilot;
    MasterComponent::masterList = oldComponents;
    SensorManager = std::move(oldSensors);
    GameObject::initialize = oldInitialize;
}

TEST_CASE("heat production/capacity re-derive keeps heat and tracks sink loss and recovery") {
    using namespace mc2::save;
    struct Mech : BattleMech {
        BattleMechType type;
        Mech() { type.setAppearanceTypeName("heat-capacity-test-missing"); }
        ObjectTypePtr getObjectType() override { return &type; }
    };
    const bool oldInitialize = GameObject::initialize;
    GameObject::initialize = true;
    auto oldSensors = std::move(SensorManager);
    SensorManager = std::make_unique<SensorSystemManager>();
    SensorManager->init(false);
    auto* oldComponents = MasterComponent::masterList;
    MasterComponent components[2];
    components[0].stats.sensor.range = 0;
    components[1].form = COMPONENT_FORM_HEATSINK;
    components[1].health = 1;
    components[1].setHeatDissipation(3.0f);
    MasterComponent::masterList = components;
    auto* oldPilot = MechWarrior::warriorList[0];
    MechWarrior pilot;
    MechWarrior::warriorList[0] = &pilot;
    {
        Mech mech;
        MechRecord seed;
        seed.value.teamId = -1;
        seed.value.screenPos = Stuff::Vector4D(0, 0, 0, 0);
        seed.load(mech);
        SensorManager->freeSensor(mech.sensorSystem); mech.sensorSystem = nullptr;
        mech.numOther = 1;
        mech.inventory[0].masterID = 1;
        mech.inventory[0].health = 1;
        mech.inventory[0].disabled = false;
        mech.rederiveHeatCapacity();
        const auto full = mech.heatState.capacity;
        CHECK(full > 0);
        mech.heatState.heatMilli = 555; mech.heatState.alphaMask.set(3); mech.heatState.alphaExpireTick = 42;
        mech.inventory[0].disabled = true; // sink destroyed
        mech.recomputeHeatCapacity();
        CHECK(mech.heatState.capacity < full);
        mech.inventory[0].disabled = false; // Mover::recover / repairAll restore it
        mech.recomputeHeatCapacity();
        CHECK(mech.heatState.capacity == full);
        mech.rederiveHeatCapacity(); // post-logistics save-load pass
        CHECK(mech.heatState.capacity == full);
        CHECK(mech.heatState.heatMilli == 555);
        CHECK(mech.heatState.alphaMask.test(3));
        CHECK(mech.heatState.alphaExpireTick == 42);
        mech.restoreHeatCapacity(); // object restore path still starts cold
        CHECK(mech.heatState.heatMilli == 0);
        CHECK(mech.heatState.capacity == full);
    }
    MechWarrior::warriorList[0] = oldPilot;
    MasterComponent::masterList = oldComponents;
    SensorManager = std::move(oldSensors);
    GameObject::initialize = oldInitialize;
}

struct CombatRetirementSeam {
    static bool rejects(Mission& owner, const char* path) {
        MissionLoadFrame frame{};
        frame.loadFileName = path;
        return owner.openSaveFile(frame);
    }
    static bool rejectsVersion(Mission& owner, const char* path) {
        MissionLoadFrame frame{};
        frame.loadFileName = path;
        return owner.openSaveFile(frame) && frame.incompatibleSave;
    }
    static void update(Mission& owner) { owner.updateObjectManager(); }
    static void complete(Mission& owner) { owner.completeOrderSimulationTick(); }
    static void reinit(Mission& owner) { owner.initTeamRelations(MissionLoadType::MISSION_LOAD_SP_QUICKSTART); }
    // Mission teardown also shuts down ABL and the renderer, absent in this fixture.
    union Storage {
        Mission owner;
        Storage() : owner() {}
        ~Storage() { CombatRetirementSeam::release(owner); }
    };
    static void release(Mission& owner) {
        std::destroy_at(&owner.orderIngress); std::destroy_at(&owner.orderRecipients);
        std::destroy_at(&owner.authoredMissionText); std::destroy_at(&owner.authoredOpordPilots);
        std::destroy_at(&owner.authoredRoutes); std::destroy_at(&owner.routeExecutions);
        std::destroy_at(&owner.rejectedRouteSignals); std::destroy_at(&owner.authoredDiagnostics);
        std::destroy_at(&owner.orderFixtureReceipt);
    }
};

TEST_CASE("save fixtures: shipped legacy mod saves refuse before teardown") {
    CombatRetirementSeam::Storage storage;
    CHECK(CombatRetirementSeam::rejectsVersion(storage.owner,
        "tests/fixtures/saves/omnitech-testgame.ims"));
    for (int index : {1, 2, 3, 4, 5, 7}) {
        auto path = std::format("tests/fixtures/saves/after_mc2_{:02}.fit", index);
        REQUIRE(std::filesystem::is_regular_file(path));
        CHECK_FALSE(isCorrectVersionCampaignSave(path));
    }
}

TEST_CASE("save fixtures: W86 W87 commander and team production value cutover") {
    // These two archived files share the packet positions pinned in manifest.json.
    // Keep the live static registries isolated even when a REQUIRE aborts the case.
    struct Registries {
        decltype(std::to_array(Team::teams)) teams = std::to_array(Team::teams);
        decltype(std::to_array(Commander::commanders)) commanders = std::to_array(Commander::commanders);
        Team* homeTeam = Team::home;
        Commander* homeCommander = Commander::home;
        long teamCount = Team::numTeams, commanderCount = Commander::numCommanders;
        decltype(Team::relations) relations;
        decltype(Team::noPain) noPain;
        Registries() {
            std::memcpy(relations, Team::relations, sizeof(relations));
            std::memcpy(noPain, Team::noPain, sizeof(noPain));
            std::fill(std::begin(Team::teams), std::end(Team::teams), nullptr);
            std::fill(std::begin(Commander::commanders), std::end(Commander::commanders), nullptr);
            Team::numTeams = Commander::numCommanders = 0;
            Team::home = nullptr; Commander::home = nullptr;
        }
        void clear() {
            for (auto*& commander : Commander::commanders) { delete commander; commander = nullptr; }
            for (auto*& team : Team::teams) { delete team; team = nullptr; }
            Team::numTeams = Commander::numCommanders = 0;
            Team::home = nullptr; Commander::home = nullptr;
        }
        ~Registries() {
            clear();
            std::copy(teams.begin(), teams.end(), Team::teams);
            std::copy(commanders.begin(), commanders.end(), Commander::commanders);
            Team::home = homeTeam; Commander::home = homeCommander;
            Team::numTeams = teamCount; Commander::numCommanders = commanderCount;
            std::memcpy(Team::relations, relations, sizeof(relations));
            std::memcpy(Team::noPain, noPain, sizeof(noPain));
        }
    } registries;
    CombatRetirementSeam::Storage storage;
    for (const char* fixture : {"w86-mc2_03.ims", "w87-mc2_03.ims"}) {
        const auto path = std::string("tests/fixtures/saves/") + fixture;
        REQUIRE_FALSE(CombatRetirementSeam::rejects(storage.owner, path.c_str()));
        PacketFile legacy;
        REQUIRE(legacy.open(path.c_str()) == NO_ERR);
        long header = -1;
        REQUIRE_FALSE(GameObjectManager::checkSavedObjects(&legacy, &header, true));
        REQUIRE(header > 0);
        ObjectManagerData manager{};
        REQUIRE(legacy.readPacket(header, reinterpret_cast<MemoryPtr>(&manager)) == sizeof(manager));
        const long watch = header + manager.maxObjects + 2;
        const auto taggedPath = (std::filesystem::temp_directory_path() /
            std::format("mc2-manager-values-{}.pak", _getpid())).string();
        PacketFile tagged;
        REQUIRE(tagged.create(taggedPath.c_str()) == NO_ERR);
        tagged.reserve(legacy.getNumPackets());
        for (int packet = 0; packet < legacy.getNumPackets(); ++packet) {
            legacy.seekPacket(packet);
            const auto size = legacy.getPacketSize();
            if (!size) continue;
            mc2::save::Bytes bytes(size);
            REQUIRE(legacy.readPacket(packet, bytes.data()) == size);
            if (packet == header)
                bytes = mc2::save::encodeTaggedRecord(manager, mc2::save::objectManagerTag);
            if (packet == watch) {
                mc2::save::SavedWatchList watches;
                watches.indices.resize(manager.maxObjects + 1);
                REQUIRE(bytes.size() == watches.indices.size() * 4);
                std::memcpy(watches.indices.data(), bytes.data(), bytes.size());
                bytes = mc2::save::encodeTaggedRecord(watches, mc2::save::watchListTag);
            }
            if (packet > header && packet < watch) {
                auto encode = [&]<class Record, class Data>(int c) {
                    Data value{};
                    REQUIRE(bytes.size() == sizeof(value));
                    std::memcpy(&value, bytes.data(), sizeof(value));
                    bytes = mc2::save::encodeTaggedRecord(Record{value}, mc2::save::objectSlotTags[c]);
                    REQUIRE_FALSE(bytes.empty());
                    CHECK(mc2::save::validObjectSlot(bytes, c, packet - header - 1));
                };
                using namespace mc2::save;
                using namespace mc2_saved_objects;
                if (size == sizeof(TerrainObjectData)) encode.operator()<TerrainRecord, TerrainObjectData>(kTerrain);
                else if (size == sizeof(BuildingData)) encode.operator()<BuildingRecord, BuildingData>(kBuilding);
                else if (size == sizeof(TurretData)) encode.operator()<TurretRecord, TurretData>(kTurret);
                else if (size == sizeof(GateData)) encode.operator()<GateRecord, GateData>(kGate);
                else if (size == sizeof(ArtilleryData)) encode.operator()<ArtilleryRecord, ArtilleryData>(kArtillery);
                else if (size == sizeof(CarnageData)) encode.operator()<CarnageRecord, CarnageData>(kCarnage);
                else if (size == sizeof(MechData)) encode.operator()<MechRecord, MechData>(kMech);
                else if (size == sizeof(GroundVehicleData)) encode.operator()<VehicleRecord, GroundVehicleData>(kVehicle);
                else if (size == sizeof(WeaponBoltData)) encode.operator()<WeaponBoltRecord, WeaponBoltData>(kBolt);
                else FAIL("unknown archived object record");
            }
            tagged.writePacket(packet, bytes.data(), static_cast<unsigned int>(bytes.size()), STORAGE_TYPE_ZLIB);
        }
        tagged.close();
        REQUIRE(tagged.open(taggedPath.c_str()) == NO_ERR);
        long found = -1;
        REQUIRE_FALSE(GameObjectManager::checkSavedObjects(&tagged, &found, true, true, true, true));
        CHECK(found == header);
        // The native-size probe must refuse a tagged run without its section opt-in.
        CHECK(GameObjectManager::checkSavedObjects(&tagged, nullptr, true).has_value());
        tagged.close();
        std::filesystem::remove(taggedPath);
        CHECK(Team::Load(&legacy, 3373) == 3376);
        CHECK(Commander::Load(&legacy, 6513) == 6516);
        REQUIRE(Team::home != nullptr);
        REQUIRE(Commander::home != nullptr);
        CHECK(Team::numTeams == 2);
        CHECK(Commander::numCommanders == 2);
        // Exercise the production >120 roster path as well as every native field.
        Team::home->rosterSize = 257;
        for (long i = 0; i < 257; ++i) Team::home->roster[i] = i + 1;
        const auto temporary = (std::filesystem::temp_directory_path() /
            std::format("mc2-command-values-{}.pak", _getpid())).string();
        PacketFile values;
        REQUIRE(values.create(temporary.c_str()) == NO_ERR);
        values.reserve(16);
        const auto teamsEnd = Team::Save(&values, 0);
        const auto end = Commander::Save(&values, teamsEnd);
        values.close();
        registries.clear();
        REQUIRE(values.open(temporary.c_str()) == NO_ERR);
        CHECK(Team::Load(&values, 0, true) == teamsEnd);
        CHECK(Commander::Load(&values, teamsEnd, true) == end);
        REQUIRE(Team::home != nullptr);
        CHECK(Team::home->rosterSize == 257);
        for (long i = 0; i < 257; ++i) CHECK(Team::home->roster[i] == i + 1);
        REQUIRE(Commander::home != nullptr);
        CHECK(Commander::home->getTeam() == Team::home);
        values.close();
        std::filesystem::remove(temporary);
        registries.clear();
    }
}

namespace {
struct World {
    Mission* oldMission = mission;
    GameObjectManager* oldObjects = ObjectManager;
    bool oldInitialize = GameObject::initialize;
    bool oldRender = renderObjects;
    float oldFrame = frameLength;
    long oldTurn = turn;
    decltype(Team::relations) oldRelations;
    decltype(TeamRelations) oldNetworkRelations;
    decltype(MPlayer) oldMultiplayer = MPlayer;
    CombatRetirementSeam::Storage storage;
    struct Watches : GameObjectManager {
        GameObject* entries[2]{};
        Watches() {
            watchList = entries; nextWatchID = 2;
            collisionSystem = nullptr; moverLineOfSightTable = nullptr;
            maxMechs = maxVehicles = maxMovers = 0;
        }
    };
    std::unique_ptr<Watches> objects;
    ObjectTypeManager* ownedTypes = nullptr;
    std::array<WeaponBoltType*, 3> ownedPolicies{};
    World() {
        std::memcpy(oldRelations, Team::relations, sizeof(oldRelations));
        std::memcpy(oldNetworkRelations, TeamRelations, sizeof(oldNetworkRelations));
        objects = std::make_unique<Watches>();
        GameObject::initialize = true;
        mission = &storage.owner;
        ObjectManager = objects.get();
        MPlayer = nullptr;
        renderObjects = false;
        mc2FreezeMoversSet(0, -1);
    }
    ~World() {
        if (ownedTypes) objects->objTypeManager = nullptr;
        objects.reset();
        for (auto* policy : ownedPolicies) delete policy;
        if (ownedTypes) {
            std::free(ownedTypes->table);
            ownedTypes->table = nullptr;
            auto* oldFile = ObjectTypeManager::objectFile;
            ObjectTypeManager::objectFile = nullptr;
            delete ownedTypes;
            ObjectTypeManager::objectFile = oldFile;
        }
        std::memcpy(Team::relations, oldRelations, sizeof(oldRelations));
        std::memcpy(TeamRelations, oldNetworkRelations, sizeof(oldNetworkRelations));
        turn = oldTurn;
        mc2FreezeMoversSet(0, -1);
        frameLength = oldFrame; renderObjects = oldRender; MPlayer = oldMultiplayer;
        ObjectManager = oldObjects; mission = oldMission;
        GameObject::initialize = oldInitialize;
        }
};
struct Bolt : WeaponBolt {
    WeaponBoltType* type;
    explicit Bolt(WeaponBoltType& value) : type(&value) {}
    ObjectTypePtr getObjectType() override { return type; }
    void textureHandles(DWORD mc, DWORD gos) { mcTextureHandle = mc; gosTextureHandle = gos; }
    DWORD textureHandle() const { return mcTextureHandle; }
    void splashShot() { weaponShot.init(0, -1, 10, 0, 0); }
};

void loadType(WeaponBoltType& type, std::string_view afterHit, bool beam = false) {
    type.destroy();
    type.init();
    type.setObjTypeNum(42);
    std::string text = "FITini\n";
    text += beam ? "[BeamProjectileData]\nb IsBeam = true\n" :
        "[BoltProjectileData]\nf ProjectileLength = 1\nf BulgeLength = 1\nf BulgeWidth = 1\n"
        "f Velocity = 1\nl HitEffect = 0\nl MissEffect = 0\nl FiringSoundFX = 0\n"
        "ul FrontRGB = 0\nul MidEdgeRGB = 0\nul MiddleRGB = 0\nul BackRGB = 0\n"
        "uc EdgeAlpha = 0\nuc BoltAlpha = 0\n";
    text += afterHit;
    text += "\n[ObjectType]\nl ExplosionObject = 0\nl DestroyedObject = 0\n\nFITend\n";
    const auto path = std::filesystem::temp_directory_path() /
        std::format("mc2-combat-retirement-seam-{}.fit", _getpid());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.write(text.data(), static_cast<std::streamsize>(text.size())).good());
    output.close();
    File source;
    REQUIRE(source.open(path.string().c_str(), READ) == NO_ERR);
    REQUIRE(type.init(&source, static_cast<unsigned long>(text.size())) == NO_ERR);
}
}

TEST_CASE("CombatRetirementProduction/texture allocation order never enters bolt saves") {
    World world;
    WeaponBoltType type;
    loadType(type, "");
    Bolt bolt(type);
    WeaponBoltData classic{}, remastered{};
    bolt.textureHandles(1125, 900);
    bolt.CopyTo(&classic);
    bolt.textureHandles(1121, 901);
    bolt.CopyTo(&remastered);
    CHECK(std::memcmp(&classic, &remastered, sizeof(classic)) == 0);
    CHECK(classic.mcTextureHandle == 0);
    CHECK(classic.gosTextureHandle == 0);
    // The unchanged versioned record reader must ignore nonzero slots from old saves.
    classic.mcTextureHandle = 1125;
    classic.gosTextureHandle = 900;
    bolt.Load(&classic);
    CHECK(bolt.textureHandle() == 1121);
    WeaponBoltData resaved{};
    bolt.CopyTo(&resaved);
    CHECK(std::memcmp(&resaved, &remastered, sizeof(resaved)) == 0);
}

// Trigger paths: code/carnage.*, code/weaponbolt*.cpp.
TEST_CASE("CombatRetirementProduction/splash hits all nearby units and buildings once") {
    World world;
    struct Target : GameObject {
        struct Passive : ObjectType { bool handleCollision(GameObjectPtr, GameObjectPtr) override { return false; } } type;
        ObjectTypePtr getObjectType() override { return &type; }
        float getExtentRadius() override { return 0; }
        void identify(int i) { watchID = i + 1; objectClass = i % 2 ? BUILDING : BATTLEMECH; }
        float damage = 0;
        long handleWeaponHit(WeaponShotInfoPtr shot, bool = false) override {
            damage += shot->damage; return NO_ERR;
        }
    } targets[5];
    ExplosionType type;
    struct Blast : Carnage {
        ExplosionType* type;
        ObjectTypePtr getObjectType() override { return type; }
    } blast;
    blast.type = &type; blast.effectId = 0;
    blast.init(CARNAGE_EXPLOSION);
    blast.objectClass = EXPLOSION; blast.setExists(true); blast.setPosition(glm::vec3(0), false);
    blast.setExtentRadius(20); blast.setExplDmg(10); blast.info.explosion.chunkSize = 10;
    for (int i = 0; i < 5; ++i) {
        targets[i].setExists(true); targets[i].identify(i);
        targets[i].setPosition(glm::vec3(i * 9, 0, 0), false);
    }
    world.objects->entries[1] = &targets[0];
    WeaponBoltType boltType;
    Bolt bolt(boltType);
    bolt.setAimedTarget(1); bolt.splashShot();
    REQUIRE(bolt.deliverAimedAreaHit(&blast));
    CarnageData saved{}; blast.CopyTo(&saved);
    blast.setCollisionFreeFromWID(0); blast.Load(&saved);
    for (auto& target : targets) CollisionSystem::detectCollision(&blast, &target);
    for (int i = 0; i < 5; ++i) CHECK(targets[i].damage == (i < 3 ? 10 : 0));
    blast.init(CARNAGE_EXPLOSION);
    CHECK(blast.getCollisionFreeFromWID() == 0);
}
TEST_CASE("CombatRetirementProduction/rejected explosion types leave the carnage pool untouched") {
    World world;
    auto& objects = *world.objects;
    objects.objTypeManager = world.ownedTypes = new ObjectTypeManager;
    auto& types = *objects.objTypeManager;
    types.numObjectTypes = 2;
    types.table = static_cast<ObjectTypePtr*>(std::calloc(2, sizeof(ObjectTypePtr)));
    objects.carnageStore = {new Carnage, new Carnage};
    objects.carnage = objects.carnageStore.data();
    objects.numCarnage = 2;
    objects.currentCarnageIndex = 0;
    struct Effects : WeaponEffects {
        WeaponEffects* previous = weaponEffects;
        EffectData entry{};
        Effects() { effects = &entry; numEffects = 1; std::strcpy(entry.effectName, "NONE"); weaponEffects = this; }
        ~Effects() { weaponEffects = previous; effects = nullptr; }
    } effects;
    glm::vec3 position{1, 2, 3};
    effects.entry.effectObjNum = 0; // FX-only content maps to the reserved null type.
    CHECK(objects.createExplosion(0, nullptr, position, 10, 20) == nullptr);
    CHECK(objects.currentCarnageIndex == 0);
    CHECK(objects.carnage[1]->carnageType == CARNAGE_NONE);
    WeaponBoltType wrongKind;
    types.table[1] = &wrongKind;
    effects.entry.effectObjNum = 1;
    CHECK(objects.createExplosion(0, nullptr, position, 10, 20) == nullptr);
    CHECK(objects.currentCarnageIndex == 0);
    CHECK(objects.carnage[1]->carnageType == CARNAGE_NONE);
}

TEST_CASE("CombatRetirementProduction/object update advances clock and real freeze hook suspends it") {
    World world;
    auto& owner = world.storage.owner;
    const auto originalTurn = turn;
    frameLength = 1.0f / 30;
    for (int i = 0; i < 30; ++i) {
        CombatRetirementSeam::update(owner);
        CombatRetirementSeam::complete(owner);
    }
    CHECK(owner.getCombatClock().ticks == 1000);
    const auto fraction = owner.getCombatClock().frac;
    mc2FreezeMoversSet(1, -1);
    for (int i = 0; i < 5; ++i) CombatRetirementSeam::update(owner);
    CHECK(owner.getCombatClock().ticks == 1000);
    CHECK(owner.getCombatClock().frac == fraction);
    CHECK(turn == originalTurn);
    mc2FreezeMoversSet(0, -1);
    CombatRetirementSeam::update(owner);
    CombatRetirementSeam::complete(owner);
    CHECK(owner.getCombatClock().ticks > 1000);
}

TEST_CASE("CombatRetirementProduction/mission init reset helper clears the previous epoch") {
    World world;
    auto& owner = world.storage.owner;
    owner.advanceCombatClock(0.00025f);
    REQUIRE(owner.getCombatClock().frac > 0);
    owner.advanceCombatClock(1.0f);
    REQUIRE(owner.getCombatClock().ticks == 1000);
    CombatRetirementSeam::reinit(owner);
    CHECK(owner.getCombatClock().ticks == 0);
    CHECK(owner.getCombatClock().frac == 0);
}

TEST_CASE("CombatRetirementProduction/real type policy and bolt init prevent provenance and reuse errors") {
    World world;
    world.storage.owner.advanceCombatClock(1.0f);
    WeaponBoltType type;
    loadType(type, "");
    CHECK(type.retirePolicy.source == "absent_default");
    CHECK(type.retirePolicy.ticks == 0);
    loadType(type, "f AfterHitTime = 0\n");
    CHECK(type.retirePolicy.source == "AfterHitTime");
    CHECK(type.retirePolicy.ticks == 0);
    loadType(type, "f AfterHitTime = 0.5\n");
    CHECK(type.retirePolicy.source == "AfterHitTime");
    REQUIRE(type.retirePolicy.ticks == 500);
    Bolt bolt(type);
    bolt.init(true, &type);
    bolt.armRetireDeadline();
    CHECK(bolt.retireDeadline.armed);
    CHECK(bolt.retireDeadline.tick == 1500);
    world.storage.owner.advanceCombatClock(1.0f);
    bolt.armRetireDeadline();
    CHECK(bolt.retireDeadline.tick == 1500);
    loadType(type, "f AfterHitTime = nan\n");
    CHECK_FALSE(type.retirePolicy.ticks);
    bolt.init(true, &type);
    CHECK_FALSE(bolt.retireDeadline.attempted);
    bolt.armRetireDeadline();
    CHECK_FALSE(bolt.retireDeadline.armed);
    loadType(type, "f AfterHitTime = nan\n", true);
    CHECK(type.retirePolicy.source == "beam_default");
    CHECK(type.retirePolicy.ticks == 0);
    bolt.init(true, &type);
    bolt.armRetireDeadline();
    CHECK(bolt.retireDeadline.armed);
    CHECK(bolt.retireDeadline.tick == 2000);
}

TEST_CASE("CombatRetirementProduction/native loader preserves absolute deadlines and rejects malformed state before teardown") {
    using namespace mc2::combat;
    World world;
    auto& owner = world.storage.owner;
    auto& objects = *world.objects;
    objects.objTypeManager = world.ownedTypes = new ObjectTypeManager;
    auto& types = *objects.objTypeManager;
    types.numObjectTypes = 64;
    types.table = static_cast<ObjectTypePtr*>(std::malloc(64 * sizeof(ObjectTypePtr)));
    std::memset(types.table, 0, 64 * sizeof(ObjectTypePtr));
    auto* normal = new WeaponBoltType;
    auto* beam = new WeaponBoltType;
    auto* invalid = new WeaponBoltType;
    world.ownedPolicies = {normal, beam, invalid};
    loadType(*normal, "f AfterHitTime = 0.5\n");
    loadType(*beam, "", true);
    loadType(*invalid, "f AfterHitTime = nan\n");
    beam->setObjTypeNum(43); invalid->setObjTypeNum(44);
    types.table[42] = normal; types.table[43] = beam; types.table[44] = invalid;
    owner.restoreCombatClock({1000, 0.875});
    const std::array<RetireDeadline, 8> deadlines{{{}, {1500, true, true, "AfterHitTime"},
        {900, true, true, "AfterHitTime"}, {0, true, true, "AfterHitTime"},
        {1000, true, true, "beam_default"}, {0, false, true, "invalid_policy"},
        {0, false, true, "deadline_overflow"}, {999, true, true, "AfterHitTime"}}};
    const auto path = (std::filesystem::temp_directory_path() /
        std::format("mc2-combat-save-{}.ims", _getpid())).string();
    const auto resavePath = path + ".resave";
    auto write = [&](SavedCombatClock clock, int mutation) {
        PacketFile file;
        REQUIRE(file.create(path.c_str()) == NO_ERR);
        file.reserve(32);
        file.writePacket(0, reinterpret_cast<MemoryPtr>(versionStamp),
            static_cast<unsigned int>(std::strlen(versionStamp) + 1), STORAGE_TYPE_RAW);
        char empty[] = "";
        file.writePacket(1, reinterpret_cast<MemoryPtr>(empty), 1, STORAGE_TYPE_RAW);
        file.writePacket(2, reinterpret_cast<MemoryPtr>(empty), 1, STORAGE_TYPE_RAW);
        file.writePacket(3, reinterpret_cast<MemoryPtr>(&clock), mutation == 4 ? 8 : sizeof(clock), STORAGE_TYPE_RAW);
        ObjectManagerData header{};
        header.numWeapons = static_cast<int>(deadlines.size());
        file.writePacket(4, reinterpret_cast<MemoryPtr>(&header), sizeof(header), STORAGE_TYPE_RAW);
        file.writePacket(5, nullptr, 0, STORAGE_TYPE_NUL);
        for (int i = 0; i < static_cast<int>(deadlines.size()); ++i) {
            auto& type = i == 4 ? *beam : i == 5 ? *invalid : *normal;
            Bolt bolt(type);
            bolt.init(true, &type);
            bolt.setHandle(i + 1);
            bolt.setExists(i != 7);
            bolt.retireDeadline = deadlines[i];
            WeaponBoltData data{};
            bolt.CopyTo(&data);
            data.hitTarget = i != 0;
            if (i == 1 && mutation == 1) data.retireDeadlineState = static_cast<SavedDeadlineState>(255);
            if (i == 1 && mutation == 2) data.retireDeadlineTick = invalidTick;
            if (i == 0 && mutation == 3) data.retireDeadlineTick = 1;
            file.writePacket(6 + i, reinterpret_cast<MemoryPtr>(&data), sizeof(data), STORAGE_TYPE_ZLIB);
        }
        int32_t watches[9]{};
        file.writePacket(14, reinterpret_cast<MemoryPtr>(watches), sizeof(watches), STORAGE_TYPE_RAW);
        file.close();
    };
    write({1000, 0.875}, 0);
    PacketFile file;
    REQUIRE(file.open(path.c_str()) == NO_ERR);
    REQUIRE_FALSE(GameObjectManager::checkSavedObjects(&file));
    const auto savedClock = mc2::save::readSavedCombatClock(file, false);
    REQUIRE(savedClock);
    owner.resetCombatClock();
    owner.restoreCombatClock(*savedClock);
    REQUIRE(objects.Load(&file, 4) == 15);
    file.close();
    if (std::getenv("COMBAT_SEAM_MUTATE_RESTORED_DEADLINE")) ++objects.weapons[1]->retireDeadline.tick;
    for (int i = 0; i < static_cast<int>(deadlines.size()); ++i) {
        const auto& actual = objects.weapons[i]->retireDeadline;
        CHECK(actual.tick == deadlines[i].tick);
        CHECK(actual.armed == deadlines[i].armed);
        CHECK(actual.attempted == deadlines[i].attempted);
        CHECK(actual.source == deadlines[i].source);
    }
    CHECK_FALSE(objects.weapons[7]->getExists());
    auto uninterrupted = CombatClock{1000, 0.875};
    for (int i = 0; i < 30; ++i) {
        owner.advanceCombatClock(1.0f / 30);
        uninterrupted.advance(1.0f / 30);
        CHECK(owner.getCombatClock().ticks == uninterrupted.ticks);
        CHECK(std::bit_cast<std::uint64_t>(owner.getCombatClock().frac) == std::bit_cast<std::uint64_t>(uninterrupted.frac));
    }
    PacketFile resave;
    REQUIRE(resave.create(resavePath.c_str()) == NO_ERR);
    resave.reserve(32);
    SavedCombatClock continued{owner.getCombatClock().ticks, owner.getCombatClock().frac};
    resave.writePacket(0, reinterpret_cast<MemoryPtr>(const_cast<char*>(mc2::save::saveStampV2)),
        sizeof(mc2::save::saveStampV2), STORAGE_TYPE_RAW);
    mc2::save::CombatClockValue value{continued.ticks, continued.frac};
    mc2::save::Archive archive;
    archive(value);
    REQUIRE(archive.good());
    resave.writePacket(3, const_cast<MemoryPtr>(archive.bytes().data()),
        static_cast<unsigned int>(archive.bytes().size()), STORAGE_TYPE_RAW);
    objects.Save(&resave, 4);
    resave.close();
    REQUIRE(resave.open(resavePath.c_str()) == NO_ERR);
    for (int i = 0; i < static_cast<int>(deadlines.size()); ++i) {
        resave.seekPacket(6 + i);
        mc2::save::Bytes bytes(resave.getPacketSize());
        REQUIRE(resave.readPacket(6 + i, bytes.data()) == bytes.size());
        const auto restored = mc2::save::decodeWeaponBoltRecord(bytes);
        REQUIRE(restored);
        CHECK(restored->retireDeadlineTick == deadlines[i].tick);
        CHECK(restored->retireDeadlineState == deadlines[i].savedState());
    }
    REQUIRE_FALSE(GameObjectManager::checkSavedObjects(&resave, nullptr, true, true, true, true));
    objects.objTypeManager = nullptr;
    objects.destroy();
    objects.objTypeManager = world.ownedTypes;
    owner.resetCombatClock();
    owner.restoreCombatClock(*mc2::save::readSavedCombatClock(resave, true));
    REQUIRE(objects.Load(&resave, 4, true, true, true) == 15);
    CHECK(owner.getCombatClock().ticks == continued.ticks);
    CHECK(std::bit_cast<std::uint64_t>(owner.getCombatClock().frac) == std::bit_cast<std::uint64_t>(continued.frac));
    for (int i = 0; i < static_cast<int>(deadlines.size()); ++i) {
        CHECK(objects.weapons[i]->retireDeadline.tick == deadlines[i].tick);
        CHECK(objects.weapons[i]->retireDeadline.savedState() == deadlines[i].savedState());
        objects.weapons[i]->armRetireDeadline();
        if (i != 0) CHECK(objects.weapons[i]->retireDeadline.tick == deadlines[i].tick);
        else CHECK(objects.weapons[i]->retireDeadline.tick == continued.ticks + 500);
    }
    resave.close();
    const auto before = owner.getCombatClock();
    auto* liveBolt = objects.weapons[1];
    for (int mutation = 1; mutation <= 4; ++mutation) {
        write({1000, 0.875}, mutation);
        CHECK(CombatRetirementSeam::rejects(owner, path.c_str()));
        CHECK(objects.weapons[1] == liveBolt);
        CHECK(owner.getCombatClock().ticks == before.ticks);
    }
    for (const auto clock : {SavedCombatClock{invalidTick, 0}, {1000, -0.1}, {1000, 1},
            {1000, std::numeric_limits<double>::quiet_NaN()}, {1000, std::numeric_limits<double>::infinity()}}) {
        write(clock, 0);
        CHECK(CombatRetirementSeam::rejects(owner, path.c_str()));
        CHECK(objects.weapons[1] == liveBolt);
        CHECK(owner.getCombatClock().frac == before.frac);
    }
}
