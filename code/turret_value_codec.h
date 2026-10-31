// Packet F preparation; native TurretData writes remain active until flip.
#pragma once
#include "turret.h"
#include "terrain_value_codec.h"

namespace mc2::save {
inline constexpr auto turretTag = fourcc('T', 'U', 'R', 'R');
struct TurretRecord {
    TurretData value{};
    void serialize(Archive& a) {
        auto& v = value;
        terrainObjectFields(a, v);
        a(v.teamId, v.turretRotation, v.targetWID);
        for (auto& time : v.readyTime) a(time);
        for (auto& time : v.lastFireTime) a(time);
        a(v.minRange, v.maxRange, v.numFunctionalWeapons, v.idleWait,
          v.idlePosition.x, v.idlePosition.y, v.idlePosition.z,
          v.oldPosition.x, v.oldPosition.y, v.oldPosition.z,
          v.parentId, v.parent, v.currentWeaponNode);
        if (a.reading()) v.didReveal = false;
    }
};
// Derived: didReveal -> false, matching Load, so LOS matrices rebuild.
// Runtime light/effect owners and multiplayer fire queues stay transient.
}
