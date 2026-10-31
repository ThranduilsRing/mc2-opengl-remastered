// Packet D: explicit pilot values, independent of the native legacy image.
#pragma once
#include "warrior_value_codec.h"
#include "tacorder_terrain_queue.h"

namespace mc2::save {
inline constexpr auto warriorDataTag = fourcc('W', 'A', 'R', 'R');

class WarriorCodec {
    Archive& a;
public:
    explicit WarriorCodec(Archive& archive) : a(archive) {}
    template<class... T> void operator()(T&... values) { (field(values), ...); }
    template<class T> requires std::is_arithmetic_v<T>
    void field(T& v) { a(v); }
    template<class T> requires std::is_enum_v<T>
    void field(T& v) {
        static_assert(sizeof(T) == 4);
        auto bits = static_cast<std::int32_t>(v);
        a(bits);
        if (a.reading() && a.good()) v = static_cast<T>(bits);
    }
    template<class T, size_t N> void field(T (&values)[N]) {
        for (auto& v : values) field(v);
    }
    void field(glm::vec3& v) { a(v.x, v.y, v.z); }
    void field(MemoryCell& v) {
        // Untagged ABL scratch union: preserve all 32 bits, including float NaNs.
        auto bits = std::bit_cast<std::uint32_t>(v);
        a(bits);
        if (a.reading() && a.good()) v = std::bit_cast<MemoryCell>(bits);
    }
    void field(RadioLog& v) {
        (*this)(v.lastUnderFire, v.weaponsIneffective, v.weaponsOut, v.lastContact,
            v.lastMessageType, v.lastMessage, v.lastMessageTime);
    }
    void field(AttackerRec& v) { (*this)(v.WID, v.lastTime); }
    void field(PilotAlarm& v) { (*this)(v.code, v.numTriggers, v.trigger); }
    void field(TargetPriority& v) { (*this)(v.type, v.params); }
    void field(TargetPriorityList& v) { (*this)(v.size, v.list); }
    void field(WayPath& v) { (*this)(v.numPoints, v.curPoint, v.points, v.mode); }
    void field(TacOrderMoveParams& v) {
        (*this)(v.wayPath, v.faceObject, v.wait, v.mode, v.escapeTile, v.jump,
            v.fromArea, v.keepMoving);
    }
    void field(TacOrderAttackParams& v) {
        (*this)(v.type, v.method, v.range, v.tactic, v.aimLocation, v.pursue,
            v.obliterate, v.targetPoint);
    }
    void field(TacticalOrder& v) {
        (*this)(v.id, v.time, v.delayedTime, v.lastTime, v.unitOrder, v.subOrder,
            v.origin, v.code, v.moveParams, v.attackParams, v.targetWID,
            v.targetObjectClass, v.selectionIndex, v.stage, v.statusCode,
            v.lastMoveGoal, v.pointLocalMoverId, v.groupFlags, v.data);
    }
    void field(QueuedTacOrder& v) {
        (*this)(v.id, v.point, v.packedData, v.tactic, v.moveMode);
        // derived marker: copyFromData rebuilds the owned waypoint appearance
        // from moveMode/point. Never persist a process pointer.
        if (a.reading()) v.marker = nullptr;
    }
    void field(GlobalPathStep& v) {
        (*this)(v.startDoor, v.thruArea, v.goalDoor, v.start, v.goal,
            v.goalCell, v.costToGoal);
    }
    void field(PathStep& v) {
        (*this)(v.cell, v.distanceToGoal, v.destination, v.direction, v.area);
    }
    void field(MovePath& v) {
        (*this)(v.goal, v.target, v.numSteps, v.numStepsWhenNotPaused, v.curStep,
            v.cost, v.stepList, v.marked, v.globalStep);
    }
    void field(SaveableMoveOrders& v) {
        (*this)(v.time, v.origin, v.speedType, v.speedVelocity, v.speedState,
            v.speedThrottle, v.goalType, v.goalObjectWID, v.goalObjectPosition,
            v.goalLocation, v.nextUpdate, v.newGoal, v.wayPath, v.numWayPts,
            v.curWayPt, v.curWayDir, v.pathType, v.originalGlobalGoal,
            v.globalGoalLocation, v.globalPath, v.numGlobalSteps, v.curGlobalStep,
            v.path, v.timeOfLastStep, v.moveState, v.moveStateGoal, v.twisting,
            v.yieldTime, v.yieldState, v.waitForPointTime, v.run);
    }
    void field(AttackOrders& v) {
        (*this)(v.time, v.origin, v.type, v.targetWID, v.targetPoint,
            v.aimLocation, v.pursue, v.tactic, v.targetTime);
    }
    void field(SituationOrders& v) {
        (*this)(v.time, v.mode, v.defFormation, v.curFormation, v.openFire,
            v.uponFireOnly, v.fireRange, v.fireOdds, v.guardObjectWID);
    }
    void identity(MechWarriorData& v) {
        (*this)(v.used, v.name, v.callsign, v.videoStr, v.audioStr, v.brainStr,
            v.index, v.paintScheme, v.photoIndex, v.rank, v.skills,
            v.professionalism, v.professionalismModifier, v.decorum,
            v.decorumModifier, v.aggressiveness, v.courage, v.baseCourage,
            v.wounds, v.health, v.status, v.escapesThruEjection, v.radioLog,
            v.notMineYet, v.teamId, v.vehicleWID, v.numSkillUses,
            v.numSkillSuccesses, v.numMechKills, v.numPhysicalAttacks,
            v.skillRank, v.skillPoints, v.originalSkills, v.startingSkills,
            v.specialtySkills, v.killed, v.numKilled, v.descID, v.nameIndex);
    }
    void orders(MechWarriorData& v) {
        (*this)(v.useGoalPlan, v.mainGoalAction, v.mainGoalObjectWID,
            v.mainGoalLocation, v.mainGoalControlRadius, v.lastGoalPathSize,
            v.lastGoalPath, v.newTacOrderReceived, v.tacOrder, v.lastTacOrder,
            v.curTacOrder, v.alarm, v.alarmHistory, v.alarmPriority,
            v.curPlayerOrderFromQueue, v.tacOrderQueueLocked,
            v.tacOrderQueueExecuting, v.tacOrderQueueLooping,
            v.numTacOrdersQueued, v.tacOrderQueue, v.tacOrderQueueIndex,
            v.nextTacOrderId, v.lastTacOrderId, v.moveOrders);
    }
    void combat(MechWarriorData& v) {
        (*this)(v.timeOfLastOrders, v.attackers, v.numAttackers, v.attackRadius,
            v.weaponsStatus, v.weaponsStatusResult, v.coreScanTargetWID,
            v.coreAttackTargetWID, v.coreMoveTargetWID, v.targetPriorityList,
            v.willHelp, v.attackOrders, v.situationOrders, v.lastTargetWID,
            v.lastTargetTime, v.lastTargetObliterate, v.lastTargetFriendly,
            v.lastTargetConserveAmmo, v.keepMoving, v.orderState);
    }
    void brain(MechWarriorData& v) {
        (*this)(v.memory, v.debugStrings, v.brainUpdate, v.combatUpdate,
            v.movementUpdate, v.brainState, v.debugFlags, v.oldPilot,
            v.warriorBrainHandle);
    }
};

// Derived storage, not additional simulation state. Each inactive array tail
// rebuilds as zero; its count remains encoded. Paused paths retain all
// numStepsWhenNotPaused steps. Wide global paths remain owned by WQT2.
inline bool canonicalizeWarrior(MechWarriorData& v) {
    auto tail = [](auto& array, int count) {
        if (count < 0 || count > static_cast<int>(std::size(array))) return false;
        std::fill(std::begin(array) + count, std::end(array), std::remove_reference_t<decltype(array[0])>{});
        return true;
    };
    auto order = [&](TacticalOrder& t) {
        const int n = t.moveParams.wayPath.numPoints;
        if (!tail(t.moveParams.wayPath.points, n * 3) || !tail(t.moveParams.wayPath.mode, n)) return false;
        // derived lastMoveGoal: no runtime readers; deterministic zero sentinel.
        t.lastMoveGoal = glm::vec3(0);
        // derived network packing cache: pack() recreates it from the order.
        t.data[0] = t.data[1] = 0;
        return true;
    };
    if (!tail(v.lastGoalPath, v.lastGoalPathSize) ||
        !tail(v.moveOrders.globalPath, v.moveOrders.numGlobalSteps) ||
        !tail(v.moveOrders.wayPath, v.moveOrders.numWayPts) ||
        !tail(v.targetPriorityList.list, v.targetPriorityList.size)) return false;
    for (auto& t : v.tacOrder) if (!order(t)) return false;
    if (!order(v.lastTacOrder) || !order(v.curTacOrder)) return false;
    for (auto& p : v.moveOrders.path) {
        if (p.numSteps < 0 || p.numStepsWhenNotPaused < p.numSteps ||
            !tail(p.stepList, p.numStepsWhenNotPaused)) return false;
    }
    // derived unused queue slots: no markers or orders beyond the queue count.
    if (!tail(v.tacOrderQueue, v.numTacOrdersQueued)) return false;
    for (auto& q : v.tacOrderQueue) q.marker = nullptr;
    return true;
}

// Non-owning view avoids TacticalOrder::operator=, which deliberately resets
// execution fields. Decode directly into caller-owned staging, then init().
struct WarriorRecord {
    MechWarriorData& value;
    SavedQueueTargets& targets;
    void serialize(Archive& a) {
        WarriorCodec fields(a);
        fields.identity(value);
        fields.orders(value);
        fields.combat(value);
        fields.brain(value);
        a(targets);
    }
};
inline bool decodeWarriorRecord(std::span<const std::uint8_t> bytes,
                                MechWarriorData& value, SavedQueueTargets& targets) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0;
    WarriorRecord record{value, targets};
    a(tag, version, record);
    return a.finish() && tag == warriorDataTag && version == 1 && canonicalizeWarrior(value) &&
        targets.targets.size() == static_cast<size_t>(value.numTacOrdersQueued);
}

// Runtime-only derived fields outside MechWarriorData (owner ruling):
// movePathRequest -> nullptr in MechWarrior::init(data); future requests are
// re-enqueued by the normal fixed-tick path update in pilot order.
// lastFailedJumpGoalCellR/C and lastJumpFailFrame -> -1 in init(false), so
// no old failure suppresses a fresh deterministic path solve.
// Sensor contacts -> loadActivateAndFinalize scans each restored team's
// battlefield before resumed ABL, preserving enemyInLOS during the rebuild.
// Music -> loadActivateAndFinalize calls playABLDigitalMusic(missionTuneNum)
// before resumed ABL can observe the script music clock.
// speedThrottle is retained explicitly above, although deriving it is allowed.
}
