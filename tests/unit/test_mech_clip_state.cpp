// tests/unit/test_mech_clip_state.cpp
//
// BT2018-FIRST-CLASS P9: the imported-mech clip state machine (mclib/mech_clip_state.h).
// Pure: gesture ids + measured speed in, clip choice out. Locks the semantics the 1C
// selector got wrong (get-up played the knockdown, hits/falls looped, limps walked, the
// whole jump was the airborne idle) and the stride-rate rule that removes foot sliding.
#include "doctest.h"
#include "mech_clip_state.h"
#include <string>

using namespace mc2clip;

namespace {
Choice pick(int gesture, Phase& ph, float speed = 0.0f, bool destroyed = false, bool finished = false,
            const char* active = "atlas_moveCoreIdle") {
    return select(gesture, speed, 0.0f, 0, destroyed, finished, active, ph);
}
}

TEST_CASE("clip state: get-up plays the get-up clip once, not the knockdown") {
    Phase ph;
    const Choice c = pick(kGestureGetUp, ph);
    CHECK(std::string(c.clip) == "atlas_downedGetup");
    CHECK(c.mode == Mode::Once);
    CHECK(c.immediate);
    CHECK(std::string(pick(kGestureRollover, ph).clip) == "atlas_downedGetup");
}

TEST_CASE("clip state: falls, hits and deaths are one-shot; prone holds the downed or death idle") {
    Phase ph;
    CHECK(pick(kGestureFallForward, ph).mode == Mode::Once);
    CHECK(std::string(pick(kGestureFallForward, ph).clip) == "atlas_hitReactKnockdownInpl");
    CHECK(std::string(pick(kGestureFallBackward, ph, 0, true).clip) == "atlas_deathKnockdown");
    CHECK(pick(kGestureHitLeft, ph).mode == Mode::Once);
    CHECK(std::string(pick(kGestureHitLeft, ph).clip) == "atlas_hitReactLgtLeft");
    CHECK(std::string(pick(kGestureFallenForward, ph).clip) == "atlas_downedIdle");
    CHECK(std::string(pick(kGestureFallenBackward, ph, 0, true).clip) == "atlas_deathIdle");
}

TEST_CASE("clip state: limps use the directional limp clips with stride matching") {
    Phase ph;
    Choice l = pick(kGestureLimpLeft, ph, 6.0f);
    CHECK(std::string(l.clip) == "atlas_moveCoreWalkLimpFwdLeft");
    CHECK(l.locomotion);
    CHECK(std::string(pick(kGestureLimpRight, ph, 6.0f).clip) == "atlas_moveCoreWalkLimpFwdRight");
}

TEST_CASE("clip state: a jump is start -> airborne idle -> land") {
    Phase ph;
    pick(kGestureWalk, ph, 10.0f);
    Choice c = pick(kGestureJump, ph);
    CHECK(std::string(c.clip) == "atlas_moveJumpUpStart");
    CHECK(c.mode == Mode::Once);
    c = pick(kGestureJump, ph, 0, false, false, "atlas_moveJumpUpStart");     // start still playing
    CHECK(std::string(c.clip) == "atlas_moveJumpUpStart");
    c = pick(kGestureJump, ph, 0, false, true, "atlas_moveJumpUpStart");      // start finished
    CHECK(std::string(c.clip) == "atlas_moveJumpUpIdle");
    CHECK(c.mode == Mode::Loop);
    c = pick(2 /*Stand*/, ph, 0, false, false, "atlas_moveJumpUpIdle");        // touched down
    CHECK(std::string(c.clip) == "atlas_moveJumpDownLand");
    CHECK(c.mode == Mode::Once);
    c = pick(2, ph, 0, false, false, "atlas_moveJumpDownLand");                // landing plays out
    CHECK(std::string(c.clip) == "atlas_moveJumpDownLand");
    c = pick(2, ph, 0, false, true, "atlas_moveJumpDownLand");                 // then locomotion
    CHECK(std::string(c.clip) == "atlas_moveCoreIdle");
}

TEST_CASE("clip state: locomotion picks walk/run/reverse and turn-in-place") {
    Phase ph;
    CHECK(std::string(pick(kGestureWalk, ph, 10.0f).clip) == "atlas_moveCoreWalkFwd");
    CHECK(std::string(pick(kGestureRun, ph, 20.0f).clip) == "atlas_moveCoreRunFwd");
    CHECK(std::string(pick(kGestureReverse, ph, 8.0f).clip) == "atlas_moveCoreWalkBwd");
    CHECK_FALSE(pick(kGestureWalk, ph, 10.0f).immediate);
    CHECK(std::string(select(2, 0.0f, 30.0f, 3, false, false, "atlas_moveCoreIdle", ph).clip) == "atlas_moveCoreTurnLeftIdle");
}

TEST_CASE("clip state: stride matching removes planted-foot drift across speeds") {
    const float clipStride = 9.5f;   // planted-foot speed of the walk clip at rate 1
    for (float speed : {4.0f, 8.0f, 9.5f, 12.0f, 16.0f}) {
        CHECK(footDrift(speed, clipStride) == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(strideRate(speed, clipStride) == doctest::Approx(speed / clipStride));
    }
    // Unmatched playback (rate 1) would slide by |speed - stride|; the clamp bounds extremes.
    CHECK(strideRate(0.0f, clipStride) == doctest::Approx(0.35f));
    CHECK(strideRate(100.0f, clipStride) == doctest::Approx(2.0f));
    CHECK(strideRate(5.0f, 0.0f) == doctest::Approx(1.0f));
}
