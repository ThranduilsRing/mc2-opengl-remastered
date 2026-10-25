#include "doctest.h"
#include "mission.h"
#include "weaponbolt.h"
#include "carnage.h"
#include "weaponfx.h"
#include "heap.h"
#include "team.h"
#include "mission_load_frame.h"
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

struct CombatRetirementSeam {
    static bool rejects(Mission& owner, const char* path) {
        MissionLoadFrame frame{};
        frame.loadFileName = path;
        return owner.openSaveFile(frame);
    }
    static void update(Mission& owner) { owner.updateObjectManager(); }
    static void complete(Mission& owner) { owner.completeOrderSimulationTick(); }
    static void reinit(Mission& owner) { owner.initTeamRelations(MISSION_LOAD_SP_QUICKSTART); }
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
    const auto savedClock = readCombatClock(file);
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
    resave.writePacket(3, reinterpret_cast<MemoryPtr>(&continued), sizeof(continued), STORAGE_TYPE_RAW);
    objects.Save(&resave, 4);
    resave.close();
    REQUIRE(resave.open(resavePath.c_str()) == NO_ERR);
    for (int i = 0; i < static_cast<int>(deadlines.size()); ++i) {
        WeaponBoltData restored{};
        REQUIRE(resave.readPacket(6 + i, reinterpret_cast<MemoryPtr>(&restored)) == sizeof(restored));
        CHECK(restored.retireDeadlineTick == deadlines[i].tick);
        CHECK(restored.retireDeadlineState == deadlines[i].savedState());
    }
    REQUIRE_FALSE(GameObjectManager::checkSavedObjects(&resave));
    objects.objTypeManager = nullptr;
    objects.destroy();
    objects.objTypeManager = world.ownedTypes;
    owner.resetCombatClock();
    owner.restoreCombatClock(*readCombatClock(resave));
    REQUIRE(objects.Load(&resave, 4) == 15);
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
