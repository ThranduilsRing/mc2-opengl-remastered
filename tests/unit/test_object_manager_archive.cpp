#include "save_field_probe.h"
#include "object_manager_save_data.h"

TEST_CASE("save fields: ObjectManagerData independent census") {
    using T = ObjectManagerData;
    auto sample = save_probe::patterned<T>(
        &T::maxObjects, &T::numElementals, &T::numTerrainObjects, &T::numBuildings, &T::numTurrets, &T::numWeapons, &T::numCarnage, &T::numLights, &T::numArtillery, &T::numGates, &T::maxMechs, &T::maxVehicles, &T::numMechs, &T::numVehicles, &T::nextWatchId);
    SAVE_FIELD_SHAPE(sample, maxObjects, numElementals, numTerrainObjects, numBuildings, numTurrets, numWeapons, numCarnage, numLights, numArtillery, numGates, maxMechs, maxVehicles, numMechs, numVehicles, nextWatchId);
    CHECK(save_probe::memberBytes<T>(
        &T::maxObjects, &T::numElementals, &T::numTerrainObjects, &T::numBuildings, &T::numTurrets, &T::numWeapons, &T::numCarnage, &T::numLights, &T::numArtillery, &T::numGates, &T::maxMechs, &T::maxVehicles, &T::numMechs, &T::numVehicles, &T::nextWatchId) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); },
        &T::maxObjects, &T::numElementals, &T::numTerrainObjects, &T::numBuildings, &T::numTurrets, &T::numWeapons, &T::numCarnage, &T::numLights, &T::numArtillery, &T::numGates, &T::maxMechs, &T::maxVehicles, &T::numMechs, &T::numVehicles, &T::nextWatchId));
    auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::objectManagerTag);
    REQUIRE(bytes.size() == 68);
    CHECK(mc2::save::Bytes(bytes.begin(), bytes.begin() + 12) ==
        mc2::save::Bytes{'O','B','M','H',1,0,0,0,1,1,1,1});
    T decoded{};
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::objectManagerTag));
    CHECK(decoded.nextWatchId == sample.nextWatchId);
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagTeam));
    bytes.pop_back();
    decoded.maxObjects = 42;
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::objectManagerTag));
    CHECK(decoded.maxObjects == 42);
}
