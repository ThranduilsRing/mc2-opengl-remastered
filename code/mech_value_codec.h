// Mech value fields; native MechData remains the legacy reader ABI.
#pragma once
#include "mech.h"
#include "mover_value_codec.h"

namespace mc2::save {
inline void mechAppearanceFields(Archive& a, MechAppearanceData& v) {
    a(v.frameNum, v.mechFrameRate, v.leftArmOff, v.rightArmOff, v.fallen,
      v.forceStop, v.atTransitionToNextGesture, v.inReverse, v.inJump,
      v.jumpSetup, v.jumpFXSetup, v.jumpAirborne, v.oncePerFrame, v.lockRotation,
      v.velocity);
    // The native appearance image uses Windows long; wire values are int32.
    const std::array fields{&MechAppearanceData::currentStateGoal,
        &MechAppearanceData::currentGestureId, &MechAppearanceData::transitionState,
        &MechAppearanceData::oldStateGoal, &MechAppearanceData::hitGestureId,
        &MechAppearanceData::currentLOD, &MechAppearanceData::isSmoking};
    for (auto member : fields) {
        auto& field = v.*member;
        if (!a.reading() && !std::in_range<std::int32_t>(field)) a.reject("mech_appearance_int32");
        auto number = static_cast<std::int32_t>(field);
        a(number);
        if (a.reading()) field = number;
    }
    a(v.currentFrame);
    for (auto& field : v.nodeUsed) {
        if (!a.reading() && !std::in_range<std::int32_t>(field)) a.reject("mech_node_int32");
        auto number = static_cast<std::int32_t>(field);
        a(number);
        if (a.reading()) field = number;
    }
    for (auto& recycle : v.nodeRecycle) a(recycle);
    a(v.jumpDestination.x, v.jumpDestination.y, v.jumpDestination.z,
      v.jumpVelocity.x, v.jumpVelocity.y, v.jumpVelocity.z, v.baseRootNodeHeight,
      v.isWaking, v.isDusting, v.fallDust, v.isHelicopter);
    if (a.reading()) {
        v.inDebugMoveMode = v.singleStepMode = v.nextStep = v.prevStep = false;
        v.status = v.sensorLevel = 0;
    }
}
inline constexpr auto mechTag = fourcc('M', 'E', 'C', 'H');
struct MechRecord {
    MechData value{};
    void serialize(Archive& a) {
        auto& v = value;
        moverFields(a, v);
        a(v.chassisClass, v.chassisBR, v.variantID);
        for (auto& c : v.variantName) a(c);
        a(v.legStatus, v.torsoStatus);
        for (auto& actuator : v.actuator) a(actuator);
        a(v.gyro, v.sensorOK, v.numJumpJets, v.lastJumpTime, v.inJump,
          v.jumpGoal.x, v.jumpGoal.y, v.jumpGoal.z, v.centerTorsoBlowTime,
          v.hitFromBehindThisFrame, v.hitFromFrontThisFrame, v.leftArmBlownThisFrame,
          v.rightArmBlownThisFrame, v.torsoRotation, v.rightArmRotation,
          v.leftArmRotation, v.fallen, v.mechRemoved, v.pivotTurnRate,
          v.playedJumpSFX, v.playedCriticalHit, v.maxWeaponDamage);
        for (auto& c : v.longName) a(c);
        a(v.pilotNum, v.captureable, v.notMineYet, v.descID, v.sentCrippledMsg);
        for (auto& rotation : v.rotateValues) a(rotation);
        for (auto& location : v.ItemLocationToInvLocation) a(location);
        a(v.damageAfterDisabled, v.numArmorComponents, v.cBills);
        mechAppearanceFields(a, v.apData);
        if (a.reading()) v.damageThisFrame = 0;
    }
};
// Derived: damageThisFrame -> 0 (CopyTo starts a new damage accumulation turn).
// Appearance debug stepping flags and sensorLevel -> 0 (CopyTo never writes
// them; copyFrom never reads them). Appearance status -> 0: copyFrom lets the
// mech reset status so destroyed shapes rebuild correctly. Effect owners
// rebuild from saved smoke/wake/dust/jump flags in copyFrom. sensorOK is health;
// contacts/owner registration rebuild in Load. Both hidden longName arrays
// persist. Legacy copyTo/copyFrom's ten-byte node-table rule is unchanged.
}
