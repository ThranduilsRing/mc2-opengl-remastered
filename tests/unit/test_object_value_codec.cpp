#include "save_field_probe.h"
#include "carnage_value_codec.h"
#include "artillery_value_codec.h"
#include "weaponbolt_value_codec.h"
#include "gate_value_codec.h"
#include "vehicle_value_codec.h"
#include "building_value_codec.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>

namespace {
std::string compactSource(const char* file) {
    std::ifstream in(std::filesystem::path(MC2_REPO_ROOT) / file);
    REQUIRE(in.good());
    std::stringstream stream; stream << in.rdbuf();
    auto source = std::regex_replace(stream.str(), std::regex("//[^\n]*"), "");
    std::erase_if(source, [](unsigned char c) { return std::isspace(c); });
    return source;
}
}

namespace save_probe {
template<> inline void pattern(CriticalSpace& v, unsigned char seed) {
    pattern(v.inventoryID, seed); pattern(v.hit, seed);
}
template<> inline bool equal(const CriticalSpace& a, const CriticalSpace& b) {
    return a.inventoryID == b.inventoryID && a.hit == b.hit;
}
template<> inline void pattern(BodyLocation& v, unsigned char seed) {
    pattern(v.CASE, seed); pattern(v.totalSpaces, seed); pattern(v.criticalSpaces, seed);
    pattern(v.curInternalStructure, seed); pattern(v.hotSpotNumber, seed);
    pattern(v.maxInternalStructure, seed); pattern(v.damageState, seed);
}
template<> inline bool equal(const BodyLocation& a, const BodyLocation& b) {
    return std::tie(a.CASE, a.totalSpaces, a.curInternalStructure, a.hotSpotNumber,
        a.maxInternalStructure, a.damageState) == std::tie(b.CASE, b.totalSpaces,
        b.curInternalStructure, b.hotSpotNumber, b.maxInternalStructure, b.damageState)
        && equal(a.criticalSpaces, b.criticalSpaces);
}
template<> inline void zero(BodyLocation& v) {
    v.CASE = false; v.totalSpaces = 0;
    for (auto& c : v.criticalSpaces) { c.inventoryID = 0; c.hit = false; }
    v.curInternalStructure = 0; v.hotSpotNumber = 0;
    v.maxInternalStructure = 0; v.damageState = 0;
}
template<> inline void pattern(ArmorLocation& v, unsigned char seed) {
    pattern(v.curArmor, seed); pattern(v.maxArmor, seed);
}
template<> inline bool equal(const ArmorLocation& a, const ArmorLocation& b) {
    return a.curArmor == b.curArmor && a.maxArmor == b.maxArmor;
}
template<> inline void pattern(InventoryItem& v, unsigned char seed) {
    v = patterned<InventoryItem>(&InventoryItem::masterID, &InventoryItem::health,
        &InventoryItem::disabled, &InventoryItem::facing, &InventoryItem::startAmount,
        &InventoryItem::amount, &InventoryItem::ammoIndex, &InventoryItem::readyTime,
        &InventoryItem::bodyLocation, &InventoryItem::effectiveness, &InventoryItem::hardpointId);
}
template<> inline bool equal(const InventoryItem& a, const InventoryItem& b) {
    return std::tie(a.masterID, a.health, a.disabled, a.facing, a.startAmount,
        a.amount, a.ammoIndex, a.readyTime, a.bodyLocation, a.effectiveness, a.hardpointId)
        == std::tie(b.masterID, b.health, b.disabled, b.facing, b.startAmount,
        b.amount, b.ammoIndex, b.readyTime, b.bodyLocation, b.effectiveness, b.hardpointId);
}
template<> inline void pattern(AmmoTally& v, unsigned char seed) {
    pattern(v.masterId, seed); pattern(v.curAmount, seed); pattern(v.maxAmount, seed);
}
template<> inline bool equal(const AmmoTally& a, const AmmoTally& b) {
    return std::tie(a.masterId, a.curAmount, a.maxAmount) == std::tie(b.masterId, b.curAmount, b.maxAmount);
}
template<> inline void pattern(MoverControl& v, unsigned char seed) {
    v.type = CONTROL_AI; v.dataType = CONTROL_DATA_GROUNDVEHICLE;
    auto& s = v.settings.groundVehicle;
    pattern(s.throttle, seed); pattern(s.rotate, seed); pattern(s.rotateTurret, seed);
    pattern(s.gestureGoal, seed); pattern(s.pivot, seed); pattern(s.isWalking, seed);
}
template<> inline void zero(MoverControl& v) {
    v.type = CONTROL_BASE; v.dataType = CONTROL_DATA_BASE; v.settings = {};
}
template<> inline bool equal(const MoverControl& a, const MoverControl& b) {
    if (a.type != b.type || a.dataType != b.dataType) return false;
    const auto& x = a.settings.groundVehicle; const auto& y = b.settings.groundVehicle;
    return std::tie(x.throttle, x.rotate, x.rotateTurret, x.gestureGoal, x.pivot, x.isWalking)
        == std::tie(y.throttle, y.rotate, y.rotateTurret, y.gestureGoal, y.pivot, y.isWalking);
}
template<> inline void pattern(MoverDynamics& v, unsigned char seed) {
    v.type = DYNAMICS_GROUNDVEHICLE;
    auto& s = v.max.groundVehicle;
    pattern(s.yawRate, seed); pattern(s.turretYawRate, seed); pattern(s.pivotRate, seed);
    pattern(s.accel, seed); pattern(s.speed, seed); pattern(s.turretYaw, seed);
}
template<> inline void zero(MoverDynamics& v) { v.type = DYNAMICS_BASE; v.max = {}; }
template<> inline bool equal(const MoverDynamics& a, const MoverDynamics& b) {
    if (a.type != b.type) return false;
    const auto& x = a.max.groundVehicle; const auto& y = b.max.groundVehicle;
    return std::tie(x.yawRate, x.turretYawRate, x.pivotRate, x.accel, x.speed, x.turretYaw)
        == std::tie(y.yawRate, y.turretYawRate, y.pivotRate, y.accel, y.speed, y.turretYaw);
}
template<> inline void pattern(Stuff::Vector4D& v, unsigned char seed) {
    pattern(v.x, seed); pattern(v.y, seed); pattern(v.z, seed); pattern(v.w, seed);
}
template<> inline bool equal(const Stuff::Vector4D& a, const Stuff::Vector4D& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
template<> inline void zero(Stuff::Vector4D& v) { v.x = v.y = v.z = v.w = 0; }
template<> inline void pattern(CarnageInfo& v, unsigned char seed) {
    pattern(v.fire.timeToBurn, seed); pattern(v.fire.radius, seed);
    pattern(v.fire.reallyVisible, seed);
}
template<> inline bool equal(const CarnageInfo& a, const CarnageInfo& b) {
    // Compare union payload bits, not a float interpretation of reallyVisible.
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}
template<> inline void pattern(ArtilleryInfo& v, unsigned char seed) {
    using T = decltype(v.strike);
    v.strike = patterned<T>(&T::timeToImpact, &T::timeToLaunch, &T::sensorRange,
        &T::timeToBlind, &T::sensorSystemIndex, &T::contactUpdate,
        &T::timeString, &T::explosionOffFlags);
}
template<> inline bool equal(const ArtilleryInfo& a, const ArtilleryInfo& b) {
    const auto& x = a.strike; const auto& y = b.strike;
    return std::tie(x.timeToImpact, x.timeToLaunch, x.sensorRange, x.timeToBlind,
        x.sensorSystemIndex, x.contactUpdate, x.explosionOffFlags) ==
        std::tie(y.timeToImpact, y.timeToLaunch, y.sensorRange, y.timeToBlind,
        y.sensorSystemIndex, y.contactUpdate, y.explosionOffFlags) &&
        equal(x.timeString, y.timeString);
}
template<> inline void pattern(WeaponShotInfo& v, unsigned char seed) {
    pattern(v.attackerWID, seed); pattern(v.masterId, seed); pattern(v.damage, seed);
    pattern(v.hitLocation, seed); pattern(v.entryAngle, seed);
}
template<> inline void zero(WeaponShotInfo& v) {
    v.attackerWID = 0; v.masterId = 0; v.damage = 0; v.hitLocation = 0; v.entryAngle = 0;
}
template<> inline bool equal(const WeaponShotInfo& a, const WeaponShotInfo& b) {
    return std::tie(a.attackerWID, a.masterId, a.damage, a.hitLocation, a.entryAngle) ==
        std::tie(b.attackerWID, b.masterId, b.damage, b.hitLocation, b.entryAngle);
}
}

// Independent of production serialize(). Base members are enumerated here;
// structured binding catches new members even when they consume old padding.
#define BASE_MEMBERS &T::objectTypeNum, &T::objectClass, &T::handle, &T::partId, \
    &T::watchID, &T::typeHandle, &T::position, &T::cellPositionRow, \
    &T::cellPositionCol, &T::d_vertexNum, &T::flags, &T::debugFlags, &T::status, \
    &T::tonnage, &T::rotation, &T::appearanceTypeID, &T::collisionFreeFromWID, \
    &T::collisionFreeTime, &T::screenPos, &T::windowsVisible, &T::explRadius, \
    &T::explDamage, &T::maxCV, &T::curCV, &T::threatRating, &T::lastFrameTime, \
    &T::blipFrame, &T::numAttackers, &T::drawFlags
#define CARNAGE_MEMBERS &T::carnageType, &T::ownerWID, &T::info, &T::effectId

#define TERRAIN_MEMBERS &T::damage, &T::vertexNumber, &T::blockNumber, \
    &T::pitchAngle, &T::fallRate, &T::powerSupply, &T::cellFootprint, \
    &T::vectorFootprint, &T::numSubAreas0, &T::numSubAreas1, &T::subAreas0, \
    &T::subAreas1, &T::listID, &T::numCellsCovered, &T::cellsCovered

#define BUILDING_MEMBERS &T::teamId, &T::commanderId, &T::refitBuddyWID, \
    &T::parentId, &T::parent, &T::listID, &T::captureTime, &T::moverInProximity, &T::proximityTimer

TEST_CASE("save fields: TerrainObjectData independent inherited census") {
    using T = TerrainObjectData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, TERRAIN_MEMBERS);
    sample.objectClass = TERRAINOBJECT; sample.windowsVisible = 0;
    sample.numSubAreas0 = sample.numSubAreas1 = MAX_SPECIAL_SUB_AREAS;
    sample.numCellsCovered = 40; sample.cellsCovered[80] = 0;
    const auto codec = [](auto& a, auto& v) {
        mc2::save::TerrainRecord record{v}; record.serialize(a);
        if (a.reading()) v = record.value;
    };
    REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, TERRAIN_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::TerrainRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.powerSupply = 0; }
    }, BASE_MEMBERS, TERRAIN_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::TerrainRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.subAreas1[MAX_SPECIAL_SUB_AREAS-1] = 0; }
    }, BASE_MEMBERS, TERRAIN_MEMBERS));
    const auto bytes = mc2::save::encodeTaggedRecord(mc2::save::TerrainRecord{sample}, mc2::save::terrainTag);
    mc2::save::TerrainRecord decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::terrainTag));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1), decoded, mc2::save::terrainTag));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::gateTag));
    // This class uses the same complete TerrainObjectData declaration census
    // and zero-tail/bounds cases as GateData; those tests run in the batch.
}

TEST_CASE("save fields: BuildingData inherited census and legacy values") {
    using T = BuildingData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, TERRAIN_MEMBERS,
        &TerrainObjectData::listID, BUILDING_MEMBERS);
    sample.objectClass = BUILDING; sample.windowsVisible = 0;
    sample.numSubAreas0 = sample.numSubAreas1 = MAX_SPECIAL_SUB_AREAS;
    sample.numCellsCovered = 40; sample.cellsCovered[80] = 0;
    const auto codec = [](auto& a, auto& v) {
        mc2::save::BuildingRecord record{v}; record.serialize(a);
        if (a.reading()) v = record.value;
    };
    REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, TERRAIN_MEMBERS,
        &TerrainObjectData::listID, BUILDING_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::BuildingRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.refitBuddyWID = 0; }
    }, BASE_MEMBERS, TERRAIN_MEMBERS, &TerrainObjectData::listID, BUILDING_MEMBERS));
    const auto bytes = mc2::save::encodeTaggedRecord(mc2::save::BuildingRecord{sample}, mc2::save::buildingTag);
    mc2::save::BuildingRecord decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::buildingTag));
    CHECK(decoded.value.TerrainObjectData::listID == sample.TerrainObjectData::listID);
    CHECK(decoded.value.listID == sample.listID);
    CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1), decoded, mc2::save::buildingTag));
    sample.baseTileId = 77; sample.updatedTurn = 88;
    CHECK(mc2::save::encodeTaggedRecord(mc2::save::BuildingRecord{sample}, mc2::save::buildingTag) == bytes);
    decoded.value.baseTileId = 99; decoded.value.updatedTurn = 100;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::buildingTag));
    CHECK(decoded.value.baseTileId == 0); CHECK(decoded.value.updatedTurn == 0);
    const auto source = compactSource("code/bldng.h");
    CHECK(source.find("struct_BuildingData:publicTerrainObjectData{charteamId;"
        "unsignedcharbaseTileId;charcommanderId;GameObjectWatchIDrefitBuddyWID;DWORDparentId;"
        "GameObjectWatchIDparent;unsignedcharlistID;floatcaptureTime;boolmoverInProximity;"
        "floatproximityTimer;int32_tupdatedTurn;};") != std::string::npos);
}
#define GATE_MEMBERS &T::teamId, &T::lockedOpen, &T::lockedClose, &T::reasonToOpen, \
    &T::opened, &T::opening, &T::closed, &T::closing, &T::justDestroyed, \
    &T::lastMarkedOpen, &T::parentId, &T::parent, &T::buildingDescriptionID

#define MOVER_MEMBERS &T::killed, &T::lost, &T::positionNormal, &T::velocity, &T::name, \
    &T::chassis, &T::startDisabled, &T::creationTime, &T::moveType, &T::moveLevel, \
    &T::followRoads, &T::lastMapCell, &T::damageRateTally, &T::damageRateCheckTime, \
    &T::pilotCheckDamageTally, &T::body, &T::numBodyLocations, &T::fieldedCV, \
    &T::attackRange, &T::armor, &T::numArmorLocations, &T::longName, &T::inventory, \
    &T::numOther, &T::numWeapons, &T::numAmmos, &T::ammoTypeTotal, &T::numAmmoTypes, \
    &T::pilotHandle, &T::cockpit, &T::engine, &T::lifeSupport, &T::sensor, &T::ecm, \
    &T::probe, &T::jumpJets, &T::nullSignature, &T::maxWeaponEffectiveness, \
    &T::weaponEffectiveness, &T::minRange, &T::maxRange, &T::optimalRange, \
    &T::numFunctionalWeapons, &T::numAntiMissileSystems, &T::antiMissileSystem, \
    &T::engineBlowTime, &T::maxMoveSpeed, &T::shutDownThisFrame, &T::startUpThisFrame, \
    &T::disableThisFrame, &T::teamId, &T::groupId, &T::squadId, &T::selectionIndex, \
    &T::teamRosterIndex, &T::commanderId, &T::unitGroup, &T::iconPictureIndex, \
    &T::suppressionFire, &T::pilotCheckModifier, &T::prevPilotCheckModifier, \
    &T::prevPilotCheckDelta, &T::prevPilotCheckUpdate, &T::failedPilotingCheck, \
    &T::lastWeaponEffectivenessCalc, &T::lastOptimalRangeCalc, &T::challengerWID, \
    &T::lastGesture, &T::control, &T::dynamics, &T::numWeaponHitsHandled, &T::timeLeft, \
    &T::exploding, &T::withdrawing, &T::yieldTimeLeft, &T::lastValidPosition, \
    &T::pivotDirection, &T::lastHustleTime, &T::salvageVehicle, &T::markDistanceMoved, \
    &T::refitBuddyWID, &T::recoverBuddyWID, &T::crashAvoidSelf, &T::crashAvoidPath, \
    &T::crashBlockSelf, &T::crashBlockPath, &T::crashYieldTime, &T::pathLockLength, \
    &T::pathLockList, &T::moveCenter, &T::moveRadius, &T::overlayWeightClass, \
    &T::timeToClearSelection, &T::timeSinceMoving, &T::timeSinceFiredLast, \
    &T::lastMovingTargetWID, &T::mechSalvage, &T::teleportPosition, &T::debugPage, \
    &T::pathLocks, &T::isOnGui, &T::conStat, &T::fadeTime, &T::alphaValue, \
    &T::causeOfDeath, &T::lowestWeaponNodeID, &T::psRed, &T::psBlue, &T::psGreen
#define VEHICLE_MEMBERS &T::accel, &T::velocityMag, &T::suspension, &T::movementEnabled, \
    &T::turretEnabled, &T::turretRotation, &T::turretBlownThisFrame, &T::captureable, \
    &T::deadByCrushed, &T::canRefit, &T::canRecover, &T::refitting, &T::recovering, \
    &T::mineSweeper, &T::sweepTime, &T::mineLayer, &T::aerospaceSpotter, &T::cellColToMine, \
    &T::cellRowToMine, &T::notMineYet, &T::battleRating, &T::descID, &T::timeInCurrent, \
    &T::dVel, &T::dRot, &T::dAcc, &T::dRVel, &T::dRacc, &T::dTime, &T::sensorOK, &T::isVehiclePilot

TEST_CASE("save fields: GroundVehicleData inherited census and legacy values") {
    using T = GroundVehicleData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, MOVER_MEMBERS, VEHICLE_MEMBERS);
    sample.objectClass = GROUNDVEHICLE; sample.windowsVisible = 0;
    const auto codec = [](auto& a, auto& v) {
        mc2::save::VehicleRecord record{v}; record.serialize(a);
        if (a.reading()) v = record.value;
    };
    REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, MOVER_MEMBERS, VEHICLE_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::VehicleRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.sensorOK = false; }
    }, BASE_MEMBERS, MOVER_MEMBERS, VEHICLE_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::VehicleRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.inventory[0].hardpointId = 0; }
    }, BASE_MEMBERS, MOVER_MEMBERS, VEHICLE_MEMBERS));
    SAVE_FIELD_SHAPE(sample.body[0], CASE, totalSpaces, criticalSpaces,
        curInternalStructure, hotSpotNumber, maxInternalStructure, damageState);
    SAVE_FIELD_SHAPE(sample.body[0].criticalSpaces[0], inventoryID, hit);
    SAVE_FIELD_SHAPE(sample.armor[0], curArmor, maxArmor);
    SAVE_FIELD_SHAPE(sample.inventory[0], masterID, health, disabled, facing,
        startAmount, amount, ammoIndex, readyTime, bodyLocation, effectiveness, hardpointId);
    SAVE_FIELD_SHAPE(sample.ammoTypeTotal[0], masterId, curAmount, maxAmount);
    SAVE_FIELD_SHAPE(sample.control.settings.groundVehicle, throttle, rotate,
        rotateTurret, gestureGoal, pivot, isWalking);
    SAVE_FIELD_SHAPE(sample.control.settings.mech, throttle, rotate, facingRotate,
        rotateTorso, rotateLeftArm, rotateRightArm, gestureGoal, blowLeftArm, blowRightArm, pivot);
    SAVE_FIELD_SHAPE(sample.control.settings.elemental, rotate);
    SAVE_FIELD_SHAPE(sample.dynamics.max.groundVehicle, yawRate, turretYawRate,
        pivotRate, accel, speed, turretYaw);
    SAVE_FIELD_SHAPE(sample.dynamics.max.mech, torsoYawRate, torsoYaw);
    SAVE_FIELD_SHAPE(sample.dynamics.max.elemental, yawRate);
    const auto bytes = mc2::save::encodeTaggedRecord(mc2::save::VehicleRecord{sample}, mc2::save::vehicleTag);
    mc2::save::VehicleRecord decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::vehicleTag));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1), decoded, mc2::save::vehicleTag));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::gateTag));
    CHECK(save_probe::equal(sample.control, decoded.value.control));
    CHECK(save_probe::equal(sample.dynamics, decoded.value.dynamics));
    const auto leaf = compactSource("code/gvehicl.h");
    CHECK(leaf.find("struct_GroundVehicleData:publicMoverData{floataccel;floatvelocityMag;"
        "floatsuspension;boolmovementEnabled;boolturretEnabled;floatturretRotation;"
        "boolturretBlownThisFrame;boolcaptureable;booldeadByCrushed;boolcanRefit;"
        "boolcanRecover;boolrefitting;boolrecovering;boolmineSweeper;floatsweepTime;"
        "int32_tmineLayer;boolaerospaceSpotter;int32_tcellColToMine;int32_tcellRowToMine;"
        "boolnotMineYet;int32_tbattleRating;int32_tdescID;floattimeInCurrent;"
        "glm::vec3dVel;glm::vec3dRot;glm::vec3dAcc;glm::vec3dRVel;glm::vec3dRacc;"
        "floatdTime;boolsensorOK;boolisVehiclePilot;};") != std::string::npos);
    const auto base = compactSource("code/mover.h");
    CHECK(base.find("classMoverControl{public:ControlTypetype;ControlDataTypedataType;"
        "ControlSettingssettings;public:") != std::string::npos);
    CHECK(base.find("classMoverDynamics{public:DynamicsTypetype;DynamicsLimitsmax;public:") != std::string::npos);
    CHECK(base.find("struct_MoverData:publicGameObjectData{boolkilled;boollost;"
        "glm::vec3positionNormal;glm::vec3velocity;charname[MAXLEN_MOVER_NAME];"
        "unsignedcharchassis;boolstartDisabled;floatcreationTime;int32_tmoveType;"
        "int32_tmoveLevel;boolfollowRoads;int32_tlastMapCell[2];floatdamageRateTally;"
        "floatdamageRateCheckTime;floatpilotCheckDamageTally;BodyLocationbody[MAX_MOVER_BODY_LOCATIONS];"
        "charnumBodyLocations;int32_tfieldedCV;int32_tattackRange;ArmorLocationarmor[MAX_MOVER_ARMOR_LOCATIONS];"
        "charnumArmorLocations;charlongName[MAXLEN_MECH_LONGNAME];InventoryIteminventory[MAX_MOVER_INVENTORY_ITEMS];"
        "unsignedcharnumOther;unsignedcharnumWeapons;unsignedcharnumAmmos;AmmoTallyammoTypeTotal[MAX_AMMO_TYPES];"
        "charnumAmmoTypes;int32_tpilotHandle;unsignedcharcockpit;unsignedcharengine;unsignedcharlifeSupport;"
        "unsignedcharsensor;unsignedcharecm;unsignedcharprobe;unsignedcharjumpJets;unsignedcharnullSignature;"
        "floatmaxWeaponEffectiveness;floatweaponEffectiveness;floatminRange;floatmaxRange;floatoptimalRange;"
        "int32_tnumFunctionalWeapons;charnumAntiMissileSystems;unsignedcharantiMissileSystem[MAX_ANTI_MISSILE_SYSTEMS];"
        "floatengineBlowTime;floatmaxMoveSpeed;boolshutDownThisFrame;boolstartUpThisFrame;booldisableThisFrame;"
        "charteamId;chargroupId;int32_tsquadId;int32_tselectionIndex;int32_tteamRosterIndex;charcommanderId;"
        "int32_tunitGroup;int32_ticonPictureIndex;boolsuppressionFire;int32_tpilotCheckModifier;"
        "int32_tprevPilotCheckModifier;int32_tprevPilotCheckDelta;floatprevPilotCheckUpdate;boolfailedPilotingCheck;"
        "floatlastWeaponEffectivenessCalc;floatlastOptimalRangeCalc;GameObjectWatchIDchallengerWID;charlastGesture;"
        "MoverControlcontrol;MoverDynamicsdynamics;int32_tnumWeaponHitsHandled;floattimeLeft;boolexploding;"
        "boolwithdrawing;floatyieldTimeLeft;glm::vec3lastValidPosition;charpivotDirection;floatlastHustleTime;"
        "boolsalvageVehicle;floatmarkDistanceMoved;GameObjectWatchIDrefitBuddyWID;GameObjectWatchIDrecoverBuddyWID;"
        "int32_tcrashAvoidSelf;int32_tcrashAvoidPath;int32_tcrashBlockSelf;int32_tcrashBlockPath;floatcrashYieldTime;"
        "int32_tpathLockLength;int32_tpathLockList[MAX_LOCK_RANGE][2];glm::vec3moveCenter;floatmoveRadius;"
        "int32_toverlayWeightClass;floattimeToClearSelection;floattimeSinceMoving;floattimeSinceFiredLast;"
        "GameObjectWatchIDlastMovingTargetWID;boolmechSalvage;glm::vec3teleportPosition;int32_tdebugPage;"
        "boolpathLocks;boolisOnGui;int32_tconStat;floatfadeTime;BYTEalphaValue;int32_tcauseOfDeath;"
        "int32_tlowestWeaponNodeID;DWORDpsRed;DWORDpsBlue;DWORDpsGreen;};") != std::string::npos);
}

// Standalone codec tests do not link mover_chunks.cpp's engine/file readers.
// None of these file-loading entry points participates in value serialization.
void MoverDynamics::init() { type = DYNAMICS_BASE; }
void MoverDynamics::init(CSVFilePtr) { FAIL("unexpected dynamics CSV load"); }
void MoverDynamics::init(FitIniFilePtr) { FAIL("unexpected dynamics FIT load"); }
long MoverControl::init(FitIniFilePtr) { FAIL("unexpected control FIT load"); return 0; }

TEST_CASE("save fields: GroundVehicleData control and dynamics union census") {
    MoverControl control;
    using S = decltype(control.settings.mech);
    auto mech = save_probe::patterned<S>(&S::throttle, &S::rotate, &S::facingRotate,
        &S::rotateTorso, &S::rotateLeftArm, &S::rotateRightArm, &S::gestureGoal,
        &S::blowLeftArm, &S::blowRightArm, &S::pivot);
    REQUIRE(save_probe::roundtrip(mech, [](auto& a, auto& s) {
        MoverControl v; v.type = CONTROL_NET; v.dataType = CONTROL_DATA_MECH;
        v.settings.mech = s; mc2::save::moverControlFields(a, v);
        if (a.reading()) s = v.settings.mech;
    }, &S::throttle, &S::rotate, &S::facingRotate, &S::rotateTorso,
        &S::rotateLeftArm, &S::rotateRightArm, &S::gestureGoal,
        &S::blowLeftArm, &S::blowRightArm, &S::pivot));
    using E = decltype(control.settings.elemental);
    REQUIRE(save_probe::roundtrip(save_probe::patterned<E>(&E::rotate), [](auto& a, auto& s) {
        MoverControl v; v.type = CONTROL_PLAYER; v.dataType = CONTROL_DATA_ELEMENTAL;
        v.settings.elemental = s; mc2::save::moverControlFields(a, v);
        if (a.reading()) s = v.settings.elemental;
    }, &E::rotate));
    MoverDynamics dynamics;
    using D = decltype(dynamics.max.mech);
    REQUIRE(save_probe::roundtrip(save_probe::patterned<D>(&D::torsoYawRate, &D::torsoYaw), [](auto& a, auto& s) {
        MoverDynamics v; v.type = DYNAMICS_MECH; v.max.mech = s;
        mc2::save::moverDynamicsFields(a, v); if (a.reading()) s = v.max.mech;
    }, &D::torsoYawRate, &D::torsoYaw));
    using F = decltype(dynamics.max.elemental);
    REQUIRE(save_probe::roundtrip(save_probe::patterned<F>(&F::yawRate), [](auto& a, auto& s) {
        MoverDynamics v; v.type = DYNAMICS_ELEMENTAL; v.max.elemental = s;
        mc2::save::moverDynamicsFields(a, v); if (a.reading()) s = v.max.elemental;
    }, &F::yawRate));
    mc2::save::VehicleRecord sample;
    const auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::vehicleTag);
    REQUIRE_FALSE(bytes.empty()); // BASE union views have no values.
    mc2::save::VehicleRecord decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::vehicleTag));
    CHECK(decoded.value.control.dataType == CONTROL_DATA_BASE);
    CHECK(decoded.value.dynamics.type == DYNAMICS_BASE);
    sample.value.control.type = static_cast<ControlType>(-1);
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::vehicleTag).empty());
    sample.value.control.type = CONTROL_BASE;
    sample.value.control.dataType = static_cast<ControlDataType>(99);
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::vehicleTag).empty());
    sample.value.control.dataType = CONTROL_DATA_BASE;
    sample.value.dynamics.type = static_cast<DynamicsType>(99);
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::vehicleTag).empty());
    const auto source = compactSource("code/mover.h");
    CHECK(source.find("unionControlSettings{struct{floatrotate;}elemental;"
        "struct{charthrottle;floatrotate;floatrotateTurret;chargestureGoal;boolpivot;boolisWalking;}groundVehicle;"
        "struct{charthrottle;floatrotate;floatfacingRotate;floatrotateTorso;floatrotateLeftArm;"
        "floatrotateRightArm;chargestureGoal;boolblowLeftArm;boolblowRightArm;boolpivot;}mech;};") != std::string::npos);
    CHECK(source.find("unionDynamicsLimits{struct{int32_tyawRate;}elemental;"
        "struct{int32_tyawRate;int32_tturretYawRate;int32_tpivotRate;floataccel;floatspeed;int32_tturretYaw;}groundVehicle;"
        "struct{int32_ttorsoYawRate;int32_ttorsoYaw;}mech;};") != std::string::npos);
}

TEST_CASE("save fields: GateData inherited census and legacy values") {
    using T = GateData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, TERRAIN_MEMBERS, GATE_MEMBERS);
    sample.objectClass = GATE;
    sample.windowsVisible = 0;
    sample.numSubAreas0 = sample.numSubAreas1 = MAX_SPECIAL_SUB_AREAS;
    sample.numCellsCovered = 40;
    sample.cellsCovered[80] = 0;
    const auto codec = [](auto& a, auto& v) {
        mc2::save::GateRecord record{v}; record.serialize(a);
        if (a.reading()) v = record.value;
    };
    REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, TERRAIN_MEMBERS, GATE_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::GateRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.parent = 0; }
    }, BASE_MEMBERS, TERRAIN_MEMBERS, GATE_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::GateRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.cellsCovered[79] = 0; }
    }, BASE_MEMBERS, TERRAIN_MEMBERS, GATE_MEMBERS));
    const auto bytes = mc2::save::encodeTaggedRecord(mc2::save::GateRecord{sample}, mc2::save::gateTag);
    mc2::save::GateRecord decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::gateTag));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1), decoded, mc2::save::gateTag));
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::artilleryTag));
    const auto source = compactSource("code/gate.h");
    CHECK(source.find("struct_GateData:publicTerrainObjectData{charteamId;"
        "boollockedOpen;boollockedClose;boolreasonToOpen;boolopened;boolopening;"
        "boolclosed;boolclosing;booljustDestroyed;boollastMarkedOpen;MoverPtrclosestObject;"
        "DWORDparentId;GameObjectWatchIDparent;longbuildingDescriptionID;longupdatedTurn;};") != std::string::npos);
    const auto terrain = compactSource("code/terrobj.h");
    CHECK(terrain.find("struct_TerrainObjectData:publicGameObjectData{floatdamage;"
        "int32_tvertexNumber;int32_tblockNumber;floatpitchAngle;floatfallRate;"
        "GameObjectWatchIDpowerSupply;shortcellFootprint[4];glm::vec3vectorFootprint[4];"
        "shortnumSubAreas0;shortnumSubAreas1;shortsubAreas0[MAX_SPECIAL_SUB_AREAS];"
        "shortsubAreas1[MAX_SPECIAL_SUB_AREAS];unsignedcharlistID;"
        "unsignedcharnumCellsCovered;shortcellsCovered[81];};") != std::string::npos);
}

TEST_CASE("save fields: GateData derived values and terrain bounds") {
    mc2::save::GateRecord sample;
    const auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag);
    REQUIRE_FALSE(bytes.empty());
    int blocker = 0;
    // An opaque non-null pointer is never dereferenced or serialized.
    sample.value.closestObject = reinterpret_cast<MoverPtr>(&blocker);
    sample.value.updatedTurn = 99;
    sample.value.subAreas0[0] = sample.value.subAreas1[0] = 15;
    sample.value.cellsCovered[80] = 12;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag) == bytes);
    mc2::save::GateRecord decoded = sample;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::gateTag));
    CHECK(decoded.value.closestObject == nullptr);
    CHECK(decoded.value.updatedTurn == 0);
    CHECK(decoded.value.subAreas0[0] == 0);
    CHECK(decoded.value.subAreas1[0] == 0);
    CHECK(decoded.value.cellsCovered[80] == 0);
    sample.value.numCellsCovered = 41;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag).empty());
    sample.value.numCellsCovered = 0;
    sample.value.numSubAreas0 = -1;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag).empty());
    sample.value.numSubAreas0 = 0;
    sample.value.numSubAreas1 = MAX_SPECIAL_SUB_AREAS + 1;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::gateTag).empty());
}

TEST_CASE("save fields: CarnageData independent base and union census") {
    using T = CarnageData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, CARNAGE_MEMBERS);
    sample.objectClass = FIRE;
    sample.windowsVisible = 0; // explicitly derived; tested separately below
    auto& base = static_cast<GameObjectData&>(sample);
    SAVE_FIELD_SHAPE(base, objectTypeNum, objectClass, handle, partId, watchID,
        typeHandle, position, cellPositionRow, cellPositionCol, d_vertexNum,
        flags, debugFlags, status, tonnage, rotation, appearanceTypeID,
        collisionFreeFromWID, collisionFreeTime, screenPos, windowsVisible,
        explRadius, explDamage, maxCV, curCV, threatRating, lastFrameTime,
        blipFrame, numAttackers, drawFlags);
    SAVE_FIELD_SHAPE(sample.info.fire, timeToBurn, radius, reallyVisible);
    SAVE_FIELD_SHAPE(sample.info.explosion, timer, radius, chunkSize);
    const auto codec = [](auto& a, auto& v) {
        mc2::save::CarnageRecord record{v};
        record.serialize(a);
        if (a.reading()) v = record.value;
    };
    for (auto kind : {CARNAGE_NONE, CARNAGE_FIRE, CARNAGE_EXPLOSION}) {
        sample.carnageType = kind;
        REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, CARNAGE_MEMBERS));
        const auto bytes = mc2::save::encodeTaggedRecord(
            mc2::save::CarnageRecord{sample}, mc2::save::carnageTag);
        mc2::save::CarnageRecord decoded;
        REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::carnageTag));
        CHECK(save_probe::equal(sample.info, decoded.value.info));
        CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1),
            decoded, mc2::save::carnageTag));
        CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagTeam));
    }
    // Negative controls: the census must observe loss in both the base and leaf.
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::CarnageRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.watchID = 0; }
    }, BASE_MEMBERS, CARNAGE_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::CarnageRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.info.explosion.chunkSize = 0; }
    }, BASE_MEMBERS, CARNAGE_MEMBERS));
}

TEST_CASE("save fields: CarnageData derived visibility and invalid discriminant") {
    mc2::save::CarnageRecord sample;
    sample.value.screenPos = Stuff::Vector4D(1, 2, 3, 4);
    const auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::carnageTag);
    REQUIRE_FALSE(bytes.empty());
    sample.value.windowsVisible = 123;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::carnageTag) == bytes);
    mc2::save::CarnageRecord decoded;
    decoded.value.windowsVisible = 456;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::carnageTag));
    CHECK(decoded.value.windowsVisible == 0);
    sample.value.carnageType = NUM_CARNAGE_TYPES;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::carnageTag).empty());
}

TEST_CASE("save fields: CarnageData inherited declaration census") {
    // C++ cannot decompose a class with fields in both base and derived class.
    // Pin the complete leaf/union declarations instead; additions inside old
    // padding must fail this test as well as omissions from the member list.
    const auto source = compactSource("code/carnage.h");
    CHECK(source.find("struct_CarnageData:publicGameObjectData{"
        "CarnageEnumTypecarnageType;GameObjectWatchIDownerWID;CarnageInfoinfo;"
        "int32_teffectId;};") != std::string::npos);
    CHECK(source.find("union_CarnageInfo{struct{floattimeToBurn;floatradius;"
        "int32_treallyVisible;}fire;struct{floattimer;floatradius;"
        "floatchunkSize;}explosion;};") != std::string::npos);
}

#define ARTILLERY_MEMBERS &T::artilleryType, &T::teamId, &T::commanderId, \
    &T::info, &T::effectId, &T::bombRunStarted, &T::inView, &T::iFacePosition

TEST_CASE("save fields: ArtilleryData independent inherited census") {
    using T = ArtilleryData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, ARTILLERY_MEMBERS);
    sample.objectClass = ARTILLERY;
    sample.windowsVisible = 0;
    SAVE_FIELD_SHAPE(sample.info.strike, timeToImpact, timeToLaunch,
        sensorRange, timeToBlind, sensorSystemIndex, contactUpdate,
        timeString, explosionOffFlags);
    const auto codec = [](auto& a, auto& v) {
        mc2::save::ArtilleryRecord record{v}; record.serialize(a);
        if (a.reading()) v = record.value;
    };
    for (char kind = 0; kind < NUM_ARTILLERY_TYPES; ++kind) {
        sample.artilleryType = kind;
        REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, ARTILLERY_MEMBERS));
        const auto bytes = mc2::save::encodeTaggedRecord(
            mc2::save::ArtilleryRecord{sample}, mc2::save::artilleryTag);
        mc2::save::ArtilleryRecord decoded;
        REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::artilleryTag));
        CHECK(save_probe::equal(decoded.value.info, sample.info));
        CHECK_FALSE(mc2::save::decodeTaggedRecord(std::span(bytes).first(bytes.size()-1),
            decoded, mc2::save::artilleryTag));
    }
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::ArtilleryRecord record{v}; record.serialize(a);
        if (a.reading()) { v = record.value; v.info.strike.explosionOffFlags = 0; }
    }, BASE_MEMBERS, ARTILLERY_MEMBERS));
    sample.artilleryType = NUM_ARTILLERY_TYPES;
    CHECK(mc2::save::encodeTaggedRecord(mc2::save::ArtilleryRecord{sample},
        mc2::save::artilleryTag).empty());
    const auto source = compactSource("code/artlry.h");
    CHECK(source.find("struct_ArtilleryData:publicGameObjectData{charartilleryType;"
        "charteamId;charcommanderId;ArtilleryInfoinfo;int32_teffectId;"
        "boolbombRunStarted;boolinView;glm::vec3iFacePosition;};") != std::string::npos);
    CHECK(source.find("union_ArtilleryInfo{struct{floattimeToImpact;floattimeToLaunch;"
        "floatsensorRange;floattimeToBlind;shortsensorSystemIndex;floatcontactUpdate;"
        "chartimeString[5];uint32_texplosionOffFlags;}strike;};") != std::string::npos);
}

#define BOLT_MEMBERS &T::ownerWID, &T::hotSpotNumber, &T::targetWID, &T::targetHotSpot, \
    &T::targetPosition, &T::distanceToTarget, &T::halfDistanceToTarget, &T::weaponShot, \
    &T::laserPosition, &T::laserVertices, &T::laserSide, &T::effectId, &T::hitTarget, \
    &T::timeLeft, &T::hsPos, &T::hitLeft, &T::mcTextureHandle, &T::gosTextureHandle, \
    &T::startUV, &T::goalHeight, &T::retireDeadlineTick, &T::retireDeadlineState

TEST_CASE("save fields: WeaponBoltData independent inherited census") {
    using T = WeaponBoltData;
    auto sample = save_probe::patterned<T>(BASE_MEMBERS, BOLT_MEMBERS);
    sample.objectClass = WEAPONBOLT;
    sample.windowsVisible = 0;
    sample.mcTextureHandle = sample.gosTextureHandle = 0; // explicitly derived
    sample.retireDeadlineState = mc2::combat::SavedDeadlineState::armed;
    SAVE_FIELD_SHAPE(sample.weaponShot, attackerWID, masterId, damage, hitLocation, entryAngle);
    const auto codec = [](auto& a, auto& v) {
        mc2::save::WeaponBoltRecord record{v}; record.serialize(a);
    };
    REQUIRE(save_probe::roundtrip(sample, codec, BASE_MEMBERS, BOLT_MEMBERS));
    auto bytes = mc2::save::encodeTaggedRecord(
        mc2::save::WeaponBoltRecord{sample}, mc2::save::weaponBoltTag);
    REQUIRE_FALSE(bytes.empty());
    auto decoded = mc2::save::decodeWeaponBoltRecord(bytes);
    REQUIRE(decoded.has_value());
    CHECK(save_probe::equal(sample.weaponShot, decoded->weaponShot));
    CHECK_FALSE(mc2::save::decodeWeaponBoltRecord(std::span(bytes).first(bytes.size()-1)));
    sample.mcTextureHandle = 123; sample.gosTextureHandle = 456;
    CHECK(mc2::save::encodeTaggedRecord(
        mc2::save::WeaponBoltRecord{sample}, mc2::save::weaponBoltTag) == bytes);
    sample.mcTextureHandle = sample.gosTextureHandle = 0;
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::WeaponBoltRecord record{v}; record.serialize(a);
        if (a.reading()) v.laserVertices[3].z = 0;
    }, BASE_MEMBERS, BOLT_MEMBERS));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) {
        mc2::save::WeaponBoltRecord record{v}; record.serialize(a);
        if (a.reading()) v.retireDeadlineTick = 0;
    }, BASE_MEMBERS, BOLT_MEMBERS));
    bytes.back() = 255; // unknown SavedDeadlineState
    CHECK_FALSE(mc2::save::decodeWeaponBoltRecord(bytes));
    sample.retireDeadlineTick = mc2::combat::invalidTick;
    CHECK(mc2::save::encodeTaggedRecord(
        mc2::save::WeaponBoltRecord{sample}, mc2::save::weaponBoltTag).empty());
    const auto source = compactSource("code/weaponbolt.h");
    CHECK(source.find("struct_WeaponBoltData:publicGameObjectData{GameObjectWatchIDownerWID;"
        "int32_thotSpotNumber;GameObjectWatchIDtargetWID;int32_ttargetHotSpot;"
        "glm::vec3targetPosition;floatdistanceToTarget;floathalfDistanceToTarget;"
        "WeaponShotInfoweaponShot;glm::vec3laserPosition;glm::vec3laserVertices[4];"
        "glm::vec3laserSide[4];int32_teffectId;boolhitTarget;floattimeLeft;glm::vec3hsPos;"
        "floathitLeft;DWORDmcTextureHandle;DWORDgosTextureHandle;floatstartUV;floatgoalHeight;"
        "std::uint64_tretireDeadlineTick;mc2::combat::SavedDeadlineStateretireDeadlineState;};")
        != std::string::npos);
}

#undef BOLT_MEMBERS
#undef ARTILLERY_MEMBERS
#undef CARNAGE_MEMBERS
#undef BASE_MEMBERS
