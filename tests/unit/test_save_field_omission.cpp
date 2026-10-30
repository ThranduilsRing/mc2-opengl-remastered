#include "save_field_probe.h"
#include "unit_anim_state.h"
#include "tactical_order_queue.h"
#include "dteam.h"

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
    &T::durationSec, &T::lastFrame, &T::phase, &T::mode, &T::locomotion
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
template<> inline void pattern(mc2clip::Phase& value, unsigned char seed) {
    value = {int(seed), true, true};
}
template<> inline void pattern(mc2::sim::ImportedAnimState& value, unsigned char) {
    using T = mc2::sim::ImportedAnimState;
    value = patterned<T>(IMPORTED_MEMBERS);
}
}

TEST_CASE("save fields: ANIM independent member census") {
    using namespace mc2::sim;
    ImportedAnimState imported;
    {
        using T = ImportedAnimState;
        imported = save_probe::patterned<T>(IMPORTED_MEMBERS);
        imported.phase = {0x15151515, true, true};
        CHECK(save_probe::memberBytes<T>(IMPORTED_MEMBERS) + 6 == sizeof(T));
        CHECK(save_probe::roundtrip(imported, [](auto& a, auto& v) { a(v); }, IMPORTED_MEMBERS));
    }
    using T = UnitAnimState;
    auto sample = save_probe::patterned<T>(ANIM_MEMBERS);
    sample.imported = imported;
    CHECK(save_probe::memberBytes<T>(ANIM_MEMBERS) + 14 == sizeof(T));
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
    WayPath path;
    {
        using T = WayPath;
        path = save_probe::patterned<T>(&T::points, &T::curPoint, &T::mode);
        path.curPoint = 1;
        CHECK(save_probe::memberBytes<T>(&T::points, &T::curPoint, &T::mode) + 4 == sizeof(T));
        CHECK(save_probe::roundtrip(path, [](auto& a, auto& v) { a(v); }, &T::points, &T::curPoint, &T::mode));
    }
    TacMoveParams move;
    {
        using T = TacMoveParams;
        move = save_probe::patterned<T>(&T::faceObject, &T::wait, &T::mode, &T::escapeTile,
            &T::jump, &T::fromArea, &T::keepMoving);
        move.wayPath = path;
        CHECK(save_probe::memberBytes<T>(&T::wayPath, &T::faceObject, &T::wait, &T::mode,
            &T::escapeTile, &T::jump, &T::fromArea, &T::keepMoving) + 7 == sizeof(T));
        CHECK(save_probe::roundtrip(move, [](auto& a, auto& v) { a(v); }, &T::wayPath,
            &T::faceObject, &T::wait, &T::mode, &T::escapeTile, &T::jump, &T::fromArea, &T::keepMoving));
    }
    TacAttackParams attack;
    {
        using T = TacAttackParams;
        attack = save_probe::patterned<T>(&T::type, &T::method, &T::range, &T::tactic,
            &T::aimLocation, &T::pursue, &T::obliterate, &T::targetPoint);
        CHECK(save_probe::memberBytes<T>(&T::type, &T::method, &T::range, &T::tactic,
            &T::aimLocation, &T::pursue, &T::obliterate, &T::targetPoint) + 2 == sizeof(T));
        CHECK(save_probe::roundtrip(attack, [](auto& a, auto& v) { a(v); }, &T::type,
            &T::method, &T::range, &T::tactic, &T::aimLocation, &T::pursue, &T::obliterate, &T::targetPoint));
    }
    using T = TacticalOrder;
#define ORDER_MEMBERS &T::orderId, &T::origin, &T::kind, &T::unitOrder, &T::subOrder, \
    &T::move, &T::attack, &T::target, &T::formationOrdinal, &T::groupFlags, &T::support
    auto sample = save_probe::patterned<T>(&T::orderId, &T::origin, &T::unitOrder,
        &T::subOrder, &T::formationOrdinal, &T::groupFlags);
    sample.kind = OrderKind::AttackObject;
    sample.move = move;
    sample.attack = attack;
    sample.target = ObjectRef{0x01010101, 0x02020202};
    CHECK(save_probe::memberBytes<T>(ORDER_MEMBERS) + 2 == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, ORDER_MEMBERS));
    sample.kind = OrderKind::Airstrike;
    sample.move.reset(); sample.attack.reset(); sample.target.reset();
    sample.support = AirstrikeCommand{CommanderId{0x03030303}, {1.25f, 2.25f, 3.25f}, StrikeKind::Large};
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, ORDER_MEMBERS));
#undef ORDER_MEMBERS
}
