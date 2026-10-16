#include "doctest.h"
#include "combat_retirement.h"

using namespace mc2::combat;

TEST_CASE("CombatRetirement/ceil preserves sub-millisecond and fractional tails") {
    CHECK(secondsToTicks(0) == 0);
    CHECK(secondsToTicks(0.0001) == 1);
    CHECK(secondsToTicks(0.5) == 500);
    CHECK(secondsToTicks(1.0 / 3) == 334);
}

TEST_CASE("CombatRetirement/invalid durations cannot clamp or wrap") {
    for (const auto seconds : {std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(), -0.01, 2e16})
        CHECK_FALSE(secondsToTicks(seconds));
}

TEST_CASE("CombatRetirement/fractional accumulation prevents 33ms loss") {
    for (const auto hz : {30, 60}) {
        CombatClock clock;
        for (int i = 0; i < hz; ++i)
            REQUIRE(clock.advance(1.0f / hz) == ClockStep::advanced);
        CHECK(clock.ticks == 1000);
        CHECK(clock.frac >= 0);
        CHECK(clock.frac < 1);
    }
    CombatClock clock;
    for (const auto step : {0.00025, 0.0015, 0.00025}) clock.advance(step);
    CHECK(clock.ticks == 2);
    CHECK(clock.frac == doctest::Approx(0));
    CHECK(clock.advance(0) == ClockStep::advanced);
    CHECK(clock.ticks == 2);
}

TEST_CASE("CombatRetirement/rejected steps leave both clock components intact") {
    CombatClock clock{7, 0.25};
    for (const auto step : {-1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN(), 2e16}) {
        CHECK(clock.advance(step) != ClockStep::advanced);
        CHECK(clock.ticks == 7);
        CHECK(clock.frac == 0.25);
    }
}

TEST_CASE("CombatRetirement/reserved maximum rejects clock and arm overflow") {
    CombatClock clock{invalidTick - 2, 0.5};
    CHECK(clock.advance(0.002) == ClockStep::clock_overflow);
    CHECK(clock.ticks == invalidTick - 2);
    CHECK(clock.frac == 0.5);
    RetireDeadline deadline;
    CHECK(deadline.arm(invalidTick - 2, {2, "AfterHitTime"}) == ArmResult::deadline_overflow);
    CHECK_FALSE(deadline.armed);
    CHECK(addTicks(invalidTick - 2, 1) == invalidTick - 1);
    CHECK_FALSE(addTicks(invalidTick, 0));
}

TEST_CASE("CombatRetirement/unarmed differs from an armed zero deadline") {
    RetireDeadline deadline;
    CHECK_FALSE(deadline.armed);
    REQUIRE(deadline.arm(0, {}) == ArmResult::armed);
    CHECK(deadline.armed);
    CHECK(deadline.tick == 0);
}

TEST_CASE("CombatRetirement/repeating impacts cannot rearm and reuse clears state") {
    RetireDeadline deadline;
    REQUIRE(deadline.arm(10, {500, "AfterHitTime"}) == ArmResult::armed);
    CHECK(deadline.arm(100, {}) == ArmResult::already_attempted);
    CHECK(deadline.tick == 510);
    deadline = {};
    CHECK_FALSE(deadline.armed);
    CHECK_FALSE(deadline.attempted);
    CHECK(deadline.arm(100, {std::nullopt, "AfterHitTime"}) == ArmResult::policy);
    CHECK_FALSE(deadline.armed);
    CHECK(deadline.arm(100, {}) == ArmResult::already_attempted);
}
