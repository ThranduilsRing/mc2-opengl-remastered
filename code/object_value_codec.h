// Object value fields; native GameObjectData remains the legacy reader ABI.
#pragma once
#include "gameobj.h"
#include "save_sections.h"

namespace mc2::save {
inline void gameObjectFields(Archive& a, GameObjectData& v) {
    std::int32_t objectClass = static_cast<std::int32_t>(v.objectClass);
    a(v.objectTypeNum, objectClass);
    if (objectClass < INVALID || objectClass > WEAPONBOLT) a.reject("object_class");
    if (a.reading() && a.good()) v.objectClass = static_cast<ObjectClass>(objectClass);
    a(v.handle, v.partId, v.watchID,
      v.typeHandle, v.position.x, v.position.y, v.position.z,
      v.cellPositionRow, v.cellPositionCol, v.d_vertexNum, v.flags,
      v.debugFlags, v.status, v.tonnage, v.rotation);
    for (auto& character : v.appearanceTypeID) a(character);
    a(v.collisionFreeFromWID, v.collisionFreeTime,
      v.screenPos.x, v.screenPos.y, v.screenPos.z, v.screenPos.w,
      v.explRadius, v.explDamage, v.maxCV, v.curCV, v.threatRating,
      v.lastFrameTime, v.blipFrame, v.numAttackers, v.drawFlags);
    // Derived: windowsVisible -> 0, matching GameObject::CopyTo/Load;
    // the next camera bounds/render pass rebuilds the visibility stamp.
    if (a.reading()) v.windowsVisible = 0;
}
}
