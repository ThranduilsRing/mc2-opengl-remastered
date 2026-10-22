// WORLD_ORDER v1: literal slot order, never pointer values or sorted identities.
#include "objmgr_internal.h"
#include "sim_world_trace.h"
#include "mc2fmt.h"
#include <format>

namespace mc2::world_trace {
namespace {
bool recording = false;
long tick = 0;
size_t ordinal = 0;
std::string transcript;

void record(const std::string& body) {
    transcript += std::format("[WORLD_ORDER v1] tick={} seq={} {}\n", tick, ordinal++, body);
}

std::string identity(GameObject* object) {
    if (!object) return "-";
    // getWatchID() normally assigns on first read; false is essential here.
    return std::format("{}:{}:{}", object->getHandle(), object->getWatchID(false), object->getExists() ? 1 : 0);
}

template<class T>
void view(const char* name, T* const* objects, long count, long cursor = -1) {
    std::string body = std::format("view={} n={} cursor={} slots=", name, count, cursor);
    for (long i = 0; i < count; ++i) {
        if (i) body += ',';
        body += identity(objects ? objects[i] : nullptr);
    }
    record(body);
}
}

void begin(long simTick) {
    tick = simTick;
    ordinal = 0;
    transcript.clear();
    recording = true;
    record("begin");
}

void end() {
    if (!recording) return;
    record("end");
    // One buffered write per tick avoids interleaved diagnostic fragments.
    mc2::fmt::print_out("{}", transcript);
    recording = false;
}

void snapshot(GameObjectManager& w, const char* phase) {
    if (!recording) return;
    record(std::format("snapshot={} collidables_dirty={}", phase, w.rebuildCollidableList ? 1 : 0));
    view("objects", w.objList, w.getMaxObjects() + 1);
    view("watch", w.watchList, w.getMaxObjects() + 1);
    view("movers", w.moverList, w.numMovers);
    view("good", w.goodMoverList, w.numGoodMovers);
    view("bad", w.badMoverList, w.numBadMovers);
    // Include allocated tails: removed movers are compacted into reserve slots.
    view("mechs", w.mechs, w.maxMechs);
    view("vehicles", w.vehicles, w.maxVehicles);
#ifdef USE_ELEMENTALS
    view("elementals", w.elementals, w.numElementals);
#else
    record(std::format("view=elementals n={} cursor=-1 slots=", w.numElementals));
#endif
    view("collidables", w.collidableList, w.numCollidables);
    view("terrain", w.terrainObjects, w.numTerrainObjects);
    view("buildings", w.buildings, w.numBuildings);
    view("special", w.specialBuildings, w.numSpecialBuildings);
    view("gate_controls", w.gateControls, w.numGateControls);
    view("turret_controls", w.turretControls, w.numTurretControls);
    view("power", w.powerGenerators, w.numPowerGenerators);
    view("gates", w.gates, w.numGates);
    view("turrets", w.turrets, w.numTurrets);
    view("weapons", w.weapons, w.numWeapons, w.currentWeaponsIndex);
    view("carnage", w.carnage, w.numCarnage, w.currentCarnageIndex);
    view("lights", w.lights, w.numLights, w.currentLightIndex);
    view("artillery", w.artillery, w.numArtillery, w.currentArtilleryIndex);
    record(std::format("populations mechs={} vehicles={}", w.numMechs, w.numVehicles));
    for (long b = 0; b < Terrain::numObjBlocks; ++b) {
        const auto& block = Terrain::objBlockInfo[b];
        std::string body = std::format("block={} active={} first={} n={} slots=", b,
            block.active ? 1 : 0, block.firstHandle, block.numObjects);
        for (long i = 0; i < block.numObjects; ++i) {
            auto* object = w.objList[block.firstHandle + i];
            if (i) body += ',';
            body += identity(object);
            body += std::format(":{}", object && Terrain::objVertexActive[object->getVertexNum()] ? 1 : 0);
        }
        record(body);
    }
}

void update(const char* name, GameObject* object) {
    if (recording) record(std::format("update={} id={}", name, identity(object)));
}

void event(const char* action, GameObject* object, long detail) {
    if (recording) record(std::format("event={} id={} detail={}", action, identity(object), detail));
}
}
