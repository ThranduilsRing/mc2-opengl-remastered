// tests/unit/test_hardpoint.cpp
// HARDPOINT-ID-SCHEMA-1 (Phase 2, slice 1 of the UNIT-PROFILE arc).
//
// Pure-logic doctest coverage for hardpoint.h: the logical-hardpoint JOIN KEY.
// PURE SCHEMA -- no engine behavior. Covers: HardpointId value equality + name
// round-trip, weapon-tag acceptance, and the damage-location link resolving
// against schemaHasLocation() (reused from damage_schema.h). Header-only, drags
// no engine/GL/IO. Mirrors test_unit_profile_validator.cpp shape.
#include "doctest.h"
#include "hardpoint.h"
#include <string>

// The logical names the schema must be able to represent (roadmap examples).
static const char* kRightTorsoEnergy1   = "RightTorso_Energy_1";
static const char* kLeftArmBallistic1   = "LeftArm_Ballistic_1";
static const char* kCenterTorsoMissile2 = "CenterTorso_Missile_2";
static const char* kInfantryPrimary     = "Infantry_PrimaryWeapon";
static const char* kVehicleTurretHeavy1 = "Vehicle_Turret_Heavy_1";

// -------------------------------------------------------------------------
// HardpointId: value equality + debug-name round-trip.
// -------------------------------------------------------------------------

TEST_CASE("hardpoint id: same name => equal, distinct names => not equal") {
    const HardpointId a = makeHardpointId(kRightTorsoEnergy1);
    const HardpointId b = makeHardpointId(kRightTorsoEnergy1);
    const HardpointId c = makeHardpointId(kLeftArmBallistic1);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(b != c);
    // All the roadmap example names are pairwise distinct.
    const HardpointId d = makeHardpointId(kCenterTorsoMissile2);
    const HardpointId e = makeHardpointId(kInfantryPrimary);
    const HardpointId f = makeHardpointId(kVehicleTurretHeavy1);
    CHECK(a != d); CHECK(a != e); CHECK(a != f);
    CHECK(c != d); CHECK(c != e); CHECK(c != f);
    CHECK(d != e); CHECK(d != f); CHECK(e != f);
}

TEST_CASE("hardpoint id: debug-name round-trips for names within the buffer") {
    const HardpointId a = makeHardpointId(kRightTorsoEnergy1);
    CHECK(std::string(a.debugName()) == kRightTorsoEnergy1);
    CHECK(a.valid());
    const HardpointId inf = makeHardpointId(kInfantryPrimary);
    CHECK(std::string(inf.debugName()) == kInfantryPrimary);
}

TEST_CASE("hardpoint id: case-sensitive (presentation sockets are case-sensitive)") {
    const HardpointId hi = makeHardpointId("RightTorso");
    const HardpointId lo = makeHardpointId("righttorso");
    CHECK(hi != lo);
}

TEST_CASE("hardpoint id: empty / default is unset and invalid") {
    HardpointId def;
    CHECK_FALSE(def.valid());
    CHECK(def.id == 0u);
    const HardpointId empty = makeHardpointId("");
    CHECK_FALSE(empty.valid());
    CHECK(def == empty);
}

TEST_CASE("hardpoint id: long names still hash in full even when name buffer truncates") {
    // Two names that share the first (CAP-1) chars but differ AFTER the buffer
    // limit must still be distinct ids -- the hash covers the untruncated name.
    std::string base(HARDPOINT_NAME_CAP + 8, 'X');
    std::string n1 = base + "_alpha";
    std::string n2 = base + "_omega";
    const HardpointId a = makeHardpointId(n1.c_str());
    const HardpointId b = makeHardpointId(n2.c_str());
    CHECK(a != b);
    // Stored debug name is truncated to the buffer cap (NUL-terminated).
    CHECK(std::string(a.debugName()).size() == (size_t)(HARDPOINT_NAME_CAP - 1));
}

// -------------------------------------------------------------------------
// WeaponTag acceptance.
// -------------------------------------------------------------------------

TEST_CASE("hardpoint accepts only its declared weapon tags") {
    HardpointProfile hp;
    hp.id = makeHardpointId(kRightTorsoEnergy1);
    addWeaponTag(hp, WTAG_ENERGY);
    CHECK(hardpointAcceptsWeapon(hp, WTAG_ENERGY));
    CHECK_FALSE(hardpointAcceptsWeapon(hp, WTAG_BALLISTIC));
    CHECK_FALSE(hardpointAcceptsWeapon(hp, WTAG_MISSILE));
    CHECK_FALSE(hardpointAcceptsWeapon(hp, WTAG_AMS));
}

TEST_CASE("hardpoint accepts multiple tags via the mask") {
    // A turret that mounts ballistic OR AMS point-defense.
    HardpointProfile hp;
    hp.id = makeHardpointId(kVehicleTurretHeavy1);
    addWeaponTag(hp, WTAG_BALLISTIC);
    addWeaponTag(hp, WTAG_AMS);
    CHECK(hardpointAcceptsWeapon(hp, WTAG_BALLISTIC));
    CHECK(hardpointAcceptsWeapon(hp, WTAG_AMS));
    CHECK_FALSE(hardpointAcceptsWeapon(hp, WTAG_ENERGY));
    CHECK_FALSE(hardpointAcceptsWeapon(hp, WTAG_MISSILE));
}

TEST_CASE("hardpoint with no tags accepts nothing") {
    HardpointProfile hp;
    for (int t = 0; t < NUM_WEAPON_TAGS; ++t) {
        CHECK_FALSE(hardpointAcceptsWeapon(hp, (WeaponTag)t));
    }
}

TEST_CASE("weapon tag bits are distinct and fit in the mask") {
    CHECK(NUM_WEAPON_TAGS <= 32);
    WeaponTagMask seen = 0u;
    for (int t = 0; t < NUM_WEAPON_TAGS; ++t) {
        WeaponTagMask bit = weaponTagBit((WeaponTag)t);
        CHECK(bit != 0u);
        CHECK((seen & bit) == 0u);   // no two tags share a bit
        seen |= bit;
    }
}

// -------------------------------------------------------------------------
// Damage-location link: resolves against schemaHasLocation (damage_schema.h).
// -------------------------------------------------------------------------

TEST_CASE("hardpoint damage-location resolves under the matching schema only") {
    // A mech hardpoint on the right arm.
    HardpointProfile mechHp;
    mechHp.id = makeHardpointId(kLeftArmBallistic1);   // name is descriptive only
    mechHp.damageLocation = DLOC_MECH_RARM;
    addWeaponTag(mechHp, WTAG_BALLISTIC);

    // Valid under the 8-part mech schema...
    CHECK(hardpointResolvesInSchema(mechHp, DMG_MECH_8PART));
    // ...invalid under vehicle facings (no arms) -- the modder-readable reject.
    CHECK_FALSE(hardpointResolvesInSchema(mechHp, DMG_VEHICLE_FACINGS));
    CHECK_FALSE(hardpointResolvesInSchema(mechHp, DMG_SQUAD));
    CHECK_FALSE(hardpointResolvesInSchema(mechHp, DMG_SINGLE_ACTOR));
}

TEST_CASE("vehicle-turret hardpoint resolves under vehicle facings only") {
    HardpointProfile vehHp;
    vehHp.id = makeHardpointId(kVehicleTurretHeavy1);
    vehHp.damageLocation = DLOC_VEH_TURRET;
    addWeaponTag(vehHp, WTAG_BALLISTIC);

    CHECK(hardpointResolvesInSchema(vehHp, DMG_VEHICLE_FACINGS));
    CHECK_FALSE(hardpointResolvesInSchema(vehHp, DMG_MECH_8PART));
}

TEST_CASE("infantry primary-weapon hardpoint resolves under squad/single-actor body") {
    HardpointProfile infHp;
    infHp.id = makeHardpointId(kInfantryPrimary);
    infHp.damageLocation = DLOC_BODY;
    addWeaponTag(infHp, WTAG_ANTIPERSONNEL);

    CHECK(hardpointResolvesInSchema(infHp, DMG_SQUAD));
    CHECK(hardpointResolvesInSchema(infHp, DMG_SINGLE_ACTOR));
    CHECK_FALSE(hardpointResolvesInSchema(infHp, DMG_MECH_8PART));
    CHECK_FALSE(hardpointResolvesInSchema(infHp, DMG_VEHICLE_FACINGS));
    CHECK(hardpointAcceptsWeapon(infHp, WTAG_ANTIPERSONNEL));
}

TEST_CASE("hardpoint profile carries socket / muzzle / ui-slot string ids") {
    HardpointProfile hp;
    hp.id = makeHardpointId(kRightTorsoEnergy1);
    hp.socket = HardpointSocketName("node_rt_energy");
    hp.muzzle = HardpointSocketName("muzzle_rt_01");
    hp.uiSlot = HardpointSocketName("mechlab_slot_rt_1");
    CHECK(std::string(hp.socket.c_str()) == "node_rt_energy");
    CHECK(std::string(hp.muzzle.c_str()) == "muzzle_rt_01");
    CHECK(std::string(hp.uiSlot.c_str()) == "mechlab_slot_rt_1");
    // Default-constructed string fields are empty (== "not mapped yet").
    HardpointProfile bare;
    CHECK(bare.socket.empty());
    CHECK(bare.muzzle.empty());
    CHECK(bare.uiSlot.empty());
}
