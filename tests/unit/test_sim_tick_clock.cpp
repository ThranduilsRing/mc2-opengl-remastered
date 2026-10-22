#include "doctest.h"
#include "../../GameOS/gameos/sim_tick_clock.h"

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
    for (double fps : {20.0, 30.0, 60.0, 75.0, 144.0, 165.0, 240.0, 1000.0})
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

TEST_CASE("tick clock: a hitch is dropped, not replayed, and the fast-forward cap scales") {
    TickClock c;
    CHECK(c.advance(5.0, 1.0) == 4);           // clamped to 0.25 s, then capped at 4
    CHECK(c.advance(1.0 / 144.0, 1.0) == 0);   // no backlog left behind
    TickClock f;
    CHECK(f.advance(0.25, 4.0) == 16);         // 1 s of sim wanted (30 ticks), cap 16
    CHECK(mc2::sim::maxTicksPerFrame(4.0) == 16);
    CHECK(mc2::sim::maxTicksPerFrame(0.25) == 4);
}

TEST_CASE("tick clock: degenerate input runs nothing") {
    TickClock c;
    CHECK(c.advance(0.0, 1.0) == 0);
    CHECK(c.advance(-1.0, 1.0) == 0);
    CHECK(c.advance(0.1, 0.0) == 0);
    CHECK(c.advance(0.1, std::numeric_limits<double>::quiet_NaN()) == 0);
}
