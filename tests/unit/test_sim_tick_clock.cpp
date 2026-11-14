#include "doctest.h"
#include "../../code/sim_tick_clock.h"

#include <limits>

using mc2::sim::TickClock;
using mc2::sim::kTickSeconds;

namespace {
// Total ticks run over `seconds` of real time at a constant frame rate.
int run(double fps, double seconds, double speed) {
    TickClock c;
    int total = 0;
    const int frames = static_cast<int>(fps * seconds + 0.5);
    for (int i = 0; i < frames; ++i) total += c.advance(1.0 / fps, speed);
    return total;
}
}

TEST_CASE("tick clock: tick count over ten seconds does not depend on frame rate") {
    for (double fps : {10.0, 20.0, 30.0, 60.0, 75.0, 144.0, 165.0, 240.0, 1000.0})
        CHECK(run(fps, 10.0, 1.0) == 300);
}

TEST_CASE("tick clock: game speed scales ticks per second, not tick length") {
    CHECK(run(60.0, 10.0, 2.0) == 600);
    CHECK(run(60.0, 10.0, 4.0) == 1200);
    CHECK(run(144.0, 10.0, 0.5) == 150);
    CHECK(run(144.0, 10.0, 0.25) == 75);
    CHECK(run(30.0, 10.0, 4.0) == 1200);   // 4 ticks per frame, none clamped
}

TEST_CASE("tick clock: exact 30 fps is one tick per frame") {
    TickClock c;
    for (int i = 0; i < 100; ++i) CHECK(c.advance(kTickSeconds, 1.0) == 1);
}

TEST_CASE("tick clock: hitches discard excess debt and immediately resume the normal rate") {
    for (double speed : {1.0, 2.0, 4.0}) {
        for (double hitch : {1.0, 5.0}) {
            TickClock c;
            CHECK(c.advance(kTickSeconds / 2, speed) <= 2); // include pre-existing phase
            CHECK(c.advance(hitch, speed) == static_cast<int>(3 * speed));
            for (int pair = 0; pair < 60; ++pair) {
                const int first = c.advance(1.0 / 60.0, speed);
                const int second = c.advance(1.0 / 60.0, speed);
                CHECK(first + second == static_cast<int>(speed));
                CHECK(first <= static_cast<int>(std::ceil(speed / 2)));
                CHECK(second <= static_cast<int>(std::ceil(speed / 2)));
            }
        }
    }
    CHECK(mc2::sim::maxTicksPerFrame(4.0) == 12);
    CHECK(mc2::sim::maxTicksPerFrame(0.25) == 3);
    CHECK(run(5.0, 10.0, 1.0) == 150); // sustained <10 fps intentionally slows the sim
}

TEST_CASE("tick clock: periodic 200 ms stalls discard excess elapsed time") {
    TickClock c;
    int ticks = 0;
    for (int second = 0; second < 10; ++second) {
        ticks += c.advance(0.2, 1.0);
        for (int frame = 0; frame < 48; ++frame) ticks += c.advance(1.0 / 60.0, 1.0);
    }
    CHECK(ticks == 270);
}

TEST_CASE("tick clock: a 20 ms tick yields and retains the remaining debt") {
    TickClock c;
    const int reserved = c.advance(2 * kTickSeconds + kTickSeconds / 2, 1.0);
    CHECK(reserved == 2);
    CHECK_FALSE(mc2::sim::catchUpSpent(0.007));
    CHECK(mc2::sim::catchUpSpent(0.008));
    CHECK(mc2::sim::catchUpSpent(0.020));
    c.defer(reserved - 1);
    CHECK(c.alpha() == 1.0);
    CHECK(c.advance(kTickSeconds, 1.0) == 2);
    CHECK(c.alpha() == doctest::Approx(0.5));
    c.defer(30);
    CHECK(c.advance(0.2, 1.0) == 3); // deferred debt is also limited to three ticks
}

TEST_CASE("tick clock: degenerate input runs nothing") {
    TickClock c;
    CHECK(c.advance(0.0, 1.0) == 0);
    CHECK(c.advance(-1.0, 1.0) == 0);
    CHECK(c.advance(0.1, 0.0) == 0);
    CHECK(c.advance(0.1, std::numeric_limits<double>::quiet_NaN()) == 0);
}
