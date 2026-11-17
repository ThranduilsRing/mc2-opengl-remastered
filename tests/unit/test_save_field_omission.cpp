#include "save_field_probe.h"
#include "unit_anim_state.h"
#include "tactical_order_queue.h"
#include "dteam.h"
#include "mech_value_codec.h"

TEST_CASE("save fields: heat extension independent member census and omission witnesses") {
    using T = mc2::heat::State;
    const auto sample = save_probe::patterned<T>(&T::heatMilli, &T::alphaMask, &T::alphaExpireTick);
    auto codec = [](auto& a, auto& v) { mc2::save::heatFields(a, v); };
    CHECK(save_probe::roundtrip(sample, codec, &T::heatMilli, &T::alphaMask, &T::alphaExpireTick));
    auto omit = [&](auto member) {
        return [=](auto& a, auto& v) {
            codec(a, v);
            if (a.reading()) save_probe::zero(v.*member);
        };
    };
    CHECK_FALSE(save_probe::roundtrip(sample, omit(&T::heatMilli), &T::heatMilli));
    CHECK_FALSE(save_probe::roundtrip(sample, omit(&T::alphaMask), &T::alphaMask));
    CHECK_FALSE(save_probe::roundtrip(sample, omit(&T::alphaExpireTick), &T::alphaExpireTick));
}

namespace mc2clip {
bool operator==(const Phase& a, const Phase& b) {
    return std::tie(a.prevGesture, a.jumpStarted, a.landing) ==
           std::tie(b.prevGesture, b.jumpStarted, b.landing);
}
}
namespace mc2::sim {
bool operator==(const ImportedAnimState& a, const ImportedAnimState& b) {
    return std::tie(a.activeClip, a.pendingClip, a.pendingStreak, a.turnStreak, a.havePrev,
        a.prevPx, a.prevPy, a.prevPz, a.prevHeading, a.clipTimeSec, a.durationSec,
        a.lastFrame, a.phase, a.mode, a.locomotion) ==
        std::tie(b.activeClip, b.pendingClip, b.pendingStreak, b.turnStreak, b.havePrev,
        b.prevPx, b.prevPy, b.prevPz, b.prevHeading, b.clipTimeSec, b.durationSec,
        b.lastFrame, b.phase, b.mode, b.locomotion);
}
}

#define IMPORTED_MEMBERS &T::activeClip, &T::pendingClip, &T::pendingStreak, &T::turnStreak, \
    &T::havePrev, &T::prevPx, &T::prevPy, &T::prevPz, &T::prevHeading, &T::clipTimeSec, \
    &T::durationSec, &T::lastFrame, &T::phase, &T::mode, &T::locomotion, &T::previousClip, \
    &T::previousClipTimeSec, &T::fadeTimeSec, &T::playRate, &T::previousPlayRate, \
    &T::previousDurationSec, &T::previousOnce, &T::fadeSourceLocals
#define ANIM_MEMBERS &T::mechFrameRate, &T::leftArmOff, &T::rightArmOff, &T::fallen, \
    &T::forceStop, &T::atTransitionToNextGesture, &T::inReverse, &T::inJump, &T::jumpSetup, \
    &T::jumpAirborne, &T::oncePerFrame, &T::lockRotation, &T::velocity, &T::currentStateGoal, \
    &T::currentGestureId, &T::transitionState, &T::oldStateGoal, &T::hitGestureId, &T::currentFrame, \
    &T::lastAnimAdvanceFrame, &T::idleTime, &T::jumpDestination, &T::jumpVelocity, &T::nodeUsed, \
    &T::nodeRecycle, &T::inCombatMode, &T::torsoRotation, &T::turretRotation, &T::pitch, &T::roll, \
    &T::velocityMagnitude, &T::gvAnimationState, &T::gvFrameRate, &T::isReversed, &T::isLooping, \
    &T::setFirstFrame, &T::canTransition, &T::bdAnimationState, &T::bdFrameRate, &T::withdrawSeconds, \
    &T::smokeLevel, &T::wakeRequested, &T::activityEffect, &T::activityLoop, &T::ejectionSerial, \
    &T::destructionSerial, &T::imported

namespace save_probe {
template<> inline void pattern(std::array<float, 16>& value, unsigned char seed) {
    for (auto& element : value) pattern(element, seed);
}
template<> inline void pattern(mc2clip::Phase& value, unsigned char seed) {
    value = {int(seed), true, true};
}
template<> inline void pattern(mc2::sim::ImportedAnimState& value, unsigned char) {
    using T = mc2::sim::ImportedAnimState;
    value = patterned<T>(IMPORTED_MEMBERS);
}
template<> inline void zero(mc2::sim::ImportedAnimState& value) {
    value = {};
    value.durationSec = 0;
    value.lastFrame = 0;
    value.phase = {0, false, false};
}
template<> inline void zero(mc2::sim::TacticalOrder& value) {
    value = {};
    value.kind = mc2::sim::OrderKind::None;
}
template<> inline void zero(mc2::sim::WayPath& value) {
    value.points.clear(); value.curPoint = 0; value.mode.clear();
}
}

TEST_CASE("save fields: nonzero defaults cannot hide a dropped field") {
    struct Defaults { uint32_t first = 7, second = 9; };
    CHECK_FALSE(save_probe::roundtrip(Defaults{1, 9}, [](auto& a, auto& v) { a(v.first); },
        &Defaults::first, &Defaults::second));
    CHECK_FALSE(save_probe::roundtrip(Defaults{1, 9}, [](auto& a, auto& v) { a(v.first); v.second = 0; },
        &Defaults::first, &Defaults::second));
}

TEST_CASE("save fields: ANIM independent member census") {
    using namespace mc2::sim;
    ImportedAnimState imported;
    {
        using T = ImportedAnimState;
        imported = save_probe::patterned<T>(IMPORTED_MEMBERS);
        SAVE_FIELD_SHAPE(imported, activeClip, pendingClip, pendingStreak, turnStreak, havePrev,
            prevPx, prevPy, prevPz, prevHeading, clipTimeSec, durationSec, lastFrame, phase, mode, locomotion,
            previousClip, previousClipTimeSec, fadeTimeSec, playRate, previousPlayRate, previousDurationSec, previousOnce, fadeSourceLocals);
        SAVE_FIELD_SHAPE(imported.phase, prevGesture, jumpStarted, landing);
        imported.phase = {0x15151515, true, true};
        CHECK(save_probe::memberBytes<T>(IMPORTED_MEMBERS) + 9 == sizeof(T));
        CHECK(save_probe::roundtrip(imported, [](auto& a, auto& v) { a(v); }, IMPORTED_MEMBERS));
    }
    using T = UnitAnimState;
    auto sample = save_probe::patterned<T>(ANIM_MEMBERS);
    sample.imported = imported;
    SAVE_FIELD_SHAPE(sample, mechFrameRate, leftArmOff, rightArmOff, fallen, forceStop,
        atTransitionToNextGesture, inReverse, inJump, jumpSetup, jumpAirborne, oncePerFrame,
        lockRotation, velocity, currentStateGoal, currentGestureId, transitionState, oldStateGoal,
        hitGestureId, currentFrame, lastAnimAdvanceFrame, idleTime, jumpDestination, jumpVelocity,
        nodeUsed, nodeRecycle, inCombatMode, torsoRotation, turretRotation, pitch, roll,
        velocityMagnitude, gvAnimationState, gvFrameRate, isReversed, isLooping, setFirstFrame,
        canTransition, bdAnimationState, bdFrameRate, withdrawSeconds, smokeLevel, wakeRequested,
        activityEffect, activityLoop, ejectionSerial, destructionSerial, imported);
    // Fixed-width animation words give both 64-bit hosts the Windows layout.
    constexpr size_t animPadding = 14;
    CHECK(save_probe::memberBytes<T>(ANIM_MEMBERS) + animPadding == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, ANIM_MEMBERS));
}

TEST_CASE("save fields: ORDQ execution independent member census") {
    using namespace mc2::sim;
    using T = OrderExecution;
    auto sample = save_probe::patterned<T>(&T::issuedTick, &T::notBeforeTick, &T::lastUpdateTick,
        &T::stage, &T::status, &T::lastMoveGoal);
    sample.payload.orderId = 0x0101010101010101ull;
    sample.payload.kind = OrderKind::Stop;
    sample.payload.unitOrder = sample.payload.subOrder = true;
    sample.payload.formationOrdinal = 0x02020202;
    sample.payload.groupFlags = 0x03030303;
    sample.pointUnit = ObjectRef{0x04040404, 0x05050505};
    SAVE_FIELD_SHAPE(sample, payload, issuedTick, notBeforeTick, lastUpdateTick, stage, status, lastMoveGoal, pointUnit);
    CHECK(save_probe::memberBytes<T>(&T::payload, &T::issuedTick, &T::notBeforeTick, &T::lastUpdateTick,
        &T::stage, &T::status, &T::lastMoveGoal, &T::pointUnit) + 0 == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::payload,
        &T::issuedTick, &T::notBeforeTick, &T::lastUpdateTick, &T::stage, &T::status,
        &T::lastMoveGoal, &T::pointUnit));
}

TEST_CASE("save fields: team roster tail retains every watch ID") {
    // This tail is one bounded vector, with no native struct or padding.
    // Team::Save/Load use this same Archive sequence primitive after TeamData.
    std::vector<uint32_t> roster(128);
    for (size_t i = 0; i < roster.size(); ++i) roster[i] = uint32_t(i + 1) * 0x010101;
    mc2::save::Archive writer;
    writer.sequence(roster, MAX_MOVERS_PER_TEAM);
    REQUIRE(writer.good());
    std::vector<uint32_t> decoded;
    mc2::save::Archive reader(writer.bytes());
    reader.sequence(decoded, MAX_MOVERS_PER_TEAM);
    REQUIRE(reader.finish());
    CHECK(decoded == roster);
    CHECK(writer.bytes().size() == sizeof(uint32_t) * (1 + roster.size()));
}

TEST_CASE("save fields: ORDQ nested payloads independent member census") {
    using namespace mc2::sim;
    mc2::sim::WayPath path;
    {
        using T = mc2::sim::WayPath;
        path = save_probe::patterned<T>(&T::points, &T::curPoint, &T::mode);
        path.curPoint = 1;
        SAVE_FIELD_SHAPE(path, points, curPoint, mode);
        CHECK(save_probe::memberBytes<T>(&T::points, &T::curPoint, &T::mode) + 4 == sizeof(T));
        CHECK(save_probe::roundtrip(path, [](auto& a, auto& v) { a(v); }, &T::points, &T::curPoint, &T::mode));
    }
    TacMoveParams move;
    {
        using T = TacMoveParams;
        move = save_probe::patterned<T>(&T::faceObject, &T::wait, &T::mode, &T::escapeTile,
            &T::jump, &T::fromArea, &T::keepMoving);
        move.wayPath = path;
        SAVE_FIELD_SHAPE(move, wayPath, faceObject, wait, mode, escapeTile, jump, fromArea, keepMoving);
        CHECK(save_probe::memberBytes<T>(&T::wayPath, &T::faceObject, &T::wait, &T::mode,
            &T::escapeTile, &T::jump, &T::fromArea, &T::keepMoving) + 11 == sizeof(T));
        CHECK(save_probe::roundtrip(move, [](auto& a, auto& v) { a(v); }, &T::wayPath,
            &T::faceObject, &T::wait, &T::mode, &T::escapeTile, &T::jump, &T::fromArea, &T::keepMoving));
    }
    TacAttackParams attack;
    {
        using T = TacAttackParams;
        attack = save_probe::patterned<T>(&T::type, &T::method, &T::range, &T::tactic,
            &T::aimLocation, &T::pursue, &T::obliterate, &T::alpha, &T::targetPoint);
        attack.method = mc2::sim::AttackMethod::Ranged;
        SAVE_FIELD_SHAPE(attack, type, method, range, tactic, aimLocation, pursue, obliterate, alpha, targetPoint);
        CHECK(save_probe::memberBytes<T>(&T::type, &T::method, &T::range, &T::tactic,
            &T::aimLocation, &T::pursue, &T::obliterate, &T::alpha, &T::targetPoint) + 1 == sizeof(T));
        CHECK(save_probe::roundtrip(attack, [](auto& a, auto& v) { a(v); }, &T::type,
            &T::method, &T::range, &T::tactic, &T::aimLocation, &T::pursue, &T::obliterate, &T::alpha, &T::targetPoint));
    }
    using T = mc2::sim::TacticalOrder;
#define ORDER_MEMBERS &T::orderId, &T::origin, &T::kind, &T::unitOrder, &T::subOrder, \
    &T::move, &T::attack, &T::target, &T::formationOrdinal, &T::groupFlags, &T::support
    auto sample = save_probe::patterned<T>(&T::orderId, &T::origin, &T::unitOrder,
        &T::subOrder, &T::formationOrdinal, &T::groupFlags);
    sample.kind = OrderKind::AttackObject;
    sample.move = move;
    sample.attack = attack;
    sample.target = ObjectRef{0x01010101, 0x02020202};
    SAVE_FIELD_SHAPE(sample, orderId, origin, kind, unitOrder, subOrder, move, attack, target, formationOrdinal, groupFlags, support);
    CHECK(save_probe::memberBytes<T>(ORDER_MEMBERS) + 10 == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, ORDER_MEMBERS));
    sample.kind = OrderKind::Airstrike;
    sample.move.reset(); sample.attack.reset(); sample.target.reset();
    sample.support = AirstrikeCommand{CommanderId{0x03030303}, {1.25f, 2.25f, 3.25f}, StrikeKind::Large};
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, ORDER_MEMBERS));
#undef ORDER_MEMBERS
}

TEST_CASE("save fields: ORDQ leaf shapes reject additions even inside padding") {
    using namespace mc2::sim;
    ObjectRef ref; SAVE_FIELD_SHAPE(ref, slot, generation);
    CommanderId commander; SAVE_FIELD_SHAPE(commander, value);
    InstanceId instance; SAVE_FIELD_SHAPE(instance, value);
    RosterEntryId roster; SAVE_FIELD_SHAPE(roster, value);
    AirstrikeCommand airstrike; SAVE_FIELD_SHAPE(airstrike, commanderId, worldPoint, strikeKind);
    ReinforcementCommand reinforcement; SAVE_FIELD_SHAPE(reinforcement, commanderId, rosterEntryIds, worldPoint);
    ObjectiveArgument objective; SAVE_FIELD_SHAPE(objective, objectiveId, completed);
    ResourceArgument resource; SAVE_FIELD_SHAPE(resource, commanderId, resourcePoints);
    MissionResultArgument result; SAVE_FIELD_SHAPE(result, result);
    MissionControlCommand control; SAVE_FIELD_SHAPE(control, action, arguments);
    OrderSource source; SAVE_FIELD_SHAPE(source, sourceId, kind, instanceId, nextSequence, lastAdmittedSequence);
    OrderEnvelope envelope; SAVE_FIELD_SHAPE(envelope, applyTick, sourceId, sequence, issuer, recipients, queueMode, payload);
    ApplicationOutcome outcome; SAVE_FIELD_SHAPE(outcome, result, reasonCode);
    RecipientResult recipient; SAVE_FIELD_SHAPE(recipient, recipient, ordinal, result, reasonCode);
    OrderReceipt receipt; SAVE_FIELD_SHAPE(receipt, receiptId, sourceId, sequence, applyTick,
        admissionStatus, applicationStatus, results, commandResult, acknowledged);
}
