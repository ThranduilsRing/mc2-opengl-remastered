// Packet F preparation; native WeaponBoltData writes remain active until flip.
#pragma once
#include "weaponbolt.h"
#include "object_value_codec.h"

namespace mc2::save {
inline constexpr auto weaponBoltTag = fourcc('B', 'O', 'L', 'T');
struct WeaponBoltRecord {
    // Decode in place: WeaponShotInfo assignment calls engine init().
    WeaponBoltData& value;
    void serialize(Archive& a) {
        auto& v = value;
        gameObjectFields(a, v);
        a(v.ownerWID, v.hotSpotNumber, v.targetWID, v.targetHotSpot,
          v.targetPosition.x, v.targetPosition.y, v.targetPosition.z,
          v.distanceToTarget, v.halfDistanceToTarget);
        auto& shot = v.weaponShot;
        if (!a.reading() && (!std::in_range<std::int32_t>(shot.masterId) ||
                            !std::in_range<std::int32_t>(shot.hitLocation)))
            a.reject("weapon_shot_int32");
        auto master = static_cast<std::int32_t>(shot.masterId);
        auto location = static_cast<std::int32_t>(shot.hitLocation);
        a(shot.attackerWID, master, shot.damage, location, shot.entryAngle);
        if (a.reading() && a.good()) { shot.masterId = master; shot.hitLocation = location; }
        a(v.laserPosition.x, v.laserPosition.y, v.laserPosition.z);
        for (auto& p : v.laserVertices) a(p.x, p.y, p.z);
        for (auto& p : v.laserSide) a(p.x, p.y, p.z);
        a(v.effectId, v.hitTarget, v.timeLeft, v.hsPos.x, v.hsPos.y, v.hsPos.z,
          v.hitLeft, v.startUV, v.goalHeight, v.retireDeadlineTick);
        auto state = std::to_underlying(v.retireDeadlineState);
        a(state);
        if (!a.good()) return;
        v.retireDeadlineState = static_cast<mc2::combat::SavedDeadlineState>(state);
        if (!mc2::combat::validSavedDeadline(v.retireDeadlineTick, v.retireDeadlineState))
            a.reject("bolt_deadline");
        if (a.reading()) v.mcTextureHandle = v.gosTextureHandle = 0;
    }
};
inline std::optional<WeaponBoltData> decodeWeaponBoltRecord(std::span<const std::uint8_t> bytes) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0;
    WeaponBoltData value{};
    WeaponBoltRecord record{value};
    a(tag, version, record);
    if (!a.finish() || tag != weaponBoltTag || version != 1) return std::nullopt;
    return value;
}
// Derived: mcTextureHandle/gosTextureHandle -> 0. Load ignores legacy values;
// init resolves textures from the bolt type for this process. The other record
// fields, including the combat retirement deadline, are all explicit values.
// Runtime effect/light owners rebuild from effectId in presentation. Existing
// aimedTargetWID remains transient: init clears it and loaded bolts use the
// legacy splash rule (see WeaponBolt::Load/deliverAimedAreaHit).
}
