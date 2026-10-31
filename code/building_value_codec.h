// Packet F preparation; native BuildingData writes remain active until flip.
#pragma once
#include "bldng.h"
#include "terrain_value_codec.h"

namespace mc2::save {
inline constexpr auto buildingTag = fourcc('B', 'L', 'D', 'G');
struct BuildingRecord {
    BuildingData value{};
    void serialize(Archive& a) {
        auto& v = value;
        terrainObjectFields(a, v);
        a(v.teamId, v.commanderId, v.refitBuddyWID, v.parentId, v.parent,
          v.listID, v.captureTime, v.moverInProximity, v.proximityTimer);
        if (a.reading()) { v.baseTileId = 0; v.updatedTurn = 0; }
    }
};
// Derived, matching Building::Load: baseTileId -> 0 (terrain/damage tile
// selection rebuilds it), updatedTurn -> 0 (next fixed update's turn stamp).
// TerrainObjectData::listID and BuildingData::listID are distinct values.
// Sensor owners/contacts and scoreTime remain runtime state, as before.
}
