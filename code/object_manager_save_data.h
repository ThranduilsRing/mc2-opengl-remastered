// Object-manager values; native layout is retained only for legacy saves.
#pragma once
#include "save_sections.h"

struct _ObjectManagerData {
    std::int32_t maxObjects;
    std::int32_t numElementals;
    std::int32_t numTerrainObjects;
    std::int32_t numBuildings;
    std::int32_t numTurrets;
    std::int32_t numWeapons;
    std::int32_t numCarnage;
    std::int32_t numLights;
    std::int32_t numArtillery;
    std::int32_t numGates;
    std::int32_t maxMechs;
    std::int32_t maxVehicles;
    std::int32_t numMechs;
    std::int32_t numVehicles;
    std::int32_t nextWatchId;

    void serialize(mc2::save::Archive& a) {
        a(maxObjects, numElementals, numTerrainObjects, numBuildings, numTurrets,
          numWeapons, numCarnage, numLights, numArtillery, numGates, maxMechs,
          maxVehicles, numMechs, numVehicles, nextWatchId);
    }
};
using ObjectManagerData = _ObjectManagerData;

namespace mc2::save {
inline constexpr auto objectManagerTag = fourcc('O', 'B', 'M', 'H');
}
