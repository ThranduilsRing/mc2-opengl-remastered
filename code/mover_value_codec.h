// Packet F inherited mover values; never archive MoverControl/Dynamics vptrs.
#pragma once
#include <cstring>
#include "mover.h"
#include "object_value_codec.h"

namespace mc2::save {
// Raw 32-bit pattern of an enum slot: a stale or corrupt value outside the enumerators must reach the range check
// below, and loading it as the enum type is undefined.
template<class E> inline std::int32_t enumBits(const E& e) {
    static_assert(sizeof(E) == sizeof(std::int32_t));
    std::int32_t bits;
    std::memcpy(&bits, &e, sizeof(bits));
    return bits;
}
inline void moverControlFields(Archive& a, MoverControl& v) {
    auto type = enumBits(v.type);
    auto data = enumBits(v.dataType);
    a(type, data);
    if (!a.good()) return;
    if (type < CONTROL_BASE || type > CONTROL_NET ||
        data < CONTROL_DATA_BASE || data > CONTROL_DATA_ELEMENTAL) {
        a.reject("mover_control"); return;
    }
    v.type = static_cast<ControlType>(type);
    v.dataType = static_cast<ControlDataType>(data);
    if (v.dataType == CONTROL_DATA_MECH) {
        auto& s = v.settings.mech;
        a(s.throttle, s.rotate, s.facingRotate, s.rotateTorso, s.rotateLeftArm,
          s.rotateRightArm, s.gestureGoal, s.blowLeftArm, s.blowRightArm, s.pivot);
    } else if (v.dataType == CONTROL_DATA_GROUNDVEHICLE) {
        auto& s = v.settings.groundVehicle;
        a(s.throttle, s.rotate, s.rotateTurret, s.gestureGoal, s.pivot, s.isWalking);
    } else if (v.dataType == CONTROL_DATA_ELEMENTAL) a(v.settings.elemental.rotate);
    else if (a.reading()) v.settings = {};
}
inline void moverDynamicsFields(Archive& a, MoverDynamics& v) {
    auto type = enumBits(v.type);
    a(type);
    if (!a.good()) return;
    if (type < DYNAMICS_BASE || type > DYNAMICS_ELEMENTAL) {
        a.reject("mover_dynamics"); return;
    }
    v.type = static_cast<DynamicsType>(type);
    if (v.type == DYNAMICS_MECH) a(v.max.mech.torsoYawRate, v.max.mech.torsoYaw);
    else if (v.type == DYNAMICS_GROUNDVEHICLE) {
        auto& s = v.max.groundVehicle;
        a(s.yawRate, s.turretYawRate, s.pivotRate, s.accel, s.speed, s.turretYaw);
    } else if (v.type == DYNAMICS_ELEMENTAL) a(v.max.elemental.yawRate);
    else if (a.reading()) v.max = {};
}
inline void moverFields(Archive& a, MoverData& v) {
    gameObjectFields(a, v);
    a(v.killed, v.lost, v.positionNormal.x, v.positionNormal.y, v.positionNormal.z,
      v.velocity.x, v.velocity.y, v.velocity.z);
    for (auto& c : v.name) a(c);
    a(v.chassis, v.startDisabled, v.creationTime, v.moveType, v.moveLevel, v.followRoads);
    for (auto& cell : v.lastMapCell) a(cell);
    a(v.damageRateTally, v.damageRateCheckTime, v.pilotCheckDamageTally);
    for (auto& b : v.body) {
        a(b.CASE, b.totalSpaces);
        for (auto& c : b.criticalSpaces) a(c.inventoryID, c.hit);
        a(b.curInternalStructure, b.hotSpotNumber, b.maxInternalStructure, b.damageState);
    }
    a(v.numBodyLocations, v.fieldedCV, v.attackRange);
    for (auto& armor : v.armor) a(armor.curArmor, armor.maxArmor);
    a(v.numArmorLocations);
    for (auto& c : v.longName) a(c);
    for (auto& i : v.inventory)
        a(i.masterID, i.health, i.disabled, i.facing, i.startAmount, i.amount,
          i.ammoIndex, i.readyTime, i.bodyLocation, i.effectiveness, i.hardpointId);
    a(v.numOther, v.numWeapons, v.numAmmos);
    for (auto& tally : v.ammoTypeTotal) {
        if (!a.reading() && (!std::in_range<std::int32_t>(tally.masterId) ||
            !std::in_range<std::int32_t>(tally.curAmount) ||
            !std::in_range<std::int32_t>(tally.maxAmount))) a.reject("mover_ammo");
        auto id = static_cast<std::int32_t>(tally.masterId);
        auto current = static_cast<std::int32_t>(tally.curAmount);
        auto maximum = static_cast<std::int32_t>(tally.maxAmount);
        a(id, current, maximum);
        if (a.reading()) { tally.masterId = id; tally.curAmount = current; tally.maxAmount = maximum; }
    }
    a(v.numAmmoTypes, v.pilotHandle, v.cockpit, v.engine, v.lifeSupport, v.sensor,
      v.ecm, v.probe, v.jumpJets, v.nullSignature, v.maxWeaponEffectiveness,
      v.weaponEffectiveness, v.minRange, v.maxRange, v.optimalRange,
      v.numFunctionalWeapons, v.numAntiMissileSystems);
    for (auto& system : v.antiMissileSystem) a(system);
    a(v.engineBlowTime, v.maxMoveSpeed, v.shutDownThisFrame, v.startUpThisFrame,
      v.disableThisFrame, v.teamId, v.groupId, v.squadId, v.selectionIndex,
      v.teamRosterIndex, v.commanderId, v.unitGroup, v.iconPictureIndex,
      v.suppressionFire, v.pilotCheckModifier, v.prevPilotCheckModifier,
      v.prevPilotCheckDelta, v.prevPilotCheckUpdate, v.failedPilotingCheck,
      v.lastWeaponEffectivenessCalc, v.lastOptimalRangeCalc, v.challengerWID, v.lastGesture);
    moverControlFields(a, v.control);
    moverDynamicsFields(a, v.dynamics);
    a(v.numWeaponHitsHandled, v.timeLeft, v.exploding, v.withdrawing,
      v.yieldTimeLeft, v.lastValidPosition.x, v.lastValidPosition.y, v.lastValidPosition.z,
      v.pivotDirection, v.lastHustleTime, v.salvageVehicle, v.markDistanceMoved,
      v.refitBuddyWID, v.recoverBuddyWID, v.crashAvoidSelf, v.crashAvoidPath,
      v.crashBlockSelf, v.crashBlockPath, v.crashYieldTime, v.pathLockLength);
    for (auto& pair : v.pathLockList) for (auto& cell : pair) a(cell);
    a(v.moveCenter.x, v.moveCenter.y, v.moveCenter.z, v.moveRadius,
      v.overlayWeightClass, v.timeToClearSelection, v.timeSinceMoving,
      v.timeSinceFiredLast, v.lastMovingTargetWID, v.mechSalvage,
      v.teleportPosition.x, v.teleportPosition.y, v.teleportPosition.z,
      v.debugPage, v.pathLocks, v.isOnGui, v.conStat, v.fadeTime, v.alphaValue,
      v.causeOfDeath, v.lowestWeaponNodeID, v.psRed, v.psBlue, v.psGreen);
}
// All MoverData value fields persist, including path-lock occupancy. Only the
// active ControlSettings/DynamicsLimits union view is encoded; BASE has none.
// Vtables derive from construction. Runtime pilot resolves from pilotHandle in
// Load; sensor contacts, pending path requests, speedThrottle and jump backoff
// are not MoverData members and rebuild in pilot/path/control updates as before.
}
