#include "save_field_probe.h"
#include "warrior_pilot_codec.h"

namespace {
// Independent declaration census. Structured binding arity catches additions
// even when they fit inside old padding. Never generate this from the codec.
#define CENSUS(T, ...) auto census(T& value) { \
    auto& [__VA_ARGS__] = value; return std::tie(__VA_ARGS__); }
CENSUS(RadioLog, lastUnderFire, weaponsIneffective, weaponsOut, lastContact,
    lastMessageType, lastMessage, lastMessageTime)
CENSUS(AttackerRec, WID, lastTime)
CENSUS(PilotAlarm, code, numTriggers, trigger)
CENSUS(TargetPriority, type, params)
CENSUS(TargetPriorityList, size, list)
CENSUS(WayPath, numPoints, curPoint, points, mode)
CENSUS(TacOrderMoveParams, wayPath, faceObject, wait, mode, escapeTile, jump,
    fromArea, keepMoving)
CENSUS(TacOrderAttackParams, type, method, range, tactic, aimLocation, pursue,
    obliterate, alpha, targetPoint)
CENSUS(TacticalOrder, id, time, delayedTime, lastTime, unitOrder, subOrder,
    origin, code, moveParams, attackParams, targetWID, targetObjectClass,
    selectionIndex, stage, statusCode, lastMoveGoal, pointLocalMoverId, groupFlags, data)
CENSUS(QueuedTacOrder, id, point, packedData, tactic, marker, moveMode)
CENSUS(PathStep, cell, distanceToGoal, destination, direction, area)
CENSUS(MovePath, goal, target, numSteps, numStepsWhenNotPaused, curStep, cost,
    stepList, marked, globalStep)
CENSUS(GlobalPathStep, startDoor, thruArea, goalDoor, start, goal, goalCell, costToGoal)
CENSUS(SaveableMoveOrders, time, origin, speedType, speedVelocity, speedState,
    speedThrottle, goalType, goalObjectWID, goalObjectPosition, goalLocation,
    nextUpdate, newGoal, wayPath, numWayPts, curWayPt, curWayDir, pathType,
    originalGlobalGoal, globalGoalLocation, globalPath, numGlobalSteps,
    curGlobalStep, path, timeOfLastStep, moveState, moveStateGoal, twisting,
    yieldTime, yieldState, waitForPointTime, run)
CENSUS(AttackOrders, time, origin, type, targetWID, targetPoint, aimLocation,
    pursue, tactic, targetTime)
CENSUS(SituationOrders, time, mode, defFormation, curFormation, openFire,
    uponFireOnly, fireRange, fireOdds, guardObjectWID)
CENSUS(MechWarriorData, used, name, callsign, videoStr, audioStr, brainStr,
    index, paintScheme, photoIndex, rank, skills, professionalism,
    professionalismModifier, decorum, decorumModifier, aggressiveness, courage,
    baseCourage, wounds, health, status, escapesThruEjection, radioLog, notMineYet,
    teamId, vehicleWID, numSkillUses, numSkillSuccesses, numMechKills,
    numPhysicalAttacks, skillRank, skillPoints, originalSkills, startingSkills,
    specialtySkills, killed, numKilled, descID, nameIndex, timeOfLastOrders,
    attackers, numAttackers, attackRadius, memory, debugStrings, brainUpdate,
    combatUpdate, movementUpdate, weaponsStatus, weaponsStatusResult,
    useGoalPlan, mainGoalAction, mainGoalObjectWID, mainGoalLocation,
    mainGoalControlRadius, lastGoalPathSize, lastGoalPath, newTacOrderReceived,
    tacOrder, lastTacOrder, curTacOrder, alarm, alarmHistory, alarmPriority,
    curPlayerOrderFromQueue, tacOrderQueueLocked, tacOrderQueueExecuting,
    tacOrderQueueLooping, numTacOrdersQueued, tacOrderQueue, tacOrderQueueIndex,
    nextTacOrderId, lastTacOrderId, coreScanTargetWID, coreAttackTargetWID,
    coreMoveTargetWID, targetPriorityList, brainState, willHelp, moveOrders,
    attackOrders, situationOrders, lastTargetWID, lastTargetTime,
    lastTargetObliterate, lastTargetFriendly, lastTargetConserveAmmo, keepMoving,
    orderState, debugFlags, oldPilot, warriorBrainHandle)
#undef CENSUS

template<class T> void pattern(T& v, unsigned& seed) {
    if constexpr (std::is_array_v<T>) for (auto& item : v) pattern(item, seed);
    else if constexpr (requires { census(v); })
        std::apply([&](auto&... fields) { (pattern(fields, seed), ...); }, census(v));
    else if constexpr (std::is_same_v<T, MemoryCell>)
        v = std::bit_cast<MemoryCell>(0x7fc12345u); // union payload is not a numeric float
    else if constexpr (std::is_same_v<T, glm::vec3>) {
        pattern(v.x, seed); pattern(v.y, seed); pattern(v.z, seed);
    } else if constexpr (std::is_pointer_v<T>) v = reinterpret_cast<T>(uintptr_t(1));
    else if constexpr (std::is_enum_v<T> && requires { enum_in_domain(T{}, int32_t{}); }) {
        // Order enums are range-checked on decode (LG14 0fae150de): an out-of-domain pattern value drops the whole order.
        int32_t bits = 1;
        for (int tries = 0; tries < 31; ++tries) {
            bits = static_cast<int32_t>(1 + seed++ % 31);
            if (enum_in_domain(T{}, bits)) break;
        }
        v = static_cast<T>(bits);
    }
    else v = static_cast<T>(1 + seed++ % 31);
}
template<class T> void clear(T& v) {
    if constexpr (std::is_array_v<T>) for (auto& item : v) clear(item);
    else if constexpr (requires { census(v); })
        std::apply([](auto&... fields) { (clear(fields), ...); }, census(v));
    else if constexpr (std::is_same_v<T, MemoryCell>) v.integer = 0;
    else v = T{};
}
template<class T> bool same(T& a, T& b) {
    if constexpr (std::is_array_v<T>) {
        for (size_t i=0; i<std::size(a); ++i) if (!same(a[i], b[i])) return false;
        return true;
    } else if constexpr (requires { census(a); }) {
        auto left = census(a), right = census(b);
        return [&]<size_t... I>(std::index_sequence<I...>) {
            return (same(std::get<I>(left), std::get<I>(right)) && ...);
        }(std::make_index_sequence<std::tuple_size_v<decltype(left)>>{});
    } else if constexpr (std::is_same_v<T, MemoryCell>)
        return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
    else return a == b;
}
void fillAll(MechWarriorData& v) {
    unsigned seed = 1;
    pattern(v, seed);
    v.lastGoalPathSize = MAX_SAVED_GLOBAL_PATH;
    v.moveOrders.numGlobalSteps = MAX_SAVED_GLOBAL_PATH;
    v.moveOrders.numWayPts = MAX_WAYPTS;
    v.targetPriorityList.size = MAX_TARGET_PRIORITIES;
    v.numTacOrdersQueued = MAX_QUEUED_TACORDERS_PER_WARRIOR;
    for (auto& p : v.moveOrders.path) p.numSteps = p.numStepsWhenNotPaused = MAX_STEPS_PER_MOVEPATH;
    for (auto& t : v.tacOrder) t.moveParams.wayPath.numPoints = MAX_WAYPTS;
    v.lastTacOrder.moveParams.wayPath.numPoints = MAX_WAYPTS;
    v.curTacOrder.moveParams.wayPath.numPoints = MAX_WAYPTS;
    for (auto& t : v.tacOrder) { t.attackParams.method = ATTACKMETHOD_RANGED; t.attackParams.alpha = true; }
    v.lastTacOrder.attackParams.method = v.curTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
    v.lastTacOrder.attackParams.alpha = v.curTacOrder.attackParams.alpha = true;
}
}

TEST_CASE("save fields: MechWarriorData independent recursive census") {
    MechWarriorData sample{}, decoded{};
    fillAll(sample);
    REQUIRE(mc2::save::canonicalizeWarrior(sample));
    clear(decoded); // independent zeroing prevents default values hiding omissions
    SavedQueueTargets targets, restored;
    targets.targets.assign(MAX_QUEUED_TACORDERS_PER_WARRIOR, 19);
    auto bytes = mc2::save::encodeTaggedRecord(mc2::save::WarriorRecord{sample, targets}, mc2::save::warriorDataTag);
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(mc2::save::decodeWarriorRecord(bytes, decoded, restored));
    CHECK(same(sample, decoded));
    CHECK(restored.targets == targets.targets);
    // A missing scalar, nested scalar, union or array element must be caught.
    decoded.health = 0; CHECK_FALSE(same(sample, decoded)); decoded.health = sample.health;
    decoded.tacOrder[1].statusCode = 0; CHECK_FALSE(same(sample, decoded));
    decoded.tacOrder[1].statusCode = sample.tacOrder[1].statusCode;
    decoded.moveOrders.path[1].stepList[199].area = 0; CHECK_FALSE(same(sample, decoded));
    decoded.moveOrders.path[1].stepList[199].area = sample.moveOrders.path[1].stepList[199].area;
    decoded.memory[59].integer = 0; CHECK_FALSE(same(sample, decoded));
    bytes.pop_back();
    CHECK_FALSE(mc2::save::decodeWarriorRecord(bytes, decoded, restored));
}

TEST_CASE("save fields: warrior derived tails are deterministic and active paused steps survive") {
    MechWarriorData sample{}, restored{};
    fillAll(sample);
    sample.moveOrders.path[0].numSteps = 0; // paused, not empty
    sample.moveOrders.path[0].numStepsWhenNotPaused = 2;
    sample.curTacOrder.moveParams.wayPath.numPoints = 1;
    sample.numTacOrdersQueued = 1;
    REQUIRE(mc2::save::canonicalizeWarrior(sample));
    CHECK(sample.moveOrders.path[0].stepList[1].area != 0);
    CHECK(sample.moveOrders.path[0].stepList[2].area == 0);
    CHECK(sample.curTacOrder.moveParams.wayPath.points[3] == 0);
    CHECK(sample.tacOrderQueue[1].packedData[0] == 0);
    CHECK(sample.tacOrderQueue[0].marker == nullptr);
    SavedQueueTargets targets, decodedTargets;
    targets.targets = {77};
    const auto bytes = mc2::save::encodeTaggedRecord(mc2::save::WarriorRecord{sample, targets}, mc2::save::warriorDataTag);
    REQUIRE(mc2::save::decodeWarriorRecord(bytes, restored, decodedTargets));
    CHECK(same(sample, restored));
    sample.moveOrders.path[0].numStepsWhenNotPaused = 201;
    CHECK_FALSE(mc2::save::canonicalizeWarrior(sample));
    sample.moveOrders.path[0].numStepsWhenNotPaused = 2;
    sample.curTacOrder.moveParams.wayPath.numPoints = std::numeric_limits<long>::max();
    CHECK_FALSE(mc2::save::canonicalizeWarrior(sample));
}

TEST_CASE("save fields: an out-of-domain order enum drops only that order (LG14)") {
    MechWarriorData sample{}, decoded{};
    fillAll(sample);
    REQUIRE(mc2::save::canonicalizeWarrior(sample));
    SavedQueueTargets targets, restored;
    targets.targets.assign(MAX_QUEUED_TACORDERS_PER_WARRIOR, 19);
    sample.tacOrder[1].code = static_cast<TacticalOrderCode>(9999);
    const auto bytes = mc2::save::encodeTaggedRecord(mc2::save::WarriorRecord{sample, targets}, mc2::save::warriorDataTag);
    REQUIRE_FALSE(bytes.empty());
    clear(decoded);
    int dropped = -1;
    REQUIRE(mc2::save::decodeWarriorRecord(bytes, decoded, restored, &dropped));
    CHECK(dropped == 1);
    CHECK(decoded.tacOrder[1].code == TACTICAL_ORDER_NONE);   // the bad order was reset by TacticalOrder::init()
    CHECK(decoded.tacOrder[1].id == 0);
    CHECK(decoded.tacOrder[1].time == -1.0);
    CHECK(same(decoded.tacOrder[0], sample.tacOrder[0]));      // its neighbours survive
    CHECK(same(decoded.tacOrder[2], sample.tacOrder[2]));
}

TEST_CASE("save fields: an order enum outside a TacticalOrder rejects the record (warrior_enum_domain)") {
    // No MechWarriorData field outside the orders is a domain enum today, so the guard is driven through the codec directly.
    mc2::save::Archive writer;
    std::int32_t outOfDomain = 99;
    writer(outOfDomain);
    const auto bytes = writer.bytes();
    mc2::save::Archive reader(bytes);
    mc2::save::WarriorCodec codec(reader);
    AttackType type = ATTACK_TO_DESTROY;
    codec.field(type);
    CHECK_FALSE(reader.good());
    CHECK(type == ATTACK_TO_DESTROY);   // an unnamed enumerator is never produced
    CHECK(codec.droppedOrders == 0);
}
