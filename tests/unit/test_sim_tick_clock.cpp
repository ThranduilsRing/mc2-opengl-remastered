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
    for (double fps : {5.0, 20.0, 30.0, 60.0, 75.0, 144.0, 165.0, 240.0, 1000.0})
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

TEST_CASE("tick clock: only debt beyond one second is dropped") {
    TickClock c;
    CHECK(c.advance(5.0, 1.0) == 30);
    CHECK(c.advance(1.0 / 144.0, 1.0) == 0);
    TickClock f;
    CHECK(f.advance(0.25, 4.0) == 30);
    CHECK(mc2::sim::maxTicksPerFrame(4.0) == 120);
    CHECK(mc2::sim::maxTicksPerFrame(0.25) == 30);
}

TEST_CASE("tick clock: periodic 200 ms stalls preserve elapsed sim time") {
    TickClock c;
    int ticks = 0;
    for (int second = 0; second < 10; ++second) {
        ticks += c.advance(0.2, 1.0);
        for (int frame = 0; frame < 48; ++frame) ticks += c.advance(1.0 / 60.0, 1.0);
    }
    CHECK(ticks == 300);
}

TEST_CASE("tick clock: a 20 ms tick yields and retains the remaining debt") {
    TickClock c;
    const int reserved = c.advance(0.2 + kTickSeconds / 2, 1.0);
    CHECK(reserved == 6);
    CHECK_FALSE(mc2::sim::catchUpSpent(0.007));
    CHECK(mc2::sim::catchUpSpent(0.008));
    CHECK(mc2::sim::catchUpSpent(0.020));
    c.defer(reserved - 1);
    CHECK(c.alpha() == 1.0);
    CHECK(c.advance(kTickSeconds, 1.0) == 6);
    CHECK(c.alpha() == doctest::Approx(0.5));
    c.defer(30);
    CHECK(c.advance(0.2, 1.0) == 30); // accumulating debt also has a one-second ceiling
}

TEST_CASE("tick clock: degenerate input runs nothing") {
    TickClock c;
    CHECK(c.advance(0.0, 1.0) == 0);
    CHECK(c.advance(-1.0, 1.0) == 0);
    CHECK(c.advance(0.1, 0.0) == 0);
    CHECK(c.advance(0.1, std::numeric_limits<double>::quiet_NaN()) == 0);
}
