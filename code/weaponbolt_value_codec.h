// Bolt value fields; native WeaponBoltData remains the legacy reader ABI.
#pragma once
#include "weaponbolt.h"
#include "object_value_codec.h"

namespace mc2::save {
inline constexpr auto weaponBoltTag = fourcc('B', 'O', 'L', 'T');
struct WeaponBoltRecord {
    // Decode in place: WeaponShotInfo assignment calls engine init().
    WeaponBoltData& value;
    GameObjectWatchID aimedTargetWID = 0;
    std::uint32_t version = 1;
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
        if (version >= 2) a(aimedTargetWID);
        if (!a.good()) return;
        v.retireDeadlineState = static_cast<mc2::combat::SavedDeadlineState>(state);
        if (!mc2::combat::validSavedDeadline(v.retireDeadlineTick, v.retireDeadlineState))
            a.reject("bolt_deadline");
        if (a.reading()) v.mcTextureHandle = v.gosTextureHandle = 0;
    }
};
inline Bytes encodeTaggedRecord(WeaponBoltRecord record, std::uint32_t tag) {
    Archive a;
    record.version = record.aimedTargetWID ? 2 : 1;
    a(tag, record.version, record);
    return a.good() ? a.bytes() : Bytes{};
}
inline bool readWeaponBoltRecord(std::span<const std::uint8_t> bytes, WeaponBoltRecord& record) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0;
    a(tag, version);
    if (!a.good() || tag != weaponBoltTag || (version != 1 && version != 2)) return false;
    record.version = version;
    record.aimedTargetWID = 0;
    a(record);
    return a.finish();
}
inline std::optional<WeaponBoltData> decodeWeaponBoltRecord(std::span<const std::uint8_t> bytes) {
    WeaponBoltData value{};
    WeaponBoltRecord record{value};
    if (!readWeaponBoltRecord(bytes, record)) return std::nullopt;
    return value;
}
// Derived: mcTextureHandle/gosTextureHandle -> 0. Load ignores legacy values;
// init resolves textures from the bolt type for this process. The other record
// fields, including the combat retirement deadline, are all explicit values.
// Runtime effect/light owners rebuild from effectId in presentation. Existing
// BOLT v2 adds aimedTargetWID; v1 and native legacy records default to stock splash.
}
