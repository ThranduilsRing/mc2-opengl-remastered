// Tagged ObjectSlot packets. OBJS opts the whole object run into this schema;
// native-size dispatch is reserved for files without that required section.
#pragma once
#include "artillery_value_codec.h"
#include "building_value_codec.h"
#include "carnage_value_codec.h"
#include "gate_value_codec.h"
#include "mech_value_codec.h"
#include "turret_value_codec.h"
#include "vehicle_value_codec.h"
#include "weaponbolt_value_codec.h"
#include "saved_objects_check.h"
#include <concepts>

namespace mc2::save {
inline constexpr std::array objectSlotTags{terrainTag, buildingTag, turretTag,
    gateTag, artilleryTag, carnageTag, mechTag, vehicleTag, weaponBoltTag};

inline std::optional<int> objectSlotClass(std::span<const std::uint8_t> bytes) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0;
    a(tag, version);
    if (!a.good() || version != 1) return std::nullopt;
    for (int c = 0; c < mc2_saved_objects::kClassCount; ++c)
        if (tag == objectSlotTags[c]) return c;
    return std::nullopt;
}

template<class Record, class Data>
bool decodeObjectSlot(std::span<const std::uint8_t> bytes, Data& data, int c) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0;
    Record record{data};
    a(tag, version, record);
    if (!a.finish() || tag != objectSlotTags[c] || version != 1) return false;
    // Bolt's wrapper references its local staging value: assigning WeaponShotInfo
    // invokes an engine method, unlike copying the other plain value records.
    if constexpr (!std::same_as<Record, WeaponBoltRecord>) data = record.value;
    return true;
}

inline bool objectSlotIdentity(const GameObjectData& v, int c, std::uint32_t slot) {
    using namespace mc2_saved_objects;
    if (slot == 0 || v.handle != slot) return false;
    // Unused pooled slots have only run GameObject's constructor. Their tag
    // still identifies the pool, while their native base class is GAMEOBJECT.
    if (v.objectTypeNum == 0 && (v.objectClass == INVALID || v.objectClass == GAMEOBJECT)) return true;
    switch (c) {
    case kTerrain: return v.objectClass == TERRAINOBJECT || v.objectClass == TREE || v.objectClass == BRIDGE;
    case kBuilding: return v.objectClass == BUILDING;
    case kTurret: return v.objectClass == TURRET;
    case kGate: return v.objectClass == GATE;
    case kArtillery: return v.objectClass == ARTILLERY;
    // Carnage::init(bool, ObjectTypePtr) calls GameObject::init, which leaves
    // GAMEOBJECT even for typed, active fire/explosion instances. The CARN tag
    // and CarnageRecord discriminant identify this pool, not objectTypeNum==0.
    case kCarnage: return v.objectClass == GAMEOBJECT || v.objectClass == FIRE || v.objectClass == EXPLOSION;
    case kMech: return v.objectClass == BATTLEMECH;
    case kVehicle: return v.objectClass == GROUNDVEHICLE;
    case kBolt: return v.objectClass == WEAPONBOLT;
    default: return false;
    }
}

inline bool validObjectSlot(std::span<const std::uint8_t> bytes, int c, std::uint32_t slot) {
    auto check = [&]<class Record, class Data>() {
        Data data{};
        return decodeObjectSlot<Record>(bytes, data, c) && objectSlotIdentity(data, c, slot);
    };
    using namespace mc2_saved_objects;
    switch (c) {
    case kTerrain: return check.operator()<TerrainRecord, TerrainObjectData>();
    case kBuilding: return check.operator()<BuildingRecord, BuildingData>();
    case kTurret: return check.operator()<TurretRecord, TurretData>();
    case kGate: return check.operator()<GateRecord, GateData>();
    case kArtillery: return check.operator()<ArtilleryRecord, ArtilleryData>();
    case kCarnage: return check.operator()<CarnageRecord, CarnageData>();
    case kMech: return check.operator()<MechRecord, MechData>();
    case kVehicle: return check.operator()<VehicleRecord, GroundVehicleData>();
    case kBolt: return check.operator()<WeaponBoltRecord, WeaponBoltData>();
    default: return false;
    }
}
}
