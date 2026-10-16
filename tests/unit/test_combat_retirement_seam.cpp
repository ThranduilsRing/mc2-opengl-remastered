#include "doctest.h"
#include "mission.h"
#include "weaponbolt.h"
#include "heap.h"
#include "team.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <process.h>

extern "C" void mc2FreezeMoversSet(int, long);
extern bool renderObjects;
extern char TeamRelations[MAX_TEAMS][MAX_TEAMS];

struct CombatRetirementSeam {
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
    UserHeap heap;
    UserHeap* oldHeap = systemHeap;
    UserHeap* oldMissionHeap = missionHeap;
    UserHeap* oldTypeCache = ObjectTypeManager::objectTypeCache;
    UserHeap* oldObjectCache = ObjectTypeManager::objectCache;
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
    World() {
        std::memcpy(oldRelations, Team::relations, sizeof(oldRelations));
        std::memcpy(oldNetworkRelations, TeamRelations, sizeof(oldNetworkRelations));
        heap.init(1024 * 1024, "combat-retirement-seam");
        systemHeap = missionHeap = &heap;
        ObjectTypeManager::objectTypeCache = ObjectTypeManager::objectCache = &heap;
        objects = std::make_unique<Watches>();
        GameObject::initialize = true;
        mission = &storage.owner;
        ObjectManager = objects.get();
        MPlayer = nullptr;
        renderObjects = false;
        mc2FreezeMoversSet(0, -1);
    }
    ~World() {
        objects.reset();
        std::memcpy(Team::relations, oldRelations, sizeof(oldRelations));
        std::memcpy(TeamRelations, oldNetworkRelations, sizeof(oldNetworkRelations));
        turn = oldTurn;
        mc2FreezeMoversSet(0, -1);
        frameLength = oldFrame; renderObjects = oldRender; MPlayer = oldMultiplayer;
        ObjectManager = oldObjects; mission = oldMission;
        GameObject::initialize = oldInitialize;
        systemHeap = oldHeap; missionHeap = oldMissionHeap;
        ObjectTypeManager::objectTypeCache = oldTypeCache;
        ObjectTypeManager::objectCache = oldObjectCache;
    }
};
struct Bolt : WeaponBolt {
    WeaponBoltType* type;
    explicit Bolt(WeaponBoltType& value) : type(&value) {}
    ObjectTypePtr getObjectType() override { return type; }
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
