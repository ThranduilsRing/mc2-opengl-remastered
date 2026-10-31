// Packet F preparation; native GateData writes remain active until flip.
#pragma once
#include "gate.h"
#include "terrain_value_codec.h"

namespace mc2::save {
inline constexpr auto gateTag = fourcc('G', 'A', 'T', 'E');
struct GateRecord {
    GateData value{};
    void serialize(Archive& a) {
        auto& v = value;
        terrainObjectFields(a, v);
        a(v.teamId, v.lockedOpen, v.lockedClose, v.reasonToOpen, v.opened,
          v.opening, v.closed, v.closing, v.justDestroyed, v.lastMarkedOpen,
          v.parentId, v.parent);
        if (!a.reading() && !std::in_range<std::int32_t>(v.buildingDescriptionID))
            a.reject("gate_description");
        auto description = static_cast<std::int32_t>(v.buildingDescriptionID);
        a(description);
        if (a.reading()) {
            v.buildingDescriptionID = description;
            v.closestObject = nullptr;
            v.updatedTurn = 0;
        }
    }
};
// Derived, matching Gate::Load: closestObject -> nullptr (next collision finds
// its blocker); updatedTurn -> 0 (next fixed update rebuilds the turn stamp).
}
