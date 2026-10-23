// WORLD_ORDER v1: literal slot order, never pointer values or sorted identities.
#include "objmgr_internal.h"
#include "sim_world_trace.h"
#include "mc2fmt.h"
#include <format>
#include <span>

namespace mc2::world_trace {
namespace {
bool recording = false;
long tick = 0;
size_t ordinal = 0;
std::string transcript;

void record(std::string_view body) {
    transcript += std::format("[WORLD_ORDER v1] tick={} seq={} {}\n", tick, ordinal++, body);
}

std::string identity(GameObject* object) {
    if (!object) return "-";
    // getWatchID() normally assigns on first read; false is essential here.
    return std::format("{}:{}:{}", object->getHandle(), object->getWatchID(false), object->getExists() ? 1 : 0);
}

void recordEvent(std::string_view action, GameObject* object, long detail) {
    if (recording) record(std::format("event={} id={} detail={}", action, identity(object), detail));
}

template<class T, size_t Extent>
void view(std::string_view name, std::span<T, Extent> objects, long cursor = -1) {
    const auto count = objects.size();
    std::string body = std::format("view={} n={} cursor={} slots=", name, count, cursor);
    for (size_t i = 0; i < count; ++i) {
        if (i) body += ',';
        body += identity(objects[i]);
    }
    record(body);
}
}

void begin(long simTick) {
    tick = simTick;
    ordinal = 0;
    transcript.clear();
    recording = true;
    detail::eventSink = recordEvent;
    record("begin");
}

void end() {
    if (!recording) return;
    record("end");
    // One buffered write per tick avoids interleaved diagnostic fragments.
    mc2::fmt::print_out("{}", transcript);
    recording = false;
    detail::eventSink = nullptr;
}

void snapshot(GameObjectManager& w, std::string_view phase) {
    if (!recording) return;
    record(std::format("snapshot={} collidables_dirty={}", phase, w.rebuildCollidableList ? 1 : 0));
    view("objects", std::span(w.objList, w.objList ? static_cast<size_t>(w.getMaxObjects() + 1) : 0));
    view("watch", std::span(w.watchList, w.watchList ? static_cast<size_t>(w.getMaxObjects() + 1) : 0));
    view("movers", std::span(w.moverList).first(w.numMovers));
    view("good", std::span(w.goodMoverList).first(w.numGoodMovers));
    view("bad", std::span(w.badMoverList).first(w.numBadMovers));
    // Include allocated tails: removed movers are compacted into reserve slots.
    view("mechs", std::span(w.mechs, w.mechs ? static_cast<size_t>(w.maxMechs) : 0));
    view("vehicles", std::span(w.vehicles, w.vehicles ? static_cast<size_t>(w.maxVehicles) : 0));
#ifdef USE_ELEMENTALS
    view("elementals", std::span(w.elementals, w.elementals ? static_cast<size_t>(w.numElementals) : 0));
#else
    record(std::format("view=elementals n={} cursor=-1 slots=", w.numElementals));
#endif
    view("collidables", std::span(w.collidableList, w.collidableList ? static_cast<size_t>(w.numCollidables) : 0));
    view("terrain", std::span(w.terrainObjects, w.terrainObjects ? static_cast<size_t>(w.numTerrainObjects) : 0));
    view("buildings", std::span(w.buildings, w.buildings ? static_cast<size_t>(w.numBuildings) : 0));
    view("special", std::span(w.specialBuildings).first(w.numSpecialBuildings));
    view("gate_controls", std::span(w.gateControls).first(w.numGateControls));
    view("turret_controls", std::span(w.turretControls).first(w.numTurretControls));
    view("power", std::span(w.powerGenerators).first(w.numPowerGenerators));
    view("gates", std::span(w.gates, w.gates ? static_cast<size_t>(w.numGates) : 0));
    view("turrets", std::span(w.turrets, w.turrets ? static_cast<size_t>(w.numTurrets) : 0));
    view("weapons", std::span(w.weapons, w.weapons ? static_cast<size_t>(w.numWeapons) : 0), w.currentWeaponsIndex);
    view("carnage", std::span(w.carnage, w.carnage ? static_cast<size_t>(w.numCarnage) : 0), w.currentCarnageIndex);
    view("lights", std::span(w.lights, w.lights ? static_cast<size_t>(w.numLights) : 0), w.currentLightIndex);
    view("artillery", std::span(w.artillery, w.artillery ? static_cast<size_t>(w.numArtillery) : 0), w.currentArtilleryIndex);
    record(std::format("populations mechs={} vehicles={} collidables={}", w.numMechs, w.numVehicles, w.numCollidables));
    for (long b = 0; b < Terrain::numObjBlocks; ++b) {
        const auto& block = Terrain::objBlockInfo[b];
        std::string body = std::format("block={} active={} first={} n={} slots=", b,
            block.active ? 1 : 0, block.firstHandle, block.numObjects);
        for (long i = 0; i < block.numObjects; ++i) {
            auto* object = w.objList[block.firstHandle + i];
            if (i) body += ',';
            body += identity(object);
            body += std::format(":{}", object && Terrain::getObjVertexActive(object->getVertexNum()) ? 1 : 0);
        }
        record(body);
    }
}

void update(std::string_view name, GameObject* object) {
    if (recording) record(std::format("update={} id={}", name, identity(object)));
}

}
