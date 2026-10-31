// Packet F preparation: Save still writes sizeof(CarnageData) until the
// coordinated ObjectSlot dispatch/preflight/required-section flip.
#pragma once
#include "carnage.h"
#include "object_value_codec.h"

namespace mc2::save {
inline constexpr auto carnageTag = fourcc('C', 'A', 'R', 'N');
struct CarnageRecord {
    CarnageData value{};
    void serialize(Archive& a) {
        auto& v = value;
        gameObjectFields(a, v);
        // Freeze the union discriminant independently of enum representation.
        std::uint32_t kind = 0;
        if (!a.reading()) {
            switch (v.carnageType) {
            case CARNAGE_NONE: kind = 0; break;
            case CARNAGE_FIRE: kind = 1; break;
            case CARNAGE_EXPLOSION: kind = 2; break;
            default: a.reject("carnage_type"); return;
            }
        }
        a(kind, v.ownerWID);
        if (!a.good()) return;
        switch (kind) {
        case 0: v.carnageType = CARNAGE_NONE; break;
        case 1: v.carnageType = CARNAGE_FIRE; break;
        case 2: v.carnageType = CARNAGE_EXPLOSION; break;
        default: a.reject("carnage_type"); return;
        }
        if (v.carnageType == CARNAGE_FIRE)
            a(v.info.fire.timeToBurn, v.info.fire.radius, v.info.fire.reallyVisible);
        else
            // init() initializes the explosion view even for inactive NONE slots.
            a(v.info.explosion.timer, v.info.explosion.radius, v.info.explosion.chunkSize);
        a(v.effectId);
    }
};
// No CarnageData class fields are derived. Runtime presentation owners
// (gosEffect, pointLight/lightId, intensity/radii/duration) remain outside
// the record, as in legacy saves: init() clears them; presentation rebuilds
// effects from effectId and the restored fire/explosion state.
}
