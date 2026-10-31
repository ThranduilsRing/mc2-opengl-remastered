// Packet F preparation; native ArtilleryData writes remain active until flip.
#pragma once
#include "artlry.h"
#include "object_value_codec.h"

namespace mc2::save {
inline constexpr auto artilleryTag = fourcc('A', 'R', 'T', 'Y');
struct ArtilleryRecord {
    ArtilleryData value{};
    void serialize(Archive& a) {
        auto& v = value;
        gameObjectFields(a, v);
        a(v.artilleryType, v.teamId, v.commanderId);
        if (v.artilleryType < 0 || v.artilleryType >= NUM_ARTILLERY_TYPES)
            a.reject("artillery_type");
        auto& s = v.info.strike;
        a(s.timeToImpact, s.timeToLaunch, s.sensorRange, s.timeToBlind,
          s.sensorSystemIndex, s.contactUpdate);
        for (auto& character : s.timeString) a(character);
        a(s.explosionOffFlags, v.effectId, v.bombRunStarted, v.inView,
          v.iFacePosition.x, v.iFacePosition.y, v.iFacePosition.z);
    }
};
// No ArtilleryData class fields are derived. In particular the sensor pool
// index and contact-update clock survive; sensor contacts themselves rebuild
// in loadActivateAndFinalize. Runtime bomber/appearance/effect owners and
// presentationInitialized/presentationImpactStarted remain outside this
// record: init() clears them and presentationUpdate reconstructs the effects.
}
