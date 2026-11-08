#include "doctest.h"
#include "../../mclib/sim_overlap.h"
#include <array>
#include <cstddef>

// Verbatim production methods, with fake slot/sensor services. No renderer,
// map, ABL or asset I/O. The legacy campaign has neither developer gate enabled.
namespace {
namespace mc2 {
namespace fmt { template<class... Args> void print_err(const char*, Args...) {} }
namespace brainpolicy { bool techBrain() { return false; } }
namespace gate {
template<std::size_t N> struct Key {
    char value[N];
    constexpr Key(const char (&s)[N]) { for (std::size_t i=0; i<N; ++i) value[i]=s[i]; }
};
template<Key> bool get() { return false; }
}
}
int rejectedSensors = 0;
#define MC2_LOG_ONCE(...) (++rejectedSensors)
struct SensorSystem {
    long index = -1;
    int clears = 0;
    long getMasterIndex() const { return index; }
    void setMasterIndex(long i) { index=i; }
    void clearContacts() { ++clears; }
};
using SensorSystemPtr = SensorSystem*;
struct TeamSensorSystem {
    long teamId = 0;
    // Padding makes the old index -1 write observable without test-side UB.
    std::array<SensorSystemPtr, 4> storage{};
    SensorSystemPtr* sensors = storage.data()+1;
    long numSensors = 0;
    void removeSensor(SensorSystemPtr sensor);
};
#include "repair_sensor_remove.inc"
#undef MC2_LOG_ONCE

struct GroundVehicle {
    long team = -1;
    SensorSystemPtr sensor = nullptr;
    TeamSensorSystem* master = nullptr;
    unsigned long watchID = 99;
    int detached = 0;
    bool exists = false;
    bool initialized = false;
    long getTeamId() const { return team; }
    long getHandle() const { return 7; }
    SensorSystemPtr getSensorSystem() const { return sensor; }
    void setTeamId(long id, bool) {
        if (team >= 0 && sensor) master->removeSensor(sensor);
        team=id;
        ++detached;
    }
    void init(bool) {
        // GroundVehicle::init drops the old sensor but retains the base team.
        CHECK(team == -1);
        sensor=nullptr;
        initialized=true;
    }
    void setExists(bool value) { exists=value; }
};
using GroundVehiclePtr = GroundVehicle*;
struct GameObjectManager {
    GroundVehiclePtr vehicles[1]{};
    long numVehicles = 0;
    long maxVehicles = 1;
    bool rebuildCollidableList = false;
    void setPartId(GroundVehiclePtr, long, long, long) {}
    GroundVehiclePtr newVehicle();
};
#include "repair_vehicle_acquire.inc"
}

TEST_CASE("repair deployment detaches legacy campaign slot before replacing its sensor") {
    SensorSystem oldSensor, otherSensor;
    TeamSensorSystem team;
    oldSensor.index=0;
    otherSensor.index=1;
    team.sensors[0]=&oldSensor;
    team.sensors[1]=&otherSensor;
    team.numSensors=2;
    GroundVehicle slot;
    slot.team=0;
    slot.sensor=&oldSensor;
    slot.master=&team;
    GameObjectManager manager;
    manager.vehicles[0]=&slot;
    // Use the real overlap worker: AI may walk team contacts later in this tick.
    ::mc2::overlap::runOnWorker([&] {
        CHECK(manager.newVehicle() == &slot);
        CHECK(slot.detached == 1);
        CHECK(oldSensor.clears == 1);
        CHECK(oldSensor.index == -1);
        CHECK(team.numSensors == 1);
        CHECK(team.sensors[0] == &otherSensor);
        CHECK(otherSensor.index == 0);
    });
    ::mc2::overlap::waitWorker();
    CHECK(slot.initialized);
    CHECK(slot.exists);
    CHECK(slot.watchID == 0);
    CHECK(manager.rebuildCollidableList);
    CHECK(manager.newVehicle() == nullptr);
}

TEST_CASE("repair deployment rejects a replacement sensor without touching team membership") {
    SensorSystem registered, fresh, sentinel;
    TeamSensorSystem team;
    team.storage[0]=&sentinel;
    team.sensors[0]=&registered;
    team.numSensors=1;
    registered.index=0;
    rejectedSensors=0;
    team.removeSensor(&fresh); // index -1: previously wrote before sensors[]
    CHECK(team.storage[0] == &sentinel);
    CHECK(team.numSensors == 1);
    CHECK(team.sensors[0] == &registered);
    CHECK(fresh.clears == 0);
    fresh.index=0; // stale index now belongs to another sensor
    team.removeSensor(&fresh);
    CHECK(team.numSensors == 1);
    CHECK(registered.index == 0);
    CHECK(registered.clears == 0);
    CHECK(rejectedSensors == 2);
    team.removeSensor(&registered);
    CHECK(team.numSensors == 0);
    CHECK(team.sensors[0] == nullptr);
    CHECK(registered.index == -1);
    CHECK(registered.clears == 1);
}
