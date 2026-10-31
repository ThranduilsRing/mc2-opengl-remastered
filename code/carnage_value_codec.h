// Carnage value fields; native CarnageData remains the legacy reader ABI.
#pragma once
#include "carnage.h"
#include "object_value_codec.h"
#include <ranges>

namespace mc2::save {
inline constexpr auto carnageTag = fourcc('C', 'A', 'R', 'N');
struct CarnageRecord {
    CarnageData value{};
    void serialize(Archive& a) {
        auto& v = value;
        gameObjectFields(a, v);
        // Freeze the union discriminant independently of enum representation.
        constexpr std::array kinds{CARNAGE_NONE, CARNAGE_FIRE, CARNAGE_EXPLOSION};
        std::uint32_t kind = 0;
        if (!a.reading()) {
            const auto found = std::ranges::find(kinds, v.carnageType);
            if (found == kinds.end()) { a.reject("carnage_type"); return; }
            kind = static_cast<std::uint32_t>(found - kinds.begin());
        }
        a(kind, v.ownerWID);
        if (!a.good()) return;
        if (kind >= kinds.size()) { a.reject("carnage_type"); return; }
        v.carnageType = kinds[kind];
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
